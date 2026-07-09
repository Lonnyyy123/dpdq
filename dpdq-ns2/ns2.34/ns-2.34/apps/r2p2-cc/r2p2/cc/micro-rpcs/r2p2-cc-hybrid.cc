#include "r2p2-cc-micro.h"
#include "simple-log.h"
#include <algorithm>
#include <stdlib.h>
#include <packet.h>
#include <limits.h>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>

#define POLICY_TS_MSG 0
#define POLICY_TS_FIFO_SENDER 1
#define POLICY_FIFO_MSG 2
#define POLICY_SRPT_MSG 3
#define POLICY_EQUAL_BIF_MSG 4

// #define SRPT_RATIO 0.7
// #define TS_RATIO 0.3

static class R2p2CCMicroHybridClass : public TclClass
{
public:
    R2p2CCMicroHybridClass() : TclClass("R2P2_CC_MICRO/HYBRID") {}
    TclObject *create(int, const char *const *)
    {
        return (new R2p2CCHybrid);
    }
} class_r2p2_cc_micro_hybrid;

namespace {
void append_hybrid_log_line(const std::string &path, const std::string &line)
{
    if (path.empty()) {
        return;
    }
    std::ofstream out(path.c_str(), std::ios::out | std::ios::app);
    if (!out.is_open()) {
        std::cerr << "Could not open log file " << path << " for append\n";
        return;
    }
    out << line << std::endl;
}
}

R2p2CCHybrid::R2p2CCHybrid() : R2p2CCMicro(new R2p2UplinkQueue(this, 1), new PollTimer(this))
{
    init();
}

int R2p2CCHybrid::command(int argc, const char *const *argv)
{
    if (argc == 3) {
        if (strcmp(argv[1], "resend-timeout-log-file") == 0) {
            resend_timeout_log_file_ = argv[2];
            return TCL_OK;
        }
    }
    return R2p2CCMicro::command(argc, argv);
}

void R2p2CCHybrid::init()
{
    budget_bytes_ = 0;
    max_srpb_ = 0;
    unsolicited_thresh_bytes_ = -2;
    unsolicited_limit_senders_ = -1;
    unsolicited_burst_when_idle_ = -1;
    priority_flow_ = -1;
    data_prio_ = -1;
    sender_mark_aimd_ = -1;
    sender_policy_ratio_ = -1.0;
    receiver_policy_ = -1;
    account_unsched_ = -1;
    ecn_thresh_pkts_ = 0;
    total_bytes_sent_ = 0;
    ecn_min_mul_nw_ = -1.0;
    sender_algo_ = -1; // 0-99: ECN, 100-199: multi-bit
    // ECN: Base: 0 | Biased: 1 | 3d bit: 2
    // Mbit: Base: 100 | Biased: 101
    eta_ = -1.0;
    wai_ = MAXINT;
    max_stage_ = MAXINT;
    resend_timeout_s_ = -1.0;
    bind("budget_bytes_", &budget_bytes_); // global budget B
    bind("max_srpb_", &max_srpb_);         // budget per sender-receiver pair at receiver
    bind("unsolicited_thresh_bytes_", &unsolicited_thresh_bytes_);
    bind("unsolicited_limit_senders_", &unsolicited_limit_senders_);
    bind("unsolicited_burst_when_idle_", &unsolicited_burst_when_idle_);
    bind("priority_flow_", &priority_flow_);
    bind("data_prio_", &data_prio_);
    bind("sender_mark_aimd_", &sender_mark_aimd_);
    bind("sender_policy_ratio_", &sender_policy_ratio_);
    bind("receiver_policy_", &receiver_policy_); // policy used by receiver to multiplex grants
    bind("account_unsched_", &account_unsched_);
    bind("ecn_thresh_pkts_", &ecn_thresh_pkts_);     // ecn marking threshold
    bind("reset_after_x_rtt_", &reset_after_x_rtt_); // < 0.0 to disable
    bind("pace_grants_", &pace_grants_);
    bind("additive_incr_mul_", &additive_incr_mul_);
    bind("sender_policy_", &sender_policy_); // 0 -> fair sharing 1-> SRPT for sender_policy_ratio_ of the BW 2-> prioritize receivers with highest credit backlog (at sender_policy_ratio_ % of the BW)
    bind("ecn_min_mul_nw_", &ecn_min_mul_nw_);
    bind("state_polling_ival_s_", &state_polling_ival_s_);
    bind("sender_algo_", &sender_algo_);
    bind("eta_", &eta_);
    bind("wai_", &wai_);
    bind("max_stage_", &max_stage_);
    bind("resend_timeout_s_", &resend_timeout_s_);

    assert(additive_incr_mul_ != -1);
    assert(sender_policy_ != -1);
    assert(ecn_thresh_pkts_ != 0);
    assert(max_srpb_ != 0);
    assert(unsolicited_thresh_bytes_ != -2);
    assert(unsolicited_limit_senders_ != -1);
    assert(unsolicited_burst_when_idle_ != -1);
    assert(priority_flow_ != -1);
    assert(data_prio_ != -1);
    assert(sender_mark_aimd_ == 0 || sender_mark_aimd_ == 1);
    assert(sender_policy_ratio_ != -1.0);
    assert(receiver_policy_ != -1);
    assert(account_unsched_ != -1);
    assert(pace_grants_ == 1 || pace_grants_ == 0);
    assert(ecn_min_mul_nw_ > 0.0);
    assert(state_polling_ival_s_ > 0.0);
    assert(sender_algo_ > -1);
    assert(eta_ != -1.0);
    assert(wai_ != MAXINT);
    assert(max_stage_ != MAXINT);

    outbound_inactive_ = new hysup::OutboundMsgs(debug_, this_addr_);
    inbound_ = new hysup::InboundMsgs(debug_, this_addr_);
    receivers_ = new hysup::Receivers(debug_, this_addr_);
    if (sender_policy_ > 0)
    {
        /* Both for SRPT and other policies that do part policy part FS. Orthogonal to priority_flow */
        receivers_->set_policy_weights((1 - sender_policy_ratio_), sender_policy_ratio_);
    }
    else
        /* Total fair sharing at the sender. Orthogonal to priority_flow */
        receivers_->set_policy_weights(1.0, 0);

    max_budget_bytes_ = budget_bytes_;
    host_min_srpb_ = ecn_min_mul_ * max_srpb_;
    nw_min_srpb_ = ecn_min_mul_nw_ * max_srpb_;
    grant_pacer_backlog_ = 0;
    data_pacer_backlog_ = 0;
    last_grant_pacer_update_ = Scheduler::instance().clock();
    last_data_pacer_update_ = Scheduler::instance().clock();
    budget_backlog_bytes_ = 0;
    grant_pacer_speed_gbps_ = link_speed_gbps_;
    data_pacer_speed_gbps_ = link_speed_gbps_;
    rtt_s_ = (max_srpb_ * 8.0 / 1000.0 / 1000.0 / 1000.0) / (link_speed_gbps_);
    if (resend_timeout_s_ <= 0.0)
    {
        resend_timeout_s_ = std::max(8.0 * poll_interval_, 2.0 * rtt_s_);
    }

    marked_backlog_ = 0;
    max_marked_backlog_ = 1000000;

    grant_bytes_drained_per_loop_ = seconds_to_bytes(poll_interval_, grant_pacer_speed_gbps_ * 1000.0 * 1000.0 * 1000.0);
    data_bytes_drained_per_loop_ = seconds_to_bytes(poll_interval_, data_pacer_speed_gbps_ * 1000.0 * 1000.0 * 1000.0);
    slog::log2(debug_, this_addr_, "R2p2CCHybrid::init():", debug_, sender_policy_, receiver_policy_, account_unsched_, unsolicited_thresh_bytes_,
               budget_bytes_, max_srpb_, grant_pacer_speed_gbps_, grant_bytes_drained_per_loop_,
               ecn_thresh_pkts_, poll_interval_, rtt_s_, reset_after_x_rtt_,
               ecn_init_slash_mul_, pace_grants_, additive_incr_mul_, nw_min_srpb_, host_min_srpb_,
               sender_algo_, eta_, wai_, max_stage_, unsolicited_limit_senders_, unsolicited_burst_when_idle_, priority_flow_, sender_policy_ratio_, state_polling_ival_s_);

    stats = new hysup::Stats{};
    last_poll_trace_ = 0.0; // high value to pass > check the first time
    last_grant_time_ = 0.0;
    hysup::Stats::register_stats_instance(stats);
}

R2p2CCHybrid::~R2p2CCHybrid()
{
    delete stats;
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////Common////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////////////////////

void R2p2CCHybrid::poll()
{
    slog::log7(debug_, this_addr_, "R2p2CCHybrid::poll()");

    num_polls_++;
    send_grants();
    send_data();
    check_resend_timeouts();
    cleanup_finished_messages();
    poll_trace();
    poll_timer_->resched(poll_interval_);
}

uint32_t R2p2CCHybrid::get_granted_bytes_queue_len()
{
    if (receivers_->num_outbound() < 2)
    {
        return 0;
        // never mark if there is a single outbound message
    }
    return receivers_->get_total_available_credit();
}

bool R2p2CCHybrid::actually_mark(hdr_r2p2 &r2p2_hdr, bool current_decision)
{
    if (sender_algo_ == 1) // Biased ECN marking
    {
        throw std::runtime_error("Not implemented");
        // uniq_req_id_t req_id = std::make_tuple(r2p2_hdr.cl_addr(),
        //                                        r2p2_hdr.cl_thread_id(),
        //                                        r2p2_hdr.req_id());
        // hysup::OutboundMsgState *msg_state = outbound_->find(req_id);
        // assert(msg_state != nullptr);
        // bool final_decision = false;
        // assert(marked_backlog_ <= max_marked_backlog_);
        // if (marked_backlog_ == max_marked_backlog_)
        // {
        //     std::cout << "marked_backlog_ == max_marked_backlog_" << std::endl;
        //     if (!current_decision)
        //         marked_backlog_--;
        //     // no choice
        //     return true;
        // }

        // hysup::OutboundMsgState *largest_rem_msg = outbound_->find_largest_remaining();
        // if (largest_rem_msg->unsent_bytes_ == 0) // last packet of last msg
        // {
        //     return current_decision;
        // }

        // assert(largest_rem_msg != nullptr);
        // assert(largest_rem_msg->unsent_bytes_ > 0);
        // double random = static_cast<double>(std::rand()) / RAND_MAX;
        // double prob = (double)(msg_state->unsent_bytes_) / largest_rem_msg->unsent_bytes_;
        // std::cout << "lrgst: " << largest_rem_msg->unsent_bytes_ << " prob: " << prob << " rand: " << random << std::endl;
        // if (current_decision) // prev decision was to mark
        // {
        //     if (prob >= random)
        //     {
        //         final_decision = true;
        //     }
        //     else
        //     {
        //         final_decision = false;
        //         marked_backlog_++;
        //     }
        // }
        // else // prev decision was not to mark
        // {
        //     if (marked_backlog_ > 0)
        //     {
        //         if (prob >= random)
        //         {
        //             // mark when you were not supposed to (less likely to happen for small msgs)
        //             final_decision = true;
        //             marked_backlog_--;
        //         }
        //         else
        //         {
        //             final_decision = false;
        //         }
        //     }
        //     else
        //     {
        //         final_decision = false;
        //     }
        // }
        // assert(marked_backlog_ >= 0);
        // assert(marked_backlog_ <= max_marked_backlog_);
        // std::cout << this_addr_ << " return final_decision on " << r2p2_hdr.req_id() << " bklg: " << marked_backlog_ << " prev: " << current_decision << " new: " << final_decision << std::endl;
        // return final_decision;
    }
    else
    {
        return current_decision;
    }
}

uint32_t R2p2CCHybrid::get_outbound_vq_len(int32_t daddr)
{
    if (sender_algo_ == 101) // Biased multi-bit algorithm
    {
        throw std::runtime_error("Not implemented");
        // // std::cout << this_addr_ << " !!! oub: " << outbound_->size() << " dadr " << daddr << std::endl;
        // if (receivers_->num_outbound() < 2)
        // {
        //     return 0;
        //     // never mark if there is a single outbound message
        // }
        // // find smallest outbound msg
        // hysup::OutboundMsgState *msg_state = outbound_->find_smallest_remaining();
        // uint32_t ret = outbound_->total_available_credit_;
        // assert(msg_state);
        // double ratio = 0.95;
        // uint32_t excess_q = std::max(static_cast<int>(outbound_->total_available_credit_) - static_cast<int>((eta_ - 1.0) * max_srpb_), 0);
        // // std::cout << "excess_q: " << excess_q << " outbound_->total_available_credit_ " << outbound_->total_available_credit_ << std::endl;
        // int num_receivers = outbound_->size(); // TODO: make it into receivers, not messages
        // // if the smallest remaining msg goes to daddr, boost it, else throttle it.
        // if (msg_state->remote_addr_ == daddr)
        // {
        //     ret = static_cast<double>(outbound_->total_available_credit_ - excess_q) + (static_cast<double>(num_receivers) * static_cast<double>(excess_q) * (1.0 - ratio));
        //     // std::cout << "Boosting daddr: " << daddr << " Init: " << outbound_->total_available_credit_ << " Actual: " << ret << std::endl;
        // }
        // else
        // {
        //     ret = static_cast<double>(outbound_->total_available_credit_ - excess_q) + (static_cast<double>(num_receivers) * static_cast<double>(excess_q) * (ratio));
        //     // std::cout << "Throttling daddr: " << daddr << " Init: " << outbound_->total_available_credit_ << " Actual: " << ret << std::endl;
        // }
        // return ret;
    }
    else if (sender_algo_ == 2) // "3d bit"
    {
        size_t num_rcv = receivers_->num_receivers_owing_credit();
        slog::log4(debug_, this_addr_, "num_receivers_owing_credit:", num_rcv);
        if (num_rcv < 2)
        {
            return 0;
        }
        return 1;
    }
    else if (sender_algo_ == 3) // reset when SRPT
    {
        hysup::ReceiverState *rcver_state = receivers_->find(daddr);
        assert(rcver_state);
        if (rcver_state->should_reset())
        {
            slog::log4(debug_, this_addr_, "sender_algo_ == 3 - resetting");
            return 0;
        }
        return 1;
    }
    else
    {
        if (receivers_->num_outbound() < 2)
        {
            return 0;
            // never mark if there is a single outbound message
        }
        return receivers_->get_total_available_credit();
    }
}

void R2p2CCHybrid::send_to_transport(hdr_r2p2 &r2p2_hdr, int payload, int32_t daddr)
{
    assert(daddr != -1);
    if (!poll_started_)
    {
        poll();
        poll_started_ = true;
    }
    // Requests (mess bcs of R2P2 back-compat..)
    bool is_reqzero_of_multipkt = is_req0_of_multipkt(&r2p2_hdr);
    bool is_reqrdy = r2p2_hdr.msg_type() == hdr_r2p2::REQRDY;
    bool is_reqzero = is_req0(&r2p2_hdr);
    bool is_single_pkt_request = ((r2p2_hdr.msg_type() == hdr_r2p2::REQUEST) && is_single_packet_msg(&r2p2_hdr));
    bool is_multi_pkt_req_not_req0 = is_from_multipkt_req_not_req0(&r2p2_hdr);
    // Replies
    bool is_reply = (r2p2_hdr.msg_type() == hdr_r2p2::REPLY);

    slog::log4(debug_, this_addr_, "R2p2CCHybrid::send_to_transport(). Msg type:", r2p2_hdr.msg_type(),
               "payload:", payload, "Destination", daddr, "App lvl id:",
               r2p2_hdr.app_level_id(), "pkt_id():", r2p2_hdr.pkt_id(),
               is_single_pkt_request,
               "|", is_reqzero, is_reqzero_of_multipkt, is_multi_pkt_req_not_req0, is_reply, "|");
    if (is_reqzero_of_multipkt)
    {
        prep_msg_send(r2p2_hdr, payload, daddr);
        shorting_req0(r2p2_hdr, payload, daddr);
    }
    else if (is_reqrdy)
    {
        throw std::invalid_argument("Hybrid protocol never sends reqrdys");
    }
    else if (is_multi_pkt_req_not_req0 || is_single_pkt_request)
    {
        if (is_single_pkt_request)
            prep_msg_send(r2p2_hdr, payload, daddr);
        sending_request(r2p2_hdr, payload, daddr);
    }
    else if (is_reply)
    {
        sending_reply(r2p2_hdr, payload, daddr);
    }
    else
    {
        throw std::invalid_argument("CC layer was asked to send unknown message type");
    }
}

void R2p2CCHybrid::recv(Packet *pkt, Handler *h)
{
    packet_received(pkt);
    hdr_ip *ip_hdr = hdr_ip::access(pkt);
    hdr_r2p2 *r2p2_hdr = hdr_r2p2::access(pkt);
    hdr_flags *flags_hdr = hdr_flags::access(pkt);
    // hdr_cmn::access(pkt)->size() is expected to be just application payload bytes
    long pkt_size = hdr_cmn::access(pkt)->size() + R2P2_ALL_HEADERS_SIZE + hdr_cmn::access(pkt)->padding();
    slog::log5(debug_, this_addr_, "R2p2CCHybrid::recv() pkt",
               " from", ip_hdr->src().addr_, "req id:",
               r2p2_hdr->app_level_id(), "Pkt type: ", r2p2_hdr->msg_type(), "with total sz (inlc hdrs & padding):", pkt_size, "first():", r2p2_hdr->first(),
               "pkt_id:", r2p2_hdr->pkt_id(), "ecn_capable?", flags_hdr->ect(), "ce?",
               "budget_:", budget_bytes_, flags_hdr->ce(), "credit_req?", r2p2_hdr->credit_req(),
               "credit?", r2p2_hdr->credit(), "req_id():", r2p2_hdr->req_id(), "cl_thread():",
               r2p2_hdr->cl_thread_id(), "cl_addr():", r2p2_hdr->cl_addr(), "inbound msg cnt:", inbound_->size(),
               "tx_bytes:", hdr_r2p2::access(pkt)->tx_bytes(),
               "ts:", hdr_r2p2::access(pkt)->ts(),
               "B:", hdr_r2p2::access(pkt)->B(),
               "qlen:", hdr_r2p2::access(pkt)->qlen());
    assert(pkt_size >= MIN_ETHERNET_FRAME);
    assert(pkt_size <= MAX_ETHERNET_FRAME);
    hysup::SenderState *sender_state = update_sender_state(pkt);
    update_receiver_state(pkt);

    bool single_pkt_reply = ((r2p2_hdr->msg_type() == hdr_r2p2::REPLY) && is_single_packet_msg(r2p2_hdr));
    bool packet_provides_credit = r2p2_hdr->credit() > 0;

    if (single_pkt_reply)
    {
        assert(r2p2_hdr->credit() == 0);
        // assert(r2p2_hdr->credit_req() == 0);
        if (r2p2_hdr->msg_type() == hdr_r2p2::REQUEST)
        {
            assert(0);
            slog::log5(debug_, this_addr_, "Received the pkt of a single-packet REQUEST");
        }
        else if (r2p2_hdr->msg_type() == hdr_r2p2::REPLY)
            slog::log6(debug_, this_addr_, "Received the pkt of a single-packet REPLY");
        else
            throw std::invalid_argument("single packet message should be a request or a reply");
        r2p2_layer_->recv(pkt, h);
        return;
    }

    if (r2p2_hdr->msg_type() == hdr_r2p2::RESEND)
    {
        handle_resend(pkt);
        Packet::free(pkt);
        return;
    }

    if (packet_provides_credit)
    {
        assert(r2p2_hdr->credit_req() == 0);
        assert(r2p2_hdr->msg_type() == hdr_r2p2::GRANT);
        // packet provides credit - sender operation
        if (r2p2_hdr->msg_type() == hdr_r2p2::GRANT)
        {
            received_credit(pkt);
            Packet::free(pkt);
        }
        else
        {
            throw std::invalid_argument("packets that provide credit must be a GRANT");
        }
        return;
    }

    // Must retrieve message state
    uniq_req_id_t req_id = std::make_tuple(r2p2_hdr->cl_addr(),
                                           r2p2_hdr->cl_thread_id(),
                                           r2p2_hdr->req_id());
    hysup::InboundMsgState *msg_state = inbound_->find(req_id);

    if (msg_state == nullptr)
    {
        if ((r2p2_hdr->msg_type() == hdr_r2p2::REQUEST || r2p2_hdr->msg_type() == hdr_r2p2::REPLY) &&
            r2p2_hdr->resend_bytes() > 0)
        {
            slog::log4(debug_, this_addr_, "Dropping stale retransmitted packet for unknown msg", r2p2_hdr->req_id(),
                       "from", ip_hdr->src().addr_, "offset", r2p2_hdr->msg_offset(),
                       "payload", hdr_cmn::access(pkt)->size());
            Packet::free(pkt);
            return;
        }
        if (r2p2_hdr->credit_req() > 0)
        {
            assert(r2p2_hdr->msg_type() == hdr_r2p2::GRANT_REQ ||
                   r2p2_hdr->msg_type() == hdr_r2p2::REQUEST);
        }
        else
        {
            assert((r2p2_hdr->msg_type() == hdr_r2p2::REQUEST ||
                    r2p2_hdr->msg_type() == hdr_r2p2::REPLY));
        }
        slog::log5(debug_, this_addr_, "Creating a new message state instance. pkt_id():",
                   r2p2_hdr->pkt_id(), "sender:", ip_hdr->src().addr_);
        msg_state = new hysup::InboundMsgState(req_id, ip_hdr->src().addr_,
                                               new hdr_r2p2(*r2p2_hdr),
                                               0, // set later
                                               false,
                                               sender_state);
        inbound_->add(msg_state);
    }
    else
    {
        // state already existed -> some invariants must hold
        assert(unsolicited_thresh_bytes_ == -1 ? msg_state->received_msg_info_ : true); // this may happen with partially solicited messages as data can arrive before the credit request (reordering at core)
    }

    /**
     * Packet requests credit
     */
    bool packet_requests_credit = r2p2_hdr->credit_req() > 0;
    bool is_data_pkt = ((r2p2_hdr->msg_type() == hdr_r2p2::REQUEST || r2p2_hdr->msg_type() == hdr_r2p2::REPLY));
    if (packet_requests_credit && !(msg_state->received_msg_info_))
    {
        assert(r2p2_hdr->credit() == 0);
        assert(r2p2_hdr->msg_type() == hdr_r2p2::GRANT_REQ ||
               r2p2_hdr->msg_type() == hdr_r2p2::REQUEST); // ||
                                                           //    r2p2_hdr->msg_type() == hdr_r2p2::REPLY);
                                                           // if (msg_state->received_msg_info_ == false)
                                                           // {
        uint64_t expected = r2p2_hdr->credit_req();

        assert(stats->credit_data_request_rcvd_ >= stats->credit_data_granted_);
        stats->credit_data_request_rcvd_ += expected;
        slog::log5(debug_, this_addr_, "stats->credit_data_request_rcvd_:", stats->credit_data_request_rcvd_,
                   "stats->credit_data_granted_:", stats->credit_data_granted_, "diff:", stats->credit_data_request_rcvd_ - stats->credit_data_granted_);
        slog::log4(debug_, this_addr_, "received grant_req for app lvl id:", r2p2_hdr->app_level_id(),
                   "r2p2_hdr->unsol_credit()", r2p2_hdr->unsol_credit(),
                   "r2p2_hdr->unsol_credit_data()", r2p2_hdr->unsol_credit_data());
        msg_state->data_bytes_expected_ = expected;
        msg_state->received_msg_info_ = true;
        ensure_inbound_tracking(msg_state);
        slog::log5(debug_, this_addr_, "Set expected bytes to:",
                   msg_state->data_bytes_expected_);

        /**
         * For messages that have an unsolicited part
         */
        if (r2p2_hdr->unsol_credit() > 0)
        {
            assert(msg_state->data_bytes_received_ == 0);
            assert(unsolicited_thresh_bytes_ != 0);

            msg_state->data_bytes_granted_ = r2p2_hdr->unsol_credit_data();

            slog::log3(debug_, this_addr_, "msg:", std::get<2>(msg_state->req_id_),
                       "received credit req for usnol msg of size:", expected,
                       "msg_type:", r2p2_hdr->msg_type(),
                       "r2p2_hdr->unsol_credit():", r2p2_hdr->unsol_credit(),
                       "r2p2_hdr->unsol_credit_data()", r2p2_hdr->unsol_credit_data());

            // retrieve sender state
            hysup::SenderState *sender_state = nullptr;
            try
            {
                sender_state = sender_state_.at(msg_state->remote_addr_);
            }
            catch (const std::out_of_range &e)
            {
                slog::error(debug_, this_addr_, "Unsolicited msg. Did not find state for sender", msg_state->remote_addr_);
                throw;
            }

            if (account_unsched_)
            {
                budget_bytes_ -= r2p2_hdr->unsol_credit();
                stats->credit_granted_ += r2p2_hdr->unsol_credit();
                sender_state->srpb_bytes_ -= r2p2_hdr->unsol_credit();
            }
            else
            {
                throw std::runtime_error("Not accounting for unscheduled bytes in B and sender buckets has been implemented but not tested. It has been seen misbehaving");
                assert(budget_bytes_ >= 0);
                assert(budget_bytes_ <= max_budget_bytes_);
                assert(sender_state->srpb_bytes_ >= 0);
                assert(sender_state->srpb_bytes_ <= max_srpb_);
            }

            slog::log3(debug_, this_addr_, "(unsol) srpb reduced to:", sender_state->srpb_bytes_,
                       "for sender:", sender_state->sender_addr_, "bcs of msg", std::get<2>(msg_state->req_id_),
                       "r2p2_hdr->unsol_credit()", r2p2_hdr->unsol_credit(), "msg_type?", r2p2_hdr->msg_type());
        }

        if (r2p2_hdr->msg_type() == hdr_r2p2::GRANT_REQ)
        {
            Packet::free(pkt);
            assert(!is_data_pkt);
            assert(msg_state->data_bytes_granted_ == 0);
            return;
        }
        else
        {

            assert(msg_state->data_bytes_granted_ > 0);
        }
    }

    // At this point the type is one of: REQUEST, REPLY.
    // it is a data packet.
    assert(is_data_pkt);
    if (is_data_pkt)
    {
        assert(r2p2_hdr->credit() == 0);
        if (received_data(pkt, msg_state))
        {
            r2p2_layer_->recv(pkt, h);
        }
        else
        {
            // std::cout << "HYBRID DROP req_id " << r2p2_hdr->req_id()
            //           << " app_level_id " << r2p2_hdr->app_level_id()
            //           << " type " << r2p2_hdr->msg_type()
            //           << " first " << r2p2_hdr->first()
            //           << " pkt_id " << r2p2_hdr->pkt_id()
            //           << " offset " << r2p2_hdr->msg_offset()
            //           << std::endl;
            Packet::free(pkt);
        }
        return;
    }

    assert(r2p2_hdr->msg_type() != hdr_r2p2::FREEZE);
    assert(r2p2_hdr->msg_type() != hdr_r2p2::UNFREEZE);
    assert(r2p2_hdr->msg_type() != hdr_r2p2::R2P2_FEEDBACK);
    assert(r2p2_hdr->msg_type() != hdr_r2p2::GRANT);
    assert(r2p2_hdr->msg_type() != hdr_r2p2::GRANT_REQ);
    throw std::invalid_argument("CC layer received unknown packet type");
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////Sender////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////////////////////

void R2p2CCHybrid::prep_msg_send(hdr_r2p2 &r2p2_hdr, int payload, int32_t daddr)
{
    hysup::ReceiverState *rcvr_state = receivers_->find_or_create(daddr, this_addr_);
    assert(rcvr_state != nullptr);

    uniq_req_id_t req_id = std::make_tuple(r2p2_hdr.cl_addr(),
                                           r2p2_hdr.cl_thread_id(),
                                           r2p2_hdr.req_id());
    hysup::OutboundMsgState *msg_state = new hysup::OutboundMsgState(req_id, daddr,
                                                                     new hdr_r2p2(r2p2_hdr),
                                                                     rcvr_state);

    msg_state->unsent_bytes_ = payload;
    msg_state->total_bytes_ = payload;
    msg_state->is_request_ = true;
    msg_state->msg_creation_time_ = r2p2_hdr.msg_creation_time();
    outbound_inactive_->append(msg_state);
}

/**
 * @brief Bypasses R2P2's req0 mechanism and gets all the data from r2p2-client.
 *
 * @param r2p2_hdr
 * @param payload
 * @param daddr
 */
void R2p2CCHybrid::shorting_req0(hdr_r2p2 &r2p2_hdr, int payload, int32_t daddr)
{
    slog::log4(debug_, this_addr_, "R2p2CCHybrid::shorting_req0() to", daddr, "app lvl id:", r2p2_hdr.app_level_id(),
               "Inactive Outbound messages =", outbound_inactive_->size(),
               "msg size (pkts) =", r2p2_hdr.pkt_id(), "total pkts:", r2p2_hdr.req_id());

    // Create outbound message state
    assert(r2p2_hdr.pkt_id() >= 1);
    assert(payload == 14);

    // send REQRDY to self
    Packet *pkt = Packet::alloc();
    hdr_r2p2 *hdr = hdr_r2p2::access(pkt);
    (*hdr) = r2p2_hdr;
    hdr->msg_type() = hdr_r2p2::REQRDY;
    hdr->first() = false;
    hdr->last() = false;

    r2p2_layer_->recv(pkt, nullptr);
    Packet::free(pkt);
}

/**
 * @brief Called when the layer above forwards the data part of a request (not for req0)
 *
 * @param r2p2_hdr
 * @param payload
 * @param daddr
 */
void R2p2CCHybrid::sending_request(hdr_r2p2 &r2p2_hdr, int payload, int32_t daddr)
{

    uniq_req_id_t req_id = std::make_tuple(r2p2_hdr.cl_addr(),
                                           r2p2_hdr.cl_thread_id(),
                                           r2p2_hdr.req_id());
    hysup::OutboundMsgState *msg_state = outbound_inactive_->find(req_id);

    // set missing info of msg_state
    assert(msg_state != nullptr);
    if ((msg_state->unsent_bytes_ == 14) && (!is_single_packet_msg(&r2p2_hdr) && r2p2_hdr.msg_type() == hdr_r2p2::REQUEST))
    {
        // ... only applies to multi-packet requests. Back compat..
        msg_state->unsent_bytes_ += payload;
        msg_state->total_bytes_ += payload;
    }

    // recalculate pkt_id (message size in pkts, carried by first() pkt)
    // because this sender will not send a req0 any more.
    uint32_t pkt_count = msg_state->total_bytes_ / MAX_R2P2_PAYLOAD;
    if (msg_state->total_bytes_ % MAX_R2P2_PAYLOAD > 0)
        pkt_count++;
    assert(pkt_count > 0);
    pkt_count--; // convetion. pkt_id() tells the receiver how many _more_ pkts to expect
    msg_state->r2p2_hdr_->pkt_id() = pkt_count;
    slog::log4(debug_, this_addr_, "R2p2CCHybrid::sending_request() to", daddr,
               "app lvl id:", r2p2_hdr.app_level_id(), "payload", payload,
               "total unsent bytes:", msg_state->unsent_bytes_, "changed pkt_id() to",
               msg_state->r2p2_hdr_->pkt_id());
    if (!((r2p2_hdr.msg_type() == hdr_r2p2::REQUEST) && is_single_packet_msg(&r2p2_hdr)))
    {
        assert(static_cast<int>(msg_state->unsent_bytes_) == payload + 14);
    }
}

/**
 * @brief Called when the layer above forwards a reply
 *
 * @param r2p2_hdr
 * @param payload
 * @param daddr
 */
void R2p2CCHybrid::sending_reply(hdr_r2p2 &r2p2_hdr, int payload, int32_t daddr)
{
    int credit_request_bytes = payload;
    slog::log5(debug_, this_addr_, "R2p2CCHybrid::sending_reply() to", daddr,
               "Total pkts:", r2p2_hdr.pkt_id(), "Payload:", payload, "will request credit:", credit_request_bytes);
    uniq_req_id_t req_id = std::make_tuple(r2p2_hdr.cl_addr(),
                                           r2p2_hdr.cl_thread_id(),
                                           r2p2_hdr.req_id());

    hysup::ReceiverState *rcvr_state = receivers_->find(daddr);
    if (rcvr_state == nullptr)
    {
        throw std::runtime_error("sending_reply() Cannot find state for receiver");
    }

    hysup::OutboundMsgState *msg_state = outbound_inactive_->find(req_id);
    assert(msg_state == nullptr);
    msg_state = new hysup::OutboundMsgState(req_id, daddr, new hdr_r2p2(r2p2_hdr), rcvr_state);
    msg_state->total_bytes_ = payload;
    msg_state->unsent_bytes_ = payload;
    msg_state->active_ = false;
    msg_state->sent_anouncement_ = false;
    msg_state->sent_first_ = false;
    msg_state->is_request_ = false;
    msg_state->msg_creation_time_ = r2p2_hdr.msg_creation_time();
    outbound_inactive_->append(msg_state);
}

void R2p2CCHybrid::received_credit(Packet *pkt)
{
    // credit reflects the cost of transmitting something ON THE WIRE (includes preamble and interpacket etc)
    hdr_ip *ip_hdr = hdr_ip::access(pkt);
    hdr_r2p2 *r2p2_hdr = hdr_r2p2::access(pkt);
    int credit_amount_bytes = r2p2_hdr->credit();
    int32_t receiver = ip_hdr->src().addr_;
    assert(credit_amount_bytes <= MAX_ETHERNET_FRAME_ON_WIRE);
    assert(credit_amount_bytes >= 1 + R2P2_ALL_HEADERS_SIZE + INTER_PKT_GAP_SIZE + ETHERNET_PREAMBLE_SIZE);
    slog::log4(debug_, this_addr_, "R2p2CCHybrid::received_credit() from receiver:",
               receiver, "req_id:", r2p2_hdr->req_id(), "that credits:", credit_amount_bytes, "credit pad:", r2p2_hdr->credit_pad());

    int credit_data_net = credit_amount_bytes - (R2P2_ALL_HEADERS_SIZE + INTER_PKT_GAP_SIZE + ETHERNET_PREAMBLE_SIZE + r2p2_hdr->credit_pad());

    // Find message state
    uniq_req_id_t req_id = std::make_tuple(r2p2_hdr->cl_addr(),
                                           r2p2_hdr->cl_thread_id(),
                                           r2p2_hdr->req_id());
    if (r2p2_hdr->bounced_credit())
    {
        // A bounced credit revokes bytes that were previously granted by this receiver.
        hysup::InboundMsgState *inbound_msg = inbound_->find(req_id);
        auto sender_it = sender_state_.find(receiver);
        if (sender_it != sender_state_.end()) {
            sender_it->second->srpb_bytes_ += credit_amount_bytes;
        }
        budget_bytes_ += credit_amount_bytes;
        
        if (inbound_msg != nullptr) {
            uint64_t outstanding = 0;
            if (inbound_msg->data_bytes_granted_ > inbound_msg->data_bytes_received_) {
                outstanding = inbound_msg->data_bytes_granted_ - inbound_msg->data_bytes_received_;
            }
            uint64_t revoke_data = std::min<uint64_t>(credit_data_net, outstanding);
            inbound_msg->data_bytes_granted_ -= revoke_data;
            inbound_msg->seq_ -= r2p2_hdr->credit() - r2p2_hdr->credit_pad() - R2P2_ALL_HEADERS_SIZE - INTER_PKT_GAP_SIZE - ETHERNET_PREAMBLE_SIZE;
            double now = Scheduler::instance().clock();
            inbound_msg->next_resend_timeout_ts_ = now + resend_timeout_s_;
        }
        
        slog::log4(debug_, this_addr_, "R2p2CCHybrid::received_credit() revoked bounced credit from sender:",
                   receiver, "budget_bytes_:", budget_bytes_);
        return;
    }

    hysup::OutboundMsgState *msg_state = receivers_->find_outbound_msg(receiver, req_id);
    if (msg_state != nullptr)
    {
        // If state is found, it means that outbound_inactive_ must not have it.
        assert(outbound_inactive_->find(req_id) == nullptr);
        assert(msg_state->active_);
    }
    else
    {
        // outbound_inactive must contain the state.
        // The message will now be active and moved from inbound_inactive to inbound
        msg_state = outbound_inactive_->find(req_id);
        if (msg_state != nullptr)
        {
            assert(!msg_state->active_);
            msg_state->active_ = true;
            outbound_inactive_->remove(msg_state);
            msg_state->rcvr_state_->add_outbound_msg(msg_state);
        } // else state may have been deleted
    }

    // Find receiver state
    hysup::ReceiverState *rcvr_state = receivers_->find(ip_hdr->src().addr_);
    assert(rcvr_state != nullptr);

    assert(rcvr_state->avail_credit_bytes_ >= 0);
    rcvr_state->avail_credit_bytes_ += credit_amount_bytes;
    // assert(rcvr_state->avail_credit_data_bytes_ >= 0); // I think this can happen because a chunk of cerdit for the last part of a larger message can be used by a smaller message etc (i.e., credit is not always "aligned")
    rcvr_state->avail_credit_data_bytes_ += credit_data_net;
    // outbound_->total_available_credit_ += credit_amount_bytes;
    stats->credit_received_ += credit_amount_bytes;
    slog::log5(debug_, this_addr_, "credit received:", stats->credit_received_, "credit used:", stats->credit_used_, "diff:", stats->credit_received_ - stats->credit_used_);
    assert(stats->credit_used_ <= stats->credit_received_);
    assert(r2p2_hdr->credit_pad() < MIN_R2P2_PAYLOAD);
    assert(r2p2_hdr->credit_pad() >= 0);
    stats->credit_data_received_ += credit_data_net;
    slog::log5(debug_, this_addr_, "stats->credit_data_requested_:", stats->credit_data_requested_, "stats->credit_data_received_:",
               stats->credit_data_received_, "diff:", stats->credit_data_requested_ - stats->credit_data_received_);
    assert(stats->credit_data_requested_ >= stats->credit_data_received_);
    slog::log5(debug_, this_addr_, "R2p2CCHybrid::received_credit()2 from receiver:",
               ip_hdr->src().addr_, "of size:", credit_amount_bytes,
               "credit avail:", rcvr_state->avail_credit_bytes_,
               "credit avail data:", rcvr_state->avail_credit_data_bytes_);
    MsgTracerLogs logs;
    MsgTracer::timestamp_pkt(pkt, MSG_TRACER_RECEIVE, MSG_TRACER_HOST, std::move(logs));
}

hysup::ReceiverState *R2p2CCHybrid::update_receiver_state(Packet *pkt)
{
    hdr_ip *ip_hdr = hdr_ip::access(pkt);
    slog::log6(debug_, this_addr_, "R2p2CCHybrid::update_receiver_state() for receiver:",
               ip_hdr->src().addr_, "Receivers registered:", receivers_->size());
    hysup::ReceiverState *rcvr_state = receivers_->find_or_create(ip_hdr->src().addr_, this_addr_);
    return rcvr_state;
}

/**
 * @brief Loops through outbound messages and forwards data when the sender
 * has received credit for the receiver of each message.
 * To send, enough time must have passed since the last packet was sent such that the
 * Tx queue has drained.
 */
void R2p2CCHybrid::send_data()
{
    update_data_backlog();

    if (receivers_->size() == 0)
        return;

    hysup::OutboundMsgState *msg_state = nullptr;

    /**
     * Self-credit unsolicited data.
     * A message either 1) is partly or wholy unsolicited or 2) is completely scheduled and has no unsolicited part
     */
    for (size_t i = 0; i < outbound_inactive_->size(); i++)
    {
        msg_state = outbound_inactive_->next();
        if ((unsolicited_thresh_bytes_ == -1 || hysup::add_header_overhead(msg_state->total_bytes_) <= unsolicited_thresh_bytes_) &&
            !msg_state->usnsol_check_done_)
        {

            bool msg_fits_in_bdp = true;
            if (hysup::add_header_overhead(msg_state->total_bytes_) > max_srpb_)
            {
                msg_fits_in_bdp = false;
            }

            int to_grant_bytes = 0; // includes headers
            int to_grant_data_bytes = 0;
            if (msg_fits_in_bdp)
            {
                to_grant_bytes = hysup::add_header_overhead(msg_state->total_bytes_);
                to_grant_data_bytes = msg_state->total_bytes_;
                msg_state->r2p2_hdr_->has_scheduled_part() = false; // default is true
            }
            else
            {
                to_grant_bytes = hysup::add_header_overhead(hysup::remove_header_overhead(max_srpb_), true);
                to_grant_data_bytes = hysup::remove_header_overhead(max_srpb_);
            }

            // Limit available credit per pipe to 1xBDP
            int available_backlog_space = max_srpb_ - msg_state->rcvr_state_->avail_credit_bytes_;
            available_backlog_space = std::max(available_backlog_space, MAX_ETHERNET_FRAME_ON_WIRE);
            if (to_grant_bytes > available_backlog_space)
            {
                // backlog space includes headers (since it consists of credit that also includes headers.)
                to_grant_bytes = hysup::add_header_overhead(hysup::remove_header_overhead(available_backlog_space), true);
                to_grant_data_bytes = hysup::remove_header_overhead(available_backlog_space);
            }
            assert(to_grant_bytes > 0);

            // -------------------
            // [Optional] Only allocate unsol credit such that the amount of credit in the backlog does not exceed the threshold.
            // available_backlog_space = (ecn_thresh_pkts_ * MAX_ETHERNET_FRAME_ON_WIRE) - receivers_->get_total_available_credit();
            // available_backlog_space = std::max(available_backlog_space, 0);
            // slog::log4(debug_, this_addr_, "msg:", std::get<2>(msg_state->req_id_), "sz:", msg_state->total_bytes_,
            //            "want to self-allocate credit:", to_grant_bytes,
            //            "to_grant_data_bytes:", to_grant_data_bytes,
            //            "available_backlog_space", available_backlog_space,
            //            "outbound_inactive_->size()", outbound_inactive_->size());

            // if (unsolicited_limit_senders_ && to_grant_bytes > available_backlog_space)
            // {
            //     assert(0);
            //     // backlog spece includes headers (since it consists of credit that also includes headers.)
            //     to_grant_bytes = hysup::add_header_overhead(hysup::remove_header_overhead(available_backlog_space), true);
            //     to_grant_data_bytes = hysup::remove_header_overhead(available_backlog_space);
            // }
            // assert(to_grant_bytes > 0);

            // ------------------
            to_grant_bytes = std::max(to_grant_bytes, MIN_ETHERNET_FRAME_ON_WIRE);

            assert(to_grant_bytes >= MIN_ETHERNET_FRAME_ON_WIRE);
            assert(msg_state->rcvr_state_->avail_credit_bytes_ >= 0);
            msg_state->rcvr_state_->avail_credit_bytes_ += to_grant_bytes;
            msg_state->self_alloc_credit_ = to_grant_bytes;
            // assert(msg_state->rcvr_state_->avail_credit_data_bytes_ >= 0); // I think this can happen because a chunk of credit for the last part of a larger message can be used by a smaller message etc (i.e., credit is not always "aligned")
            msg_state->rcvr_state_->avail_credit_data_bytes_ += to_grant_data_bytes;
            msg_state->self_alloc_credit_data_ = to_grant_data_bytes;
            msg_state->remaining_self_alloc_credit_data_ = to_grant_data_bytes;
            stats->credit_received_ += to_grant_bytes;
            slog::log3(debug_, this_addr_, "self-allocated credit:", to_grant_bytes,
                       "to_grant_data_bytes:", to_grant_data_bytes,
                       "msg:", std::get<2>(msg_state->req_id_),
                       "available_backlog_space", available_backlog_space,
                       "request?", msg_state->is_request_);

            msg_state->usnsol_check_done_ = true;
            msg_state->is_scheduled_ = false;
        }
    }

    /**
     * Only messages that are completely scheduled get to send a credit request.
     * Those with self-allocated credit are just activated here and nothing more
     */
    size_t num_inactive = outbound_inactive_->size();
    for (size_t i = 0; i < num_inactive; i++)
    {
        msg_state = outbound_inactive_->next();
        slog::log6(debug_, this_addr_, "Considering Activating message:", std::get<2>(msg_state->req_id_),
                   "Before, len of inactive:", outbound_inactive_->size());
        if (!msg_state->sent_anouncement_)
        {
            assert(msg_state->total_bytes_ == msg_state->unsent_bytes_);

            /**
             * Send a credit request if msg is completely scheduled
             */
            if (msg_state->is_scheduled_)
            {
                assert(unsolicited_thresh_bytes_ != -1); // because then no message would send a credit req
                hdr_r2p2 hdr = *msg_state->r2p2_hdr_;
                hdr.msg_type() = hdr_r2p2::GRANT_REQ;

                // GRANT_REQ default set seq to 0
                hdr.seq() = 0;
                hdr.credit() = 0;
                hdr.credit_req() = msg_state->total_bytes_;
                hdr.msg_creation_time() = msg_state->msg_creation_time_;
                hdr.unsol_credit() = 0;
                hdr.unsol_credit_data() = 0;

                assert(unsolicited_thresh_bytes_ == 0 ? (hdr.unsol_credit() == 0) : true);

                forward_to_transport(std::make_tuple(hdr,
                                                     GRANT_REQ_MSG_SIZE,
                                                     msg_state->remote_addr_),
                                     MsgTracerLogs());
                data_pacer_backlog_ += GRANT_REQ_MSG_SIZE + R2P2_ALL_HEADERS_SIZE + INTER_PKT_GAP_SIZE + ETHERNET_PREAMBLE_SIZE;
                stats->credit_data_requested_ += msg_state->total_bytes_;
                slog::log5(debug_, this_addr_, "stats->credit_data_requested_:", stats->credit_data_requested_, "stats->credit_data_received_:",
                           stats->credit_data_received_, "diff:", stats->credit_data_requested_ - stats->credit_data_received_);
                assert(stats->credit_data_requested_ >= stats->credit_data_received_);
                slog::log4(debug_, this_addr_, "Forwarded standalone grant_request asking for",
                           msg_state->total_bytes_,
                           "bytes. App msg id:", msg_state->r2p2_hdr_->app_level_id(),
                           "new data_pacer_backlog_:", data_pacer_backlog_, "hdr.unsol_credit()", hdr.unsol_credit());
                msg_state->sent_anouncement_ = true;
            }

            slog::log5(debug_, this_addr_, "Activating message:", std::get<2>(msg_state->req_id_),
                       "Before, len of inactive:", outbound_inactive_->size());
            msg_state->active_ = true;
            outbound_inactive_->remove(msg_state);
            msg_state->rcvr_state_->add_outbound_msg(msg_state);
        }
    }

    if (((double)data_pacer_backlog_ > 1.5 * data_bytes_drained_per_loop_))
    {
        return;
    }

    if (send_one_retransmission())
    {
        return;
    }

    // INVARIANT: IF RECEIVER GETS DEFICIT ALLOCATED, IT MUST BE ABLE TO SEND

    // REMEMBER: PROBLEM when sender self-allocates credit but doesn't tell receiver.

    receivers_->update_policy_state(this_addr_, sender_policy_); // called for every packet

    const hysup::PolicyState *ps = receivers_->select_msg_to_send();

    if (ps == nullptr)
    {
        return;
    }

    msg_state = ps->msg_state_;
    uint64_t credit_avail = msg_state->rcvr_state_->avail_credit_bytes_;
    uint32_t to_send = std::max(std::min((uint32_t)MAX_R2P2_PAYLOAD, msg_state->unsent_bytes_) + R2P2_ALL_HEADERS_SIZE + INTER_PKT_GAP_SIZE + ETHERNET_PREAMBLE_SIZE, (uint32_t)MIN_ETHERNET_FRAME_ON_WIRE);
    assert(credit_avail >= to_send);
    assert(to_send >= ps->want_to_send_bytes_);

    slog::log5(debug_, this_addr_, "Sending pkt of msg (", std::get<2>(msg_state->req_id_),
               ") attributes: unsent_bytes_:", msg_state->unsent_bytes_,
               "rcvr:", msg_state->remote_addr_, "credit avail:", credit_avail, "to_send", to_send);

    int payload = 0;
    bool all_will_be_sent = msg_state->unsent_bytes_ <= (uint32_t)MAX_R2P2_PAYLOAD;
    if (all_will_be_sent)
    {
        payload = msg_state->unsent_bytes_;
        slog::log5(debug_, this_addr_, "Sending all remaining bytes:", payload);
    }
    else
    {
        payload = MAX_R2P2_PAYLOAD;
        slog::log5(debug_, this_addr_, "Sending SOME OF the remaining bytes:", payload);
    }
    slog::log5(debug_, this_addr_, "unused credit:", credit_avail,
               "will use:", to_send,
               "unsent_bytes_:", msg_state->unsent_bytes_,
               "total_bytes_:", msg_state->total_bytes_);
    assert(msg_state->unsent_bytes_ > 0);
    assert(msg_state->total_bytes_ > 0);
    assert(payload > 0);

    /**
     * If the mssage is not yet active (not announced to receiver), then make sure it gets announced now.
     */
    hdr_r2p2 hdr = *(msg_state->r2p2_hdr_);
    hdr.msg_creation_time() = msg_state->msg_creation_time_;
    int32_t daddr = msg_state->remote_addr_;
    assert(msg_state->active_);
    hdr.msg_type() = msg_state->is_request_ ? hdr_r2p2::REQUEST : hdr_r2p2::REPLY;

    uint32_t msg_offset = msg_state->total_bytes_ - msg_state->unsent_bytes_;
    hdr.msg_offset() = static_cast<int>(msg_offset);
    if (msg_state->is_request_) {
        hdr.msg_type() = hdr_r2p2::REQUEST;
    } else {
        hdr.msg_type() = hdr_r2p2::REPLY;
    }
    msg_state->seq_ = static_cast<int32_t>(msg_offset + payload);
    hdr.seq() = msg_state->seq_;

    hdr.credit() = 0;
    hdr.resend_bytes() = 0;
    hdr.credit_req() = 0;

    if (!msg_state->sent_first_)
    {
        // the first data packet of a message. This is for r2p2-server to work
        assert(msg_state->total_bytes_ == msg_state->unsent_bytes_);
        hdr.first() = true;
        msg_state->r2p2_hdr_->first() = false;
        msg_state->sent_first_ = true;
    }
    else
    {
        assert(msg_state->total_bytes_ >= msg_state->unsent_bytes_);
        assert(msg_state->sent_anouncement_);
        hdr.first() = false;
    }

    /**
     * Packets of message that do _not_ send a separate credit request
     */
    if (!msg_state->is_scheduled_)
    {
        msg_state->remaining_self_alloc_credit_data_ -= payload;
        hdr.unsol_credit() = msg_state->self_alloc_credit_;
        hdr.unsol_credit_data() = msg_state->self_alloc_credit_data_;
        hdr.used_unsol_credit() = false;
        if (msg_state->remaining_self_alloc_credit_data_ >= 0)
        {
            hdr.used_unsol_credit() = true;
        }
        hdr.credit() = 0;
        hdr.credit_req() = msg_state->total_bytes_;
        hdr.msg_creation_time() = msg_state->msg_creation_time_;
        if (!msg_state->sent_anouncement_)
        {
            stats->credit_data_requested_ += msg_state->total_bytes_;
            assert(stats->credit_data_requested_ >= stats->credit_data_received_);
        }
        msg_state->sent_anouncement_ = true;
    }

    if (priority_flow_ && ps->priority_flow_)
    {
        hdr.priority_flow() = true;
        hdr.bw_ratio() = ps->quantum_;
        // std::cout << "Sender: yes prioflow " << ps->priority_flow_ << " outbound: " << receivers_->num_outbound() << " ratio: " << hdr.bw_ratio() << "  ps->quantum_ " << ps->quantum_ << std::endl;
    }
    else
    {
        hdr.priority_flow() = false;
        // assert(ps->num_can_send_ > 0);
        hdr.bw_ratio() = ps->quantum_;
        // hdr.bw_ratio() = ps->quantum_ / (ps->num_can_send_ - 1);
        // std::cout << "Sender: not prioflow" << ps->priority_flow_ << " outbound: " << receivers_->num_outbound() << " ratio: " << hdr.bw_ratio() << "  ps->quantum_ " << ps->quantum_ << std::endl;
    }

    msg_state->bytes_sent_ = std::max<uint32_t>(msg_state->bytes_sent_, msg_offset + payload);
    msg_state->unsent_bytes_ -= payload;
    assert(msg_state->rcvr_state_->avail_credit_bytes_ >= (int)(payload + R2P2_ALL_HEADERS_SIZE + INTER_PKT_GAP_SIZE + ETHERNET_PREAMBLE_SIZE));
    msg_state->rcvr_state_->avail_credit_bytes_ -= to_send;
    assert(msg_state->rcvr_state_->avail_credit_bytes_ >= 0);
    msg_state->rcvr_state_->avail_credit_data_bytes_ -= payload;
    if (msg_state->rcvr_state_->avail_credit_data_bytes_ < 0)
    {
        slog::log5(debug_, this_addr_, "msg_state->rcvr_state_->avail_credit_data_bytes_ < 0",
                   msg_state->rcvr_state_->avail_credit_data_bytes_, "with headers:", msg_state->rcvr_state_->avail_credit_bytes_,
                   "credit_avail", credit_avail, "to_send", to_send);
    }
    // assert(msg_state->rcvr_state_->avail_credit_data_bytes_ >= 0); // I think this can happen because a chunk of cerdit for the last part of a larger message can be used by a smaller message etc (i.e., credit is not always "aligned")
    stats->credit_used_ += to_send;
    data_pacer_backlog_ += to_send;

    last_pkt_sent_ = Scheduler::instance().clock();
    assert(payload <= MAX_R2P2_PAYLOAD);
    assert(pace_grants_); // r2p2-udp will not work well if many pakcets are sent to it (see how it makrs based on ECN)
    // std::string other_msgs = outbound_->print_all(this_addr_);
    slog::log5(debug_, this_addr_, "credit received:", stats->credit_received_, "credit used:", stats->credit_used_, "diff:", stats->credit_received_ - stats->credit_used_);

    MsgTracerLogs logs;
    if (MsgTracer::do_trace_)
    {
        logs.logs_.push_back(MsgTracerLog("Rcver Avail Cr", std::to_string(msg_state->rcvr_state_->avail_credit_bytes_)));
        logs.logs_.push_back(MsgTracerLog("All Avail Cr", std::to_string(receivers_->get_total_available_credit())));
        logs.logs_.push_back(MsgTracerLog("Num outb.", std::to_string(receivers_->num_outbound())));
        // logs.logs_.push_back(MsgTracerLog("Outb", other_msgs)); // TODO: FIX
    }

    hdr.is_unsol_pkt() = false;
    if (data_prio_ && msg_state->total_bytes_ - msg_state->unsent_bytes_ <= msg_state->self_alloc_credit_data_)
    {
        hdr.is_unsol_pkt() = true;
    }
    forward_to_transport(std::make_tuple(hdr, payload, daddr), std::move(logs));

    /* TODO: Add function in receivers_ */
    if (do_trace_)
    {
        hysup::Stats::num_data_pkts_sent_++;
        size_t order = receivers_->get_msg_srpt_order(msg_state);
        if (order < STATS_NUM_SRPT_MSG_TRACED)
        {
            hysup::Stats::num_data_pkts_of_SRPT_msg_sent_[order]++;
        }
    }

    // if unsent_packets = 0 -> delete msg state
    if (msg_state->unsent_bytes_ == 0)
    {
        msg_state->resend_retire_ts_ = Scheduler::instance().clock() + 1000.0 * resend_timeout_s_;
        slog::log4(debug_, this_addr_, "Finished sending outbound msg", std::get<2>(msg_state->req_id_),
                   "retaining state for resend until", msg_state->resend_retire_ts_);
    }
    else
    {
        slog::log5(debug_, this_addr_, "Sending packet of msg",
                   std::get<2>(msg_state->req_id_), "to:", daddr, "Payload:", payload,
                   "announced:", msg_state->sent_anouncement_,
                   "Bytes tb sent:", payload + R2P2_ALL_HEADERS_SIZE + INTER_PKT_GAP_SIZE + ETHERNET_PREAMBLE_SIZE,
                   "avail crdt", msg_state->rcvr_state_->avail_credit_bytes_,
                   //    "credit backlog", receivers_->get_total_available_credit(), "unsent_bytes_:", msg_state->unsent_bytes_,
                   "all bytes:", msg_state->total_bytes_, "new data_pacer_backlog_:", data_pacer_backlog_);
    }
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////Receiver////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////////////////////

bool R2p2CCHybrid::received_data(Packet *pkt, hysup::InboundMsgState *msg_state)
{
    hdr_ip *ip_hdr = hdr_ip::access(pkt);
    hdr_r2p2 *r2p2_hdr = hdr_r2p2::access(pkt);
    assert(msg_state != nullptr);
    ensure_inbound_tracking(msg_state);
    auto srch = sender_state_.find(ip_hdr->src().addr_);
    assert(srch != sender_state_.end());
    hysup::SenderState *state = nullptr;
    state = srch->second;
    // expects payload_rec to be only application data. No headers or other overheads.
    int payload_rec = hdr_cmn::access(pkt)->size();
    uint32_t pkt_start = static_cast<uint32_t>(std::max(0, r2p2_hdr->msg_offset()));
    uint32_t pkt_index = pkt_start / MAX_R2P2_PAYLOAD;
    if (!msg_state->received_pkt_bitmap_.empty() && pkt_index >= msg_state->received_pkt_bitmap_.size())
    {
        // std::cout << "HYBRID BITMAP OOB req_id " << std::get<2>(msg_state->req_id_)
        //           << " expected_bytes " << msg_state->data_bytes_expected_
        //           << " offset " << pkt_start
        //           << " pkt_index " << pkt_index
        //           << " bitmap_size " << msg_state->received_pkt_bitmap_.size()
        //           << std::endl;
    }
    if (!msg_state->received_pkt_bitmap_.empty() && pkt_index < msg_state->received_pkt_bitmap_.size() && msg_state->received_pkt_bitmap_[pkt_index])
    {
        msg_state->last_data_pkt_ts_ = Scheduler::instance().clock();
        msg_state->next_resend_timeout_ts_ = msg_state->last_data_pkt_ts_ + resend_timeout_s_;
        slog::log4(debug_, this_addr_, "Ignoring duplicate data packet for msg", std::get<2>(msg_state->req_id_), "offset", pkt_start);
        // std::cout << "HYBRID DUP req_id " << std::get<2>(msg_state->req_id_)
        //           << " app_level_id " << r2p2_hdr->app_level_id()
        //           << " offset " << pkt_start
        //           << " received " << msg_state->data_bytes_received_
        //           << " expected " << msg_state->data_bytes_expected_
        //           << " granted " << msg_state->data_bytes_granted_
        //           << std::endl;
        return false;
    }
    if (!msg_state->received_pkt_bitmap_.empty() && pkt_index < msg_state->received_pkt_bitmap_.size())
    {
        msg_state->received_pkt_bitmap_[pkt_index] = 1;
    }
    msg_state->last_data_pkt_ts_ = Scheduler::instance().clock();
    msg_state->next_resend_timeout_ts_ = msg_state->last_data_pkt_ts_ + resend_timeout_s_;
    msg_state->data_bytes_received_ += (uint64_t)payload_rec;
    int bytes_replenished = std::max(payload_rec + R2P2_ALL_HEADERS_SIZE + INTER_PKT_GAP_SIZE + ETHERNET_PREAMBLE_SIZE, MIN_ETHERNET_FRAME_ON_WIRE);
    if (account_unsched_)
    {
        budget_bytes_ += bytes_replenished;
        state->srpb_bytes_ += bytes_replenished;
        stats->credit_replenished_ += bytes_replenished;
    }
    else
    {
        throw std::runtime_error("Not accounting for unscheduled bytes in B and sender buckets has been implemented but not tested. It has been seen misbehaving");
        if (!r2p2_hdr->used_unsol_credit())
        {
            budget_bytes_ += bytes_replenished;
            state->srpb_bytes_ += bytes_replenished;
            stats->credit_replenished_ += bytes_replenished;
        }
    }

    slog::log6(debug_, this_addr_, "(rcv) srpb increased to:", state->srpb_bytes_,
               "for sender:", state->sender_addr_, "bcs of msg", std::get<2>(msg_state->req_id_), "bytes_replenished:", bytes_replenished, "msg_type?", r2p2_hdr->msg_type());

    slog::log5(debug_, this_addr_, "credit granted:", stats->credit_granted_, "credit replenished:", stats->credit_replenished_, "diff:", stats->credit_granted_ - stats->credit_replenished_);
    if (stats->credit_replenished_ > stats->credit_granted_)
    {
        slog::log1(debug_, this_addr_, "credit granted:", stats->credit_granted_, "credit replenished:", stats->credit_replenished_, "diff:", stats->credit_granted_ - stats->credit_replenished_);
    }
    assert(stats->credit_replenished_ <= stats->credit_granted_);
    slog::log5(debug_, this_addr_, "R2p2CCHybrid::received_data() for msg:", r2p2_hdr->app_level_id(),
               "From:", msg_state->remote_addr_, "payload_rec: ", payload_rec, "Budget:",
               budget_bytes_, "SRPB:", state->srpb_bytes_, "replenished:", bytes_replenished, "sndr:",
               state->sender_addr_, "So far: bytes expected:", msg_state->data_bytes_expected_,
               "Bytes received:", msg_state->data_bytes_received_,
               "received_msg_info_:", msg_state->received_msg_info_);
    assert(payload_rec > 0);
    assert(budget_bytes_ <= max_budget_bytes_);
    if (msg_state->received_msg_info_)
        assert(msg_state->data_bytes_received_ <= msg_state->data_bytes_expected_); // only if msg info has been received (else expected == 0)

    MsgTracerLogs logs;
    if (MsgTracer::do_trace_)
    {
        std::string other_msgs = inbound_->print_all(this_addr_);
        logs.logs_.push_back(MsgTracerLog("SBmax", std::to_string(msg_state->sender_state_->max_srpb_bytes_)));
        logs.logs_.push_back(MsgTracerLog("BiF", std::to_string(max_srpb_ - msg_state->sender_state_->srpb_bytes_)));
        logs.logs_.push_back(MsgTracerLog("B avail", std::to_string(budget_bytes_)));
        logs.logs_.push_back(MsgTracerLog("Num Inb", std::to_string(inbound_->size())));
        logs.logs_.push_back(MsgTracerLog("Inb", other_msgs));
    }
    MsgTracer::timestamp_pkt(pkt, MSG_TRACER_RECEIVE, MSG_TRACER_HOST, std::move(logs));

    if (msg_state->data_bytes_received_ == msg_state->data_bytes_expected_ &&
        (msg_state->data_bytes_granted_ == msg_state->data_bytes_expected_)) // w/o this, msg_state may get deleted before all the bytes it requested are granted (bcs of commong grant pool)
    {
        slog::log4(debug_, this_addr_, "Removing inbound message state of msg", std::get<2>(msg_state->req_id_));
        inbound_->print_all(this_addr_);
        assert(msg_state->received_msg_info_);
        // delete state
        inbound_->remove(msg_state);
    }
    return true;
}

void R2p2CCHybrid::ensure_inbound_tracking(hysup::InboundMsgState *msg_state)
{
    if (!msg_state->received_msg_info_ || msg_state->data_bytes_expected_ == 0)
    {
        return;
    }

    uint32_t pkt_count = static_cast<uint32_t>((msg_state->data_bytes_expected_ + MAX_R2P2_PAYLOAD - 1) / MAX_R2P2_PAYLOAD);
    if (msg_state->received_pkt_bitmap_.size() != pkt_count)
    {
        msg_state->received_pkt_bitmap_.assign(pkt_count, 0);
    }
    if (msg_state->next_resend_timeout_ts_ <= 0.0)
    {
        double now = Scheduler::instance().clock();
        msg_state->last_data_pkt_ts_ = now;
        msg_state->next_resend_timeout_ts_ = now + resend_timeout_s_;
    }
}

uint32_t R2p2CCHybrid::payload_from_offset(uint32_t total_bytes, uint32_t start_offset) const
{
    if (start_offset >= total_bytes)
    {
        return 0;
    }
    return std::min<uint32_t>(MAX_R2P2_PAYLOAD, total_bytes - start_offset);
}

void R2p2CCHybrid::send_resend(hysup::InboundMsgState *msg_state, uint32_t start_offset, uint32_t resend_bytes)
{
    if (resend_bytes == 0)
    {
        return;
    }

    hdr_r2p2 hdr;
    hdr.msg_type() = hdr_r2p2::RESEND;
    hdr.req_id() = msg_state->first_header_->req_id();
    hdr.cl_addr() = msg_state->first_header_->cl_addr();
    hdr.cl_thread_id() = msg_state->first_header_->cl_thread_id();
    hdr.sr_addr() = msg_state->first_header_->sr_addr();
    hdr.sr_thread_id() = msg_state->first_header_->sr_thread_id();
    hdr.app_level_id() = msg_state->first_header_->app_level_id();
    hdr.msg_creation_time() = Scheduler::instance().clock();
    hdr.msg_offset() = static_cast<int>(start_offset);
    hdr.resend_bytes() = resend_bytes;
    hdr.seq() = static_cast<int>(start_offset + resend_bytes);
    hdr.first() = false;
    hdr.last() = false;
    forward_to_transport(std::make_tuple(hdr, RESEND_MSG_SIZE, msg_state->remote_addr_), MsgTracerLogs());
    data_pacer_backlog_ += RESEND_MSG_SIZE + R2P2_ALL_HEADERS_SIZE + INTER_PKT_GAP_SIZE + ETHERNET_PREAMBLE_SIZE;
    slog::log4(debug_, this_addr_, "Sent RESEND for msg", std::get<2>(msg_state->req_id_), "offset", start_offset, "bytes", resend_bytes);
}

void R2p2CCHybrid::check_resend_timeouts()
{
    double now = Scheduler::instance().clock();
    for (auto *msg_state : *(inbound_->get_msgs_states()))
    {
        if (!msg_state->received_msg_info_ || msg_state->data_bytes_expected_ == 0)
        {
            continue;
        }
        if (msg_state->data_bytes_received_ >= msg_state->data_bytes_expected_)
        {
            continue;
        }
        ensure_inbound_tracking(msg_state);
        if (now < msg_state->next_resend_timeout_ts_)
        {
            continue;
        }

        uint64_t granted_limit = std::min<uint64_t>(msg_state->data_bytes_granted_, msg_state->data_bytes_expected_);
        if (granted_limit <= msg_state->data_bytes_received_)
        {
            msg_state->next_resend_timeout_ts_ = now + resend_timeout_s_;
            continue;
        }

        uint32_t end_index = static_cast<uint32_t>((granted_limit + MAX_R2P2_PAYLOAD - 1) / MAX_R2P2_PAYLOAD);
        uint32_t range_start = 0;
        uint32_t range_bytes = 0;
        uint32_t total_resend_bytes = 0;
        uint32_t resend_range_count = 0;
        for (uint32_t i = 0; i < end_index; ++i)
        {
            bool received = (i < msg_state->received_pkt_bitmap_.size()) && msg_state->received_pkt_bitmap_[i];
            uint32_t pkt_start = i * MAX_R2P2_PAYLOAD;
            uint32_t pkt_payload = payload_from_offset(static_cast<uint32_t>(msg_state->data_bytes_expected_), pkt_start);
            if (pkt_payload == 0)
            {
                continue;
            }
            if (!received)
            {
                if (range_bytes == 0)
                {
                    range_start = pkt_start;
                }
                range_bytes += pkt_payload;
            }
            else if (range_bytes > 0)
            {
                total_resend_bytes += range_bytes;
                resend_range_count++;
                send_resend(msg_state, range_start, range_bytes);
                range_bytes = 0;
            }
        }
        if (range_bytes > 0)
        {
            total_resend_bytes += range_bytes;
            resend_range_count++;
            send_resend(msg_state, range_start, range_bytes);
        }
        if (resend_range_count > 0 && !resend_timeout_log_file_.empty())
        {
            std::ostringstream oss;
            oss << std::fixed << std::setprecision(10)
                << now
                << "," << this_addr_
                << ",RESEND_TIMEOUT"
                << "," << msg_state->remote_addr_
                << "," << std::get<2>(msg_state->req_id_)
                << "," << resend_range_count
                << "," << total_resend_bytes
                << "," << msg_state->data_bytes_received_
                << "," << msg_state->data_bytes_granted_
                << "," << msg_state->data_bytes_expected_;
            append_hybrid_log_line(resend_timeout_log_file_, oss.str());
        }
        msg_state->next_resend_timeout_ts_ = now + resend_timeout_s_;
    }
}

void R2p2CCHybrid::queue_resend(hysup::OutboundMsgState *msg_state, uint32_t start_offset, uint32_t resend_bytes)
{
    if (resend_bytes == 0)
    {
        return;
    }
    msg_state->resend_ranges_.push_back(std::make_pair(start_offset, resend_bytes));
    if (!msg_state->resend_queued_)
    {
        msg_state->resend_queued_ = true;
        resend_pending_.push_back(msg_state);
    }
}

bool R2p2CCHybrid::send_one_retransmission()
{
    while (!resend_pending_.empty())
    {
        hysup::OutboundMsgState *msg_state = resend_pending_.front();
        if (msg_state->resend_ranges_.empty())
        {
            msg_state->resend_queued_ = false;
            resend_pending_.pop_front();
            continue;
        }

        auto &range = msg_state->resend_ranges_.front();
        uint32_t start_offset = range.first;
        uint32_t payload = std::min<uint32_t>(MAX_R2P2_PAYLOAD, range.second);
        hdr_r2p2 hdr = *(msg_state->r2p2_hdr_);
        hdr.msg_creation_time() = msg_state->msg_creation_time_;
        hdr.msg_type() = msg_state->is_request_ ? hdr_r2p2::REQUEST : hdr_r2p2::REPLY;
        hdr.first() = (start_offset == 0);
        hdr.last() = (start_offset + payload >= msg_state->total_bytes_);
        hdr.credit() = 0;
        hdr.credit_req() = 0;
        hdr.unsol_credit() = 0;
        hdr.unsol_credit_data() = 0;
        hdr.used_unsol_credit() = false;
        hdr.is_unsol_pkt() = false;
        hdr.msg_offset() = static_cast<int>(start_offset);
        hdr.seq() = static_cast<int>(start_offset + payload);
        hdr.resend_bytes() = payload;

        data_pacer_backlog_ += std::max<int>(payload + R2P2_ALL_HEADERS_SIZE + INTER_PKT_GAP_SIZE + ETHERNET_PREAMBLE_SIZE, MIN_ETHERNET_FRAME_ON_WIRE);
        forward_to_transport(std::make_tuple(hdr, static_cast<int>(payload), msg_state->remote_addr_), MsgTracerLogs());
        slog::log4(debug_, this_addr_, "Retransmitted msg", std::get<2>(msg_state->req_id_), "offset", start_offset, "payload", payload);

        range.first += payload;
        range.second -= payload;
        if (range.second == 0)
        {
            msg_state->resend_ranges_.pop_front();
        }
        if (msg_state->resend_ranges_.empty())
        {
            msg_state->resend_queued_ = false;
            resend_pending_.pop_front();
        }
        return true;
    }
    return false;
}

void R2p2CCHybrid::handle_resend(Packet *pkt)
{
    hdr_ip *ip_hdr = hdr_ip::access(pkt);
    hdr_r2p2 *r2p2_hdr = hdr_r2p2::access(pkt);
    uniq_req_id_t req_id = std::make_tuple(r2p2_hdr->cl_addr(), r2p2_hdr->cl_thread_id(), r2p2_hdr->req_id());
    hysup::OutboundMsgState *msg_state = receivers_->find_outbound_msg(ip_hdr->src().addr_, req_id);
    if (msg_state == nullptr)
    {
        msg_state = outbound_inactive_->find(req_id);
        if (msg_state != nullptr)
        {
            msg_state->active_ = true;
            outbound_inactive_->remove(msg_state);
            msg_state->rcvr_state_->add_outbound_msg(msg_state);
        }
    }
    if (msg_state == nullptr)
    {
        slog::log4(debug_, this_addr_, "Ignoring RESEND for unknown msg", r2p2_hdr->req_id(), "from", ip_hdr->src().addr_);
        return;
    }

    uint32_t start_offset = static_cast<uint32_t>(std::max(0, r2p2_hdr->msg_offset()));
    uint32_t resend_bytes = r2p2_hdr->resend_bytes();
    uint32_t resend_end = std::min<uint32_t>(start_offset + resend_bytes, msg_state->total_bytes_);
    uint32_t sent_frontier = msg_state->bytes_sent_;

    if (start_offset < sent_frontier)
    {
        uint32_t overlap_end = std::min<uint32_t>(resend_end, sent_frontier);
        queue_resend(msg_state, start_offset, overlap_end - start_offset);
    }

    if (resend_end > sent_frontier)
    {
        uint32_t implicit_data = resend_end - std::max<uint32_t>(start_offset, sent_frontier);
        if (implicit_data > 0)
        {
            // RESEND is a loss-recovery signal only. Do not treat the
            // unsent suffix as an implicit grant; the sender must still obey
            // its currently available explicit credit.
            slog::log4(debug_, this_addr_, "Ignoring RESEND beyond sent frontier for msg", std::get<2>(msg_state->req_id_),
                       "offset", std::max<uint32_t>(start_offset, sent_frontier), "data bytes", implicit_data,
                       "available credit bytes", msg_state->rcvr_state_->avail_credit_bytes_,
                       "available data credit bytes", msg_state->rcvr_state_->avail_credit_data_bytes_);
        }
    }
}

void R2p2CCHybrid::cleanup_finished_messages()
{
    double now = Scheduler::instance().clock();
    std::vector<hysup::OutboundMsgState *> to_remove;
    for (auto &entry : receivers_->get_remotes())
    {
        auto *msgs = entry.second->get_outbound_msgs()->get_msgs_states();
        for (auto *msg_state : *msgs)
        {
            if (msg_state->unsent_bytes_ == 0 && msg_state->resend_retire_ts_ > 0.0 && now >= msg_state->resend_retire_ts_ && !msg_state->resend_queued_)
            {
                to_remove.push_back(msg_state);
            }
        }
    }
    for (auto *msg_state : to_remove)
    {
        msg_state->rcvr_state_->remove_outbound_msg(msg_state);
        delete msg_state->r2p2_hdr_;
        delete msg_state;
    }
}

hysup::SenderState *R2p2CCHybrid::update_sender_state(Packet *pkt)
{

    double reset_after = reset_after_x_rtt_ * rtt_s_;
    stats->markable_pkts_recvd_++;
    hdr_ip *ip_hdr = hdr_ip::access(pkt);

    slog::log6(debug_, this_addr_, "R2p2CCHybrid::update_sender_state() for sender:",
               ip_hdr->src().addr_, "Senders registered:", inbound_->size());
    auto srch = sender_state_.find(ip_hdr->src().addr_);
    hysup::SenderState *state = nullptr;
    if (srch == sender_state_.end())
    {
        // new sender
        slog::log4(debug_, this_addr_, "Adding sender", ip_hdr->src().addr_);
        state = new hysup::SenderState(ip_hdr->src().addr_, max_srpb_);
        sender_state_[ip_hdr->src().addr_] = state;
    }
    else
    {
        state = srch->second;
    }

    slog::log6(debug_, this_addr_, "Old nw marked ratio =", state->nw_marked_ratio_,
               "Old host marked ratio =", state->host_marked_ratio_,
               "Old srpb=", state->srpb_bytes_);

    /* Reset sbmax if more than "reset_after" time has elapsed since the last packet from this sender arrived */
    // if ((reset_after_x_rtt_ >= 0.0) && (Scheduler::instance().clock() - state->last_pkt_) > reset_after && (hdr_r2p2::access(pkt)->has_scheduled_part()))
    // {
    //     // std::cout << "it happaned " << hdr_r2p2::access(pkt)->has_scheduled_part() << std::endl;
    //     state->host_max_srpb_bytes_ = std::max(static_cast<int>(max_srpb_ * 0.5), state->host_max_srpb_bytes_);
    //     state->nw_max_srpb_bytes_ = std::max(static_cast<int>(max_srpb_ * 0.5), state->nw_max_srpb_bytes_);
    // }

    // --------------------------------------------------------------------------
    bool update_ratios = false;
    state->pkts_since_last_ratio_update_++;
    if (state->pkts_since_last_ratio_update_ == state->pkts_at_next_ratio_update_)
    {
        update_ratios = true;
    }
    /* all calculations should be made given the current window size:
     *   if the window is small, then fewer packets will arrive per RTT.
     *   since the updates happen per packet, updates when the window is small must have a larger weight.
     */

    update_nw_state(pkt, state);
    if (sender_mark_aimd_)
    {
        update_sn_state(pkt, state);
    }
    else
    {
        // Keep the sender-congestion control loop open so receiver scheduling
        // is governed only by the switch ECN feedback in update_nw_state().
        state->host_max_srpb_bytes_ = state->srpb_ceiling_;
        state->host_marked_ratio_ = 0.0;
        state->pkts_ht_marked_since_last_ratio_update_ = 0;
    }

    // --------------------------------------------------------------------------

    // Update GLOBAL budget
    state->max_srpb_bytes_ = std::min(state->nw_max_srpb_bytes_, state->host_max_srpb_bytes_);

    // if (priority_flow_ && hdr_r2p2::access(pkt)->msg_type() == hdr_r2p2::REQUEST)
    // {
    //     if (hdr_r2p2::access(pkt)->priority_flow())
    //     {
    //         state->max_srpb_bytes_ = std::max(state->max_srpb_bytes_, static_cast<int>((hdr_r2p2::access(pkt)->bw_ratio() / 2.0) * max_srpb_));
    //     }
    //     else
    //     {
    //         state->max_srpb_bytes_ = std::min(state->max_srpb_bytes_, static_cast<int>((hdr_r2p2::access(pkt)->bw_ratio() * 2.0) * max_srpb_));
    //     }
    // }

    if (priority_flow_ &&
        hdr_r2p2::access(pkt)->priority_flow() &&
        hdr_r2p2::access(pkt)->msg_type() == hdr_r2p2::REQUEST)
    {
        state->max_srpb_bytes_ = state->srpb_ceiling_;
    }

    state->last_pkt_ = Scheduler::instance().clock();
    if (update_ratios)
    {
        state->pkts_since_last_ratio_update_ = 0;
        state->pkts_at_next_ratio_update_ = (uint32_t)((double)state->max_srpb_bytes_ / MAX_ETHERNET_FRAME_ON_WIRE);
        slog::log5(debug_, this_addr_, "Updated. New nw marked ratio =", state->nw_marked_ratio_,
                   "New host marked ratio =", state->host_marked_ratio_,
                   "New max_srpb=", state->max_srpb_bytes_,
                   "New nw max_srpb=", state->nw_max_srpb_bytes_,
                   "New host max_srpb=", state->host_max_srpb_bytes_,
                   "Is prio flow=", hdr_r2p2::access(pkt)->priority_flow(),
                   "srpb=", state->srpb_bytes_, "sndr:", state->sender_addr_,
                   "Next update in pkts:", state->pkts_at_next_ratio_update_);
    }
    else
    {
        slog::log5(debug_, this_addr_, "New nw marked ratio =", state->nw_marked_ratio_,
                   "New host marked ratio =", state->host_marked_ratio_,
                   "New max_srpb=", state->max_srpb_bytes_,
                   "New nw max_srpb=", state->nw_max_srpb_bytes_,
                   "New host max_srpb=", state->host_max_srpb_bytes_,
                   "srpb=", state->srpb_bytes_, "sndr:", state->sender_addr_,
                   "Next update in pkts:", state->pkts_at_next_ratio_update_);
    }

    return state;
}

// Update SBmax for network
void R2p2CCHybrid::update_nw_state(Packet *pkt, hysup::SenderState *state)
{
    // hdr_ip *ip_hdr = hdr_ip::access(pkt);
    hdr_flags *flags_hdr = hdr_flags::access(pkt);
    bool nw_is_marked = false;
    bool update_ratios = false;
    bool reduce_nw_window = false;
    bool nw_should_ai = false;
    state->pkts_nw_since_last_wnd_update_++;

    if (state->pkts_since_last_ratio_update_ == state->pkts_at_next_ratio_update_)
    {
        update_ratios = true;
    }

    if (ecn_capable_ && flags_hdr->ect() && flags_hdr->ce())
    {
        nw_is_marked = true;
        stats->nw_marked_pkts_recvd_++;
        state->pkts_nw_marked_since_last_ratio_update_++;
    }

    if (ecn_capable_)
    {
        if (!state->nw_ecn_burst_ && nw_is_marked)
        {
            state->nw_ecn_burst_ = true;
        }
        else if (state->nw_ecn_burst_ && !nw_is_marked)
        {
            state->nw_ecn_burst_ = false;
        }

        nw_should_ai = (!nw_is_marked || state->nw_ecn_burst_);

        // if (nw_is_marked) {
        //     if (!state->nw_ecn_burst_) {
        //         state->nw_ecn_burst_ = true;
        //     }
        //     nw_should_ai = false;
        // } else {
        //     if (state->nw_ecn_burst_) {
        //         state->nw_ecn_burst_ = false;
        //     }
        //     nw_should_ai = true;
        // }
    }

    if (update_ratios)
    {

        double nw_ratio = (double)(state->pkts_nw_marked_since_last_ratio_update_) / (double)(state->pkts_since_last_ratio_update_);
        state->nw_marked_ratio_ = (1.0 - ce_new_weight_) * state->nw_marked_ratio_ + ce_new_weight_ * nw_ratio;
        state->pkts_nw_marked_since_last_ratio_update_ = 0;
    }

    double nw_change = 0.0;
    if (nw_should_ai)
    {
        nw_change += (double)additive_incr_mul_ * (MAX_ETHERNET_FRAME_ON_WIRE * MAX_ETHERNET_FRAME_ON_WIRE * 1 / state->max_srpb_bytes_);
    }
    reduce_nw_window = nw_is_marked && (state->pkts_nw_since_last_wnd_update_ >= state->pkts_nw_at_next_wnd_update_);
    if (reduce_nw_window) // only do MD when a marked pkt is received after at least one window of data
    {
        nw_change -= (state->nw_max_srpb_bytes_ * state->nw_marked_ratio_ / 2.0);
        slog::log5(debug_, this_addr_, "Reducing nw window. avg ratio:", state->nw_marked_ratio_, "change:", nw_change);
        state->pkts_nw_since_last_wnd_update_ = 0;
        state->pkts_nw_at_next_wnd_update_ = (uint32_t)(1.5 * (double)state->max_srpb_bytes_ / MAX_ETHERNET_FRAME_ON_WIRE);
    }

    state->nw_max_srpb_bytes_ += nw_change;
    state->nw_max_srpb_bytes_ = std::max(nw_min_srpb_, state->nw_max_srpb_bytes_);
    state->nw_max_srpb_bytes_ = std::min(state->nw_max_srpb_bytes_, max_srpb_);
}

// Update SBmax for sender
void R2p2CCHybrid::update_sn_state(Packet *pkt, hysup::SenderState *state)
{
    if (sender_algo_ == 0 || sender_algo_ == 1 || sender_algo_ == 2 || sender_algo_ == 3) // DCTCP (2 is for 3d bit)
    {
        hdr_flags *flags_hdr = hdr_flags::access(pkt);
        bool host_is_marked = false;
        bool update_ratios = false;
        bool reduce_ht_window = false;
        bool ht_should_ai = false;
        state->pkts_ht_since_last_wnd_update_++;

        if (state->pkts_since_last_ratio_update_ == state->pkts_at_next_ratio_update_)
        {
            update_ratios = true;
        }

        if (ecn_capable_ && flags_hdr->ect() && hdr_r2p2::access(pkt)->sender_marked())
        {
            host_is_marked = true;
            stats->host_marked_pkts_recvd_++;
            state->pkts_ht_marked_since_last_ratio_update_++;
        }

        if (ecn_capable_)
        {
            if (!state->host_ecn_burst_ && host_is_marked)
            {
                state->host_ecn_burst_ = true;
            }
            else if (state->host_ecn_burst_ && !host_is_marked)
            {
                state->host_ecn_burst_ = false;
            }

            ht_should_ai = (!host_is_marked || state->host_ecn_burst_);
        }

        if (update_ratios)
        {
            double host_ratio = (double)(state->pkts_ht_marked_since_last_ratio_update_) / (double)(state->pkts_since_last_ratio_update_);
            state->host_marked_ratio_ = (1.0 - ce_new_weight_) * state->host_marked_ratio_ + ce_new_weight_ * host_ratio;
            state->pkts_ht_marked_since_last_ratio_update_ = 0;
        }

        double host_change = 0.0;
        if (ht_should_ai)
        {
            host_change += (double)additive_incr_mul_ * (MAX_ETHERNET_FRAME_ON_WIRE * MAX_ETHERNET_FRAME_ON_WIRE * 1 / state->max_srpb_bytes_);
        }

        reduce_ht_window = host_is_marked && (state->pkts_ht_since_last_wnd_update_ >= state->pkts_ht_at_next_wnd_update_);
        if (reduce_ht_window)
        {
            host_change -= (state->host_max_srpb_bytes_ * state->host_marked_ratio_ / 2.0);
            slog::log5(debug_, this_addr_, "Reducing host window. avg ratio:", state->nw_marked_ratio_, "change:", host_change);
            state->pkts_ht_since_last_wnd_update_ = 0;
            state->pkts_ht_at_next_wnd_update_ = (uint32_t)(1.5 * (double)state->max_srpb_bytes_ / MAX_ETHERNET_FRAME_ON_WIRE);
        }

        state->host_max_srpb_bytes_ += host_change;
        state->host_max_srpb_bytes_ = std::max(host_min_srpb_, state->host_max_srpb_bytes_);
        state->host_max_srpb_bytes_ = std::min(state->host_max_srpb_bytes_, max_srpb_);
        if (sender_algo_ == 2) // for communicating exact queue length
        {
            throw std::runtime_error("Correcntess not checked");
            hdr_r2p2 *r2p2_hdr = hdr_r2p2::access(pkt);
            if (r2p2_hdr->qlen() == 0)
            {
                slog::log4(debug_, this_addr_, "Resetting host_max_srpb_bytes_");
                state->host_max_srpb_bytes_ = max_srpb_;
            }
        }
        else if (sender_algo_ == 3) // reset when SRPT
        {
            throw std::runtime_error("Correcntess not checked");
            hdr_r2p2 *r2p2_hdr = hdr_r2p2::access(pkt);
            if (r2p2_hdr->qlen() == 0)
            {
                slog::log4(debug_, this_addr_, "Resetting host_max_srpb_bytes_ to", static_cast<int>(sender_policy_ratio_ * max_srpb_));
                state->host_max_srpb_bytes_ = static_cast<int>(sender_policy_ratio_ * max_srpb_);
            }
        }
    }
    else if (sender_algo_ == 100 || sender_algo_ == 101) // HPCC and biased HPCC
    {
        hdr_r2p2 *r2p2_hdr = hdr_r2p2::access(pkt);
        bool update_ratios = false;
        if (state->pkts_since_last_ratio_update_ == state->pkts_at_next_ratio_update_)
        {
            update_ratios = true; // TODO: to use for Wc
        }

        // MeasureInflight
        slog::log6(debug_, this_addr_, "r2p2_hdr->tx_bytes():", r2p2_hdr->tx_bytes(),
                   "state->prev_tx_bytes_:", state->prev_tx_bytes_,
                   "msg type:", r2p2_hdr->msg_type());
        slog::log6(debug_, this_addr_, "r2p2_hdr->ts():", r2p2_hdr->ts(), "state->prev_ts_:", state->prev_ts_);
        // double tx_rate = (r2p2_hdr->tx_bytes() - state->prev_tx_bytes_);
        // assert(r2p2_hdr->tx_bytes() != 0);
        // assert(r2p2_hdr->ts() != 0);
        // assert(r2p2_hdr->tx_bytes() != state->prev_tx_bytes_); // can be negative bcs of reordering
        // double dt = (r2p2_hdr->ts() - state->prev_ts_);

        // // NOT For reordering networks
        // // assert(dt > 0.000000001);
        // // assert(tx_rate > 0.000000001);
        // slog::log6(debug_, this_addr_, "tx_rate:", tx_rate, "dt:", dt);

        // assert(dt != 0.0);        // can be negative bcs of reordering
        // assert(dt * tx_rate > 0); // both must be negative or both positive
        // state->prev_tx_bytes_ = r2p2_hdr->tx_bytes();
        // state->prev_ts_ = r2p2_hdr->ts();
        // tx_rate = tx_rate / dt; // tx_rate is in bytes per second
        // assert(tx_rate > 0.0);

        double tx_rate = r2p2_hdr->tx_rate_Bps();
        double dt = r2p2_hdr->dt();
        slog::log6(debug_, this_addr_, "new tx_rate:", tx_rate);
        slog::log5(debug_, this_addr_, "tx_rate (gbps):", tx_rate * 8.0 / 1000.0 / 1000.0 / 1000.0, "dt (ns):", dt * 1000.0 * 1000.0 * 1000.0);

        assert(tx_rate > 0.0);
        assert(tx_rate * 8.0 / 1000000000.0 <= 1.1 * link_speed_gbps_);
        assert(dt > 0.0);

        // slog::log6(debug_, this_addr_, "Caluclated TXrate:", tx_rate, "Received TXrate:", r2p2_hdr->tx_rate_Bps());

        slog::log5(debug_, this_addr_, "state->prev_qlen_:", state->prev_qlen_, "2p2_hdr->qlen():", r2p2_hdr->qlen());
        uint32_t qlen = std::min(state->prev_qlen_, r2p2_hdr->qlen());
        state->prev_qlen_ = r2p2_hdr->qlen();

        double u_new = 0;
        double bw_Bps = r2p2_hdr->B() / 8.0 * 1000.0 * 1000.0 * 1000.0; // bytes per second
        double BDP = bw_Bps * rtt_s_;                                   // bytes
        slog::log6(debug_, this_addr_, "BDP", BDP);

        u_new = qlen / BDP + tx_rate / bw_Bps;
        slog::log5(debug_, this_addr_, "u_new", u_new);

        // use EWMA
        double tau = std::min(dt, rtt_s_);
        u_new = (1 - tau / rtt_s_) * state->prev_U_ + tau / rtt_s_ * u_new;
        state->prev_U_ = u_new;
        slog::log5(debug_, this_addr_, "sender:", state->sender_addr_, "u_new after filter", u_new, "new weight:",
                   tau / rtt_s_, "tau (ns)", tau * 1000.0 * 1000.0 * 1000.0, "dt (ns):", dt * 1000.0 * 1000.0 * 1000.0, "update_ratios", update_ratios);

        // Compute New Window value (not doing: Wc, max_stage)
        double new_srpb = 0;
        if (u_new >= eta_ || state->inc_stage_ >= max_stage_)
        {

            slog::log6(debug_, this_addr_, "u_new", u_new, "state->inc_stage_", state->inc_stage_);
            if (u_new < eta_ && state->inc_stage_ >= max_stage_)
                slog::log6(debug_, this_addr_, "here bcs of max stage");

            new_srpb = state->Wc_ / (u_new / eta_) + wai_;
            if (update_ratios)
            {
                state->Wc_ = state->host_max_srpb_bytes_;
                state->inc_stage_ = 0;
            }
        }
        else
        {
            new_srpb = state->Wc_ + wai_;
            if (update_ratios)
            {
                state->Wc_ = state->host_max_srpb_bytes_;
                state->inc_stage_++;
            }
        }
        new_srpb = std::min(new_srpb, (double)max_srpb_);
        state->host_max_srpb_bytes_ = new_srpb;
        state->host_max_srpb_bytes_ = std::max(host_min_srpb_, state->host_max_srpb_bytes_);
        state->host_max_srpb_bytes_ = std::min(state->host_max_srpb_bytes_, max_srpb_);
        slog::log4(debug_, this_addr_, "host_max_srpb_bytes_", state->host_max_srpb_bytes_, "qlen:", qlen, "tx_rate/bw_Bps:", tx_rate / bw_Bps, "state->Wc_", state->Wc_, "new_srpb:", new_srpb);
        slog::log6(debug_, this_addr_, "new state->host_max_srpb_bytes", state->host_max_srpb_bytes_);
    }
    else
    {
        throw std::invalid_argument("Unsupported sender algorithm");
    }
}

void R2p2CCHybrid::send_grants()
{
    if(Scheduler::instance().clock() - last_grant_time_ < 0.00000048)
        return;
    last_grant_time_ = Scheduler::instance().clock();
    if (!pace_grants_)
        throw std::invalid_argument("pace_grants_ == 0 not supported");
    update_grant_backlog();
    slog::log7(debug_, this_addr_, "R2p2CCHybrid::send_grants(). Inbound messages:",
               inbound_->size(), "Pacer backlog:", grant_pacer_backlog_);
    switch (receiver_policy_)
    {
    case POLICY_TS_MSG:
        send_grants_ts_msg();
        break;
    case POLICY_SRPT_MSG:
        send_grants_srpt_msg();
        break;
    case POLICY_EQUAL_BIF_MSG:
        send_grants_equal_bif_msg();
        break;
    default:
        throw std::invalid_argument("Unsuported receiver policy");
        break;
    }
}

void R2p2CCHybrid::send_grants_ts_msg()
{
    hysup::InboundMsgState *msg_state = nullptr;
    int ret = -1;
    for (size_t i = 0; i < inbound_->size(); i++)
    {
        if (pace_grants_ && (grant_pacer_backlog_ > 1.5 * grant_bytes_drained_per_loop_))
        {
            break;
        }
        msg_state = inbound_->peek_next();

        slog::log5(debug_, this_addr_, "Granting out of", inbound_->size(), ": Next msg (", std::get<2>(msg_state->req_id_),
                   ") attributes: from", msg_state->remote_addr_, "ret:", ret, "expected:", msg_state->data_bytes_expected_,
                   "received:", msg_state->data_bytes_received_, "pacer blog:", grant_pacer_backlog_,
                   "recvd info:", msg_state->received_msg_info_);
        if (msg_state->received_msg_info_) // until the total msg size is known, do not send grants
        {
            assert(msg_state->data_bytes_expected_ > 0);
            ret = send_credit_policy_common(msg_state);
            // Note: send_credit_policy_common can delete the object pointed by msg_state
        }

        if (ret == 3)
        {
            // w/o this, messages can starve if the bottleneck is the sender
            break;
        }
        inbound_->next();
    }
}

/**
 * @brief Grants messages in "FIFO" order. IF a message doesn't have enough bytes requested
 * then this policy moves to the next oldest one etc.
 *
 */
void R2p2CCHybrid::send_grants_srpt_msg()
{
    if (pace_grants_ && (grant_pacer_backlog_ > 1.5 * grant_bytes_drained_per_loop_))
    {
        // performance optimization to avoid sorting
        return;
    }

    hysup::InboundMsgState *msg_state = nullptr;
    inbound_->sort_by_fewer_remaining(); // srpt
    inbound_->reset();

    int ret = -1;
    for (size_t i = 0; i < inbound_->size(); i++)
    {
        if (!pace_grants_)
        {
            assert(0); // w/o pacing, multiple grants may be sent per loop, in which case the algo is not SRPT (bcs the sorting happens once). next() is in case the shortest can't get grants for some reason
            throw std::runtime_error("Disabling credit pacing not supported.");
        }
        if (pace_grants_ && (grant_pacer_backlog_ > 1.5 * grant_bytes_drained_per_loop_))
        {
            break;
        }
        msg_state = inbound_->next();
        slog::log7(debug_, this_addr_, "Granting: Next msg (", std::get<2>(msg_state->req_id_),
                   ") attributes: expected:", msg_state->data_bytes_expected_,
                   "received:", msg_state->data_bytes_received_, "ret", ret, "Num inbound:", inbound_->size());

        if (msg_state->received_msg_info_) // until the total msg size is known, do not send grants
        {
            assert(msg_state->data_bytes_expected_ > 0);
            ret = send_credit_policy_common(msg_state);
        }
    }
}

void R2p2CCHybrid::send_grants_equal_bif_msg(void)
{
    if (pace_grants_ && (grant_pacer_backlog_ > 1.5 * grant_bytes_drained_per_loop_))
    {
        // performance optimization to avoid sorting
        return;
    }

    hysup::InboundMsgState *msg_state = nullptr;
    inbound_->sort_by_fewer_bif(); // srpt
    inbound_->reset();

    int ret = -1;
    for (size_t i = 0; i < inbound_->size(); i++)
    {
        if (!pace_grants_)
        {
            assert(0); // not tested
            throw std::runtime_error("Disabling credit pacing not supported.");
        }
        if (pace_grants_ && (grant_pacer_backlog_ > 1.5 * grant_bytes_drained_per_loop_))
        {
            break;
        }
        msg_state = inbound_->next();
        slog::log7(debug_, this_addr_, "Granting: Next msg (", std::get<2>(msg_state->req_id_),
                   ") attributes: expected:", msg_state->data_bytes_expected_,
                   "received:", msg_state->data_bytes_received_, "ret", ret, "Num inbound:", inbound_->size());

        if (msg_state->received_msg_info_) // until the total msg size is known, do not send grants
        {
            assert(msg_state->data_bytes_expected_ > 0);
            ret = send_credit_policy_common(msg_state);
        }
    }
}

/**
 * @brief Common code for all policies
 *
 * @param msg_state
 * @return 0 if credit was sent, 1 if there are no more credit to send,
 * 2 if (1) and msg_state was deleted, 3 if there is not enough global budget.
 */
int R2p2CCHybrid::send_credit_policy_common(hysup::InboundMsgState *msg_state)
{
    assert(msg_state->data_bytes_received_ <= msg_state->data_bytes_expected_); // must hold because rcvr should only send grants _after_ knowning msg size
    if ((msg_state->data_bytes_received_ == msg_state->data_bytes_expected_) &&
        (msg_state->data_bytes_granted_ == msg_state->data_bytes_expected_)) // w/o this, msg_state may get deleted before all the bytes it requested are granted (bcs of commong grant pool)
    {
        slog::log4(debug_, this_addr_, "send_credit_policy_common() Removing inbound message state of msg", std::get<2>(msg_state->req_id_));
        inbound_->print_all(this_addr_);
        assert(msg_state->received_msg_info_);
        // assert(msg_state->data_bytes_expected_ == msg_state->data_bytes_granted_); // not true because of fungible credit at sender
        // delete state
        // inbound_->print_all(this_addr_);
        inbound_->remove(msg_state);
        return 2;
    }
    // retrieve sender state
    hysup::SenderState *sender_state = msg_state->sender_state_;
    assert(sender_state != nullptr);
    // try
    // {
    //     sender_state = sender_state_.at(msg_state->remote_addr_);
    // }
    // catch (const std::out_of_range &e)
    // {
    //     slog::error(debug_, this_addr_, "R2p2CCHybrid::send_grants(). Did not find state for sender", msg_state->remote_addr_);
    //     throw;
    // }
    assert(sender_state != nullptr);

    assert(msg_state->received_msg_info_);
    assert(msg_state->data_bytes_expected_ > 0);
    int uncreditd_data = msg_state->data_bytes_expected_ - msg_state->data_bytes_granted_;
    assert(uncreditd_data >= 0);
    if (uncreditd_data == 0)
        return 1;
    int credit_needed_data = std::min((int)MAX_R2P2_PAYLOAD, uncreditd_data);
    int credit_needed = std::max((int)(credit_needed_data + R2P2_ALL_HEADERS_SIZE + INTER_PKT_GAP_SIZE + ETHERNET_PREAMBLE_SIZE), (int)MIN_ETHERNET_FRAME_ON_WIRE);
    int credit_padding = std::max(0, MIN_ETHERNET_FRAME_ON_WIRE - ((int)credit_needed_data + R2P2_ALL_HEADERS_SIZE + INTER_PKT_GAP_SIZE + ETHERNET_PREAMBLE_SIZE));
    // if (max_srpb_ + MAX_ETHERNET_FRAME_ON_WIRE < sender_state->srpb_bytes_)
    // {
    //     slog::error(debug_, this_addr_, "max_srpb_:", max_srpb_, "sender:", sender_state->sender_addr_,
    //                 "sender_state->srpb_bytes_:", sender_state->srpb_bytes_);
    //     slog::error(debug_, this_addr_, "stats->credit_replenished_:", stats->credit_replenished_,
    //                 "stats->credit_granted_:", stats->credit_granted_);
    //     slog::error(debug_, this_addr_, inbound_->print_all(this_addr_));
    // }
    assert(max_budget_bytes_ >= budget_bytes_);
    // assert(max_srpb_ + 2 * MAX_ETHERNET_FRAME_ON_WIRE >= sender_state->srpb_bytes_); // relaxing it a bit bceause an old message may use unsolicited credit of a new message before any pkts are sent for the latter (even when the sender does TS on a pipe level)
    int bytes_in_flight = max_srpb_ - sender_state->srpb_bytes_;
    int available_budget = std::min({budget_bytes_, sender_state->srpb_bytes_});
    slog::log5(debug_, this_addr_, "Send credit for msg", std::get<2>(msg_state->req_id_), "? needed:", credit_needed, "credit_needed_data:", credit_needed_data,
               "uncreditd_data:", uncreditd_data,
               "budget_bytes_:", budget_bytes_, "srpb_bytes_", sender_state->srpb_bytes_,
               "bytes_in_flight:", bytes_in_flight,
               "max_srpb_bytes_", sender_state->max_srpb_bytes_);
    if (available_budget >= credit_needed && bytes_in_flight <= (int)sender_state->max_srpb_bytes_)
    {
        budget_bytes_ -= credit_needed;

        sender_state->srpb_bytes_ -= credit_needed;
        slog::log6(debug_, this_addr_, "(crd) srpb decreased to:", sender_state->srpb_bytes_,
                   "for sender:", sender_state->sender_addr_, "bcs of msg", std::get<2>(msg_state->req_id_), "credit_needed:", credit_needed, "msg_type?", msg_state->first_header_->msg_type());
        stats->credit_granted_ += credit_needed;
        stats->credit_data_granted_ += credit_needed_data;
        slog::log5(debug_, this_addr_, "stats->credit_data_request_rcvd_:", stats->credit_data_request_rcvd_, "stats->credit_data_granted_:",
                   stats->credit_data_granted_, "diff:", stats->credit_data_request_rcvd_ - stats->credit_data_granted_);
        assert(stats->credit_data_request_rcvd_ >= stats->credit_data_granted_);
        grant_pacer_backlog_ += credit_needed;
        msg_state->data_bytes_granted_ += credit_needed_data;
        slog::log4(debug_, this_addr_, ">>>>Sending credit to:", msg_state->remote_addr_, "msg:", std::get<2>(msg_state->req_id_), "credit_needed:", credit_needed,
                   "credit_needed_data", credit_needed_data, "available_budget:", available_budget,
                   "new Global budget:", budget_bytes_, "new srpb_bytes_:", sender_state->srpb_bytes_,
                   "max_srpb_bytes_:", sender_state->max_srpb_bytes_, "bytes_in_flight", bytes_in_flight, "grant_pacer_backlog_:", grant_pacer_backlog_,
                   "nw marked_ratio_:", sender_state->nw_marked_ratio_, "host marked_ratio_:", sender_state->host_marked_ratio_);
        // inbound_->print_all(this_addr_);
        forward_grant(msg_state, credit_needed, credit_padding);
        slog::log5(debug_, this_addr_, "credit granted:", stats->credit_granted_, "credit replenished:", stats->credit_replenished_, "diff:", stats->credit_granted_ - stats->credit_replenished_);
    }
    else if (budget_bytes_ < credit_needed)
    {
        return 3;
    }
    assert(msg_state->data_bytes_granted_ <= msg_state->data_bytes_expected_);
    assert(sender_state->nw_marked_ratio_ >= -0.000001 && sender_state->nw_marked_ratio_ <= 1.000001);
    assert(sender_state->host_marked_ratio_ >= -0.000001 && sender_state->host_marked_ratio_ <= 1.000001);
    return 0;
}

/**
 * @brief Forward a grant.
 * credit_bytes includes header bytes
 */
void R2p2CCHybrid::forward_grant(hysup::InboundMsgState *msg_state, int credit_bytes, int padding)
{
    slog::log4(debug_, this_addr_, "R2p2CCHybrid::forward_grant() to ", msg_state->remote_addr_,
               "that provides credit:", credit_bytes, "for app lvl id:", msg_state->first_header_->app_level_id(),
               "for message:", msg_state->first_header_->req_id());
    hdr_r2p2 hdr;
    hdr.msg_type() = hdr_r2p2::GRANT;
    
    // seq is the data byte seq number, excluding headers and paddings
    msg_state->seq_ += credit_bytes - padding - R2P2_ALL_HEADERS_SIZE - INTER_PKT_GAP_SIZE - ETHERNET_PREAMBLE_SIZE;
    hdr.seq() = msg_state->seq_;
    hdr.credit() = credit_bytes; // this credit includes headers
    assert(padding < MIN_R2P2_PAYLOAD);
    hdr.credit_pad() = padding;
    hdr.first() = false;
    hdr.last() = false;
    hdr.req_id() = msg_state->first_header_->req_id();
    hdr.pkt_id() = msg_state->first_header_->pkt_id(); // not sure what we need here
    hdr.cl_addr() = msg_state->first_header_->cl_addr();
    hdr.cl_thread_id() = msg_state->first_header_->cl_thread_id();
    hdr.sr_addr() = msg_state->first_header_->sr_addr();
    hdr.sr_thread_id() = msg_state->first_header_->sr_thread_id();
    hdr.app_level_id() = msg_state->first_header_->app_level_id();
    hdr.uniq_req_id() = msg_state->first_header_->uniq_req_id();
    hdr.msg_creation_time() = Scheduler::instance().clock();
    data_pacer_backlog_ += GRANT_MSG_SIZE + R2P2_ALL_HEADERS_SIZE + INTER_PKT_GAP_SIZE + ETHERNET_PREAMBLE_SIZE;
    stats->num_grants_sent_++;

    if (do_trace_)
    {
        hysup::Stats::num_credit_pkts_sent_++;
        size_t order = inbound_->msg_order_size(msg_state);
        if (order < STATS_NUM_SRPT_MSG_TRACED)
        {
            hysup::Stats::num_credit_pkts_of_SRPT_msg_sent_[order]++;
        }
    }

    MsgTracerLogs logs;
    if (MsgTracer::do_trace_)
    {
        std::string other_msgs = inbound_->print_all(this_addr_); // slows down simulation a lot
        logs.logs_.push_back(MsgTracerLog("SBmax", std::to_string(msg_state->sender_state_->max_srpb_bytes_)));
        logs.logs_.push_back(MsgTracerLog("BiF", std::to_string(max_srpb_ - msg_state->sender_state_->srpb_bytes_)));
        logs.logs_.push_back(MsgTracerLog("B avail", std::to_string(budget_bytes_)));
        logs.logs_.push_back(MsgTracerLog("Num Inb", std::to_string(inbound_->size())));
        logs.logs_.push_back(MsgTracerLog("Inb", other_msgs));
    }

    forward_to_transport(std::make_tuple(hdr, GRANT_MSG_SIZE, msg_state->remote_addr_), std::move(logs));
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////Stats///////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////////////////////

void R2p2CCHybrid::poll_trace()
{
    if (do_trace_)
    {
        double now = Scheduler::instance().clock();
        if (now > last_poll_trace_ + state_polling_ival_s_)
        {
            trace_state("pol", -1);
            last_poll_trace_ = now;
        }
    }
}

void R2p2CCHybrid::per_loop_stats()
{
    stats->budget_bytes_ = budget_bytes_;
    stats->num_bytes_expected_ = 0;
    stats->num_bytes_in_flight_rec_ = 0;
    for (size_t i = 0; i < inbound_->size(); i++)
    {
        hysup::InboundMsgState *msg_state = inbound_->next();
        stats->num_bytes_expected_ += msg_state->data_bytes_expected_;
        stats->num_bytes_in_flight_rec_ += (msg_state->data_bytes_granted_ - msg_state->data_bytes_received_);
    }

    stats->num_active_outbound_ = -1; // What does "avtive" mean?
    // for (size_t i = 0; i < outbound_->size(); i++)
    // {
    //     hysup::OutboundMsgState *msg_state = outbound_->next();
    //     if (msg_state->active_)
    //         stats->num_active_outbound_++;
    // }

    stats->num_active_inbound_ = 0;
    for (size_t i = 0; i < inbound_->size(); i++)
    {
        hysup::InboundMsgState *msg_state = inbound_->next();
        if (msg_state->received_msg_info_)
            stats->num_active_inbound_++;
    }

    stats->mean_srpb_max_ = 0.0;
    stats->mean_nw_srpb_max_ = 0.0;
    stats->mean_host_srpb_max_ = 0.0;
    stats->min_srpb_max_ = INT_MAX;
    stats->min_nw_srpb_max_ = INT_MAX;
    stats->min_host_srpb_max_ = INT_MAX;
    stats->sum_srpb_ = 0;
    stats->max_srpb_ = 0;
    stats->avg_nw_marked_ratio_ = 0.0;
    stats->avg_host_marked_ratio_ = 0.0;
    stats->sum_bytes_in_flight_sndr_ = 0;
    for (auto it = sender_state_.begin(); it != sender_state_.end(); it++)
    {
        hysup::SenderState *state = it->second;
        stats->mean_srpb_max_ += (double)state->max_srpb_bytes_ / (double)sender_state_.size();
        stats->mean_nw_srpb_max_ += (double)state->nw_max_srpb_bytes_ / (double)sender_state_.size();
        stats->mean_host_srpb_max_ += (double)state->host_max_srpb_bytes_ / (double)sender_state_.size();
        stats->min_srpb_max_ = std::min(stats->min_srpb_max_, (uint32_t)state->max_srpb_bytes_);
        stats->min_nw_srpb_max_ = std::min(stats->min_nw_srpb_max_, (uint32_t)state->nw_max_srpb_bytes_);
        stats->min_host_srpb_max_ = std::min(stats->min_host_srpb_max_, (uint32_t)state->host_max_srpb_bytes_);
        stats->sum_srpb_ += state->srpb_bytes_;
        if (state->srpb_bytes_ > stats->max_srpb_)
            stats->max_srpb_ = state->srpb_bytes_;
        stats->avg_nw_marked_ratio_ += state->nw_marked_ratio_ / (double)sender_state_.size();
        stats->avg_host_marked_ratio_ += state->host_marked_ratio_ / (double)sender_state_.size();
        stats->sum_bytes_in_flight_sndr_ += max_srpb_ - state->srpb_bytes_; // wrong - this is receiver data (not sender)
    }

    stats->granted_bytes_queue_len_ = receivers_->get_total_available_credit();

    hysup::Stats::num_ticks_++;
    if (data_pacer_backlog_ > 0)
    {
        hysup::Stats::uplink_busy_count_++;
    }

    if (data_pacer_backlog_ <= 0 && receivers_->num_outbound() > 0)
    {
        hysup::Stats::uplink_idle_while_outbound_count_++;
    }

    // credit accumulated from each receiver
    for (size_t i = 0; i < STATS_MAX_NUM_RECEIVERS; i++)
    {
        stats->credit_from_rcver_i_[i] = receivers_->get_available_credit(i);
    }

    // credit this receiver has allocated to each sender
    for (size_t i = 0; i < STATS_MAX_NUM_SENDERS; i++)
    {
        auto sender_it = sender_state_.find(i);
        if (sender_it != sender_state_.end())
        {
            stats->credit_to_sender_i_[i] = (sender_it->second->srpb_ceiling_ - sender_it->second->srpb_bytes_);
        }
        else
        {
            stats->credit_to_sender_i_[i] = 0;
        }
    }

    hysup::Stats::write_static_vars(this_addr_);
}

void R2p2CCHybrid::trace_state(std::string event, double a_double)
{
    per_loop_stats();
    std::vector<std::string> vars;
    vars.push_back(event);
    vars.push_back(std::to_string(this_addr_));
    vars.push_back(std::to_string(a_double));
    vars.push_back(std::to_string(stats->min_srpb_max_));
    vars.push_back(std::to_string(stats->min_nw_srpb_max_));
    vars.push_back(std::to_string(stats->min_host_srpb_max_));
    vars.push_back(std::to_string(budget_bytes_));
    vars.push_back(std::to_string(receivers_->num_outbound()));
    vars.push_back(std::to_string(inbound_->size()));
    vars.push_back(std::to_string(stats->num_active_outbound_));
    vars.push_back(std::to_string(stats->num_active_inbound_));
    vars.push_back(std::to_string(stats->num_bytes_expected_));
    vars.push_back(std::to_string(stats->num_bytes_in_flight_rec_));
    vars.push_back(std::to_string(stats->mean_srpb_max_));
    vars.push_back(std::to_string(stats->mean_nw_srpb_max_));
    vars.push_back(std::to_string(stats->mean_host_srpb_max_));
    vars.push_back(std::to_string(stats->sum_srpb_));
    vars.push_back(std::to_string(stats->avg_nw_marked_ratio_));
    vars.push_back(std::to_string(stats->avg_host_marked_ratio_));
    vars.push_back(std::to_string(stats->sum_bytes_in_flight_sndr_));
    vars.push_back(std::to_string(stats->granted_bytes_queue_len_));
    vars.push_back(std::to_string(stats->markable_pkts_recvd_));
    vars.push_back(std::to_string(stats->host_marked_pkts_recvd_));
    vars.push_back(std::to_string(stats->nw_marked_pkts_recvd_));
    for (size_t i = 0; i < STATS_NUM_SRPT_MSG_TRACED; ++i)
    {
        vars.push_back(std::to_string(stats->ratio_data_pkts_of_SRPT_msg_sent_[i]));
    }
    vars.push_back(std::to_string(stats->total_budget_bytes_));
    vars.push_back(std::to_string(stats->total_credit_backlog_));
    vars.push_back(std::to_string(stats->total_bif_rec_));
    vars.push_back(std::to_string(stats->total_avg_host_marked_ratio_));
    for (size_t i = 0; i < STATS_NUM_SRPT_MSG_TRACED; ++i)
    {
        vars.push_back(std::to_string(stats->ratio_credit_pkts_of_SRPT_msg_sent_[i]));
    }
    vars.push_back(std::to_string(grant_pacer_backlog_));
    vars.push_back(std::to_string(data_pacer_backlog_));
    vars.push_back(std::to_string(stats->total_ratio_uplink_busy_));
    vars.push_back(std::to_string(stats->total_ratio_uplink_idle_while_data_));
    vars.push_back(std::to_string(stats->max_srpb_));

    // FIXME: hacks
    for (size_t i = 0; i < STATS_MAX_NUM_RECEIVERS; i++)
    {
        vars.push_back(std::to_string(stats->credit_from_rcver_i_[i]));
    }
    for (size_t i = 0; i < STATS_MAX_NUM_SENDERS; i++)
    {
        vars.push_back(std::to_string(stats->credit_to_sender_i_[i]));
    }

    tracer_->trace(vars);
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////Helper//////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////////////////////

void R2p2CCHybrid::update_grant_backlog()
{
    double now = Scheduler::instance().clock();                          // seconds
    double interval = std::min(now - last_grant_pacer_update_, 0.00001); // bcs of bootstraping
    grant_pacer_backlog_ -= static_cast<int>(seconds_to_bytes(interval, grant_pacer_speed_gbps_ * 1000.0 * 1000.0 * 1000.0));
    grant_pacer_backlog_ = std::max(grant_pacer_backlog_, 0);
    last_grant_pacer_update_ = Scheduler::instance().clock();
}

void R2p2CCHybrid::update_data_backlog()
{
    double now = Scheduler::instance().clock();                         // seconds
    double interval = std::min(now - last_data_pacer_update_, 0.00001); // bcs of bootstraping
    data_pacer_backlog_ -= static_cast<int>(seconds_to_bytes(interval, data_pacer_speed_gbps_ * 1000.0 * 1000.0 * 1000.0));
    data_pacer_backlog_ = std::max(data_pacer_backlog_, 0);
    last_data_pacer_update_ = Scheduler::instance().clock();
}
