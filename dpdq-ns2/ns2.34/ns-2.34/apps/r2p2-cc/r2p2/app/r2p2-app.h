/**
 * @file r2p2-app.h
 * @author Konstantinos Prasopoulos
 * R2p2Application represents an application that uses R2P2 as transport.
 */

#ifndef ns_r2p2_app_h
#define ns_r2p2_app_h

#include <string>
#include <queue>
#include "r2p2.h"
#include "r2p2-generic.h"
#include "generic-app.h"
#include "r2p2-hdr.h"

class R2p2Generic;
struct RequestIdTuple;

class R2p2Application : public GenericApp
{
public:
    R2p2Application();
    ~R2p2Application();
    // using normal calls instead of callbacks
    void req_recv(int req_size, RequestIdTuple &&request_id_tuple);     // app gets request
    void req_success(int resp_size, RequestIdTuple &&request_id_tuple); // app gets reply
    void send_request(RequestIdTuple *, size_t) override;               // TODO: why public?
    void send_response() override;
    void send_incast() override;

protected:
    int command(int argc, const char *const *argv);
    void attach_r2p2_layer(R2p2Generic *r2p2_layer);
    void start_app() override;
    void stop_app() override;
    void finish_sim() override;
    void attach_agent(int argc, const char *const *argv) override;

    R2p2Generic *r2p2_layer_;
    int n_;
    int num_reqs_rcved_;
    int num_resp_rcved_;
};

#endif