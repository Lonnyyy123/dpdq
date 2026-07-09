#include "uplink-queue.h"
#include <iomanip>
#include "simple-log.h"

R2p2UplinkQueue::R2p2UplinkQueue(R2p2CCGeneric *cc_module) : uplink_occupied_(false),
                                                             link_speed_(-1),
                                                             drain_timer_(this),
                                                             cc_module_(cc_module),
                                                             policy_(FCFS),
                                                             last_index_(-1),
                                                             last_dest_indx_(0),
                                                             last_uniqreqid_indx_(0) {}

R2p2UplinkQueue::R2p2UplinkQueue(R2p2CCGeneric *cc_module, double link_speed) : uplink_occupied_(false),
                                                                                link_speed_(link_speed),
                                                                                drain_timer_(this),
                                                                                cc_module_(cc_module),
                                                                                policy_(FCFS),
                                                                                last_index_(-1),
                                                                                last_dest_indx_(0),
                                                                                last_uniqreqid_indx_(0) {}

R2p2UplinkQueue::~R2p2UplinkQueue() {}

void R2p2UplinkQueue::enque(packet_info_t message, int extra_prio)
{
    throw std::invalid_argument("R2p2UplinkQueue not supported");
    hdr_r2p2 r2p2_hdr = std::get<0>(message);
    int payload_sz = std::get<1>(message);
    uniq_req_id_t req_id = std::make_tuple(r2p2_hdr.cl_addr(),
                                           r2p2_hdr.cl_thread_id(),
                                           r2p2_hdr.req_id());
    QueueItem item;
    item.req_id_ = req_id;
    item.pkt_info_ = message;
    item.bytes_left_ = payload_sz;
    item.prio_ = r2p2_hdr.prio();
    item.pkt_id_ = r2p2_hdr.pkt_id();
    item.first_ = true;
    item.urpc_first_ = r2p2_hdr.first_urpc();
    item.is_stopped_ = false;
    item.stopped_until_ = 0.0;
    item.reply_ = r2p2_hdr.msg_type() == hdr_r2p2::REPLY;
    item.extra_prio_ = extra_prio;
    q_.push_back(item);
    auto srch_dst = msgs_per_dest_.find(std::get<2>(item.pkt_info_));
    if (srch_dst != msgs_per_dest_.end())
    {
        ++srch_dst->second;
    }
    else
    {
        msgs_per_dest_[std::get<2>(item.pkt_info_)] = 1;
    }
    auto srch_rid = msgs_per_uniqreqid_.find(item.req_id_);
    if (srch_rid != msgs_per_uniqreqid_.end())
    {
        ++srch_rid->second;
    }
    else
    {
        msgs_per_uniqreqid_[item.req_id_] = 1;
    }

    if (!uplink_occupied_)
    {
        send();
    }
}

void R2p2UplinkQueue::enque(packet_info_t message)
{
    enque(message, -1);
}

void R2p2UplinkQueue::freeze(uniq_req_id_t req_id, double freeze_dur)
{
    double now = Scheduler::instance().clock();
    for (auto it = q_.begin(); it != q_.end(); ++it)
    {
        if (it->req_id_ == req_id)
        {
            it->is_stopped_ = true;
            if (freeze_dur < 0.0)
            {
                it->stopped_indef_ = true;
            }
            else
            {
                it->stopped_indef_ = false;
                it->stopped_until_ = now + freeze_dur;
            }
        }
    }
}

void R2p2UplinkQueue::unfreeze(uniq_req_id_t req_id)
{
    for (auto it = q_.begin(); it != q_.end(); ++it)
    {
        if (it->req_id_ == req_id)
        {
            it->is_stopped_ = false;
            it->stopped_indef_ = false;
        }
    }
}

/**
 * This should be called only when the link is "free" and a new packet
 * transmission should happen.
 */
void R2p2UplinkQueue::send()
{
    if (!q_.empty())
    {
        check_stopped();
        int index = next_message();

        // std::cout << "send " << " index " << index << " q_size " << q_.size() << std::endl;
        if (index == -1)
        {
            // queue is not empty but all messages are stopped
            //  check again in MTU transmission time
            drain_timer_.resched(calc_trans_delay(pkt_size));
            uplink_occupied_ = false;
        }
        else
        {
            QueueItem &item = q_[index];
            hdr_r2p2 r2p2_hdr = std::get<0>(item.pkt_info_);
            if (item.first_)
            {
                item.first_ = false;
                // the header field first should have the value provided by the layer above
                // only when the first packet is to be sent. Else it should be false.
                if (item.reply_)
                {
                    // send the first packet of a reply with high priority
                    r2p2_hdr.prio() = item.extra_prio_;
                }
            }
            else
            {
                r2p2_hdr.first() = false;
                r2p2_hdr.prio() = item.prio_;
            }
            // Poor quality code...
            if (item.urpc_first_)
            {
                item.urpc_first_ = false;
            }
            else
            {
                r2p2_hdr.first_urpc() = false;
            }
            // I think: for the first packet of a queue item, it allows the pkt_id defined
            // by the layer above. For the next, it increments the provided value.
            // TODO: this is ok for req0, reqrdy, reqN but NOT for reply as the first packet
            // in this case the fisrt packet of a burst carries the total num of reply packets.
            // it won't break anything now bcs of no retransmissions.
            r2p2_hdr.pkt_id() = item.pkt_id_++;
            // std::cout << "PACKET id: " << r2p2_hdr.pkt_id() << std::endl;

            int32_t daddr = std::get<2>(item.pkt_info_);
            int bytes_to_send;
            // std::cout << Scheduler::instance().clock() << " > QUEUE: sending packet for: " << r2p2_hdr.cl_addr() << " " << r2p2_hdr.req_id() << " to: " << daddr << " bytes left: " << item.bytes_left_ << std::endl;
            slog::log5(cc_module_->get_debug(), cc_module_->this_addr_, "^ Uplink queue packet frwrd",
                       "msg_type", r2p2_hdr.msg_type(),
                       "req_id", r2p2_hdr.req_id(),
                       "pkt_id", r2p2_hdr.pkt_id(),
                       "Bytes left", item.bytes_left_,
                       "first", r2p2_hdr.first(),
                       "cl_addr", r2p2_hdr.cl_addr(),
                       "sr_addr", r2p2_hdr.sr_addr(),
                       "first_urpc", r2p2_hdr.first_urpc(),
                       "umsg_id", r2p2_hdr.umsg_id());
            if (item.bytes_left_ > max_payload_size_)
            {
                // remaining bytes do not fit in the next packet
                item.bytes_left_ -= max_payload_size_;
                bytes_to_send = max_payload_size_;
                // std::cout << "bytes left: " << item.bytes_left_ << std::endl;
                // std::cout << Scheduler::instance().clock() << " > QUEUE: sending packet for: " << r2p2_hdr.cl_addr() << " " << r2p2_hdr.req_id() << " size: " << bytes_to_send << " to: " << daddr << std::endl;
                cc_module_->forward_to_transport(std::make_tuple(r2p2_hdr, bytes_to_send, daddr), MsgTracerLogs());
            }
            else
            {
                // remaining bytes fit in the next packet
                bytes_to_send = item.bytes_left_;

                cc_module_->forward_to_transport(std::make_tuple(r2p2_hdr, bytes_to_send, daddr), MsgTracerLogs());

                assert(msgs_per_dest_.find(std::get<2>(item.pkt_info_)) != msgs_per_dest_.end());
                assert(msgs_per_uniqreqid_.find(item.req_id_) != msgs_per_uniqreqid_.end());
                try
                {
                    msgs_per_dest_.at(std::get<2>(item.pkt_info_))--;
                    msgs_per_uniqreqid_.at(item.req_id_)--;
                }
                catch (const std::out_of_range &e)
                {
                    std::cerr << e.what() << "Error while reducing the number of msgs per dest/msg_id" << std::endl;
                    throw;
                }
                assert(msgs_per_dest_.at(std::get<2>(item.pkt_info_)) >= 0);
                assert(msgs_per_uniqreqid_.at(item.req_id_) >= 0);
                if (msgs_per_dest_.at(std::get<2>(item.pkt_info_)) == 0)
                {
                    msgs_per_dest_.erase(std::get<2>(item.pkt_info_));
                }
                if (msgs_per_uniqreqid_.at(item.req_id_) == 0)
                {
                    msgs_per_uniqreqid_.erase(item.req_id_);
                }

                // erase item - all bytes trnansmitted
                q_.erase(q_.begin() + index);
            }

            uplink_occupied_ = true;
            drain_timer_.resched(calc_trans_delay(bytes_to_send + headers_size_));
        }
    }
    else
    {
        uplink_occupied_ = false;
    }
}

int R2p2UplinkQueue::next_message()
{
    assert(!q_.empty());
    int index = -1;
    switch (policy_)
    {
    case FCFS:
        index = 0;
        break;
    case RR_MSG:
        index = message_rr();
        break;
    case RR_DEST:
        index = destination_rr();
        break;
    case HIGHEST_PRIO:
        index = find_highest_prio_message();
        break;
    }
    assert(index != -1);

    last_index_ = index;
    return index;
}

int R2p2UplinkQueue::message_rr()
{
    // int index = (last_index_ + 1) % q_.size();
    // Do round robin accross unique message ids (not accross queue items)
    // If there are multiple uMSGs of the same MSG, only the oldest uMSG will be served.
    // Ugly copy paste
    int index = -1;
    int new_index = (last_uniqreqid_indx_ + 1) % num_uniq_msgid();
    int iter = 0;
    uniq_req_id_t msg_uniqreqid;
    for (auto it = msgs_per_uniqreqid_.begin(); it != msgs_per_uniqreqid_.end(); ++it)
    {
        if (new_index == iter)
        {
            msg_uniqreqid = it->first;
            break;
        }
        ++iter;
    }
    last_uniqreqid_indx_ = new_index;
    for (std::size_t i = 0; i < q_.size(); i++)
    {
        if (q_[i].req_id_ == msg_uniqreqid)
        {
            // this message (eg uRPC) has the unique id of the message that should be sent
            // and is the oldest one to do so.
            index = i;
            break;
        }
    }
    slog::log4(cc_module_->get_debug(), cc_module_->this_addr_,
               "message_rr(): index =", index, "dest =", std::get<2>(q_[index].pkt_info_),
               "req_id_ =", std::get<2>(q_[index].req_id_), "bytes left =",
               q_[index].bytes_left_, "num msgs =", q_.size(), "num dests =",
               num_destinations(), "num_uniqreqids =", num_uniq_msgid());
    return index;
}

int R2p2UplinkQueue::destination_rr()
{
    int index = -1;
    int new_index = (last_dest_indx_ + 1) % num_destinations();
    int iter = 0;
    int dest_addr = -1;
    for (auto it = msgs_per_dest_.begin(); it != msgs_per_dest_.end(); ++it)
    {
        if (new_index == iter)
        {
            dest_addr = it->first;
            break;
        }
        ++iter;
    }
    last_dest_indx_ = new_index;
    assert(dest_addr != -1);
    // find the oldest message destined for dest_addr
    for (std::size_t i = 0; i < q_.size(); i++)
    {
        if (std::get<2>(q_[i].pkt_info_) == static_cast<int32_t>(dest_addr))
        {
            // this message is desitned for dest_addr
            index = i;
            break;
        }
    }
    slog::log4(cc_module_->get_debug(), cc_module_->this_addr_,
               "message_rr(): index =", index, "dest =", std::get<2>(q_[index].pkt_info_),
               "req_id_ =", std::get<2>(q_[index].req_id_), "bytes left =",
               q_[index].bytes_left_, "num msgs =", q_.size(), "num dests =",
               num_destinations(), "num_uniqreqids =", num_uniq_msgid());
    return index;
}

/**
 * Returns the index of the highest priority message that is not stopped.
 * If none is found, it returns -1
 */
int R2p2UplinkQueue::find_highest_prio_message()
{
    int max_prio = -1;
    int index = -1;
    for (std::size_t i = 0; i < q_.size(); i++)
    {
        int prio = q_[i].prio_;
        // add logic to unstop if stopped_until_ < now
        if (prio > max_prio && !q_[i].is_stopped_)
        {
            max_prio = prio;
            index = i;
        }
    }
    slog::log4(cc_module_->get_debug(), cc_module_->this_addr_,
               "highest_prio(): index =", index, "dest =", std::get<2>(q_[index].pkt_info_),
               "req_id_ =", std::get<2>(q_[index].req_id_), "bytes left =",
               q_[index].bytes_left_, "num msgs =", q_.size(), "num dests =",
               num_destinations(), "num_uniqreqids =", num_uniq_msgid());
    return index;
}

void R2p2UplinkQueue::check_stopped()
{
    double now = Scheduler::instance().clock();
    for (std::size_t i = 0; i < q_.size(); i++)
    {
        if (!q_[i].stopped_indef_ && q_[i].is_stopped_ && now > q_[i].stopped_until_)
        {
            q_[i].is_stopped_ = false;
        }
    }
}

void R2p2UplinkQueue::set_deque_policy(DeqPolicy policy)
{
    policy_ = policy;
}

void R2p2UplinkQueue::set_link_speed(double link_speed_bps)
{
    slog::log2(cc_module_->get_debug(), cc_module_->this_addr_, "R2p2UplinkQueue::set_link_speed() to:", link_speed_bps);
    link_speed_ = link_speed_bps;
}

size_t R2p2UplinkQueue::size()
{
    return q_.size();
}

size_t R2p2UplinkQueue::num_uniq_msgid()
{
    return msgs_per_uniqreqid_.size();
}

size_t R2p2UplinkQueue::num_destinations()
{
    return msgs_per_dest_.size();
}

void DrainTimer::expire(Event *e)
{
    q_->send();
}