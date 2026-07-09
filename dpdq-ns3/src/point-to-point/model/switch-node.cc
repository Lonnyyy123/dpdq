#include "ns3/ipv4.h"
#include "ns3/packet.h"
#include "ns3/ipv4-header.h"
#include "ns3/pause-header.h"
#include "ns3/flow-id-tag.h"
#include "ns3/boolean.h"
#include "ns3/uinteger.h"
#include "ns3/double.h"
#include "ns3/simulator.h"
#include "ns3/trace-source-accessor.h"
#include "switch-node.h"
#include "qbb-net-device.h"
#include "qbb-channel.h"
#include "qbb-header.h"
#include "ppp-header.h"
#include "ns3/int-header.h"
#include "tlt-tag.h"
#include "creditbouncer-tag.h"
#include <algorithm>
#include <iostream>

namespace ns3 {
	struct stat_tx_ stat_tx;

namespace {

static constexpr uint32_t kCbSymmetricGlobalSeed = 0;

class CbJustBouncedTag : public Tag
{
public:
	CbJustBouncedTag()
		: m_justBounced(0) {}

	static TypeId GetTypeId(void)
	{
		static TypeId tid = TypeId("ns3::CbJustBouncedTag")
			.SetParent<Tag>()
			.AddConstructor<CbJustBouncedTag>();
		return tid;
	}

	virtual TypeId GetInstanceTypeId(void) const
	{
		return GetTypeId();
	}

	virtual uint32_t GetSerializedSize(void) const
	{
		return 1;
	}

	virtual void Serialize(TagBuffer i) const
	{
		i.WriteU8(m_justBounced);
	}

	virtual void Deserialize(TagBuffer i)
	{
		m_justBounced = i.ReadU8();
	}

	virtual void Print(std::ostream &os) const
	{
		os << "justBounced=" << static_cast<uint32_t>(m_justBounced);
	}

	void Set(bool justBounced)
	{
		m_justBounced = justBounced ? 1 : 0;
	}

	bool Get(void) const
	{
		return m_justBounced != 0;
	}

private:
	uint8_t m_justBounced;
};

struct __attribute__((packed)) CbSymmetricHashKey
{
	uint32_t firstIp;
	uint32_t secondIp;
	uint16_t firstPort;
	uint16_t secondPort;
	uint16_t pg;
	uint32_t seq;
};

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

static const char*
CbDataClassToString(CreditBouncerTag::DataClass dataClass)
{
	switch (dataClass) {
	case CreditBouncerTag::CB_DATA_CLASS_UNSOLICITED: return "unsolicited";
	case CreditBouncerTag::CB_DATA_CLASS_UNSCHEDULED: return "unscheduled";
	case CreditBouncerTag::CB_DATA_CLASS_SCHEDULED: return "scheduled";
	default: return "none";
	}
}

static const char*
CbControlPacketTypeForLog(Ptr<const Packet> p, const CustomHeader &ch)
{
	if (ch.l3Prot != 0xFB || p == 0)
		return "non_cb";
	CreditBouncerTag cbTag;
	if (!p->PeekPacketTag(cbTag) || cbTag.GetType() == CreditBouncerTag::CB_MSG_NONE)
		return "cb_unknown";
	return CbMessageTypeToString(cbTag.GetType());
}

}

TypeId SwitchNode::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::SwitchNode")
    .SetParent<Node> ()
    .AddConstructor<SwitchNode> ()
	.AddAttribute("EcnEnabled",
			"Enable ECN marking.",
			BooleanValue(false),
			MakeBooleanAccessor(&SwitchNode::m_ecnEnabled),
			MakeBooleanChecker())
	.AddAttribute("CcMode",
			"CC mode.",
			UintegerValue(0),
			MakeUintegerAccessor(&SwitchNode::m_ccMode),
			MakeUintegerChecker<uint32_t>())
	.AddAttribute("AckHighPrio",
			"Set high priority for ACK/NACK or not",
			UintegerValue(0),
			MakeUintegerAccessor(&SwitchNode::m_ackHighPrio),
			MakeUintegerChecker<uint32_t>())
	.AddAttribute("CreditBouncerSymmetricRouting",
			"Use direction-independent ECMP hashing for CreditBouncer traffic.",
			BooleanValue(true),
			MakeBooleanAccessor(&SwitchNode::m_creditbouncerSymmetricRouting),
			MakeBooleanChecker())
	.AddAttribute("CreditBouncerRouteLog",
			"Emit CreditBouncer route-selection logs at the switch.",
			BooleanValue(false),
			MakeBooleanAccessor(&SwitchNode::m_creditbouncerRouteLog),
			MakeBooleanChecker())
	.AddAttribute("CreditBouncerRouteMaxLogs",
			"Max number of CreditBouncer route-selection logs emitted per switch.",
			UintegerValue(200),
			MakeUintegerAccessor(&SwitchNode::m_creditbouncerRouteMaxLogs),
			MakeUintegerChecker<uint32_t>())
	.AddAttribute("CreditBouncerCBEnable",
			"Enable switch-side CB algorithm-1 behavior for CreditBouncer packets.",
			BooleanValue(true),
			MakeBooleanAccessor(&SwitchNode::m_creditbouncerCBEnable),
			MakeBooleanChecker())
	.AddAttribute("CreditBouncerCcEnable",
			"Enable switch-side CreditBouncer congestion-control behavior (ECN/CC path).",
			BooleanValue(true),
			MakeBooleanAccessor(&SwitchNode::m_creditbouncerCcEnable),
			MakeBooleanChecker())
	.AddAttribute("CreditBouncerFcEnable",
			"Enable switch-side CreditBouncer flow-control behavior (PAUSE/BOUNCE path).",
			BooleanValue(true),
			MakeBooleanAccessor(&SwitchNode::m_creditbouncerFcEnable),
			MakeBooleanChecker())
	.AddAttribute("CreditBouncerXonBytes",
			"CB switch Xon threshold in bytes.",
			UintegerValue(65536),
			MakeUintegerAccessor(&SwitchNode::m_creditbouncerXonBytes),
			MakeUintegerChecker<uint64_t>())
	.AddAttribute("CreditBouncerXoffBytes",
			"CB switch Xoff threshold in bytes.",
			UintegerValue(131072),
			MakeUintegerAccessor(&SwitchNode::m_creditbouncerXoffBytes),
			MakeUintegerChecker<uint64_t>())
	.AddAttribute("CreditBouncerXecnBytes",
			"CB switch Xecn threshold in bytes.",
			UintegerValue(100 * 1024), // 0.5 * BDP (BDP=200KB)
			MakeUintegerAccessor(&SwitchNode::m_creditbouncerXecnBytes),
			MakeUintegerChecker<uint64_t>())
	.AddAttribute("CreditBouncerDynamicThreshold",
			"Enable PFC-like dynamic Xon/Xoff threshold for CB switch state machine.",
			BooleanValue(true),
			MakeBooleanAccessor(&SwitchNode::m_creditbouncerDynamicThreshold),
			MakeBooleanChecker())
	.AddAttribute("CreditBouncerDynamicAlpha",
			"Alpha factor for dynamic CB Xon/Xoff threshold.",
			DoubleValue(0.0625),
			MakeDoubleAccessor(&SwitchNode::m_creditbouncerDynamicAlpha),
			MakeDoubleChecker<double>())
	.AddAttribute("CreditBouncerDynamicOffDiffBytes",
			"Hysteresis offset in bytes for dynamic CB Xon threshold.",
			UintegerValue(16),
			MakeUintegerAccessor(&SwitchNode::m_creditbouncerDynamicOffDiffBytes),
			MakeUintegerChecker<uint64_t>())
	.AddAttribute("CreditBouncerDedicatedFcMarginBytes",
			"Headroom in bytes used by dedicated CB FC mode so Xon = Xoff - headroom.",
			UintegerValue(3 * 1024),
			MakeUintegerAccessor(&SwitchNode::m_creditbouncerDedicatedFcMarginBytes),
			MakeUintegerChecker<uint64_t>())
	.AddAttribute("CreditBouncerDedicatedXoffMarginBytes",
			"Fixed margin in bytes subtracted from the dynamic dedicated CB FC Xoff base.",
			UintegerValue(3 * 1024),
			MakeUintegerAccessor(&SwitchNode::m_creditbouncerDedicatedXoffMarginBytes),
			MakeUintegerChecker<uint64_t>())
	.AddAttribute("CreditBouncerStateLog",
			"Emit switch-side CB state-machine logs for CreditBouncer traffic.",
			BooleanValue(false),
			MakeBooleanAccessor(&SwitchNode::m_creditbouncerStateLog),
			MakeBooleanChecker())
	.AddAttribute("CreditBouncerStateMaxLogs",
			"Max number of switch-side CB state-machine logs emitted per switch.",
			UintegerValue(200),
			MakeUintegerAccessor(&SwitchNode::m_creditbouncerStateMaxLogs),
			MakeUintegerChecker<uint32_t>())
	.AddTraceSource("CreditBouncerBounce",
			"Triggered when the switch bounces a CreditBouncer grant back toward the receiver.",
			MakeTraceSourceAccessor(&SwitchNode::m_traceCreditBouncerBounce))
	.AddTraceSource("CreditBouncerFcState",
			"Triggered when the CreditBouncer FC state changes. args: outDev, oldState, newState.",
			MakeTraceSourceAccessor(&SwitchNode::m_traceCreditBouncerFcState))
	.AddTraceSource("CreditBouncerPacketEvent",
			"Per-packet accounting event for CreditBouncer traffic. args: nodeId, outDev, sip, dip, sport, dport, classId(1=credit,2=data), isDrop(0/1).",
			MakeTraceSourceAccessor(&SwitchNode::m_traceCreditBouncerPacketEvent))
	.AddAttribute("CreditBouncerDedicatedMmuEnable",
			"Enable dedicated CreditBouncer dual-queue MMU path for switch egress admission.",
			BooleanValue(false),
			MakeBooleanAccessor(&SwitchNode::m_creditbouncerDedicatedMmuEnable),
			MakeBooleanChecker())
	.AddAttribute("CreditBouncerDedicatedCreditQueueBytes",
			"Fixed queue size per egress port for CreditBouncer control packets.",
			UintegerValue(64 * 1024),
			MakeUintegerAccessor(&SwitchNode::m_creditbouncerDedicatedCreditQueueBytes),
			MakeUintegerChecker<uint32_t>())
	.AddAttribute("CreditBouncerDedicatedDataHeadroomBytes",
			"Legacy compatibility field retained for configs/logging; ignored by the current dedicated DPDQ MMU admission rule.",
			UintegerValue(12500 + 2 * SwitchMmu::MTU),
			MakeUintegerAccessor(&SwitchNode::m_creditbouncerDedicatedDataHeadroomBytes),
			MakeUintegerChecker<uint32_t>())
	.AddAttribute("CreditBouncerDedicatedDataGuaranteeBytes",
			"Legacy compatibility field retained for configs/logging; ignored by the current dedicated DPDQ MMU admission rule.",
			UintegerValue(1048),
			MakeUintegerAccessor(&SwitchNode::m_creditbouncerDedicatedDataGuaranteeBytes),
			MakeUintegerChecker<uint32_t>())
	.AddAttribute("CreditBouncerDedicatedDataEcnKminBytes",
			"Dedicated CreditBouncer data queue ECN minimum threshold in bytes.",
			UintegerValue(64 * 1024),
			MakeUintegerAccessor(&SwitchNode::m_creditbouncerDedicatedDataEcnKminBytes),
			MakeUintegerChecker<uint32_t>())
	.AddAttribute("CreditBouncerDedicatedDataEcnKmaxBytes",
			"Dedicated CreditBouncer data queue ECN maximum threshold in bytes.",
			UintegerValue(96 * 1024),
			MakeUintegerAccessor(&SwitchNode::m_creditbouncerDedicatedDataEcnKmaxBytes),
			MakeUintegerChecker<uint32_t>())
	.AddAttribute("CreditBouncerDedicatedDataEcnPmax",
			"Dedicated CreditBouncer data queue ECN max probability.",
			DoubleValue(1.0),
			MakeDoubleAccessor(&SwitchNode::m_creditbouncerDedicatedDataEcnPmax),
			MakeDoubleChecker<double>())
		  ;
	  return tid;
}

SwitchNode::SwitchNode(){
	m_ecmpSeed = m_id;
	m_node_type = 1;
	m_creditbouncerSymmetricRouting = true;
	m_creditbouncerRouteLog = false;
	m_creditbouncerRouteMaxLogs = 200;
	m_creditbouncerRouteLogCount = 0;
	m_creditbouncerCBEnable = true;
	m_creditbouncerCcEnable = true;
	m_creditbouncerFcEnable = true;
	m_creditbouncerXonBytes = 65536;
	m_creditbouncerXoffBytes = 131072;
	m_creditbouncerXecnBytes = 100 * 1024;
	m_creditbouncerDynamicThreshold = true;
	m_creditbouncerDynamicAlpha = 0.0625;
	m_creditbouncerDynamicOffDiffBytes = 16;
	m_creditbouncerDedicatedFcMarginBytes = 3 * 1024;
	m_creditbouncerDedicatedXoffMarginBytes = 3 * 1024;
	m_creditbouncerStateLog = false;
	m_creditbouncerStateMaxLogs = 200;
	m_creditbouncerStateLogCount = 0;
	m_creditbouncerDedicatedMmuEnable = false;
	m_creditbouncerDedicatedCreditQueueBytes = 64 * 1024;
	m_creditbouncerDedicatedDataHeadroomBytes = 12500 + 2 * SwitchMmu::MTU;
	m_creditbouncerDedicatedDataGuaranteeBytes = 1048;
	m_creditbouncerDedicatedDataEcnKminBytes = 64 * 1024;
	m_creditbouncerDedicatedDataEcnKmaxBytes = 96 * 1024;
	m_creditbouncerDedicatedDataEcnPmax = 1.0;
	m_mmu = CreateObject<SwitchMmu>();
	m_cbMmu = CreateObject<SwitchCbMmu>();
	for (uint32_t i = 0; i < pCnt; i++)
		for (uint32_t j = 0; j < pCnt; j++)
			for (uint32_t k = 0; k < qCnt; k++)
				m_bytes[i][j][k] = 0;
	for (uint32_t i = 0; i < pCnt; i++) {
		m_txBytes[i] = 0;
		m_creditbouncerPortState[i].m_fcState = CB_FCSTATE_NORMAL;
		m_creditbouncerPortState[i].m_qpLenBytes = 0;
		m_creditbouncerPortState[i].m_qdLenBytes = 0;
		m_creditbouncerPortState[i].m_lastUpdateTimeNs = 0;
		m_creditbouncerPortState[i].m_xonBytes = m_creditbouncerXonBytes;
		m_creditbouncerPortState[i].m_xoffBytes = m_creditbouncerXoffBytes;
		m_creditbouncerPortState[i].m_xecnBytes = m_creditbouncerXecnBytes;
	}
}

int SwitchNode::GetOutDev(Ptr<const Packet> p, CustomHeader &ch){
	// look up entries
	auto entry = m_rtTable.find(ch.dip);

	// no matching entry
	if (entry == m_rtTable.end())
		return -1;

	// entry found
	auto &nexthops = entry->second;

	CreditBouncerTag cbTag;
	bool hasCbTag = p->PeekPacketTag(cbTag) && cbTag.GetType() != CreditBouncerTag::CB_MSG_NONE;
	uint16_t cbSport = 0;
	uint16_t cbDport = 0;
	uint16_t cbPg = 0;
	uint32_t cbSeq = 0;
	bool cbHasTuple = false;
	if (hasCbTag) {
		if (ch.l3Prot == 0x11) {
			cbSport = ch.udp.sport;
			cbDport = ch.udp.dport;
			cbPg = ch.udp.pg;
			cbSeq = ch.udp.seq;
			cbHasTuple = true;
		} else if (ch.l3Prot == 0xFB || ch.l3Prot == 0xFC || ch.l3Prot == 0xFD) {
			cbSport = ch.ack.sport;
			cbDport = ch.ack.dport;
			cbPg = ch.ack.pg;
			cbSeq = ch.ack.seq;
			cbHasTuple = true;
		}
	}

	if (m_creditbouncerSymmetricRouting && hasCbTag && cbHasTuple) {
		bool isCbRequestData = ch.l3Prot == 0x11 && cbTag.IsRequest();
		bool isCbControl = ch.l3Prot == 0xFB &&
			(cbTag.IsGrant() || cbTag.IsRequest() || cbTag.IsBouncedCredit());
		if (isCbRequestData || isCbControl) {
			uint32_t hash = BuildCreditBouncerSymmetricHash(
				ch.sip, ch.dip, cbSport, cbDport, cbPg, cbSeq);
			int outDev = nexthops[hash % nexthops.size()];
			if (m_creditbouncerRouteLog && m_creditbouncerRouteLogCount < m_creditbouncerRouteMaxLogs) {
				std::cerr << "CBROUTE switch node=" << GetId()
					<< " type=" << CbMessageTypeToString(cbTag.GetType())
					<< " mode=symmetric"
					<< " sip=" << Ipv4Address(ch.sip)
					<< " dip=" << Ipv4Address(ch.dip)
					<< " sport=" << cbSport
					<< " dport=" << cbDport
					<< " pg=" << cbPg
					<< " seq=" << cbSeq
					<< " seed=" << kCbSymmetricGlobalSeed
					<< " hash=" << hash
					<< " out_dev=" << outDev
					<< "\n";
				m_creditbouncerRouteLogCount++;
			}
			return outDev;
		}
	}

	// pick one next hop based on hash
	union {
		uint8_t u8[4+4+2+2];
		uint32_t u32[3];
	} buf;
	buf.u32[0] = ch.sip;
	buf.u32[1] = ch.dip;
	if (ch.l3Prot == 0x6)
		buf.u32[2] = ch.tcp.sport | ((uint32_t)ch.tcp.dport << 16);
	else if (ch.l3Prot == 0x11)
		buf.u32[2] = ch.udp.sport | ((uint32_t)ch.udp.dport << 16);
	else if (ch.l3Prot == 0xFC || ch.l3Prot == 0xFD || ch.l3Prot == 0xFB)
		buf.u32[2] = ch.ack.sport | ((uint32_t)ch.ack.dport << 16);

	uint32_t hash = EcmpHash(buf.u8, 12, m_ecmpSeed);
	int outDev = nexthops[hash % nexthops.size()];
	if (hasCbTag && cbHasTuple && m_creditbouncerRouteLog && m_creditbouncerRouteLogCount < m_creditbouncerRouteMaxLogs) {
		std::cerr << "CBROUTE switch node=" << GetId()
			<< " type=" << CbMessageTypeToString(cbTag.GetType())
			<< " mode=default"
			<< " sip=" << Ipv4Address(ch.sip)
			<< " dip=" << Ipv4Address(ch.dip)
			<< " sport=" << cbSport
			<< " dport=" << cbDport
			<< " pg=" << cbPg
			<< " seq=" << cbSeq
			<< " hash=" << hash
			<< " out_dev=" << outDev
			<< "\n";
		m_creditbouncerRouteLogCount++;
	}
	return outDev;
}

uint32_t SwitchNode::BuildCreditBouncerSymmetricHash(uint32_t sip, uint32_t dip, uint16_t sport, uint16_t dport, uint16_t pg, uint32_t seq){
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
	key.seq = seq;
	return EcmpHash(reinterpret_cast<const uint8_t *>(&key), sizeof(key), kCbSymmetricGlobalSeed);
}

bool SwitchNode::IsCreditBouncerControlPacket(Ptr<const Packet> p, const CustomHeader &ch) const{
	if (ch.l3Prot != 0xFB)
		return false;
	CreditBouncerTag cbTag;
	if (!p->PeekPacketTag(cbTag))
		return false;
	// Dedicated CB priority queue should only carry credit/signal control packets:
	// GRANT, GRANT_REQ, BOUNCED_CREDIT, and RESEND.
	return cbTag.IsGrant() || cbTag.IsGrantReq() || cbTag.IsBouncedCredit() || cbTag.IsResend();
}

bool SwitchNode::IsCreditGrantPacket(Ptr<const Packet> p, const CustomHeader &ch) const{
	if (!IsCreditBouncerControlPacket(p, ch))
		return false;
	CreditBouncerTag cbTag;
	return p->PeekPacketTag(cbTag) && cbTag.IsGrant();
}

bool SwitchNode::IsBouncedCreditPacket(Ptr<const Packet> p, const CustomHeader &ch) const{
	if (!IsCreditBouncerControlPacket(p, ch))
		return false;
	CreditBouncerTag cbTag;
	return p->PeekPacketTag(cbTag) && cbTag.IsBouncedCredit();
}

bool SwitchNode::IsSignalPacket(Ptr<const Packet> p, const CustomHeader &ch) const{
	if (!IsCreditBouncerControlPacket(p, ch))
		return false;
	CreditBouncerTag cbTag;
	if (!p->PeekPacketTag(cbTag))
		return false;
	return cbTag.IsGrantReq() || cbTag.IsResend();
}

bool SwitchNode::IsUnscheduledDataPacket(Ptr<const Packet> p, const CustomHeader &ch) const{
	if (ch.l3Prot != 0x11)
		return false;
	CreditBouncerTag cbTag;
	if (!p->PeekPacketTag(cbTag))
		return false;
	return cbTag.IsRequest() && cbTag.IsUnscheduledData();
}

bool SwitchNode::IsDataPacket(const CustomHeader &ch) const{
	return ch.l3Prot == 0x11 || ch.l3Prot == 0x06;
}

uint32_t SwitchNode::GetDedicatedCbQueueIndex(Ptr<const Packet> p, const CustomHeader &ch) const{
	if (ch.l3Prot == 0xFF || ch.l3Prot == 0xFE) {
		return 0;
	}
	if (IsCreditBouncerControlPacket(p, ch)) {
		return 0;
	}
	if (m_ackHighPrio && (ch.l3Prot == 0xFD || ch.l3Prot == 0xFC)) {
		return 0;
	}
	return 1;
}

void SwitchNode::SyncDedicatedCbMmuConfig(void){
	if (!m_creditbouncerDedicatedMmuEnable || m_cbMmu == 0) {
		return;
	}

	uint32_t activePortCnt = GetNDevices() > 0 ? (GetNDevices() - 1) : 0;
	if (m_cbMmu->GetActivePortCnt() != activePortCnt) {
		m_cbMmu->SetActivePortCnt(activePortCnt);
	}

	m_cbMmu->SetNodeId(GetId());
	m_cbMmu->SetCreditQueueLimitBytes(m_creditbouncerDedicatedCreditQueueBytes);
	m_cbMmu->SetDefaultDataHeadroomBytes(m_creditbouncerDedicatedDataHeadroomBytes);
	m_cbMmu->SetDefaultDataGuaranteeBytes(m_creditbouncerDedicatedDataGuaranteeBytes);
	m_cbMmu->SetDataEcnKminBytes(m_creditbouncerDedicatedDataEcnKminBytes);
	m_cbMmu->SetDataEcnKmaxBytes(m_creditbouncerDedicatedDataEcnKmaxBytes);
	m_cbMmu->SetDataEcnPmax(m_creditbouncerDedicatedDataEcnPmax);

	// Keep legacy per-port headroom bookkeeping in sync for compatibility/debug
	// visibility, even though the current dedicated DPDQ MMU admission rule
	// ignores headroom.
	for (uint32_t port = 1; port <= activePortCnt && port < pCnt; ++port) {
		Ptr<QbbNetDevice> dev = DynamicCast<QbbNetDevice>(GetDevice(port));
		if (dev == 0) {
			continue;
		}
		Ptr<QbbChannel> ch = DynamicCast<QbbChannel>(dev->GetChannel());
		if (ch == 0) {
			continue;
		}
		uint64_t rateBps = dev->GetDataRate().GetBitRate();
		uint64_t linkDelayNs = ch->GetDelay().GetNanoSeconds();
		m_cbMmu->ConfigDataHeadroomByBdp(port, rateBps, linkDelayNs);
	}
}

bool SwitchNode::ShouldBounceCreditPacket(uint32_t inDev, uint32_t outDev, Ptr<const Packet> p, const CustomHeader &ch) const{
	bool cbFcEnabled = m_creditbouncerCBEnable && m_creditbouncerFcEnable;
	if (!cbFcEnabled)
		return false;
	if (inDev >= pCnt)
		return false;
	if (outDev >= pCnt)
		return false;
	if (!IsCreditGrantPacket(p, ch))
		return false;
	// Credit bounce is an ingress behavior: only bounce when the packet
	// arrives on a paused ingress port.
	return m_creditbouncerPortState[inDev].m_fcState == CB_FCSTATE_PAUSE;
}

const char *SwitchNode::CbFcStateToString(CreditBouncerFcState state) const{
	return state == CB_FCSTATE_PAUSE ? "PAUSE" : "NORMAL";
}

void SwitchNode::LogCBPortEvent(const char *event,
	uint32_t outDev,
	const CustomHeader &ch,
	Ptr<const Packet> p,
	uint64_t qpBefore,
	uint64_t qdBefore,
	uint64_t qpAfter,
	uint64_t qdAfter,
	uint64_t deltaBytes,
	int32_t peerDev){
	if (!m_creditbouncerStateLog || m_creditbouncerStateLogCount >= m_creditbouncerStateMaxLogs)
		return;
	if (event == 0)
		return;
	if (outDev >= pCnt)
		return;

	CreditBouncerTag cbTag;
	bool hasCbTag = p != 0 && p->PeekPacketTag(cbTag) && cbTag.GetType() != CreditBouncerTag::CB_MSG_NONE;
	uint16_t sport = 0;
	uint16_t dport = 0;
	uint16_t pg = 0;
	if (ch.l3Prot == 0x11) {
		sport = ch.udp.sport;
		dport = ch.udp.dport;
		pg = ch.udp.pg;
	} else if (ch.l3Prot == 0xFB || ch.l3Prot == 0xFC || ch.l3Prot == 0xFD) {
		sport = ch.ack.sport;
		dport = ch.ack.dport;
		pg = ch.ack.pg;
	}

	std::cerr << "CBPH event=" << event
		<< " node=" << GetId()
		<< " out_dev=" << outDev;
	if (peerDev >= 0)
		std::cerr << " peer_dev=" << peerDev;
	uint64_t qcNow = 0;
	uint64_t qdNow = m_creditbouncerPortState[outDev].m_qdLenBytes;
	uint64_t qdQcNow = qdNow;
	uint64_t compareNow = std::max(qdNow, m_creditbouncerPortState[outDev].m_qpLenBytes);
	uint64_t thresholdBudgetNow = 0;
	if (IsCreditBouncerDedicatedMmuEnabled()) {
		qdNow = m_cbMmu->GetDataQueueBytes(outDev);
		qcNow = m_cbMmu->GetCreditQueueBytes(outDev);
		qdQcNow = qdNow + qcNow;
		compareNow = std::max(qdQcNow, m_creditbouncerPortState[outDev].m_qpLenBytes);
		if (m_creditbouncerDynamicThreshold) {
			uint64_t dedicatedFcBs = m_cbMmu->GetDedicatedFcBsBytes();
			uint64_t totalQd = m_cbMmu->GetTotalDataQueueBytes();
			uint64_t dedicatedFcAvail = dedicatedFcBs > totalQd ? (dedicatedFcBs - totalQd) : 0;
			thresholdBudgetNow = static_cast<uint64_t>(
				m_creditbouncerDynamicAlpha * static_cast<double>(dedicatedFcAvail));
		}
	}
	std::cerr << " fc=" << CbFcStateToString(m_creditbouncerPortState[outDev].m_fcState)
		<< " xon=" << m_creditbouncerPortState[outDev].m_xonBytes
		<< " xoff=" << m_creditbouncerPortState[outDev].m_xoffBytes
		<< " xecn=" << m_creditbouncerPortState[outDev].m_xecnBytes
		<< " qc_now=" << qcNow
		<< " qp_before=" << qpBefore
		<< " qp_after=" << qpAfter
		<< " qd_before=" << qdBefore
		<< " qd_after=" << qdAfter
		<< " qd_now=" << qdNow
		<< " qd_qc_now=" << qdQcNow
		<< " compare_now=" << compareNow
		<< " t_now=" << thresholdBudgetNow
		<< " delta=" << deltaBytes
		<< " pkt_size=" << (p != 0 ? p->GetSize() : 0)
		<< " sip=" << Ipv4Address(ch.sip)
		<< " dip=" << Ipv4Address(ch.dip)
		<< " sport=" << sport
		<< " dport=" << dport
		<< " pg=" << pg
		<< " l3=" << static_cast<uint32_t>(ch.l3Prot);
	if (hasCbTag) {
		std::cerr << " cb_type=" << CbMessageTypeToString(cbTag.GetType())
			<< " cb_credit=" << cbTag.GetCreditBytes()
			<< " cb_credit_req=" << cbTag.GetCreditReqBytes()
			<< " cb_data_class=" << CbDataClassToString(cbTag.GetDataClass());
	}
	std::cerr << "\n";
	m_creditbouncerStateLogCount++;
}

void SwitchNode::MarkPacketAsBouncedCredit(Ptr<Packet> p) const{
	CreditBouncerTag oldTag;
	if (!p->PeekPacketTag(oldTag))
		return;
	if (oldTag.GetType() == CreditBouncerTag::CB_MSG_NONE)
		return;
	CreditBouncerTag bouncedTag = oldTag;
	bouncedTag.SetType(CreditBouncerTag::CB_MSG_BOUNCED_CREDIT);
	p->RemovePacketTag(oldTag);
	p->AddPacketTag(bouncedTag);
}

void SwitchNode::UpdateCBPortStateOnTransmit(uint32_t outDev, Ptr<Packet> p, CustomHeader &ch){
	bool cbCcEnabled = m_creditbouncerCBEnable && m_creditbouncerCcEnable;
	bool cbFcEnabled = m_creditbouncerCBEnable && m_creditbouncerFcEnable;
	if ((!cbCcEnabled && !cbFcEnabled) || outDev >= pCnt)
		return;

	CbJustBouncedTag justBouncedTag;
	bool justBounced = p->PeekPacketTag(justBouncedTag) && justBouncedTag.Get();
	if (justBounced) {
		p->RemovePacketTag(justBouncedTag);
	}

	CreditBouncerPortState &state = m_creditbouncerPortState[outDev];
	state.m_xonBytes = m_creditbouncerXonBytes;
	state.m_xoffBytes = m_creditbouncerXoffBytes;
	state.m_xecnBytes = m_creditbouncerXecnBytes;
	uint64_t qdLen = state.m_qdLenBytes;
	uint64_t qcLen = 0;
	if (m_creditbouncerDedicatedMmuEnable && m_cbMmu != 0) {
		qdLen = m_cbMmu->GetDataQueueBytes(outDev);
		qcLen = m_cbMmu->GetCreditQueueBytes(outDev);
	}
	uint64_t qdQcLen = qdLen + qcLen;
	uint64_t compareLen = (m_creditbouncerDedicatedMmuEnable && m_cbMmu != 0)
		? std::max(qdQcLen, state.m_qpLenBytes)
		: std::max(qdLen, state.m_qpLenBytes);
	CreditBouncerFcState oldState = state.m_fcState;

	if (cbFcEnabled && m_creditbouncerDynamicThreshold && m_creditbouncerDedicatedMmuEnable && m_cbMmu != 0) {
		uint64_t dedicatedFcBs = m_cbMmu->GetDedicatedFcBsBytes();
		uint64_t totalQd = m_cbMmu->GetTotalDataQueueBytes();
		uint64_t dedicatedFcAvail = dedicatedFcBs > totalQd ? (dedicatedFcBs - totalQd) : 0;
		uint64_t thresholdBudget = static_cast<uint64_t>(
			m_creditbouncerDynamicAlpha * static_cast<double>(dedicatedFcAvail));
		uint64_t xoffBase = qdQcLen + thresholdBudget;
		uint64_t xoffMarginBytes = m_creditbouncerDedicatedXoffMarginBytes;
		uint64_t headroomBytes = m_creditbouncerDedicatedFcMarginBytes;
		state.m_xoffBytes = xoffBase > xoffMarginBytes
			? (xoffBase - xoffMarginBytes)
			: 0;
		state.m_xonBytes = state.m_xoffBytes > headroomBytes
			? (state.m_xoffBytes - headroomBytes)
			: 0;
	}

	if (cbFcEnabled && m_creditbouncerDynamicThreshold && m_creditbouncerDedicatedMmuEnable && m_cbMmu != 0) {
		if (compareLen > state.m_xoffBytes && state.m_fcState == CB_FCSTATE_NORMAL)
			state.m_fcState = CB_FCSTATE_PAUSE;
		if (compareLen < state.m_xonBytes && state.m_fcState == CB_FCSTATE_PAUSE)
			state.m_fcState = CB_FCSTATE_NORMAL;
	} else if (cbFcEnabled) {
		if (compareLen > state.m_xoffBytes && state.m_fcState == CB_FCSTATE_NORMAL)
			state.m_fcState = CB_FCSTATE_PAUSE;
		if (compareLen < state.m_xonBytes && state.m_fcState == CB_FCSTATE_PAUSE)
			state.m_fcState = CB_FCSTATE_NORMAL;
	}

	if (cbFcEnabled && state.m_fcState != oldState){
		m_traceCreditBouncerFcState(outDev,
			static_cast<uint32_t>(oldState),
			static_cast<uint32_t>(state.m_fcState));
		const char *stateEvent = oldState == CB_FCSTATE_NORMAL
			? "state_change_normal_to_pause"
			: "state_change_pause_to_normal";
		LogCBPortEvent(stateEvent, outDev, ch, p,
			state.m_qpLenBytes, state.m_qdLenBytes,
			state.m_qpLenBytes, state.m_qdLenBytes, compareLen);
	}

	if (cbCcEnabled && IsDataPacket(ch) && compareLen > state.m_xecnBytes){
		PppHeader ppp;
		Ipv4Header h;
		p->RemoveHeader(ppp);
		p->RemoveHeader(h);
		h.SetEcn((Ipv4Header::EcnType)0x03);
		p->AddHeader(h);
		p->AddHeader(ppp);
	}

	if (cbFcEnabled && IsBouncedCreditPacket(p, ch) && !justBounced){
		CreditBouncerTag cbTag;
		if (p->PeekPacketTag(cbTag)) {
			uint64_t bouncedCredit = cbTag.GetCreditBytes();
			uint64_t qpBefore = state.m_qpLenBytes;
			state.m_qpLenBytes += p->GetSize();
			if (state.m_qpLenBytes >= bouncedCredit)
				state.m_qpLenBytes -= bouncedCredit;
			else
				state.m_qpLenBytes = 0;
			LogCBPortEvent("dequeue_bounced_credit", outDev, ch, p,
				qpBefore, state.m_qdLenBytes,
				state.m_qpLenBytes, state.m_qdLenBytes, bouncedCredit);
		}
	}

	if (IsUnscheduledDataPacket(p, ch) || IsCreditGrantPacket(p, ch) || IsSignalPacket(p, ch)) {
		uint64_t qpBefore = state.m_qpLenBytes;
		state.m_qpLenBytes += p->GetSize();
		const char *eventName = IsCreditGrantPacket(p, ch)
			? "phantom_enqueue_grant"
			: (IsSignalPacket(p, ch) ? "phantom_enqueue_signal" : "phantom_enqueue_unscheduled");
		LogCBPortEvent(eventName, outDev, ch, p,
			qpBefore, state.m_qdLenBytes,
			state.m_qpLenBytes, state.m_qdLenBytes, p->GetSize());
	}

	uint64_t nowNs = Simulator::Now().GetNanoSeconds();
	if (state.m_lastUpdateTimeNs != 0 && nowNs > state.m_lastUpdateTimeNs){
		uint64_t intervalNs = nowNs - state.m_lastUpdateTimeNs;
		Ptr<QbbNetDevice> outDevPtr = DynamicCast<QbbNetDevice>(m_devices[outDev]);
		if (outDevPtr != NULL){
			uint64_t lineRateBps = outDevPtr->GetDataRate().GetBitRate();
			uint64_t drainedBytes = (intervalNs * lineRateBps) / 8000000000ULL;
			uint64_t qpBefore = state.m_qpLenBytes;
			if (state.m_qpLenBytes >= drainedBytes)
				state.m_qpLenBytes -= drainedBytes;
			else
				state.m_qpLenBytes = 0;
			if (qpBefore != state.m_qpLenBytes){
				LogCBPortEvent("phantom_drain_estimate", outDev, ch, p,
					qpBefore, state.m_qdLenBytes,
					state.m_qpLenBytes, state.m_qdLenBytes, drainedBytes);
			}
		}
	}

	state.m_lastUpdateTimeNs = nowNs;
}

void SwitchNode::CheckAndSendPfc(uint32_t inDev, uint32_t qIndex){
	Ptr<QbbNetDevice> device = DynamicCast<QbbNetDevice>(m_devices[inDev]);
	bool pClasses[qCnt] = { 0 };
	m_mmu->GetPauseClasses(inDev, qIndex, pClasses);
	for (int j = 0; j < qCnt; j++)
	{
		if(pClasses[j]) {
			uint32_t paused_time = device->SendPfc(j, 0);
			m_mmu->SetPause(inDev, j, paused_time);
			m_mmu->m_pause_remote[inDev][j] = true;
			if(device->IsQbbEnabled())
				stat_tx.PauseSendCnt++;
		}
	}

	for (int j = 0; j < qCnt; j++)
	{
		if(!m_mmu->m_pause_remote[inDev][j])
			continue;

		if (m_mmu->GetResumeClasses(inDev, j)){
			device->SendPfc(j, 1);
			m_mmu->SetResume(inDev, j);
			m_mmu->m_pause_remote[inDev][j] = false;
		}
	}
}
void SwitchNode::CheckAndSendResume(uint32_t inDev, uint32_t qIndex){
	Ptr<QbbNetDevice> device = DynamicCast<QbbNetDevice>(m_devices[inDev]);
	if (m_mmu->GetResumeClasses(inDev, qIndex)){
		device->SendPfc(qIndex, 1);
		m_mmu->SetResume(inDev, qIndex);
	}
}

void SwitchNode::SendToDev(Ptr<Packet>p, CustomHeader &ch, uint32_t inDevHint){
	int idx = GetOutDev(p, ch);
	if (idx >= 0){
		int forwardIdx = idx;
		auto emitCbPacketEvent = [&](uint32_t outDev, bool isDrop) {
			bool isCredit = IsCreditBouncerControlPacket(p, ch);
			bool isData = IsDataPacket(ch);
			if (!isCredit && !isData) {
				return;
			}
			uint16_t sport = 0;
			uint16_t dport = 0;
			if (ch.l3Prot == 0x11) {
				sport = ch.udp.sport;
				dport = ch.udp.dport;
			} else if (ch.l3Prot == 0x06) {
				sport = ch.tcp.sport;
				dport = ch.tcp.dport;
			} else if (ch.l3Prot == 0xFB || ch.l3Prot == 0xFC || ch.l3Prot == 0xFD) {
				sport = ch.ack.sport;
				dport = ch.ack.dport;
			}
			uint32_t classId = isCredit ? 1u : 2u;
			m_traceCreditBouncerPacketEvent(GetId(), outDev, ch.sip, ch.dip, sport, dport, classId, isDrop ? 1u : 0u);
		};
		if (ShouldBounceCreditPacket(inDevHint, static_cast<uint32_t>(idx), p, ch) && inDevHint < pCnt && m_devices[inDevHint] != 0){
			uint64_t qpBefore = m_creditbouncerPortState[idx].m_qpLenBytes;
			uint64_t qdBefore = m_creditbouncerPortState[idx].m_qdLenBytes;
			uint64_t bouncedCreditBytes = 0;
			CreditBouncerTag cbTag;
			if (p->PeekPacketTag(cbTag))
				bouncedCreditBytes = cbTag.GetCreditBytes();
			forwardIdx = static_cast<int>(inDevHint);
			MarkPacketAsBouncedCredit(p);
			CbJustBouncedTag justBouncedTag;
			justBouncedTag.Set(true);
			p->AddPacketTag(justBouncedTag);
			m_traceCreditBouncerBounce(static_cast<uint32_t>(inDevHint), static_cast<uint32_t>(idx), bouncedCreditBytes);
			// Bounced credit must travel receiver-ward. Swap tuple direction on the
			// packet so downstream routing/host lookup matches REQUEST/GRANT_REQ keys.
			if (ch.l3Prot == 0xFB) {
				PppHeader ppp;
				Ipv4Header ipHeader;
				qbbHeader qbb;
				p->RemoveHeader(ppp);
				p->RemoveHeader(ipHeader);
				p->RemoveHeader(qbb);
				uint32_t oldSip = ipHeader.GetSource().Get();
				uint32_t oldDip = ipHeader.GetDestination().Get();
				uint16_t oldSport = qbb.GetSport();
				uint16_t oldDport = qbb.GetDport();
				ipHeader.SetSource(Ipv4Address(oldDip));
				ipHeader.SetDestination(Ipv4Address(oldSip));
				qbb.SetSport(oldDport);
				qbb.SetDport(oldSport);
				p->AddHeader(qbb);
				p->AddHeader(ipHeader);
				p->AddHeader(ppp);
				ch.sip = oldDip;
				ch.dip = oldSip;
				ch.ack.sport = oldDport;
				ch.ack.dport = oldSport;
			}
			LogCBPortEvent("bounce_redirect", static_cast<uint32_t>(idx), ch, p,
				qpBefore, qdBefore, qpBefore, qdBefore, 0, forwardIdx);
		} else if (m_creditbouncerCBEnable && IsCreditGrantPacket(p, ch) && inDevHint >= 0 && static_cast<uint32_t>(inDevHint) < pCnt) {
			CreditBouncerTag cbTag;
			if (p->PeekPacketTag(cbTag)) {
				uint64_t qpBefore = m_creditbouncerPortState[inDevHint].m_qpLenBytes;
				m_creditbouncerPortState[inDevHint].m_qpLenBytes += cbTag.GetCreditBytes();
				LogCBPortEvent("reserve_grant_credit", static_cast<uint32_t>(inDevHint), ch, p,
					qpBefore, m_creditbouncerPortState[inDevHint].m_qdLenBytes,
					m_creditbouncerPortState[inDevHint].m_qpLenBytes, m_creditbouncerPortState[inDevHint].m_qdLenBytes,
					cbTag.GetCreditBytes());
			}
		}

		NS_ASSERT_MSG(m_devices[forwardIdx]->IsLinkUp(), "The routing table look up should return link that is up");

		// determine the qIndex
		uint32_t qIndex;
		if (m_creditbouncerDedicatedMmuEnable) {
			qIndex = GetDedicatedCbQueueIndex(p, ch);
			if (m_creditbouncerStateLog && m_creditbouncerStateLogCount < m_creditbouncerStateMaxLogs) {
				CreditBouncerTag cbTag;
				std::cerr << "CBMMU_MAP node=" << GetId()
					<< " out_dev=" << forwardIdx
					<< " qidx=" << qIndex
					<< " l3=" << static_cast<uint32_t>(ch.l3Prot)
					<< " pkt_size=" << p->GetSize();
				if (p->PeekPacketTag(cbTag)) {
					std::cerr << " cb_type=" << CbMessageTypeToString(cbTag.GetType())
						<< " cb_data_class=" << CbDataClassToString(cbTag.GetDataClass());
				}
				std::cerr << "\n";
				m_creditbouncerStateLogCount++;
			}
		} else if (ch.l3Prot == 0xFF || ch.l3Prot == 0xFE || (m_ackHighPrio && (ch.l3Prot == 0xFD || ch.l3Prot == 0xFC))){  //QCN or PFC or NACK, go highest priority
			qIndex = 0;
		}else{
			if (ch.l3Prot == 0x06) { // TCP
				qIndex = 1;
			} else if (ch.l3Prot == 0xFB || ch.l3Prot == 0xFC || ch.l3Prot == 0xFD) { // CreditBouncer/ACK/NACK control headers
				qIndex = ch.ack.pg;
			} else {
				qIndex = ch.udp.pg;
			}
		}

		bool tlt_drop = false;

		// TLT
		TltTag tlt;
		if (!m_creditbouncerDedicatedMmuEnable && m_mmu->m_tlt) {
			if (p->PeekPacketTag(tlt)) {
				if (tlt.GetType() == TltTag::PACKET_NOT_IMPORTANT) {
					if (!m_mmu->CheckEgressTLT(idx, qIndex, p->GetSize())) {
						tlt_drop = true;
						stat_tx.txTltDropBytes += p->GetSize();
					}
				}
			}
		}

		// admission control
		FlowIdTag t;
		bool hasFlowTag = p->PeekPacketTag(t);
		uint32_t inDev = hasFlowTag ? t.GetFlowId() : inDevHint;
		if (inDev >= pCnt)
			inDev = inDevHint;
		if (m_creditbouncerDedicatedMmuEnable) {
			SyncDedicatedCbMmuConfig();
			bool admitted = false;
				if (qIndex == 0) {
					admitted = m_cbMmu->CheckCreditAdmission(forwardIdx, p->GetSize());
					if (admitted) {
						m_cbMmu->UpdateCreditAdmission(forwardIdx, p->GetSize());
					}
			} else {
				admitted = m_cbMmu->CheckDataAdmission(forwardIdx, p->GetSize());
				if (admitted) {
					m_cbMmu->UpdateDataAdmission(forwardIdx, p->GetSize());
				}
				}
							if (!admitted) {
								emitCbPacketEvent(static_cast<uint32_t>(forwardIdx), true);
								if (qIndex == 0) {
									// Always print a dedicated signal for priority credit-queue drops.
									std::cerr << "CBMMU_CREDIT_DROP node=" << GetId()
										<< " out_dev=" << forwardIdx
										<< " qidx=" << qIndex
										<< " l3=" << static_cast<uint32_t>(ch.l3Prot)
										<< " cb_type=" << CbControlPacketTypeForLog(p, ch)
										<< " pkt_size=" << p->GetSize()
										<< " reason=credit_queue_full_or_admission_failed\n";
								}
								if (m_creditbouncerStateLog && m_creditbouncerStateLogCount < m_creditbouncerStateMaxLogs) {
									std::cerr << "CBMMU_DROP node=" << GetId()
										<< " out_dev=" << forwardIdx
										<< " qidx=" << qIndex
										<< " l3=" << static_cast<uint32_t>(ch.l3Prot)
										<< " cb_type=" << CbControlPacketTypeForLog(p, ch)
										<< " pkt_size=" << p->GetSize()
										<< " reason=admission_failed\n";
									m_creditbouncerStateLogCount++;
								}
					return;
				}
			} else if (qIndex != 0){ //not highest priority
			if (!tlt_drop && m_mmu->CheckIngressAdmission(inDev, qIndex, p->GetSize()) && m_mmu->CheckEgressAdmission(forwardIdx, qIndex, p->GetSize())){			// Admission control
				m_mmu->UpdateIngressAdmission(inDev, qIndex, p->GetSize());
				m_mmu->UpdateEgressAdmission(forwardIdx, qIndex, p->GetSize());
				if (m_mmu->m_tlt) {
					if (tlt.GetType() == TltTag::PACKET_NOT_IMPORTANT) {
						m_mmu->UpdateIngressTLT(inDev, qIndex, p->GetSize());
						m_mmu->UpdateEgressTLT(forwardIdx, qIndex, p->GetSize());
						stat_tx.txUimpBytes += p->GetSize();
					} else if (tlt.GetType() == TltTag::PACKET_IMPORTANT || tlt.GetType() == TltTag::PACKET_IMPORTANT_FORCE) {
						stat_tx.txImpBytes += p->GetSize();
					} else if (tlt.GetType() == TltTag::PACKET_IMPORTANT_ECHO || tlt.GetType() == TltTag::PACKET_IMPORTANT_ECHO_FORCE) {
						stat_tx.txImpEBytes += p->GetSize();
					} else {
						stat_tx.txUimpBytes += p->GetSize();
					}
				}
			} else if (!tlt_drop) {
				emitCbPacketEvent(static_cast<uint32_t>(forwardIdx), true);
				if(m_mmu->m_tlt && tlt.GetType() != TltTag::PACKET_NOT_IMPORTANT) {
					stat_tx.importantDropBytes += p->GetSize();
					stat_tx.importantDropCnt += 1;
					std::cerr << "Warning: Important Packet has been dropped" << std::endl;
				}
				return;
			} else {
				emitCbPacketEvent(static_cast<uint32_t>(forwardIdx), true);
				return; // Drop
			}
			CheckAndSendPfc(inDev, qIndex);
		}
		emitCbPacketEvent(static_cast<uint32_t>(forwardIdx), false);
		if (m_creditbouncerCBEnable && forwardIdx >= 0 && static_cast<uint32_t>(forwardIdx) < pCnt && IsDataPacket(ch)) {
			uint64_t qdBefore = m_creditbouncerPortState[forwardIdx].m_qdLenBytes;
			m_creditbouncerPortState[forwardIdx].m_qdLenBytes += p->GetSize();
			LogCBPortEvent("enqueue_data_physical", static_cast<uint32_t>(forwardIdx), ch, p,
				m_creditbouncerPortState[forwardIdx].m_qpLenBytes, qdBefore,
				m_creditbouncerPortState[forwardIdx].m_qpLenBytes, m_creditbouncerPortState[forwardIdx].m_qdLenBytes,
				p->GetSize(), inDev);
		}
		m_bytes[inDev][forwardIdx][qIndex] += p->GetSize();
		m_devices[forwardIdx]->SwitchSend(qIndex, p, ch);
	}else
		return; // Drop
}

uint32_t SwitchNode::EcmpHash(const uint8_t* key, size_t len, uint32_t seed) {
  uint32_t h = seed;
  if (len > 3) {
    const uint32_t* key_x4 = (const uint32_t*) key;
    size_t i = len >> 2;
    do {
      uint32_t k = *key_x4++;
      k *= 0xcc9e2d51;
      k = (k << 15) | (k >> 17);
      k *= 0x1b873593;
      h ^= k;
      h = (h << 13) | (h >> 19);
      h += (h << 2) + 0xe6546b64;
    } while (--i);
    key = (const uint8_t*) key_x4;
  }
  if (len & 3) {
    size_t i = len & 3;
    uint32_t k = 0;
    key = &key[i - 1];
    do {
      k <<= 8;
      k |= *key--;
    } while (--i);
    k *= 0xcc9e2d51;
    k = (k << 15) | (k >> 17);
    k *= 0x1b873593;
    h ^= k;
  }
  h ^= len;
  h ^= h >> 16;
  h *= 0x85ebca6b;
  h ^= h >> 13;
  h *= 0xc2b2ae35;
  h ^= h >> 16;
  return h;
}

void SwitchNode::SetEcmpSeed(uint32_t seed){
	m_ecmpSeed = seed;
}

void SwitchNode::AddTableEntry(Ipv4Address &dstAddr, uint32_t intf_idx){
	uint32_t dip = dstAddr.Get();
	auto &ports = m_rtTable[dip];
	if (std::find(ports.begin(), ports.end(), static_cast<int>(intf_idx)) == ports.end())
		ports.push_back(intf_idx);
	std::sort(ports.begin(), ports.end());
}

void SwitchNode::ClearTable(){
	m_rtTable.clear();
}

// This function can only be called in switch mode
bool SwitchNode::SwitchReceiveFromDevice(Ptr<NetDevice> device, Ptr<Packet> packet, CustomHeader &ch){
	uint32_t inDev = device != 0 ? device->GetIfIndex() : 0;
	SendToDev(packet, ch, inDev);
	return true;
}

void SwitchNode::SwitchNotifyDequeue(uint32_t ifIndex, uint32_t qIndex, Ptr<Packet> p){
	CustomHeader ch(CustomHeader::L2_Header | CustomHeader::L3_Header | CustomHeader::L4_Header);
	p->PeekHeader(ch);
	FlowIdTag t;
	bool hasFlowTag = p->PeekPacketTag(t);
		if (m_creditbouncerDedicatedMmuEnable) {
			uint32_t inDev = hasFlowTag ? t.GetFlowId() : ifIndex;
			if (inDev >= pCnt)
				inDev = ifIndex;
		if (m_bytes[inDev][ifIndex][qIndex] >= p->GetSize())
			m_bytes[inDev][ifIndex][qIndex] -= p->GetSize();
		else
			m_bytes[inDev][ifIndex][qIndex] = 0;

			if (qIndex == 0) {
				m_cbMmu->RemoveCreditAdmission(ifIndex, p->GetSize());
			} else {
				m_cbMmu->RemoveDataAdmission(ifIndex, p->GetSize());
				if (m_creditbouncerCBEnable && ifIndex < pCnt && IsDataPacket(ch)) {
					uint64_t qdBefore = m_creditbouncerPortState[ifIndex].m_qdLenBytes;
					if (m_creditbouncerPortState[ifIndex].m_qdLenBytes >= p->GetSize())
						m_creditbouncerPortState[ifIndex].m_qdLenBytes -= p->GetSize();
					else
						m_creditbouncerPortState[ifIndex].m_qdLenBytes = 0;
					LogCBPortEvent("dequeue_data_physical", ifIndex, ch, p,
						m_creditbouncerPortState[ifIndex].m_qpLenBytes, qdBefore,
						m_creditbouncerPortState[ifIndex].m_qpLenBytes, m_creditbouncerPortState[ifIndex].m_qdLenBytes,
						p->GetSize(), inDev);
				}
				// DPDQ ECN marking is handled centrally in UpdateCBPortStateOnTransmit()
				// using the CreditBouncer XECN threshold and max(qd + qc, qp). The dedicated
				// MMU still owns admission/accounting, but it no longer marks packets.
		}
	} else if (qIndex != 0){
		uint32_t inDev = hasFlowTag ? t.GetFlowId() : ifIndex;
		if (inDev >= pCnt)
			inDev = ifIndex;
		m_mmu->RemoveFromIngressAdmission(inDev, qIndex, p->GetSize());
		m_mmu->RemoveFromEgressAdmission(ifIndex, qIndex, p->GetSize());
		{
			TltTag tlt;
			if (m_mmu->m_tlt && p->PeekPacketTag(tlt) && tlt.GetType() == TltTag::PACKET_NOT_IMPORTANT) {
				m_mmu->RemoveFromIngressTLT(inDev, qIndex, p->GetSize());
				m_mmu->RemoveFromEgressTLT(ifIndex, qIndex, p->GetSize());
			}
		}
		m_bytes[inDev][ifIndex][qIndex] -= p->GetSize();
		if (m_creditbouncerCBEnable && ifIndex < pCnt && IsDataPacket(ch)) {
			uint64_t qdBefore = m_creditbouncerPortState[ifIndex].m_qdLenBytes;
			if (m_creditbouncerPortState[ifIndex].m_qdLenBytes >= p->GetSize())
				m_creditbouncerPortState[ifIndex].m_qdLenBytes -= p->GetSize();
			else
				m_creditbouncerPortState[ifIndex].m_qdLenBytes = 0;
			LogCBPortEvent("dequeue_data_physical", ifIndex, ch, p,
				m_creditbouncerPortState[ifIndex].m_qpLenBytes, qdBefore,
				m_creditbouncerPortState[ifIndex].m_qpLenBytes, m_creditbouncerPortState[ifIndex].m_qdLenBytes,
				p->GetSize(), inDev);
		}
		if (m_ecnEnabled){
			bool egressCongested = m_mmu->ShouldSendCN(ifIndex, qIndex);
			if (egressCongested){
				PppHeader ppp;
				Ipv4Header h;
				p->RemoveHeader(ppp);
				p->RemoveHeader(h);
				h.SetEcn((Ipv4Header::EcnType)0x03);
				p->AddHeader(h);
				p->AddHeader(ppp);
			}
		}
		//CheckAndSendPfc(inDev, qIndex);
		CheckAndSendResume(inDev, qIndex);
	}
	UpdateCBPortStateOnTransmit(ifIndex, p, ch);
	if (1){
		uint8_t* buf = p->GetBuffer();
		if (buf[PppHeader::GetStaticSize() + 9] == 0x11){ // udp packet
			IntHeader *ih = (IntHeader*)&buf[PppHeader::GetStaticSize() + 20 + 8 + 6]; // ppp, ip, udp, SeqTs, INT
			Ptr<QbbNetDevice> dev = DynamicCast<QbbNetDevice>(m_devices[ifIndex]);
			if (m_ccMode == 3){ // HPCC
				ih->PushHop(Simulator::Now().GetTimeStep(), m_txBytes[ifIndex], dev->GetQueue()->GetNBytesTotal(), dev->GetDataRate().GetBitRate());
			}
		}
	}
	m_txBytes[ifIndex] += p->GetSize();
}

} /* namespace ns3 */
