#include "generic-app.h"
#include <iostream>
#include <math.h>
#include <iomanip>
#include <sstream>
#include "simple-log.h"
#include <stdlib.h>
#include "r2p2.h"
#include "homa-hdr.h"
#include "msg-tracer.h"

static class GenericAppClass : public TclClass
{
public:
    GenericAppClass() : TclClass("Generic_app") {}
    TclObject *create(int, const char *const *)
    {
        return (new GenericApp);
    }
} class_generic_app;

GenericApp::GenericApp(GenericApp *app) : reqs_sent_(0),
                                          send_req_timer_(app),
                                          send_resp_timer_(app),
                                          send_incast_timer_(app),
                                          dst_ids_(new std::vector<int32_t>())
{
    init();
}

GenericApp::GenericApp() : reqs_sent_(0),
                           send_req_timer_(this),
                           send_resp_timer_(this),
                           send_incast_timer_(this),
                           dst_ids_(new std::vector<int32_t>())
{
    init();
}

void GenericApp::init()
{
    bind("request_size_B_", &request_size_B_);
    bind("response_size_B_", &response_size_B_);
    bind("request_interval_sec_", &request_interval_sec_);
    bind("incast_interval_sec_", &incast_interval_sec_);
    bind("enable_incast_", &enable_incast_);
    bind("incast_size_", &incast_size_);
    bind("incast_request_size_", &incast_request_size_);
    bind("num_clients_", &num_clients_);
    bind("num_servers_", &num_servers_);
    bind("service_time_sec_", &service_time_sec_);
    bind("thread_id_", &thread_id_);
    bind("capture_msg_trace_", &MsgTracer::do_trace_);
    bind("debug_", &debug_);

    send_once_ = false;
    single_req_size_ = -1;
    single_req_target_ = -1;
    single_req_msg_class_ = MSG_CLASS_WORKLOAD;

    assert(
        request_size_B_ != -1 &&
        response_size_B_ != -1 &&
        request_interval_sec_ != -1 &&
        incast_interval_sec_ != -1 &&
        enable_incast_ != -1 &&
        incast_size_ != -1 &&
        incast_request_size_ != -1 &&
        num_clients_ != -1 &&
        num_servers_ != -1);
}

GenericApp::~GenericApp()
{
    delete send_interval_;
    delete proc_time_;
    delete req_size_;
    delete resp_size_;
    delete dst_thread_gen_;
    for (auto it = app_req_id_to_req_info_.begin(); it != app_req_id_to_req_info_.end(); ++it)
    {
        delete it->second;
    }
}

int GenericApp::command(int argc, const char *const *argv)
{
    Tcl &tcl = Tcl::instance();
    if (argc == 2)
    {
        if (strcmp(argv[1], "start") == 0)
        {
            start_app();
            return (TCL_OK);
        }
        else if (strcmp(argv[1], "stop") == 0)
        {
            stop_app();
            return (TCL_OK);
        }
        else if (strcmp(argv[1], "finish") == 0)
        {
            finish_sim();
            return (TCL_OK);
        }
        else if (strcmp(argv[1], "stop-tracing") == 0)
        {
            stop_tracing();
            return (TCL_OK);
        }
    }
    else if (argc == 3)
    {
        if (strcmp(argv[1], "send-single-request") == 0)
        {
            single_req_size_ = atoi(argv[2]);
            single_req_target_ = -1;
            single_req_msg_class_ = MSG_CLASS_WORKLOAD;
            send_once_ = true;
            send_request(nullptr, 0);
            return (TCL_OK);
        }
        else if (strcmp(argv[1], "start-tracing") == 0)
        {
            std::string file_path = argv[2];
            start_tracing(file_path + "/applications_trace.str");
            return (TCL_OK);
        }
        else if (strcmp(argv[1], "set-msg-trace-file") == 0)
        {
            // this happens once per app instance which is reduntant given that the tracer is static.
            std::string file_path = argv[2];
            MsgTracer::set_out_file(file_path + "/msg_trace.json");
            return (TCL_OK);
        }
        else if (strcmp(argv[1], "set-load-pattern") == 0)
        {
            if (strcmp(argv[2], "fixed") == 0)
            {
                load_pattern_ = new FixedLoadPattern();
            }
            else
            {
                tcl.resultf("no such load pattern supported %s", argv[2]);
                return (TCL_ERROR);
            }
            return (TCL_OK);
        }
    }
    else if (argc == 5)
    {
        if (strcmp(argv[1], "send-single-request-to") == 0)
        {
            single_req_size_ = atoi(argv[2]);
            single_req_target_ = atoi(argv[3]);
            single_req_msg_class_ = atoi(argv[4]);
            send_once_ = true;
            send_request(nullptr, 0);
            return (TCL_OK);
        }
    }
    else if (argc == 4)
    {
        if (strcmp(argv[1], "set-req-dstr") == 0)
        {
            int base_seed = atoi(argv[3]);
            // std::cout << "seed for set-req-dstr: " << base_seed << std::endl;
            if (request_interval_sec_ > 0.0)
            {
                if (strcmp(argv[2], "exponential") == 0)
                {
                    send_interval_ = new ExpDistr(request_interval_sec_, base_seed);
                }
                else if (strcmp(argv[2], "fixed") == 0)
                {
                    send_interval_ = new FixedDistr(request_interval_sec_);
                }
                else if (strcmp(argv[2], "mlpm") == 0)
                {
                    send_interval_ = new FixedDistr(request_interval_sec_);
                }
                else
                {
                    tcl.resultf("no such req distribution supported %s", argv[2]);
                    return (TCL_ERROR);
                }
            }
            return (TCL_OK);
        }
        else if (strcmp(argv[1], "set-load-pattern") == 0)
        {
            if (strcmp(argv[2], "step") == 0)
            {
                // Ugh..
                std::string params = std::string(argv[3]);
                std::string delim = " ";
                size_t pos = 0;

                pos = params.find(" ");
                double high_ratio = atof(params.substr(0, pos).c_str());
                params.erase(0, pos + delim.length());

                pos = params.find(" ");
                double period = atof(params.substr(0, pos).c_str());
                params.erase(0, pos + delim.length());

                pos = params.find(" ");
                double high_value_multiplier = atof(params.substr(0, pos).c_str());
                params.erase(0, pos + delim.length());

                pos = params.find(" ");
                double low_value_multiplier = atof(params.substr(0, pos).c_str());
                params.erase(0, pos + delim.length());

                load_pattern_ = new StepLoadPattern(high_ratio, period, high_value_multiplier, low_value_multiplier);
            }
            else
            {
                tcl.resultf("no such load pattern supported %s", argv[2]);
                return (TCL_ERROR);
            }
            return (TCL_OK);
        }
        else if (strcmp(argv[1], "set-target-distr") == 0)
        {
            if (strcmp(argv[2], "uniform") == 0)
            {
                // std::cout << "seed for set-target-distr: " << local_addr_ << " atoi(argv[3]) " << atoi(argv[3]) << std::endl;
                assert(local_addr_ >= 0); // if not, maybe address was not set
                dst_thread_gen_ = new UnifTargetIntDistr(atoi(argv[3]) - 1, dst_ids_, local_addr_);
            }
            else
            {
                tcl.resultf("no such target distribution supported %s", argv[2]);
                return (TCL_ERROR);
            }
            return (TCL_OK);
        }
        else if (strcmp(argv[1], "set-resp-dstr") == 0)
        {
            int base_seed = atoi(argv[3]) * 13;
            if (strcmp(argv[2], "exponential") == 0)
            {
                proc_time_ = new ExpDistr(service_time_sec_, base_seed);
            }
            else if (strcmp(argv[2], "fixed") == 0)
            {
                proc_time_ = new FixedDistr(service_time_sec_);
            }
            else
            {
                tcl.resultf("no such resp distribution supported %s", argv[2]);
                return (TCL_ERROR);
            }
            return (TCL_OK);
        }
        else if (strcmp(argv[1], "set-req-sz-dstr") == 0)
        {
            int base_seed = atoi(argv[3]);
            if (strcmp(argv[2], "fixed") == 0)
            {
                req_size_ = new FixedDistr(request_size_B_);
            }
            else if (strcmp(argv[2], "w1") == 0)
            {
                req_size_ = new W1Distr((unsigned int)base_seed, request_size_B_);
            }
            else if (strcmp(argv[2], "w2") == 0)
            {
                req_size_ = new W2Distr((unsigned int)base_seed, request_size_B_);
            }
            else if (strcmp(argv[2], "w3") == 0)
            {
                req_size_ = new W3Distr((unsigned int)base_seed, request_size_B_);
            }
            else if (strcmp(argv[2], "w4") == 0)
            {
                req_size_ = new W4Distr((unsigned int)base_seed, request_size_B_);
            }
            else if (strcmp(argv[2], "w5") == 0)
            {
                req_size_ = new W5Distr((unsigned int)base_seed, request_size_B_);
            }
            else
            {
                tcl.resultf("no such req size distribution supported %s", argv[2]);
                return (TCL_ERROR);
            }
            return (TCL_OK);
        }
        else if (strcmp(argv[1], "set-resp-sz-dstr") == 0)
        {
            int base_seed = atoi(argv[3]) * 39;
            if (strcmp(argv[2], "fixed") == 0)
            {
                resp_size_ = new FixedDistr(response_size_B_);
            }
            else if (strcmp(argv[2], "w1") == 0)
            {
                resp_size_ = new W1Distr((unsigned int)base_seed, response_size_B_);
            }
            else if (strcmp(argv[2], "w2") == 0)
            {
                resp_size_ = new W2Distr((unsigned int)base_seed, response_size_B_);
            }
            else if (strcmp(argv[2], "w3") == 0)
            {
                resp_size_ = new W3Distr((unsigned int)base_seed, response_size_B_);
            }
            else if (strcmp(argv[2], "w4") == 0)
            {
                resp_size_ = new W4Distr((unsigned int)base_seed, response_size_B_);
            }
            else if (strcmp(argv[2], "w5") == 0)
            {
                resp_size_ = new W5Distr((unsigned int)base_seed, response_size_B_);
            }
            else
            {
                tcl.resultf("no such resp size distribution supported %s", argv[2]);
                return (TCL_ERROR);
            }
            return (TCL_OK);
        }
        else if (strcmp(argv[1], "attach-agent") == 0)
        {
            try
            {
                attach_agent(argc, argv);
            }
            catch (const std::invalid_argument &e)
            {
                printf("Errror attaching agent %s", argv[2]);
                tcl.resultf("Errror attaching agent %s", argv[2]);
                return (TCL_ERROR);
            }

            return (TCL_OK);
        }
    }
    else if (argc == 5)
    {
        if (strcmp(argv[1], "set-req-dstr") == 0)
        {
            const char *events_file = argv[3];
            int machine_id = atoi(argv[4]);
            if (strcmp(argv[2], "manual") == 0)
            {
                send_interval_ = new ManualDistr(events_file, machine_id, 0);
            }
            else
            {
                tcl.resultf("no such req distribution supported %s", argv[2]);
                return (TCL_ERROR);
            }
            return (TCL_OK);
        }
        else if (strcmp(argv[1], "set-target-distr") == 0)
        {
            if (strcmp(argv[2], "manual") == 0)
            {
                const char *events_file = argv[3];
                int machine_id = atoi(argv[4]);
                dst_thread_gen_ = new ManualDistr(events_file, machine_id, 1);
            }
            else
            {
                tcl.resultf("no such target distribution supported %s", argv[2]);
                return (TCL_ERROR);
            }
            return (TCL_OK);
        }
        else if (strcmp(argv[1], "set-req-sz-dstr") == 0)
        {
            if (strcmp(argv[2], "manual") == 0)
            {
                const char *events_file = argv[3];
                int machine_id = atoi(argv[4]);
                req_size_ = new ManualDistr(events_file, machine_id, 2);
            }
            else
            {
                tcl.resultf("no such req size distribution supported %s", argv[2]);
                return (TCL_ERROR);
            }
            return (TCL_OK);
        }
        else if (strcmp(argv[1], "set-resp-sz-dstr") == 0)
        {
            // double sigma = atof(argv[3]);
            // int base_seed = atoi(argv[4]) * 39;
            // if (strcmp(argv[2], "lognormal") == 0)
            // {
            //     // std::cout << "lognormal resp_sz " << response_size_B_ << " " << sigma << " " << base_seed << std::endl;
            //     resp_size_ = new LogNormalDistr(response_size_B_, sigma, base_seed);
            // }
            // else
            // {
            tcl.resultf("no such resp size distribution supported %s", argv[2]);
            return (TCL_ERROR);
            // }
            // return (TCL_OK);
        }
    }
    return (Process::command(argc, argv));
}

void GenericApp::trace_state(std::string &&event,
                             int32_t remote_addr,
                             int req_id,
                             long app_level_id,
                             double req_dur,
                             int req_size,
                             int resp_size,
                             uint64_t wildcard)
{
    std::vector<std::string> vars;
    vars.push_back(event);
    vars.push_back(std::to_string(local_addr_));
    vars.push_back(std::to_string(remote_addr));
    vars.push_back(std::to_string(thread_id_));
    vars.push_back(std::to_string(req_id));
    vars.push_back(std::to_string(app_level_id));
    std::stringstream stream;
    stream << std::fixed << std::setprecision(9) << req_dur;
    vars.push_back(stream.str());
    vars.push_back(std::to_string(req_size));
    vars.push_back(std::to_string(resp_size));
    vars.push_back(std::to_string(pending_tasks_.size()));
    vars.push_back(std::to_string(wildcard));
    tracer_->trace(vars);
}

void SendReqTimer::expire(Event *)
{
    app_->send_request();
}

void SendIncastTimer::expire(Event *)
{
    app_->send_incast();
}

void SendRespTimer::expire(Event *)
{
    app_->send_response();
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////// Load pattern /////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

LoadPattern::LoadPattern() : start_time_(-1.0) {}

void LoadPattern::UpdateTimer::expire(Event *)
{
    if (ptrn_)
        ptrn_->update();
}

FixedLoadPattern::FixedLoadPattern() { load_value_ = 1.0; }

double FixedLoadPattern::get_load_multiplier()
{
    return load_value_;
}

StepLoadPattern::StepLoadPattern(double high_ratio,
                                 double period,
                                 double high_value_mul,
                                 double low_value_mul) : high_ratio_(high_ratio),
                                                         period_(period),
                                                         high_value_multiplier_(high_value_mul),
                                                         low_value_multiplier_(low_value_mul)
{
    update_timer_ = UpdateTimer(this);
    load_value_ = -1.0;
    assert(period_ > 0.0);
    assert(high_value_multiplier_ >= 0.0);
    assert(low_value_multiplier_ >= 0.0);
    assert(high_ratio_ > 0.0);
    assert(high_ratio_ < 1.0);
}

double StepLoadPattern::get_load_multiplier()
{
    if (start_time_ == -1.0)
    {
        start_time_ = Scheduler::instance().clock();
        update();
    }
    assert(load_value_ >= 0.0);
    return load_value_;
}

void StepLoadPattern::update()
{
    assert(start_time_ != -1.0);
    double total_duration = Scheduler::instance().clock() - start_time_;
    // find time in period
    int periods_past = total_duration / period_;
    double point_in_period = (total_duration - periods_past * period_) / period_;
    // find load value
    if (point_in_period >= 1.0 - high_ratio_)
    {
        // low value time is over
        load_value_ = high_value_multiplier_;
    }
    else
    {
        load_value_ = low_value_multiplier_;
    }
    update_timer_.resched(period_ / 500.0);
}
