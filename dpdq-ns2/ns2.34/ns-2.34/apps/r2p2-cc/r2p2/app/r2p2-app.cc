#include <iostream>
#include <math.h>
#include <iomanip>
#include <sstream>
#include "r2p2-app.h"
#include "simple-log.h"
#include "r2p2-udp.h"

static class R2p2AppClass : public TclClass
{
public:
    R2p2AppClass() : TclClass("Generic_app/R2P2_APP") {}
    TclObject *create(int, const char *const *)
    {
        return (new R2p2Application);
    }
} class_r2p2_app;

R2p2Application::R2p2Application() : GenericApp(this), num_reqs_rcved_(0), num_resp_rcved_(0)
{
    // These values agree with the values set at the tcl level when the
    // object is created ONLY if they are set universaly like so:
    // R2P2_APP set request_size_B_ $mean_req_size_B
    // Setting the value on the TCL object obv means that the value
    // is not available upon construction
    bind("n_", &n_);
}

R2p2Application::~R2p2Application()
{
    slog::log2(debug_, local_addr_, "R2p2Application::~R2p2Application()");
    delete r2p2_layer_;
}

void R2p2Application::start_app()
{
    slog::log2(debug_, local_addr_, "================== R2p2Application::start_app() ==================");
    slog::log2(debug_, local_addr_, "msg_tracing:", MsgTracer::do_trace_,
               enable_incast_, incast_interval_sec_, num_clients_,
               num_servers_, incast_size_, incast_request_size_, dst_ids_->size());
    if (request_interval_sec_ > 0.0)
    {
        double first = send_interval_->get_next();
        send_req_timer_.resched(first);
    }
    if (MsgTracer::do_trace_)
    {
        MsgTracer::init_tracer(debug_);
    }
    assert(dst_ids_->size() > 0); // At least one reachable server

    bool manual_mode = false;
    if (request_interval_sec_ > 0.0 && dynamic_cast<ManualDistr *>(send_interval_)) // not perfect check
        manual_mode = true;

    if (manual_mode)
    {
        if (enable_incast_ == 1)
        {
            throw std::invalid_argument("Incast not available with manual mode");
        }
    }

    if (enable_incast_)
    {
        IncastGenerator::init(num_clients_, num_servers_, incast_request_size_, incast_size_); // must get total number of clients and incast size.
        send_incast_timer_.resched(incast_interval_sec_);
    }
}

void R2p2Application::stop_app()
{
    slog::log2(debug_, local_addr_, "R2p2Application::stop_app()");
    if (send_req_timer_.status() == TIMER_PENDING)
        send_req_timer_.cancel();
    if (send_incast_timer_.status() == TIMER_PENDING)
        send_incast_timer_.cancel();
}

void R2p2Application::finish_sim()
{
    slog::log2(debug_, local_addr_, "R2p2Application::finish_sim()");
    if (MsgTracer::do_trace_)
    {
        MsgTracer::finish();
    }
}

void R2p2Application::attach_agent(int argc, const char *const *argv)
{
    R2p2Agent *agent = (R2p2Agent *)TclObject::lookup(argv[2]);
    if (agent == 0)
    {
        throw std::invalid_argument("attach_agent: agent is nullptr");
    }
    // destination thread id
    int num_threads = atoi(argv[3]);
    assert(num_threads == 1); // no more than one thread per destination currently supported
    dst_ids_->push_back(agent->daddr());
    local_addr_ = agent->addr();
    slog::log3(debug_, local_addr_, "R2p2Application::attach_agent(). attached agent to target:", agent->daddr());
}

void R2p2Application::attach_r2p2_layer(R2p2Generic *r2p2_layer)
{
    slog::log2(debug_, -1, "R2p2Application::attach_r2p2_layer()");
    r2p2_layer_ = r2p2_layer;
    r2p2_layer->attach_r2p2_application(this);
}

void R2p2Application::send_request(RequestIdTuple *, size_t)
{
    int next_req_size;
    if (single_req_size_ > 0) {
        next_req_size = single_req_size_;
        single_req_size_ = -1;
    } else {
        next_req_size = (int)(req_size_->get_next());
        if (next_req_size < 4)
            next_req_size = 4;
    }

    const bool is_single_target = single_req_target_ >= 0;
    const bool is_incast = single_req_msg_class_ == MSG_CLASS_INCAST;
    int32_t srvr_addr = is_single_target ? single_req_target_ : dst_thread_gen_->get_next();

    if (do_trace_)
    {
        trace_state(is_incast ? "sin" : "srq", srvr_addr, -1, reqs_sent_, -1, next_req_size, -1, 0);
    }
    if (is_incast)
    {
        R2p2Agent::register_foreground_message(local_addr_, reqs_sent_);
    }
    MsgTracer::app_init_msg(reqs_sent_, local_addr_, local_addr_, srvr_addr, next_req_size, "Request");
    assert(srvr_addr != local_addr_); // don't send to self
    int srvr_thread = SERVER_THREAD_BASE;
    int clnt_thread = thread_id_;
    slog::log4(debug_, local_addr_, "R2p2Application::send_request(). srvr_addr:", srvr_addr, "srvr_thread:", srvr_thread, "app_level_id_:", reqs_sent_);
    r2p2_layer_->r2p2_send_req(next_req_size, RequestIdTuple(reqs_sent_,
                                                             local_addr_, srvr_addr,
                                                             clnt_thread, srvr_thread,
                                                             Scheduler::instance().clock()));
    reqs_sent_++;
    single_req_target_ = -1;
    single_req_msg_class_ = MSG_CLASS_WORKLOAD;
    if (!send_once_) {
        // update load factor
        send_interval_->set_mean(request_interval_sec_ * (1 / load_pattern_->get_load_multiplier()));
        send_req_timer_.resched(send_interval_->get_next());
    } else {
        send_once_ = false;
    }
}

void R2p2Application::send_incast()
{
    slog::log4(debug_, local_addr_, "R2p2Application::send_incast()");
    IncastGenerator::BurstInfo binfo = IncastGenerator::should_send(local_addr_);

    if (binfo.should_send_)
    {
        if (do_trace_)
        {
            trace_state("sin", binfo.incast_target_, -1, reqs_sent_, -1, binfo.req_size_, -1, 0);
        }
        r2p2_layer_->r2p2_send_req(binfo.req_size_, RequestIdTuple(reqs_sent_, local_addr_,
                                                                   binfo.incast_target_,
                                                                   thread_id_,
                                                                   SERVER_THREAD_BASE,
                                                                   Scheduler::instance().clock()));
        reqs_sent_++;
    }

    send_incast_timer_.resched(incast_interval_sec_);
}

void R2p2Application::req_recv(int req_size, RequestIdTuple &&request_id_tuple)
{
    double msg_created_at = request_id_tuple.ts_;
    /*slog::log4(debug_, local_addr_, "R2p2Application::req_recv(). size", req_size, "duration:", (Scheduler::instance().clock() - msg_created_at),
               "From:", request_id_tuple.cl_addr_, "app_level_id_:", request_id_tuple.app_level_id_,
               "that was created on:", msg_created_at);
    assert(msg_created_at > 9.9); // assumes sim starts at 10.0*/
    if (do_trace_)
    {
        trace_state("rrq",
                    request_id_tuple.cl_addr_,
                    request_id_tuple.req_id_,
                    request_id_tuple.app_level_id_,
                    Scheduler::instance().clock() - msg_created_at, req_size, -1, 0);
    }
    MsgTracer::complete_msg(request_id_tuple.app_level_id_, request_id_tuple.cl_addr_, request_id_tuple.cl_addr_);

    // this is supposed to be a single thread that processes requests in a FIFO manner
    // The service time distribution is supposed to capture other variables such as processor
    // sharing, request complexity etc

    if (pending_tasks_.empty())
    {
        send_resp_timer_.sched(proc_time_->get_next());
    }
    pending_tasks_.push(request_id_tuple);
}

void R2p2Application::send_response()
{
    // TODO: reduce access count
    int next_resp_size = (int)resp_size_->get_next();
    if (next_resp_size == 0)
        next_resp_size = 1;
    // if (do_trace_)
    //     trace_state("srs",
    //                 pending_tasks_.front().cl_addr_,
    //                 pending_tasks_.front().req_id_,
    //                 pending_tasks_.front().app_level_id_,
    //                 -1, -1, next_resp_size, 0);
    r2p2_layer_->r2p2_send_response(next_resp_size, pending_tasks_.front(), n_);
    pending_tasks_.pop();

    // TODO: add strategies for configurable distributions
    // schedule next response if one is pending
    if (!pending_tasks_.empty())
    {
        send_resp_timer_.resched(proc_time_->get_next());
    }
}

void R2p2Application::req_success(int resp_size, RequestIdTuple &&request_id_tuple)
{
    /*
    app_req_id_to_req_info_.at(request_id_tuple.app_level_id_)->\
            response_size_ = resp_size;
    app_req_id_to_req_info_.at(request_id_tuple.app_level_id_)->\
            time_completed_ = now;
    */
    slog::log4(debug_, local_addr_, "R2p2Application::req_success(). resp_size", resp_size, "From server:", request_id_tuple.sr_addr_);
    /*
    double req_dur = Scheduler::instance().clock() - app_req_id_to_req_info_.at(request_id_tuple.app_level_id_)->time_sent_;

    req_compl_times_.at(request_id_tuple.app_level_id_) = req_dur;

    if (do_trace_)
        trace_state("suc",
                    request_id_tuple.sr_addr_,
                    request_id_tuple.req_id_,
                    request_id_tuple.app_level_id_,
                    req_dur,
                    app_req_id_to_req_info_.at(request_id_tuple.app_level_id_)->request_size_,
                    resp_size, 0);
    */
}

int R2p2Application::command(int argc, const char *const *argv)
{
    Tcl &tcl = Tcl::instance();

    if (argc == 3)
    {
        if (strcmp(argv[1], "attach-r2p2-layer") == 0)
        {
            R2p2Generic *r2p2_layer = (R2p2 *)TclObject::lookup(argv[2]);
            if (!r2p2_layer)
            {
                tcl.resultf("no such R2P2 layer", argv[2]);
                return (TCL_ERROR);
            }
            attach_r2p2_layer(r2p2_layer);
            local_addr_ = r2p2_layer->get_local_addr();
            slog::log2(debug_, local_addr_, "R2p2Application::command(). Attach r2p2 layer. Local addr:", local_addr_);
            return (TCL_OK);
        }
    }
    return (GenericApp::command(argc, argv));
}
