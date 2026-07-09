/**
 * @file generic-app.h
 * @author Konstantinos Prasopoulos
 * GenericApp represents an application running on a host.
 * Extends ns-2's Application.
 * To be extended by applications that use R2P2 (?). They need to define `send_request()`
 * and `send_response()`
 *
 * Used to configure the request and response distribution of sizes and interarrival distributions.
 * Empirical and theoretical distributions are defined in generic-app.cc (TODO: Change)
 */

#ifndef ns_generic_app_h
#define ns_generic_app_h

#include <string>
#include <queue>
#include <random>
#include <map>
#include <vector>
#include "ns-process.h"
#include "timer-handler.h"
#include "rng.h"
#include "simple-tracer.h"
#include "r2p2-generic.h"
#include "app.h"
#include "traced-class.h"
#include "msg-tracer.h"
#include <workload-distr.h>

class GenericApp;

/* Thread id's are used to sitinguish between application classes */
#define SERVER_THREAD_BASE 0

#define MSG_CLASS_WORKLOAD 0
#define MSG_CLASS_INCAST 1

struct RequestInfo
{
    double time_sent_;
    double time_completed_;
    int request_size_;
    int response_size_;
};

class SendReqTimer : public TimerHandler
{
public:
    SendReqTimer(GenericApp *app) : app_(app) {}

protected:
    void expire(Event *);
    GenericApp *app_;
};

class SendIncastTimer : public TimerHandler
{
public:
    SendIncastTimer(GenericApp *app) : app_(app) {}

protected:
    void expire(Event *);
    GenericApp *app_;
};

class SendRespTimer : public TimerHandler
{
public:
    SendRespTimer(GenericApp *app) : app_(app) {}

protected:
    void expire(Event *);
    GenericApp *app_;
};

// //////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// //////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// /////////////////////////////////////////////////// Load pattern /////////////////////////////////////////////////////////////////
// //////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// //////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

class LoadPattern
{
public:
    LoadPattern();
    ~LoadPattern(){};
    // enum PatternTypes
    // {
    //     FIXED,
    //     TRIANGLE,
    //     STEP,
    // };
    virtual double get_load_multiplier() = 0;
    virtual void update(){};

    /*
     * Timer
     */
    class UpdateTimer : public TimerHandler
    {
    public:
        UpdateTimer() : ptrn_(nullptr) {}
        UpdateTimer(LoadPattern *pattern) : ptrn_(pattern) {}

    protected:
        void expire(Event *);
        LoadPattern *ptrn_;
    };

protected:
    UpdateTimer update_timer_;
    double start_time_;
    double load_value_;
};

/**
 * @brief Step pattern. Starts at low_value_multiplier_ and repeats every period
 *
 */
class FixedLoadPattern : public LoadPattern
{
public:
    FixedLoadPattern();
    ~FixedLoadPattern(){};
    double get_load_multiplier() override; // always one
};

class StepLoadPattern : public LoadPattern
{
public:
    StepLoadPattern(double high_ratio, double period, double high_value_mul, double low_value_mul);
    ~StepLoadPattern(){};
    double get_load_multiplier() override;
    void update() override;

protected:
    double high_ratio_;
    double period_; // in seconds
    double high_value_multiplier_;
    double low_value_multiplier_;
};

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////// Generic Application //////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

class GenericApp : public Application,
                   public SimpleTracedClass
{
public:
    GenericApp();
    GenericApp(GenericApp *);
    virtual ~GenericApp();
    virtual void send_request(RequestIdTuple *req = nullptr, size_t size = 0){};
    virtual void send_response(){};
    virtual void send_incast(){};
    int get_thread_id() { return thread_id_; }

protected:
    bool send_once_;
    int single_req_size_;
    int32_t single_req_target_;
    int single_req_msg_class_;
    void init();
    virtual int command(int argc, const char *const *argv);
    virtual void start_app(){};
    virtual void stop_app(){};
    /**
     * Called when just before the simulation ends.
     */
    virtual void finish_sim(){};
    virtual void attach_agent(int argc, const char *const *argv){};

    long reqs_sent_;
    SendReqTimer send_req_timer_;
    SendRespTimer send_resp_timer_;
    SendIncastTimer send_incast_timer_;
    double request_size_B_;
    double response_size_B_;
    double request_interval_sec_;
    double incast_interval_sec_;
    int enable_incast_;
    int incast_size_;
    int incast_request_size_;
    int num_clients_; /* Total number of clients in simulation */
    int num_servers_; /* Total number of servers in simulation */
    double service_time_sec_;
    int thread_id_;
    int32_t local_addr_;
    int debug_;

    RndDistr *send_interval_;
    RndDistr *proc_time_;
    RndDistr *req_size_;
    RndDistr *resp_size_;
    LoadPattern *load_pattern_;

    std::queue<RequestIdTuple> pending_tasks_; // requests that need processing
    std::unordered_map<long, RequestInfo *> app_req_id_to_req_info_;

    RndDistr *dst_thread_gen_;      // random req target generator
    std::vector<int32_t> *dst_ids_; // addr of possible request destinations

    void trace_state(std::string &&event,
                     int32_t remote_addr,
                     int req_id,
                     long app_level_id,
                     double req_dur,
                     int req_size,
                     int resp_size,
                     uint64_t wildcard);
};

#endif