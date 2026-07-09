/**
 * @file r2p2.h
 * @author Konstantinos Prasopoulos
 * 
 * R2p2Generic is the base class for R2P2 transports.
 * R2p2Transport has common functionality of R2p2 and R2p2Router.
 */

#ifndef ns_r2p2_generic_h
#define ns_r2p2_generic_h

#include <unordered_map>
#include "ns-process.h"
#include "packet.h"
#include "r2p2-udp.h"

class R2p2Agent;
class Handler;
class R2p2Application;

class R2p2Generic : public NsObject
{
public:
    R2p2Generic();
    virtual ~R2p2Generic();
    // API
    virtual void r2p2_send_req(int payload, const RequestIdTuple &request_id_tuple) {}
    virtual void r2p2_send_response(int payload, const RequestIdTuple &request_id_tuple, int new_n) {}
    void recv(Packet *pkt, Handler *h) override;
    virtual void attach_r2p2_application(R2p2Application *r2p2_application) {}
    virtual int32_t get_local_addr() { return -1; }

protected:
    int command(int argc, const char *const *argv) override;
};

// common functionality of r2p2 and r2p2Router
class R2p2Transport : public R2p2Generic
{
public:
    // From transport
    void recv(Packet *pkt, Handler *h) override;
    int32_t this_addr_;

protected:
    R2p2Transport();
    virtual ~R2p2Transport();
    int command(int argc, const char *const *argv) override;
    virtual void attach_r2p2_agent(R2p2Agent *r2p2_agent);
    std::unordered_map<int32_t, R2p2Agent *> r2p2_agents_;
};

#endif