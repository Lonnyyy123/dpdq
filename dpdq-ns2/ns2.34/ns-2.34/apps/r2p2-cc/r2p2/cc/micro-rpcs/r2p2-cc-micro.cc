#include "r2p2-cc-micro.h"
#include "simple-log.h"
#include <algorithm>

static class R2p2CCMicroClass : public TclClass
{
public:
    R2p2CCMicroClass() : TclClass("R2P2_CC_MICRO") {}
    TclObject *create(int, const char *const *)
    {
        return (new R2p2CCMicro);
    }
} class_r2p2_cc_micro;

R2p2CCMicro::R2p2CCMicro() : urpc_sz_bytes_(0),
                             single_path_per_msg_(0),
                             pace_uplink_(0),
                             uplink_queue_(new R2p2UplinkQueue(this, 0)),
                             poll_timer_(new PollTimer(this)),
                             poll_interval_(POLL_INTERVAL),
                             last_pkt_arival_(0.0),
                             last_pkt_sent_(0.0),
                             link_busy_(false),
                             last_link_busy_(0.0),
                             last_link_idle_(0.0),
                             poll_started_(false),
                             link_idle_when_pending_work_cnt_(0),
                             num_polls_(0),
                             umsg_cnt_(0),
                             ecn_capable_(0),
                             global_marked_ratio_(0.0),
                             ce_new_weight_(0.1),
                             ecn_mechanism_influence_(1.0),
                             ecn_init_slash_mul_(0.5),
                             ecn_min_mul_(0.1)
{
    bind("urpc_sz_bytes_", &urpc_sz_bytes_);
    bind("single_path_per_msg_", &single_path_per_msg_);
    bind("pace_uplink_", &pace_uplink_);
    bind("uplink_deque_policy_", &uplink_deque_policy_);
    bind("ecn_capable_", &ecn_capable_);
    bind("link_speed_gbps_", &link_speed_gbps_);
    bind("ce_new_weight_", &ce_new_weight_);
    bind("ecn_mechanism_influence_", &ecn_mechanism_influence_);
    bind("ecn_init_slash_mul_", &ecn_init_slash_mul_);
    bind("ecn_min_mul_", &ecn_min_mul_);
    bind("debug_", &debug_);
    poll_interval_ = (POLL_INTERVAL) / (link_speed_gbps_ / 1000.0); // POLL_INTERVAL is for 1Tbps
    uplink_queue_->set_deque_policy(static_cast<R2p2UplinkQueue::DeqPolicy>(uplink_deque_policy_));
    uplink_queue_->set_link_speed(link_speed_gbps_ * 1000.0 * 1000.0 * 1000.0);
    slog::log2(debug_, this_addr_, "R2p2CCMicro() (def)", urpc_sz_bytes_, single_path_per_msg_,
               pace_uplink_, uplink_deque_policy_, ecn_capable_, link_speed_gbps_, ce_new_weight_,
               ecn_mechanism_influence_, ecn_init_slash_mul_, ecn_min_mul_, poll_interval_, debug_);
}

R2p2CCMicro::R2p2CCMicro(R2p2UplinkQueue *uplink_queue, PollTimer *poll_timer) : urpc_sz_bytes_(0),
                                                                                 single_path_per_msg_(0),
                                                                                 pace_uplink_(0),
                                                                                 uplink_queue_(uplink_queue),
                                                                                 poll_timer_(poll_timer),
                                                                                 poll_interval_(POLL_INTERVAL),
                                                                                 last_pkt_arival_(0.0),
                                                                                 last_pkt_sent_(0.0),
                                                                                 link_busy_(false),
                                                                                 last_link_busy_(0.0),
                                                                                 last_link_idle_(0.0),
                                                                                 poll_started_(false),
                                                                                 link_idle_when_pending_work_cnt_(0),
                                                                                 num_polls_(0),
                                                                                 umsg_cnt_(0),
                                                                                 ecn_capable_(0),
                                                                                 global_marked_ratio_(0.0),
                                                                                 ce_new_weight_(0.1),
                                                                                 ecn_mechanism_influence_(1.0),
                                                                                 ecn_init_slash_mul_(0.5),
                                                                                 ecn_min_mul_(0.1)
{
    bind("urpc_sz_bytes_", &urpc_sz_bytes_);
    bind("single_path_per_msg_", &single_path_per_msg_);
    bind("pace_uplink_", &pace_uplink_);
    bind("uplink_deque_policy_", &uplink_deque_policy_);
    bind("ecn_capable_", &ecn_capable_);
    bind("link_speed_gbps_", &link_speed_gbps_);
    bind("ce_new_weight_", &ce_new_weight_);
    bind("ecn_mechanism_influence_", &ecn_mechanism_influence_);
    bind("ecn_init_slash_mul_", &ecn_init_slash_mul_);
    bind("ecn_min_mul_", &ecn_min_mul_);
    bind("debug_", &debug_);
    poll_interval_ = (POLL_INTERVAL) / (link_speed_gbps_ / 1000.0); // POLL_INTERVAL is for 1Tbps
    uplink_queue_->set_deque_policy(static_cast<R2p2UplinkQueue::DeqPolicy>(uplink_deque_policy_));
    uplink_queue_->set_link_speed(link_speed_gbps_ * 1000.0 * 1000.0 * 1000.0);
    slog::log2(debug_, this_addr_, "R2p2CCMicro() (def)", urpc_sz_bytes_,
               single_path_per_msg_, pace_uplink_, uplink_deque_policy_, ecn_capable_, link_speed_gbps_,
               ce_new_weight_, ecn_mechanism_influence_, ecn_init_slash_mul_, ecn_min_mul_, poll_interval_, debug_);
}

R2p2CCMicro::~R2p2CCMicro()
{
    delete uplink_queue_;
}

void R2p2CCMicro::recv(Packet *pkt, Handler *h)
{
    throw std::runtime_error("R2p2CCMicro::recv() not supported"); // headers are incorrect
    // Per-packet operation, very expensive in terms of space
    // if (do_trace_)
    //     trace_state("rcv", 0.0);
    hdr_r2p2 *r2p2_hdr = hdr_r2p2::access(pkt);
    hdr_flags *flags_hdr = hdr_flags::access(pkt);
    long pkt_size = hdr_cmn::access(pkt)->size() + HEADERS_SIZE;
    slog::log5(debug_, this_addr_, "R2p2CCMicro::recv() pkt with total sz:", pkt_size, ". IMS count=", inbound_msgs_.size(), " Pkt type: ", r2p2_hdr->msg_type(), "first():", r2p2_hdr->first(), "first_urpc():", r2p2_hdr->first_urpc(), "ecn_capable?", flags_hdr->ect(), "ce?", flags_hdr->ce(), "cl_thread_id", r2p2_hdr->cl_thread_id(), "sr_thread_id", r2p2_hdr->sr_thread_id());
    packet_received(pkt);

    if (r2p2_hdr->msg_type() == hdr_r2p2::GRANT_REQ)
    {
        // Some server wants to send a response to this client. Create state for the response.
        uniq_req_id_t req_id = std::make_tuple(r2p2_hdr->cl_addr(),
                                               r2p2_hdr->cl_thread_id(),
                                               r2p2_hdr->req_id());
        slog::log5(debug_, this_addr_, "GRANT_REQ unique req_id: ", r2p2_hdr->cl_addr(), r2p2_hdr->cl_thread_id(), r2p2_hdr->req_id());

        InboundMsgState *ims = find_ims(req_id);
        assert(ims == NULL);
        ims = new InboundMsgState();
        ims->req_id_ = req_id;
        ims->pkts_expected_ = r2p2_hdr->pkt_id();
        ims->data_pkts_received_ = 0; // only received the request to send reply packets
        ims->pkts_uncredited_ = ims->pkts_expected_;
        ims->pkts_received_ += 1;
        if (hdr_flags::access(pkt)->ect())
        {
            slog::log5(debug_, this_addr_, "grant_req is ecn capable");
        }
        if (ecn_capable_ && hdr_flags::access(pkt)->ect() && hdr_flags::access(pkt)->ce())
        {
            slog::log5(debug_, this_addr_, "grant_req experienced congestion!! ");
            ims->pkts_marked_ += 1;
            ims->marked_ratio_estim_ = ecn_init_slash_mul_;
            update_global_ce_ratio(1.0);
        }
        else
        {
            update_global_ce_ratio(0.0);
        }
        inbound_msgs_.push_back(ims);
        // Handle grant request - send grant immediately in this class
        handle_grant_request(r2p2_hdr);
        Packet::free(pkt);
        return;
    }
    else if (r2p2_hdr->msg_type() == hdr_r2p2::GRANT)
    {
        if (do_trace_)
            trace_state("rgr", r2p2_hdr->credit());
        handle_grant(r2p2_hdr);
        Packet::free(pkt);
        return;
    }
    else if (r2p2_hdr->msg_type() == hdr_r2p2::REQRDY)
    {
        if (do_trace_)
            trace_state("rrr", r2p2_hdr->credit());
        slog::log4(debug_, this_addr_, "Received REQRDY. Credit =", r2p2_hdr->credit());
    }
    else if (r2p2_hdr->msg_type() == hdr_r2p2::REQUEST || r2p2_hdr->msg_type() == hdr_r2p2::REPLY)
    {
        // if this packet belongs to a request (but not req0) or a reply, this instance must
        // check if this is the first pkt of the uRPC and if so, send a grant
        // 1/8/21, note: first_urpc is false for req0 packets.
        bool single_packet_msg = is_single_packet_msg(r2p2_hdr);
        slog::log5(debug_, this_addr_, "Received REQ/REP packet. Is single packet?", single_packet_msg);
        if (!single_packet_msg)
        {
            if (r2p2_hdr->msg_type() == hdr_r2p2::REQUEST)
                slog::log5(debug_, this_addr_, "Received pkt of a multipkt REQUEST");
            else
                slog::log5(debug_, this_addr_, "Received pkt of a multipkt REPLY");
            uniq_req_id_t req_id = std::make_tuple(r2p2_hdr->cl_addr(),
                                                   r2p2_hdr->cl_thread_id(),
                                                   r2p2_hdr->req_id());
            slog::log5(debug_, this_addr_, "REQ/REP unique req_id: ", r2p2_hdr->cl_addr(), r2p2_hdr->cl_thread_id(), r2p2_hdr->req_id());
            InboundMsgState *ims = find_ims(req_id);
            if (ims == NULL)
            {
                slog::log4(debug_, this_addr_, "Received REQ0");
                // New unique message, create state, assert that this is the first packet (REQ0)
                assert(r2p2_hdr->first());
                // also, since servers must send a grant request, the state for the reply
                // must have already been established
                assert(r2p2_hdr->msg_type() != hdr_r2p2::REPLY);
                ims = new InboundMsgState();
                ims->req_id_ = req_id;
                ims->pkts_expected_ = r2p2_hdr->pkt_id() + 1; // NOT correct if the packet is part of a reply
                ims->data_pkts_received_ = 1;
                ims->pkts_uncredited_ = ims->pkts_expected_ - ims->data_pkts_received_;
                ims->pkts_received_ += 1;
                if (ecn_capable_ && hdr_flags::access(pkt)->ect() && hdr_flags::access(pkt)->ce())
                {
                    slog::log5(debug_, this_addr_, "REQ0 experienced congestion!!. Will set the marked ratio estimate to", ecn_init_slash_mul_);
                    ims->pkts_marked_ += 1;
                    ims->marked_ratio_estim_ = ecn_init_slash_mul_;
                    update_global_ce_ratio(1.0);
                }
                else
                {
                    update_global_ce_ratio(0.0);
                }
                inbound_msgs_.push_back(ims);

                hdr_ip *ip_hdr = hdr_ip::access(pkt);
                slog::log5(debug_, this_addr_, "TTL of received REQ0 =", ip_hdr->ttl(), "source addr =", r2p2_hdr->cl_addr());
            }
            else
            {
                // There is already state for this unique message id
                ims->data_pkts_received_++;
                ims->pkts_received_++;
                // For non first() packers
                if (ecn_capable_ && hdr_flags::access(pkt)->ect() && hdr_flags::access(pkt)->ce())
                {
                    slog::log5(debug_, this_addr_, "REQN/REPN", r2p2_hdr->app_level_id(), "experienced congestion!! ratio=", ims->marked_ratio_estim_);
                    ims->pkts_marked_ += 1;
                    double contrib = 1.0;
                    ims->marked_ratio_estim_ = (1.0 - ce_new_weight_) * ims->marked_ratio_estim_ + ce_new_weight_ * contrib;
                    update_global_ce_ratio(1.0);
                }
                else
                {
                    if (ecn_capable_)
                    {
                        ims->marked_ratio_estim_ = (1.0 - ce_new_weight_) * ims->marked_ratio_estim_;
                    }
                    update_global_ce_ratio(0.0);
                }
            }
            // calculate the number of credits to be granted
            int pkts_remaining = ims->pkts_expected_ - ims->data_pkts_received_;
            slog::log7(debug_, this_addr_, "Pkts remaining=", pkts_remaining);
            assert(pkts_remaining >= 0);
            if (pkts_remaining == 0)
            {
                // the message has been entirely received
                assert(ims);
                delete ims;
                inbound_msgs_.remove(ims);
            }
            // req0 pkts have first_urpc() set to 0
            else if (r2p2_hdr->first_urpc() && ims->pkts_uncredited_ > 0)
            {
                // Who needs the grant? A client (request) or a server (reply)
                int32_t daddr = r2p2_hdr->msg_type() == hdr_r2p2::REQUEST ? r2p2_hdr->cl_addr() : r2p2_hdr->sr_addr();
                // if (ecn_capable_ && hdr_flags::access(pkt)->ect() && hdr_flags::access(pkt)->ce() && r2p2_hdr->first())
                // {
                //     slog::log5(debug_, this_addr_, "REQ0 experienced congestion!! ");
                //     ims->pkts_marked_ += 1;
                //     ims->marked_ratio_estim_ = 0.5;
                // }
                send_grant(r2p2_hdr, daddr);
            }
        }
        else
        {
            if (r2p2_hdr->msg_type() == hdr_r2p2::REQUEST)
                slog::log5(debug_, this_addr_, "Received the pkt of a single-packet REQUEST");
            else
                slog::log5(debug_, this_addr_, "Received the pkt of a single-packet REPLY");
        }
    }
    assert(r2p2_hdr->msg_type() != hdr_r2p2::FREEZE);
    assert(r2p2_hdr->msg_type() != hdr_r2p2::UNFREEZE);
    assert(r2p2_hdr->msg_type() != hdr_r2p2::R2P2_FEEDBACK);
    assert(r2p2_hdr->msg_type() != hdr_r2p2::GRANT);
    assert(r2p2_hdr->msg_type() != hdr_r2p2::GRANT_REQ);
    r2p2_layer_->recv(pkt, h);
}

void R2p2CCMicro::handle_grant_request(hdr_r2p2 *const r2p2_hdr)
{
    slog::log4(debug_, this_addr_, "R2p2CCMicro::handle_grant_request()");
    // A server wants to send the response to this client.
    // grant request must carry the total number of
    // In this class, only servers send grant_requests
    assert(r2p2_hdr->sr_addr() != this_addr_); // make sure this is not the server (bcs the server asked)
    send_grant(r2p2_hdr, r2p2_hdr->sr_addr());
}

void R2p2CCMicro::send_grant(hdr_r2p2 *const r2p2_hdr, int32_t daddr)
{
    throw std::runtime_error("R2p2CCMicro::send_grant() not supported"); // headers are wrong
    // repetitive but I want to make this function independent for subclasses
    // (w/o changing it bcs it is a hassle)
    slog::log4(debug_, this_addr_, "R2p2CCMicro::send_grant()");
    uniq_req_id_t req_id = std::make_tuple(r2p2_hdr->cl_addr(),
                                           r2p2_hdr->cl_thread_id(),
                                           r2p2_hdr->req_id());
    InboundMsgState *ims = find_ims(req_id);
    assert(ims);
    int credit_bytes = 0;
    if (ims->pkts_uncredited_ > 0)
    {
        credit_bytes = calc_credit(*r2p2_hdr, ims);
        slog::log4(debug_, this_addr_, "credit bytes calculated = ", credit_bytes);
        // assert(bytes_to_pkts(credit_bytes, false) > 0);
    }
    else
    {
        throw std::invalid_argument("pkts_to_be_credited must be > 0");
    }
    // slog::log4(debug_, this_addr_, "Sending GRANT credit / pkts_uncredited_", bytes_to_pkts(credit_bytes, false), "/", ims->pkts_uncredited_);
    // ims->pkts_uncredited_ -= bytes_to_pkts(credit_bytes, false);
    assert(ims->pkts_uncredited_ >= 0);
    if (ims->pkts_uncredited_ == 0)
    {
        slog::log4(debug_, this_addr_, "all packets are credited. req_id:",
                   r2p2_hdr->cl_addr(), r2p2_hdr->cl_thread_id(), r2p2_hdr->req_id());
    }
    hdr_r2p2 hdr = *r2p2_hdr;
    hdr.msg_type() = hdr_r2p2::GRANT;
    hdr.pkt_id() = 0;
    hdr.credit() = credit_bytes;
    forward_grant(hdr, daddr);
}

int R2p2CCMicro::calc_credit(hdr_r2p2 &r2p2_hdr, InboundMsgState *ims)
{
    throw std::runtime_error("R2p2CCMicro::calc_credit() not supported"); // headers are wrong
    return 1;
    // return std::min(full_pkts_to_bytes(ims->pkts_uncredited_, false), urpc_sz_bytes_);
}

void R2p2CCMicro::forward_grant(hdr_r2p2 &r2p2_hdr, int32_t daddr)
{
    throw std::runtime_error("R2p2CCMicro::forward_grant() not supported"); // headers are wrong
    slog::log4(debug_, this_addr_, "R2p2CCMicro::forward_grant()");
    // r2p2_hdr.credit() = bytes_to_pkts(r2p2_hdr.credit(), false);
    assert(r2p2_hdr.credit() > 0);
    forward_to_transport(std::make_tuple(r2p2_hdr, GRANT_MSG_SIZE, daddr), MsgTracerLogs());
}

void R2p2CCMicro::handle_grant(hdr_r2p2 *const r2p2_hdr)
{
    slog::log4(debug_, this_addr_, "R2p2CCMicro::handle_grant(). Credit =", r2p2_hdr->credit());
    OutboundMsgState *oms = find_oms(std::make_tuple(r2p2_hdr->cl_addr(),
                                                     r2p2_hdr->cl_thread_id(),
                                                     r2p2_hdr->req_id()));
    if (oms == NULL)
    {
        slog::log4(debug_, this_addr_, "Sender received grant for non-existant outbound message "
                                       "(the message state may have never been created or may have been deleted)."
                                       " Unique req_id: ",
                   r2p2_hdr->cl_addr(), r2p2_hdr->cl_thread_id(), r2p2_hdr->req_id());
        throw std::invalid_argument("");
    }
    // found the message that the grant is for, update its state
    assert(oms->pkts_granted_ == 0);
    assert(oms->pkts_left_ > 0);
    oms->pkts_granted_ = r2p2_hdr->credit();
    forward_granted();
}

// TODO: this is bad. this function and the caller are not separated properly.
// ...essentially, if daddr==-1, the caller sends a req0.
// if r2p2-client sends a REQ0 msg, then payload is the size
void R2p2CCMicro::send_to_transport(hdr_r2p2 &r2p2_hdr, int payload, int32_t daddr)
{
    throw std::runtime_error("R2p2CCMicro::send_to_transport() not supported"); // headers are incorrect
    assert(daddr != -1);
    if (do_trace_)
        trace_state("snd", 0.0);
    slog::log4(debug_, this_addr_, "R2p2CCMicro::send_to_transport(). Msg type:", r2p2_hdr.msg_type(), "Destination", daddr);
    bool is_multi_pkt_req_but_not_req0 = is_from_multipkt_req_not_req0(&r2p2_hdr);
    bool is_multi_pkt_rep = is_from_multipkt_reply(&r2p2_hdr);
    if (is_multi_pkt_req_but_not_req0 || is_multi_pkt_rep)
    {
        // NOT a single packet msg => must not forward immediately
        uniq_req_id_t req_id = std::make_tuple(r2p2_hdr.cl_addr(),
                                               r2p2_hdr.cl_thread_id(),
                                               r2p2_hdr.req_id());
        assert(find_oms(req_id) == NULL);
        OutboundMsgState *msg_state = new OutboundMsgState();
        msg_state->req_id_ = req_id;
        msg_state->r2p2_hdr_ = new hdr_r2p2(r2p2_hdr);
        msg_state->daddr_ = daddr;
        assert(daddr != -1);
        msg_state->bytes_left_ = payload;
        // msg_state->pkts_left_ = bytes_to_pkts(payload, true);
        msg_state->pkts_granted_ = 0;
        msg_state->msg_active_ = 0;
        if (r2p2_hdr.msg_type() == hdr_r2p2::REQUEST)
        {
            // Some request bytes are already granted (this instance received a REQRdy for this request)
            // The REQRDY carries the number of credited pkts
            // REQN packets also carry the number of packets that where credited by the REQRDY (in credit field)
            msg_state->pkts_granted_ = r2p2_hdr.credit();
            msg_state->msg_active_ = 1;
        }
        outbound_msgs_.push_back(msg_state);

        update_outbound();
        return;
    }

    if (r2p2_hdr.msg_type() == hdr_r2p2::REQRDY)
    {
        send_reqrdy(r2p2_hdr, payload, daddr);
        return;
    }

    // FEEDBACK, REQ0 end up here. None of them are uRPCs
    r2p2_hdr.first_urpc() = false;
    forward_to_transport(std::make_tuple(r2p2_hdr, payload, daddr), MsgTracerLogs());
}

// handled separately so that sub classes can customize
void R2p2CCMicro::send_reqrdy(hdr_r2p2 &r2p2_hdr, int nbytes, int32_t daddr)
{
    throw std::runtime_error("R2p2CCMicro::send_reqrdy() not supported"); // headers are incorrect
    slog::log4(debug_, this_addr_, "R2p2CCMicro::send_reqrdy()");
    r2p2_hdr.first_urpc() = false;
    // how much credit should this reqready packet provide...
    uniq_req_id_t req_id = std::make_tuple(r2p2_hdr.cl_addr(),
                                           r2p2_hdr.cl_thread_id(),
                                           r2p2_hdr.req_id());
    InboundMsgState *ims = find_ims(req_id);
    assert(ims);
    assert(ims->pkts_uncredited_ > 0);
    int credit_pkts = 1;
    // int credit_pkts = bytes_to_pkts(calc_credit(r2p2_hdr, ims), false);
    assert(credit_pkts > 0);
    slog::log4(debug_, this_addr_, "Sending REQRDY credit / pkts_uncredited_", credit_pkts, "/", ims->pkts_uncredited_);
    ims->pkts_uncredited_ -= credit_pkts;
    assert(ims->pkts_uncredited_ >= 0);
    if (ims->pkts_uncredited_ == 0)
    {
        slog::log4(debug_, this_addr_, "all packets are credited. req_id:",
                   r2p2_hdr.cl_addr(), r2p2_hdr.cl_thread_id(), r2p2_hdr.req_id());
    }
    r2p2_hdr.credit() = credit_pkts;
    forward_reqrdy(r2p2_hdr, nbytes, daddr);
}

// handled separately so that sub classes can customize
void R2p2CCMicro::forward_reqrdy(hdr_r2p2 &r2p2_hdr, int nbytes, int32_t daddr)
{
    slog::log4(debug_, this_addr_, "R2p2CCMicro::forward_reqrdy()");
    forward_to_transport(std::make_tuple(r2p2_hdr, nbytes, daddr), MsgTracerLogs());
}

void R2p2CCMicro::update_outbound()
{
    slog::log5(debug_, this_addr_, "R2p2CCMicro::update_outbound()");
    activate_msgs();
    forward_granted();
}

void R2p2CCMicro::forward_granted()
{
    throw std::runtime_error("R2p2CCMicro::forward_granted() not supported"); // headers are incorrect
    slog::log5(debug_, this_addr_, "R2p2CCMicro::forward_granted()");
    for (auto it = outbound_msgs_.begin(); it != outbound_msgs_.end();)
    {
        if ((*it)->pkts_granted_ > 0)
        {
            int payload_pkts = (*it)->pkts_granted_ > (*it)->pkts_left_ ? (*it)->pkts_left_ : (*it)->pkts_granted_;
            int payload_bytes = std::min(payload_pkts * (PACKET_SIZE - HEADERS_SIZE), (*it)->bytes_left_);
            hdr_r2p2 hdr = *(*it)->r2p2_hdr_;
            hdr.first_urpc() = true;
            hdr.umsg_id() = umsg_cnt_++;
            if (pace_uplink_)
            {
                // pacing REQN and REPLY pkts
                hdr.prio() = 0; // all msgs (REQS and REPS) have the same priority
                assert(0);
                uplink_queue_->enque(std::make_tuple(hdr, payload_bytes, (*it)->daddr_));
            }
            else
            {
                forward_to_transport(std::make_tuple(hdr, payload_bytes, (*it)->daddr_), MsgTracerLogs());
            }
            (*it)->pkts_granted_ = 0;
            (*it)->pkts_left_ -= payload_pkts;
            (*it)->bytes_left_ -= payload_pkts * (PACKET_SIZE - HEADERS_SIZE);
            // NOT compatible with retransmission (all sent, delete state).
            // But it is not clear where retransmissions should be handled.
            if ((*it)->pkts_left_ == 0)
            {
                assert((*it)->bytes_left_ <= 0);
                // delete state and list entry
                delete (*it)->r2p2_hdr_;
                delete (*it);
                it = outbound_msgs_.erase(it);
            }
            else
            {
                assert((*it)->bytes_left_ > 0);
                assert((*it)->pkts_left_ > 0);
                ++it;
                continue; // ugly
            }
        }
        ++it;
    }
}

void R2p2CCMicro::activate_msgs()
{
    slog::log5(debug_, this_addr_, "R2p2CCMicro::activate_msgs()");
    for (auto it = outbound_msgs_.begin(); it != outbound_msgs_.end(); ++it)
    {
        if ((*it)->msg_active_ == 0)
        {
            // Careful. Assumes no loss of the grant request
            (*it)->msg_active_ = 1;
            assert((*it)->r2p2_hdr_->pkt_id() == (*it)->pkts_left_);
            send_grant_req((*it)->r2p2_hdr_, (*it)->daddr_);
        }
    }
}

void R2p2CCMicro::send_grant_req(hdr_r2p2 *const r2p2_hdr, int32_t daddr)
{
    slog::log4(debug_, this_addr_, "R2p2CCMicro::send_grant_req()");
    hdr_r2p2 hdr = *r2p2_hdr;
    hdr.msg_type() = hdr_r2p2::GRANT_REQ;
    hdr.pkt_id() = r2p2_hdr->pkt_id(); // pointing this out. The grant_req receiver knows how much credit this sender needs
    forward_to_transport(std::make_tuple(hdr, GRANT_REQ_MSG_SIZE, daddr), MsgTracerLogs());
}

void R2p2CCMicro::forward_to_transport(packet_info_t pkt_info, MsgTracerLogs &&logs)
{
    int32_t daddr = std::get<2>(pkt_info);
    int nbytes = std::get<1>(pkt_info);
    hdr_r2p2 r2p2_hdr = std::get<0>(pkt_info);
    slog::log5(debug_, this_addr_, "R2p2CCMicro::forward_to_transport(). daddr:", daddr,
               "req_id:", r2p2_hdr.req_id(), "first():", r2p2_hdr.first());
    assert(daddr != -1);
    // Note: if deque_prio_ is false in the sim config then this setting has no effect
    // Note: can selectively enable deque_prio_ just for host uplinks. Check for r2p2_host_uplink_prio
    int prio = 7; // define prios somewhere
                  // if (is_req0_of_multipkt(&r2p2_hdr) ||
    if (r2p2_hdr.msg_type() == hdr_r2p2::GRANT_REQ ||
        r2p2_hdr.msg_type() == hdr_r2p2::REQRDY ||
        r2p2_hdr.msg_type() == hdr_r2p2::GRANT ||
        r2p2_hdr.msg_type() == hdr_r2p2::RESEND ||
        r2p2_hdr.resend_bytes() > 0 )
    {
        // classes of packets that are prioritised in the network (irrelevant to uplink which came before)
        // or not
        prio = 0;
    }
    // A random fid is equivalent to different hash per pkt_info
    int fid = -2;
    if (single_path_per_msg_)
    {
        fid = FID_TO_REQID;
    }
    else if (pace_uplink_)
    {
        // fid is the umsg_id because this function is called for each packet. All pakcets of the
        // umsg/urpc should hame the same fid -> follow the same path.
        fid = r2p2_hdr.umsg_id();
    }
    else
    {
        fid = rand() % 200;
    }
    assert(fid != -2);
    try
    {
        slog::log6(debug_, this_addr_, "R2p2CCMicro::forward_to_transport(). Forwarding to daddr:", daddr);
        r2p2_agents_.at(daddr)
            ->sendmsg(nbytes, r2p2_hdr, std::move(logs), fid, prio, -1, -1, ecn_capable_);
    }
    catch (const std::out_of_range &e)
    {
        slog::error(debug_, this_addr_, "R2p2CCMicro::forward_to_transport() cannot find agent for remote address:", daddr);
        throw;
    }
}

/**
 * Each message (request/reply) is uniquely identided by uniq_req_id_t (<client_addr, thread_id, req_id>)
 * Assuming that a client does not send rpcs to itself (so that there are duplicates in the list)
 */
R2p2CCMicro::OutboundMsgState *R2p2CCMicro::find_oms(const uniq_req_id_t &req_id)
{
    slog::log5(debug_, this_addr_, "R2p2CCMicro::find_oms()");
    for (auto it = outbound_msgs_.begin(); it != outbound_msgs_.end(); ++it)
    {
        if ((*it)->req_id_ == req_id)
        {
            return *it;
        }
    }
    return NULL;
}

/**
 * Assuming that a client does not send rpcs to itself (so that there are duplicates in the list)
 */
R2p2CCMicro::InboundMsgState *R2p2CCMicro::find_ims(const uniq_req_id_t &req_id)
{
    slog::log5(debug_, this_addr_, "R2p2CCMicro::find_ims()");
    for (auto it = inbound_msgs_.begin(); it != inbound_msgs_.end(); ++it)
    {
        if ((*it)->req_id_ == req_id)
        {
            return *it;
        }
    }
    slog::log5(debug_, this_addr_, "Did not find ims");
    return NULL;
}

void R2p2CCMicro::packet_received(Packet *const pkt)
{
    slog::log5(debug_, this_addr_, "R2p2CCMicro::packet_received()");
    deduce_link_state(pkt);
    if (!poll_started_)
    {
        poll();
        poll_started_ = true;
    }
}

bool R2p2CCMicro::deduce_link_state(Packet *const pkt)
{
    double now = Scheduler::instance().clock();
    bool state_changed = false;
    double distance = now - last_pkt_arival_;
    int total_pkt_size = 0;
    if (pkt == NULL)
    {
        total_pkt_size = MAX_ETHERNET_FRAME_ON_WIRE; // worst case
    }
    else
    {
        total_pkt_size = hdr_cmn::access(pkt)->size() + R2P2_ALL_HEADERS_SIZE + INTER_PKT_GAP_SIZE + ETHERNET_PREAMBLE_SIZE;
    }
    double trans_time = bytes_to_seconds(total_pkt_size, link_speed_gbps_ * 1000.0 * 1000.0 * 1000.0);
    if (distance > 1.1 * trans_time)
    {
        // Packet did not arrive right after the previous pkt
        // => link is not busy
        if (link_busy_)
        {
            last_link_busy_ = last_pkt_arival_;
            state_changed = true;
        }
        link_busy_ = false;
    }
    else
    {
        if (!link_busy_)
        {
            last_link_idle_ = last_pkt_arival_;
            state_changed = true;
        }
        link_busy_ = true;
    }

    if (pkt != NULL)
        last_pkt_arival_ = now;
    return state_changed;
}

void R2p2CCMicro::update_global_ce_ratio(double new_event)
{
    global_marked_ratio_ = (1.0 - ce_new_weight_) * global_marked_ratio_ + ce_new_weight_ * new_event;
}

void R2p2CCMicro::poll()
{
    slog::log7(debug_, this_addr_, "R2p2CCMicro::poll()");
    if (do_trace_)
    {
        num_polls_++;
        if (num_polls_ % 10 == 0)
            trace_state("pol", -1);
    }
    bool state_changed = deduce_link_state(NULL);
    // capture busy intervals
    if (state_changed && !link_busy_ && do_trace_)
    {
        // Capture the previous busy interval
        double busy_interval_us = (Scheduler::instance().clock() - last_link_idle_) * 1000.0 * 1000.0;
        trace_state("biv", busy_interval_us);
    }

    poll_timer_->resched(poll_interval_);
}

void R2p2CCMicro::trace_state(std::string event, double a_double)
{
    std::vector<std::string> vars;
    vars.push_back(event);
    vars.push_back(std::to_string(this_addr_));
    vars.push_back(std::to_string(uplink_queue_->size()));
    vars.push_back(std::to_string(uplink_queue_->num_uniq_msgid()));
    vars.push_back(std::to_string(uplink_queue_->num_destinations()));
    vars.push_back(std::to_string(outbound_msgs_.size()));
    vars.push_back(std::to_string(a_double));
    vars.push_back(std::to_string(global_marked_ratio_));
    tracer_->trace(vars);
}

int R2p2CCMicro::command(int argc, const char *const *argv)
{
    Tcl &tcl = Tcl::instance();

    if (argc == 3)
    {
        // Let the cc module know about the router (for hack)
        if (strcmp(argv[1], "attach-router") == 0)
        {
            R2p2Router *r2p2_router = (R2p2Router *)TclObject::lookup(argv[2]);
            if (!r2p2_router)
            {
                tcl.resultf("no such R2P2 router", argv[2]);
                return (TCL_ERROR);
            }
            r2p2_router_ = r2p2_router;
            // std::cout << "Attached router " << r2p2_router_->this_addr_ << " to me: " << this_addr_ << ". Can see budget is " << r2p2_router_->pooled_sender_credit_bytes_ << std::endl;
            return (TCL_OK);
        }
    }
    return (R2p2CCGeneric::command(argc, argv));
}

void PollTimer::expire(Event *e)
{
    cc_module_->poll();
}
