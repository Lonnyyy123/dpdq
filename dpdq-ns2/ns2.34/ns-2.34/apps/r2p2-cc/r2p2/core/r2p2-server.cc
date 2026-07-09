#include "r2p2-server.h"
#include "simple-log.h"

struct ServerRequestState
{
    ServerRequestState() : req_pkts_expected_(-1),
                           req_pkts_received_(0),
                           req_bytes_received_(0),
                           reply_sent_(false),
                           old_state_(false) {}
    int32_t cl_addr_;
    int req_pkts_expected_;
    int req_pkts_received_;
    int req_bytes_received_;
    bool reply_sent_;
    bool old_state_;
};

R2p2Server::R2p2Server(R2p2 *r2p2_layer) : r2p2_layer_(r2p2_layer),
                                           garbage_timer_(this),
                                           gc_freq_sec_(1000.0 / 1000.0 / 1000.0)
{
    garbage_timer_.resched(gc_freq_sec_);
}

R2p2Server::~R2p2Server()
{
    delete r2p2_layer_;
    for (auto it_map = cl_tup_to_pending_requests_.begin();
         it_map != cl_tup_to_pending_requests_.end(); it_map++)
    {
        for (auto it = it_map->second->begin();
             it != it_map->second->end(); it++)
        {
            delete it->second;
        }
        delete it_map->second;
    }
}

void R2p2Server::handle_request_pkt(hdr_r2p2 &r2p2_hdr, int payload)
{
    slog::log5(r2p2_layer_->get_debug(), r2p2_layer_->get_local_addr(),
               "R2p2Server::handle_request_pkt(), req id", r2p2_hdr.req_id(),
               "with pkt_id", r2p2_hdr.pkt_id(), "(first",
               r2p2_hdr.first(), ") from client", r2p2_hdr.cl_addr());
    if (do_trace_)
        trace_state("req", -1, -1, -1, r2p2_hdr.sr_thread_id());
    cl_addr_thread_t cl_tup = std::make_tuple(r2p2_hdr.cl_addr(), r2p2_hdr.cl_thread_id());
    auto search_res = cl_tup_to_pending_requests_.find(cl_tup);
    req_id_to_req_state_t *req_id_to_req_state;
    // has the (remote) client sent before?
    if (search_res != cl_tup_to_pending_requests_.end())
    {
        req_id_to_req_state = search_res->second;
    }
    else
    {
        req_id_to_req_state = new req_id_to_req_state_t();
        cl_tup_to_pending_requests_[cl_tup] = req_id_to_req_state;
    }

    // 4 cases: (state_exists, pkt->first()): (0,0), (0,1), (1,0), (1,1)
    ServerRequestState *req_state = NULL;
    // New message?
    auto search_msg = req_id_to_req_state->find(r2p2_hdr.req_id());
    // happens only once per message
    if (search_msg == req_id_to_req_state->end())
    {
        bool single_pkt = false;
        req_state = new ServerRequestState();
        // new message. Is it received out of order (i.e., is it not first())?
        if (r2p2_hdr.first())
        {
            slog::log5(r2p2_layer_->get_debug(), r2p2_layer_->get_local_addr(),
                       "received first() packet. Creating state");
            // (!state_exists, pkt->first())
            int pkts_expected = r2p2_hdr.pkt_id() + 1;
            req_state->req_pkts_expected_ = pkts_expected;
            req_state->req_pkts_received_ = 1;
            req_state->req_bytes_received_ = payload;

            if (pkts_expected == 1)
            {
                // The request is complete, must forward to application
                r2p2_layer_->send_to_application(r2p2_hdr, req_state->req_bytes_received_);
                single_pkt = true;
            }
        }
        else
        {
            // Out of order receipt
            slog::log5(r2p2_layer_->get_debug(), r2p2_layer_->get_local_addr(),
                       "received out of order packet first");
            req_state->req_pkts_received_ = 1;
            req_state->req_bytes_received_ = payload;
        }
        (*req_id_to_req_state)[r2p2_hdr.req_id()] = req_state;
        if (single_pkt)
            return;
    }
    else
    {
        // state exists
        req_state = search_msg->second;
        // Is this first?
        if (r2p2_hdr.first())
        {
            slog::log4(r2p2_layer_->get_debug(), r2p2_layer_->get_local_addr(),
                       "received first() packet out of order",
                       "req_state->req_pkts_expected_", req_state->req_pkts_expected_,
                       "app id:", r2p2_hdr.app_level_id(),
                       "req id:", r2p2_hdr.req_id(),
                       "number of request states:", req_id_to_req_state->size());
            // (state_exists, pkt->first())
            // then the server doesn't already know how many packets are expected..
            assert(req_state->req_pkts_expected_ == -1); // it is likely that the 16 bit req_id has wrapped
            req_state->req_pkts_received_++;
            req_state->req_bytes_received_ += payload;
            req_state->req_pkts_expected_ = r2p2_hdr.pkt_id() + 1;
        }
        else
        {
            // (state_exists, !pkt->first())
            // most common case
            // assert(req_state->req_pkts_expected_ != -1); can happen if two+ ooo pkts are reveived before first()
            req_state->req_pkts_received_++;
            req_state->req_bytes_received_ += payload;
        }
    }

    if (req_state->req_pkts_expected_ != -1) // i.e., total message size is known
    {
        // have all the packets been received?
        if (req_state->req_pkts_expected_ == req_state->req_pkts_received_)
        {
            // the request can be forwarded to the application
            slog::log3(r2p2_layer_->get_debug(), r2p2_layer_->get_local_addr(),
                       "Passing complete request to application. Req ID:", r2p2_hdr.req_id(),
                       "from client", r2p2_hdr.cl_addr());
            r2p2_layer_->send_to_application(r2p2_hdr, req_state->req_bytes_received_);
        }
    }
}

void R2p2Server::send_response(int payload, const RequestIdTuple &request_id_tuple, int new_n)
{
    // throw std::runtime_error("R2p2Server::send_response() should not be called (one way messages only)");
    slog::log5(r2p2_layer_->get_debug(), r2p2_layer_->get_local_addr(),
               "R2p2Server::send_response()");
    // assume thread count agnostic for now. Find takes const time on avg
    auto srch = thrd_id_to_reqs_served_.find(request_id_tuple.sr_thread_id_);
    long reqs_served;
    if (srch != thrd_id_to_reqs_served_.end())
    {
        reqs_served = ++srch->second;
    }
    else
    {
        thrd_id_to_reqs_served_[request_id_tuple.sr_thread_id_] = 1;
        reqs_served = 1;
    }

    if (do_trace_)
        trace_state("res", -1, -1, reqs_served, request_id_tuple.sr_thread_id_);

    int num_pkts = payload / MAX_R2P2_PAYLOAD;
    int bytes_rmn = payload % MAX_R2P2_PAYLOAD;
    if (bytes_rmn > 0)
        num_pkts++;

    hdr_r2p2 r2p2_hdr;
    // will only apply to the first packet -
    // used to determine the total number of reply pkts
    r2p2_hdr.first() = true;
    r2p2_hdr.last() = false;
    r2p2_hdr.msg_type() = hdr_r2p2::REPLY;
    r2p2_hdr.policy() = hdr_r2p2::UNRESTRICTED;
    r2p2_hdr.req_id() = request_id_tuple.req_id_;
    // not sure abt this, but it seems ok to have a diff counter for REPLY
    // put the number of REPLY pkts that will be sent
    r2p2_hdr.pkt_id() = num_pkts;
    // will be interpreted as: send to r2p2-router
    r2p2_hdr.cl_addr() = request_id_tuple.cl_addr_;
    r2p2_hdr.sr_addr() = request_id_tuple.sr_addr_;
    r2p2_hdr.cl_thread_id() = request_id_tuple.cl_thread_id_;
    r2p2_hdr.sr_thread_id() = request_id_tuple.sr_thread_id_;
    r2p2_hdr.app_level_id() = request_id_tuple.app_level_id_;
    // r2p2_hdr.msg_size_bytes() = payload;
    r2p2_hdr.msg_creation_time() = Scheduler::instance().clock();

    cl_addr_thread_t cl_tup = std::make_tuple(request_id_tuple.cl_addr_,
                                              request_id_tuple.cl_thread_id_);
    req_id_to_req_state_t *state_map = nullptr;
    try
    {
        state_map = cl_tup_to_pending_requests_.at(cl_tup);
    }
    catch (const std::out_of_range &e)
    {
        slog::error(r2p2_layer_->get_debug(), r2p2_layer_->get_local_addr(), "failed to get state_map using client tuple (addr, thread)",
                    request_id_tuple.cl_addr_, request_id_tuple.cl_thread_id_);
        throw;
    }

    assert(state_map != nullptr);
    (*state_map)[request_id_tuple.req_id_]->reply_sent_ = true;
    // send_feedback(request_id_tuple, new_n);
    // std::cout << "SERVER RESPONSE SIZE: " << payload << std::endl;
    // std::cout << Scheduler::instance().clock() << " > sending REPLY for: " << r2p2_hdr.cl_addr() << " " << r2p2_hdr.req_id() << " size: " << payload << " to: " <<  request_id_tuple.cl_addr_ << std::endl;
    r2p2_layer_->send_to_transport(r2p2_hdr, payload, request_id_tuple.cl_addr_);
}

void R2p2Server::send_feedback(const RequestIdTuple &request_id_tuple, int new_n)
{
    assert(0);
    if (do_trace_)
        trace_state("fdb", -1, -1, -1, request_id_tuple.sr_thread_id_);
    hdr_r2p2 r2p2_hdr;
    r2p2_hdr.msg_type() = hdr_r2p2::R2P2_FEEDBACK;
    r2p2_hdr.req_id() = request_id_tuple.req_id_;
    r2p2_hdr.policy() = hdr_r2p2::UNRESTRICTED;
    r2p2_hdr.sr_addr() = request_id_tuple.sr_addr_;
    r2p2_hdr.cl_addr() = request_id_tuple.cl_addr_;
    r2p2_hdr.cl_thread_id() = request_id_tuple.cl_thread_id_;
    r2p2_hdr.sr_thread_id() = request_id_tuple.sr_thread_id_;
    r2p2_hdr.n() = new_n;
    r2p2_hdr.reqs_served() = thrd_id_to_reqs_served_.at(request_id_tuple.sr_thread_id_);
    r2p2_layer_->send_to_transport(r2p2_hdr, 12, -1);
}

void StateGarbageCollectionTimer::expire(Event *)
{
    r2p2_server_->garbage_collect_state();
}

void R2p2Server::garbage_collect_state()
{
    // loop for each client address-thread (request state indexed by client address)
    for (auto it_map = cl_tup_to_pending_requests_.begin();
         it_map != cl_tup_to_pending_requests_.end(); ++it_map)
    {
        req_id_to_req_state_t *state_map = it_map->second;
        if (do_trace_)
            trace_state("gct", std::get<0>(it_map->first), state_map->size(),
                        -1, std::get<1>(it_map->first));
        for (auto it = state_map->begin(); it != state_map->end();)
        {
            ServerRequestState *state = it->second;
            if (state->reply_sent_)
            {
                if (state->old_state_)
                {
                    delete state;
                    it = state_map->erase(it);
                }
                else
                {
                    state->old_state_ = true;
                    ++it;
                }
            }
            else
            {
                ++it;
            }
        }
    }
    garbage_timer_.resched(gc_freq_sec_);
}

// Messy...
void R2p2Server::trace_state(std::string &&event, int client_addr,
                             int state_size, long reqs_served, int sr_thread_id)
{
    std::vector<std::string> vars;
    vars.push_back(event);
    vars.push_back(std::to_string(r2p2_layer_->get_local_addr()));
    vars.push_back(std::to_string(sr_thread_id));
    vars.push_back(std::to_string(reqs_served));
    vars.push_back(std::to_string(client_addr));
    vars.push_back(std::to_string(state_size));
    tracer_->trace(vars);
}