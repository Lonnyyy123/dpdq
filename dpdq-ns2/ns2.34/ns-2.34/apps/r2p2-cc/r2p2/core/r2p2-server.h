#ifndef ns_r2p2_server_h
#define ns_r2p2_server_h

#include <unordered_map>
#include <map>
#include "r2p2-hdr.h"
#include "r2p2.h"
// #include "simple-tracer.h"
#include "traced-class.h"
#include "r2p2-generic.h"

class R2p2;
class R2p2Server;
struct ServerRequestState;
struct RequestIdTuple;

class StateGarbageCollectionTimer : public TimerHandler
{
public:
    StateGarbageCollectionTimer(R2p2Server *r2p2_server) : r2p2_server_(r2p2_server) {}

protected:
    void expire(Event *);
    R2p2Server *r2p2_server_;
};

class R2p2Server : public SimpleTracedClass
{
public:
    R2p2Server(R2p2 *r2p2_layer);
    virtual ~R2p2Server();
    void send_response(int payload, const RequestIdTuple &request_id_tuple, int new_n);
    void handle_request_pkt(hdr_r2p2 &r2p2_hdr, int payload);

    // void set_max_payload(int);
    void garbage_collect_state();
    // TODO: make r2p2 friend class
protected:
    void send_feedback(const RequestIdTuple &, int new_n);
    typedef std::tuple<int32_t, int> cl_addr_thread_t;
    typedef std::unordered_map<request_id, ServerRequestState *> req_id_to_req_state_t;
    std::map<cl_addr_thread_t, req_id_to_req_state_t *> cl_tup_to_pending_requests_;
    // one counter per thread
    std::unordered_map<int, int> thrd_id_to_reqs_served_;

    // int max_payload_;
    R2p2 *r2p2_layer_;
    StateGarbageCollectionTimer garbage_timer_;
    double gc_freq_sec_;

    // TODO: create a Traceable class that has all functions + command
    void trace_state(std::string &&event, int client_addr, int state_size,
                     long reqs_served, int sr_thread_id);
};
#endif