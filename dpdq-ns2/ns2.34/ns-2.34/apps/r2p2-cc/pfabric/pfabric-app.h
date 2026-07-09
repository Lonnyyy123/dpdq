/**
 * @file pfabric-app.h
 * @author Konstantinos Prasopoulos
 * PfabricApplication Represents an application that uses pFabric as transport.
 */

#ifndef ns_pfabbric_app_h
#define ns_pfabbric_app_h

#include <string>
#include <queue>
#include "app.h"
#include "r2p2-hdr.h"
#include "generic-app.h"
#include "tcp-full.h"
#include <map>
#include <unordered_map>
#include <vector>

template <typename T>
class PfabricApplication;

template <typename T>
class WarmupGapTimer : public TimerHandler
{
public:
    WarmupGapTimer(PfabricApplication<T> *pfab_app) : pfab_app_(pfab_app) {}

protected:
    void expire(Event *);
    PfabricApplication<T> *pfab_app_;
};

struct MsgState;
// big and messy
template <typename T>
class PfabricApplication : public GenericApp
{
    friend class WarmupGapTimer<T>;

public:
    PfabricApplication();
    ~PfabricApplication();
    void recv_msg(int payload, RequestIdTuple &&req_id);
    void send_request(RequestIdTuple *, size_t) override;
    void send_response() override;
    void send_incast() override;

protected:
    typedef std::vector<T *> free_connections_pool_t;
    typedef std::queue<RequestIdTuple> queued_requests_t;
    int command(int argc, const char *const *argv);
    void start_app() override;
    void stop_app() override;
    void attach_agent(int argc, const char *const *argv) override;
    void warmup();
    void forward_request(RequestIdTuple &req_id, free_connections_pool_t *pool, int32_t srvr_addr);

    WarmupGapTimer<T> warmup_timer_;
    int warmup_phase_;
    int warmup_pool_counter_;

    // to queue requests when the connection pool is empty

    std::map<int32_t, free_connections_pool_t *> dstid_to_free_agent_pool_;
    std::map<int32_t, queued_requests_t *> dstid_to_queued_requests_;
    uint32_t queued_requests_;

    // (for client) to identify which connection needs to be freed (srvr_addr, srvr_thread)
    std::unordered_map<long, std::tuple<int32_t, int, T *>> req_id_to_busy_agent_;

    // For server related stuff (mostly from r2p2-server.h.. )
    // to identify each client thread
    typedef std::tuple<int32_t, int> cl_addr_thread_t;

    // Map from the app-level request id created by a specific client thread
    // to that request's state.
    typedef std::unordered_map<long, MsgState *> req_id_to_req_state_t;

    // locate the map of a specific client thread.
    std::map<cl_addr_thread_t, req_id_to_req_state_t *> cl_tup_to_pending_requests_;

    // to keep track of a reply (app_lvl_req_id to MsgState)
    std::unordered_map<long, MsgState *> req_id_to_reply_state_;

    void trace_state(std::string &&event,
                     int32_t remote_addr,
                     int req_id,
                     long app_level_id,
                     double req_dur,
                     int req_size,
                     int resp_size,
                     int pool_size);
};

#endif