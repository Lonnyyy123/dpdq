#ifndef SWITCH_NODE_H
#define SWITCH_NODE_H

#include <unordered_map>
#include <ns3/node.h>
#include <ns3/traced-callback.h>
#include "qbb-net-device.h"
#include "switch-mmu.h"
#include "switch-cb-mmu.h"

namespace ns3 {
struct stat_tx_ {
	uint64_t txUimpBytes = 0;
	uint64_t txImpBytes = 0;
	uint64_t txImpEBytes = 0;
	uint64_t txUimpBytesNIC = 0;
	uint64_t txImpBytesNIC = 0;
	uint64_t txImpBytesNIC_PL = 0;
	uint64_t txImpBytesNIC_PLR = 0;
	uint64_t txImpBytesNIC_PLE = 0;
	uint64_t txImpBytesNIC_CNP = 0;
	uint64_t txImpBytesNIC_ACK = 0;
	uint64_t txImpBytesNIC_NACK = 0;
	uint64_t txImpEBytesNIC = 0;
	uint64_t txImpFBytesNIC = 0;
	uint64_t txImpEFBytesNIC = 0;
	uint64_t txImpCBytesNIC = 0;
	uint64_t txTltDropBytes = 0;
	uint64_t importantDropBytes = 0;
	uint64_t importantDropCnt = 0;
	uint64_t RetxTimeoutCnt = 0;
	uint64_t PauseSendCnt = 0;
	bool stat_print = false;
};

class Packet;

class SwitchNode : public Node{
	enum CreditBouncerFcState {
		CB_FCSTATE_NORMAL = 0,
		CB_FCSTATE_PAUSE = 1,
	};

	struct CreditBouncerPortState {
		// Algorithm-1 FC state on this output port.
		CreditBouncerFcState m_fcState;
		// Qp_len: in-flight/queued credit bytes accounted on this output port.
		uint64_t m_qpLenBytes;
		// Qd_len: in-flight/queued data bytes accounted on this output port.
		uint64_t m_qdLenBytes;
		// last_time used for queue-drain estimation.
		uint64_t m_lastUpdateTimeNs;
		// Port-local thresholds used by CB state machine.
		uint64_t m_xonBytes;
		uint64_t m_xoffBytes;
		uint64_t m_xecnBytes;

		CreditBouncerPortState()
			: m_fcState(CB_FCSTATE_NORMAL),
			  m_qpLenBytes(0),
			  m_qdLenBytes(0),
			  m_lastUpdateTimeNs(0),
			  m_xonBytes(0),
			  m_xoffBytes(0),
			  m_xecnBytes(0) {}
	};

	static const unsigned qCnt = 8;	// Number of queues/priorities used
	static const unsigned pCnt = 128; // port 0 is not used so + 1	// Number of ports used
	uint32_t m_ecmpSeed;
	std::unordered_map<uint32_t, std::vector<int> > m_rtTable; // map from ip address (u32) to possible ECMP port (index of dev)

	// monitor of PFC
	uint32_t m_bytes[pCnt][pCnt][qCnt]; // m_bytes[inDev][outDev][qidx] is the bytes from inDev enqueued for outDev at qidx
	
	uint64_t m_txBytes[pCnt]; // counter of tx bytes
	// Per-port CB state placeholders introduced in phase1.
	CreditBouncerPortState m_creditbouncerPortState[pCnt];

protected:
	bool m_ecnEnabled;
	uint32_t m_ccMode;

	uint32_t m_ackHighPrio; // set high priority for ACK/NACK
	// When true, CreditBouncer REQUEST/GRANT traffic uses a
	// direction-independent ECMP hash so the two directions stay on the same
	// switch path without forcing all control packets to do so.
	bool m_creditbouncerSymmetricRouting;
	// Whether to emit CreditBouncer route-selection logs at the switch.
	bool m_creditbouncerRouteLog;
	// Maximum number of route-selection logs emitted by this switch.
	uint32_t m_creditbouncerRouteMaxLogs;
	// Number of route-selection logs already emitted by this switch.
	uint32_t m_creditbouncerRouteLogCount;
	// Enable switch-side CB algorithm-1 behavior.
	bool m_creditbouncerCBEnable;
	// Enable switch-side CreditBouncer congestion-control logic (ECN marking).
	bool m_creditbouncerCcEnable;
	// Enable switch-side CreditBouncer flow-control logic (bounce/state machine).
	bool m_creditbouncerFcEnable;
	// CB thresholds in bytes.
	uint64_t m_creditbouncerXonBytes;
	uint64_t m_creditbouncerXoffBytes;
	uint64_t m_creditbouncerXecnBytes;
	// Enable switch-side dynamic threshold for Xon/Xoff (PFC-like).
	bool m_creditbouncerDynamicThreshold;
	// Alpha factor used by dynamic threshold.
	double m_creditbouncerDynamicAlpha;
	// Hysteresis offset for dynamic resume threshold.
	uint64_t m_creditbouncerDynamicOffDiffBytes;
	// Headroom between Xoff and Xon in dedicated CB FC mode.
	uint64_t m_creditbouncerDedicatedFcMarginBytes;
	// Fixed margin subtracted from the dynamic Xoff base in dedicated CB FC mode.
	uint64_t m_creditbouncerDedicatedXoffMarginBytes;
	// Emit switch-side CB state-machine logs.
	bool m_creditbouncerStateLog;
	// Max number of switch-side CB state-machine logs.
	uint32_t m_creditbouncerStateMaxLogs;
	// Number of switch-side CB state-machine logs emitted.
	uint32_t m_creditbouncerStateLogCount;
	// Trace source for bounced-credit events emitted by this switch.
	TracedCallback<uint32_t, uint32_t, uint64_t> m_traceCreditBouncerBounce;
	// Trace source for CB FC state transitions. args: outDev, oldState, newState.
	TracedCallback<uint32_t, uint32_t, uint32_t> m_traceCreditBouncerFcState;
	// Per-packet accounting event for CB credit/data traffic.
	// args: nodeId, outDev, sip, dip, sport, dport, classId(1=credit,2=data), isDrop(0/1).
	TracedCallback<uint32_t, uint32_t, uint32_t, uint32_t, uint16_t, uint16_t, uint32_t, uint32_t> m_traceCreditBouncerPacketEvent;
	// Use dedicated CB dual-queue MMU instead of the shared SwitchMmu path.
	bool m_creditbouncerDedicatedMmuEnable;
	// Fixed queue limit for CB control packets per egress port.
	uint32_t m_creditbouncerDedicatedCreditQueueBytes;
	// Legacy compatibility field retained for configs/logging; ignored by the
	// current dedicated DPDQ MMU admission rule.
	uint32_t m_creditbouncerDedicatedDataHeadroomBytes;
	// Legacy compatibility field retained for configs/logging; ignored by the
	// current dedicated DPDQ MMU admission rule.
	uint32_t m_creditbouncerDedicatedDataGuaranteeBytes;
	// ECN thresholds used by dedicated CB MMU on data queue bytes.
	uint32_t m_creditbouncerDedicatedDataEcnKminBytes;
	uint32_t m_creditbouncerDedicatedDataEcnKmaxBytes;
	double m_creditbouncerDedicatedDataEcnPmax;

private:
	bool IsCreditBouncerControlPacket(Ptr<const Packet> p, const CustomHeader &ch) const;
	bool IsCreditGrantPacket(Ptr<const Packet> p, const CustomHeader &ch) const;
	bool IsBouncedCreditPacket(Ptr<const Packet> p, const CustomHeader &ch) const;
	bool IsSignalPacket(Ptr<const Packet> p, const CustomHeader &ch) const;
	bool IsUnscheduledDataPacket(Ptr<const Packet> p, const CustomHeader &ch) const;
	bool IsDataPacket(const CustomHeader &ch) const;
	uint32_t GetDedicatedCbQueueIndex(Ptr<const Packet> p, const CustomHeader &ch) const;
	const char *CbFcStateToString(CreditBouncerFcState state) const;
	void LogCBPortEvent(const char *event,
		uint32_t outDev,
		const CustomHeader &ch,
		Ptr<const Packet> p,
		uint64_t qpBefore,
		uint64_t qdBefore,
		uint64_t qpAfter,
		uint64_t qdAfter,
		uint64_t deltaBytes = 0,
		int32_t peerDev = -1);
	bool ShouldBounceCreditPacket(uint32_t inDev, uint32_t outDev, Ptr<const Packet> p, const CustomHeader &ch) const;
	void MarkPacketAsBouncedCredit(Ptr<Packet> p) const;
	void UpdateCBPortStateOnTransmit(uint32_t outDev, Ptr<Packet> p, CustomHeader &ch);
	void SyncDedicatedCbMmuConfig(void);
	int GetOutDev(Ptr<const Packet>, CustomHeader &ch);
	void SendToDev(Ptr<Packet>p, CustomHeader &ch, uint32_t inDevHint);
	static uint32_t EcmpHash(const uint8_t* key, size_t len, uint32_t seed);
	static uint32_t BuildCreditBouncerSymmetricHash(uint32_t sip, uint32_t dip, uint16_t sport, uint16_t dport, uint16_t pg, uint32_t seq);
	void CheckAndSendPfc(uint32_t inDev, uint32_t qIndex);
	void CheckAndSendResume(uint32_t inDev, uint32_t qIndex);
public:
	//Ptr<BroadcomNode> m_broadcom;
	Ptr<SwitchMmu> m_mmu;
	Ptr<SwitchCbMmu> m_cbMmu;

	static TypeId GetTypeId (void);
	SwitchNode();
	bool IsCreditBouncerDedicatedMmuEnabled() const {
		return m_creditbouncerDedicatedMmuEnable && m_cbMmu != 0;
	}
	uint32_t GetMonitoredEgressDataQueueBytes(uint32_t ifIndex) const {
		if (IsCreditBouncerDedicatedMmuEnabled())
			return m_cbMmu->GetDataQueueBytes(ifIndex);
		return m_mmu != 0 ? m_mmu->GetEgressDataQueueBytes(ifIndex) : 0;
	}
	uint32_t GetCreditBouncerPriorityQueueBytes(uint32_t ifIndex) const {
		return IsCreditBouncerDedicatedMmuEnabled() ? m_cbMmu->GetCreditQueueBytes(ifIndex) : 0;
	}
	uint64_t GetCreditBouncerPhantomQueueBytes(uint32_t ifIndex) const {
		return (m_creditbouncerCBEnable && ifIndex < pCnt) ? m_creditbouncerPortState[ifIndex].m_qpLenBytes : 0;
	}
	void SetEcmpSeed(uint32_t seed);
	void AddTableEntry(Ipv4Address &dstAddr, uint32_t intf_idx);
	void ClearTable();
	bool SwitchReceiveFromDevice(Ptr<NetDevice> device, Ptr<Packet> packet, CustomHeader &ch);
	void SwitchNotifyDequeue(uint32_t ifIndex, uint32_t qIndex, Ptr<Packet> p);
};

} /* namespace ns3 */

#endif /* SWITCH_NODE_H */
