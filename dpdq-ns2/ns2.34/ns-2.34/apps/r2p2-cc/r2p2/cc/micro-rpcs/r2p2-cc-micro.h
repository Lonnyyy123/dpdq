#ifndef ns_r2p2_cc_micro_h
#define ns_r2p2_cc_micro_h

#include <list>
#include <string>
#include <unordered_map>

#include "r2p2-hdr.h"
#include "packet.h"
#include "flags.h"
#include "r2p2-cc.h"
#include "r2p2-leaky-bucket.h"
#include "uplink-queue.h"
#include "r2p2-router.h"
#include "r2p2-hybrid-support.h"

#define POLL_INTERVAL 3 / 1000.0 / 1000.0 / 1000.0
#define UPDATE_GAP 10

/**
 * Base class for the micro-RPC idea. RPCs are split into smaller chunks (uRPCs)
 * and routed independently in the core. They are the unit of scheduling too.
 * These classes make sure thet each uRPC (from the same RPC) produces a different hash
 * as far as multipath is concerned.
 */

class R2p2LeakyBucket;
class R2p2CCMicroIdle;
class R2p2CCMicro;

/* A timer that fires with frequency similar to that of a real polling thread */
class PollTimer : public TimerHandler
{
public:
    PollTimer(R2p2CCMicro *cc_module) : cc_module_(cc_module) {}

protected:
    void expire(Event *);
    R2p2CCMicro *cc_module_;
};

/**
 * Describes R2p2CCMicro and classes extending it.
 * These classes implement the idea of spliting RPCs int "micro-RPCs" that are
 * scheduled independently.
 */
class R2p2CCMicro : public R2p2CCGeneric
{
    friend class R2p2LeakyBucket;
    friend class R2p2UplinkQueue;

public:
    R2p2CCMicro();
    R2p2CCMicro(R2p2UplinkQueue *uplink_queue_, PollTimer *poll_timer);
    virtual ~R2p2CCMicro();

    virtual void recv(Packet *pkt, Handler *h);
    virtual void send_to_transport(hdr_r2p2 &, int nbytes, int32_t daddr);
    /* Simulates the polling that occurs on a real system */
    virtual void poll();

protected:
    struct OutboundMsgState
    {
        uniq_req_id_t req_id_;
        int bytes_left_;   // bytes that are not granted
        int pkts_left_;    // Using packets because: the first pkt of a msg carries the number of pkts
                           // the message will occupy for transmission. This is what the r2p2 layer above uses
                           // to know if a message has been received (and not bytes).
                           // Therefore, a an RPC that does not perfectly fit in N pkts must be
                           // split into uRPCs such that the first K uRPCs consist of MSS byte pkts and
                           // uRPC K+1 can have a non MSS byte pkt.
        int pkts_granted_; // pkts that are granted and pending the transmission of a grant
        int msg_active_;
        int32_t daddr_;
        hdr_r2p2 *r2p2_hdr_;
    };

    struct InboundMsgState
    {
        InboundMsgState() : pkts_expected_(0),
                            data_pkts_received_(0),
                            pkts_uncredited_(0),
                            pkts_received_(0),
                            pkts_marked_(0),
                            marked_ratio_estim_(0.0),
                            grants_sent_(0) {}
        uniq_req_id_t req_id_;
        int pkts_expected_;
        int data_pkts_received_;
        int pkts_uncredited_;
        int pkts_received_;
        int pkts_marked_;
        double marked_ratio_estim_;
        int grants_sent_;
    };

    virtual void forward_to_transport(packet_info_t, MsgTracerLogs &&logs);
    virtual void send_reqrdy(hdr_r2p2 &r2p2_hdr, int reqrdy_bytes, int32_t daddr);
    virtual void send_grant_req(hdr_r2p2 *const r2p2_hdr, int32_t daddr);
    virtual void forward_reqrdy(hdr_r2p2 &r2p2_hdr, int reqrdy_bytes, int32_t daddr);
    virtual void handle_grant(hdr_r2p2 *const r2p2_hdr);
    virtual void handle_grant_request(hdr_r2p2 *const r2p2_hdr);

    virtual void update_outbound();
    virtual void forward_granted();
    virtual void activate_msgs();

    virtual void send_grant(hdr_r2p2 *const r2p2_hdr, int32_t daddr);
    virtual void forward_grant(hdr_r2p2 &r2p2_hdr, int32_t daddr);
    virtual void packet_received(Packet *const pkt);

    /**
     * to be called to decide the amount of credit a receiver should get given the circumstaces
     * Returns credit in bytes
     */
    virtual int calc_credit(hdr_r2p2 &r2p2_hdr, InboundMsgState *ims);

    // oms -> OutboundMsgState
    virtual OutboundMsgState *find_oms(const uniq_req_id_t &req_id);
    virtual InboundMsgState *find_ims(const uniq_req_id_t &req_id);

    inline double bytes_to_seconds(int bytes, double link_speed_bps)
    {
        return ((double)bytes * 8.0) / link_speed_bps;
    }

    inline double seconds_to_bytes(double seconds, double link_speed_bps)
    {
        return (seconds * link_speed_bps) / 8.0;
    }

    // Checks if the packet (with the provided header) is a single packet request or reply
    inline bool is_single_packet_msg(hdr_r2p2 *const r2p2_hdr)
    {
        if (r2p2_hdr->msg_type() == hdr_r2p2::REQUEST)
        {
            // in REQ0s, pkt_id carries the number of remaining request packets (i.e. excluding REQ0)
            // In the first packet of replies, pkt_id carries the total number of reply packets
            if (r2p2_hdr->first() && r2p2_hdr->pkt_id() == 0)
            {
                return true;
            }
        }
        else if (r2p2_hdr->msg_type() == hdr_r2p2::REPLY)
        {
            if (r2p2_hdr->first() && r2p2_hdr->pkt_id() == 1)
            {
                return true;
            }
        }
        else
        {
            throw std::invalid_argument(std::string("Cannot determine whether the message is single-packet. Invalid message type: ") + std::to_string(r2p2_hdr->msg_type()));
        }
        return false;
    }

    inline bool is_req0(hdr_r2p2 *const r2p2_hdr)
    {
        return (r2p2_hdr->msg_type() == hdr_r2p2::REQUEST && r2p2_hdr->first());
    }

    inline bool is_req0_of_multipkt(hdr_r2p2 *const r2p2_hdr)
    {
        return (is_req0(r2p2_hdr) && r2p2_hdr->pkt_id() >= 1);
    }

    inline bool is_from_multipkt_req_not_req0(hdr_r2p2 *const r2p2_hdr)
    {
        return (r2p2_hdr->msg_type() == hdr_r2p2::REQUEST && !r2p2_hdr->first());
    }

    inline bool is_from_multipkt_reply(hdr_r2p2 *const r2p2_hdr)
    {
        if (r2p2_hdr->msg_type() == hdr_r2p2::REPLY)
        {
            if (r2p2_hdr->first() && r2p2_hdr->pkt_id() != 1)
                return true;
            if (!r2p2_hdr->first())
                return true;
        }
        return false;
    }

    virtual void trace_state(std::string event, double a_double);
    virtual bool deduce_link_state(Packet *const pkt);
    virtual void update_global_ce_ratio(double new_event);
    virtual int command(int argc, const char *const *argv);
    std::list<OutboundMsgState *> outbound_msgs_;
    std::list<InboundMsgState *> inbound_msgs_;
    int urpc_sz_bytes_;
    int single_path_per_msg_; // if 1: all uRPCs follow the same path in core.
    int pace_uplink_;
    R2p2UplinkQueue *uplink_queue_;
    PollTimer *poll_timer_;
    double poll_interval_; // How frequently hosts run their logic
    double last_pkt_arival_;
    double last_pkt_sent_;
    bool link_busy_;
    double last_link_busy_;
    double last_link_idle_;
    bool poll_started_;
    long link_idle_when_pending_work_cnt_;
    uint64_t num_polls_;
    int uplink_deque_policy_;
    long umsg_cnt_;
    int ecn_capable_;
    double link_speed_gbps_;
    double global_marked_ratio_; // updated by the same algo as the on for individual msgs but takes all (appropriate?) packets into account
    double ce_new_weight_;
    double ecn_mechanism_influence_;
    double ecn_init_slash_mul_;
    double ecn_min_mul_;
    R2p2Router *r2p2_router_; // hack
};

class R2p2CCHybrid : public R2p2CCMicro
{
    friend class R2p2Agent;

public:
    R2p2CCHybrid();
    virtual ~R2p2CCHybrid();
    void send_to_transport(hdr_r2p2 &, int nbytes, int32_t daddr) override;
    void recv(Packet *pkt, Handler *h) override;
    void poll() override;
    int command(int argc, const char *const *argv) override;

protected:
    virtual void init();
    virtual void shorting_req0(hdr_r2p2 &r2p2_hdr, int payload, int32_t daddr);
    virtual void sending_request(hdr_r2p2 &r2p2_hdr, int payload, int32_t daddr);
    virtual void sending_reply(hdr_r2p2 &r2p2_hdr, int payload, int32_t daddr);
    virtual void prep_msg_send(hdr_r2p2 &r2p2_hdr, int payload, int32_t daddr);
    virtual hysup::SenderState *update_sender_state(Packet *pkt);
    virtual void update_nw_state(Packet *pkt, hysup::SenderState *state); // Update SBmax for network
    virtual void update_sn_state(Packet *pkt, hysup::SenderState *state); // Update SBmax for sender
    virtual hysup::ReceiverState *update_receiver_state(Packet *pkt);
    virtual void received_credit(Packet *pkt);
    virtual bool received_data(Packet *pkt, hysup::InboundMsgState *msg_state);
    virtual void poll_trace();
    virtual void send_grants();
    virtual void send_grants_ts_msg();
    // virtual void send_grants_ts_fifo_sender();
    // virtual void send_grants_fifo_msg();
    virtual void send_grants_srpt_msg();
    virtual void send_grants_equal_bif_msg();
    virtual int send_credit_policy_common(hysup::InboundMsgState *msg_state);
    virtual void forward_grant(hysup::InboundMsgState *msg_state, int credit_bytes, int padding);
    virtual void send_data();
    virtual void handle_resend(Packet *pkt);
    virtual void check_resend_timeouts();
    virtual void send_resend(hysup::InboundMsgState *msg_state, uint32_t start_offset, uint32_t resend_bytes);
    virtual bool send_one_retransmission();
    virtual void cleanup_finished_messages();
    virtual void queue_resend(hysup::OutboundMsgState *msg_state, uint32_t start_offset, uint32_t resend_bytes);
    virtual void ensure_inbound_tracking(hysup::InboundMsgState *msg_state);
    virtual uint32_t payload_from_offset(uint32_t total_bytes, uint32_t start_offset) const;
    virtual void update_grant_backlog();
    virtual void update_data_backlog();
    virtual void per_loop_stats();
    void trace_state(std::string event, double a_double) override;

    hysup::OutboundMsgs *outbound_inactive_; // messages for which no credit has ever arrived
    hysup::InboundMsgs *inbound_;
    std::unordered_map<int32_t, hysup::SenderState *> sender_state_; // FIXME: should be encapuslated in own class like receivers_
    hysup::Receivers *receivers_;
    int budget_bytes_;
    int max_budget_bytes_; // for debugging
    int max_srpb_;
    int nw_min_srpb_;
    int host_min_srpb_;
    int unsolicited_thresh_bytes_; // -1 means all messages are have an unsolicited part (none is completely scheduled)
    int unsolicited_limit_senders_;
    int unsolicited_burst_when_idle_;
    int priority_flow_;
    int data_prio_;
    int sender_mark_aimd_;
    double sender_policy_ratio_;
    int receiver_policy_; // 0: TS among msgs, 1: TS among senders, 2: FIFO among msgs
    int account_unsched_;
    // start grant pacer
    int grant_pacer_backlog_;
    double last_grant_pacer_update_;
    int grant_bytes_drained_per_loop_;
    double grant_pacer_speed_gbps_;
    // end grant pacer
    // start data pacer // TODO: Should create a standalone class at this point
    int data_pacer_backlog_;
    double last_data_pacer_update_;
    int data_bytes_drained_per_loop_;
    double data_pacer_speed_gbps_;
    // end data pacer
    int pace_grants_;
    int additive_incr_mul_;
    int sender_policy_;
    double rtt_s_;
    double reset_after_x_rtt_;
    int budget_backlog_bytes_;
    // Sender Algorithm Variables - to be accessed by r2p2-udp.cc
    // common
    uint32_t get_granted_bytes_queue_len();
    uint32_t marked_backlog_;
    uint32_t max_marked_backlog_;
    bool actually_mark(hdr_r2p2 &r2p2_hdr, bool current_decision);
    uint32_t get_outbound_vq_len(int32_t daddr);
    // DCTCP
    uint32_t ecn_thresh_pkts_;
    // HPCC
    uint64_t total_bytes_sent_;

    double last_pkt_received_ts_;
    double last_poll_trace_;
    double ecn_min_mul_nw_;
    int sender_algo_;
    double eta_;
    uint32_t wai_;
    uint32_t max_stage_;

    /* Stats */
    double state_polling_ival_s_;
    hysup::Stats *stats;

    double last_grant_time_;
    double resend_timeout_s_;
    std::list<hysup::OutboundMsgState *> resend_pending_;
    std::string resend_timeout_log_file_;
};

#endif
