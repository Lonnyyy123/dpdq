/**
 * @file r2p2-router.h
 * @author Konstantinos Prasopoulos
 * 
 */

#ifndef ns_r2p2_router_h
#define ns_r2p2_router_h

#include <vector>
#include <map>
#include <queue>
#include <string>
#include "r2p2-generic.h"
#include "packet.h"
#include "r2p2-udp.h"
// #include "simple-tracer.h"
#include "generic-app.h"
#include "traced-class.h"

class Packet;
class Event;
class Handler;
struct ServerState;

class R2p2Router : public R2p2Transport, public SimpleTracedClass
{
public:
    R2p2Router();
    virtual ~R2p2Router();
    // From transport
    void recv(Packet *pkt, Handler *h);
    typedef std::tuple<int32_t, int> sr_addr_thread_t;
    int pooled_sender_credit_bytes_;

protected:
    enum OpMode
    {
        JBSQ,
        RANDOM,
        RR
    };
    void handle(Event *e);
    bool find_worker_shortest_q(sr_addr_thread_t &, int32_t client_addr);
    void forward_queued_pkts();
    void forward_request(Packet *p, sr_addr_thread_t worker);
    void attach_r2p2_server_agent(R2p2Agent *r2p2_agent, int num_threads);
    void attach_r2p2_client_agent(R2p2Agent *r2p2_agent);
    int command(int argc, const char *const *argv);

    RndDistr *dst_thread_gen_;
    std::vector<sr_addr_thread_t> server_workers_;
    std::map<sr_addr_thread_t, ServerState *> server_tup_to_server_state_;
    std::queue<Packet *> pending_pkts_;
    double router_latency_s_;
    int op_mode_;
    void trace_state(std::string event);
};

#endif