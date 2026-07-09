/**
 * @file r2p2.h
 * @author Konstantinos Prasopoulos
 * R2p2 connects vertically with the application above and the transport bellow.
 * It also connects horizontally with the R2p2Client and R2p2Server classes.
 * Requests from the application are forwarded to the client. The client prepares the request
 * and calls `send_to_transport` (of this class). The request is forwarded to the
 * attached R2p2CCGeneric class.
 */

#ifndef ns_r2p2_h
#define ns_r2p2_h

#include "r2p2-generic.h"
#include "r2p2-app.h"
#include "r2p2-hdr.h"
#include "r2p2-client.h"
#include "r2p2-server.h"
#include "r2p2-cc.h"
#include "traced-class.h"

// R2p2 parameters
#define PACKET_SIZE 1500
#define HEADERS_SIZE 50
#define IP_HEADER_SIZE 20
#define UDP_HEADER_SIZE 8
// #define R2P2_HEADER_SiZE 8

// #define MAX_ETHERNET_PAYLOAD_BYTES 1500
// #define ETHERNET_HDR_SIZE 14
// #define ETHERNET_CRC_SIZE 4
// #define ETHERNET_PREAMBLE_SIZE 8
// #define INTER_PKT_GAP 12
// #define MIN_ETHERNET_PAYLOAD_BYTES 46 // only used in WorkloadSynthesizer

// ~~~~~~~~~~~~~~~~ New ~~~~~~~~~~~~~~~~
// payed for every ethernet frame
#define INTER_PKT_GAP_SIZE 12
#define ETHERNET_PREAMBLE_SIZE 8

// standard headers on every frame
#define ETHERNET_HEADER_SIZE 18
#define IP_HEADER_SIZE 20
#define UDP_HEADER_SIZE 8
#define TCP_HEADER_SIZE 20

#define MIN_ETHERNET_PAYLOAD 46
#define MAX_ETHERNET_PAYLOAD 1500
#define MAX_UDP_PAYLOAD 1472 // 1518 - eth - IP - UDP
#define MIN_UDP_PAYLOAD 18   // 64 (min frame) - 18 (ETH) - 20 (IP) - 8 (UDP)
#define MAX_TCP_PAYLOAD 1460 // 1518 - eth - IP - TCP
#define MIN_TCP_PAYLOAD 6    // 64 (min frame) - 18 (ETH) - 20 (IP) - 20 (TCP)

#define MIN_ETHERNET_FRAME 64
#define MIN_ETHERNET_FRAME_ON_WIRE 84   // 64 + 12 + 8 (preamble and interpacket)
#define MAX_ETHERNET_FRAME 1518         // max eth payload 1500 + eth header 18
#define MAX_ETHERNET_FRAME_ON_WIRE 1538 // 1518 + 12 + 8 (preamble and interpacket)

#define UDP_COMMON_HEADERS_SIZE 46 // ETH + IP + UDP | added to payload
#define TCP_COMMON_HEADERS_SIZE 58 // ETH + IP + TCP

// R2P2
#define R2P2_HEADER_SIZE 14
#define R2P2_ALL_HEADERS_SIZE 60
#define MAX_R2P2_PAYLOAD 1458 // 1472 - R2P2_HEADER_SIZE
#define MIN_R2P2_PAYLOAD 4    // MIN_UDP_PAYLOAD - R2P2_HEADER_SIZE (with padding if needed)

// Homa

class R2p2Application;
class Packet;
class R2p2Client;
class R2p2Server;
class Handler;
class R2p2CCGeneric;

class R2p2 : public R2p2Generic, public SimpleTracedClass
{
    friend class R2p2Client;
    friend class R2p2Server;

public:
    R2p2();
    virtual ~R2p2();
    // API
    void r2p2_send_req(int payload, const RequestIdTuple &request_id_tuple) override;
    void r2p2_send_response(int payload, const RequestIdTuple &request_id_tuple, int new_n) override;
    // From transport
    void recv(Packet *pkt, Handler *);

    void attach_r2p2_application(R2p2Application *r2p2_application) override;
    int32_t get_local_addr() override;
    inline int get_debug()
    {
        return debug_;
    }

protected:
    // For friend classes client and server
    void send_to_transport(hdr_r2p2 &, int nbytes, int32_t daddr);
    void send_to_application(hdr_r2p2 &, int req_resp_size);

    void attach_cc_module(R2p2CCGeneric *r2p2_cc_module);
    int command(int argc, const char *const *argv);

    // int max_payload_;
    R2p2Client *r2p2_client_;
    R2p2Server *r2p2_server_;
    std::unordered_map<int, R2p2Application *> thread_id_to_app_;
    R2p2CCGeneric *r2p2_cc_;

    void start_tracing(std::string file_path);
    void stop_tracing();
};

#endif