#ifndef ns_udp_homa_h
#define ns_udp_homa_h

#include "udp.h"
#include "homa-hdr.h"
#include <iostream>
#include <string>
#include "r2p2-udp.h" // for some macros

class HomaTransport;

class HomaAgent : public UdpAgent
{
public:
    HomaAgent();
    HomaAgent(packet_t);
    void sendmsg(hdr_homa *homa_hdr, int prio = NO_PRIO, const char *flags = 0);
    void recv(Packet *, Handler *);

    void attach_homa_transport(HomaTransport *homa_transport);

private:
    HomaTransport *homa_layer_;
};

#endif
