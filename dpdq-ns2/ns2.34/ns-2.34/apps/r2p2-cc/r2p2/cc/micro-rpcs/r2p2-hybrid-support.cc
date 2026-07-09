#include "r2p2-hybrid-support.h"
#include <algorithm>
#include <set>

//////////////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////Common///////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////////////////////

int hysup::add_header_overhead(int payload, bool skip_last_pkt_check)
{
    int w_hdrs = (payload / MAX_R2P2_PAYLOAD) * (MAX_R2P2_PAYLOAD + R2P2_ALL_HEADERS_SIZE + INTER_PKT_GAP_SIZE + ETHERNET_PREAMBLE_SIZE);
    if ((payload % MAX_R2P2_PAYLOAD > 0) && !(skip_last_pkt_check))
    {
        int rem = ((payload % MAX_R2P2_PAYLOAD) + R2P2_ALL_HEADERS_SIZE + INTER_PKT_GAP_SIZE + ETHERNET_PREAMBLE_SIZE);
        w_hdrs += std::max(rem, MIN_ETHERNET_FRAME_ON_WIRE);
    }
    return w_hdrs;
}

int hysup::remove_header_overhead(int size)
{
    // if size does not map to integer packets, the last pkt is ignored
    int wo_hdrs = (size / MAX_ETHERNET_FRAME_ON_WIRE) * MAX_R2P2_PAYLOAD;
    return wo_hdrs;
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////Sender////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////Receivers////////////////////////////////////////////////////////

void hysup::Receivers::on_create(int32_t daddr, hysup::ReceiverState *state, int32_t this_addr)
{
    this_addr_ = this_addr;
    slog::log6(debug_, this_addr_, "hysup::Receivers::on_create() daddr", daddr);
    PolicyState ps = PolicyState();
    ps.rcver_state_ = state;
    rcver_policy_states_[daddr] = ps;
}

size_t hysup::Receivers::num_receivers_owing_credit()
{
    size_t num = 0;
    for (const auto rcver : remotes_)
    {
        uint32_t cr_expected = rcver.second->credit_expected();
        if (cr_expected > 0)
            num++;
    }
    return num;
}

size_t hysup::Receivers::num_outbound()
{
    size_t num = 0;
    for (const auto rcver : remotes_)
    {
        num += rcver.second->num_outbound();
    }
    return num;
}

uint64_t hysup::Receivers::get_total_available_credit()
{
    uint64_t num = 0;
    for (const auto rcver : remotes_)
    {
        num += rcver.second->avail_credit_bytes_;
    }
    return num;
}

uint64_t hysup::Receivers::get_available_credit(size_t receiver)
{
    auto it = remotes_.find(receiver);
    if (it != remotes_.end())
    {
        return it->second->avail_credit_bytes_;
    }
    else
    {
        return 0;
    }
}

size_t hysup::Receivers::get_msg_srpt_order(OutboundMsgState *msg_state)
{
    OutboundMsgs outbound_snap = OutboundMsgs(0, 0);
    for (const auto rcver : remotes_)
    {
        OutboundMsgs *this_receivers_msgs = rcver.second->get_outbound_msgs();
        std::vector<OutboundMsgState *> *msgs = this_receivers_msgs->get_msgs_states();
        for (const auto out_msg_state : *msgs)
        {
            outbound_snap.append(out_msg_state);
        }
    }
    return outbound_snap.msg_order_size(msg_state);
}

hysup::OutboundMsgState *hysup::Receivers::find_outbound_msg(int32_t receiver, uniq_req_id_t req_id)
{
    hysup::OutboundMsgState *msg = nullptr;
    try
    {
        msg = remotes_.at(receiver)->find_outbound_msg(req_id);
    }
    catch (const std::out_of_range &e)
    {
        slog::error(debug_, this_addr_, "hysup::Receivers::find_outbound_msg(): Receiver does not exist\n", e.what());
    }
    return msg;
}

void hysup::Receivers::add_outbound_msg(int32_t receiver, hysup::OutboundMsgState *msg_state)
{
    try
    {
        remotes_.at(receiver)->add_outbound_msg(msg_state);
    }
    catch (const std::out_of_range &e)
    {
        slog::error(debug_, this_addr_, "hysup::Receivers::add_outbound_msg(): Receiver does not exist\n", e.what());
    }
}

void hysup::Receivers::update_policy_state(int32_t this_addr, int sender_policy)
{
    assert(remotes_.size() > 0);
    this_addr_ = this_addr;

    slog::log5(debug_, this_addr_, "Before update_policy_state");
    for (auto &kv : rcver_policy_states_)
    {
        slog::log6(debug_, this_addr_, "rcver:", kv.second.rcver_state_->rcver_addr_,
                   "deficit:", kv.second.deficit_, "quantum:", kv.second.quantum_);
    }

    /* Identify the receivers to which data can be forwarded to later update the quantum */
    // TODO: This is highly non-scalable with the number of receivers...
    int num_can_send = 0;
    int strongest_prio_flow = MAXINT; // the value of the msg that will be given the high_prio flag
    int strongest_prio = MAXINT;
    if (sender_policy == 2)
        strongest_prio = 0;

    int32_t rcver_with_highest_prio = -1;
    int32_t rcver_with_highest_prio_flow = -1;
    for (auto &kv : rcver_policy_states_)
    {
        int32_t rcver_addr = kv.first;
        PolicyState &ps = kv.second;
        ReceiverState *rcver_state = ps.rcver_state_;

        rcver_state->want_to_send(ps);
        if (ps.can_send_)
        {
            num_can_send++;
            assert(ps.msg_state_ != nullptr);
            if (sender_policy == 2)
            {
                if (static_cast<int>(ps.rcver_state_->avail_credit_data_bytes_) > strongest_prio)
                {
                    strongest_prio = ps.rcver_state_->avail_credit_data_bytes_;
                    rcver_with_highest_prio = rcver_addr;
                }
            }
            else if (sender_policy == 0 || sender_policy == 1)
            {
                if (static_cast<int>(ps.msg_state_->unsent_bytes_) < strongest_prio)
                {
                    strongest_prio = ps.msg_state_->unsent_bytes_;
                    rcver_with_highest_prio = rcver_addr;
                }
            }
            else
            {
                slog::error(debug_, this_addr_, "Unknown sender policy:", sender_policy);
                throw invalid_argument("Unknown sender policy");
            }
            // this is always SRPT
            if (static_cast<int>(ps.msg_state_->unsent_bytes_) < strongest_prio_flow)
            {
                strongest_prio_flow = ps.msg_state_->unsent_bytes_;
                rcver_with_highest_prio_flow = rcver_addr;
            }
        }
        else
        {
            assert(ps.deficit_ < 0.000001);
        }

        assert(ps.deficit_ >= 0.0);
    }

    if (num_can_send == 0)
    {
        return;
    }

    for (auto &kv : rcver_policy_states_)
    {
        kv.second.num_can_send_ = num_can_send;
    }

    assert(rcver_with_highest_prio >= 0);
    assert(rcver_with_highest_prio_flow >= 0);

    /* Calculate the quantum for each receiver based on the policy */
    /* Note that the srpt flow also gets part of the fair sharing quantum*/
    double fs_quantum = 1.0 / static_cast<double>(num_can_send);
    double srpt_quantum = 1.0;
    // slog::log5(debug_, this_addr_, "update_policy_state, fs_quantum:",
    //            fs_quantum, "srpt_quantum:", srpt_quantum, "num_can_send:", num_can_send);
    double high_prio_quantum = fs_quantum * fs_weight_ + srpt_quantum * srpt_weight_;
    double low_prio_quantum = fs_quantum * fs_weight_;

    assert((high_prio_quantum + (num_can_send - 1) * low_prio_quantum) - 1.0 < 0.00001);
    slog::log5(debug_, this_addr_, "update_policy_state, high_prio_quantum:",
               high_prio_quantum, "low_prio_quantum:", low_prio_quantum,
               "rcver_with_highest_prio:", rcver_with_highest_prio,
               "rcver_with_highest_prio_flow:", rcver_with_highest_prio_flow);

    assert(low_prio_quantum >= 0.0);
    assert(low_prio_quantum <= 1.0);
    assert(high_prio_quantum >= 0.0);
    assert(high_prio_quantum <= 1.0);

    /* Add deficits. IMPORTANT: This way of calculating deficits is not byte-accurate */
    for (auto &kv : rcver_policy_states_)
    {
        int32_t rcver_addr = kv.first;
        // ReceiverState *rcver_state = remotes_.at(rcver_addr);
        PolicyState &ps = kv.second;

        if (ps.can_send_)
        {
            assert(ps.want_to_send_bytes_ > 0);
            assert(ps.deficit_ >= 0);
            assert(ps.msg_state_ != nullptr);
            if (rcver_addr == rcver_with_highest_prio)
            {
                ps.deficit_ += high_prio_quantum;
                ps.quantum_ = high_prio_quantum;

                // // reset receiver window if not reset (sender_algo_ == 3)
                // bool rst = (!(ps.msg_state_->reset_performed_) && ps.msg_state_->sent_anouncement_);
                // rst = rst && (ps.msg_state_->total_bytes_ > 95000 && ps.msg_state_->total_bytes_ < 500000);
                // if (rst)
                // {
                //     rcver_state->set_reset();
                //     ps.msg_state_->reset_performed_ = true;
                // }
            }
            else
            {
                ps.deficit_ += low_prio_quantum;
                ps.quantum_ = low_prio_quantum;
                // ps.priority_flow_ = false;
            }
            if (rcver_addr == rcver_with_highest_prio_flow)
            {
                ps.priority_flow_ = true;
            }
            else
            {
                ps.priority_flow_ = false;
            }
        }
        else
        {
            assert(ps.deficit_ < 0.000001);
        }
    }

    slog::log5(debug_, this_addr_, "After update_policy_state");
    for (auto &kv : rcver_policy_states_)
    {
        slog::log5(debug_, this_addr_, "rcver:", kv.second.rcver_state_->rcver_addr_,
                   "Avail credit:", kv.second.rcver_state_->avail_credit_data_bytes_);
    }
}

/**
 * Select the message with the highest deficit.
 * If none has deficit > 1, re-add the quantum (this is a bit sketchy but should work)
 * TODO: REFACTOR TO SIMPLIFY
 */
const hysup::PolicyState *hysup::Receivers::select_msg_to_send()
{
    PolicyState *ps_ret = nullptr;
    double highest_deficit = -1.0;
    // slog::log6(debug_, this_addr_, "select_msg_to_send()");
    for (auto &kv : rcver_policy_states_)
    {
        PolicyState &ps = kv.second;
        if (ps.can_send_)
        {
            // slog::log6(debug_, this_addr_, "ps.can_send_");
            if (ps.deficit_ > highest_deficit)
            {
                highest_deficit = ps.deficit_;
                assert(ps.deficit_ >= 0.000001);
                assert(ps.quantum_ >= 0.000001);
                ps_ret = &ps;
                // slog::log6(debug_, this_addr_, "ps.deficit_ > highest_deficit (new is:", highest_deficit);
            }
        }
        else
        {
            // slog::log6(debug_, this_addr_, "not ps.can_send_");
            assert(ps.deficit_ < 0.000001);
        }
    }

    /* TODO: THIS is hard to read */
    if (ps_ret && highest_deficit < 1.0)
    {
        assert(ps_ret != nullptr);
        // slog::log6(debug_, this_addr_, "select_msg_to_send(), highest_deficit:",
        //            highest_deficit, "msg:", std::get<2>(ps_ret->msg_state_->req_id_));
        for (auto &kv : rcver_policy_states_)
        {
            if (kv.second.can_send_)
            {
                // slog::log5(debug_, this_addr_, "Remove..", "rcver:", kv.first, "quant:", kv.second.quantum_,
                //            "highest_deficit", highest_deficit);
                assert(kv.second.quantum_ >= 0.000001);
                kv.second.deficit_ += kv.second.quantum_;
            }
        }
        // slog::log5(debug_, this_addr_, "After recursive update:");
        // for (auto &kv : rcver_policy_states_)
        // {
        //     slog::log6(debug_, this_addr_, "rcver:", kv.second.rcver_state_->rcver_addr_,
        //                "deficit:", kv.second.deficit_, "quantum:", kv.second.quantum_);
        // }
        return select_msg_to_send();
    }
    else
    {
        if (ps_ret)
        {
            // slog::log5(debug_, this_addr_, "select_msg_to_send() returning final decision, highest_deficit:",
            //            highest_deficit, "msg:", std::get<2>(ps_ret->msg_state_->req_id_), "All deficits:");
            // for (auto &kv : rcver_policy_states_)
            // {
            //     slog::log6(debug_, this_addr_, "rcver:", kv.second.rcver_state_->rcver_addr_,
            //                "deficit:", kv.second.deficit_, "quantum:", kv.second.quantum_);
            // }
            ps_ret->deficit_ -= 1.0;
            // slog::log5(debug_, this_addr_, "After subtracting:");
            // for (auto &kv : rcver_policy_states_)
            // {
            //     slog::log6(debug_, this_addr_, "rcver:", kv.second.rcver_state_->rcver_addr_,
            //                "deficit:", kv.second.deficit_, "quantum:", kv.second.quantum_);
            // }
            assert(ps_ret->deficit_ <= 10.0); // I think this should hold.
        }
        return ps_ret;
    }
}

////////////////////////////////////////////////ReceiverState////////////////////////////////////////////////////////

hysup::ReceiverState::ReceiverState(int debug,
                                    int32_t this_addr,
                                    int32_t daddr) : GenericRemoteState(debug, this_addr),
                                                     avail_credit_bytes_(0),
                                                     avail_credit_data_bytes_(0),
                                                     rcver_addr_(daddr),
                                                     out_msgs_(new OutboundMsgs(debug, this_addr)),
                                                     should_reset_(false) {}

hysup::ReceiverState::~ReceiverState() { delete out_msgs_; }
uint32_t hysup::ReceiverState::credit_expected()
{
    int unsent_bytes = out_msgs_->unsent_bytes();
    int credit_still_expected = unsent_bytes - avail_credit_bytes_; /* with headers */
    assert(credit_still_expected >= 0);
    return static_cast<uint32_t>(credit_still_expected);
}

size_t hysup::ReceiverState::num_outbound()
{
    return out_msgs_->size();
}

hysup::OutboundMsgState *hysup::ReceiverState::find_outbound_msg(uniq_req_id_t req_id)
{
    return out_msgs_->find(req_id);
}

void hysup::ReceiverState::add_outbound_msg(hysup::OutboundMsgState *msg_state)
{
    out_msgs_->append(msg_state);
}

void hysup::ReceiverState::remove_outbound_msg(hysup::OutboundMsgState *msg_state)
{
    out_msgs_->remove(msg_state);
}

void hysup::ReceiverState::want_to_send(PolicyState &ps)
{
    // slog::log7(debug_, this_addr_, "want_to_send() for receiver:");
    ps.can_send_ = false;
    ps.msg_state_ = nullptr;
    ps.want_to_send_bytes_ = 0;

    size_t num_outbound_msgs = out_msgs_->size();
    if (num_outbound_msgs == 0)
    {
        // slog::log6(debug_, this_addr_, "SHOULD HAPPEND?1");
        // slog::log7(debug_, this_addr_, "want_to_send() out_msgs_->size() == 0");
        ps.deficit_ = 0.0;
        ps.want_to_send_bytes_ = 0;
        ps.quantum_ = 0.0;
        ps.msg_state_ = nullptr;
        return;
    }

    uint64_t credit_avail = avail_credit_bytes_;
    /* Do SRPT across messages to same receiver. TODO: Generalize */
    out_msgs_->sort_by_fewer_remaining();
    out_msgs_->reset();

    hysup::OutboundMsgState *msg_state = nullptr;
    for (size_t i = 0; i < num_outbound_msgs; i++)
    {

        msg_state = out_msgs_->next();
        // Completed messages may be retained temporarily for RESEND handling,
        // but they must not participate in sender scheduling again.
        if (msg_state->unsent_bytes_ == 0)
        {
            continue;
        }
        uint64_t to_send = std::max(std::min((uint32_t)MAX_R2P2_PAYLOAD, msg_state->unsent_bytes_) + R2P2_ALL_HEADERS_SIZE + INTER_PKT_GAP_SIZE + ETHERNET_PREAMBLE_SIZE, (uint32_t)MIN_ETHERNET_FRAME_ON_WIRE);
        assert(to_send >= MIN_ETHERNET_FRAME_ON_WIRE);
        assert(to_send <= MAX_ETHERNET_FRAME_ON_WIRE);
        if (credit_avail >= to_send)
        {
            if (ps.can_send_)
            {
                /**
                 * Looking for any lower priority messages that should be sent bcs they are not announced
                 * IMPORTANT: We don't want to advertise the size (or other metric) of the low priority message
                 * as then, an ananounced large message may block an anounced small message
                 */

                if (!msg_state->sent_anouncement_)
                {
                    // slog::log5(debug_, this_addr_, "Data: Next msg !msg_state->sent_anouncement_ (", std::get<2>(msg_state->req_id_),
                    //            ") attributes: unsent_bytes_:", msg_state->unsent_bytes_,
                    //            "rcvr:", msg_state->remote_addr_, "credit avail:", credit_avail, "to_send", to_send);
                    ps.can_send_ = true;
                    ps.msg_state_ = msg_state;
                    ps.want_to_send_bytes_ = to_send;
                }
            }
            else
            {
                /* Looking for the fist msg that can be sent */
                // slog::log5(debug_, this_addr_, "Data: Next msg (", std::get<2>(msg_state->req_id_),
                //            ") attributes: unsent_bytes_:", msg_state->unsent_bytes_,
                //            "rcvr:", msg_state->remote_addr_, "credit avail:", credit_avail, "to_send", to_send);
                ps.can_send_ = true;
                ps.msg_state_ = msg_state;
                ps.want_to_send_bytes_ = to_send;
            }
        }
    }
    if (!ps.can_send_)
    {
        /* Equivelent to queue being empty */
        // slog::log6(debug_, this_addr_, "SHOULD HAPPEND?2");
        ps.deficit_ = 0.0;
        ps.want_to_send_bytes_ = 0;
        ps.quantum_ = 0.0;
        ps.msg_state_ = nullptr;
    }
}

////////////////////////////////////////////////OutboundMsgs////////////////////////////////////////////////////////

void hysup::OutboundMsgs::append(OutboundMsgState *const msg)
{
    msgs_.push_back(msg);
}

void hysup::OutboundMsgs::remove(OutboundMsgState *const msg)
{
    assert(msg != nullptr);
    size_t pos = find_pos(msg);
    msgs_.erase(msgs_.begin() + pos);
    if (size() == 0)
    {
        current_ = 0;
    }
    else
    {
        if (current_ > pos)
        {
            current_--;
        }
        else if (current_ == pos)
        {
            current_ = (current_) % size();
        }
    }
}

size_t hysup::OutboundMsgs::find_pos(OutboundMsgState *const msg)
{
    size_t pos = 0;
    for (auto it = msgs_.begin(); it != msgs_.end(); ++it)
    {
        if ((*it)->req_id_ == msg->req_id_)
        {
            return pos;
        }
        pos++;
    }
    throw std::invalid_argument("Can't find_pos() of msg");
}

hysup::OutboundMsgState *hysup::OutboundMsgs::find(const uniq_req_id_t &req_id)
{
    for (auto it = msgs_.begin(); it != msgs_.end(); ++it)
    {
        if ((*it)->req_id_ == req_id)
        {
            return *it;
        }
    }
    return nullptr;
}

hysup::OutboundMsgState *hysup::OutboundMsgs::find_largest_remaining()
{
    if (size() == 0)
    {
        return nullptr;
    }
    uint32_t largest_rem = 0;
    OutboundMsgState *ret = nullptr;
    for (auto it = msgs_.begin(); it != msgs_.end(); ++it)
    {
        if ((*it)->unsent_bytes_ >= largest_rem)
        {
            largest_rem = (*it)->unsent_bytes_;
            ret = (*it);
        }
    }
    assert(ret != nullptr);
    return ret;
}

hysup::OutboundMsgState *hysup::OutboundMsgs::find_smallest_remaining()
{
    if (size() == 0)
    {
        return nullptr;
    }
    uint32_t sm_rem = MAXINT;
    OutboundMsgState *ret = nullptr;
    for (auto it = msgs_.begin(); it != msgs_.end(); ++it)
    {
        if ((*it)->unsent_bytes_ <= sm_rem)
        {
            sm_rem = (*it)->unsent_bytes_;
            ret = (*it);
        }
    }
    assert(ret != nullptr);
    return ret;
}

hysup::OutboundMsgState *hysup::OutboundMsgs::peek_next()
{
    if (size() == 0)
        throw std::invalid_argument("hysup::OutboundMsgs:: there is no peek_next() because the array is empty");
    hysup::OutboundMsgState *ret = nullptr;
    try
    {
        ret = msgs_.at(current_);
    }
    catch (const std::out_of_range &e)
    {
        slog::error(debug_, this_addr_, "hysup::OutboundMsgs::peek_next(). no message on position", current_, "Total messages:", size());
        throw;
    }
    return ret;
}

hysup::OutboundMsgState *hysup::OutboundMsgs::next()
{
    if (size() == 0)
        throw std::invalid_argument("hysup::OutboundMsgs:: there is no next() because the array is empty");
    hysup::OutboundMsgState *ret = peek_next();
    current_ = (current_ + 1) % size();
    return ret;
}

hysup::OutboundMsgState *hysup::OutboundMsgs::next_highest_credit()
{
    if (size() == 0)
    {
        throw std::invalid_argument("hysup::OutboundMsgs::next_highest_credit there is no next() because the array is empty");
    }
    int avail = 0;
    OutboundMsgState *ret = nullptr;
    for (auto it = msgs_.begin(); it != msgs_.end(); ++it)
    {
        if ((*it)->rcvr_state_->avail_credit_bytes_ >= avail)
        {
            avail = (*it)->rcvr_state_->avail_credit_bytes_;
            ret = (*it);
        }
    }
    assert(ret != nullptr);
    return ret;
}

// TODO: This is not correct as the receiver gets more weight in the rr alog (one slot peer msg)
// The round robin should be per receiver and then by msg
hysup::OutboundMsgState *hysup::OutboundMsgs::smallest_of_next_sender()
{
    // hysup::OutboundMsgState *ret = next_highest_credit(); // TODO: next_highest_credit will break the purpose of the next if
    hysup::OutboundMsgState *ret = next();

    if (!ret->sent_anouncement_)
    {
        /**
         * This is needed such that the first packet of every msg is sent. It is to avoid the following scenario:
         * A small message to rcver A uses self-allocated credit of a larger msg to rcver A. The larger msg is not announced to rcverA
         * and thus the latter does not remove the unsolicited credit from B and SBmax..
         */
        return ret;
    }
    // for this receiver, look if there is a smaller message (slow)
    uint32_t min = ret->unsent_bytes_;
    int32_t rcvr = ret->remote_addr_;
    // slog::log4(debug_, -1, "Normally message that would be returned:", ret->unsent_bytes_, "daddr:", rcvr);
    for (auto it = msgs_.begin(); it != msgs_.end(); ++it)
    {
        OutboundMsgState *msg = *it;
        // slog::log4(debug_, -1, "considering:", msg->unsent_bytes_, "daddr:", msg->remote_addr_);
        if ((rcvr == msg->remote_addr_) && (msg->unsent_bytes_ < min))
        {
            // slog::log4(debug_, -1, "replacing:", msg->unsent_bytes_, "with:", msg->unsent_bytes_, "daddr:", msg->remote_addr_);
            ret = msg;
            min = msg->unsent_bytes_;
        }
    }
    // slog::log4(debug_, -1, "selected message:", ret->unsent_bytes_, "daddr:", ret->remote_addr_);
    return ret;
}

/**
 * Returns the SRPT order of a msg_state among other outbound messages
 */
size_t hysup::OutboundMsgs::msg_order_size(OutboundMsgState const *const msg_state)
{
    if (size() == 0)
    {
        throw std::runtime_error("msg_order_size() called on empty message list");
    }
    uint32_t rem_bytes = msg_state->unsent_bytes_;
    size_t order = 0; // smallest
    for (auto it = msgs_.begin(); it != msgs_.end(); ++it)
    {
        if ((*it)->unsent_bytes_ < rem_bytes)
        {
            order++;
        }
    }
    return order;
}

void hysup::OutboundMsgs::sort_by_fewer_remaining()
{
    std::sort(msgs_.begin(), msgs_.end(), fewer_outbound_bytes_remaining());
}

void hysup::OutboundMsgs::reset()
{
    current_ = 0;
}

int hysup::OutboundMsgs::unsent_bytes()
{
    int unsent_bytes = 0;
    for (OutboundMsgState *msg : msgs_)
    {
        unsent_bytes += add_header_overhead(msg->unsent_bytes_);
    }
    return unsent_bytes;
}

std::string hysup::OutboundMsgs::print_all(int32_t local_addr)
{
    std::string ret = "";
    if (msgs_.size() > 0)
    {
        slog::log4(debug_, local_addr, "~~~~ Outbound Messages ~~~~ ", msgs_.size());
        for (auto it = msgs_.begin(); it != msgs_.end(); ++it)
        {
            OutboundMsgState *msg = *it;
            // meh, log level should be an input..
            slog::log4(debug_, local_addr, "Msg:", std::get<2>(msg->req_id_), "app id:", msg->r2p2_hdr_->app_level_id(),
                       "Active:", msg->active_, "To:", msg->remote_addr_, "Unsent / total:",
                       msg->unsent_bytes_, "/", msg->total_bytes_,
                       "Is req:", msg->is_request_,
                       "credit avail:", msg->rcvr_state_->avail_credit_bytes_);
            ret += std::to_string(std::get<2>(msg->req_id_)) + ": Rem/Tot " + std::to_string(msg->unsent_bytes_) + "/" + std::to_string(msg->total_bytes_) + " Cr " + std::to_string(msg->rcvr_state_->avail_credit_bytes_) + " | ";
        }
        return ret;
    }
    else
    {
        return "None";
    }
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////Receiver////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////////////////////

/**
 * @brief Construct a new hysup:: Inbound Msgs:: Inbound Msgs object
 *
 */
hysup::InboundMsgs::InboundMsgs(int debug, int32_t this_addr) : current_(0),
                                                                debug_(debug),
                                                                this_addr_(this_addr)
{
}

hysup::InboundMsgs::~InboundMsgs() {}

void hysup::InboundMsgs::add(InboundMsgState *const msg)
{
    msgs_.push_back(msg);
}

void hysup::InboundMsgs::remove(InboundMsgState *const msg)
{
    assert(msg != nullptr);
    size_t pos = find_pos(msg);
    delete msg->first_header_;
    delete msg;
    msgs_.erase(msgs_.begin() + pos);
    if (size() == 0)
    {
        current_ = 0;
    }
    else
    {
        if (current_ > pos)
        {
            current_--;
        }
        else if (current_ == pos)
        {
            current_ = (current_) % size();
        }
    }
}

size_t hysup::InboundMsgs::find_pos(InboundMsgState *const msg)
{
    size_t pos = 0;
    for (auto it = msgs_.begin(); it != msgs_.end(); ++it)
    {
        if ((*it)->req_id_ == msg->req_id_)
        {
            return pos;
        }
        pos++;
    }
    throw std::invalid_argument("Can't find_pos() of msg");
}

std::string hysup::InboundMsgs::print_all(int32_t local_addr)
{
    std::string ret = "";
    if (msgs_.size() > 0)
    {
        slog::log4(debug_, local_addr, "~~~~ Inbound Messages ~~~~ ", msgs_.size());
        for (auto it = msgs_.begin(); it != msgs_.end(); ++it)
        {
            InboundMsgState *msg = *it;
            // meh
            slog::log4(debug_, local_addr, "Msg:", std::get<2>(msg->req_id_),
                       "From:", msg->remote_addr_, "Received / Expected:",
                       msg->data_bytes_received_, "/", msg->data_bytes_expected_,
                       "Remaining:", (msg->data_bytes_expected_ - msg->data_bytes_received_),
                       "granted:", msg->data_bytes_granted_, "SBMax:", msg->sender_state_->max_srpb_bytes_, "srpb:", msg->sender_state_->srpb_bytes_,
                       "host marked ratio:", msg->sender_state_->host_marked_ratio_, "Got info:", msg->received_msg_info_);
            ret += std::to_string(std::get<2>(msg->req_id_)) + "<-" + std::to_string(msg->remote_addr_) + ": Rem/Tot " + std::to_string(msg->data_bytes_expected_ - msg->data_bytes_received_) + "/" + std::to_string(msg->data_bytes_expected_) + " SBmax " + std::to_string(msg->sender_state_->max_srpb_bytes_) + " HMRat " + std::to_string(msg->sender_state_->host_marked_ratio_) + " | ";
        }
        return ret;
    }
    else
    {
        return "None";
    }
}

hysup::InboundMsgState *hysup::InboundMsgs::find(const uniq_req_id_t &req_id)
{
    for (auto it = msgs_.begin(); it != msgs_.end(); ++it)
    {
        if ((*it)->req_id_ == req_id)
        {
            return *it;
        }
    }
    return nullptr;
}

hysup::InboundMsgState *hysup::InboundMsgs::peek_next()
{
    if (size() == 0)
        throw std::invalid_argument("hysup::InboundMsgs:: there is no peek_next() because the array is empty");
    hysup::InboundMsgState *ret = nullptr;
    try
    {
        ret = msgs_.at(current_);
    }
    catch (const std::out_of_range &e)
    {
        slog::error(debug_, this_addr_, "hysup::InboundMsgs::peek_next(). no message on position", current_, "Total messages:", size());
        throw;
    }
    return ret;
}

hysup::InboundMsgState *hysup::InboundMsgs::next()
{
    if (size() == 0)
        throw std::invalid_argument("hysup::InboundMsgs:: there is no next() because the array is empty");
    hysup::InboundMsgState *ret = peek_next();
    current_ = (current_ + 1) % size();
    return ret;
}

size_t hysup::InboundMsgs::msg_order_size(InboundMsgState const *const msg_state)
{
    if (size() == 0)
    {
        throw std::runtime_error("msg_order_size() called on empty inbound message list");
    }
    uint32_t rem_bytes = msg_state->data_bytes_expected_ - msg_state->data_bytes_received_;
    size_t order = 0; // smallest
    for (auto it = msgs_.begin(); it != msgs_.end(); ++it)
    {
        if (((*it)->data_bytes_expected_ - (*it)->data_bytes_received_) < rem_bytes)
        {
            if ((*it)->data_bytes_expected_ - (*it)->data_bytes_granted_ > 0)
                order++;
        }
    }
    return order;
}

size_t hysup::InboundMsgs::size()
{
    return msgs_.size();
}

void hysup::InboundMsgs::sort_by_fewer_remaining()
{
    std::sort(msgs_.begin(), msgs_.end(), fewer_inbound_bytes_remaining());
}

void hysup::InboundMsgs::sort_by_fewer_bif()
{
    std::sort(msgs_.begin(), msgs_.end(), fewer_bif());
}

void hysup::InboundMsgs::reset()
{
    current_ = 0;
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////Stats////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////////////////////
std::vector<hysup::Stats *> hysup::Stats::all_stats_ = std::vector<hysup::Stats *>();
int32_t hysup::Stats::static_leader_ = -1; // the host that updates static variables
uint64_t hysup::Stats::num_ticks_ = 0;
uint64_t hysup::Stats::num_data_pkts_sent_ = 0;
uint64_t hysup::Stats::num_data_pkts_of_SRPT_msg_sent_[STATS_NUM_SRPT_MSG_TRACED] = {0};
double hysup::Stats::ratio_data_pkts_of_SRPT_msg_sent_[STATS_NUM_SRPT_MSG_TRACED] = {0.0};
uint64_t hysup::Stats::num_credit_pkts_sent_ = 0;
uint64_t hysup::Stats::num_credit_pkts_of_SRPT_msg_sent_[STATS_NUM_SRPT_MSG_TRACED] = {0};
double hysup::Stats::ratio_credit_pkts_of_SRPT_msg_sent_[STATS_NUM_SRPT_MSG_TRACED] = {0.0};
int hysup::Stats::total_budget_bytes_ = 0;
uint32_t hysup::Stats::total_credit_backlog_ = 0;
uint32_t hysup::Stats::total_bif_rec_ = 0;
double hysup::Stats::total_avg_host_marked_ratio_ = 0;
uint64_t hysup::Stats::uplink_busy_count_ = 0;
uint64_t hysup::Stats::uplink_idle_while_outbound_count_ = 0;
double hysup::Stats::total_ratio_uplink_busy_ = 0;
double hysup::Stats::total_ratio_uplink_idle_while_data_ = 0;

void hysup::Stats::register_stats_instance(hysup::Stats *stats)
{
    all_stats_.push_back(stats);
}

void hysup::Stats::write_static_vars(int32_t addr)
{
    if (static_leader_ == -1) // addr is not available during initialization so it is done here. This is inefficient and branch hints are not available for C++11
    {
        static_leader_ = addr;
    }

    if (addr != static_leader_)
    {
        return;
    }

    for (size_t i = 0; i < STATS_NUM_SRPT_MSG_TRACED; ++i)
    {
        if (num_data_pkts_sent_ > 0)
            ratio_data_pkts_of_SRPT_msg_sent_[i] = (double)(num_data_pkts_of_SRPT_msg_sent_[i]) / num_data_pkts_sent_;
        else
            ratio_data_pkts_of_SRPT_msg_sent_[i] = 1.0;
        if (num_credit_pkts_sent_ > 0)
            ratio_credit_pkts_of_SRPT_msg_sent_[i] = (double)(num_credit_pkts_of_SRPT_msg_sent_[i]) / num_credit_pkts_sent_;
        else
            ratio_credit_pkts_of_SRPT_msg_sent_[i] = 1.0;
    }

    total_budget_bytes_ = 0;
    total_credit_backlog_ = 0;
    total_bif_rec_ = 0;
    total_bif_rec_ = 0;
    total_avg_host_marked_ratio_ = 0.0;
    for (Stats *stats : all_stats_)
    {
        total_budget_bytes_ += stats->budget_bytes_;
        total_credit_backlog_ += stats->granted_bytes_queue_len_;
        total_bif_rec_ += stats->num_bytes_in_flight_rec_;
        total_avg_host_marked_ratio_ += stats->avg_host_marked_ratio_;
    }
    total_avg_host_marked_ratio_ /= all_stats_.size();

    total_ratio_uplink_busy_ = static_cast<double>(uplink_busy_count_) / num_ticks_;
    total_ratio_uplink_idle_while_data_ = static_cast<double>(uplink_idle_while_outbound_count_) / num_ticks_;
}
