#include "r2p2-router.h"
#include "simple-log.h"
#include "flags.h"

static class R2p2RouterClass : public TclClass
{
public:
    R2p2RouterClass() : TclClass("R2P2_Router") {}
    TclObject *create(int, const char *const *)
    {
        return (new R2p2Router);
    }
} class_r2p2_router;

struct ServerState
{
    ServerState() : served_reqs_(0), pending_reqs_(0), n_(3) {}
    long served_reqs_;
    int pending_reqs_;
    int n_;
};

R2p2Router::R2p2Router()
{
    bind("router_latency_s_", &router_latency_s_);
    bind("pooled_sender_credit_bytes_", &pooled_sender_credit_bytes_);
}

// TODO: delete ServerStates
R2p2Router::~R2p2Router()
{
    delete dst_thread_gen_;
}

void R2p2Router::recv(Packet *pkt, Handler *h)
{
    hdr_ip *ip_hdr = hdr_ip::access(pkt);
    hdr_r2p2 *r2p2_hdr = hdr_r2p2::access(pkt);
    slog::log5(debug_, this_addr_, "R2p2Router::recv(). Src addr:", ip_hdr->src().addr_, "TTL:", ip_hdr->ttl());

    if (r2p2_hdr->msg_type() == hdr_r2p2::REQUEST)
    {
        if (do_trace_)
            trace_state("rrq");
        Scheduler::instance().schedule(this, pkt, router_latency_s_);
    }
    else if (r2p2_hdr->msg_type() == hdr_r2p2::R2P2_FEEDBACK)
    {
        if (do_trace_)
            trace_state("fdb");
        ServerState *server_state = server_tup_to_server_state_
                                        .at(std::make_tuple(r2p2_hdr->sr_addr(), r2p2_hdr->sr_thread_id()));
        int new_num_of_pending_reqs = server_state->pending_reqs_ - r2p2_hdr->reqs_served() + server_state->served_reqs_;
        if (new_num_of_pending_reqs < 0)
            throw std::range_error("Number of pending requirements is negative");
        server_state->served_reqs_ = r2p2_hdr->reqs_served();
        server_state->n_ = r2p2_hdr->n();
        server_state->pending_reqs_ = new_num_of_pending_reqs;

        forward_queued_pkts();

        Packet::free(pkt);
    }
}

/**
 * Handle the incoming packet after some artificial "processing" delay
 */
void R2p2Router::handle(Event *e)
{
    sr_addr_thread_t target_worker;
    Packet *pkt = (Packet *)e;
    int32_t client_addr = hdr_r2p2::access(pkt)->cl_addr();
    if (op_mode_ == JBSQ)
    {
        if (!find_worker_shortest_q(target_worker, client_addr))
        {
            if (do_trace_)
                trace_state("enq");
            pending_pkts_.push(pkt);
        }
        else
        {
            forward_request(pkt, target_worker);
        }
    }
    else if (op_mode_ == RANDOM)
    {
        // make sure to not send to the server that is on the same machine as the client
        bool found_worker = false;
        while (!found_worker)
        {
            target_worker = server_workers_.at(dst_thread_gen_->get_next());
            if (client_addr != std::get<0>(target_worker))
                found_worker = true;
        }
        forward_request(pkt, target_worker);
    }
    slog::log5(debug_, this_addr_, "R2p2Router::handle(). Router sent packet to worker (addr, thread):", std::get<0>(target_worker), std::get<1>(target_worker));
}

/**
 * Find the shortest worker queue (if one has space) - just scan workers for now
 * make sure to not send to the server that is on the same machine as the client
 */
// TODO: FOR JBSQ: make this function not select a server that is in client_addr.
bool R2p2Router::find_worker_shortest_q(sr_addr_thread_t &worker, int32_t client_addr)
{
    sr_addr_thread_t shortest_q_worker;
    int shortest_q_length = 1000000000;
    bool found_worker = false;
    for (auto worker = server_workers_.begin(); worker != server_workers_.end(); ++worker)
    {
        ServerState *state = server_tup_to_server_state_.at(*worker);
        if (state->pending_reqs_ < shortest_q_length && state->pending_reqs_ < state->n_)
        {
            shortest_q_length = state->pending_reqs_;
            shortest_q_worker = *worker;
            found_worker = true;
        }
    }
    worker = shortest_q_worker;
    return found_worker;
}

/**
 * Send one queued packet (if present) for each newly available worker.
 * Called after receiving feedback.
 * If a feedback pkt is lost, there might be more than one work spots available.
 */
void R2p2Router::forward_queued_pkts()
{
    sr_addr_thread_t target_worker;
    int32_t cl_addr;
    if (op_mode_ == JBSQ)
    {
        cl_addr = hdr_r2p2::access(pending_pkts_.front())->cl_addr();
        while (find_worker_shortest_q(target_worker, cl_addr))
        {
            if (!pending_pkts_.empty())
            {
                forward_request(pending_pkts_.front(), target_worker);
                pending_pkts_.pop();
            }
            else
            {
                return;
            }
            cl_addr = hdr_r2p2::access(pending_pkts_.front())->cl_addr();
        }
    }
}

void R2p2Router::forward_request(Packet *pkt, sr_addr_thread_t worker)
{
    hdr_ip *ip_hdr = hdr_ip::access(pkt);
    if (do_trace_)
        trace_state("frq");
    hdr_r2p2 *r2p2_hdr = hdr_r2p2::access(pkt);
    int pkt_size = hdr_cmn::access(pkt)->size();
    // The serverAddr can be set here.
    r2p2_hdr->sr_addr() = std::get<0>(worker);
    // will be used by r2p2 server to demultiplex the destination application
    r2p2_hdr->sr_thread_id() = std::get<1>(worker);
    int ecn_capable = 0;
    if (hdr_flags::access(pkt)->ect())
    {
        ecn_capable = 1;
    }
    r2p2_agents_.at(std::get<0>(worker))->sendmsg(pkt_size, *r2p2_hdr, MsgTracerLogs(), ip_hdr->flowid(), ip_hdr->prio(), ip_hdr->ttl(), ip_hdr->src().addr_, ecn_capable);
    server_tup_to_server_state_.at(worker)->pending_reqs_++;
    slog::log5(debug_, this_addr_, "R2p2Router::forward_request() sent request to addr:", ip_hdr->dst().addr_, "with prio:", ip_hdr->prio(), "and current TTL:", ip_hdr->ttl());
    Packet::free(pkt);
}

/**
 * Assumes that server threads will have ids [0,inf]
 */
void R2p2Router::attach_r2p2_server_agent(R2p2Agent *r2p2_server_agent, int num_threads)
{
    for (int i = 0; i < num_threads; i++)
    {
        sr_addr_thread_t tup = std::make_tuple(r2p2_server_agent->daddr(), i);
        server_workers_.push_back(tup);
        server_tup_to_server_state_[tup] = new ServerState();
    }
    R2p2Transport::attach_r2p2_agent(r2p2_server_agent);
}

void R2p2Router::attach_r2p2_client_agent(R2p2Agent *r2p2_client_agent)
{
    R2p2Transport::attach_r2p2_agent(r2p2_client_agent);
}

int R2p2Router::command(int argc, const char *const *argv)
{
    Tcl &tcl = Tcl::instance();
    if (argc == 2)
    {
        if (strcmp(argv[1], "stop-tracing") == 0)
        {
            stop_tracing();
            return (TCL_OK);
        }
    }
    else if (argc == 3)
    {
        if (strcmp(argv[1], "start-tracing") == 0)
        {
            std::string file_path = argv[2];
            start_tracing(file_path + "/router_trace.str");
            return (TCL_OK);
        }
        else if (strcmp(argv[1], "op-mode") == 0)
        {
            if (strcmp(argv[2], "jbsq") == 0)
            {
                op_mode_ = JBSQ;
            }
            else if (strcmp(argv[2], "random") == 0)
            {
                op_mode_ = RANDOM;
            }
            else if (strcmp(argv[2], "round-robin") == 0)
            {
                op_mode_ = RR;
            }
            return (TCL_OK);
        }
        else if (strcmp(argv[1], "num-workers") == 0)
        {
            dst_thread_gen_ = new UnifIntDistr(atoi(argv[2]) - 1, 0);
            assert(0); // seed is wrong (fixed)
            return (TCL_OK);
        }
        else if (strcmp(argv[1], "attach-client-agent") == 0)
        {
            R2p2Agent *r2p2_client_agent = (R2p2Agent *)TclObject::lookup(argv[2]);
            if (!r2p2_client_agent)
            {
                tcl.resultf("no such R2P2 agent", argv[2]);
                return (TCL_ERROR);
            }
            attach_r2p2_client_agent(r2p2_client_agent);
            return (TCL_OK);
        }
    }
    else if (argc == 4)
    {
        if (strcmp(argv[1], "attach-server-agent") == 0)
        {
            R2p2Agent *r2p2_server_agent = (R2p2Agent *)TclObject::lookup(argv[2]);
            if (!r2p2_server_agent)
            {
                tcl.resultf("no such R2P2 agent", argv[2]);
                return (TCL_ERROR);
            }
            int num_threads = atoi(argv[3]);
            attach_r2p2_server_agent(r2p2_server_agent, num_threads);
            return (TCL_OK);
        }
    }
    return (R2p2Transport::command(argc, argv));
}

void R2p2Router::trace_state(std::string event)
{
    std::vector<std::string> vars;
    vars.push_back(event);
    vars.push_back(std::to_string(pending_pkts_.size()));
    tracer_->trace(vars);
}