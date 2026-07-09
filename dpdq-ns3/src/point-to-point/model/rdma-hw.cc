#include <ns3/simulator.h>
#include <ns3/seq-ts-header.h>
#include <ns3/udp-header.h>
#include <ns3/ipv4-header.h>
#include "ns3/ppp-header.h"
#include "ns3/boolean.h"
#include "ns3/uinteger.h"
#include "ns3/double.h"
#include "ns3/data-rate.h"
#include "ns3/enum.h"
#include "ns3/pointer.h"
#include "rdma-hw.h"
#include "ppp-header.h"
#include "qbb-header.h"
#include "cn-header.h"
#include "creditbouncer-tag.h"
#include "ns3/flow-id-num-tag.h"
#include "flow-stat-tag.h"
#include "tlt-tag.h"
#include "ns3/hash.h"
#include "ns3/switch-node.h"
#include <climits>
#include <algorithm>

#define TLT_DEBUG_ENABLE 0
#if TLT_DEBUG_ENABLE
#define TLT_DEBUG_TARGET 100
#define TLT_IS_DEBUG_TARGET(x) ((x)->m_flow_id == TLT_DEBUG_TARGET)
#define TLT_DEBUG_PRINT(x) (std::cerr << x << std::endl);
#else
#define TLT_IS_DEBUG_TARGET(x) (false)
#define TLT_DEBUG_PRINT(x)
#endif
namespace ns3{

NS_LOG_COMPONENT_DEFINE("RdmaHw");

std::unordered_map<unsigned, unsigned> acc_timeout_count;
extern struct stat_tx_ stat_tx;
uint64_t RdmaHw::s_creditbouncer_tickCountGlobal = 0;
uint64_t RdmaHw::s_creditbouncer_tickGrantCountGlobal = 0;

namespace {
static constexpr uint32_t DCQCN_STANDALONE_CNP_INTERVAL_US = 50;
static constexpr uint32_t CB_DATA_HEADERS_ON_WIRE = 48; // SeqTs(mode=5) + UDP + IPv4 + PPP
static constexpr uint32_t CB_LINK_OVERHEAD_ON_WIRE = 20; // IFG + preamble
static constexpr uint32_t CB_MIN_ETHERNET_ON_WIRE = 84;
static constexpr uint32_t CB_MAX_ETHERNET_FRAME_ON_WIRE = 1534; // 1466B payload + 48B headers + 20B link overhead.
static std::unordered_map<uint32_t, Ptr<RdmaHw> > s_creditbouncerOwnerByIp;

struct __attribute__((packed)) CbSymmetricHashKey
{
	uint32_t firstIp;
	uint32_t secondIp;
	uint16_t firstPort;
	uint16_t secondPort;
	uint16_t pg;
};

static uint64_t
CbAddHeaderOverhead(uint64_t payloadBytes, uint32_t maxPayloadBytes, bool skipLastPktCheck = false)
{
	if (payloadBytes == 0 || maxPayloadBytes == 0)
		return 0;

	uint64_t fullPkts = payloadBytes / maxPayloadBytes;
	uint64_t remBytes = payloadBytes % maxPayloadBytes;
	uint64_t maxPktOnWire = static_cast<uint64_t>(maxPayloadBytes) + CB_DATA_HEADERS_ON_WIRE + CB_LINK_OVERHEAD_ON_WIRE;
	uint64_t total = fullPkts * maxPktOnWire;

	if (remBytes > 0) {
		uint64_t remOnWire = remBytes + CB_DATA_HEADERS_ON_WIRE + CB_LINK_OVERHEAD_ON_WIRE;
		total += std::max<uint64_t>(remOnWire, CB_MIN_ETHERNET_ON_WIRE);
	} else if (skipLastPktCheck && fullPkts > 0) {
		// Keep parity with R2P2's optional "skip last packet check" behavior.
		total = std::max<uint64_t>(total, CB_MIN_ETHERNET_ON_WIRE);
	}
	return total;
}

static uint64_t
CbRemoveHeaderOverhead(uint64_t onWireBytes, uint32_t maxPayloadBytes)
{
	if (onWireBytes == 0 || maxPayloadBytes == 0)
		return 0;

	uint64_t maxPktOnWire = static_cast<uint64_t>(maxPayloadBytes) + CB_DATA_HEADERS_ON_WIRE + CB_LINK_OVERHEAD_ON_WIRE;
	uint64_t fullPkts = onWireBytes / maxPktOnWire;
	uint64_t remOnWire = onWireBytes % maxPktOnWire;
	uint64_t payload = fullPkts * maxPayloadBytes;

	// Allow the final short packet to consume a short on-wire credit grant.
	if (remOnWire >= CB_MIN_ETHERNET_ON_WIRE) {
		uint64_t remPayload = remOnWire - CB_DATA_HEADERS_ON_WIRE - CB_LINK_OVERHEAD_ON_WIRE;
		payload += std::min<uint64_t>(remPayload, maxPayloadBytes);
	}
	return payload;
}

static uint64_t
CbAlignChunkPrefixBytes(uint64_t candidateBytes, uint64_t messageBytes,
			uint32_t chunkBytes)
{
	if (candidateBytes == 0 || messageBytes == 0 || chunkBytes == 0)
		return 0;
	if (candidateBytes >= messageBytes)
		return messageBytes;
	return (candidateBytes / chunkBytes) * chunkBytes;
}

static uint32_t
CbNextChunkPayloadBytes(uint64_t nextOffset, uint64_t messageBytes,
			uint32_t chunkBytes)
{
	if (chunkBytes == 0 || nextOffset >= messageBytes)
		return 0;
	return static_cast<uint32_t>(
		std::min<uint64_t>(chunkBytes, messageBytes - nextOffset));
}

static uint64_t
CbNextChunkOnWireBytes(uint64_t nextOffset, uint64_t messageBytes,
		       uint32_t chunkBytes)
{
	uint32_t payloadBytes = CbNextChunkPayloadBytes(nextOffset, messageBytes,
						 chunkBytes);
	return CbAddHeaderOverhead(payloadBytes, chunkBytes, true);
}

static const char*
CbDataClassToString(CreditBouncerTag::DataClass cls)
{
	switch (cls) {
	case CreditBouncerTag::CB_DATA_CLASS_UNSOLICITED: return "unsolicited";
	case CreditBouncerTag::CB_DATA_CLASS_UNSCHEDULED: return "unscheduled";
	case CreditBouncerTag::CB_DATA_CLASS_SCHEDULED: return "scheduled";
	default: return "none";
	}
}

static const char*
CbMessageTypeToString(CreditBouncerTag::MessageType type)
{
	switch (type) {
	case CreditBouncerTag::CB_MSG_GRANT_REQ: return "grant_req";
	case CreditBouncerTag::CB_MSG_REQUEST: return "request";
	case CreditBouncerTag::CB_MSG_GRANT: return "grant";
	case CreditBouncerTag::CB_MSG_BOUNCED_CREDIT: return "bounced_credit";
	case CreditBouncerTag::CB_MSG_RESEND: return "resend";
	default: return "none";
	}
}

static Ptr<Packet>
BuildCreditBouncerCtrlPacket(Ipv4Address sip,
				 Ipv4Address dip,
				 uint16_t sport,
				 uint16_t dport,
				 uint16_t pg,
				 uint32_t seq,
				 CreditBouncerTag::MessageType msgType,
				 uint32_t creditReqBytes,
				 uint32_t creditBytes,
				 uint16_t ipid,
				 uint16_t creditPad = 0,
				 uint32_t resendStartIdx = 0,
				 uint32_t resendNumChunks = 0)
{
	qbbHeader hdr;
	hdr.SetSeq(seq);
	hdr.SetPG(pg);
	hdr.SetSport(sport);
	hdr.SetDport(dport);
	hdr.SetIrnNack(0);
	hdr.SetIrnNackSize(0);

	Ptr<Packet> p = Create<Packet>(std::max(60 - 14 - 20 - (int)hdr.GetSerializedSize(), 0));
	p->AddPacketTag(CreditBouncerTag(msgType, creditReqBytes, creditBytes, false,
		false, CreditBouncerTag::CB_DATA_CLASS_NONE, 0, 0, false, creditPad,
		resendStartIdx, resendNumChunks));
	p->AddHeader(hdr);

	Ipv4Header ipHeader;
	ipHeader.SetSource(sip);
	ipHeader.SetDestination(dip);
	ipHeader.SetProtocol(RdmaHw::RDMA_L3_PROT_CREDITBOUNCER);
	ipHeader.SetPayloadSize(p->GetSize());
	ipHeader.SetTtl(64);
	ipHeader.SetIdentification(ipid);
	p->AddHeader(ipHeader);

	PppHeader ppp;
	ppp.SetProtocol(0x0021);
	p->AddHeader(ppp);
	return p;
}

static CreditBouncerTag::DataClass
CbDataClassForOffset(const Ptr<RdmaQueuePair>& qp, uint64_t offset)
{
	if (!qp->cb.m_isScheduled) {
		if (!qp->cb.m_hasScheduledPart) {
			return CreditBouncerTag::CB_DATA_CLASS_UNSOLICITED;
		}
		if (offset < qp->cb.m_selfAllocCreditDataBytes) {
			return CreditBouncerTag::CB_DATA_CLASS_UNSCHEDULED;
		}
	}
	return CreditBouncerTag::CB_DATA_CLASS_SCHEDULED;
}

static Ptr<Packet>
BuildCreditBouncerDataPacket(const Ptr<RdmaQueuePair>& qp,
			     uint32_t mtu,
			     uint32_t payloadSize,
			     uint32_t seq,
			     CreditBouncerTag::DataClass dataClass,
			     bool csnMarked,
			     bool priorityFlow)
{
	Ptr<Packet> p = Create<Packet>(payloadSize);

	SeqTsHeader seqTs;
	seqTs.SetSeq(seq);
	seqTs.SetPG(qp->m_pg);
	p->AddHeader(seqTs);

	UdpHeader udpHeader;
	udpHeader.SetDestinationPort(qp->dport);
	udpHeader.SetSourcePort(qp->sport);
	p->AddHeader(udpHeader);

	Ipv4Header ipHeader;
	ipHeader.SetSource(qp->sip);
	ipHeader.SetDestination(qp->dip);
	ipHeader.SetProtocol(RdmaHw::RDMA_L3_PROT_UDP);
	ipHeader.SetPayloadSize(p->GetSize());
	ipHeader.SetTtl(64);
	ipHeader.SetTos(0);
	ipHeader.SetIdentification(qp->m_ipid++);
	p->AddHeader(ipHeader);

	PppHeader ppp;
	ppp.SetProtocol(0x0021);
	p->AddHeader(ppp);

	FlowIDNUMTag fint;
	if (!p->PeekPacketTag(fint)) {
		fint.SetId(qp->m_flow_id);
		fint.SetFlowSize(qp->m_size);
		p->AddPacketTag(fint);
	}

	FlowStatTag fst;
	if (!p->PeekPacketTag(fst)) {
		uint64_t endSeq = static_cast<uint64_t>(seq) + payloadSize;
		if (qp->m_size < mtu && endSeq >= qp->m_size) {
			fst.SetType(FlowStatTag::FLOW_START_AND_END);
		} else if (endSeq >= qp->m_size) {
			fst.SetType(FlowStatTag::FLOW_END);
		} else if (seq == 0) {
			fst.SetType(FlowStatTag::FLOW_START);
		} else {
			fst.SetType(FlowStatTag::FLOW_NOTEND);
		}
		fst.setInitiatedTime(Simulator::Now().GetSeconds());
		p->AddPacketTag(fst);
	}

	CreditBouncerTag dataTag(CreditBouncerTag::CB_MSG_REQUEST,
		0,
		0,
		csnMarked,
		priorityFlow,
		dataClass,
		0,
		0,
		false,
		0);
	p->AddPacketTag(dataTag);
	return p;
}
}

TypeId RdmaHw::GetTypeId (void)
{
	static TypeId tid = TypeId ("ns3::RdmaHw")
		.SetParent<Object> ()
		.AddAttribute("MinRate",
				"Minimum rate of a throttled flow",
				DataRateValue(DataRate("100Mb/s")),
				MakeDataRateAccessor(&RdmaHw::m_minRate),
				MakeDataRateChecker())
		.AddAttribute("Mtu",
				"Mtu.",
				UintegerValue(1466),
				MakeUintegerAccessor(&RdmaHw::m_mtu),
				MakeUintegerChecker<uint32_t>())
		.AddAttribute ("CcMode",
				"which mode of DCQCN is running",
				UintegerValue(0),
				MakeUintegerAccessor(&RdmaHw::m_cc_mode),
				MakeUintegerChecker<uint32_t>())
		.AddAttribute("NACK Generation Interval",
				"The NACK Generation interval",
				DoubleValue(500.0),
				MakeDoubleAccessor(&RdmaHw::m_nack_interval),
				MakeDoubleChecker<double>())
		.AddAttribute("L2ChunkSize",
				"Layer 2 chunk size. Disable chunk mode if equals to 0.",
				UintegerValue(0),
				MakeUintegerAccessor(&RdmaHw::m_chunk),
				MakeUintegerChecker<uint32_t>())
		.AddAttribute("L2AckInterval",
				"Layer 2 Ack intervals. Disable ack if equals to 0.",
				UintegerValue(0),
				MakeUintegerAccessor(&RdmaHw::m_ack_interval),
				MakeUintegerChecker<uint32_t>())
		.AddAttribute("L2BackToZero",
				"Layer 2 go back to zero transmission.",
				BooleanValue(false),
				MakeBooleanAccessor(&RdmaHw::m_backto0),
				MakeBooleanChecker())
		.AddAttribute("EwmaGain",
				"Control gain parameter which determines the level of rate decrease",
				DoubleValue(1.0 / 16),
				MakeDoubleAccessor(&RdmaHw::m_g),
				MakeDoubleChecker<double>())
		.AddAttribute ("RateOnFirstCnp",
				"the fraction of rate on first CNP",
				DoubleValue(1.0),
				MakeDoubleAccessor(&RdmaHw::m_rateOnFirstCNP),
				MakeDoubleChecker<double> ())
		.AddAttribute("ClampTargetRate",
				"Clamp target rate.",
				BooleanValue(false),
				MakeBooleanAccessor(&RdmaHw::m_EcnClampTgtRate),
				MakeBooleanChecker())
		.AddAttribute("RPTimer",
				"The rate increase timer at RP in microseconds",
				DoubleValue(1500.0),
				MakeDoubleAccessor(&RdmaHw::m_rpgTimeReset),
				MakeDoubleChecker<double>())
		.AddAttribute("RPByteReset",
				"The byte-counter threshold at RP in bytes",
				UintegerValue(10000000),
				MakeUintegerAccessor(&RdmaHw::m_rpgByteReset),
				MakeUintegerChecker<uint64_t>(1))
		.AddAttribute("RateDecreaseInterval",
				"The interval of rate decrease check",
				DoubleValue(4.0),
				MakeDoubleAccessor(&RdmaHw::m_rateDecreaseInterval),
				MakeDoubleChecker<double>())
		.AddAttribute("FastRecoveryTimes",
				"The rate increase timer at RP",
				UintegerValue(5),
				MakeUintegerAccessor(&RdmaHw::m_rpgThreshold),
				MakeUintegerChecker<uint32_t>())
		.AddAttribute("AlphaResumInterval",
				"The interval of resuming alpha",
				DoubleValue(55.0),
				MakeDoubleAccessor(&RdmaHw::m_alpha_resume_interval),
				MakeDoubleChecker<double>())
		.AddAttribute("RateAI",
				"Rate increment unit in AI period",
				DataRateValue(DataRate("5Mb/s")),
				MakeDataRateAccessor(&RdmaHw::m_rai),
				MakeDataRateChecker())
		.AddAttribute("RateHAI",
				"Rate increment unit in hyperactive AI period",
				DataRateValue(DataRate("50Mb/s")),
				MakeDataRateAccessor(&RdmaHw::m_rhai),
				MakeDataRateChecker())
		.AddAttribute("VarWin",
				"Use variable window size or not",
				BooleanValue(false),
				MakeBooleanAccessor(&RdmaHw::m_var_win),
				MakeBooleanChecker())
		.AddAttribute("FastReact",
				"Fast React to congestion feedback",
				BooleanValue(true),
				MakeBooleanAccessor(&RdmaHw::m_fast_react),
				MakeBooleanChecker())
		.AddAttribute("MiThresh",
				"Threshold of number of consecutive AI before MI",
				UintegerValue(5),
				MakeUintegerAccessor(&RdmaHw::m_miThresh),
				MakeUintegerChecker<uint32_t>())
		.AddAttribute("TargetUtil",
				"The Target Utilization of the bottleneck bandwidth, by default 95%",
				DoubleValue(0.95),
				MakeDoubleAccessor(&RdmaHw::m_targetUtil),
				MakeDoubleChecker<double>())
		.AddAttribute("UtilHigh",
				"The upper bound of Target Utilization of the bottleneck bandwidth, by default 98%",
				DoubleValue(0.98),
				MakeDoubleAccessor(&RdmaHw::m_utilHigh),
				MakeDoubleChecker<double>())
		.AddAttribute("RateBound",
				"Bound packet sending by rate, for test only",
				BooleanValue(true),
				MakeBooleanAccessor(&RdmaHw::m_rateBound),
				MakeBooleanChecker())
		.AddAttribute("CreditBouncerSymmetricRouting",
				"Use direction-independent ECMP hashing for CreditBouncer traffic.",
				BooleanValue(true),
				MakeBooleanAccessor(&RdmaHw::m_creditbouncerSymmetricRouting),
				MakeBooleanChecker())
		.AddAttribute("MultiRate",
				"Maintain multiple rates in HPCC",
				BooleanValue(true),
				MakeBooleanAccessor(&RdmaHw::m_multipleRate),
				MakeBooleanChecker())
		.AddAttribute("SampleFeedback",
				"Whether sample feedback or not",
				BooleanValue(false),
				MakeBooleanAccessor(&RdmaHw::m_sampleFeedback),
				MakeBooleanChecker())
		.AddAttribute("TimelyAlpha",
				"Alpha of TIMELY",
				DoubleValue(0.875),
				MakeDoubleAccessor(&RdmaHw::m_tmly_alpha),
				MakeDoubleChecker<double>())
		.AddAttribute("TimelyBeta",
				"Beta of TIMELY",
				DoubleValue(0.8),
				MakeDoubleAccessor(&RdmaHw::m_tmly_beta),
				MakeDoubleChecker<double>())
		.AddAttribute("TimelyTLow",
				"TLow of TIMELY (ns)",
				UintegerValue(50000),
				MakeUintegerAccessor(&RdmaHw::m_tmly_TLow),
				MakeUintegerChecker<uint64_t>())
		.AddAttribute("TimelyTHigh",
				"THigh of TIMELY (ns)",
				UintegerValue(500000),
				MakeUintegerAccessor(&RdmaHw::m_tmly_THigh),
				MakeUintegerChecker<uint64_t>())
		.AddAttribute("TimelyMinRtt",
				"MinRtt of TIMELY (ns)",
				UintegerValue(20000),
				MakeUintegerAccessor(&RdmaHw::m_tmly_minRtt),
				MakeUintegerChecker<uint64_t>())
		.AddAttribute("DctcpRateAI",
				"DCTCP's Rate increment unit in AI period",
				DataRateValue(DataRate("1000Mb/s")),
				MakeDataRateAccessor(&RdmaHw::m_dctcp_rai),
				MakeDataRateChecker())
		.AddAttribute("IrnEnable",
				"Enable IRN",
				BooleanValue(false),
				MakeBooleanAccessor(&RdmaHw::m_irn),
				MakeBooleanChecker())
		.AddAttribute("IrnRtoLow",
			"Low RTO for IRN",
			TimeValue (MicroSeconds (454)),
			MakeTimeAccessor(&RdmaHw::m_irn_rtoLow),
			MakeTimeChecker())
		.AddAttribute("IrnRtoHigh",
			"High RTO for IRN",
			TimeValue (MicroSeconds (1350)),
			MakeTimeAccessor(&RdmaHw::m_irn_rtoHigh),
			MakeTimeChecker())
		.AddAttribute("IrnBdp",
			"BDP Limit for IRN in Bytes",
			UintegerValue(100000),
			MakeUintegerAccessor(&RdmaHw::m_irn_bdp),
			MakeUintegerChecker<uint32_t>())
    	.AddAttribute ("L2Timeout",
			"Sender's timer of waiting for the ack",
			TimeValue (MilliSeconds (4)),
			MakeTimeAccessor (&RdmaHw::m_waitAckTimeout),
			MakeTimeChecker ())
		.AddAttribute("TltEnable",
				"Enable TLT",
				BooleanValue(false),
				MakeBooleanAccessor(&RdmaHw::m_tlt),
				MakeBooleanChecker())
		.AddAttribute("TltImportantMarkingInterval",
				"Marking interval of important packet (rate-based CC only)",
				UintegerValue(96),
				MakeUintegerAccessor(&RdmaHw::m_tlt_important_marking_interval),
				MakeUintegerChecker<uint32_t>())
		.AddAttribute("CreditBouncerEnable",
				"Enable host-side CreditBouncer logic",
				BooleanValue(false),
				MakeBooleanAccessor(&RdmaHw::m_creditbouncer),
				MakeBooleanChecker())
		.AddAttribute("CreditBouncerCcEnable",
				"Enable host-side CreditBouncer CC behavior (ECN/CSN AIMD).",
				BooleanValue(true),
				MakeBooleanAccessor(&RdmaHw::m_creditbouncerCcEnable),
				MakeBooleanChecker())
		.AddAttribute("CreditBouncerCsnEnable",
				"Enable host-side CreditBouncer CSN marking/carrying on data packets.",
				BooleanValue(false),
				MakeBooleanAccessor(&RdmaHw::m_creditbouncerCsnEnable),
				MakeBooleanChecker())
		.AddAttribute("CreditBouncerUnsolicitedThresholdBytes",
				"Unsolicited threshold in on-wire bytes",
				UintegerValue(200 * 1024), // 1 * BDP (BDP=200KB)
				MakeUintegerAccessor(&RdmaHw::m_creditbouncer_unsolicitedThresh_bytes),
				MakeUintegerChecker<uint32_t>())
		.AddAttribute("CreditBouncerMaxSrpbBytes",
				"Max sender-receiver pair budget (on-wire bytes) used for unsolicited/unscheduled split",
				UintegerValue(200 * 1024), // 1 * BDP
				MakeUintegerAccessor(&RdmaHw::m_creditbouncer_maxSrpb_bytes),
				MakeUintegerChecker<uint32_t>())
		.AddAttribute("CreditBouncerGlobalBucketBytes",
				"Receiver global credit bucket upper bound B in on-wire bytes",
				UintegerValue(300 * 1024), // 1.5 * BDP
				MakeUintegerAccessor(&RdmaHw::m_creditbouncer_globalBucket_bytes),
				MakeUintegerChecker<uint32_t>())
		.AddAttribute("CreditBouncerSenderThresholdBytes",
				"Sender credit backlog threshold Sthr in on-wire bytes",
				UintegerValue(65536),
				MakeUintegerAccessor(&RdmaHw::m_creditbouncer_senderThreshold_bytes),
				MakeUintegerChecker<uint32_t>())
		.AddAttribute("CreditBouncerGrantGranularityBytes",
				"Credit grant granularity in bytes",
				UintegerValue(1466),
				MakeUintegerAccessor(&RdmaHw::m_creditbouncer_grantGranularity_bytes),
				MakeUintegerChecker<uint32_t>())
		.AddAttribute("CreditBouncerAiStepBytes",
				"AIMD additive increase multiplier (applied to MSS^2/W)",
				DoubleValue(1.0),
				MakeDoubleAccessor(&RdmaHw::m_creditbouncer_aiStep_bytes),
				MakeDoubleChecker<double>())
		.AddAttribute("CreditBouncerMdFactor",
				"AIMD decrease coefficient (decrease = W * marked_ratio * factor)",
				DoubleValue(0.5),
				MakeDoubleAccessor(&RdmaHw::m_creditbouncer_mdFactor),
				MakeDoubleChecker<double>(0.0, 1.0))
		.AddAttribute("CreditBouncerCeNewWeight",
				"EWMA weight for CE/CSN marked-ratio updates",
				DoubleValue(0.1),
				MakeDoubleAccessor(&RdmaHw::m_creditbouncer_ceNewWeight),
				MakeDoubleChecker<double>(0.0, 1.0))
		.AddAttribute("CreditBouncerEcnMinMulNw",
				"Lower bound multiplier for network-side SRPB window",
				DoubleValue(0.1),
				MakeDoubleAccessor(&RdmaHw::m_creditbouncer_ecnMinMul_nw),
				MakeDoubleChecker<double>(0.0, 1.0))
		.AddAttribute("CreditBouncerEcnMinMulHost",
				"Lower bound multiplier for host-side SRPB window",
				DoubleValue(0.1),
				MakeDoubleAccessor(&RdmaHw::m_creditbouncer_ecnMinMul_host),
				MakeDoubleChecker<double>(0.0, 1.0))
		.AddAttribute("CreditBouncerGrantIntervalNs",
				"Grant pacer tick interval in nanoseconds",
				UintegerValue(480),
				MakeUintegerAccessor(&RdmaHw::m_creditbouncer_grantIntervalNs),
				MakeUintegerChecker<uint32_t>())
		.AddAttribute("CreditBouncerResendIntervalNs",
				"Receiver-side resend timeout interval in nanoseconds",
				UintegerValue(4000000),
				MakeUintegerAccessor(&RdmaHw::m_creditbouncer_resendIntervalNs),
				MakeUintegerChecker<uint32_t>())
		.AddAttribute("CreditBouncerDebugLog",
				"Enable minimal CreditBouncer runtime logs for validation",
				BooleanValue(false),
				MakeBooleanAccessor(&RdmaHw::m_creditbouncer_debugLog),
				MakeBooleanChecker())
		.AddAttribute("CreditBouncerDebugMaxLogs",
				"Max number of CreditBouncer debug lines per host",
				UintegerValue(200),
				MakeUintegerAccessor(&RdmaHw::m_creditbouncer_debugMaxLogs),
				MakeUintegerChecker<uint32_t>())
		.AddAttribute("CreditBouncerSenderSchedPolicy",
				"Sender-side scheduling policy: RR or SRPT",
				EnumValue(RdmaHw::CB_SCHED_RR),
				MakeEnumAccessor(&RdmaHw::m_creditbouncer_senderSchedPolicy),
				MakeEnumChecker(RdmaHw::CB_SCHED_RR, "RR",
						RdmaHw::CB_SCHED_SRPT, "SRPT"))
		.AddAttribute("CreditBouncerGrantSchedPolicy",
				"Receiver-side grant scheduling policy: RR or SRPT",
				EnumValue(RdmaHw::CB_SCHED_RR),
				MakeEnumAccessor(&RdmaHw::m_creditbouncer_grantSchedPolicy),
				MakeEnumChecker(RdmaHw::CB_SCHED_RR, "RR",
						RdmaHw::CB_SCHED_SRPT, "SRPT"))
		.AddAttribute("CreditBouncerSenderPolicyRatio",
				"FS/SRPT mixing ratio for sender-side scheduling in [0,1]",
				DoubleValue(0.5),
				MakeDoubleAccessor(&RdmaHw::m_creditbouncer_senderPolicyRatio),
				MakeDoubleChecker<double>(0.0, 1.0))
		.AddTraceSource("TimeoutEvent",
				"Triggered when RDMA retransmission timeout handler runs for a queue pair.",
				MakeTraceSourceAccessor(&RdmaHw::m_traceTimeoutEvent))
		.AddTraceSource("CreditBouncerResendTimeoutEvent",
				"Triggered when CreditBouncer receiver-side resend timeout handler runs for a message.",
				MakeTraceSourceAccessor(&RdmaHw::m_traceCreditBouncerResendTimeoutEvent))
		;
	return tid;
}

RdmaHw::RdmaHw(){
	m_creditbouncerSymmetricRouting = true;
	m_creditbouncerCcEnable = true;
	m_creditbouncerCsnEnable = false;
	m_creditbouncer_globalOutstanding_bytes = 0;
	m_creditbouncer_debugLog = false;
	m_creditbouncer_debugMaxLogs = 200;
	m_creditbouncer_debugLogCount = 0;
	m_creditbouncer_senderSchedPolicy = CB_SCHED_RR;
	m_creditbouncer_grantSchedPolicy = CB_SCHED_RR;
	m_creditbouncer_senderPolicyRatio = 0.5;
	m_creditbouncer_grantRrCursor = 0;
	m_creditbouncer_tickCount = 0;
	m_creditbouncer_tickGrantCount = 0;
	m_creditbouncer_resendIntervalNs = 4000000;
}

RdmaHw::ReceiverState &RdmaHw::GetOrCreateReceiverState(uint32_t receiverAddr, Ptr<RdmaQueuePair> msgState) {
	auto it = m_receiverStates.find(receiverAddr);
	if (it == m_receiverStates.end()) {
		it = m_receiverStates.emplace(receiverAddr, ReceiverState()).first;
	}
	ReceiverState &state = it->second;
	if (msgState != NULL) {
		bool exists = false;
		for (const auto &q : state.m_msgStates) {
			if (q == msgState) {
				exists = true;
				break;
			}
		}
		if (!exists)
			state.m_msgStates.push_back(msgState);
	}
	return state;
}

RdmaHw::SenderState &RdmaHw::GetOrCreateSenderState(uint32_t senderAddr, Ptr<RdmaRxQueuePair> msgState) {
	auto it = m_senderStates.find(senderAddr);
	if (it == m_senderStates.end()) {
		it = m_senderStates.emplace(senderAddr, SenderState()).first;
	}
	SenderState &state = it->second;
	state.m_senderAddr = senderAddr;
	if (msgState != NULL) {
		bool exists = false;
		for (const auto &q : state.m_msgStates) {
			if (q == msgState) {
				exists = true;
				break;
			}
		}
		if (!exists)
			state.m_msgStates.push_back(msgState);
	}
	return state;
}

void RdmaHw::CleanupCreditBouncerReceiverState(Ptr<RdmaQueuePair> qp) {
	if (!m_creditbouncer || qp == 0 || !qp->cb.m_enabled)
		return;

	auto it = m_receiverStates.find(qp->dip.Get());
	if (it == m_receiverStates.end())
		return;

	std::vector<Ptr<RdmaQueuePair> > &msgStates = it->second.m_msgStates;
	msgStates.erase(
		std::remove(msgStates.begin(), msgStates.end(), qp),
		msgStates.end());
	if (msgStates.empty())
		m_receiverStates.erase(it);
}

void RdmaHw::CleanupCreditBouncerSenderState(Ptr<RdmaRxQueuePair> rxQp) {
	if (!m_creditbouncer || rxQp == 0 || !rxQp->cb.m_enabled)
		return;

	auto it = m_senderStates.find(rxQp->dip);
	if (it == m_senderStates.end())
		return;

	std::vector<Ptr<RdmaRxQueuePair> > &msgStates = it->second.m_msgStates;
	msgStates.erase(
		std::remove(msgStates.begin(), msgStates.end(), rxQp),
		msgStates.end());
	if (msgStates.empty())
		m_senderStates.erase(it);
}

void RdmaHw::ApplyCreditAimd(uint64_t &limitBytes, bool marked, uint64_t minBytes, uint64_t maxBytes, double aiStepBytes, double mdFactor){
	if (maxBytes < minBytes)
		maxBytes = minBytes;
	if (limitBytes < minBytes)
		limitBytes = minBytes;
	if (limitBytes > maxBytes)
		limitBytes = maxBytes;

	if (marked){
		double boundedMd = std::min(1.0, std::max(0.0, mdFactor));
		uint64_t dec = static_cast<uint64_t>(limitBytes * boundedMd);
		limitBytes = std::max(minBytes, dec);
	}else{
		uint64_t ai = aiStepBytes > 0.0 ? static_cast<uint64_t>(std::max(1.0, aiStepBytes)) : 0;
		if (ai > 0)
			limitBytes = std::min(maxBytes, limitBytes + ai);
	}
}

uint64_t RdmaHw::GetHostSchedCreditAvailableBytes() const{
	uint64_t total = 0;
	for (const auto &kv : m_receiverStates){
		const ReceiverState &state = kv.second;
		if (state.m_enabled)
			total += state.m_availCreditBytes;
	}
	return total;
}

int RdmaHw::GetNextCreditBouncerQindex(Ptr<RdmaQueuePairGroup> qpGrp, bool paused[], uint32_t rrLast, uint32_t mtu){
	if (!m_creditbouncer || qpGrp == NULL)
		return -1024;

	uint32_t fcount = qpGrp->GetN();
	if (fcount == 0)
		return -1024;

	if (m_creditbouncer_senderSchedPolicy == CB_SCHED_RR) {
		for (uint32_t i = 0; i < fcount; ++i) {
			if (qpGrp->IsQpFinished(i))
				continue;
			Ptr<RdmaQueuePair> qp = qpGrp->Get(i);
			if (qp != NULL)
				qp->cb.m_priorityFlowPending = false;
		}
		for (uint32_t step = 1; step <= fcount; ++step) {
			uint32_t idx = (rrLast + step) % fcount;
			if (qpGrp->IsQpFinished(idx))
				continue;
			Ptr<RdmaQueuePair> qp = qpGrp->Get(idx);
			if (qp == NULL || !qp->cb.m_enabled)
				continue;
			if (qp->GetBytesLeft() == 0)
				continue;
			if (paused[qp->m_pg])
				continue;
			if (qp->m_nextAvail.GetTimeStep() > Simulator::Now().GetTimeStep())
				continue;

			ReceiverState &receiverState = GetOrCreateReceiverState(qp->dip.Get(), qp);
			receiverState.m_enabled = true;
			uint64_t sendWire = CbNextChunkOnWireBytes(qp->snd_nxt, qp->m_size, mtu);
			bool canSend = sendWire > 0 && sendWire <= receiverState.m_availCreditBytes;
			if (!canSend && qp->cb.m_isScheduled && !qp->cb.m_sentAnnouncement) {
				// Scheduled first announcement can be sent as a standalone GRANT_REQ.
				canSend = true;
			}
			if (!canSend)
				continue;
			return static_cast<int>(idx);
		}
		return -1024;
	}

	struct ReceiverCandidate {
		uint32_t receiverAddr;
		ReceiverState *state;
		Ptr<RdmaQueuePair> qp;
		uint32_t qindex;
		uint64_t unsentBytes;
		bool highPrio;
	};

	std::vector<ReceiverCandidate> candidates;
	candidates.reserve(m_receiverStates.size());

	for (auto &kv : m_receiverStates) {
		uint32_t receiverAddr = kv.first;
		ReceiverState &receiverState = kv.second;
		if (!receiverState.m_enabled || receiverState.m_msgStates.empty()) {
			receiverState.m_schedDeficit = 0.0;
			receiverState.m_schedQuantum = 0.0;
			continue;
		}

		std::vector<std::pair<uint64_t, Ptr<RdmaQueuePair> > > localMsgs;
		localMsgs.reserve(receiverState.m_msgStates.size());
		for (const auto &qp : receiverState.m_msgStates) {
			if (qp == NULL || !qp->cb.m_enabled)
				continue;
			if (qp->GetBytesLeft() == 0)
				continue;
			if (paused[qp->m_pg])
				continue;
			if (qp->m_nextAvail.GetTimeStep() > Simulator::Now().GetTimeStep())
				continue;

			localMsgs.push_back(std::make_pair(qp->GetBytesLeft(), qp));
		}

		if (localMsgs.empty()) {
			receiverState.m_schedDeficit = 0.0;
			receiverState.m_schedQuantum = 0.0;
			continue;
		}

		std::sort(localMsgs.begin(), localMsgs.end(),
			[](const std::pair<uint64_t, Ptr<RdmaQueuePair> > &a,
			   const std::pair<uint64_t, Ptr<RdmaQueuePair> > &b) {
				return a.first < b.first;
			});

		Ptr<RdmaQueuePair> localSelected = NULL;
		uint64_t localUnsent = 0;
		for (const auto &entry : localMsgs) {
			Ptr<RdmaQueuePair> qp = entry.second;
			bool canSend = false;
			uint64_t sendWire = CbNextChunkOnWireBytes(
				qp->snd_nxt, qp->m_size, mtu);
			canSend = sendWire > 0 && sendWire <= receiverState.m_availCreditBytes;
			if (!canSend && qp->cb.m_isScheduled && !qp->cb.m_sentAnnouncement) {
				// Scheduled first announcement can be sent as a standalone GRANT_REQ.
				canSend = true;
			}

			if (!canSend)
				continue;

			if (localSelected == NULL) {
				localSelected = qp;
				localUnsent = entry.first;
			}
			if (!qp->cb.m_sentAnnouncement) {
				localSelected = qp;
				localUnsent = entry.first;
				break;
			}
		}

		if (localSelected == NULL) {
			receiverState.m_schedDeficit = 0.0;
			receiverState.m_schedQuantum = 0.0;
			continue;
		}

		int qindex = -1;
		for (uint32_t step = 1; step <= fcount; ++step) {
			uint32_t idx = (rrLast + step) % fcount;
			if (qpGrp->IsQpFinished(idx))
				continue;
			if (qpGrp->Get(idx) == localSelected) {
				qindex = static_cast<int>(idx);
				break;
			}
		}
		if (qindex < 0) {
			receiverState.m_schedDeficit = 0.0;
			receiverState.m_schedQuantum = 0.0;
			continue;
		}

		ReceiverCandidate candidate;
		candidate.receiverAddr = receiverAddr;
		candidate.state = &receiverState;
		candidate.qp = localSelected;
		candidate.qindex = static_cast<uint32_t>(qindex);
		candidate.unsentBytes = localUnsent;
		candidate.highPrio = false;
		candidates.push_back(candidate);
	}

	if (candidates.empty())
		return -1024;

	for (auto &c : candidates)
		c.qp->cb.m_priorityFlowPending = false;

	uint32_t priorityFlowQindex = UINT32_MAX;
	uint64_t priorityFlowUnsent = UINT64_MAX;
	for (const auto &c : candidates) {
		if (c.unsentBytes < priorityFlowUnsent ||
			(c.unsentBytes == priorityFlowUnsent && c.qindex < priorityFlowQindex)) {
			priorityFlowUnsent = c.unsentBytes;
			priorityFlowQindex = c.qindex;
		}
	}

	uint64_t minUnsent = UINT64_MAX;
	for (const auto &c : candidates)
		minUnsent = std::min(minUnsent, c.unsentBytes);
	for (auto &c : candidates)
		c.highPrio = (c.unsentBytes == minUnsent);

	double ratio = std::max(0.0, std::min(1.0, m_creditbouncer_senderPolicyRatio));
	double fsQuantum = 1.0 / static_cast<double>(candidates.size());
	double srptQuantum = 1.0;
	double highQuantum = fsQuantum * (1.0 - ratio) + srptQuantum * ratio;
	double lowQuantum = fsQuantum * (1.0 - ratio);

	for (auto &c : candidates) {
		c.state->m_schedQuantum = c.highPrio ? highQuantum : lowQuantum;
		c.state->m_schedDeficit += c.state->m_schedQuantum;
	}

	for (uint32_t refill = 0; refill < 8; ++refill) {
		double maxDeficit = -1.0;
		for (const auto &c : candidates)
			maxDeficit = std::max(maxDeficit, c.state->m_schedDeficit);
		if (maxDeficit >= 1.0)
			break;
		for (auto &c : candidates)
			c.state->m_schedDeficit += c.state->m_schedQuantum;
	}

	int selected = -1;
	double bestDeficit = -1.0;
	uint64_t bestUnsent = UINT64_MAX;
	for (const auto &c : candidates) {
		double deficit = c.state->m_schedDeficit;
		if (deficit > bestDeficit || (deficit == bestDeficit && c.unsentBytes < bestUnsent)) {
			bestDeficit = deficit;
			bestUnsent = c.unsentBytes;
			selected = static_cast<int>(c.qindex);
		}
	}

	if (selected >= 0) {
		for (auto &c : candidates) {
			if (static_cast<int>(c.qindex) == selected) {
				c.state->m_schedDeficit = std::max(0.0, c.state->m_schedDeficit - 1.0);
				c.qp->cb.m_priorityFlowPending = (c.qindex == priorityFlowQindex);
				break;
			}
		}
	}

	return selected;
}

void RdmaHw::UpdateSenderStateAimdOnData(SenderState &senderState, bool csnMarked, bool ecnMarked, bool priorityFlow){
	if (!senderState.m_enabled)
		return;

	uint64_t srpbCeiling = senderState.m_srpbCeilingBytes > 0
		? senderState.m_srpbCeilingBytes
		: std::max<uint64_t>(1, senderState.m_maxSrpbBytes);
	uint64_t mssOnWire = CB_MAX_ETHERNET_FRAME_ON_WIRE;
	uint64_t nwBefore = senderState.m_nwMaxSrpbBytes;
	uint64_t hostBefore = senderState.m_hostMaxSrpbBytes;
	uint64_t pairBefore = senderState.m_maxSrpbBytes;
	uint64_t srpbBefore = senderState.m_srpbBytes;
	double nwRatioBefore = senderState.m_nwMarkedRatio;
	double hostRatioBefore = senderState.m_hostMarkedRatio;

	senderState.m_pktsSinceLastRatioUpdate++;
	senderState.m_pktsNwSinceLastWndUpdate++;
	senderState.m_pktsHtSinceLastWndUpdate++;

	if (ecnMarked)
		senderState.m_pktsNwMarkedSinceLastRatioUpdate++;
	if (csnMarked)
		senderState.m_pktsHtMarkedSinceLastRatioUpdate++;

	if (!senderState.m_nwEcnBurst && ecnMarked)
		senderState.m_nwEcnBurst = true;
	else if (senderState.m_nwEcnBurst && !ecnMarked)
		senderState.m_nwEcnBurst = false;

	if (!senderState.m_hostEcnBurst && csnMarked)
		senderState.m_hostEcnBurst = true;
	else if (senderState.m_hostEcnBurst && !csnMarked)
		senderState.m_hostEcnBurst = false;

	bool updateRatios = senderState.m_pktsSinceLastRatioUpdate == std::max<uint32_t>(1, senderState.m_pktsAtNextRatioUpdate);
	if (updateRatios) {
		double sampleDen = std::max<uint32_t>(1, senderState.m_pktsSinceLastRatioUpdate);
		double nwSample = static_cast<double>(senderState.m_pktsNwMarkedSinceLastRatioUpdate) / sampleDen;
		double hostSample = static_cast<double>(senderState.m_pktsHtMarkedSinceLastRatioUpdate) / sampleDen;
		double w = std::min(1.0, std::max(0.0, m_creditbouncer_ceNewWeight));
		senderState.m_nwMarkedRatio = (1.0 - w) * senderState.m_nwMarkedRatio + w * nwSample;
		senderState.m_hostMarkedRatio = (1.0 - w) * senderState.m_hostMarkedRatio + w * hostSample;
		senderState.m_pktsNwMarkedSinceLastRatioUpdate = 0;
		senderState.m_pktsHtMarkedSinceLastRatioUpdate = 0;
	}

	double aiMul = senderState.m_aiStepBytes > 0.0 ? senderState.m_aiStepBytes : 1.0;
	double wRef = std::max<uint64_t>(1, senderState.m_maxSrpbBytes);
	double aiDelta = aiMul * (static_cast<double>(mssOnWire) * static_cast<double>(mssOnWire)) / wRef;
	uint64_t aiBytes = static_cast<uint64_t>(std::max(1.0, aiDelta));
	double mdCoeff = std::min(1.0, std::max(0.0, senderState.m_mdFactor));

	bool nwShouldAi = (!ecnMarked || senderState.m_nwEcnBurst);
	bool didNwAi = nwShouldAi;
	if (nwShouldAi)
		senderState.m_nwMaxSrpbBytes += aiBytes;

	bool reduceNw = ecnMarked &&
		(senderState.m_pktsNwSinceLastWndUpdate >= std::max<uint32_t>(1, senderState.m_pktsNwAtNextWndUpdate));
	if (reduceNw) {
		double nwDec = static_cast<double>(senderState.m_nwMaxSrpbBytes) *
			std::max(0.0, std::min(1.0, senderState.m_nwMarkedRatio)) * mdCoeff;
		uint64_t nwDecBytes = static_cast<uint64_t>(std::max(0.0, nwDec));
		if (nwDecBytes > senderState.m_nwMaxSrpbBytes)
			senderState.m_nwMaxSrpbBytes = 0;
		else
			senderState.m_nwMaxSrpbBytes -= nwDecBytes;
		senderState.m_pktsNwSinceLastWndUpdate = 0;
		senderState.m_pktsNwAtNextWndUpdate = std::max<uint32_t>(
			1, static_cast<uint32_t>(1.5 * static_cast<double>(std::max<uint64_t>(1, senderState.m_maxSrpbBytes)) / static_cast<double>(mssOnWire)));
	}

	bool hostShouldAi = (!csnMarked || senderState.m_hostEcnBurst);
	bool didHostAi = hostShouldAi;
	if (hostShouldAi)
		senderState.m_hostMaxSrpbBytes += aiBytes;

	bool reduceHost = csnMarked &&
		(senderState.m_pktsHtSinceLastWndUpdate >= std::max<uint32_t>(1, senderState.m_pktsHtAtNextWndUpdate));
	if (reduceHost) {
		double hostDec = static_cast<double>(senderState.m_hostMaxSrpbBytes) *
			std::max(0.0, std::min(1.0, senderState.m_hostMarkedRatio)) * mdCoeff;
		uint64_t hostDecBytes = static_cast<uint64_t>(std::max(0.0, hostDec));
		if (hostDecBytes > senderState.m_hostMaxSrpbBytes)
			senderState.m_hostMaxSrpbBytes = 0;
		else
			senderState.m_hostMaxSrpbBytes -= hostDecBytes;
		senderState.m_pktsHtSinceLastWndUpdate = 0;
		senderState.m_pktsHtAtNextWndUpdate = std::max<uint32_t>(
			1, static_cast<uint32_t>(1.5 * static_cast<double>(std::max<uint64_t>(1, senderState.m_maxSrpbBytes)) / static_cast<double>(mssOnWire)));
	}

	senderState.m_nwMaxSrpbBytes = std::max(senderState.m_netBucketMinBytes, senderState.m_nwMaxSrpbBytes);
	senderState.m_nwMaxSrpbBytes = std::min(senderState.m_nwMaxSrpbBytes, srpbCeiling);
	senderState.m_hostMaxSrpbBytes = std::max(senderState.m_senderBucketMinBytes, senderState.m_hostMaxSrpbBytes);
	senderState.m_hostMaxSrpbBytes = std::min(senderState.m_hostMaxSrpbBytes, srpbCeiling);

	uint64_t pairLimit = std::min(senderState.m_senderBucketLimitSrpbBytes, senderState.m_netBucketLimitSrpbBytes);
	pairLimit = std::min(senderState.m_nwMaxSrpbBytes, senderState.m_hostMaxSrpbBytes);
	if (priorityFlow)
		pairLimit = srpbCeiling;
	pairLimit = std::max<uint64_t>(1, std::min(srpbCeiling, pairLimit));
	senderState.m_senderBucketLimitSrpbBytes = senderState.m_hostMaxSrpbBytes;
	senderState.m_netBucketLimitSrpbBytes = senderState.m_nwMaxSrpbBytes;
	senderState.m_maxSrpbBytes = pairLimit;
	if (senderState.m_srpbBytes > senderState.m_maxSrpbBytes)
		senderState.m_srpbBytes = senderState.m_maxSrpbBytes;

	if (updateRatios) {
		senderState.m_pktsSinceLastRatioUpdate = 0;
		senderState.m_pktsAtNextRatioUpdate = std::max<uint32_t>(
			1, static_cast<uint32_t>(static_cast<double>(senderState.m_maxSrpbBytes) / static_cast<double>(mssOnWire)));
	}

	if (m_creditbouncer_debugLog && m_creditbouncer_debugLogCount < m_creditbouncer_debugMaxLogs) {
		uint32_t nodeId = m_node ? m_node->GetId() : UINT32_MAX;
		std::cerr << "CBDBG aimd node=" << nodeId
			<< " sender=" << Ipv4Address(senderState.m_senderAddr)
			<< " ecn=" << (ecnMarked ? 1 : 0)
			<< " csn=" << (csnMarked ? 1 : 0)
			<< " ratio_upd=" << (updateRatios ? 1 : 0)
			<< " ai_nw=" << (didNwAi ? 1 : 0)
			<< " ai_host=" << (didHostAi ? 1 : 0)
			<< " md_nw=" << (reduceNw ? 1 : 0)
			<< " md_host=" << (reduceHost ? 1 : 0)
			<< " prio=" << (priorityFlow ? 1 : 0)
			<< " ai_bytes=" << aiBytes
			<< " nw_ratio_before=" << nwRatioBefore
			<< " nw_ratio_after=" << senderState.m_nwMarkedRatio
			<< " host_ratio_before=" << hostRatioBefore
			<< " host_ratio_after=" << senderState.m_hostMarkedRatio
			<< " nw_before=" << nwBefore
			<< " nw_after=" << senderState.m_nwMaxSrpbBytes
			<< " host_before=" << hostBefore
			<< " host_after=" << senderState.m_hostMaxSrpbBytes
			<< " pair_before=" << pairBefore
			<< " pair_after=" << senderState.m_maxSrpbBytes
			<< " srpb_before=" << srpbBefore
			<< " srpb_after=" << senderState.m_srpbBytes
			<< "\n";
		m_creditbouncer_debugLogCount++;
	}
}

bool RdmaHw::TryIssueCreditGrant(){
	struct GrantCandidate {
		uint32_t senderAddr;
		SenderState *senderState;
		Ptr<RdmaRxQueuePair> msgState;
		uint64_t msgPendingDataBytes;
	};

	std::vector<GrantCandidate> candidates;
	candidates.reserve(m_senderStates.size());
	for (auto &kv : m_senderStates){
		uint32_t senderAddr = kv.first;
		SenderState &senderState = kv.second;
		if (!senderState.m_enabled || senderState.m_pendingDemandDataBytes == 0)
			continue;

		for (const auto &msgState : senderState.m_msgStates) {
			if (msgState == NULL || !msgState->cb.m_enabled)
				continue;
			if (msgState->cb.m_pendingDemandBytes == 0)
				continue;

			GrantCandidate c;
			c.senderAddr = senderAddr;
			c.senderState = &senderState;
			c.msgState = msgState;
			c.msgPendingDataBytes = msgState->cb.m_pendingDemandBytes;
			candidates.push_back(c);
		}
	}

	if (candidates.empty())
		return false;

	uint64_t globalLimit = m_creditbouncer_globalBucket_bytes;
	if (globalLimit == 0)
		globalLimit = UINT64_MAX;

	SenderState *selected = NULL;
	Ptr<RdmaRxQueuePair> selectedMsgState = NULL;
	uint32_t selectedSenderAddr = 0;
	uint64_t selectedAskData = 0;
	uint64_t selectedAskWire = 0;
	auto trySelectCandidate = [&](const GrantCandidate &candidate) -> bool {
		SenderState &senderState = *candidate.senderState;
		// Match r2p2 gating: only issue new grant when current bytes_in_flight
		// (receiver-attributed outstanding bytes for this sender) is within the
		// dynamic AIMD cap.
		uint64_t bytesInFlight = senderState.m_OutstandingBytes;
		if (bytesInFlight > senderState.m_maxSrpbBytes)
			return false;
		uint64_t unitData = senderState.m_grantGranularityDataBytes > 0 ? senderState.m_grantGranularityDataBytes : m_mtu;
		uint64_t minGrantData = std::min<uint64_t>(candidate.msgPendingDataBytes, m_mtu);
		if (unitData < minGrantData)
			unitData = minGrantData;
		uint64_t askData = std::min(candidate.msgPendingDataBytes, unitData);
		askData = std::min(askData, senderState.m_pendingDemandDataBytes);
		askData = CbAlignChunkPrefixBytes(askData,
						  candidate.msgPendingDataBytes,
						  m_mtu);
		if (askData == 0)
			return false;
		uint64_t askWire = CbAddHeaderOverhead(askData, m_mtu, true);
		if (m_creditbouncer_globalOutstanding_bytes + askWire > globalLimit)
			return false;
		if (askWire > senderState.m_srpbBytes)
			return false;
		selected = &senderState;
		selectedSenderAddr = candidate.senderAddr;
		selectedMsgState = candidate.msgState;
		selectedAskData = askData;
		selectedAskWire = askWire;
		return true;
	};

	if (m_creditbouncer_grantSchedPolicy == CB_SCHED_SRPT) {
		std::sort(candidates.begin(), candidates.end(),
			[](const GrantCandidate &a, const GrantCandidate &b) {
				if (a.msgPendingDataBytes != b.msgPendingDataBytes)
					return a.msgPendingDataBytes < b.msgPendingDataBytes;
				if (a.senderAddr != b.senderAddr)
					return a.senderAddr < b.senderAddr;
				return a.msgState->m_flow_id < b.msgState->m_flow_id;
			});
		for (const auto &candidate : candidates){
			if (trySelectCandidate(candidate))
				break;
		}
	} else {
		size_t total = candidates.size();
		if (total > 0) {
			if (m_creditbouncer_grantRrCursor >= total)
				m_creditbouncer_grantRrCursor = 0;
			for (size_t offset = 0; offset < total; ++offset) {
				size_t idx = (static_cast<size_t>(m_creditbouncer_grantRrCursor) + offset) % total;
				if (trySelectCandidate(candidates[idx])) {
					m_creditbouncer_grantRrCursor = static_cast<uint32_t>((idx + 1) % total);
					break;
				}
			}
		}
	}

	if (selected != NULL){
		if (selectedMsgState == NULL)
			return false;

		uint64_t msgBytes = selectedMsgState->cb.m_announcementExpectedBytes;
		uint64_t pendingBeforeGrant = selectedMsgState->cb.m_pendingDemandBytes;
		uint32_t grantSeq = 0;
		if (msgBytes > pendingBeforeGrant) {
			uint64_t grantSeq64 = msgBytes - pendingBeforeGrant;
			grantSeq = grantSeq64 > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(grantSeq64);
		}

		selected->m_pendingDemandDataBytes -= selectedAskData;
		selected->m_outstandingDataBytes += selectedAskData;
		if (selected->m_srpbBytes >= selectedAskWire)
			selected->m_srpbBytes -= selectedAskWire;
		else
			selected->m_srpbBytes = 0;
		selected->m_OutstandingBytes += selectedAskWire;
		m_creditbouncer_globalOutstanding_bytes += selectedAskWire;

		if (selectedMsgState->cb.m_pendingDemandBytes >= selectedAskData)
			selectedMsgState->cb.m_pendingDemandBytes -= selectedAskData;
		else
			selectedMsgState->cb.m_pendingDemandBytes = 0;
		if (m_creditbouncer_debugLog && m_creditbouncer_debugLogCount < m_creditbouncer_debugMaxLogs){
			std::cerr << "CBDBG grant node=" << m_node->GetId()
				<< " tick=" << m_creditbouncer_tickCount
				<< " sender=" << Ipv4Address(selectedSenderAddr)
				<< " flow=" << selectedMsgState->m_flow_id
				<< " askData=" << selectedAskData
				<< " askWire=" << selectedAskWire
				<< " rem=" << selected->m_pendingDemandDataBytes
				<< " outData=" << selected->m_outstandingDataBytes
				<< " srpbRem=" << selected->m_srpbBytes
				<< " gOut=" << m_creditbouncer_globalOutstanding_bytes
				<< "\n";
			m_creditbouncer_debugLogCount++;
		}

		uint64_t minWireNoPad = selectedAskData + CB_DATA_HEADERS_ON_WIRE + CB_LINK_OVERHEAD_ON_WIRE;
		uint16_t selectedCreditPad = selectedAskWire > minWireNoPad
			? static_cast<uint16_t>(selectedAskWire - minWireNoPad)
			: 0;

			Ptr<Packet> gp = BuildCreditBouncerCtrlPacket(
				Ipv4Address(selectedMsgState->sip), Ipv4Address(selectedMsgState->dip),
				selectedMsgState->sport, selectedMsgState->dport,
				selectedMsgState->m_ecn_source.qIndex,
				grantSeq,
				CreditBouncerTag::CB_MSG_GRANT,
				0,
				(uint32_t)selectedAskWire,
			selectedMsgState->m_ipid++,
			selectedCreditPad);
		uint32_t nic_idx = GetNicIdxOfRxQp(selectedMsgState);
		if (m_creditbouncer_debugLog && m_creditbouncer_debugLogCount < m_creditbouncer_debugMaxLogs) {
			uint32_t rxHash = selectedMsgState->GetHash();
			uint32_t symHash = GetCreditBouncerSymmetricHash(
				selectedMsgState->sip, selectedMsgState->dip, selectedMsgState->sport,
				selectedMsgState->dport, selectedMsgState->m_ecn_source.qIndex);
			std::cerr << "CBROUTE host node=" << m_node->GetId()
				<< " type=" << CbMessageTypeToString(CreditBouncerTag::CB_MSG_GRANT)
				<< " mode=" << ((m_creditbouncerSymmetricRouting && selectedMsgState->cb.m_enabled) ? "symmetric" : "default")
				<< " flow=" << selectedMsgState->m_flow_id
				<< " sip=" << Ipv4Address(selectedMsgState->sip)
				<< " dip=" << Ipv4Address(selectedMsgState->dip)
				<< " sport=" << selectedMsgState->sport
				<< " dport=" << selectedMsgState->dport
				<< " pg=" << selectedMsgState->m_ecn_source.qIndex
				<< " hash=" << ((m_creditbouncerSymmetricRouting && selectedMsgState->cb.m_enabled) ? symHash : rxHash)
				<< " legacy_hash=" << rxHash
				<< " symmetric_hash=" << symHash
				<< " nic=" << nic_idx
				<< "\n";
			m_creditbouncer_debugLogCount++;
		}
		m_nic[nic_idx].dev->RdmaEnqueueHighPrioQ(gp);
		m_nic[nic_idx].dev->TriggerTransmit();
		return true;
	}

	return false;
}

void RdmaHw::ScheduleCreditGrantTick(){
	if (m_creditbouncer_grantTickEvent.IsRunning())
		return;
	for (auto &kv : m_senderStates){
		SenderState &state = kv.second;
		if (state.m_enabled && state.m_pendingDemandDataBytes > 0){
			Time interval = NanoSeconds(m_creditbouncer_grantIntervalNs > 0 ? m_creditbouncer_grantIntervalNs : 1);
			m_creditbouncer_grantTickEvent = Simulator::Schedule(interval, &RdmaHw::CreditGrantTick, this);
			return;
		}
	}
}

void RdmaHw::CreditGrantTick(){
	m_creditbouncer_tickCount++;
	s_creditbouncer_tickCountGlobal++;
	bool issued = TryIssueCreditGrant();
	if (issued)
	{
		m_creditbouncer_tickGrantCount++;
		s_creditbouncer_tickGrantCountGlobal++;
	}
	if (m_creditbouncer_debugLog && m_creditbouncer_debugLogCount < m_creditbouncer_debugMaxLogs){
		std::cerr << "CBDBG tick node=" << m_node->GetId()
			<< " tick=" << m_creditbouncer_tickCount
			<< " issued=" << (issued ? 1 : 0)
			<< " grantTicks=" << m_creditbouncer_tickGrantCount
			<< " gOut=" << m_creditbouncer_globalOutstanding_bytes
			<< "\n";
		m_creditbouncer_debugLogCount++;
	}
	for (auto &kv : m_senderStates){
		SenderState &state = kv.second;
		if (state.m_enabled && state.m_pendingDemandDataBytes > 0){
			ScheduleCreditGrantTick();
			break;
		}
	}
}

void RdmaHw::ScheduleCreditBouncerResend(Ptr<RdmaRxQueuePair> rxQp){
	if (!m_creditbouncer || rxQp == 0 || !rxQp->cb.m_enabled)
		return;
	if (rxQp->cb.m_completionNotified || rxQp->cb.m_expectedChunks == 0)
		return;
	if (rxQp->cb.m_resendEvent.IsRunning())
		rxQp->cb.m_resendEvent.Cancel();
	Time interval = NanoSeconds(m_creditbouncer_resendIntervalNs > 0 ? m_creditbouncer_resendIntervalNs : 1);
	rxQp->cb.m_resendEvent = Simulator::Schedule(interval,
						      &RdmaHw::HandleCreditBouncerResendTimeout,
						      this,
						      rxQp);
}

void RdmaHw::SendCreditBouncerResend(Ptr<RdmaRxQueuePair> rxQp,
				     uint32_t startIdx,
				     uint32_t numChunks){
	if (rxQp == 0 || numChunks == 0)
		return;
	Ptr<Packet> resend = BuildCreditBouncerCtrlPacket(
		Ipv4Address(rxQp->sip),
		Ipv4Address(rxQp->dip),
		rxQp->sport,
		rxQp->dport,
		rxQp->m_ecn_source.qIndex,
		0,
		CreditBouncerTag::CB_MSG_RESEND,
		0,
		0,
		rxQp->m_ipid++,
		0,
		startIdx,
		numChunks);
	uint32_t nic_idx = GetNicIdxOfRxQp(rxQp);
	m_nic[nic_idx].dev->RdmaEnqueueHighPrioQ(resend);
	m_nic[nic_idx].dev->TriggerTransmit();
}

void RdmaHw::HandleCreditBouncerResendTimeout(Ptr<RdmaRxQueuePair> rxQp){
	if (!m_creditbouncer || rxQp == 0 || !rxQp->cb.m_enabled)
		return;
	if (rxQp->cb.m_completionNotified || rxQp->cb.m_expectedChunks == 0)
		return;
	m_traceCreditBouncerResendTimeoutEvent(
		rxQp,
		static_cast<uint64_t>(m_creditbouncer_resendIntervalNs));

	SenderState &senderState = GetOrCreateSenderState(rxQp->dip, rxQp);
	if (m_mtu == 0)
		return;
	uint64_t msgBytes = rxQp->cb.m_announcementExpectedBytes;
	uint64_t pendingBytes = std::min<uint64_t>(rxQp->cb.m_pendingDemandBytes, msgBytes);
	uint64_t grantedBytes = msgBytes - pendingBytes;
	uint32_t grantedChunkLimit = static_cast<uint32_t>((grantedBytes + m_mtu - 1) / m_mtu);
	grantedChunkLimit = std::min(grantedChunkLimit, rxQp->cb.m_expectedChunks);
	rxQp->cb.m_grantedChunkLimit = grantedChunkLimit;

	if (grantedChunkLimit > 0 && !rxQp->cb.m_receivedChunkBitmap.empty()) {
		uint32_t rangeStart = 0;
		uint32_t rangeLen = 0;
		for (uint32_t i = 0; i < grantedChunkLimit; ++i) {
			if (rxQp->cb.m_receivedChunkBitmap[i] == 0) {
				if (rangeLen == 0)
					rangeStart = i;
				++rangeLen;
			} else if (rangeLen != 0) {
				SendCreditBouncerResend(rxQp, rangeStart, rangeLen);
				rangeLen = 0;
			}
		}
		if (rangeLen != 0) {
			SendCreditBouncerResend(rxQp, rangeStart, rangeLen);
		}
	}

	if (!rxQp->cb.m_completionNotified &&
	    rxQp->cb.m_receivedChunks < rxQp->cb.m_expectedChunks &&
	    (senderState.m_pendingDemandDataBytes > 0 ||
	     grantedChunkLimit > rxQp->cb.m_receivedChunks)) {
		ScheduleCreditBouncerResend(rxQp);
	}
}

void RdmaHw::PrintStat(void) {
	extern std::unordered_map<unsigned, Time> acc_pause_time;
	uint32_t nodeId = m_node ? m_node->GetId() : UINT32_MAX;

	if(!stat_tx.stat_print) {
		printf("%.8lf\tIMP_STAT\t%p\t%lu\n", Simulator::Now().GetSeconds(), this, stat_tx.txImpBytes);
		printf("%.8lf\tIMPE_STAT\t%p\t%lu\n", Simulator::Now().GetSeconds(), this,  stat_tx.txImpEBytes);
		printf("%.8lf\tUIMP_STAT\t%p\t%lu\n", Simulator::Now().GetSeconds(), this, stat_tx.txUimpBytes);
		printf("%.8lf\tIMP_STAT_NIC\t%p\t%lu\n", Simulator::Now().GetSeconds(), this,  stat_tx.txImpBytesNIC);
		printf("%.8lf\tIMPE_STAT_NIC\t%p\t%lu\n", Simulator::Now().GetSeconds(), this, stat_tx.txImpEBytesNIC);
		printf("%.8lf\tIMPF_STAT_NIC\t%p\t%lu\n", Simulator::Now().GetSeconds(), this, stat_tx.txImpFBytesNIC);
		printf("%.8lf\tIMPEF_STAT_NIC\t%p\t%lu\n", Simulator::Now().GetSeconds(), this, stat_tx.txImpEFBytesNIC);
		printf("%.8lf\tIMPC_STAT_NIC\t%p\t%lu\n", Simulator::Now().GetSeconds(), this, stat_tx.txImpCBytesNIC);
		printf("%.8lf\tUIMP_STAT_NIC\t%p\t%lu\n", Simulator::Now().GetSeconds(), this, stat_tx.txUimpBytesNIC);

		printf("%.8lf\tIMP_STAT_NIC_PL\t%p\t%lu\n", Simulator::Now().GetSeconds(), this,  stat_tx.txImpBytesNIC_PL);
		printf("%.8lf\tIMP_STAT_NIC_PLE\t%p\t%lu\n", Simulator::Now().GetSeconds(), this,  stat_tx.txImpBytesNIC_PLE);
		printf("%.8lf\tIMP_STAT_NIC_PLR\t%p\t%lu\n", Simulator::Now().GetSeconds(), this, stat_tx.txImpBytesNIC_PLR);
		printf("%.8lf\tIMP_STAT_NIC_CNP\t%p\t%lu\n", Simulator::Now().GetSeconds(), this,  stat_tx.txImpBytesNIC_CNP);
		printf("%.8lf\tIMP_STAT_NIC_ACK\t%p\t%lu\n", Simulator::Now().GetSeconds(), this,  stat_tx.txImpBytesNIC_ACK);
		printf("%.8lf\tIMP_STAT_NIC_NACK\t%p\t%lu\n", Simulator::Now().GetSeconds(), this, stat_tx.txImpBytesNIC_NACK);

		printf("%.8lf\tTLT_DROP\t%p\t%lu\n", Simulator::Now().GetSeconds(), this,  stat_tx.txTltDropBytes);
		printf("%.8lf\tPAUSE\t%p\t%lu\n", Simulator::Now().GetSeconds(), this, stat_tx.PauseSendCnt);
		printf("%.8lf\tL2_RTO\t%p\t%lu\n", Simulator::Now().GetSeconds(), this, stat_tx.RetxTimeoutCnt);
		printf("%.8lf\tIMP_DROP_BYTES\t%p\t%lu\n", Simulator::Now().GetSeconds(), this, stat_tx.importantDropBytes);
		printf("%.8lf\tIMP_DROP_CNT\t%p\t%lu\n", Simulator::Now().GetSeconds(), this, stat_tx.importantDropCnt);

		Time totalPauseDuration;

		for (auto it = acc_pause_time.begin(); it != acc_pause_time.end(); ++it) {
			totalPauseDuration += it->second;
		}

		printf("%.8lf\tPFC_TIME_TOTAL\t%p\t%.8lf\n", Simulator::Now().GetSeconds(), this, totalPauseDuration.GetSeconds());
		if (m_creditbouncer) {
			printf("%.8lf\tCB_TICK\tnode=%u\thw=%p\tlocal=%lu\tglobal=%lu\n",
				Simulator::Now().GetSeconds(),
				nodeId,
				this,
				m_creditbouncer_tickCount,
				s_creditbouncer_tickCountGlobal);
			printf("%.8lf\tCB_TICK_GRANT\tnode=%u\thw=%p\tlocal=%lu\tglobal=%lu\n",
				Simulator::Now().GetSeconds(),
				nodeId,
				this,
				m_creditbouncer_tickGrantCount,
				s_creditbouncer_tickGrantCountGlobal);
			printf("%.8lf\tCB_GLOBAL_OUT\tnode=%u\thw=%p\tlocal=%lu\n",
				Simulator::Now().GetSeconds(),
				nodeId,
				this,
				m_creditbouncer_globalOutstanding_bytes);
		}

		stat_tx.stat_print = true;
	}
}

void RdmaHw::SetNode(Ptr<Node> node){
	m_node = node;
}
void RdmaHw::Setup(QpCompleteCallback cb){
	for (uint32_t i = 0; i < m_nic.size(); i++){
		Ptr<QbbNetDevice> dev = m_nic[i].dev;
		if (dev == NULL)
			continue;
		// share data with NIC
		dev->m_rdmaEQ->m_qpGrp = m_nic[i].qpGrp;
		// setup callback
		dev->m_rdmaReceiveCb = MakeCallback(&RdmaHw::Receive, this);
		dev->m_rdmaLinkDownCb = MakeCallback(&RdmaHw::SetLinkDown, this);
		dev->m_rdmaPktSent = MakeCallback(&RdmaHw::PktSent, this);
		// config NIC
		dev->m_rdmaEQ->m_mtu = m_mtu;
		dev->m_rdmaEQ->m_rdmaGetNxtPkt = MakeCallback(&RdmaHw::GetNxtPacket, this);
		dev->m_rdmaEQ->m_rdmaGetNextCreditBouncerQindex = MakeCallback(&RdmaHw::GetNextCreditBouncerQindex, this);
	}
	// setup qp complete callback
	m_qpCompleteCallback = cb;
}

uint32_t RdmaHw::GetNicIdxOfQp(Ptr<RdmaQueuePair> qp){
	auto &v = m_rtTable[qp->dip.Get()];
	if (v.size() > 0){
		if (m_creditbouncerSymmetricRouting && qp->cb.m_enabled) {
			uint32_t hash = GetCreditBouncerSymmetricHash(
				qp->sip.Get(), qp->dip.Get(), qp->sport, qp->dport, qp->m_pg);
			return v[hash % v.size()];
		}
		return v[qp->GetHash() % v.size()];
	}else{
		NS_ASSERT_MSG(false, "We assume at least one NIC is alive");
	}
}
uint32_t RdmaHw::GetCreditBouncerSymmetricHash(uint32_t sip, uint32_t dip, uint16_t sport, uint16_t dport, uint16_t pg) const{
	CbSymmetricHashKey key;
	bool swapEndpoints = (sip > dip) || (sip == dip && sport > dport);
	if (swapEndpoints) {
		key.firstIp = dip;
		key.secondIp = sip;
		key.firstPort = dport;
		key.secondPort = sport;
	} else {
		key.firstIp = sip;
		key.secondIp = dip;
		key.firstPort = sport;
		key.secondPort = dport;
	}
	key.pg = pg;
	return Hash32(reinterpret_cast<const char *>(&key), sizeof(key));
}
uint64_t RdmaHw::GetQpKey(uint16_t sport, uint16_t pg){
	return ((uint64_t)sport << 16) | (uint64_t)pg;
}
Ptr<RdmaQueuePair> RdmaHw::GetQp(uint16_t sport, uint16_t pg){
	uint64_t key = GetQpKey(sport, pg);
	auto it = m_qpMap.find(key);
	if (it != m_qpMap.end())
		return it->second;
	return NULL;
}
void RdmaHw::AddQueuePair(uint64_t size, uint16_t pg, Ipv4Address sip, Ipv4Address dip, uint16_t sport, uint16_t dport, uint32_t win, uint64_t baseRtt, int32_t flow_id){
	// create qp
	Ptr<RdmaQueuePair> qp = CreateObject<RdmaQueuePair>(pg, sip, dip, sport, dport);
	qp->SetSize(size);
	qp->SetWin(win);
	qp->SetBaseRtt(baseRtt);
	qp->SetVarWin(m_var_win);
	qp->SetFlowId(flow_id);
	qp->SetTimeout(m_waitAckTimeout);

	if (m_creditbouncer) {
		qp->cb.m_enabled = true;
		ReceiverState &receiverState = GetOrCreateReceiverState(dip.Get(), qp);
		if (!receiverState.m_enabled) {
			receiverState.m_enabled = true;
			receiverState.m_senderBacklogThresholdBytes = m_creditbouncer_senderThreshold_bytes;
			receiverState.m_unsolicitedThreshBytes = m_creditbouncer_unsolicitedThresh_bytes;
			receiverState.m_aiStepBytes = m_creditbouncer_aiStep_bytes;
			receiverState.m_mdFactor = m_creditbouncer_mdFactor;
		}
	}

	if (m_irn) {
		qp->irn.m_enabled = m_irn;
		qp->irn.m_bdp = m_irn_bdp;
		qp->irn.m_rtoLow = m_irn_rtoLow;
		qp->irn.m_rtoHigh = m_irn_rtoHigh;
	}

	
	if (m_tlt) {
		qp->tlt.m_cc_type = GetCcType();
		qp->tlt.m_enabled = m_tlt;
		qp->tlt.m_sendState = TLT_STATE_IMPORTANT;
		qp->tlt.m_tlt_unimportant_pkts_current_round = CreateObject<SelectivePacketQueue>();
		qp->tlt.m_tlt_unimportant_pkts_prev_round = CreateObject<SelectivePacketQueue>();
		NS_ASSERT(qp->tlt.m_tlt_unimportant_pkts_current_round);
		NS_ASSERT(qp->tlt.m_tlt_unimportant_pkts_prev_round);
	}

	// add qp
	uint32_t nic_idx = GetNicIdxOfQp(qp);
	m_nic[nic_idx].qpGrp->AddQp(qp);
	uint64_t key = GetQpKey(sport, pg);
	m_qpMap[key] = qp;
	if (m_creditbouncer && qp->cb.m_enabled) {
		s_creditbouncerOwnerByIp[sip.Get()] = this;
	}

	// set init variables
	DataRate m_bps = m_nic[nic_idx].dev->GetDataRate();
	qp->m_rate = m_bps;
	qp->m_max_rate = m_bps;
	if (m_cc_mode == 1){
		qp->mlx.m_targetRate = m_bps;
		qp->mlx.m_rpByteResetBytes = m_rpgByteReset;
	}else if (m_cc_mode == 3){
		qp->hp.m_curRate = m_bps;
		if (m_multipleRate){
			for (uint32_t i = 0; i < IntHeader::maxHop; i++)
				qp->hp.hopState[i].Rc = m_bps;
		}
	}else if (m_cc_mode == 7){
		qp->tmly.m_curRate = m_bps;
	}

	// Notify Nic
	m_nic[nic_idx].dev->NewQp(qp);
}

Ptr<RdmaRxQueuePair> RdmaHw::GetRxQp(uint32_t sip, uint32_t dip, uint16_t sport, uint16_t dport, uint16_t pg, bool create){
	uint64_t key = ((uint64_t)dip << 32) | ((uint64_t)pg << 16) | (uint64_t)dport;
	auto it = m_rxQpMap.find(key);
	if (it != m_rxQpMap.end())
		return it->second;
	if (create){
		// create new rx qp
		Ptr<RdmaRxQueuePair> q = CreateObject<RdmaRxQueuePair>();
		// init the qp
		q->sip = sip;
		q->dip = dip;
		q->sport = sport;
		q->dport = dport;
		q->m_ecn_source.qIndex = pg;
		q->m_flow_id = -1; // unknown
		q->m_tlt_recvState = TLT_STATE_IDLE;
		if (m_creditbouncer) {
			q->cb.m_enabled = true;
			SenderState &senderState = GetOrCreateSenderState(dip, q);
			if (!senderState.m_enabled) {
				senderState.m_enabled = true;
				senderState.m_senderAddr = dip;
				senderState.m_maxSrpbBytes = m_creditbouncer_maxSrpb_bytes;
				senderState.m_srpbCeilingBytes = m_creditbouncer_maxSrpb_bytes;
				senderState.m_srpbBytes = senderState.m_maxSrpbBytes;
				senderState.m_nwMaxSrpbBytes = senderState.m_maxSrpbBytes;
				senderState.m_hostMaxSrpbBytes = senderState.m_maxSrpbBytes;
				senderState.m_globalBucketLimitBytes = m_creditbouncer_globalBucket_bytes;
				senderState.m_grantGranularityDataBytes = m_creditbouncer_grantGranularity_bytes;
				senderState.m_netBucketLimitSrpbBytes = senderState.m_maxSrpbBytes;
				senderState.m_senderBucketLimitSrpbBytes = senderState.m_maxSrpbBytes;
				uint64_t nwMinByMul = static_cast<uint64_t>(m_creditbouncer_ecnMinMul_nw * static_cast<double>(senderState.m_srpbCeilingBytes));
				uint64_t hostMinByMul = static_cast<uint64_t>(m_creditbouncer_ecnMinMul_host * static_cast<double>(senderState.m_srpbCeilingBytes));
				senderState.m_netBucketMinBytes = std::max<uint64_t>(1, nwMinByMul);
				senderState.m_senderBucketMinBytes = std::max<uint64_t>(1, hostMinByMul);
				senderState.m_aiStepBytes = m_creditbouncer_aiStep_bytes;
				senderState.m_mdFactor = m_creditbouncer_mdFactor;
				uint64_t mssOnWire = CB_MAX_ETHERNET_FRAME_ON_WIRE;
				// Match R2P2's initial cadence: update ratios on the first packet,
				// but defer the first MD decision until roughly one window has arrived.
				senderState.m_pktsAtNextRatioUpdate = 1;
				senderState.m_pktsNwAtNextWndUpdate = std::max<uint32_t>(
					1, static_cast<uint32_t>(static_cast<double>(senderState.m_maxSrpbBytes) / static_cast<double>(mssOnWire)));
				senderState.m_pktsHtAtNextWndUpdate = std::max<uint32_t>(
					1, static_cast<uint32_t>(static_cast<double>(senderState.m_maxSrpbBytes) / static_cast<double>(mssOnWire)));
			}
		}
		// store in map
		m_rxQpMap[key] = q;
		return q;
	}
	return NULL;
}
uint32_t RdmaHw::GetNicIdxOfRxQp(Ptr<RdmaRxQueuePair> q, bool useSymmetricRouting){
	auto &v = m_rtTable[q->dip];
	if (v.size() > 0){
		if (useSymmetricRouting && m_creditbouncerSymmetricRouting && q->cb.m_enabled) {
			uint32_t hash = GetCreditBouncerSymmetricHash(
				q->sip, q->dip, q->sport, q->dport, q->m_ecn_source.qIndex);
			return v[hash % v.size()];
		}
		return v[q->GetHash() % v.size()];
	}else{
		NS_ASSERT_MSG(false, "We assume at least one NIC is alive");
	}
}

#if TLT_DEBUG_ENABLE

void tlt_debug_print(Ptr<RdmaQueuePair> qp, Ptr<RdmaRxQueuePair> rxQp, uint32_t seq, TltTag &tlt, std::string msg, std::string additional) {
	
	std::string tagtype;
	switch(tlt.GetType()) {
	case TltTag::PACKET_IMPORTANT:
		tagtype = "Imp  ";
		break;
	case TltTag::PACKET_IMPORTANT_ECHO:
		tagtype = "ImpE ";
		break;
	case TltTag::PACKET_IMPORTANT_FORCE:
		tagtype = "ImpF ";
		break;
	case TltTag::PACKET_IMPORTANT_ECHO_FORCE:
		tagtype = "ImpFE";
		break;
	case TltTag::PACKET_NOT_IMPORTANT:
		tagtype = "Uimp ";
		break;
	case TltTag::PACKET_IMPORTANT_CONTROL:
		tagtype = "ImpC ";
		break;
	case TltTag::PACKET_IMPORTANT_FAST_RETRANS:
		tagtype = "ImpfR";
		break;
	default:
		tagtype = "Unknown";
		break;
	}

	char mss[1024]; sprintf(mss, "%.8lf", Simulator::Now().GetSeconds() * 1000.);
	TLT_DEBUG_PRINT("[" <<  std::string(mss) <<"] Flow " << (qp ? (qp->m_flow_id) : rxQp->m_flow_id) << " : " << msg << " " << tagtype << " " << seq << additional);
}
inline void tlt_debug_recv_print(Ptr<RdmaRxQueuePair> rxQp, uint32_t seq, TltTag &tlt, std::string additional) {
	if(TLT_IS_DEBUG_TARGET(rxQp))
		tlt_debug_print(nullptr, rxQp, seq, tlt, "Recv UDP                                     ", additional);
}
inline void tlt_debug_recv_print(Ptr<RdmaQueuePair> qp, uint32_t seq, TltTag &tlt, std::string additional) {
	if(TLT_IS_DEBUG_TARGET(qp))
		tlt_debug_print(qp, nullptr, seq, tlt, "Recv ACK             ", additional);
}
inline void tlt_debug_send_print(Ptr<RdmaRxQueuePair> rxQp, uint32_t seq, TltTag &tlt, std::string additional) {
	if(TLT_IS_DEBUG_TARGET(rxQp))
		tlt_debug_print(nullptr, rxQp, seq, tlt, "Send ACK                         ", additional);
}
inline void tlt_debug_send_print(Ptr<RdmaQueuePair> qp, uint32_t seq, TltTag &tlt, std::string additional) {
	if(TLT_IS_DEBUG_TARGET(qp))
		tlt_debug_print(qp, nullptr, seq, tlt, "Send UDP ", additional);
}
#else
inline void tlt_debug_recv_print(Ptr<RdmaRxQueuePair> rxQp, uint32_t seq, TltTag &tlt, std::string additional) {}
inline void tlt_debug_recv_print(Ptr<RdmaQueuePair> qp, uint32_t seq, TltTag &tlt, std::string additional) {}
inline void tlt_debug_send_print(Ptr<RdmaRxQueuePair> rxQp, uint32_t seq, TltTag &tlt, std::string additional) {}
inline void tlt_debug_send_print(Ptr<RdmaQueuePair> qp, uint32_t seq, TltTag &tlt, std::string additional) {}
#endif


int RdmaHw::ReceiveUdp(Ptr<Packet> p, CustomHeader &ch){
	uint8_t ecnbits = ch.GetIpv4EcnBits();

	uint32_t payload_size = p->GetSize() - ch.GetSerializedSize();

	// TODO find corresponding rx queue pair
	Ptr<RdmaRxQueuePair> rxQp = GetRxQp(ch.dip, ch.sip, ch.udp.dport, ch.udp.sport, ch.udp.pg, true);
	// CreditBouncer REQUEST data packets are handled in ReceiveCreditBouncer.
	if (ecnbits != 0){
		rxQp->m_ecn_source.ecnbits |= ecnbits;
		rxQp->m_ecn_source.qfb++;
	}
	rxQp->m_ecn_source.total++;
	if (ecnbits != 0 && m_cc_mode == 1) {
		MaybeSendDcqcnCnp(rxQp);
	}
	rxQp->m_milestone_rx = m_ack_interval;

	if (rxQp->m_flow_id < 0) {
		FlowIDNUMTag fit;
		if (p->PeekPacketTag(fit)) {
			rxQp->m_flow_id = fit.GetId();
		}
	}

	// if (Simulator::Now().GetSeconds() * 1000 >= 20.52949 && rxQp->m_flow_id == TLT_DEBUG_TARGET) {
	// 	std::cerr << "Reached debug point." << std::endl;
	// }
	if (m_tlt) {
		TltTag tlt;
		if (p->PeekPacketTag(tlt)) {
			tlt_debug_recv_print(rxQp, ch.udp.seq, tlt, std::string());
			if (IsWindowBasedCC()) {
				if (tlt.GetType() == TltTag::PACKET_IMPORTANT)
					rxQp->m_tlt_recvState = TLT_STATE_IMPORTANT;
				else if (tlt.GetType() == TltTag::PACKET_IMPORTANT_FORCE)
					rxQp->m_tlt_recvState = TLT_STATE_IMPORTANT_FORCE;

				if (payload_size == 0 && (tlt.GetType() == TltTag::PACKET_IMPORTANT || tlt.GetType() == TltTag::PACKET_IMPORTANT_FORCE)) {
					// TLT FIN : do not deliver to App. layer
					char mss[1024]; sprintf(mss, "%.8lf", Simulator::Now().GetSeconds() * 1000.);
					if (TLT_IS_DEBUG_TARGET(rxQp))
						TLT_DEBUG_PRINT("[" <<  std::string(mss) <<"] Flow " << (rxQp->m_flow_id) << " : Recv FIN");
					return 1;
				}
			}
		} else {
			std::cerr << "Warning: Cannot find TLT tag" << std::endl;
		}
	}
	
	int x = ReceiverCheckSeq(ch.udp.seq, rxQp, payload_size);
	// {
	// 	FlowIDNUMTag fit;
	// 	if (p->PeekPacketTag(fit)) {
	// 		if (fit.GetId() == 15 ) {
	// 			std::cout << "FLOW: Recv " << ch.udp.seq << ", x=" << x << ", ack=" << rxQp->ReceiverNextExpectedSeq << ", sack=" << rxQp->m_irn_sack_ << std::endl;
	// 		}
	// 	}
	// }
	if (x == 1 || x == 2 || x == 6 || (x == 4 && m_tlt && rxQp->m_tlt_recvState != TLT_STATE_IDLE)){ //generate ACK or NACK
		qbbHeader seqh;
		seqh.SetSeq(rxQp->ReceiverNextExpectedSeq);
		seqh.SetPG(ch.udp.pg);
		seqh.SetSport(ch.udp.dport);
		seqh.SetDport(ch.udp.sport);
		seqh.SetIntHeader(ch.udp.ih);

		if (m_irn) {
			if (x == 2) {
				seqh.SetIrnNack(ch.udp.seq);
				seqh.SetIrnNackSize(payload_size);
			} else {
				seqh.SetIrnNack(0); // NACK without ackSyndrome (ACK) in loss recovery mode
				seqh.SetIrnNackSize(0);
			}
		}

		if (ecnbits && m_cc_mode != 1)
			seqh.SetCnp();

		Ptr<Packet> newp = Create<Packet>(std::max(60-14-20-(int)seqh.GetSerializedSize(), 0));
		newp->AddHeader(seqh);

		Ipv4Header head;	// Prepare IPv4 header
		head.SetDestination(Ipv4Address(ch.sip));
		head.SetSource(Ipv4Address(ch.dip));
		head.SetProtocol(x == 1 ? 0xFC : 0xFD); //ack=0xFC nack=0xFD
		head.SetTtl(64);
		head.SetPayloadSize(newp->GetSize());
		head.SetIdentification(rxQp->m_ipid++);

		{
			FlowIDNUMTag fit;
			if (p->PeekPacketTag(fit)) {
				newp->AddPacketTag(fit);
			}
		}

		newp->AddHeader(head);
		AddHeader(newp, 0x800);	// Attach PPP header

		if (m_tlt) {
			TltTag tlt;
			if (IsWindowBasedCC()) {
				if (rxQp->m_tlt_recvState == TLT_STATE_IMPORTANT)
					tlt.SetType(TltTag::PACKET_IMPORTANT_ECHO);
				else if (rxQp->m_tlt_recvState == TLT_STATE_IMPORTANT_FORCE)
					tlt.SetType(TltTag::PACKET_IMPORTANT_ECHO_FORCE);
				else
					tlt.SetType(TltTag::PACKET_IMPORTANT_CONTROL);
				rxQp->m_tlt_recvState = TLT_STATE_IDLE;
				tlt.SetControlType(TltTag::PACKET_NACK);
				newp->AddPacketTag(tlt);

				#if TLT_DEBUG_ENABLE
				char tbuf[128] = { 0, };
				if (x == 2)
					sprintf(tbuf, "(NACK %u-%u)", ch.udp.seq, ch.udp.seq + payload_size);
				tlt_debug_send_print(rxQp, rxQp->ReceiverNextExpectedSeq, tlt, std::string(tbuf));
				#endif
			} else {
				tlt.SetType(TltTag::PACKET_IMPORTANT_CONTROL);
				tlt.SetControlType(x == 1 ? TltTag::PACKET_ACK : TltTag::PACKET_NACK);
				newp->AddPacketTag(tlt);
				tlt_debug_send_print(rxQp, rxQp->ReceiverNextExpectedSeq, tlt, "");
			}
		}

		// send
		uint32_t nic_idx = GetNicIdxOfRxQp(rxQp);
		m_nic[nic_idx].dev->RdmaEnqueueHighPrioQ(newp);
		m_nic[nic_idx].dev->TriggerTransmit();
	}
	return 0;
}

void RdmaHw::SendDcqcnCnp(Ptr<RdmaRxQueuePair> rxQp){
	if (rxQp == 0 || rxQp->m_ecn_source.ecnbits == 0 || rxQp->m_ecn_source.total == 0) {
		return;
	}

	// RxQp stores ports in receiver perspective; sender sport is rxQp->dport.
	CnHeader cnh(rxQp->dport,
		     rxQp->m_ecn_source.qIndex,
		     rxQp->m_ecn_source.ecnbits,
		     rxQp->m_ecn_source.qfb,
		     rxQp->m_ecn_source.total);
	Ptr<Packet> p = Create<Packet>(std::max(60 - 14 - 20 - static_cast<int>(cnh.GetSerializedSize()), 0));
	p->AddHeader(cnh);

	Ipv4Header head;
	head.SetDestination(Ipv4Address(rxQp->dip));
	head.SetSource(Ipv4Address(rxQp->sip));
	head.SetProtocol(RDMA_L3_PROT_CNP);
	head.SetTtl(64);
	head.SetPayloadSize(p->GetSize());
	head.SetIdentification(rxQp->m_ipid++);
	p->AddHeader(head);
	AddHeader(p, 0x800);

	uint32_t nic_idx = GetNicIdxOfRxQp(rxQp);
	m_nic[nic_idx].dev->RdmaEnqueueHighPrioQ(p);
	m_nic[nic_idx].dev->TriggerTransmit();

	rxQp->m_ecn_source.ecnbits = 0;
	rxQp->m_ecn_source.qfb = 0;
	rxQp->m_ecn_source.total = 0;
}

void RdmaHw::HandleDcqcnCnpTimer(Ptr<RdmaRxQueuePair> rxQp){
	if (rxQp == 0) {
		return;
	}
	if (rxQp->m_qcnPending) {
		rxQp->m_qcnPending = false;
		SendDcqcnCnp(rxQp);
		rxQp->QcnTimerEvent = Simulator::Schedule(MicroSeconds(DCQCN_STANDALONE_CNP_INTERVAL_US),
			&RdmaHw::HandleDcqcnCnpTimer, this, rxQp);
	}
}

void RdmaHw::MaybeSendDcqcnCnp(Ptr<RdmaRxQueuePair> rxQp){
	if (rxQp == 0) {
		return;
	}
	if (!rxQp->QcnTimerEvent.IsRunning()) {
		rxQp->m_qcnPending = false;
		SendDcqcnCnp(rxQp);
		rxQp->QcnTimerEvent = Simulator::Schedule(MicroSeconds(DCQCN_STANDALONE_CNP_INTERVAL_US),
			&RdmaHw::HandleDcqcnCnpTimer, this, rxQp);
	} else {
		rxQp->m_qcnPending = true;
	}
}

int RdmaHw::ReceiveCnp(Ptr<Packet> p, CustomHeader &ch){
	// QCN on NIC
	// This is a Congestion signal
	// Then, extract data from the congestion packet.
	// We assume, without verify, the packet is destinated to me
	uint32_t qIndex = ch.cnp.qIndex;
	if (qIndex == 1){		//DCTCP
		std::cout << "TCP--ignore\n";
		return 0;
	}
	uint16_t udpport = ch.cnp.fid; // corresponds to the sport
	uint8_t ecnbits = ch.cnp.ecnBits;
	uint16_t qfb = ch.cnp.qfb;
	uint16_t total = ch.cnp.total;
	(void) ecnbits;
	(void) qfb;
	(void) total;

	// get qp
	Ptr<RdmaQueuePair> qp = GetQp(udpport, qIndex);
	if (qp == NULL) {
		std::cout << "ERROR: QCN NIC cannot find the flow\n";
		return 0;
	}
	// get nic
	uint32_t nic_idx = GetNicIdxOfQp(qp);
	Ptr<QbbNetDevice> dev = m_nic[nic_idx].dev;

	if (qp->m_rate == 0)			//lazy initialization	
	{
		qp->m_rate = dev->GetDataRate();
		if (m_cc_mode == 1){
			qp->mlx.m_targetRate = dev->GetDataRate();
		}else if (m_cc_mode == 3){
			qp->hp.m_curRate = dev->GetDataRate();
			if (m_multipleRate){
				for (uint32_t i = 0; i < IntHeader::maxHop; i++)
					qp->hp.hopState[i].Rc = dev->GetDataRate();
			}
		}else if (m_cc_mode == 7){
			qp->tmly.m_curRate = dev->GetDataRate();
		}
	}
	if (m_cc_mode == 1) {
		cnp_received_mlx(qp);
	}
	return 0;
}

int RdmaHw::ReceiveAck(Ptr<Packet> p, CustomHeader &ch){
	uint16_t qIndex = ch.ack.pg;
	uint16_t port = ch.ack.dport;
	uint32_t seq = ch.ack.seq;
	int i;
	Ptr<RdmaQueuePair> qp = GetQp(port, qIndex);
	if (qp == NULL){
		std::cout << "ERROR: " << "node:" << m_node->GetId() << ' ' << (ch.l3Prot == 0xFC ? "ACK" : "NACK") << " NIC cannot find the flow\n";
		return 0;
	}
	if (m_creditbouncer && qp->cb.m_enabled){
		// CB uses message-level receiver-driven completion/recovery only.
		return 0;
	}
	uint32_t nic_idx = GetNicIdxOfQp(qp);
	Ptr<QbbNetDevice> dev = m_nic[nic_idx].dev;

	TltTag tlt;
	if (m_tlt)
	{
		if (p->PeekPacketTag(tlt)) {
			tlt_debug_recv_print(qp, seq, tlt, std::string());
			if (IsWindowBasedCC()) {
				if (tlt.GetType() == TltTag::PACKET_IMPORTANT_ECHO || tlt.GetType() == TltTag::PACKET_IMPORTANT_ECHO_FORCE) {
					if (qp->tlt.m_sendState == TLT_STATE_IMPORTANT || qp->tlt.m_sendState == TLT_STATE_IMPORTANT_FORCE) {
						std::cout << "WARN : Already pending important here... two important echoes?" << std::endl;
					}
					if (qp->tlt.m_highestImportantAck < seq)
						qp->tlt.m_highestImportantAck = seq;
					qp->tlt.m_sendState = TLT_STATE_IMPORTANT;
					if (tlt.GetType() == TltTag::PACKET_IMPORTANT_ECHO_FORCE) {
						// if (seq < qp->snd_una) {
						// 	// do not deliver to CC layer
						// 	TLT_DEBUG_PRINT("Flow " << qp->m_flow_id << " : -> Not delivering because seq=" << seq << ", snd_una=" << qp->snd_una);
						// 	return 0;
						// }
					}
				}
			}
		} else {
			std::cerr << "Warning: Cannot find TLT tag" << std::endl;
		}

		// if (Simulator::Now().GetSeconds() * 1000 >= 20.4073 && qp->m_flow_id == TLT_DEBUG_TARGET) {  //22.16406
		// 	std::cerr << "Reached debug point." << std::endl;
		// }

		if (IsWindowBasedCC()) {
			qp->tlt.m_tlt_unimportant_pkts.discardUpTo(SequenceNumber32(seq));
			qp->tlt.m_tlt_unimportant_pkts_prev_round->discardUpTo(SequenceNumber32(seq));
			qp->tlt.m_tlt_unimportant_pkts_current_round->discardUpTo(SequenceNumber32(seq));
		}
	}

	if (m_ack_interval == 0)
		std::cout << "ERROR: shouldn't receive ack\n";
	else {
		if (!m_backto0){
			qp->Acknowledge(seq);
		}else {
			uint32_t goback_seq = seq / m_chunk * m_chunk;
			qp->Acknowledge(goback_seq);
		}
		if (qp->irn.m_enabled) {
			// handle NACK
			NS_ASSERT(ch.l3Prot == 0xFD);

			//for bdp-fc calculation update m_irn_maxAck
			if (seq > qp->irn.m_highest_ack)
				qp->irn.m_highest_ack = seq;
			

			if (ch.ack.irnNackSize != 0) {
				// ch.ack.irnNack contains the seq triggered this NACK
				qp->irn.m_sack.sack(ch.ack.irnNack, ch.ack.irnNackSize);
			}

			if (qp->tlt.m_enabled && IsWindowBasedCC() && ch.ack.irnNackSize > 0) {
				SelectivePacketQueue::SackList list;
				list.push_back(std::pair<SequenceNumber32, SequenceNumber32>(SequenceNumber32(ch.ack.irnNack), SequenceNumber32(ch.ack.irnNack+ch.ack.irnNackSize)));
				qp->tlt.m_tlt_unimportant_pkts.updateSack(list);
				qp->tlt.m_tlt_unimportant_pkts_prev_round->updateSack(list);
				qp->tlt.m_tlt_unimportant_pkts_current_round->updateSack(list);
			}

			{
				uint32_t sack_seq, sack_len;
				if (qp->irn.m_sack.peekFrontBlock(&sack_seq, &sack_len)) {
					if (qp->snd_una == sack_seq) {
						qp->snd_una += sack_len;
					}
				}
			}

			qp->irn.m_sack.discardUpTo(qp->snd_una);
			
			if (qp->snd_nxt < qp->snd_una) {
				qp->snd_nxt = qp->snd_una;
			}
			//if (qp->irn.m_sack.IsEmpty())  { // 
			if (qp->irn.m_recovery && qp->snd_una >= qp->irn.m_recovery_seq) {
				qp->irn.m_recovery = false;
			}

			if (qp->tlt.m_enabled && IsWindowBasedCC()) {
				qp->tlt.m_tlt_unimportant_pkts.discardUpTo(SequenceNumber32(qp->snd_una));
			}
			
			// {
			// 	FlowIDNUMTag fit;
			// 	if (p->PeekPacketTag(fit)) {
			// 		if (fit.GetId() == 15 ) {
			// 			std::cout << "FLOW: Nack " << ch.udp.seq << ", (" << ch.ack.irnNack << "), snd_una=" << qp->snd_una << ", snd_nxt=" << qp->snd_nxt << ", sack=" << qp->irn.m_sack << std::endl;
			// 		}
			// 	}
			// }

		} else {
			if (qp->snd_nxt < qp->snd_una) {
				qp->snd_nxt = qp->snd_una;
			}
		}
		if (qp->IsFinished()){
			if(qp->tlt.m_enabled && IsWindowBasedCC()) {
				if (!qp->tlt.m_sent_fin && qp->tlt.m_sendState != TLT_STATE_IDLE) {
					// TLT : Tail loss (packet transmitted after last Imp packet) might incur timeout
					GenerateTltFin(qp);
					qp->tlt.m_sent_fin = true;
					QpComplete(qp);
				}
				else
				{
					QpComplete(qp);
					qp->tlt.m_sent_fin = true;
				}
			} else {
				QpComplete(qp);
			}
		}
	}

	/** 
	 * IB Spec Vol. 1 o9-85
	 * The requester need not separately time each request launched into the
	 * fabric, but instead simply begins the timer whenever it is expecting a response.
	 * Once started, the timer is restarted each time an acknowledge
	 * packet is received as long as there are outstanding expected responses.
	 * The timer does not detect the loss of a particular expected acknowledge
	 * packet, but rather simply detects the persistent absence of response
	 * packets.
	 * */
	if (!qp->IsFinished() && qp->GetOnTheFly() > 0) {
		if (qp->m_retransmit.IsRunning())
			qp->m_retransmit.Cancel();
		qp->m_retransmit = Simulator::Schedule(qp->GetRto(m_mtu), &RdmaHw::HandleTimeout, this, qp, qp->GetRto(m_mtu));
	}

	if (m_irn) {
		if (ch.ack.irnNackSize != 0) {
			if (!qp->irn.m_recovery) {
				qp->irn.m_recovery_seq = qp->snd_nxt;
				RecoverQueue(qp);
				qp->irn.m_recovery = true;
			}
		} else {
			if (qp->irn.m_recovery) {
				qp->irn.m_recovery = false;
			}
		}
			
	} else if (ch.l3Prot == 0xFD) // NACK
		RecoverQueue(qp);

	if (m_cc_mode == 3){
		HandleAckHp(qp, p, ch);
	}else if (m_cc_mode == 7){
		HandleAckTimely(qp, p, ch);
	}else if (m_cc_mode == 8){
		HandleAckDctcp(qp, p, ch);
	}
	// ACK may advance the on-the-fly window, allowing more packets to send
	dev->TriggerTransmit();
	

	// Must be done after DoForwardUp(TriggerTransmit)
	if (qp->tlt.m_enabled && IsWindowBasedCC()) {
#if 0
		bool cond_window = !qp->IsWinBound() && (!qp->irn.m_enabled || qp->CanIrnTransmit(m_mtu));
		// checking if qp->tlt.m_sendState == TLT_STATE_IMPORTANT is not correct.
		// queued important packet might be send next time..
		// TODO: think about this..
		if (qp->tlt.m_sendState == TLT_STATE_IMPORTANT && !cond_window && !qp->IsFinished()) {
			// TLT force transmission required
			qp->tlt.m_sendUnit = m_mtu; // Reset to MTU(MSS)
			bool tlt_success = forceSendTLT(qp, nullptr);
			// TODO: Fill up here
		}
#endif
		if (tlt.GetType() == TltTag::PACKET_IMPORTANT_ECHO || tlt.GetType() == TltTag::PACKET_IMPORTANT_ECHO_FORCE) {
			qp->tlt.m_tlt_unimportant_pkts_prev_round = qp->tlt.m_tlt_unimportant_pkts_current_round;
			qp->tlt.m_tlt_unimportant_pkts_current_round = CreateObject<SelectivePacketQueue>();
			NS_ASSERT(qp->tlt.m_tlt_unimportant_pkts_prev_round);
			NS_ASSERT(qp->tlt.m_tlt_unimportant_pkts_current_round);
		}
	}
	return 0;
}

void RdmaHw::GenerateTltFin(Ptr<RdmaQueuePair> qp) { //generate ACK or NACK
	Ptr<Packet> p = Create<Packet> (0);
	// add SeqTsHeader
	SeqTsHeader seqTs;
	seqTs.SetSeq (qp->m_size);
	seqTs.SetPG (qp->m_pg);
	p->AddHeader (seqTs);
	// add udp header
	UdpHeader udpHeader;
	udpHeader.SetDestinationPort (qp->dport);
	udpHeader.SetSourcePort (qp->sport);
	p->AddHeader (udpHeader);
	// add ipv4 header
	Ipv4Header ipHeader;
	ipHeader.SetSource (qp->sip);
	ipHeader.SetDestination (qp->dip);
	ipHeader.SetProtocol (0x11);
	ipHeader.SetPayloadSize (p->GetSize());
	ipHeader.SetTtl (64);
	ipHeader.SetTos (0);
	ipHeader.SetIdentification (qp->m_ipid);
	p->AddHeader(ipHeader);
	// add ppp header
	PppHeader ppp;
	ppp.SetProtocol (0x0021); // EtherToPpp(0x800), see point-to-point-net-device.cc
	p->AddHeader (ppp);

	// attach Stat Tag 
	{
		FlowIDNUMTag fint;
		if (!p->PeekPacketTag(fint)) {
			fint.SetId(qp->m_flow_id);
			fint.SetFlowSize(qp->m_size);
			p->AddPacketTag(fint);
		}
		FlowStatTag fst;
		uint64_t size = qp->m_size;
		if (!p->PeekPacketTag(fst))
		{
			fst.SetType(FlowStatTag::FLOW_FIN);
			fst.setInitiatedTime(Simulator::Now().GetSeconds());
			p->AddPacketTag(fst);
		}
	}

	TltTag tlt;
	tlt.SetType(TltTag::PACKET_IMPORTANT);
	tlt.SetControlType(TltTag::PACKET_PAYLOAD_EOF);
	p->AddPacketTag(tlt);
	
	char mss[1024]; sprintf(mss, "%.8lf", Simulator::Now().GetSeconds() * 1000.);
	if (TLT_IS_DEBUG_TARGET(qp))
		TLT_DEBUG_PRINT("[" <<  std::string(mss) <<"] Flow " << (qp->m_flow_id) << " : Send FIN");
	// send
	uint32_t nic_idx = GetNicIdxOfQp(qp);
	m_nic[nic_idx].dev->RdmaEnqueueHighPrioQ(p);
	m_nic[nic_idx].dev->TriggerTransmit();
}

bool RdmaHw::forceSendTLT(Ptr<RdmaQueuePair> qp, int *pSize) {
	if (!qp->tlt.m_enabled)
		return false;
	if (qp->IsFinished())
		return false;
	if (!IsWindowBasedCC())
		return false;	//no force transmission on rate-based CC

	if (qp->tlt.m_tlt_unimportant_pkts.size() == 0) {
		std::cerr << "WARNING : No Data to Force Retransmit : Must not reach here!! SocketId=" << qp->m_flow_id << std::endl;
		abort();
    	return false;
	}
	
	NS_ASSERT(qp->tlt.m_tlt_unimportant_pkts.size() > 0);
	// first packet as unimportant

	auto targetPair = qp->tlt.m_tlt_unimportant_pkts.peek(m_mtu);
	SequenceNumber32 targetSeq = targetPair.first;
	uint32_t targetSz = targetPair.second;
	
	if (targetSeq >= SequenceNumber32(qp->m_size) || !targetSz) {
		NS_LOG_INFO("No Data to Force Retransmit");
		return false;
	}

	uint32_t nPacketsSent = 0;
	uint32_t availSz = qp->m_size - targetSeq.GetValue();
	availSz = std::min(availSz, targetSz);

	uint32_t actualSz = availSz;

	bool is_loss_probable = !(qp->tlt.m_tlt_unimportant_pkts_prev_round->isEmpty() && qp->tlt.m_tlt_unimportant_pkts_prev_round->isDirty());
	uint32_t tlt_su = is_loss_probable ? m_mtu : 1;
	actualSz = std::min(tlt_su, availSz);
	
	if(!qp->tlt.m_tlt_unimportant_pkts_prev_round->isEmpty()) {
		auto ret = qp->tlt.m_tlt_unimportant_pkts_prev_round->pop(actualSz);
		targetSeq = ret.first;
		actualSz = ret.second;
		NS_ASSERT(targetSeq >= SequenceNumber32(qp->snd_una));
		qp->tlt.m_tlt_unimportant_pkts.discard(targetSeq, actualSz);
		qp->tlt.m_tlt_unimportant_pkts_current_round->discard(targetSeq, actualSz);
	} else {
		auto ret = qp->tlt.m_tlt_unimportant_pkts.pop(actualSz); // assume queue not modified between peek and pop
		NS_ABORT_UNLESS(targetSeq == ret.first);
		NS_ABORT_UNLESS(actualSz == ret.second);
		qp->tlt.m_tlt_unimportant_pkts_prev_round->discard(targetSeq, actualSz);
		qp->tlt.m_tlt_unimportant_pkts_current_round->discard(targetSeq, actualSz);
	}

	NS_ASSERT(qp->tlt.m_sendState == TLT_STATE_IMPORTANT);

	if (actualSz) {
		qp->tlt.m_forcetx_queue.push_back(std::pair<uint32_t, uint32_t>(targetSeq.GetValue(), actualSz));
		qp->tlt.m_sendState = TLT_STATE_SCHEDULED;
		if(pSize)
			*pSize = actualSz;
		qp->tlt.stat_uimp_forcegen += actualSz;
		qp->tlt.stat_uimp_forcegen_cnt++;
		nPacketsSent++;
	}

	return (nPacketsSent > 0);
}

int RdmaHw::Receive(Ptr<Packet> p, CustomHeader &ch){
	CreditBouncerTag cbTag;
	if (m_creditbouncer && p->PeekPacketTag(cbTag) && cbTag.GetType() != CreditBouncerTag::CB_MSG_NONE) {
		int rc = ReceiveCreditBouncer(p, ch);
		if (cbTag.IsRequest() && ch.l3Prot == RDMA_L3_PROT_UDP)
			return rc;
		if (ch.l3Prot == RDMA_L3_PROT_CREDITBOUNCER)
			return rc;
	}
	if (ch.l3Prot == RDMA_L3_PROT_UDP){ // UDP
		return ReceiveUdp(p, ch);
	}else if (ch.l3Prot == RDMA_L3_PROT_CNP){ // CNP
		return ReceiveCnp(p, ch);
	}else if (ch.l3Prot == RDMA_L3_PROT_NACK){ // NACK
		return ReceiveAck(p, ch);
	}else if (ch.l3Prot == RDMA_L3_PROT_ACK){ // ACK
		return ReceiveAck(p, ch);
	}
	return 0;
}

int RdmaHw::ReceiveCreditBouncer(Ptr<Packet> p, CustomHeader &ch){
	if (!m_creditbouncer)
		return 0;

	CreditBouncerTag cbTag;
	bool hasCbTag = p->PeekPacketTag(cbTag);
	bool isGrantReq = hasCbTag && cbTag.IsGrantReq();
	bool isRequest = hasCbTag && cbTag.IsRequest();
	bool isGrant = hasCbTag && cbTag.IsGrant();
	bool isBouncedCredit = hasCbTag && cbTag.IsBouncedCredit();
	bool isResend = hasCbTag && cbTag.IsResend();
	uint32_t creditReqBytes = hasCbTag ? cbTag.GetCreditReqBytes() : 0;
	uint32_t creditBytes = hasCbTag ? cbTag.GetCreditBytes() : 0;
	uint32_t unsolCreditBytes = hasCbTag ? cbTag.GetUnsolCreditBytes() : 0;
	uint32_t unsolCreditDataBytes = hasCbTag ? cbTag.GetUnsolCreditDataBytes() : 0;
	if (!hasCbTag)
		return 0;

	auto saturatingAdd = [](uint64_t base, uint64_t delta) -> uint64_t {
		if (UINT64_MAX - base < delta)
			return UINT64_MAX;
		return base + delta;
	};

	auto applyNewAnnouncement = [&](Ptr<RdmaRxQueuePair> rxQp, SenderState &senderState) -> bool {
		bool newAnnouncement = creditReqBytes > 0 && !rxQp->cb.m_announcementAccounted;
		if (!newAnnouncement)
			return false;

		rxQp->cb.m_announcementAccounted = true;
		rxQp->cb.m_announcementExpectedBytes = creditReqBytes;
		rxQp->cb.m_receivedDataBytes = 0;
		rxQp->cb.m_completionNotified = false;
		rxQp->cb.m_expectedChunks = m_mtu == 0
			? 0
			: static_cast<uint32_t>((rxQp->cb.m_announcementExpectedBytes + m_mtu - 1) / m_mtu);
		rxQp->cb.m_receivedChunks = 0;
		rxQp->cb.m_grantedChunkLimit = 0;
		rxQp->cb.m_receivedChunkBitmap.assign(rxQp->cb.m_expectedChunks, 0);
		if (rxQp->cb.m_resendEvent.IsRunning())
			rxQp->cb.m_resendEvent.Cancel();

		senderState.m_pendingDemandDataBytes = saturatingAdd(senderState.m_pendingDemandDataBytes, creditReqBytes);
		rxQp->cb.m_pendingDemandBytes = saturatingAdd(rxQp->cb.m_pendingDemandBytes, creditReqBytes);

		if (unsolCreditBytes > 0 || unsolCreditDataBytes > 0) {
			uint64_t grantWire = unsolCreditBytes;
			uint64_t grantData = std::min<uint64_t>(unsolCreditDataBytes, senderState.m_pendingDemandDataBytes);

			if (senderState.m_pendingDemandDataBytes >= grantData)
				senderState.m_pendingDemandDataBytes -= grantData;
			else
				senderState.m_pendingDemandDataBytes = 0;

			if (rxQp->cb.m_pendingDemandBytes >= grantData)
				rxQp->cb.m_pendingDemandBytes -= grantData;
			else
				rxQp->cb.m_pendingDemandBytes = 0;

			senderState.m_outstandingDataBytes = saturatingAdd(senderState.m_outstandingDataBytes, grantData);
			if (senderState.m_srpbBytes >= grantWire)
				senderState.m_srpbBytes -= grantWire;
			else
				senderState.m_srpbBytes = 0;
			senderState.m_OutstandingBytes = saturatingAdd(senderState.m_OutstandingBytes, grantWire);
			m_creditbouncer_globalOutstanding_bytes = saturatingAdd(m_creditbouncer_globalOutstanding_bytes, grantWire);
		}
		if (rxQp->cb.m_expectedChunks > 0)
			ScheduleCreditBouncerResend(rxQp);
		return true;
	};

	// REQUEST data packets are handled here directly so receiver-side
	// completion can finish the sender QP locally without a reply packet.
	if (isRequest && ch.l3Prot == RDMA_L3_PROT_UDP) {
		uint8_t ecnbits = ch.GetIpv4EcnBits();
		uint32_t payload_size = p->GetSize() - ch.GetSerializedSize();
		CreditBouncerTag::DataClass rxDataClass = cbTag.GetDataClass();
		bool csnMarkedOnData = m_creditbouncerCsnEnable && cbTag.IsCsnMarked();
		bool priorityFlowOnData = cbTag.IsPriorityFlow();

		Ptr<RdmaRxQueuePair> rxQp = GetRxQp(ch.dip, ch.sip, ch.udp.dport, ch.udp.sport, ch.udp.pg, true);
		if (rxQp && rxQp->cb.m_enabled) {
			SenderState &senderState = GetOrCreateSenderState(ch.sip, rxQp);
			senderState.m_enabled = true;
			senderState.m_lastEcnMarked = (ecnbits != 0);

			// Apply AIMD immediately on receiver-side data arrival before any
			// per-packet credit/accounting updates, matching the intended ingress ordering.
			if (m_creditbouncerCcEnable)
				UpdateSenderStateAimdOnData(senderState, csnMarkedOnData, ecnbits != 0, priorityFlowOnData);
			applyNewAnnouncement(rxQp, senderState);

			bool firstSeenChunk = true;
			uint32_t chunkIdx = UINT32_MAX;
			if (m_mtu > 0) {
				chunkIdx = ch.udp.seq / m_mtu;
				if (chunkIdx < rxQp->cb.m_expectedChunks &&
				    rxQp->cb.m_receivedChunkBitmap.size() == rxQp->cb.m_expectedChunks) {
					firstSeenChunk = (rxQp->cb.m_receivedChunkBitmap[chunkIdx] == 0);
					if (firstSeenChunk) {
						rxQp->cb.m_receivedChunkBitmap[chunkIdx] = 1;
						rxQp->cb.m_receivedChunks++;
					}
				}
			}

			bool wasScheduled = senderState.m_outstandingDataBytes > 0 && cbTag.IsScheduledData();
			uint64_t pendingBeforeReclaim = senderState.m_pendingDemandDataBytes;
			uint64_t outDataBeforeReclaim = senderState.m_outstandingDataBytes;
			uint64_t outWireBeforeReclaim = senderState.m_OutstandingBytes;
			uint64_t srpbBeforeReclaim = senderState.m_srpbBytes;
			uint64_t globalOutBeforeReclaim = m_creditbouncer_globalOutstanding_bytes;
			uint64_t msgPendingBeforeReclaim = rxQp->cb.m_pendingDemandBytes;
			uint64_t creditUsedData = 0;
			uint64_t creditUsedWire = 0;
			if (firstSeenChunk) {
				creditUsedData = std::min<uint64_t>(payload_size, senderState.m_outstandingDataBytes);
				senderState.m_outstandingDataBytes -= creditUsedData;
				creditUsedWire = CbAddHeaderOverhead(creditUsedData, m_mtu, true);
				if (senderState.m_srpbBytes + creditUsedWire > senderState.m_maxSrpbBytes)
					senderState.m_srpbBytes = senderState.m_maxSrpbBytes;
				else
					senderState.m_srpbBytes += creditUsedWire;
				if (senderState.m_OutstandingBytes >= creditUsedWire)
					senderState.m_OutstandingBytes -= creditUsedWire;
				else
					senderState.m_OutstandingBytes = 0;
				if (m_creditbouncer_globalOutstanding_bytes >= creditUsedWire)
					m_creditbouncer_globalOutstanding_bytes -= creditUsedWire;
				else
					m_creditbouncer_globalOutstanding_bytes = 0;
			}

			if (m_creditbouncer_debugLog && m_creditbouncer_debugLogCount < m_creditbouncer_debugMaxLogs){
				std::cerr << "CBDBG reclaim_on_data node=" << m_node->GetId()
					<< " flow=" << rxQp->m_flow_id
					<< " first_seen=" << (firstSeenChunk ? 1 : 0)
					<< " chunk=" << chunkIdx
					<< " payload=" << payload_size
					<< " used_data=" << creditUsedData
					<< " used_wire=" << creditUsedWire
					<< " pending_before=" << pendingBeforeReclaim
					<< " pending_after=" << senderState.m_pendingDemandDataBytes
					<< " out_data_before=" << outDataBeforeReclaim
					<< " out_data_after=" << senderState.m_outstandingDataBytes
					<< " out_wire_before=" << outWireBeforeReclaim
					<< " out_wire_after=" << senderState.m_OutstandingBytes
					<< " srpb_before=" << srpbBeforeReclaim
					<< " srpb_after=" << senderState.m_srpbBytes
					<< " msg_pending_before=" << msgPendingBeforeReclaim
					<< " msg_pending_after=" << rxQp->cb.m_pendingDemandBytes
					<< " gOut_before=" << globalOutBeforeReclaim
					<< " gOut_after=" << m_creditbouncer_globalOutstanding_bytes
					<< "\n";
				m_creditbouncer_debugLogCount++;
			}

			if (ecnbits != 0){
				rxQp->m_ecn_source.ecnbits |= ecnbits;
				rxQp->m_ecn_source.qfb++;
			}
			rxQp->m_ecn_source.total++;
			rxQp->m_milestone_rx = m_ack_interval;

			if (rxQp->m_flow_id < 0) {
				FlowIDNUMTag fit;
				if (p->PeekPacketTag(fit))
					rxQp->m_flow_id = fit.GetId();
			}

			if (firstSeenChunk && rxQp->cb.m_announcementExpectedBytes > 0) {
				uint64_t acceptedPayload = payload_size;
				if (m_mtu > 0 && chunkIdx < rxQp->cb.m_expectedChunks) {
					uint64_t chunkBase = static_cast<uint64_t>(chunkIdx) * m_mtu;
					if (chunkBase < rxQp->cb.m_announcementExpectedBytes) {
						uint64_t maxChunkPayload = std::min<uint64_t>(
							m_mtu, rxQp->cb.m_announcementExpectedBytes - chunkBase);
						acceptedPayload = std::min<uint64_t>(acceptedPayload, maxChunkPayload);
					} else {
						acceptedPayload = 0;
					}
				}
				rxQp->cb.m_receivedDataBytes = std::min<uint64_t>(
					rxQp->cb.m_announcementExpectedBytes,
					rxQp->cb.m_receivedDataBytes + acceptedPayload);
			}

			// Once the full message has been received, locate the sender-side
			// QP on its owner and complete it locally instead of emitting a reply packet.
			if (!rxQp->cb.m_completionNotified &&
				rxQp->cb.m_announcementExpectedBytes > 0 &&
				((rxQp->cb.m_expectedChunks > 0 && rxQp->cb.m_receivedChunks >= rxQp->cb.m_expectedChunks) ||
				 rxQp->cb.m_receivedDataBytes >= rxQp->cb.m_announcementExpectedBytes)) {
				rxQp->cb.m_completionNotified = true;
				if (rxQp->cb.m_resendEvent.IsRunning())
					rxQp->cb.m_resendEvent.Cancel();
				auto ownerIt = s_creditbouncerOwnerByIp.find(ch.sip);
				if (ownerIt != s_creditbouncerOwnerByIp.end() && ownerIt->second != 0) {
					ownerIt->second->CompleteCreditBouncerSenderQp(
						ch.udp.sport, ch.udp.pg, rxQp->m_flow_id);
				} else {
					std::cerr << "WARNING: CB completion owner lookup miss node=" << m_node->GetId()
						<< " sender=" << Ipv4Address(ch.sip)
						<< " flow=" << rxQp->m_flow_id
						<< " sport=" << ch.udp.sport
							<< " pg=" << ch.udp.pg
							<< "\n";
				}
				CleanupCreditBouncerSenderState(rxQp);
				if (m_creditbouncer_debugLog && m_creditbouncer_debugLogCount < m_creditbouncer_debugMaxLogs){
					std::cerr << "CBDBG complete_sender_qp node=" << m_node->GetId()
						<< " flow=" << rxQp->m_flow_id
						<< " sender=" << Ipv4Address(ch.sip)
						<< " sport=" << ch.udp.sport
						<< " pg=" << ch.udp.pg
						<< " expected=" << rxQp->cb.m_announcementExpectedBytes
						<< " received=" << rxQp->cb.m_receivedDataBytes
						<< "\n";
					m_creditbouncer_debugLogCount++;
				}
			}

			if (m_creditbouncer_debugLog && m_creditbouncer_debugLogCount < m_creditbouncer_debugMaxLogs && (wasScheduled || senderState.m_pendingDemandDataBytes > 0)){
				std::cerr << "CBDBG data node=" << m_node->GetId()
					<< " flow=" << rxQp->m_flow_id
					<< " class=" << CbDataClassToString(rxDataClass)
					<< " usedData=" << creditUsedData
					<< " usedWire=" << creditUsedWire
					<< " anno=" << (rxQp->cb.m_announcementAccounted ? 1 : 0)
					<< " unsolWire=" << unsolCreditBytes
					<< " unsolData=" << unsolCreditDataBytes
					<< " usedUnsolFlag=" << (cbTag.IsUsedUnsolCredit() ? 1 : 0)
					<< " pending=" << senderState.m_pendingDemandDataBytes
					<< " outData=" << senderState.m_outstandingDataBytes
					<< " srpbRem=" << senderState.m_srpbBytes
					<< " gOut=" << m_creditbouncer_globalOutstanding_bytes
					<< " ecn=" << (ecnbits != 0 ? 1 : 0)
					<< " csn=" << (csnMarkedOnData ? 1 : 0)
					<< " prio=" << (priorityFlowOnData ? 1 : 0)
					<< "\n";
				m_creditbouncer_debugLogCount++;
			}

			if (m_creditbouncer_debugLog && m_creditbouncer_debugLogCount < m_creditbouncer_debugMaxLogs){
				std::cerr << "CBDBG rx_data node=" << m_node->GetId()
					<< " flow=" << rxQp->m_flow_id
					<< " class=" << CbDataClassToString(rxDataClass)
					<< " first_seen=" << (firstSeenChunk ? 1 : 0)
					<< " chunk=" << chunkIdx
					<< " payload=" << payload_size
					<< " credit_req=" << creditReqBytes
					<< " unsol_wire=" << unsolCreditBytes
					<< " unsol_data=" << unsolCreditDataBytes
					<< " used_unsol=" << (cbTag.IsUsedUnsolCredit() ? 1 : 0)
					<< " prio=" << (priorityFlowOnData ? 1 : 0)
					<< " pending_after=" << senderState.m_pendingDemandDataBytes
					<< " out_after=" << senderState.m_outstandingDataBytes
					<< " srpb_after=" << senderState.m_srpbBytes
					<< " recvd=" << rxQp->cb.m_receivedDataBytes
					<< " expected=" << rxQp->cb.m_announcementExpectedBytes
					<< " recv_chunks=" << rxQp->cb.m_receivedChunks
					<< " expected_chunks=" << rxQp->cb.m_expectedChunks
					<< "\n";
				m_creditbouncer_debugLogCount++;
			}

			if (!rxQp->cb.m_completionNotified && rxQp->cb.m_expectedChunks > 0)
				ScheduleCreditBouncerResend(rxQp);

			if (senderState.m_pendingDemandDataBytes > 0)
				ScheduleCreditGrantTick();
		}
		return 0;
	}

	if (isGrantReq || (isRequest && ch.l3Prot == RDMA_L3_PROT_CREDITBOUNCER)) {
		Ptr<RdmaRxQueuePair> rxQp = GetRxQp(ch.dip, ch.sip, ch.ack.dport, ch.ack.sport, ch.ack.pg, true);
		if (rxQp && rxQp->cb.m_enabled) {
			SenderState &senderState = GetOrCreateSenderState(ch.sip, rxQp);
			senderState.m_enabled = true;
			if (isGrantReq && m_creditbouncerCcEnable) {
				bool ecnMarked = ch.GetIpv4EcnBits() != 0;
				bool csnMarked = m_creditbouncerCsnEnable && hasCbTag && cbTag.IsCsnMarked();
				UpdateSenderStateAimdOnData(senderState, csnMarked, ecnMarked, false);
			}
			bool newAnnouncement = applyNewAnnouncement(rxQp, senderState);
			if (newAnnouncement) {
				if (m_creditbouncer_debugLog && m_creditbouncer_debugLogCount < m_creditbouncer_debugMaxLogs){
					std::cerr << "CBDBG announce_accept node=" << m_node->GetId()
						<< " flow=" << rxQp->m_flow_id
						<< " req=" << creditReqBytes
						<< " unsol_wire=" << unsolCreditBytes
						<< " unsol_data=" << unsolCreditDataBytes
						<< " pending_after=" << senderState.m_pendingDemandDataBytes
						<< " out_after=" << senderState.m_outstandingDataBytes
						<< "\n";
					m_creditbouncer_debugLogCount++;
				}
			} else if (creditReqBytes > 0 && m_creditbouncer_debugLog && m_creditbouncer_debugLogCount < m_creditbouncer_debugMaxLogs){
				std::cerr << "CBDBG announce_skip node=" << m_node->GetId()
					<< " flow=" << rxQp->m_flow_id
					<< " req=" << creditReqBytes
					<< " remembered_req=" << rxQp->cb.m_announcementExpectedBytes
					<< " reason=already_accounted"
					<< "\n";
				m_creditbouncer_debugLogCount++;
			}
			if (m_creditbouncer_debugLog && m_creditbouncer_debugLogCount < m_creditbouncer_debugMaxLogs){
				std::cerr << "CBDBG req node=" << m_node->GetId()
					<< " flow=" << rxQp->m_flow_id
					<< " req=" << creditReqBytes
					<< " new_anno=" << (newAnnouncement ? 1 : 0)
					<< " pair_pending=" << senderState.m_pendingDemandDataBytes
					<< " pair_out_data=" << senderState.m_outstandingDataBytes
					<< " pair_srpb_rem=" << senderState.m_srpbBytes
					<< " msg_pending=" << rxQp->cb.m_pendingDemandBytes
					<< " gOut=" << m_creditbouncer_globalOutstanding_bytes
					<< "\n";
				m_creditbouncer_debugLogCount++;
			}
			if (senderState.m_pendingDemandDataBytes > 0)
				ScheduleCreditGrantTick();
		}
	}

	if (isResend && ch.l3Prot == RDMA_L3_PROT_CREDITBOUNCER) {
		Ptr<RdmaQueuePair> qp = GetQp(ch.ack.dport, ch.ack.pg);
		if (qp && qp->cb.m_enabled && m_mtu > 0) {
			uint32_t startIdx = cbTag.GetResendStartIdx();
			uint32_t numChunks = cbTag.GetResendNumChunks();
			uint32_t nic_idx = GetNicIdxOfQp(qp);
			uint32_t sentPkts = 0;
			for (uint32_t i = 0; i < numChunks; ++i) {
				uint32_t chunkIdx = startIdx + i;
				if (chunkIdx < startIdx)
					break;
				uint64_t offset = static_cast<uint64_t>(chunkIdx) * m_mtu;
				if (offset >= qp->m_size)
					break;
				uint32_t payloadSize = CbNextChunkPayloadBytes(offset, qp->m_size, m_mtu);
				if (payloadSize == 0)
					continue;
				uint64_t chunkEnd = offset + static_cast<uint64_t>(payloadSize);
				if (chunkEnd > qp->snd_nxt)
					continue;
				CreditBouncerTag::DataClass dataClass = CbDataClassForOffset(qp, offset);
				Ptr<Packet> resendData = BuildCreditBouncerDataPacket(
					qp, m_mtu, payloadSize, static_cast<uint32_t>(offset), dataClass, false, false);
				m_nic[nic_idx].dev->RdmaEnqueueHighPrioQ(resendData);
				sentPkts++;
			}
			if (sentPkts > 0)
				m_nic[nic_idx].dev->TriggerTransmit();
		}
	}

	if (isGrant) {
		Ptr<RdmaQueuePair> qp = GetQp(ch.ack.dport, ch.ack.pg);
		if (qp && qp->cb.m_enabled) {
			ReceiverState &receiverState = GetOrCreateReceiverState(qp->dip.Get(), qp);
			receiverState.m_enabled = true;
			uint64_t availWireBefore = receiverState.m_availCreditBytes;
			uint64_t availDataBefore = receiverState.m_availCreditDataBytes;
			uint16_t creditPad = cbTag.GetCreditPad();
			uint64_t minWireNoPad = CB_DATA_HEADERS_ON_WIRE + CB_LINK_OVERHEAD_ON_WIRE + creditPad;
			uint64_t creditData = 0;
			if (creditBytes > minWireNoPad)
				creditData = creditBytes - minWireNoPad;
				receiverState.m_availCreditBytes += creditBytes;
				receiverState.m_availCreditDataBytes += creditData;
			qp->cb.m_statCreditGrantPkts++;
			if (m_creditbouncer_debugLog && m_creditbouncer_debugLogCount < m_creditbouncer_debugMaxLogs){
				std::cerr << "CBDBG rx_grant node=" << m_node->GetId()
					<< " flow=" << qp->m_flow_id
					<< " credit_wire=" << creditBytes
					<< " credit_pad=" << creditPad
					<< " credit_data=" << creditData
					<< " avail_wire_before=" << availWireBefore
					<< " avail_wire_after=" << receiverState.m_availCreditBytes
					<< " avail_data_before=" << availDataBefore
					<< " avail_data_after=" << receiverState.m_availCreditDataBytes
					<< "\n";
				m_creditbouncer_debugLogCount++;
			}

			uint32_t nic_idx = GetNicIdxOfQp(qp);
			m_nic[nic_idx].dev->TriggerTransmit();
		}
	}

	if (isBouncedCredit) {
		// BOUNCED_CREDIT is generated by a switch from a GRANT packet and sent back
		// toward the receiver. It must roll back grant-side accounting.
		// Prefer canonical receiver-side tuple direction used by REQUEST/GRANT_REQ:
		//   GetRxQp(ch.dip, ch.sip, ch.ack.dport, ch.ack.sport, ...)
		// Fall back to legacy direction for backward compatibility with older traces.
		Ptr<RdmaRxQueuePair> rxQp = GetRxQp(
			ch.dip, ch.sip, ch.ack.dport, ch.ack.sport, ch.ack.pg, false);
		uint32_t senderAddr = ch.sip;
		if (rxQp && rxQp->cb.m_enabled) {
			// Account against the original data sender for the matched tuple direction.
			SenderState &senderState = GetOrCreateSenderState(senderAddr, rxQp);
			senderState.m_enabled = true;
			uint64_t pendingBefore = senderState.m_pendingDemandDataBytes;
			uint64_t outDataBefore = senderState.m_outstandingDataBytes;
			uint64_t outWireBefore = senderState.m_OutstandingBytes;
			uint64_t srpbBefore = senderState.m_srpbBytes;
			uint64_t globalOutBefore = m_creditbouncer_globalOutstanding_bytes;
			uint64_t msgPendingBefore = rxQp->cb.m_pendingDemandBytes;

				uint64_t bouncedWire = creditBytes;
				uint16_t bouncedCreditPad = cbTag.GetCreditPad();
				uint64_t bouncedMinWireNoPad = CB_DATA_HEADERS_ON_WIRE + CB_LINK_OVERHEAD_ON_WIRE + bouncedCreditPad;
				uint64_t bouncedData = 0;
				if (bouncedWire > bouncedMinWireNoPad)
					bouncedData = bouncedWire - bouncedMinWireNoPad;
				uint64_t effectiveWire = std::min<uint64_t>(bouncedWire, senderState.m_OutstandingBytes);
				uint64_t effectiveData = std::min<uint64_t>(bouncedData, senderState.m_outstandingDataBytes);

			// Roll back outstanding grant accounting conservatively so duplicate
			// bounced packets cannot create negative/duplicate credit.
			senderState.m_OutstandingBytes -= effectiveWire;
			senderState.m_outstandingDataBytes -= effectiveData;
			senderState.m_pendingDemandDataBytes += effectiveData;
			if (rxQp->cb.m_pendingDemandBytes + effectiveData < rxQp->cb.m_pendingDemandBytes)
				rxQp->cb.m_pendingDemandBytes = UINT64_MAX;
			else
				rxQp->cb.m_pendingDemandBytes += effectiveData;
			if (senderState.m_srpbBytes + effectiveWire > senderState.m_maxSrpbBytes)
				senderState.m_srpbBytes = senderState.m_maxSrpbBytes;
			else
				senderState.m_srpbBytes += effectiveWire;
			if (m_creditbouncer_globalOutstanding_bytes >= effectiveWire)
				m_creditbouncer_globalOutstanding_bytes -= effectiveWire;
			else
				m_creditbouncer_globalOutstanding_bytes = 0;

			// Phase1 DPDQ mapping is carried by the canonical CreditBouncer fields:
			// b         -> (B - m_creditbouncer_globalOutstanding_bytes)
			// sb_i      -> senderState.m_OutstandingBytes
			// rem_i     -> senderState.m_pendingDemandDataBytes
			// netBkt_i  -> senderState.m_netBucketLimitSrpbBytes

			if (m_creditbouncer_debugLog && m_creditbouncer_debugLogCount < m_creditbouncer_debugMaxLogs){
				std::cerr << "CBDBG rx_bounced_credit node=" << m_node->GetId()
					<< " flow=" << rxQp->m_flow_id
					<< " sender=" << Ipv4Address(senderState.m_senderAddr)
					<< " bounced_wire=" << bouncedWire
					<< " bounced_data=" << bouncedData
					<< " effective_wire=" << effectiveWire
					<< " effective_data=" << effectiveData
					<< " pending_before=" << pendingBefore
					<< " pending_after=" << senderState.m_pendingDemandDataBytes
					<< " out_data_before=" << outDataBefore
					<< " out_data_after=" << senderState.m_outstandingDataBytes
					<< " out_wire_before=" << outWireBefore
					<< " out_wire_after=" << senderState.m_OutstandingBytes
					<< " srpb_before=" << srpbBefore
					<< " srpb_after=" << senderState.m_srpbBytes
					<< " msg_pending_before=" << msgPendingBefore
					<< " msg_pending_after=" << rxQp->cb.m_pendingDemandBytes
					<< " gOut_before=" << globalOutBefore
					<< " gOut_after=" << m_creditbouncer_globalOutstanding_bytes
					<< "\n";
				m_creditbouncer_debugLogCount++;
			}

			if (!rxQp->cb.m_completionNotified && rxQp->cb.m_expectedChunks > 0)
				ScheduleCreditBouncerResend(rxQp);

			if (senderState.m_pendingDemandDataBytes > 0)
				ScheduleCreditGrantTick();
		} else if (m_creditbouncer_debugLog && m_creditbouncer_debugLogCount < m_creditbouncer_debugMaxLogs) {
			std::cerr << "CBDBG rx_bounced_credit_miss node=" << m_node->GetId()
				<< " sip=" << Ipv4Address(ch.sip)
				<< " dip=" << Ipv4Address(ch.dip)
				<< " sport=" << ch.ack.sport
				<< " dport=" << ch.ack.dport
				<< " pg=" << ch.ack.pg
				<< "\n";
			m_creditbouncer_debugLogCount++;
		}
	}

	(void)p;
	return 0;
}

int RdmaHw::ReceiverCheckSeq(uint32_t seq, Ptr<RdmaRxQueuePair> q, uint32_t size){
	uint32_t expected = q->ReceiverNextExpectedSeq;
	if (seq == expected || (seq < expected && seq + size >= expected)){
		if (m_irn) {
			if (q->m_milestone_rx < seq + size)
				q->m_milestone_rx = seq + size;
			q->ReceiverNextExpectedSeq += size - (expected - seq); 
			{
				uint32_t sack_seq, sack_len;
				if (q->m_irn_sack_.peekFrontBlock(&sack_seq, &sack_len)) {
					if (sack_seq <= q->ReceiverNextExpectedSeq)
						q->ReceiverNextExpectedSeq += (sack_len - (q->ReceiverNextExpectedSeq-sack_seq));
				}
			}
			size_t progress = q->m_irn_sack_.discardUpTo(q->ReceiverNextExpectedSeq);
			if (q->m_irn_sack_.IsEmpty()) {
				return 6; // This generates NACK, but actually functions as an ACK (indicates all packet has been received)
			} else {
				//should we put nack timer here
				return 2; // Still in loss recovery mode of IRN
			}
			return 0; // should not reach here
		}

		q->ReceiverNextExpectedSeq += size - (expected - seq);
		if (q->ReceiverNextExpectedSeq >= q->m_milestone_rx){
			q->m_milestone_rx += m_ack_interval;
			return 1; //Generate ACK
		}else if (q->ReceiverNextExpectedSeq % m_chunk == 0){
			return 1;
		}else {
			return 5;
		}
	} else if (seq > expected) {
		// Generate NACK
		if (m_irn) {
			if (q->m_milestone_rx < seq + size)
				q->m_milestone_rx = seq + size;
			
			//if seq is already nacked, check for nacktimer
			if (q->m_irn_sack_.blockExists(seq, size) && Simulator::Now() < q->m_nackTimer) {
				return 4; // don't need to send nack yet
			}
			q->m_nackTimer = Simulator::Now() + MicroSeconds(m_nack_interval);
			q->m_irn_sack_.sack(seq, size);
			NS_ASSERT(q->m_irn_sack_.discardUpTo(expected) == 0); // SACK blocks must be larger than expected
			return 2;
		}
		if (Simulator::Now() >= q->m_nackTimer || q->m_lastNACK != expected){
			q->m_nackTimer = Simulator::Now() + MicroSeconds(m_nack_interval);
			q->m_lastNACK = expected;
			if (m_backto0){
				q->ReceiverNextExpectedSeq = q->ReceiverNextExpectedSeq / m_chunk*m_chunk;
			}
			return 2;
		}else
			return 4;
	}else {
		// Duplicate. 
		if (m_irn) {
			// if (q->ReceiverNextExpectedSeq - 1 == q->m_milestone_rx) {
			// 	return 6; // This generates NACK, but actually functions as an ACK (indicates all packet has been received)
			// }
			if (q->m_irn_sack_.IsEmpty()) {
				return 6; // This generates NACK, but actually functions as an ACK (indicates all packet has been received)
			} else {
				//should we put nack timer here
				return 2; // Still in loss recovery mode of IRN
			}
		}
		// Duplicate. 
		return 1; // According to IB Spec C9-110
		/**
		 * IB Spec C9-110
		 * A responder shall respond to all duplicate requests in PSN order;
		 * i.e. the request with the (logically) earliest PSN shall be executed first. If,
		 * while responding to a new or duplicate request, a duplicate request is received
		 * with a logically earlier PSN, the responder shall cease responding
		 * to the original request and shall begin responding to the duplicate request
		 * with the logically earlier PSN.
		 */
	}
}
void RdmaHw::AddHeader (Ptr<Packet> p, uint16_t protocolNumber){
	PppHeader ppp;
	ppp.SetProtocol (EtherToPpp (protocolNumber));
	p->AddHeader (ppp);
}
uint16_t RdmaHw::EtherToPpp (uint16_t proto){
	switch(proto){
		case 0x0800: return 0x0021;   //IPv4
		case 0x86DD: return 0x0057;   //IPv6
		default: NS_ASSERT_MSG (false, "PPP Protocol number not defined!");
	}
	return 0;
}

void RdmaHw::RecoverQueue(Ptr<RdmaQueuePair> qp){
	qp->snd_nxt = qp->snd_una;
	qp->tlt.m_first_retx = true;
}

void RdmaHw::CompleteCreditBouncerSenderQp(uint16_t sport, uint16_t pg, int32_t expectedFlowId){
	Ptr<RdmaQueuePair> qp = GetQp(sport, pg);
	if (qp == 0 || !qp->cb.m_enabled) {
		std::cerr << "WARNING: CB completion sender qp lookup miss node=" << m_node->GetId()
			<< " sport=" << sport
			<< " pg=" << pg
			<< " expected_flow=" << expectedFlowId
			<< "\n";
		return;
	}

	if (expectedFlowId >= 0 && qp->m_flow_id >= 0 && qp->m_flow_id != expectedFlowId) {
		std::cerr << "WARNING: CB completion flow mismatch node=" << m_node->GetId()
			<< " sport=" << sport
			<< " pg=" << pg
			<< " expected_flow=" << expectedFlowId
			<< " actual_flow=" << qp->m_flow_id
			<< "\n";
		return;
	}

	bool alreadyFinished = qp->IsFinishedConst();
	qp->snd_una = qp->m_size;
	if (qp->snd_nxt < qp->m_size)
		qp->snd_nxt = qp->m_size;
	if (qp->m_retransmit.IsRunning())
		qp->m_retransmit.Cancel();

	if (m_creditbouncer_debugLog && m_creditbouncer_debugLogCount < m_creditbouncer_debugMaxLogs){
		std::cerr << "CBDBG sender_complete node=" << m_node->GetId()
			<< " flow=" << qp->m_flow_id
			<< " sport=" << sport
			<< " pg=" << pg
			<< " finished_before=" << (alreadyFinished ? 1 : 0)
			<< "\n";
		m_creditbouncer_debugLogCount++;
	}

	CleanupCreditBouncerReceiverState(qp);
	if (!alreadyFinished)
		QpComplete(qp);
}

void RdmaHw::QpComplete(Ptr<RdmaQueuePair> qp){
	NS_ASSERT(!m_qpCompleteCallback.IsNull());
	if (m_cc_mode == 1){
		Simulator::Cancel(qp->mlx.m_eventUpdateAlpha);
		Simulator::Cancel(qp->mlx.m_eventDecreaseRate);
		Simulator::Cancel(qp->mlx.m_rpTimer);
	}
	if (qp->m_retransmit.IsRunning())
		qp->m_retransmit.Cancel();
	CleanupCreditBouncerReceiverState(qp);
	m_qpCompleteCallback(qp);
}

void RdmaHw::SetLinkDown(Ptr<QbbNetDevice> dev){
	printf("RdmaHw: node:%u a link down\n", m_node->GetId());
}

void RdmaHw::AddTableEntry(Ipv4Address &dstAddr, uint32_t intf_idx){
	uint32_t dip = dstAddr.Get();
	auto &ports = m_rtTable[dip];
	if (std::find(ports.begin(), ports.end(), static_cast<int>(intf_idx)) == ports.end())
		ports.push_back(intf_idx);
	std::sort(ports.begin(), ports.end());
}

void RdmaHw::ClearTable(){
	m_rtTable.clear();
}

void RdmaHw::RedistributeQp(){
	// clear old qpGrp
	for (uint32_t i = 0; i < m_nic.size(); i++){
		if (m_nic[i].dev == NULL)
			continue;
		m_nic[i].qpGrp->Clear();
	}

	// redistribute qp
	for (auto &it : m_qpMap){
		Ptr<RdmaQueuePair> qp = it.second;
		uint32_t nic_idx = GetNicIdxOfQp(qp);
		m_nic[nic_idx].qpGrp->AddQp(qp);
		// Notify Nic
		m_nic[nic_idx].dev->ReassignedQp(qp);
	}
}

Ptr<Packet> RdmaHw::GetNxtPacket(Ptr<RdmaQueuePair> qp){
	uint64_t bytesLeft = qp->GetBytesLeft();
	uint32_t payload_size = CbNextChunkPayloadBytes(qp->snd_nxt, qp->m_size, m_mtu);
	if (payload_size > bytesLeft)
		payload_size = static_cast<uint32_t>(std::min<uint64_t>(bytesLeft, UINT32_MAX));
	bool cbDataCsnMarked = false;
	uint32_t cbCreditReqBytes = 0;
	uint32_t cbUnsolCreditBytes = 0;
	uint32_t cbUnsolCreditDataBytes = 0;
	bool cbUsedUnsolCredit = false;
	uint16_t cbCreditPad = 0;
	uint64_t cbToSendWire = 0;
	CreditBouncerTag::DataClass cbDataClass = CreditBouncerTag::CB_DATA_CLASS_NONE;
	bool cbPriorityFlow = false;
	ReceiverState *receiverStatePtr = NULL;
	 
	if (m_creditbouncer && qp->cb.m_enabled) {
		cbPriorityFlow = qp->cb.m_priorityFlowPending;
		qp->cb.m_priorityFlowPending = false;
		receiverStatePtr = &GetOrCreateReceiverState(qp->dip.Get(), qp);
		receiverStatePtr->m_enabled = true;
			if (!qp->cb.m_usnsolCheckDone) {
				uint64_t msgBytes = qp->m_size;
				uint64_t msgOnWireBytes = CbAddHeaderOverhead(msgBytes, m_mtu);
			uint64_t unsolicitedThreshOnWire = receiverStatePtr->m_unsolicitedThreshBytes;
			uint64_t maxSrpbOnWire = m_creditbouncer_maxSrpb_bytes;

			qp->cb.m_selfAllocCreditBytes = 0;
			qp->cb.m_selfAllocCreditDataBytes = 0;
			qp->cb.m_remainingSelfAllocCreditDataBytes = 0;
			qp->cb.m_isScheduled = true;
			qp->cb.m_hasScheduledPart = true;

				if (msgOnWireBytes <= unsolicitedThreshOnWire) {
					uint64_t toGrantWire = msgOnWireBytes;
					uint64_t toGrantData = msgBytes;
					if (msgOnWireBytes > maxSrpbOnWire) {
						toGrantData = CbRemoveHeaderOverhead(maxSrpbOnWire, m_mtu);
						toGrantWire = CbAddHeaderOverhead(toGrantData, m_mtu, true);
					}

					// Bound self-allocation so per-pair credit stays near 1xBDP.
					uint64_t availOnWire = receiverStatePtr->m_availCreditBytes;
					uint64_t backlogSpaceOnWire = maxSrpbOnWire > availOnWire ? (maxSrpbOnWire - availOnWire) : 0;
					uint64_t minGrantOnWire = CbAddHeaderOverhead(
						std::min<uint64_t>(msgBytes, m_mtu), m_mtu, true);
					backlogSpaceOnWire = std::max(backlogSpaceOnWire, minGrantOnWire);
					if (toGrantWire > backlogSpaceOnWire) {
						toGrantData = CbRemoveHeaderOverhead(backlogSpaceOnWire, m_mtu);
						toGrantWire = CbAddHeaderOverhead(toGrantData, m_mtu, true);
					}

					if (toGrantData > msgBytes)
						toGrantData = msgBytes;
					toGrantData = CbAlignChunkPrefixBytes(toGrantData, msgBytes, m_mtu);
					toGrantWire = CbAddHeaderOverhead(toGrantData, m_mtu, true);
					if (toGrantWire > 0 && toGrantData > 0) {
						qp->cb.m_selfAllocCreditBytes = toGrantWire;
						qp->cb.m_selfAllocCreditDataBytes = toGrantData;
					qp->cb.m_remainingSelfAllocCreditDataBytes = static_cast<int64_t>(toGrantData);
					qp->cb.m_isScheduled = false;
					qp->cb.m_hasScheduledPart = msgBytes > toGrantData;
					receiverStatePtr->m_availCreditBytes += toGrantWire;
					receiverStatePtr->m_availCreditDataBytes += toGrantData;
				}
			}

			if (m_creditbouncer_debugLog && m_creditbouncer_debugLogCount < m_creditbouncer_debugMaxLogs){
				const char* initClass = qp->cb.m_isScheduled
					? "scheduled"
					: (qp->cb.m_hasScheduledPart ? "unscheduled" : "unsolicited");
				std::cerr << "CBDBG classify node=" << m_node->GetId()
					<< " flow=" << qp->m_flow_id
					<< " class=" << initClass
					<< " msg_data=" << qp->m_size
					<< " msg_wire=" << msgOnWireBytes
					<< " unsol_thresh_wire=" << unsolicitedThreshOnWire
					<< " self_alloc_wire=" << qp->cb.m_selfAllocCreditBytes
					<< " self_alloc_data=" << qp->cb.m_selfAllocCreditDataBytes
					<< " has_sched_part=" << (qp->cb.m_hasScheduledPart ? 1 : 0)
					<< " avail_wire_after=" << receiverStatePtr->m_availCreditBytes
					<< " avail_data_after=" << receiverStatePtr->m_availCreditDataBytes
					<< "\n";
				m_creditbouncer_debugLogCount++;
			}

			qp->cb.m_unsolicitedBytesBudget = qp->cb.m_selfAllocCreditDataBytes;
			qp->cb.m_unsolicitedBytesSent = 0;
			qp->cb.m_pendingScheduledBytes = qp->cb.m_hasScheduledPart && qp->m_size > qp->cb.m_selfAllocCreditDataBytes
				? (qp->m_size - qp->cb.m_selfAllocCreditDataBytes)
				: 0;
			qp->cb.m_sentAnnouncement = false;
			qp->cb.m_schedRequestAnnounced = false;
			qp->cb.m_standaloneGrantReqSent = false;
			qp->cb.m_usnsolCheckDone = true;
			qp->cb.m_classificationReady = true;
		}

		if (qp->cb.m_isScheduled && !qp->cb.m_sentAnnouncement) {
			uint32_t reqBytes = qp->m_size > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(qp->m_size);
			if (reqBytes == 0)
				return 0;
			qp->cb.m_standaloneGrantReqSent = true;
			qp->cb.m_sentAnnouncement = true;
			qp->cb.m_schedRequestAnnounced = true;
			qp->cb.m_pendingScheduledBytes = reqBytes;
			qp->cb.m_statCreditReqPkts++;
			if (m_creditbouncer_debugLog && m_creditbouncer_debugLogCount < m_creditbouncer_debugMaxLogs) {
				uint32_t qpHash = qp->GetHash();
				uint32_t symHash = GetCreditBouncerSymmetricHash(
					qp->sip.Get(), qp->dip.Get(), qp->sport, qp->dport, qp->m_pg);
				uint32_t nic_idx = GetNicIdxOfQp(qp);
				std::cerr << "CBROUTE host node=" << m_node->GetId()
					<< " type=" << CbMessageTypeToString(CreditBouncerTag::CB_MSG_GRANT_REQ)
					<< " mode=" << ((m_creditbouncerSymmetricRouting && qp->cb.m_enabled) ? "symmetric" : "default")
					<< " flow=" << qp->m_flow_id
					<< " sip=" << qp->sip
					<< " dip=" << qp->dip
					<< " sport=" << qp->sport
					<< " dport=" << qp->dport
					<< " pg=" << qp->m_pg
					<< " hash=" << ((m_creditbouncerSymmetricRouting && qp->cb.m_enabled) ? symHash : qpHash)
					<< " legacy_hash=" << qpHash
					<< " symmetric_hash=" << symHash
					<< " nic=" << nic_idx
					<< "\n";
				m_creditbouncer_debugLogCount++;
			}

				return BuildCreditBouncerCtrlPacket(
					qp->sip, qp->dip,
					qp->sport, qp->dport,
					qp->m_pg,
					0,
					CreditBouncerTag::CB_MSG_GRANT_REQ,
					reqBytes,
					0,
				qp->m_ipid++);
		}

			uint64_t availCreditWire = receiverStatePtr->m_availCreditBytes;
			if (availCreditWire == 0)
				return 0;
			if (payload_size == 0)
				return 0;
			uint64_t toSendWire = CbAddHeaderOverhead(payload_size, m_mtu, true);
		if (toSendWire == 0 || toSendWire > receiverStatePtr->m_availCreditBytes)
			return 0;
		cbToSendWire = toSendWire;

		if (m_creditbouncerCcEnable && m_creditbouncerCsnEnable) {
			uint64_t hostCredit = GetHostSchedCreditAvailableBytes();
			cbDataCsnMarked = hostCredit >= receiverStatePtr->m_senderBacklogThresholdBytes;
		}

		if (!qp->cb.m_isScheduled) {
			cbDataClass = qp->cb.m_hasScheduledPart
				? CreditBouncerTag::CB_DATA_CLASS_UNSCHEDULED
				: CreditBouncerTag::CB_DATA_CLASS_UNSOLICITED;
			if (qp->cb.m_hasScheduledPart)
				qp->cb.m_statUnscheduledDataPkts++;
			else
				qp->cb.m_statUnsolicitedDataPkts++;

			cbCreditReqBytes = qp->m_size > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(qp->m_size);
			cbUnsolCreditBytes = qp->cb.m_selfAllocCreditBytes > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(qp->cb.m_selfAllocCreditBytes);
			cbUnsolCreditDataBytes = qp->cb.m_selfAllocCreditDataBytes > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(qp->cb.m_selfAllocCreditDataBytes);
			qp->cb.m_remainingSelfAllocCreditDataBytes -= static_cast<int64_t>(payload_size);
			cbUsedUnsolCredit = qp->cb.m_remainingSelfAllocCreditDataBytes >= 0;
			if (!qp->cb.m_sentAnnouncement) {
				qp->cb.m_sentAnnouncement = true;
				qp->cb.m_schedRequestAnnounced = true;
				qp->cb.m_statCreditReqPkts++;
			}
			uint64_t remainingSelf = qp->cb.m_remainingSelfAllocCreditDataBytes > 0
				? static_cast<uint64_t>(qp->cb.m_remainingSelfAllocCreditDataBytes)
				: 0;
			if (qp->cb.m_selfAllocCreditDataBytes >= remainingSelf)
				qp->cb.m_unsolicitedBytesSent = qp->cb.m_selfAllocCreditDataBytes - remainingSelf;
			else
				qp->cb.m_unsolicitedBytesSent = 0;
		} else {
			cbDataClass = CreditBouncerTag::CB_DATA_CLASS_SCHEDULED;
			qp->cb.m_statScheduledDataPkts++;
			if (cbDataCsnMarked)
				qp->cb.m_statCsnMarkedPkts++;
		}

		receiverStatePtr->m_availCreditBytes -= toSendWire;
		uint64_t availWireAfterSend = receiverStatePtr->m_availCreditBytes;
		uint64_t availDataBeforeSend = receiverStatePtr->m_availCreditDataBytes;
		if (receiverStatePtr->m_availCreditDataBytes >= payload_size)
			receiverStatePtr->m_availCreditDataBytes -= payload_size;
		else
			receiverStatePtr->m_availCreditDataBytes = 0;
		if (m_creditbouncer_debugLog && m_creditbouncer_debugLogCount < m_creditbouncer_debugMaxLogs){
			std::cerr << "CBDBG tx_data node=" << m_node->GetId()
				<< " flow=" << qp->m_flow_id
				<< " class=" << CbDataClassToString(cbDataClass)
				<< " payload=" << payload_size
				<< " send_wire=" << cbToSendWire
				<< " credit_req=" << cbCreditReqBytes
				<< " unsol_wire=" << cbUnsolCreditBytes
				<< " unsol_data=" << cbUnsolCreditDataBytes
				<< " used_unsol=" << (cbUsedUnsolCredit ? 1 : 0)
				<< " avail_wire_before=" << availCreditWire
				<< " avail_wire_after=" << availWireAfterSend
				<< " avail_data_before=" << availDataBeforeSend
				<< " avail_data_after=" << receiverStatePtr->m_availCreditDataBytes
				<< " bytes_left_after=" << (qp->GetBytesLeft() > payload_size ? (qp->GetBytesLeft() - payload_size) : 0)
				<< "\n";
			m_creditbouncer_debugLogCount++;
		}
	}

	uint32_t seq = (uint32_t) qp->snd_nxt;
	TltTag tlt;
	bool proceed_snd_nxt = true;
	if (qp->tlt.m_enabled) {
		if (IsWindowBasedCC()) {
			bool cond_window = !qp->IsWinBound() && (!qp->irn.m_enabled || qp->CanIrnTransmit(m_mtu));
			if ((!cond_window || !payload_size)&& qp->tlt.m_sendState == TLT_STATE_IMPORTANT && !qp->IsFinished()) {
				// TLT force transmission required
				qp->tlt.m_sendUnit = m_mtu; // Reset to MTU(MSS)
				bool tlt_success = forceSendTLT(qp, nullptr);
			}

			if (qp->tlt.m_forcetx_queue.size() > 0) {
				auto pair = qp->tlt.m_forcetx_queue.front();
				seq = pair.first;
				payload_size = pair.second;
				qp->tlt.m_forcetx_queue.pop_front();
				proceed_snd_nxt = false;
			} else if (GetCcType() == CC_TYPE_DYNAMIC_WINDOW) {
				// TODO: check if we need to intercept unimportant packet and convert to force transmission here
				// condition: if (1) CC is in recovery mode (2) not performing force retransmission (3) Pending Important (4) CC is trying to transmit seq larger than highestImportantAck
				// do we need this? this is kind of an hack for TCP. let's disable for now
				#if 0
				bool cond = false;
				if (cond) {
					int sz;
					if (forceSendTLT(qp, &sz))
					{
						NS_ASSERT(qp->tlt.m_forcetx_queue.size() > 0);
						auto pair = qp->tlt.m_forcetx_queue.front();
						seq = pair.first;
						payload_size = pair.second;
						qp->tlt.m_forcetx_queue.pop_front();
						TLT_DEBUG_PRINT("Flow " << qp->m_flow_id << " : Success in intercepting force Retransmission TLT here!! sz=" << sz);
					}
				}
				#endif
			}

			
			tlt.SetControlType(TltTag::PACKET_PAYLOAD);
			if (qp->tlt.m_sendState == TLT_STATE_IMPORTANT) {
				tlt.SetType(TltTag::PACKET_IMPORTANT);
				qp->tlt.m_sendState = TLT_STATE_IDLE;
			} else if (qp->tlt.m_sendState == TLT_STATE_SCHEDULED) {
				tlt.SetType(TltTag::PACKET_IMPORTANT_FORCE);
				qp->tlt.m_sendState = TLT_STATE_IDLE;
			} else {
				tlt.SetType(TltTag::PACKET_NOT_IMPORTANT);
				qp->tlt.m_tlt_unimportant_pkts.socketId = qp->m_flow_id;
				qp->tlt.m_tlt_unimportant_pkts.push(SequenceNumber32(seq), payload_size);  // linked list implementation of blocks
   
				qp->tlt.m_tlt_unimportant_pkts_prev_round->socketId = qp->m_flow_id;
				qp->tlt.m_tlt_unimportant_pkts_current_round->socketId = qp->m_flow_id;
				qp->tlt.m_tlt_unimportant_pkts_current_round->push(SequenceNumber32(seq), payload_size);
			}
			
			#if TLT_DEBUG_ENABLE
			char tbuf[128] = {0, };
			sprintf(tbuf, "(len %u)", payload_size);
			tlt_debug_send_print(qp, seq, tlt, std::string(tbuf));
			#endif
		} else {
			// rate-based CC will be handled below
		}
		
		qp->tlt.m_sent_pkt_count++;
	}

	qp->stat.txTotalPkts += 1;
	qp->stat.txTotalBytes += payload_size;
	if (qp->cb.m_enabled && m_creditbouncer_debugLog && m_creditbouncer_debugLogCount < m_creditbouncer_debugMaxLogs && seq == 0) {
		uint32_t qpHash = qp->GetHash();
		uint32_t symHash = GetCreditBouncerSymmetricHash(
			qp->sip.Get(), qp->dip.Get(), qp->sport, qp->dport, qp->m_pg);
		uint32_t nic_idx = GetNicIdxOfQp(qp);
		std::cerr << "CBROUTE host node=" << m_node->GetId()
			<< " type=" << CbMessageTypeToString(CreditBouncerTag::CB_MSG_REQUEST)
			<< " mode=" << ((m_creditbouncerSymmetricRouting && qp->cb.m_enabled) ? "symmetric" : "default")
			<< " flow=" << qp->m_flow_id
			<< " class=" << CbDataClassToString(cbDataClass)
			<< " sip=" << qp->sip
			<< " dip=" << qp->dip
			<< " sport=" << qp->sport
			<< " dport=" << qp->dport
			<< " pg=" << qp->m_pg
			<< " seq=" << seq
			<< " hash=" << ((m_creditbouncerSymmetricRouting && qp->cb.m_enabled) ? symHash : qpHash)
			<< " legacy_hash=" << qpHash
			<< " symmetric_hash=" << symHash
			<< " nic=" << nic_idx
			<< "\n";
		m_creditbouncer_debugLogCount++;
	}

	Ptr<Packet> p = Create<Packet> (payload_size);
	// add SeqTsHeader
	SeqTsHeader seqTs;
	seqTs.SetSeq (seq);
	seqTs.SetPG (qp->m_pg);
	p->AddHeader (seqTs);
	// add udp header
	UdpHeader udpHeader;
	udpHeader.SetDestinationPort (qp->dport);
	udpHeader.SetSourcePort (qp->sport);
	p->AddHeader (udpHeader);
	// add ipv4 header
	Ipv4Header ipHeader;
	ipHeader.SetSource (qp->sip);
	ipHeader.SetDestination (qp->dip);
	ipHeader.SetProtocol (0x11);
	ipHeader.SetPayloadSize (p->GetSize());
	ipHeader.SetTtl (64);
	ipHeader.SetTos (0);
	ipHeader.SetIdentification (qp->m_ipid);
	p->AddHeader(ipHeader);
	// add ppp header
	PppHeader ppp;
	ppp.SetProtocol (0x0021); // EtherToPpp(0x800), see point-to-point-net-device.cc
	p->AddHeader (ppp);

	// attach Stat Tag
	uint8_t packet_pos = UINT8_MAX;
	{
		FlowIDNUMTag fint;
		if (!p->PeekPacketTag(fint)) {
			fint.SetId(qp->m_flow_id);
			fint.SetFlowSize(qp->m_size);
			p->AddPacketTag(fint);
		}
		FlowStatTag fst;
		uint64_t size = qp->m_size;
		if (!p->PeekPacketTag(fst))
		{
			if (size < m_mtu && qp->snd_nxt+payload_size >= qp->m_size) {
				fst.SetType(FlowStatTag::FLOW_START_AND_END);
			} else if (qp->snd_nxt+payload_size >= qp->m_size) {
				fst.SetType(FlowStatTag::FLOW_END);
			} else if (qp->snd_nxt == 0) {
				fst.SetType(FlowStatTag::FLOW_START);
			} else {
				fst.SetType(FlowStatTag::FLOW_NOTEND);
			}
			packet_pos = fst.GetType();
			fst.setInitiatedTime(Simulator::Now().GetSeconds());
			p->AddPacketTag(fst);
		}
	}

	if (qp->tlt.m_enabled) {
		if (!IsWindowBasedCC()) {
			// Mark packet every predefined period
			if (packet_pos == FlowStatTag::FLOW_START_AND_END || packet_pos == FlowStatTag::FLOW_END) {
				tlt.SetType(TltTag::PACKET_IMPORTANT);
				tlt.SetControlType(TltTag::PACKET_PAYLOAD_EOF);
				qp->tlt.m_last_marked_sent_pkt_count = qp->tlt.m_sent_pkt_count;
			} else if ((qp->tlt.m_sent_pkt_count-qp->tlt.m_last_marked_sent_pkt_count) % m_tlt_important_marking_interval == 0) {
				tlt.SetType(TltTag::PACKET_IMPORTANT);
				tlt.SetControlType(TltTag::PACKET_PAYLOAD_PERIODIC);
				qp->tlt.m_last_marked_sent_pkt_count = qp->tlt.m_sent_pkt_count;
			} else if (qp->tlt.m_first_retx) {
				// mark the first packet of every retransmission
				tlt.SetType(TltTag::PACKET_IMPORTANT);
				tlt.SetControlType(TltTag::PACKET_PAYLOAD_RETX);
				qp->tlt.m_last_marked_sent_pkt_count = qp->tlt.m_sent_pkt_count;
			} else {
				tlt.SetType(TltTag::PACKET_NOT_IMPORTANT);
				tlt.SetControlType(TltTag::PACKET_PAYLOAD);
			}
			#if TLT_DEBUG_ENABLE
			char tbuf[128] = {0,};
			sprintf(tbuf, "(len %u)", payload_size);
			tlt_debug_send_print(qp, seq, tlt, std::string(tbuf));
			#endif
		}
		qp->tlt.m_first_retx = false;
		p->AddPacketTag(tlt);
	}

	if (qp->irn.m_enabled) {
		if (qp->irn.m_max_seq < seq)
			qp->irn.m_max_seq = seq;
	}

	if (m_creditbouncer && qp->cb.m_enabled) {
		CreditBouncerTag dataTag(CreditBouncerTag::CB_MSG_REQUEST,
			cbCreditReqBytes,
			0,
			cbDataCsnMarked,
			cbPriorityFlow,
			cbDataClass,
			cbUnsolCreditBytes,
			cbUnsolCreditDataBytes,
			cbUsedUnsolCredit,
			cbCreditPad);
		p->AddPacketTag(dataTag);
	}

	// update state
	if(proceed_snd_nxt)
		qp->snd_nxt += payload_size;
	qp->m_ipid++;

	// return
	return p;
}

void RdmaHw::PktSent(Ptr<RdmaQueuePair> qp, Ptr<Packet> pkt, Time interframeGap){
	qp->lastPktSize = pkt->GetSize();
	UpdateNextAvail(qp, interframeGap, pkt->GetSize());

	if(pkt) {
		CustomHeader ch(CustomHeader::L2_Header | CustomHeader::L3_Header | CustomHeader::L4_Header);
		pkt->PeekHeader(ch);
		if(ch.l3Prot == 0x11) { // UDP
			if (m_cc_mode == 1 && !qp->mlx.m_first_cnp && qp->mlx.m_rpByteResetBytes > 0) {
				qp->mlx.m_rpByteCounterBytes += pkt->GetSize();
				while (qp->mlx.m_rpByteCounterBytes >= qp->mlx.m_rpByteResetBytes) {
					qp->mlx.m_rpByteCounterBytes -= qp->mlx.m_rpByteResetBytes;
					RateIncEventMlx(qp, false);
				}
			}
			// CB completion is receiver-driven, so per-packet ACK timeout stays disabled for CB flows.
			if (!(m_creditbouncer && qp->cb.m_enabled)) {
				if (qp->m_retransmit.IsRunning())
					qp->m_retransmit.Cancel();
				qp->m_retransmit = Simulator::Schedule(qp->GetRto(m_mtu), &RdmaHw::HandleTimeout, this, qp, qp->GetRto(m_mtu));
			}
		} else if (ch.l3Prot == 0xFC || ch.l3Prot == 0xFD|| ch.l3Prot == 0xFF) { //ACK, NACK, CNP

		}
		else if (ch.l3Prot == 0xFE)
		{ // PFC

		}
		if (m_node->GetNodeType() == 0){
			TltTag tlt;
			if (qp->tlt.m_enabled && pkt->PeekPacketTag(tlt)) {
				if (tlt.GetType() == TltTag::PACKET_IMPORTANT) {
					stat_tx.txImpBytesNIC += pkt->GetSize();
					if (tlt.GetControlType() == TltTag::PACKET_PAYLOAD)
						stat_tx.txImpBytesNIC_PL += pkt->GetSize();
					if (tlt.GetControlType() == TltTag::PACKET_ACK)
						stat_tx.txImpBytesNIC_ACK += pkt->GetSize();
					if (tlt.GetControlType() == TltTag::PACKET_NACK)
						stat_tx.txImpBytesNIC_NACK += pkt->GetSize();
					if (tlt.GetControlType() == TltTag::PACKET_CNP)
						stat_tx.txImpBytesNIC_CNP += pkt->GetSize();
					if (tlt.GetControlType() == TltTag::PACKET_PAYLOAD_EOF)
						stat_tx.txImpBytesNIC_PLE += pkt->GetSize();
					if (tlt.GetControlType() == TltTag::PACKET_PAYLOAD_RETX)
						stat_tx.txImpBytesNIC_PLR += pkt->GetSize();
				} else if (tlt.GetType() == TltTag::PACKET_IMPORTANT_ECHO) {
					stat_tx.txImpEBytesNIC += pkt->GetSize();
				} else if (tlt.GetType() == TltTag::PACKET_IMPORTANT_FORCE) {
					stat_tx.txImpFBytesNIC += pkt->GetSize();
				} else if (tlt.GetType() == TltTag::PACKET_IMPORTANT_ECHO_FORCE) {
					stat_tx.txImpEFBytesNIC += pkt->GetSize();
				} else if (tlt.GetType() == TltTag::PACKET_IMPORTANT_CONTROL) {
					stat_tx.txImpCBytesNIC += pkt->GetSize();
				} else if (tlt.GetType() == TltTag::PACKET_NOT_IMPORTANT) {
					stat_tx.txUimpBytesNIC += pkt->GetSize();
				}
			}
		}
	}
}

void RdmaHw::HandleTimeout(Ptr<RdmaQueuePair> qp, Time rto) {
	if (m_creditbouncer && qp->cb.m_enabled)
		return;
	

	// Assume Outstanding Packets are lost
	// std::cerr << "Timeout on qp=" << qp << std::endl;

	if (qp->IsFinished())
	{
		// std::cerr << "Why still scheduled?" << std::endl;
		return;
	}

	uint32_t nic_idx = GetNicIdxOfQp(qp);
	Ptr<QbbNetDevice> dev = m_nic[nic_idx].dev;

	// IRN: disable timeouts when PFC is enabled to prevent spurious retransmissions
	if (qp->irn.m_enabled && dev->IsQbbEnabled())
		return;

	stat_tx.RetxTimeoutCnt++;

	if (qp->tlt.m_enabled) {
		std::cerr << "Warning: TLT Timeout Detected." << std::endl;
	}

	if (qp->tlt.m_enabled && IsWindowBasedCC()) {
		qp->tlt.m_sendState = TLT_STATE_IMPORTANT;
		qp->tlt.m_sent_fin = false;
	}

	if (acc_timeout_count.find(qp->m_flow_id) == acc_timeout_count.end())
		acc_timeout_count[qp->m_flow_id] = 0;
	acc_timeout_count[qp->m_flow_id]++;
	m_traceTimeoutEvent(qp, qp->irn.m_enabled, static_cast<uint64_t>(rto.GetTimeStep()));

	if (qp->irn.m_enabled)
		qp->irn.m_recovery = true;
		
	RecoverQueue(qp);
	dev->TriggerTransmit();
}

void RdmaHw::UpdateNextAvail(Ptr<RdmaQueuePair> qp, Time interframeGap, uint32_t pkt_size){
	Time sendingTime;
	if (m_rateBound)
		sendingTime = interframeGap + Seconds(qp->m_rate.CalculateTxTime(pkt_size));
	else
		sendingTime = interframeGap + Seconds(qp->m_max_rate.CalculateTxTime(pkt_size));
	qp->m_nextAvail = Simulator::Now() + sendingTime;
}

void RdmaHw::ChangeRate(Ptr<RdmaQueuePair> qp, DataRate new_rate){
	#if 1
	Time sendingTime = Seconds(qp->m_rate.CalculateTxTime(qp->lastPktSize));
	Time new_sendintTime = Seconds(new_rate.CalculateTxTime(qp->lastPktSize));
	qp->m_nextAvail = qp->m_nextAvail + new_sendintTime - sendingTime;
	// update nic's next avail event
	uint32_t nic_idx = GetNicIdxOfQp(qp);
	m_nic[nic_idx].dev->UpdateNextAvail(qp->m_nextAvail);
	#endif

	// change to new rate
	qp->m_rate = new_rate;
}

#define PRINT_LOG 0
/******************************
 * Mellanox's version of DCQCN
 *****************************/
void RdmaHw::UpdateAlphaMlx(Ptr<RdmaQueuePair> q){
	#if PRINT_LOG
	//std::cout << Simulator::Now() << " alpha update:" << m_node->GetId() << ' ' << q->mlx.m_alpha << ' ' << (int)q->mlx.m_alpha_cnp_arrived << '\n';
	//printf("%lu alpha update: %08x %08x %u %u %.6lf->", Simulator::Now().GetTimeStep(), q->sip.Get(), q->dip.Get(), q->sport, q->dport, q->mlx.m_alpha);
	#endif
	if (q->mlx.m_alpha_cnp_arrived){
		q->mlx.m_alpha = (1 - m_g)*q->mlx.m_alpha + m_g; 	//binary feedback
	}else {
		q->mlx.m_alpha = (1 - m_g)*q->mlx.m_alpha; 	//binary feedback
	}
	#if PRINT_LOG
	//printf("%.6lf\n", q->mlx.m_alpha);
	#endif
	q->mlx.m_alpha_cnp_arrived = false; // clear the CNP_arrived bit
	ScheduleUpdateAlphaMlx(q);
}
void RdmaHw::ScheduleUpdateAlphaMlx(Ptr<RdmaQueuePair> q){
	q->mlx.m_eventUpdateAlpha = Simulator::Schedule(MicroSeconds(m_alpha_resume_interval), &RdmaHw::UpdateAlphaMlx, this, q);
}

void RdmaHw::cnp_received_mlx(Ptr<RdmaQueuePair> q){
	q->mlx.m_alpha_cnp_arrived = true; // set CNP_arrived bit for alpha update
	q->mlx.m_decrease_cnp_arrived = true; // set CNP_arrived bit for rate decrease
	if (q->mlx.m_first_cnp){
		// init alpha
		q->mlx.m_alpha = 1;
		q->mlx.m_alpha_cnp_arrived = false;
		// schedule alpha update
		ScheduleUpdateAlphaMlx(q);
		// schedule rate decrease
		ScheduleDecreaseRateMlx(q, 1); // add 1 ns to make sure rate decrease is after alpha update
		// set rate on first CNP
		q->mlx.m_targetRate = q->m_rate = m_rateOnFirstCNP * q->m_rate;
		q->mlx.m_first_cnp = false;
	}
}

void RdmaHw::CheckRateDecreaseMlx(Ptr<RdmaQueuePair> q){
	ScheduleDecreaseRateMlx(q, 0);
	if (q->mlx.m_decrease_cnp_arrived){
		#if PRINT_LOG
		printf("%lu rate dec: %08x %08x %u %u (%0.3lf %.3lf)->", Simulator::Now().GetTimeStep(), q->sip.Get(), q->dip.Get(), q->sport, q->dport, q->mlx.m_targetRate.GetBitRate() * 1e-9, q->m_rate.GetBitRate() * 1e-9);
		#endif
		bool clamp = true;
		if (!m_EcnClampTgtRate){
			if (std::max(q->mlx.m_rpTimeStage, q->mlx.m_rpByteStage) == 0)
				clamp = false;
		}
		if (clamp)
			q->mlx.m_targetRate = q->m_rate;
		q->m_rate = std::max(m_minRate, q->m_rate * (1 - q->mlx.m_alpha / 2));
		// reset rate increase related things
		q->mlx.m_rpTimeStage = 0;
		q->mlx.m_rpByteStage = 0;
		q->mlx.m_rpByteCounterBytes = 0;
		q->mlx.m_decrease_cnp_arrived = false;
		Simulator::Cancel(q->mlx.m_rpTimer);
		q->mlx.m_rpTimer = Simulator::Schedule(MicroSeconds(m_rpgTimeReset), &RdmaHw::RateIncEventTimerMlx, this, q);
		#if PRINT_LOG
		printf("(%.3lf %.3lf)\n", q->mlx.m_targetRate.GetBitRate() * 1e-9, q->m_rate.GetBitRate() * 1e-9);
		#endif
	}
}
void RdmaHw::ScheduleDecreaseRateMlx(Ptr<RdmaQueuePair> q, uint32_t delta){
	q->mlx.m_eventDecreaseRate = Simulator::Schedule(MicroSeconds(m_rateDecreaseInterval) + NanoSeconds(delta), &RdmaHw::CheckRateDecreaseMlx, this, q);
}

void RdmaHw::RateIncEventTimerMlx(Ptr<RdmaQueuePair> q){
	q->mlx.m_rpTimer = Simulator::Schedule(MicroSeconds(m_rpgTimeReset), &RdmaHw::RateIncEventTimerMlx, this, q);
	RateIncEventMlx(q, true);
}
void RdmaHw::RateIncEventMlx(Ptr<RdmaQueuePair> q, bool byTimer){
	uint32_t maxStage = std::max(q->mlx.m_rpTimeStage, q->mlx.m_rpByteStage);
	uint32_t minStage = std::min(q->mlx.m_rpTimeStage, q->mlx.m_rpByteStage);
	// check which increase phase using both the timer- and byte-driven stages
	if (maxStage < m_rpgThreshold){
		FastRecoveryMlx(q);
	}else if (maxStage > m_rpgThreshold && minStage < m_rpgThreshold){
		ActiveIncreaseMlx(q);
	}else if (minStage > m_rpgThreshold){
		HyperIncreaseMlx(q);
	}else {
		ActiveIncreaseMlx(q);
	}
	if (byTimer)
		q->mlx.m_rpTimeStage++;
	else
		q->mlx.m_rpByteStage++;
}

void RdmaHw::FastRecoveryMlx(Ptr<RdmaQueuePair> q){
	#if PRINT_LOG
	printf("%lu fast recovery: %08x %08x %u %u (%0.3lf %.3lf)->", Simulator::Now().GetTimeStep(), q->sip.Get(), q->dip.Get(), q->sport, q->dport, q->mlx.m_targetRate.GetBitRate() * 1e-9, q->m_rate.GetBitRate() * 1e-9);
	#endif
	q->m_rate = (q->m_rate / 2) + (q->mlx.m_targetRate / 2);
	#if PRINT_LOG
	printf("(%.3lf %.3lf)\n", q->mlx.m_targetRate.GetBitRate() * 1e-9, q->m_rate.GetBitRate() * 1e-9);
	#endif
}
void RdmaHw::ActiveIncreaseMlx(Ptr<RdmaQueuePair> q){
	#if PRINT_LOG
	printf("%lu active inc: %08x %08x %u %u (%0.3lf %.3lf)->", Simulator::Now().GetTimeStep(), q->sip.Get(), q->dip.Get(), q->sport, q->dport, q->mlx.m_targetRate.GetBitRate() * 1e-9, q->m_rate.GetBitRate() * 1e-9);
	#endif
	// get NIC
	uint32_t nic_idx = GetNicIdxOfQp(q);
	Ptr<QbbNetDevice> dev = m_nic[nic_idx].dev;
	// increate rate
	q->mlx.m_targetRate += m_rai;
	if (q->mlx.m_targetRate > dev->GetDataRate())
		q->mlx.m_targetRate = dev->GetDataRate();
	q->m_rate = (q->m_rate / 2) + (q->mlx.m_targetRate / 2);
	#if PRINT_LOG
	printf("(%.3lf %.3lf)\n", q->mlx.m_targetRate.GetBitRate() * 1e-9, q->m_rate.GetBitRate() * 1e-9);
	#endif
}
void RdmaHw::HyperIncreaseMlx(Ptr<RdmaQueuePair> q){
	#if PRINT_LOG
	printf("%lu hyper inc: %08x %08x %u %u (%0.3lf %.3lf)->", Simulator::Now().GetTimeStep(), q->sip.Get(), q->dip.Get(), q->sport, q->dport, q->mlx.m_targetRate.GetBitRate() * 1e-9, q->m_rate.GetBitRate() * 1e-9);
	#endif
	// get NIC
	uint32_t nic_idx = GetNicIdxOfQp(q);
	Ptr<QbbNetDevice> dev = m_nic[nic_idx].dev;
	// increate rate
	q->mlx.m_targetRate += m_rhai;
	if (q->mlx.m_targetRate > dev->GetDataRate())
		q->mlx.m_targetRate = dev->GetDataRate();
	q->m_rate = (q->m_rate / 2) + (q->mlx.m_targetRate / 2);
	#if PRINT_LOG
	printf("(%.3lf %.3lf)\n", q->mlx.m_targetRate.GetBitRate() * 1e-9, q->m_rate.GetBitRate() * 1e-9);
	#endif
}

/***********************
 * High Precision CC
 ***********************/
void RdmaHw::HandleAckHp(Ptr<RdmaQueuePair> qp, Ptr<Packet> p, CustomHeader &ch){
	uint32_t ack_seq = ch.ack.seq;
	// update rate
	if (ack_seq > qp->hp.m_lastUpdateSeq){ // if full RTT feedback is ready, do full update
		UpdateRateHp(qp, p, ch, false);
	}else{ // do fast react
		FastReactHp(qp, p, ch);
	}
}

void RdmaHw::UpdateRateHp(Ptr<RdmaQueuePair> qp, Ptr<Packet> p, CustomHeader &ch, bool fast_react){
	uint32_t next_seq = qp->snd_nxt;
	bool print = !fast_react || true;
	if (qp->hp.m_lastUpdateSeq == 0){ // first RTT
		qp->hp.m_lastUpdateSeq = next_seq;
		// store INT
		IntHeader &ih = ch.ack.ih;
		NS_ASSERT(ih.nhop <= IntHeader::maxHop);
		for (uint32_t i = 0; i < ih.nhop; i++)
			qp->hp.hop[i] = ih.hop[i];
		#if PRINT_LOG
		if (print){
			printf("%lu %s %08x %08x %u %u [%u,%u,%u]", Simulator::Now().GetTimeStep(), fast_react? "fast" : "update", qp->sip.Get(), qp->dip.Get(), qp->sport, qp->dport, qp->hp.m_lastUpdateSeq, ch.ack.seq, next_seq);
			for (uint32_t i = 0; i < ih.nhop; i++)
				printf(" %u %lu %lu", ih.hop[i].GetQlen(), ih.hop[i].GetBytes(), ih.hop[i].GetTime());
			printf("\n");
		}
		#endif
	}else {
		// check packet INT
		IntHeader &ih = ch.ack.ih;
		if (ih.nhop <= IntHeader::maxHop){
			double max_c = 0;
			bool inStable = false;
			#if PRINT_LOG
			if (print)
				printf("%lu %s %08x %08x %u %u [%u,%u,%u]", Simulator::Now().GetTimeStep(), fast_react? "fast" : "update", qp->sip.Get(), qp->dip.Get(), qp->sport, qp->dport, qp->hp.m_lastUpdateSeq, ch.ack.seq, next_seq);
			#endif
			// check each hop
			double U = 0;
			uint64_t dt = 0;
			bool updated[IntHeader::maxHop] = {false}, updated_any = false;
			NS_ASSERT(ih.nhop <= IntHeader::maxHop);
			for (uint32_t i = 0; i < ih.nhop; i++){
				if (m_sampleFeedback){
					if (ih.hop[i].GetQlen() == 0 and fast_react)
						continue;
				}
				updated[i] = updated_any = true;
				#if PRINT_LOG
				if (print)
					printf(" %u(%u) %lu(%lu) %lu(%lu)", ih.hop[i].GetQlen(), qp->hp.hop[i].GetQlen(), ih.hop[i].GetBytes(), qp->hp.hop[i].GetBytes(), ih.hop[i].GetTime(), qp->hp.hop[i].GetTime());
				#endif
				uint64_t tau = ih.hop[i].GetTimeDelta(qp->hp.hop[i]);;
				double duration = tau * 1e-9;
				double txRate = (ih.hop[i].GetBytesDelta(qp->hp.hop[i])) * 8 / duration;
				double u = txRate / ih.hop[i].GetLineRate() + (double)std::min(ih.hop[i].GetQlen(), qp->hp.hop[i].GetQlen()) * qp->m_max_rate.GetBitRate() / ih.hop[i].GetLineRate() /qp->m_win;
				#if PRINT_LOG
				if (print)
					printf(" %.3lf %.3lf", txRate, u);
				#endif
				if (!m_multipleRate){
					// for aggregate (single R)
					if (u > U){
						U = u;
						dt = tau;
					}
				}else {
					// for per hop (per hop R)
					if (tau > qp->m_baseRtt)
						tau = qp->m_baseRtt;
					qp->hp.hopState[i].u = (qp->hp.hopState[i].u * (qp->m_baseRtt - tau) + u * tau) / double(qp->m_baseRtt);
				}
				qp->hp.hop[i] = ih.hop[i];
			}

			DataRate new_rate;
			int32_t new_incStage;
			DataRate new_rate_per_hop[IntHeader::maxHop];
			int32_t new_incStage_per_hop[IntHeader::maxHop];
			if (!m_multipleRate){
				// for aggregate (single R)
				if (updated_any){
					if (dt > qp->m_baseRtt)
						dt = qp->m_baseRtt;
					qp->hp.u = (qp->hp.u * (qp->m_baseRtt - dt) + U * dt) / double(qp->m_baseRtt);
					max_c = qp->hp.u / m_targetUtil;

					if (max_c >= 1 || qp->hp.m_incStage >= m_miThresh){
						new_rate = qp->hp.m_curRate / max_c + m_rai;
						new_incStage = 0;
					}else{
						new_rate = qp->hp.m_curRate + m_rai;
						new_incStage = qp->hp.m_incStage+1;
					}
					if (new_rate < m_minRate)
						new_rate = m_minRate;
					if (new_rate > qp->m_max_rate)
						new_rate = qp->m_max_rate;
					#if PRINT_LOG
					if (print)
						printf(" u=%.6lf U=%.3lf dt=%u max_c=%.3lf", qp->hp.u, U, dt, max_c);
					#endif
					#if PRINT_LOG
					if (print)
						printf(" rate:%.3lf->%.3lf\n", qp->hp.m_curRate.GetBitRate()*1e-9, new_rate.GetBitRate()*1e-9);
					#endif
				}
			}else{
				// for per hop (per hop R)
				new_rate = qp->m_max_rate;
				for (uint32_t i = 0; i < ih.nhop; i++){
					if (updated[i]){
						double c = qp->hp.hopState[i].u / m_targetUtil;
						if (c >= 1 || qp->hp.hopState[i].incStage >= m_miThresh){
							new_rate_per_hop[i] = qp->hp.hopState[i].Rc / c + m_rai;
							new_incStage_per_hop[i] = 0;
						}else{
							new_rate_per_hop[i] = qp->hp.hopState[i].Rc + m_rai;
							new_incStage_per_hop[i] = qp->hp.hopState[i].incStage+1;
						}
						// bound rate
						if (new_rate_per_hop[i] < m_minRate)
							new_rate_per_hop[i] = m_minRate;
						if (new_rate_per_hop[i] > qp->m_max_rate)
							new_rate_per_hop[i] = qp->m_max_rate;
						// find min new_rate
						if (new_rate_per_hop[i] < new_rate)
							new_rate = new_rate_per_hop[i];
						#if PRINT_LOG
						if (print)
							printf(" [%u]u=%.6lf c=%.3lf", i, qp->hp.hopState[i].u, c);
						#endif
						#if PRINT_LOG
						if (print)
							printf(" %.3lf->%.3lf", qp->hp.hopState[i].Rc.GetBitRate()*1e-9, new_rate.GetBitRate()*1e-9);
						#endif
					}else{
						if (qp->hp.hopState[i].Rc < new_rate)
							new_rate = qp->hp.hopState[i].Rc;
					}
				}
				#if PRINT_LOG
				printf("\n");
				#endif
			}
			if (updated_any)
				ChangeRate(qp, new_rate);
			if (!fast_react){
				if (updated_any){
					qp->hp.m_curRate = new_rate;
					qp->hp.m_incStage = new_incStage;
				}
				if (m_multipleRate){
					// for per hop (per hop R)
					for (uint32_t i = 0; i < ih.nhop; i++){
						if (updated[i]){
							qp->hp.hopState[i].Rc = new_rate_per_hop[i];
							qp->hp.hopState[i].incStage = new_incStage_per_hop[i];
						}
					}
				}
			}
		}
		if (!fast_react){
			if (next_seq > qp->hp.m_lastUpdateSeq)
				qp->hp.m_lastUpdateSeq = next_seq; //+ rand() % 2 * m_mtu;
		}
	}
}

void RdmaHw::FastReactHp(Ptr<RdmaQueuePair> qp, Ptr<Packet> p, CustomHeader &ch){
	if (m_fast_react)
		UpdateRateHp(qp, p, ch, true);
}

/**********************
 * TIMELY
 *********************/
void RdmaHw::HandleAckTimely(Ptr<RdmaQueuePair> qp, Ptr<Packet> p, CustomHeader &ch){
	uint32_t ack_seq = ch.ack.seq;
	// update rate
	if (ack_seq > qp->tmly.m_lastUpdateSeq){ // if full RTT feedback is ready, do full update
		UpdateRateTimely(qp, p, ch, false);
	}else{ // do fast react
		FastReactTimely(qp, p, ch);
	}
}
void RdmaHw::UpdateRateTimely(Ptr<RdmaQueuePair> qp, Ptr<Packet> p, CustomHeader &ch, bool us){
	uint32_t next_seq = qp->snd_nxt;
	uint64_t rtt = Simulator::Now().GetTimeStep() - ch.ack.ih.ts;
	bool print = !us;
	if (qp->tmly.m_lastUpdateSeq != 0){ // not first RTT
		int64_t new_rtt_diff = (int64_t)rtt - (int64_t)qp->tmly.lastRtt;
		double rtt_diff = (1 - m_tmly_alpha) * qp->tmly.rttDiff + m_tmly_alpha * new_rtt_diff;
		double gradient = rtt_diff / m_tmly_minRtt;
		bool inc = false;
		double c = 0;
		#if PRINT_LOG
		if (print)
			printf("%lu node:%u rtt:%lu rttDiff:%.0lf gradient:%.3lf rate:%.3lf", Simulator::Now().GetTimeStep(), m_node->GetId(), rtt, rtt_diff, gradient, qp->tmly.m_curRate.GetBitRate() * 1e-9);
		#endif
		if (rtt < m_tmly_TLow){
			inc = true;
		}else if (rtt > m_tmly_THigh){
			c = 1 - m_tmly_beta * (1 - (double)m_tmly_THigh / rtt);
			inc = false;
		}else if (gradient <= 0){
			inc = true;
		}else{
			c = 1 - m_tmly_beta * gradient;
			if (c < 0)
				c = 0;
			inc = false;
		}
		if (inc){
			if (qp->tmly.m_incStage < 5){
				qp->m_rate = qp->tmly.m_curRate + m_rai;
			}else{
				qp->m_rate = qp->tmly.m_curRate + m_rhai;
			}
			if (qp->m_rate > qp->m_max_rate)
				qp->m_rate = qp->m_max_rate;
			if (!us){
				qp->tmly.m_curRate = qp->m_rate;
				qp->tmly.m_incStage++;
				qp->tmly.rttDiff = rtt_diff;
			}
		}else{
			qp->m_rate = std::max(m_minRate, qp->tmly.m_curRate * c); 
			if (!us){
				qp->tmly.m_curRate = qp->m_rate;
				qp->tmly.m_incStage = 0;
				qp->tmly.rttDiff = rtt_diff;
			}
		}
		#if PRINT_LOG
		if (print){
			printf(" %c %.3lf\n", inc? '^':'v', qp->m_rate.GetBitRate() * 1e-9);
		}
		#endif
	}
	if (!us && next_seq > qp->tmly.m_lastUpdateSeq){
		qp->tmly.m_lastUpdateSeq = next_seq;
		// update
		qp->tmly.lastRtt = rtt;
	}
}
void RdmaHw::FastReactTimely(Ptr<RdmaQueuePair> qp, Ptr<Packet> p, CustomHeader &ch){
}

/**********************
 * DCTCP
 *********************/
void RdmaHw::HandleAckDctcp(Ptr<RdmaQueuePair> qp, Ptr<Packet> p, CustomHeader &ch){
	uint32_t ack_seq = ch.ack.seq;
	uint8_t cnp = (ch.ack.flags >> qbbHeader::FLAG_CNP) & 1;
	bool new_batch = false;

	// update alpha
	qp->dctcp.m_ecnCnt += (cnp > 0);
	if (ack_seq > qp->dctcp.m_lastUpdateSeq){ // if full RTT feedback is ready, do alpha update
		#if PRINT_LOG
		printf("%lu %s %08x %08x %u %u [%u,%u,%u] %.3lf->", Simulator::Now().GetTimeStep(), "alpha", qp->sip.Get(), qp->dip.Get(), qp->sport, qp->dport, qp->dctcp.m_lastUpdateSeq, ch.ack.seq, qp->snd_nxt, qp->dctcp.m_alpha);
		#endif
		new_batch = true;
		if (qp->dctcp.m_lastUpdateSeq == 0){ // first RTT
			qp->dctcp.m_lastUpdateSeq = qp->snd_nxt;
			qp->dctcp.m_batchSizeOfAlpha = qp->snd_nxt / m_mtu + 1;
		}else {
			double frac = std::min(1.0, double(qp->dctcp.m_ecnCnt) / qp->dctcp.m_batchSizeOfAlpha);
			qp->dctcp.m_alpha = (1 - m_g) * qp->dctcp.m_alpha + m_g * frac;
			qp->dctcp.m_lastUpdateSeq = qp->snd_nxt;
			qp->dctcp.m_ecnCnt = 0;
			qp->dctcp.m_batchSizeOfAlpha = (qp->snd_nxt - ack_seq) / m_mtu + 1;
			#if PRINT_LOG
			printf("%.3lf F:%.3lf", qp->dctcp.m_alpha, frac);
			#endif
		}
		#if PRINT_LOG
		printf("\n");
		#endif
	}

	// check cwr exit
	if (qp->dctcp.m_caState == 1){
		if (ack_seq > qp->dctcp.m_highSeq)
			qp->dctcp.m_caState = 0;
	}

	// check if need to reduce rate: ECN and not in CWR
	if (cnp && qp->dctcp.m_caState == 0){
		#if PRINT_LOG
		printf("%lu %s %08x %08x %u %u %.3lf->", Simulator::Now().GetTimeStep(), "rate", qp->sip.Get(), qp->dip.Get(), qp->sport, qp->dport, qp->m_rate.GetBitRate()*1e-9);
		#endif
		qp->m_rate = std::max(m_minRate, qp->m_rate * (1 - qp->dctcp.m_alpha / 2));
		#if PRINT_LOG
		printf("%.3lf\n", qp->m_rate.GetBitRate() * 1e-9);
		#endif
		qp->dctcp.m_caState = 1;
		qp->dctcp.m_highSeq = qp->snd_nxt;
	}

	// additive inc
	if (qp->dctcp.m_caState == 0 && new_batch)
		qp->m_rate = std::min(qp->m_max_rate, qp->m_rate + m_dctcp_rai);
}

}
