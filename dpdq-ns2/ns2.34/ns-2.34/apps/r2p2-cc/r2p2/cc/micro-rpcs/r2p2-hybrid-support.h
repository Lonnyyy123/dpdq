#ifndef ns_r2p2_hynrid_support_h
#define ns_r2p2_hybrid_support_h

#include <map>
#include <vector>
#include <list>
#include <set>
#include <utility>
#include "r2p2-hdr.h"
#include "packet.h"
#include "flags.h"
#include "simple-log.h"
#include "r2p2-cc.h"

#define STATS_MAX_NUM_RECEIVERS 5
#define STATS_MAX_NUM_SENDERS 5
#define STATS_NUM_SRPT_MSG_TRACED 10
namespace hysup
{
    class SenderState;
    class ReceiverState;
    struct OutboundMsgState;
    class OutboundMsgs;

    //////////////////////////////////////////////////////////////////////////////////////////////////////////////
    //////////////////////////////////////////////////////////////////////////////////////////////////////////////
    /////////////////////////////////////////////Common///////////////////////////////////////////////////////////
    //////////////////////////////////////////////////////////////////////////////////////////////////////////////
    //////////////////////////////////////////////////////////////////////////////////////////////////////////////

    struct GenericMsgState
    {
    protected:
        GenericMsgState(uniq_req_id_t req_id,
                        int32_t remote_addr) : req_id_(req_id),
                                               remote_addr_(remote_addr),
                                               seq_(0) {}

    public:
        uniq_req_id_t req_id_;
        int32_t remote_addr_;
        int32_t seq_;
    };

    struct GenericRemoteState
    {
    protected:
        GenericRemoteState(int debug, int32_t addr) : debug_(debug),
                                                      this_addr_(addr) {}

    protected:
        int debug_;
        int32_t this_addr_;
    };

    template <typename T>
    class GenericRemotesContainer
    {
    protected:
        GenericRemotesContainer(int debug, int32_t this_addr) : debug_(debug), this_addr_(this_addr) {}

    public:
        T *find_or_create(int32_t daddr, int32_t this_addr)
        {
            this_addr_ = this_addr;
            T *state = find(daddr);
            if (state == nullptr)
            {
                state = new T(debug_, this_addr_, daddr);
                remotes_[daddr] = state;
                on_create(daddr, state, this_addr);
            }
            return state;
        }
        /* Returns nullptr if receiver is not found */
        T *find(int32_t daddr)
        {
            T *state = nullptr;
            auto srch = remotes_.find(daddr);
            if (srch != remotes_.end())
            {
                state = srch->second;
            }
            return state;
        }
        virtual void on_create(int32_t daddr, T *state, int32_t this_addr) = 0;
        size_t size() { return remotes_.size(); }
        std::unordered_map<int32_t, T *> &get_remotes() { return remotes_; }

    protected:
        std::unordered_map<int32_t, T *> remotes_;
        int debug_;
        int32_t this_addr_;
    };

    int add_header_overhead(int payload, bool skip_last_pkt_check = false);

    int remove_header_overhead(int size);

    //////////////////////////////////////////////////////////////////////////////////////////////////////////////
    //////////////////////////////////////////////////////////////////////////////////////////////////////////////
    ////////////////////////////////////////////////Sender////////////////////////////////////////////////////////
    //////////////////////////////////////////////////////////////////////////////////////////////////////////////
    //////////////////////////////////////////////////////////////////////////////////////////////////////////////

    struct PolicyState
    {
        PolicyState() : deficit_(0.0),
                        quantum_(-1.0),
                        can_send_(false),
                        priority_flow_(false),
                        num_can_send_(-1),
                        want_to_send_bytes_(0),
                        msg_state_(nullptr),
                        rcver_state_(nullptr) {}
        double deficit_;
        double quantum_;
        bool can_send_;
        bool priority_flow_;
        int num_can_send_;
        uint32_t want_to_send_bytes_; /* Includes headers */
        OutboundMsgState *msg_state_;
        ReceiverState *rcver_state_;
    };

    class Receivers : public GenericRemotesContainer<ReceiverState>
    {
    public:
        Receivers(int debug, int32_t this_addr) : GenericRemotesContainer(debug, this_addr) {}
        void on_create(int32_t daddr, ReceiverState *state, int32_t this_addr) override;
        /* Returns the number of receivers that still need to send credit */
        size_t num_receivers_owing_credit();
        /* Returns the number of outbound messages across all receivers. */
        size_t num_outbound();
        /* Returns the total amount of available credit (incl headers) across all receivers */
        uint64_t get_total_available_credit();
        /* Returns available credit for receiver i. Returns 0 if the receiver is unknown */
        uint64_t get_available_credit(size_t receiver);

        /* Returns the SRPT order of the msg with msg_state */
        size_t get_msg_srpt_order(OutboundMsgState *msg_state);

        // void want_to_send(int32_t rcver);

        /* Shorthand for finding an outbound message across all receivers */
        OutboundMsgState *find_outbound_msg(int32_t receiver, uniq_req_id_t req_id);
        /* Shorthand for adding a new outbound message TODO: Needed? */
        void add_outbound_msg(int32_t receiver, OutboundMsgState *msg_state);

        /* Policy related fields and functions. TODO: Generalize */

        /* Update rcver_policy_states_ - Calculate deficits - addr argument is a hack*/
        void update_policy_state(int32_t this_addr, int sender_policy);

        /*  TODO: refactor */
        const PolicyState *select_msg_to_send();

        void set_policy_weights(double fs_weight, double srpt_weight)
        {
            assert(1.0 - (fs_weight + srpt_weight) < 0.000001);
            fs_weight_ = fs_weight;
            srpt_weight_ = srpt_weight;
        }

    protected:
        double fs_weight_ = 1.0;
        double srpt_weight_ = 0.0;
        std::unordered_map<int32_t, PolicyState> rcver_policy_states_;
    };

    class ReceiverState : public GenericRemoteState // used by senders
    {
    public:
        ReceiverState(int debug, int32_t this_addr, int32_t rcver_addr);
        ~ReceiverState();

        /* Returns the mount of credit still expected for all messages to this receiver */
        uint32_t credit_expected();
        /* Returns the number of outbound messages */
        size_t num_outbound();
        /* Return outbound msg or nullptr */
        OutboundMsgState *find_outbound_msg(uniq_req_id_t req_id);
        /* Add outbound message */
        void add_outbound_msg(OutboundMsgState *msg_state);
        /* Remove outbound message */
        void remove_outbound_msg(OutboundMsgState *msg_state);
        /* Return OutboundMsgs */
        OutboundMsgs *get_outbound_msgs()
        {
            return out_msgs_;
        }
        /**
         * Fill PolicyState fields: can_send_, want_to_send_bytes_,  and msg_state_.
         * Modifies ps.
         * TODO: is this funtionality of OutboundMsgs? Probably
         */
        void want_to_send(PolicyState &ps);

        bool should_reset()
        {
            bool ret = should_reset_;
            if (should_reset_)
                should_reset_ = false;
            return ret;
        }

        void set_reset()
        {
            should_reset_ = true;
        }

    public:
        /* the amount of available credit for this receiver. Includes headers */
        int avail_credit_bytes_;
        /* the amount of available credit for this receiver */
        int avail_credit_data_bytes_;
        int32_t rcver_addr_;

    protected:
        /* Messages to this receiver */
        OutboundMsgs *out_msgs_;
        bool should_reset_;
    };

    struct OutboundMsgState : public GenericMsgState
    {
        OutboundMsgState(uniq_req_id_t req_id,
                         int32_t daddr,
                         hdr_r2p2 *r2p2_hdr,
                         ReceiverState *rstate)
            : GenericMsgState(req_id, daddr),
              r2p2_hdr_(r2p2_hdr), total_bytes_(0),
              unsent_bytes_(0), is_request_(true), sent_anouncement_(false),
              sent_first_(false), active_(false), usnsol_check_done_(false),
              is_scheduled_(true),
              self_alloc_credit_(0), self_alloc_credit_data_(0),
              remaining_self_alloc_credit_data_(0),
              rcvr_state_(rstate),
              msg_creation_time_(-1.0),
              bytes_sent_(0),
              resend_queued_(false),
              resend_retire_ts_(-1.0) {}
        hdr_r2p2 *r2p2_hdr_; // the REQRDY/"REP0" header
        uint32_t total_bytes_;
        uint32_t unsent_bytes_;
        bool is_request_;
        bool sent_anouncement_; // whether a packet that announces this message has been sent
        bool sent_first_;       // whether the first() packet of the message has been sent
        bool active_;           // whether some credit has arrived for this message
        bool usnsol_check_done_;
        bool is_scheduled_;                    // Whether this message is completely scheduled
        int self_alloc_credit_;                // the amount of initially self allocated credit (with headers)
        int self_alloc_credit_data_;           // the amount of initially self allocated credit (w/o headers)
        int remaining_self_alloc_credit_data_; // self credited data not yet spent
        ReceiverState *rcvr_state_;
        double msg_creation_time_;
        uint32_t bytes_sent_;
        std::list<std::pair<uint32_t, uint32_t>> resend_ranges_;
        bool resend_queued_;
        double resend_retire_ts_;
    };

    class OutboundMsgs
    {
    public:
        OutboundMsgs(int debug, int32_t this_addr) : current_(0),
                                                     debug_(debug),
                                                     this_addr_(this_addr) {}
        ~OutboundMsgs() {};

        /*** API ***/

        /* Modifying calls */
        void append(OutboundMsgState *const msg);
        void remove(OutboundMsgState *const msg);
        OutboundMsgState *next();
        OutboundMsgState *smallest_of_next_sender();
        void sort_by_fewer_remaining();

        /* Read only calls */
        size_t find_pos(OutboundMsgState *const msg);
        OutboundMsgState *find_largest_remaining();
        OutboundMsgState *find_smallest_remaining();
        /* Returns nullptr if message is not found */
        OutboundMsgState *find(const uniq_req_id_t &req_id);
        OutboundMsgState *peek_next();
        OutboundMsgState *next_highest_credit();
        std::vector<OutboundMsgState *> *get_msgs_states()
        {
            return &msgs_;
        }
        int unsent_bytes();
        /**
         * @brief Returns the oreder of the message provided based on size.
         * 0 means that the message provided is the smallest one.
         * @throws runtime_error
         */
        size_t msg_order_size(OutboundMsgState const *const msg_state);
        inline size_t size() { return msgs_.size(); }
        void reset();
        std::string print_all(int32_t local_addr);

    protected:
        std::vector<OutboundMsgState *> msgs_;
        size_t current_;
        int debug_;
        int32_t this_addr_;
    };

    //////////////////////////////////////////////////////////////////////////////////////////////////////////////
    //////////////////////////////////////////////////////////////////////////////////////////////////////////////
    //////////////////////////////////////////////Receiver////////////////////////////////////////////////////////
    //////////////////////////////////////////////////////////////////////////////////////////////////////////////
    //////////////////////////////////////////////////////////////////////////////////////////////////////////////

    class SenderState : public GenericRemoteState // used by receivers
    {
    public:
        SenderState(int32_t sender_addr,
                    int srpb_bytes) : GenericRemoteState(-1, -1),
                                      sender_addr_(sender_addr),
                                      srpb_bytes_(srpb_bytes),
                                      nw_max_srpb_bytes_(srpb_bytes),
                                      host_max_srpb_bytes_(srpb_bytes),
                                      max_srpb_bytes_(srpb_bytes),
                                      srpb_ceiling_(srpb_bytes),
                                      nw_marked_ratio_(0.0),
                                      host_marked_ratio_(0.0),
                                      last_pkt_(0.0),
                                      pkts_since_last_ratio_update_(0),
                                      pkts_at_next_ratio_update_(1),
                                      pkts_nw_marked_since_last_ratio_update_(0),
                                      pkts_ht_marked_since_last_ratio_update_(0),
                                      pkts_nw_since_last_wnd_update_(0),
                                      pkts_nw_at_next_wnd_update_(srpb_bytes / MAX_ETHERNET_FRAME_ON_WIRE),
                                      pkts_ht_since_last_wnd_update_(0),
                                      pkts_ht_at_next_wnd_update_(srpb_bytes / MAX_ETHERNET_FRAME_ON_WIRE),
                                      nw_ecn_burst_(false),
                                      host_ecn_burst_(false),
                                      prev_tx_bytes_(0),
                                      prev_ts_(0),
                                      prev_qlen_(0),
                                      prev_U_(0),
                                      Wc_(srpb_bytes),
                                      inc_stage_(0) {}

        int32_t sender_addr_;      // the address of the sender
        int srpb_bytes_;           // The remaining budget between a sender-receiver pair (incr when getting data, decr when sending grants)
        int nw_max_srpb_bytes_;    // adjusted based on nw ecn signal
        int host_max_srpb_bytes_;  // adjusted based on host ecn signal
        int max_srpb_bytes_;       // the most conservative of nw and host maxs
        int srpb_ceiling_;         // max value of max_srpb_bytes_
        double nw_marked_ratio_;   // ratio of network ecn marked to unmarked packets
        double host_marked_ratio_; // ratio of host ecn marked to unmarked packets
        double last_pkt_;          // when a packet last arrived
        uint32_t pkts_since_last_ratio_update_;
        uint32_t pkts_at_next_ratio_update_;
        uint32_t pkts_nw_marked_since_last_ratio_update_;
        uint32_t pkts_ht_marked_since_last_ratio_update_;
        uint32_t pkts_nw_since_last_wnd_update_;
        uint32_t pkts_nw_at_next_wnd_update_;
        uint32_t pkts_ht_since_last_wnd_update_;
        uint32_t pkts_ht_at_next_wnd_update_;
        bool nw_ecn_burst_; // like in tcp-full
        bool host_ecn_burst_;

        // HPCC variables
        uint64_t prev_tx_bytes_;
        double prev_ts_;
        uint32_t prev_qlen_;
        double prev_U_; // previous normalized bytes in flight
        double Wc_;
        uint32_t inc_stage_;
    };

    struct InboundMsgState : public GenericMsgState
    {
        InboundMsgState(uniq_req_id_t req_id,
                        int32_t sender_addr,
                        hdr_r2p2 *r2p2_hdr,
                        uint64_t data_bytes_expected,
                        bool got_info,
                        SenderState *sender_state)
            : GenericMsgState(req_id, sender_addr),
              first_header_(r2p2_hdr),
              data_bytes_expected_(data_bytes_expected),
              data_bytes_received_(0),
              data_bytes_granted_(0),
              received_msg_info_(got_info),
              sender_state_(sender_state),
              last_data_pkt_ts_(0.0),
              next_resend_timeout_ts_(0.0) { assert(sender_state != nullptr); }
        hdr_r2p2 *first_header_;
        uint64_t data_bytes_expected_;
        uint64_t data_bytes_received_; // 0 means no info
        uint64_t data_bytes_granted_;
        bool received_msg_info_; // either a grant_req or a first() pkt with credit req info has been received
        SenderState *sender_state_;
        double last_data_pkt_ts_;
        double next_resend_timeout_ts_;
        std::vector<uint8_t> received_pkt_bitmap_;
    };

    class InboundMsgs
    {
    public:
        InboundMsgs(int debug, int32_t this_addr);
        ~InboundMsgs();
        void add(InboundMsgState *const msg);
        void remove(InboundMsgState *const msg);
        size_t find_pos(InboundMsgState *const msg);
        InboundMsgState *find(const uniq_req_id_t &req_id);
        InboundMsgState *peek_next();
        InboundMsgState *next();
        size_t msg_order_size(InboundMsgState const *const msg_state);
        size_t size();
        std::vector<InboundMsgState *> *get_msgs_states() { return &msgs_; }
        void sort_by_fewer_remaining();
        void sort_by_fewer_bif();
        void reset();
        std::string print_all(int32_t local_addr);

    protected:
        std::vector<InboundMsgState *> msgs_;
        size_t current_;
        int debug_;
        int32_t this_addr_;
    };

    struct fewer_inbound_bytes_remaining
    {
        inline bool operator()(const InboundMsgState *s1, const InboundMsgState *s2)
        {
            return ((s1->data_bytes_expected_ - s1->data_bytes_received_) < (s2->data_bytes_expected_ - s2->data_bytes_received_));
        }
    };

    struct fewer_bif
    {
        inline bool operator()(const InboundMsgState *s1, const InboundMsgState *s2)
        {
            return ((s1->sender_state_->srpb_bytes_) > (s2->sender_state_->srpb_bytes_));
            // return ((s1->data_bytes_expected_ - s1->data_bytes_received_) < (s2->data_bytes_expected_ - s2->data_bytes_received_));
        }
    };

    struct fewer_outbound_bytes_remaining
    {
        inline bool operator()(const OutboundMsgState *s1, const OutboundMsgState *s2)
        {
            return (s1->unsent_bytes_ < s2->unsent_bytes_);
        }
    };

    //////////////////////////////////////////////////////////////////////////////////////////////////////////////
    //////////////////////////////////////////////////////////////////////////////////////////////////////////////
    //////////////////////////////////////////////Stats///////////////////////////////////////////////////////////
    //////////////////////////////////////////////////////////////////////////////////////////////////////////////
    //////////////////////////////////////////////////////////////////////////////////////////////////////////////

    struct Stats
    {
        int budget_bytes_;
        uint64_t markable_pkts_recvd_;
        uint64_t host_marked_pkts_recvd_;
        uint64_t nw_marked_pkts_recvd_;
        uint64_t credit_granted_;           // receiver (incl. headers)
        uint64_t credit_replenished_;       // receiver (incl. headers)
        uint64_t credit_received_;          // sender (incl. headers)
        uint64_t credit_used_;              // sender (incl. headers)
        uint64_t credit_data_request_rcvd_; // receiver
        uint64_t credit_data_granted_;      // receiver
        uint64_t credit_data_requested_;    // sender
        uint64_t credit_data_received_;     // sender
        long num_grants_sent_;

        int num_bytes_expected_;
        int num_bytes_in_flight_rec_;
        int num_active_outbound_;
        int num_active_inbound_;
        double mean_srpb_max_;
        double mean_nw_srpb_max_;
        double mean_host_srpb_max_;
        uint32_t min_srpb_max_;
        uint32_t min_nw_srpb_max_;
        uint32_t min_host_srpb_max_;
        int sum_srpb_;
        int max_srpb_;
        double avg_nw_marked_ratio_;
        double avg_host_marked_ratio_;
        uint32_t sum_bytes_in_flight_sndr_;
        uint32_t granted_bytes_queue_len_;
        // TODO: this tracing design is not serving its purpose any more. Rework.
        uint32_t credit_from_rcver_i_[STATS_MAX_NUM_RECEIVERS] = {0}; // sender. hacky way to get how much credit a sender has from each rcver. Works for receivers 0 to 4
        uint32_t credit_to_sender_i_[STATS_MAX_NUM_SENDERS] = {0};    // receiver. to know how much credit this receiver has allocated to sender i

        /**
         * Static
         */
        static void register_stats_instance(Stats *stats);
        static void write_static_vars(int32_t addr);
        static std::vector<Stats *> all_stats_;
        static int32_t static_leader_; // the host that updates static variables

        /* Variables */
        static uint64_t num_ticks_;                                                   // number of samples collected
        static uint64_t num_data_pkts_sent_;                                          // senders - total number of pkts sent
        static uint64_t num_data_pkts_of_SRPT_msg_sent_[STATS_NUM_SRPT_MSG_TRACED];   // senders - Num pkts sent when the msg was the smallest msg, the second smallest, and the third smallest (in terms of bytes remaining)
        static double ratio_data_pkts_of_SRPT_msg_sent_[STATS_NUM_SRPT_MSG_TRACED];   // senders - ratio of above two
        static uint64_t num_credit_pkts_sent_;                                        // receivers - number of credit pkts sent
        static uint64_t num_credit_pkts_of_SRPT_msg_sent_[STATS_NUM_SRPT_MSG_TRACED]; // receivers. Num credits sent for a msg when the msg was the smallest msg, the second smallest, and the third smallest (in terms of bytes remaining)
        static double ratio_credit_pkts_of_SRPT_msg_sent_[STATS_NUM_SRPT_MSG_TRACED]; // receivers - ratio of the above two
        static int total_budget_bytes_;                                               // receivers - total credit available at receivers
        static uint32_t total_credit_backlog_;                                        // senders - total credit accumulated at senders
        static uint32_t total_bif_rec_;                                               // receivers - total known bytes in flight per receiver,
        static double total_avg_host_marked_ratio_;                                   // receivers - aggregate marked ratio as seen by all receivers
        static uint64_t uplink_busy_count_;                                           // ONLY RELIABLE WITH VERY FREQUENT POLLING number of polls when there were data in the data_backlog.
        static uint64_t uplink_idle_while_outbound_count_;                            // ONLY RELIABLE WITH VERY FREQUENT POLLING number of polls when there were no data in the data_backlog and outbound messages exist
        static double total_ratio_uplink_busy_;                                       // ONLY RELIABLE WITH VERY FREQUENT POLLING
        static double total_ratio_uplink_idle_while_data_;                            // ONLY RELIABLE WITH VERY FREQUENT POLLING
    };
}
#endif
