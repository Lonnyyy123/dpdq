#include <iostream>
#include "r2p2-client.h"
#include "r2p2-hdr.h"
#include "simple-log.h"

struct ClientRequestState
{
    ClientRequestState() : req_bytes_left_(0), reply_pkts_to_rec_(0),
                           reply_pkts_recvd_(0), reply_bytes_recvd_(0),
                           msg_creation_time_(-1.0) {}
    int req_bytes_left_;
    int reply_pkts_to_rec_;
    int reply_pkts_recvd_;
    int reply_bytes_recvd_;
    double msg_creation_time_;
};

R2p2Client::R2p2Client(R2p2 *r2p2_layer) : r2p2_layer_(r2p2_layer) {}

R2p2Client::~R2p2Client()
{
    delete r2p2_layer_;
    for (auto it_map = thrd_id_to_pending_reqs_map_.begin();
         it_map != thrd_id_to_pending_reqs_map_.end(); it_map++)
    {
        for (auto it = it_map->second->begin();
             it != it_map->second->end(); it++)
        {
            delete it->second;
        }
        delete it_map->second;
    }
}

void R2p2Client::send_req(int payload, const RequestIdTuple &request_id_tuple)
{
    if (payload < 1)
    {
        throw std::invalid_argument("nbytes must be > 0");
    }
    // TODO: define constants somewhere
    int bytes_in_req0 = REQ0_MSG_SIZE;
    int num_pkts = payload / MAX_R2P2_PAYLOAD;
    if (payload % MAX_R2P2_PAYLOAD > 0)
        num_pkts++;
    bool single_pkt_rpc = num_pkts == 1;
    // TODO: This should not be happening -req0 should be just control
    int reqn_payload = payload - bytes_in_req0;

    if (!single_pkt_rpc)
    {
        num_pkts = reqn_payload / MAX_R2P2_PAYLOAD;
        if (reqn_payload % MAX_R2P2_PAYLOAD > 0)
            num_pkts++;
    }
    else
    {
        num_pkts = 0;
    }
    // check if this thread has sent before
    request_id current_rid;
    int thread_id = request_id_tuple.cl_thread_id_;
    // const on avg
    auto srch = thrd_id_to_req_id_.find(thread_id);
    if (srch != thrd_id_to_req_id_.end())
    {
        current_rid = ++srch->second;
    }
    else
    {
        thrd_id_to_req_id_[thread_id] = 0;
        current_rid = 0;
        thrd_id_to_pending_reqs_map_[thread_id] = new req_id_to_req_state_t();
    }

    ClientRequestState *client_request_state = new ClientRequestState();
    hdr_r2p2 r2p2_hdr;
    r2p2_hdr.first() = true;
    r2p2_hdr.last() = single_pkt_rpc;
    r2p2_hdr.msg_type() = hdr_r2p2::REQUEST;
    r2p2_hdr.policy() = hdr_r2p2::UNRESTRICTED;
    r2p2_hdr.req_id() = current_rid;
    r2p2_hdr.pkt_id() = num_pkts;
    // will be interpreted as: send to r2p2-router
    r2p2_hdr.cl_addr() = request_id_tuple.cl_addr_;
    r2p2_hdr.cl_thread_id() = thread_id;
    r2p2_hdr.app_level_id() = request_id_tuple.app_level_id_;
    r2p2_hdr.sr_addr() = request_id_tuple.sr_addr_;
    r2p2_hdr.sr_thread_id() = request_id_tuple.sr_thread_id_;
    // r2p2_hdr.msg_size_bytes() = reqn_payload;
    r2p2_hdr.msg_creation_time() = request_id_tuple.ts_;

    slog::log4(r2p2_layer_->get_debug(), r2p2_layer_->get_local_addr(),
               "R2p2Client::send_req(). app lvl id:", r2p2_hdr.app_level_id(), "req id:", r2p2_hdr.req_id(), "single pkt?", single_pkt_rpc, "from:", r2p2_hdr.cl_addr(),
               "thread:", r2p2_hdr.cl_thread_id(), ">to:",
               r2p2_hdr.sr_addr(), "thread:", r2p2_hdr.sr_thread_id());
    // if the RPC does not fit in a single packet, the protocol sends a 64 byte packet -
    // given 50 bytes of headers, that leaves 14 bytes of data.
    client_request_state->req_bytes_left_ =
        single_pkt_rpc ? 0 : reqn_payload;
    client_request_state->msg_creation_time_ = request_id_tuple.ts_;
    (*thrd_id_to_pending_reqs_map_.at(thread_id))[current_rid] = client_request_state;

    int32_t daddr = -2; // -1 used to be destination: r2p2 router
    daddr = r2p2_hdr.sr_addr();
    // add the size of all headers here (eth, ip, udp, r2p2)
    r2p2_layer_->send_to_transport(r2p2_hdr, single_pkt_rpc ? payload : bytes_in_req0, daddr);
}

void R2p2Client::handle_req_rdy(hdr_r2p2 &r2p2_hdr, int payload)
{
    slog::log5(r2p2_layer_->get_debug(), r2p2_layer_->get_local_addr(),
               "handle_req_rdy", r2p2_hdr.req_id(),
               "with pkt_id", r2p2_hdr.pkt_id(), "(first",
               r2p2_hdr.first(), ") from server", r2p2_hdr.sr_addr());
    r2p2_hdr.msg_type() = hdr_r2p2::REQUEST;
    // CAREFULL -> pkt_id will carry the prio level.
    // pkt_id == 1 will be assigned to the first REQN packet
    r2p2_hdr.pkt_id() = 1;
    // unecessary
    r2p2_hdr.first() = false;
    ClientRequestState *client_request_state = thrd_id_to_pending_reqs_map_.at(r2p2_hdr.cl_thread_id())->at(r2p2_hdr.req_id());
    int bytes_left = client_request_state->req_bytes_left_;
    r2p2_hdr.msg_creation_time() = client_request_state->msg_creation_time_;
    r2p2_layer_->send_to_transport(r2p2_hdr, bytes_left, r2p2_hdr.sr_addr());
}

void R2p2Client::handle_reply_pkt(hdr_r2p2 &r2p2_hdr, int payload)
{
    bool is_ooo = false;
    slog::log5(r2p2_layer_->get_debug(), r2p2_layer_->get_local_addr(),
               "Handling reply packet with req id", r2p2_hdr.req_id(),
               "with pkt_id", r2p2_hdr.pkt_id(), "(first",
               r2p2_hdr.first(), ") from server", r2p2_hdr.sr_addr());
    ClientRequestState *client_request_state = NULL;
    req_id_to_req_state_t *pending_reqs = NULL;
    auto thrd_it = thrd_id_to_pending_reqs_map_.find(r2p2_hdr.cl_thread_id());
    if (thrd_it == thrd_id_to_pending_reqs_map_.end())
    {
        slog::log3(r2p2_layer_->get_debug(), r2p2_layer_->get_local_addr(),
                   "Ignoring reply for unknown client thread", r2p2_hdr.cl_thread_id(),
                   "req id", r2p2_hdr.req_id(), "from server", r2p2_hdr.sr_addr());
        return;
    }

    pending_reqs = thrd_it->second;
    auto req_it = pending_reqs->find(r2p2_hdr.req_id());
    if (req_it == pending_reqs->end())
    {
        // Late duplicate/retransmitted reply packet after the request state was retired.
        slog::log3(r2p2_layer_->get_debug(), r2p2_layer_->get_local_addr(),
                   "Ignoring late reply for completed request", r2p2_hdr.req_id(),
                   "thread", r2p2_hdr.cl_thread_id(), "from server", r2p2_hdr.sr_addr());
        return;
    }

    client_request_state = req_it->second;
    client_request_state->reply_pkts_recvd_++;
    if (r2p2_hdr.first())
    {
        client_request_state->reply_pkts_to_rec_ = r2p2_hdr.pkt_id();
    }
    if (!r2p2_hdr.first() && client_request_state->reply_pkts_to_rec_ == 0)
    {
        // TODO: can it be that there is a first() packet that legitimately carries 0?
        // Some packet from the reply has been received before the first() packet
        // of the reply. Ignore for now - but don't judge wheteher the whole reply
        // has been received at this point.
        // This is either due to re-ordering or due to loss.
        // On 13/04/2021 there is no retransmission. So the whole RPC will fail.
        // Also, no reordering between the first packet and any other packet of the same uRPC is possible
        // as each uRPC (or RPC) follows the same path.
        // 31/07/2021: From what I understand, this code will work with OOO pkts
        // That is, as long as no packet is not dropped, the reply will be successfully forwarded to app layer.
        std::cerr << "Some packet (" << r2p2_hdr.pkt_id() << ") from reply has been received before the first() packet "
                  << "of the reply." << std::endl;
        slog::log2(r2p2_layer_->get_debug(), r2p2_layer_->get_local_addr(),
                   "Info:",
                   "\nmsg_type", r2p2_hdr.msg_type(),
                   "\nreq_id",
                   r2p2_hdr.req_id(),
                   "\npkt_id",
                   r2p2_hdr.pkt_id(),
                   "\nfirst",
                   r2p2_hdr.first(),
                   "\ncl_addr",
                   r2p2_hdr.cl_addr(),
                   "\nsr_addr",
                   r2p2_hdr.sr_addr(),
                   "\napp_level_id",
                   r2p2_hdr.app_level_id(),
                   "\nmsg_bytes",
                   r2p2_hdr.msg_bytes(),
                   "\nfirst_urpc",
                   r2p2_hdr.first_urpc(),
                   "\numsg_id",
                   r2p2_hdr.umsg_id());
        is_ooo = true;
    }
    client_request_state->reply_bytes_recvd_ += payload;
    if (!is_ooo && client_request_state->reply_pkts_recvd_ == client_request_state->reply_pkts_to_rec_)
    {
        slog::log5(r2p2_layer_->get_debug(), r2p2_layer_->get_local_addr(),
                   "Passing complete reply to application. Req ID:", r2p2_hdr.req_id(),
                   "from server", r2p2_hdr.sr_addr());
        r2p2_layer_->send_to_application(r2p2_hdr, client_request_state->reply_bytes_recvd_);
        pending_reqs->erase(req_it);
        delete client_request_state;
    }
    // TODO: Add check -> have more bytes than expected been received?
}
