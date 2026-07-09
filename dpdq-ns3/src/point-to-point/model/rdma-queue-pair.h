#ifndef RDMA_QUEUE_PAIR_H
#define RDMA_QUEUE_PAIR_H

#include <ns3/object.h>
#include <ns3/packet.h>
#include <ns3/ipv4-address.h>
#include <ns3/data-rate.h>
#include <ns3/event-id.h>
#include <ns3/custom-header.h>
#include <ns3/int-header.h>
#include <vector>
#include <ns3/selective-packet-queue.h>

#include <climits>		/* for CHAR_BIT */

#define BITMASK(b) (1 << ((b) % CHAR_BIT))
#define BITSLOT(b) ((b) / CHAR_BIT)
#define BITSET(a, b) ((a)[BITSLOT(b)] |= BITMASK(b))
#define BITCLEAR(a, b) ((a)[BITSLOT(b)] &= ~BITMASK(b))
#define BITTEST(a, b) ((a)[BITSLOT(b)] & BITMASK(b))
#define BITNSLOTS(nb) ((nb + CHAR_BIT - 1) / CHAR_BIT)

#define ESTIMATED_MAX_FLOW_PER_HOST 9120

namespace ns3 {

enum CcMode {
	CC_MODE_DCQCN = 1,
	CC_MODE_HPCC = 3,
	CC_MODE_TIMELY = 7,
	CC_MODE_DCTCP = 8,
	CC_MODE_UNDEFINED = 0,
};
enum TltCcType
{
	CC_TYPE_RATE,
	CC_TYPE_DYNAMIC_WINDOW,
	CC_TYPE_STATIC_WINDOW
};

class IrnSackManager {
private:
    std::list<std::pair<uint32_t, uint32_t>> m_data;

public:
	int socketId {-1};

	IrnSackManager();
	IrnSackManager(int flow_id);
	void sack(uint32_t seq, uint32_t size); // put blocks
	size_t discardUpTo(uint32_t seq); // return number of blocks removed
	bool IsEmpty();
	bool blockExists(uint32_t seq, uint32_t size); // query if block exists inside SACK table
	bool peekFrontBlock(uint32_t *pseq, uint32_t *psize);
	
	friend std::ostream &operator<<(std::ostream &os, const IrnSackManager &im);
};


enum TltState { TLT_STATE_IDLE = 0, TLT_STATE_IMPORTANT, TLT_STATE_IMPORTANT_FORCE, TLT_STATE_SCHEDULED };

class RdmaQueuePair : public Object {
public:
	Time startTime;
	Ipv4Address sip, dip;
	uint16_t sport, dport;
	uint64_t m_size;
	uint64_t snd_nxt, snd_una; // next seq to send, the highest unacked seq
	uint16_t m_pg;
	uint16_t m_ipid;
	uint32_t m_win; // bound of on-the-fly packets
	uint64_t m_baseRtt; // base RTT of this qp
	DataRate m_max_rate; // max rate
	bool m_var_win; // variable window size
	Time m_nextAvail;	//< Soonest time of next send
	uint32_t wp; // current window of packets
	uint32_t lastPktSize;
	int32_t m_flow_id;
	Time m_timeout;

	/******************************
	 * runtime states
	 *****************************/
	DataRate m_rate;	//< Current rate
	struct {
		DataRate m_targetRate;	//< Target rate
		EventId m_eventUpdateAlpha;
		double m_alpha;
		bool m_alpha_cnp_arrived; // indicate if CNP arrived in the last slot
		bool m_first_cnp; // indicate if the current CNP is the first CNP
		EventId m_eventDecreaseRate;
		bool m_decrease_cnp_arrived; // indicate if CNP arrived in the last slot
		uint32_t m_rpTimeStage;
		uint32_t m_rpByteStage;
		uint64_t m_rpByteCounterBytes;
		uint64_t m_rpByteResetBytes;
		EventId m_rpTimer;
	} mlx;
	struct {
		uint32_t m_lastUpdateSeq;
		DataRate m_curRate;
		IntHop hop[IntHeader::maxHop];
		uint32_t keep[IntHeader::maxHop];
		uint32_t m_incStage;
		double m_lastGap;
		double u;
		struct {
			double u;
			DataRate Rc;
			uint32_t incStage;
		}hopState[IntHeader::maxHop];
	} hp;
	struct{
		uint32_t m_lastUpdateSeq;
		DataRate m_curRate;
		uint32_t m_incStage;
		uint64_t lastRtt;
		double rttDiff;
	} tmly;
	struct{
		uint32_t m_lastUpdateSeq;
		uint32_t m_caState;
		uint32_t m_highSeq; // when to exit cwr
		double m_alpha;
		uint32_t m_ecnCnt;
		uint32_t m_batchSizeOfAlpha;
	} dctcp;

	struct {
		bool m_enabled;
		uint32_t m_bdp; // m_irn_maxAck_
		uint32_t m_highest_ack; // m_irn_maxAck_
		uint32_t m_max_seq; // m_irn_maxSeq_
		Time m_rtoLow;
		Time m_rtoHigh;
		IrnSackManager m_sack;
		bool m_recovery;
		uint32_t m_recovery_seq;
	} irn;

		struct {
		bool m_enabled;
		
		// Window-based CC
		TltState m_sendState;
		TltCcType m_cc_type;
		uint32_t m_sendUnit;
		uint32_t m_highestImportantAck;
		std::list<std::pair<uint32_t, uint32_t>> m_forcetx_queue;
		uint64_t stat_uimp_forcegen;
		uint32_t stat_uimp_forcegen_cnt;
		
		Ptr<SelectivePacketQueue> m_tlt_unimportant_pkts_current_round;
		SelectivePacketQueue m_tlt_unimportant_pkts;
		Ptr<SelectivePacketQueue> m_tlt_unimportant_pkts_prev_round;
		bool m_sent_fin;

		// Rate-based CC
		uint64_t m_sent_pkt_count;
		bool m_first_retx;
		uint64_t m_last_marked_sent_pkt_count;
	} tlt;

		struct {
			// Whether CreditBouncer is enabled for this sender-side QP.
			bool m_enabled;
			// Whether initial classification into scheduled/unsolicited has been completed.
			bool m_classificationReady;
			// Whether this message still has a scheduled tail after the self-allocated part.
			bool m_hasScheduledPart;
			// Whether the scheduled portion has already been announced to the receiver.
			bool m_schedRequestAnnounced;
			// Remaining payload bytes that still require scheduled grants.
			uint64_t m_pendingScheduledBytes;
			// Total payload budget initially covered by unsolicited/self-allocated credit.
			uint64_t m_unsolicitedBytesBudget;
			// Payload bytes already sent using the unsolicited/self-allocated budget.
			uint64_t m_unsolicitedBytesSent;
			// Whether a standalone GRANT_REQ control packet has been sent for this message.
			bool m_standaloneGrantReqSent;
			// Whether the one-time unsolicited eligibility/self-allocation check has run.
			bool m_usnsolCheckDone;
			// True when the whole message depends on receiver grants; false once any self-allocation exists.
			bool m_isScheduled;
			// Whether any packet has already announced this message's demand to the receiver.
			bool m_sentAnnouncement;
			// Initial self-allocated credit carried by the first packet, measured in on-wire bytes.
			uint64_t m_selfAllocCreditBytes;
			// Initial self-allocated credit carried by the first packet, measured in payload bytes.
			uint64_t m_selfAllocCreditDataBytes;
			// Remaining payload bytes from the self-allocation that have not yet been consumed locally.
			int64_t m_remainingSelfAllocCreditDataBytes;
			// Number of credit request announcements sent by this QP.
			uint64_t m_statCreditReqPkts;
			// Number of grant packets received by this QP.
			uint64_t m_statCreditGrantPkts;
			// Number of scheduled data packets sent by this QP.
			uint64_t m_statScheduledDataPkts;
			// Number of fully unsolicited data packets sent by this QP.
			uint64_t m_statUnsolicitedDataPkts;
			// Number of partially unsolicited/partially scheduled data packets sent by this QP.
			uint64_t m_statUnscheduledDataPkts;
			// Number of scheduled data packets that carried the host congestion signal.
			uint64_t m_statCsnMarkedPkts;
			// One-shot marker set by the sender scheduler when the next REQUEST data packet
			// should carry the always-SRPT priority-flow bit.
			bool m_priorityFlowPending;
		} cb;

	struct {
		uint64_t txTotalPkts {0};
		uint64_t txTotalBytes {0};
	} stat;

	// Implement Timeout according to IB Spec Vol. 1 C9-139.
	// For an HCA requester using Reliable Connection service, to detect missing responses,
	// every Send queue is required to implement a Transport Timer to time outstanding requests.
	EventId m_retransmit;

	/***********
	 * methods
	 **********/
	static TypeId GetTypeId (void);
	RdmaQueuePair(uint16_t pg, Ipv4Address _sip, Ipv4Address _dip, uint16_t _sport, uint16_t _dport);
	void SetSize(uint64_t size);
	void SetWin(uint32_t win);
	void SetBaseRtt(uint64_t baseRtt);
	void SetVarWin(bool v);
	void SetFlowId(int32_t v);
	void SetTimeout(Time v);

	uint64_t GetBytesLeft();
	uint32_t GetHash(void);
	void Acknowledge(uint64_t ack);
	uint64_t GetOnTheFly();
	bool IsWinBound();
	uint64_t GetWin(); // window size calculated from m_rate
	bool IsFinished();
	inline bool IsFinishedConst() const {
		return snd_una >= m_size;
	}

	uint64_t HpGetCurWin();  // window size calculated from hp.m_curRate, used by HPCC

	inline uint32_t GetIrnBytesInFlight() const {
		// IRN do not consider SACKed segments for simplicity
		return irn.m_max_seq - irn.m_highest_ack;
	}

	Time GetRto(uint32_t mtu) {
		if (irn.m_enabled) {
			if (GetIrnBytesInFlight() > 3*mtu) {
				return irn.m_rtoHigh;
			}
			return irn.m_rtoLow;
		} else {
			return m_timeout;
		}
	}

	inline bool CanIrnTransmit(uint32_t mtu) const {
		uint64_t len_left = m_size >= snd_nxt ? m_size - snd_nxt : 0;

		return !irn.m_enabled || (GetIrnBytesInFlight() + ((len_left > mtu) ? mtu : len_left)) < irn.m_bdp || (irn.m_highest_ack + irn.m_bdp > snd_nxt);
	}

	bool TltForceTxReady();
};

class RdmaRxQueuePair : public Object { // Rx side queue pair
public:
	struct ECNAccount{
		uint16_t qIndex;
		uint8_t ecnbits;
		uint16_t qfb;
		uint16_t total;

		ECNAccount() { memset(this, 0, sizeof(ECNAccount));}
	};
	ECNAccount m_ecn_source;
	uint32_t sip, dip;
	uint16_t sport, dport;
	uint16_t m_ipid;
	uint32_t ReceiverNextExpectedSeq;
	Time m_nackTimer;
	int32_t m_milestone_rx;
	uint32_t m_lastNACK;
	EventId QcnTimerEvent; // if destroy this rxQp, remember to cancel this timer
	bool m_qcnPending;
	IrnSackManager m_irn_sack_;
	TltState m_tlt_recvState;
	int32_t m_flow_id;
		struct {
			// Whether CreditBouncer accounting is enabled for this receiver-side QP.
			bool m_enabled;
		// This message's remaining payload demand as tracked on the receiver.
		// Paper mapping (per-message): rem_i.
		uint64_t m_pendingDemandBytes;
		// Guard bit used so the first announcement is counted only once.
		bool m_announcementAccounted;
		// The expected message size remembered from the latest accepted announcement.
		uint64_t m_announcementExpectedBytes;
			// Payload bytes received for this message, used for receiver-side completion detection.
			uint64_t m_receivedDataBytes;
			// Whether receiver-side completion has already been triggered for this message.
			bool m_completionNotified;
			// Number of chunks expected for this message using fixed MSS chunking.
			uint32_t m_expectedChunks;
			// Number of chunks already received at least once.
			uint32_t m_receivedChunks;
			// Number of chunks granted so far (exclusive frontier).
			uint32_t m_grantedChunkLimit;
			// Chunk-received bitmap (0/1 per chunk index).
			std::vector<uint8_t> m_receivedChunkBitmap;
			// Receiver-side resend timeout event for this message.
			EventId m_resendEvent;
		} cb;

	static TypeId GetTypeId (void);
	RdmaRxQueuePair();
	uint32_t GetHash(void);
};

class RdmaQueuePairGroup : public Object {
public:
	std::vector<Ptr<RdmaQueuePair> > m_qps;
	//std::vector<Ptr<RdmaRxQueuePair> > m_rxQps;
	char m_qp_finished[BITNSLOTS(ESTIMATED_MAX_FLOW_PER_HOST)];

	static TypeId GetTypeId (void);
	RdmaQueuePairGroup(void);
	uint32_t GetN(void);
	Ptr<RdmaQueuePair> Get(uint32_t idx);
	Ptr<RdmaQueuePair> operator[](uint32_t idx);
	void AddQp(Ptr<RdmaQueuePair> qp);
	//void AddRxQp(Ptr<RdmaRxQueuePair> rxQp);
	void Clear(void);
	inline bool IsQpFinished(uint32_t idx) {
		if (__glibc_unlikely(idx >= ESTIMATED_MAX_FLOW_PER_HOST))
			return false;
		return BITTEST(m_qp_finished, idx);
	}

	inline void SetQpFinished(uint32_t idx) {
		if (__glibc_unlikely(idx >= ESTIMATED_MAX_FLOW_PER_HOST))
			return;
		BITSET(m_qp_finished, idx);
	}
};

}

#endif /* RDMA_QUEUE_PAIR_H */
