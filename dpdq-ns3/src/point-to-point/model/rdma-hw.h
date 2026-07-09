#ifndef RDMA_HW_H
#define RDMA_HW_H

#include <ns3/rdma.h>
#include "rdma-queue-pair.h"
#include <ns3/node.h>
#include <ns3/custom-header.h>
#include <ns3/selective-packet-queue.h>
#include <ns3/traced-callback.h>
#include "qbb-net-device.h"
#include <unordered_map>
#include <cstdio>

namespace ns3 {

struct RdmaInterfaceMgr{
	Ptr<QbbNetDevice> dev;
	Ptr<RdmaQueuePairGroup> qpGrp;

	RdmaInterfaceMgr() : dev(NULL), qpGrp(NULL) {}
	RdmaInterfaceMgr(Ptr<QbbNetDevice> _dev){
		dev = _dev;
	}
};

class RdmaHw : public Object {
public:
	enum CreditBouncerSchedPolicy {
		CB_SCHED_RR = 0,
		CB_SCHED_SRPT = 1
	};

	static TypeId GetTypeId(void);
	RdmaHw();

	Ptr<Node> m_node;
	DataRate m_minRate;		//< Min sending rate
	uint32_t m_mtu;
	uint32_t m_cc_mode;
	double m_nack_interval;
	uint32_t m_chunk;
	uint32_t m_ack_interval;
	bool m_backto0;
	bool m_var_win, m_fast_react;
	bool m_rateBound;
	// When true, CreditBouncer REQUEST/GRANT traffic uses a
	// direction-independent ECMP hash so the two directions stay on the same
	// path without forcing all control packets to do so.
	bool m_creditbouncerSymmetricRouting;
	std::vector<RdmaInterfaceMgr> m_nic; // list of running nic controlled by this RdmaHw
	std::unordered_map<uint64_t, Ptr<RdmaQueuePair> > m_qpMap; // mapping from uint64_t to qp
	std::unordered_map<uint64_t, Ptr<RdmaRxQueuePair> > m_rxQpMap; // mapping from uint64_t to rx qp
	std::unordered_map<uint32_t, std::vector<int> > m_rtTable; // map from ip address (u32) to possible ECMP port (index of dev)

	// qp complete callback
	typedef Callback<void, Ptr<RdmaQueuePair> > QpCompleteCallback;
	QpCompleteCallback m_qpCompleteCallback;
	TracedCallback<Ptr<RdmaQueuePair>, bool, uint64_t> m_traceTimeoutEvent;
	TracedCallback<Ptr<RdmaRxQueuePair>, uint64_t> m_traceCreditBouncerResendTimeoutEvent;

	void SetNode(Ptr<Node> node);
	void Setup(QpCompleteCallback cb); // setup shared data and callbacks with the QbbNetDevice
	static uint64_t GetQpKey(uint16_t sport, uint16_t pg); // get the lookup key for m_qpMap
	Ptr<RdmaQueuePair> GetQp(uint16_t sport, uint16_t pg); // get the qp
	uint32_t GetNicIdxOfQp(Ptr<RdmaQueuePair> qp); // get the NIC index of the qp
	uint32_t GetCreditBouncerSymmetricHash(uint32_t sip, uint32_t dip, uint16_t sport, uint16_t dport, uint16_t pg) const;
	void AddQueuePair(uint64_t size, uint16_t pg, Ipv4Address _sip, Ipv4Address _dip, uint16_t _sport, uint16_t _dport, uint32_t win, uint64_t baseRtt, int32_t flow_id); // add a nw qp (new send)
	void AddQueuePair(uint64_t size, uint16_t pg, Ipv4Address _sip, Ipv4Address _dip, uint16_t _sport, uint16_t _dport, uint32_t win, uint64_t baseRtt) {
		this->AddQueuePair(size, pg, _sip, _dip, _sport, _dport, win, baseRtt, -1);
	} 
	
	Ptr<RdmaRxQueuePair> GetRxQp(uint32_t sip, uint32_t dip, uint16_t sport, uint16_t dport, uint16_t pg, bool create); // get a rxQp
	uint32_t GetNicIdxOfRxQp(Ptr<RdmaRxQueuePair> q, bool useSymmetricRouting = true); // get the NIC index of the rxQp

	// IPv4 protocol numbers used by this RDMA transport.
	static constexpr uint8_t RDMA_L3_PROT_UDP = 0x11;
	static constexpr uint8_t RDMA_L3_PROT_CNP = 0xFF;
	static constexpr uint8_t RDMA_L3_PROT_NACK = 0xFD;
	static constexpr uint8_t RDMA_L3_PROT_ACK = 0xFC;
	static constexpr uint8_t RDMA_L3_PROT_CREDITBOUNCER = 0xFB;

	int ReceiveUdp(Ptr<Packet> p, CustomHeader &ch);
	int ReceiveCnp(Ptr<Packet> p, CustomHeader &ch);
	int ReceiveAck(Ptr<Packet> p, CustomHeader &ch); // handle both ACK and NACK

	int ReceiveCreditBouncer(Ptr<Packet> p, CustomHeader &ch); // handle creditbouncer control packet
	void ApplyCreditAimd(uint64_t &limitBytes, bool marked, uint64_t minBytes, uint64_t maxBytes, double aiStepBytes, double mdFactor);

		// Sender-side state keyed by receiver (r2p2 ReceiverState semantics).
		struct ReceiverState {
			// Whether CreditBouncer accounting has been initialized for this receiver.
			bool m_enabled;
			// Credit in on-wire bytes.
			uint64_t m_availCreditBytes;
			// Credit in application payload bytes.
			uint64_t m_availCreditDataBytes;
			// Host-side congestion threshold: mark scheduled data when total local sched credit exceeds this.
			uint64_t m_senderBacklogThresholdBytes;
			// Small-message cutoff in on-wire bytes for unsolicited/self-allocated transmission.
			uint64_t m_unsolicitedThreshBytes;
			// Additive increase step applied to receiver-related limits.
			double m_aiStepBytes;
			// Multiplicative decrease factor applied to receiver-related limits.
			double m_mdFactor;
			// All local send QPs currently targeting this receiver.
			std::vector<Ptr<RdmaQueuePair> > m_msgStates;
			// Sender-side deficit used by mixed FS/SRPT scheduling.
			double m_schedDeficit;
			// Sender-side quantum used by mixed FS/SRPT scheduling.
			double m_schedQuantum;

			ReceiverState()
				: m_enabled(false),
				  m_availCreditBytes(0),
				  m_availCreditDataBytes(0),
				  m_senderBacklogThresholdBytes(0),
				  m_unsolicitedThreshBytes(0),
				  m_aiStepBytes(0.0),
				  m_mdFactor(0.0),
				  m_schedDeficit(0.0),
				  m_schedQuantum(0.0) {}
		};

		// Receiver-side state keyed by sender (r2p2 SenderState semantics).
		struct SenderState {
			// Whether CreditBouncer accounting has been initialized for this sender.
			bool m_enabled;
			// Remote sender IP address used as the key of this aggregate state.
			uint32_t m_senderAddr;
			// Payload bytes still waiting for grants across all messages from this sender.
			// Paper mapping (aggregate): rem_i.
			uint64_t m_pendingDemandDataBytes;
			// Payload bytes already granted but not yet received.
			uint64_t m_outstandingDataBytes;
			// Portion of the receiver-global bucket currently attributed to this sender.
			// Paper mapping (aggregate): sb_i.
			uint64_t m_OutstandingBytes;
			// Remaining sender-receiver pair budget in on-wire bytes.
			uint64_t m_srpbBytes;
			// Effective SRPB cap after combining network and host-side limits.
			uint64_t m_maxSrpbBytes;
			// Immutable per sender-receiver SRPB ceiling used as AIMD upper bound.
			uint64_t m_srpbCeilingBytes;
			// Network-ECN-controlled SRPB cap.
			uint64_t m_nwMaxSrpbBytes;
			// Host-mark-controlled SRPB cap.
			uint64_t m_hostMaxSrpbBytes;
			// Receiver-global outstanding-byte limit B.
			uint64_t m_globalBucketLimitBytes;
			// Target grant size in payload bytes for each grant issued to this sender.
			uint64_t m_grantGranularityDataBytes;
			// Current network-side AIMD budget limit before min(nw, host).
			// Paper mapping (aggregate): netBkt_i.
			uint64_t m_netBucketLimitSrpbBytes;
			// Current host-side AIMD budget limit before min(nw, host).
			uint64_t m_senderBucketLimitSrpbBytes;
			// Lower bound for the network-side AIMD budget.
			uint64_t m_netBucketMinBytes;
			// Lower bound for the host-side AIMD budget.
			uint64_t m_senderBucketMinBytes;
			// Additive increase step for both AIMD loops.
			double m_aiStepBytes;
			// Multiplicative decrease factor for both AIMD loops.
			double m_mdFactor;
			// Cached ECN outcome of the most recently received data packet.
			bool m_lastEcnMarked;
			// EWMA ratio of network-CE-marked packets.
			double m_nwMarkedRatio;
			// EWMA ratio of host-CSN-marked packets.
			double m_hostMarkedRatio;
			// Packets seen since the last ratio update.
			uint32_t m_pktsSinceLastRatioUpdate;
			// Packet budget before the next ratio update.
			uint32_t m_pktsAtNextRatioUpdate;
			// Network-CE-marked packets counted in the current ratio window.
			uint32_t m_pktsNwMarkedSinceLastRatioUpdate;
			// Host-CSN-marked packets counted in the current ratio window.
			uint32_t m_pktsHtMarkedSinceLastRatioUpdate;
			// Packets seen since last network-window decrease decision.
			uint32_t m_pktsNwSinceLastWndUpdate;
			// Packet budget before next network-window decrease decision.
			uint32_t m_pktsNwAtNextWndUpdate;
			// Packets seen since last host-window decrease decision.
			uint32_t m_pktsHtSinceLastWndUpdate;
			// Packet budget before next host-window decrease decision.
			uint32_t m_pktsHtAtNextWndUpdate;
			// Track whether the network-side signal is in an ECN burst.
			bool m_nwEcnBurst;
			// Track whether the host-side signal is in a CSN burst.
			bool m_hostEcnBurst;
			// All receiver-side rxQPs currently associated with this sender.
			std::vector<Ptr<RdmaRxQueuePair> > m_msgStates;

		SenderState()
			: m_enabled(false),
			  m_senderAddr(0),
			  m_pendingDemandDataBytes(0),
			  m_outstandingDataBytes(0),
			  m_OutstandingBytes(0),
			  m_srpbBytes(0),
			  m_maxSrpbBytes(0),
			  m_srpbCeilingBytes(0),
			  m_nwMaxSrpbBytes(0),
			  m_hostMaxSrpbBytes(0),
			  m_globalBucketLimitBytes(0),
			  m_grantGranularityDataBytes(0),
			  m_netBucketLimitSrpbBytes(0),
			  m_senderBucketLimitSrpbBytes(0),
			  m_netBucketMinBytes(0),
			  m_senderBucketMinBytes(0),
			  m_aiStepBytes(0.0),
			  m_mdFactor(0.0),
			  m_lastEcnMarked(false),
			  m_nwMarkedRatio(0.0),
			  m_hostMarkedRatio(0.0),
			  m_pktsSinceLastRatioUpdate(0),
			  m_pktsAtNextRatioUpdate(1),
			  m_pktsNwMarkedSinceLastRatioUpdate(0),
			  m_pktsHtMarkedSinceLastRatioUpdate(0),
			  m_pktsNwSinceLastWndUpdate(0),
			  m_pktsNwAtNextWndUpdate(1),
			  m_pktsHtSinceLastWndUpdate(0),
			  m_pktsHtAtNextWndUpdate(1),
			  m_nwEcnBurst(false),
			  m_hostEcnBurst(false)
		{}
			};

	ReceiverState &GetOrCreateReceiverState(uint32_t receiverAddr, Ptr<RdmaQueuePair> msgState = NULL);
	SenderState &GetOrCreateSenderState(uint32_t senderAddr, Ptr<RdmaRxQueuePair> msgState = NULL);
	void CleanupCreditBouncerReceiverState(Ptr<RdmaQueuePair> qp);
	void CleanupCreditBouncerSenderState(Ptr<RdmaRxQueuePair> rxQp);
	int GetNextCreditBouncerQindex(Ptr<RdmaQueuePairGroup> qpGrp, bool paused[], uint32_t rrLast, uint32_t mtu);
	void UpdateSenderStateAimdOnData(SenderState &senderState, bool csnMarked, bool ecnMarked, bool priorityFlow);
	uint64_t GetHostSchedCreditAvailableBytes() const;
	bool TryIssueCreditGrant();
	void ScheduleCreditGrantTick();
	void CreditGrantTick();
	void ScheduleCreditBouncerResend(Ptr<RdmaRxQueuePair> rxQp);
	void HandleCreditBouncerResendTimeout(Ptr<RdmaRxQueuePair> rxQp);
	void SendCreditBouncerResend(Ptr<RdmaRxQueuePair> rxQp,
				     uint32_t startIdx,
				     uint32_t numChunks);
	
	int Receive(Ptr<Packet> p, CustomHeader &ch); // callback function that the QbbNetDevice should use when receive packets. Only NIC can call this function. And do not call this upon PFC

	void CheckandSendQCN(Ptr<RdmaRxQueuePair> q);
	int ReceiverCheckSeq(uint32_t seq, Ptr<RdmaRxQueuePair> q, uint32_t size);
	void AddHeader (Ptr<Packet> p, uint16_t protocolNumber);
	static uint16_t EtherToPpp (uint16_t protocol);

	void RecoverQueue(Ptr<RdmaQueuePair> qp);
	void CompleteCreditBouncerSenderQp(uint16_t sport, uint16_t pg, int32_t expectedFlowId);
	void QpComplete(Ptr<RdmaQueuePair> qp);
	void SetLinkDown(Ptr<QbbNetDevice> dev);

	// call this function after the NIC is setup
	void AddTableEntry(Ipv4Address &dstAddr, uint32_t intf_idx);
	void ClearTable();
	void RedistributeQp();

	Ptr<Packet> GetNxtPacket(Ptr<RdmaQueuePair> qp); // get next packet to send, inc snd_nxt
	void PktSent(Ptr<RdmaQueuePair> qp, Ptr<Packet> pkt, Time interframeGap);
	void UpdateNextAvail(Ptr<RdmaQueuePair> qp, Time interframeGap, uint32_t pkt_size);
	void ChangeRate(Ptr<RdmaQueuePair> qp, DataRate new_rate);

	void HandleTimeout(Ptr<RdmaQueuePair> qp, Time rto);
	void PrintStat(void);
	/******************************
	 * Mellanox's version of DCQCN
	 *****************************/
	double m_g; //feedback weight
	double m_rateOnFirstCNP; // the fraction of line rate to set on first CNP
	bool m_EcnClampTgtRate;
	double m_rpgTimeReset;
	uint64_t m_rpgByteReset;
	double m_rateDecreaseInterval;
	uint32_t m_rpgThreshold;
	double m_alpha_resume_interval;
	DataRate m_rai;		//< Rate of additive increase
	DataRate m_rhai;		//< Rate of hyper-additive increase

	// the Mellanox's version of alpha update:
	// every fixed time slot, update alpha.
	void UpdateAlphaMlx(Ptr<RdmaQueuePair> q);
	void ScheduleUpdateAlphaMlx(Ptr<RdmaQueuePair> q);

	// Mellanox's version of CNP receive
	void cnp_received_mlx(Ptr<RdmaQueuePair> q);

	// Mellanox's version of rate decrease
	// It checks every m_rateDecreaseInterval if CNP arrived (m_decrease_cnp_arrived).
	// If so, decrease rate, and reset all rate increase related things
	void CheckRateDecreaseMlx(Ptr<RdmaQueuePair> q);
	void ScheduleDecreaseRateMlx(Ptr<RdmaQueuePair> q, uint32_t delta);

	// Mellanox's version of rate increase
	void RateIncEventTimerMlx(Ptr<RdmaQueuePair> q);
	void RateIncEventMlx(Ptr<RdmaQueuePair> q, bool byTimer);
	void FastRecoveryMlx(Ptr<RdmaQueuePair> q);
	void ActiveIncreaseMlx(Ptr<RdmaQueuePair> q);
	void HyperIncreaseMlx(Ptr<RdmaQueuePair> q);
	void SendDcqcnCnp(Ptr<RdmaRxQueuePair> rxQp);
	void HandleDcqcnCnpTimer(Ptr<RdmaRxQueuePair> rxQp);
	void MaybeSendDcqcnCnp(Ptr<RdmaRxQueuePair> rxQp);

	// Implement Timeout according to IB Spec Vol. 1 C9-139.
	// For an HCA requester using Reliable Connection service, to detect missing responses,
	// every Send queue is required to implement a Transport Timer to time outstanding requests.
	Time m_waitAckTimeout;

	/***********************
	 * High Precision CC
	 ***********************/
	double m_targetUtil;
	double m_utilHigh;
	uint32_t m_miThresh;
	bool m_multipleRate;
	bool m_sampleFeedback; // only react to feedback every RTT, or qlen > 0
	void HandleAckHp(Ptr<RdmaQueuePair> qp, Ptr<Packet> p, CustomHeader &ch);
	void UpdateRateHp(Ptr<RdmaQueuePair> qp, Ptr<Packet> p, CustomHeader &ch, bool fast_react);
	void UpdateRateHpTest(Ptr<RdmaQueuePair> qp, Ptr<Packet> p, CustomHeader &ch, bool fast_react);
	void FastReactHp(Ptr<RdmaQueuePair> qp, Ptr<Packet> p, CustomHeader &ch);

	/**********************
	 * TIMELY
	 *********************/
	double m_tmly_alpha, m_tmly_beta;
	uint64_t m_tmly_TLow, m_tmly_THigh, m_tmly_minRtt;
	void HandleAckTimely(Ptr<RdmaQueuePair> qp, Ptr<Packet> p, CustomHeader &ch);
	void UpdateRateTimely(Ptr<RdmaQueuePair> qp, Ptr<Packet> p, CustomHeader &ch, bool us);
	void FastReactTimely(Ptr<RdmaQueuePair> qp, Ptr<Packet> p, CustomHeader &ch);

	/**********************
	 * DCTCP
	 *********************/
	DataRate m_dctcp_rai;
	void HandleAckDctcp(Ptr<RdmaQueuePair> qp, Ptr<Packet> p, CustomHeader &ch);

	
	/**********************
	 * IRN
	 *********************/
	bool m_irn;
	Time m_irn_rtoLow;
	Time m_irn_rtoHigh;
	uint32_t m_irn_bdp;

	
	/**********************
	 * TLT
	 *********************/
	bool m_tlt;
	uint32_t m_tlt_important_marking_interval;
	inline TltCcType GetCcType() const {
		if (m_cc_mode == CC_MODE_DCQCN && !m_irn)
			return CC_TYPE_RATE;
		if (m_cc_mode == CC_MODE_DCQCN && m_irn)
			return CC_TYPE_STATIC_WINDOW;
		if (m_cc_mode == CC_MODE_HPCC)
			return CC_TYPE_DYNAMIC_WINDOW;
		if (m_cc_mode == CC_MODE_TIMELY)
			return CC_TYPE_RATE;
		NS_ABORT_MSG("CC Type not supported by TLT");
		return CC_TYPE_RATE; // cannot reach here
	}
	inline bool IsWindowBasedCC() const {
		TltCcType c = GetCcType();
		return c == CC_TYPE_STATIC_WINDOW || c == CC_TYPE_DYNAMIC_WINDOW;
	}
	bool forceSendTLT(Ptr<RdmaQueuePair> qp, int *psize);
	void GenerateTltFin(Ptr<RdmaQueuePair> qp);

	/**********************
	 * CreditBouncer Host
	 *********************/
	// Global feature flag for CreditBouncer host-side logic.
	bool m_creditbouncer;
	// Enable CreditBouncer host-side CC logic (ECN/CSN-driven AIMD).
	bool m_creditbouncerCcEnable;
	// Enable CSN marking/carrying in CreditBouncer data packets.
	bool m_creditbouncerCsnEnable;
	// Small-message cutoff for unsolicited transmission, measured in on-wire bytes.
	uint32_t m_creditbouncer_unsolicitedThresh_bytes;
	// Per sender-receiver pair ceiling (SRPB) in on-wire bytes.
	uint32_t m_creditbouncer_maxSrpb_bytes;
	// Receiver-global outstanding budget B in on-wire bytes.
	uint32_t m_creditbouncer_globalBucket_bytes;
	// Host-side backlog threshold used to set the CSN/sender_marked signal.
	uint32_t m_creditbouncer_senderThreshold_bytes;
	// Grant quantum in payload bytes.
	uint32_t m_creditbouncer_grantGranularity_bytes;
	// Additive increase step for network and host-side CreditBouncer AIMD loops.
	double m_creditbouncer_aiStep_bytes;
	// Multiplicative decrease factor for network and host-side CreditBouncer AIMD loops.
	double m_creditbouncer_mdFactor;
	// EWMA weight for CE/CSN ratio updates.
	double m_creditbouncer_ceNewWeight;
	// Lower bound of network-side window as a fraction of SRPB ceiling.
	double m_creditbouncer_ecnMinMul_nw;
	// Lower bound of host-side window as a fraction of SRPB ceiling.
	double m_creditbouncer_ecnMinMul_host;
	// Period of the receiver-side grant pacer tick.
	uint32_t m_creditbouncer_grantIntervalNs;
	// Receiver-side resend timeout interval for missing granted chunks.
	uint32_t m_creditbouncer_resendIntervalNs;
	// Receiver-global outstanding on-wire bytes currently granted but not yet replenished.
	// Paper mapping: b can be viewed as free budget = (B - m_creditbouncer_globalOutstanding_bytes),
	// where B is m_creditbouncer_globalBucket_bytes.
	uint64_t m_creditbouncer_globalOutstanding_bytes;
	// Scheduled event for the receiver-side periodic grant pacer.
	EventId m_creditbouncer_grantTickEvent;
	// Whether to emit CreditBouncer debug logs.
	bool m_creditbouncer_debugLog;
	// Maximum number of debug log lines emitted per host.
	uint32_t m_creditbouncer_debugMaxLogs;
	// Number of debug log lines already emitted by this host.
	uint32_t m_creditbouncer_debugLogCount;
	// Sender-side scheduling policy used by GetNextCreditBouncerQindex.
	uint32_t m_creditbouncer_senderSchedPolicy;
	// Receiver-side grant selection policy used by TryIssueCreditGrant.
	uint32_t m_creditbouncer_grantSchedPolicy;
	// FS/SRPT mixing ratio for sender-side CreditBouncer scheduling.
	double m_creditbouncer_senderPolicyRatio;
	// RR cursor over grant candidates (receiver-side grant scheduler).
	uint32_t m_creditbouncer_grantRrCursor;
	// Number of local grant-pacer ticks executed by this host.
	uint64_t m_creditbouncer_tickCount;
	// Number of local grant-pacer ticks that actually issued a grant.
	uint64_t m_creditbouncer_tickGrantCount;
	// Global counter of grant-pacer ticks across all hosts.
	static uint64_t s_creditbouncer_tickCountGlobal;
	// Global counter of grant-pacer ticks that issued a grant across all hosts.
	static uint64_t s_creditbouncer_tickGrantCountGlobal;
	// Sender-side state indexed by receiver address.
	std::unordered_map<uint32_t, ReceiverState> m_receiverStates;
	// Receiver-side state indexed by sender address.
	std::unordered_map<uint32_t, SenderState> m_senderStates;
};

} /* namespace ns3 */

#endif /* RDMA_HW_H */
