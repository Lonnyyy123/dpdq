#ifndef ns_r2p2_client_h
#define ns_r2p2_client_h

#include <unordered_map>
#include "r2p2-hdr.h"
#include "packet.h"
#include "r2p2.h"

class R2p2;
struct ClientRequestState;
struct RequestIdTuple;

class R2p2Client
{
public:
    R2p2Client(R2p2 *r2p2_layer);
    virtual ~R2p2Client();
    void send_req(int payload, const RequestIdTuple &request_id_tuple);
    void handle_req_rdy(hdr_r2p2 &r2p2_hdr, int nbytes);
    void handle_reply_pkt(hdr_r2p2 &r2p2_hdr, int nbytes);

    // void set_max_payload(int);

protected:
    // req_id will be local to the client thread id (as it is for port in the impl)
    typedef std::unordered_map<request_id, ClientRequestState *> req_id_to_req_state_t;
    std::unordered_map<int, req_id_to_req_state_t *> thrd_id_to_pending_reqs_map_;
    // To keep track of the current reqid per thread
    std::unordered_map<int, request_id> thrd_id_to_req_id_;
    // int max_payload_;
    R2p2 *r2p2_layer_;
};
#endif