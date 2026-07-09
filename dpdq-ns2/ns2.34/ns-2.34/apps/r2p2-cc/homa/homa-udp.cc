#include "homa-udp.h"
#include "homa-transport.h"
#include "rtp.h"
#include "random.h"
#include "simple-log.h"
#include "flags.h"
#include "app-message.h"

// OTcl linkange class
static class HomaAgentClass : public TclClass
{
public:
    HomaAgentClass() : TclClass("Agent/UDP/HOMA") {}
    TclObject *create(int, const char *const *)
    {
        return (new HomaAgent());
    }
} class_homa_agent;

HomaAgent::HomaAgent() : UdpAgent()
{
}

HomaAgent::HomaAgent(packet_t type) : UdpAgent(type)
{
}

void HomaAgent::sendmsg(hdr_homa *homa_hdr, int prio, const char *flags)
{
    assert(homa_hdr->msg_creation_time_var() > 0.0); // check that it has been set.
    size_t headers_size_no_eth = IP_HEADER_SIZE + UDP_HEADER_SIZE + homa_hdr->headerSize();
    size_t payload_size = MAX_ETHERNET_PAYLOAD - headers_size_no_eth;
    size_t total_header_size = headers_size_no_eth + ETHERNET_HEADER_SIZE + INTER_PKT_GAP_SIZE + ETHERNET_PREAMBLE_SIZE;
    int data_bytes = homa_hdr->getDataBytes();
    // std::cout << "homa_hdr->headerSize() " << homa_hdr->headerSize() << " data_bytes: " << data_bytes << " total_header_size " << total_header_size << std::endl;
    assert(data_bytes < 1500); // make sure this will be a single packet (to verify Homa's behavior)
    Packet *p;
    int n;
    // assert(size_ > 0);
    n = data_bytes / payload_size;
    assert(data_bytes >= 0); // grants have size 0
    if (data_bytes == 0)
        data_bytes = 1;
    // std::cout << "sending pkt with prio: " << homa_hdr->getPriority() << "    prio = " << prio << std::endl;
    assert((uint32_t)prio == homa_hdr->getPriority());
    if (prio == NO_PRIO)
    {
        prio = DEFAULT_PRIO;
    }
    slog::log6(debug_, homa_layer_->this_addr_, "HomaAgent::sendmsg(). Max payload size =",
               payload_size, "data bytes =", data_bytes, "prio=", prio, "total_header_size=",
               total_header_size, "homa_hdr->headerSize()", homa_hdr->headerSize(),
               "arrival:", homa_hdr->getArrivalTime(), "creation:", homa_hdr->getCreationTime(),
               "msgID", homa_hdr->msgId_var());

    hdr_homa *new_hdr = nullptr;
    double local_time = Scheduler::instance().clock();
    while (n-- > 0)
    {
        p = allocpkt();

        // shallow copy should be ok since no pointers - FIX
        new_hdr = hdr_homa::access(p);
        (*new_hdr) = *homa_hdr;
        if (new_hdr->pktType_var() == PktType::REQUEST || new_hdr->pktType_var() == PktType::UNSCHED_DATA)
        {
            new_hdr->unschedFields_var().prioUnschedBytes =
                new std::vector<uint32_t>(*homa_hdr->unschedFields_var().prioUnschedBytes);
        }

        // set a random flow id to effectively achieve random per-packet spraying
        hdr_ip::access(p)
            ->flowid() = Random::random();

        hdr_ip::access(p)->prio() = prio;

        // change cmpared to default udp: size_ now will be payload
        // adding all header sizes so that the link transmits payload + headers
        // eth 14, ip 20, udp 8, r2p2 8
        hdr_cmn::access(p)->size() = std::max(payload_size + total_header_size, (size_t)MIN_ETHERNET_FRAME_ON_WIRE);
        hdr_rtp *rh = hdr_rtp::access(p);
        rh->flags() = 0;
        rh->seqno() = ++seqno_;
        hdr_cmn::access(p)->timestamp() =
            (u_int32_t)(SAMPLERATE * local_time);
        // add "beginning of talkspurt" labels (tcl/ex/test-rcvr.tcl)
        if (flags && (0 == strcmp(flags, "NEW_BURST")))
            rh->flags() |= RTP_M;

        if (hdr_homa::access(p)->msgType_var() == HomaMsgType::REQUEST_MSG)
            slog::log5(debug_, homa_layer_->this_addr_,
                       "HomaAgent::sendmsg() of", new_hdr->appLevelId_var(), "to", hdr_ip::access(p)->daddr(), "data |1. Sending packet to addr:", hdr_ip::access(p)->dst().addr_,
                       "iph->daddr():", hdr_ip::access(p)->daddr(),
                       "hdr_cmn::access(p)->size():", hdr_cmn::access(p)->size(),
                       "hdr_ip::access(p)->flowid():", hdr_ip::access(p)->flowid(),
                       "addr():", addr(),
                       "daddr():", daddr(),
                       "type:", new_hdr->msgType_var(),
                       "arrival:", homa_hdr->getArrivalTime(), "creation:", homa_hdr->getCreationTime(),
                       "msgID", homa_hdr->msgId_var());
        else
            slog::log5(debug_, homa_layer_->this_addr_,
                       "HomaAgent::sendmsg() of", new_hdr->appLevelId_var(), "to", hdr_ip::access(p)->daddr(), "cntrl |1. Sending packet to addr:", hdr_ip::access(p)->dst().addr_,
                       "iph->daddr():", hdr_ip::access(p)->daddr(),
                       "hdr_cmn::access(p)->size():", hdr_cmn::access(p)->size(),
                       "hdr_ip::access(p)->flowid():", hdr_ip::access(p)->flowid(),
                       "addr():", addr(),
                       "daddr():", daddr(),
                       "type:", new_hdr->msgType_var(),
                       "arrival:", homa_hdr->getArrivalTime(), "creation:", homa_hdr->getCreationTime(),
                       "msgID", homa_hdr->msgId_var());
        assert(hdr_cmn::access(p)->size() >= MIN_ETHERNET_FRAME_ON_WIRE);
        assert(hdr_cmn::access(p)->size() <= MAX_ETHERNET_FRAME_ON_WIRE);
        target_->recv(p);

        // pkt_id++;
        // only the first packet should have the FIRST FLAG set no matter what
        // homa_hdr.first() = false;
        // homa_hdr.first_urpc() = false; // same applies to uRPCs
    }
    n = data_bytes % payload_size;

    if (n > 0)
    {
        p = allocpkt();
        // homa_hdr.pkt_id() = pkt_id;
        new_hdr = hdr_homa::access(p);
        (*new_hdr) = *homa_hdr;
        if (new_hdr->pktType_var() == PktType::REQUEST || new_hdr->pktType_var() == PktType::UNSCHED_DATA)
        {
            new_hdr->unschedFields_var().prioUnschedBytes =
                new std::vector<uint32_t>(*homa_hdr->unschedFields_var().prioUnschedBytes);
        }
        // The header size also needs to be transmitted. Also, this is over ethernet (64)
        // lets ignore that for now because it makes getting the response size for the client more
        // complicated..
        // hdr_cmn::access(p)->size() = n + HEADERS_SIZE < 64? 64 : n + HEADERS_SIZE;

        hdr_ip::access(p)->flowid() = Random::random();
        hdr_ip::access(p)->prio() = prio;
        hdr_cmn::access(p)->size() = std::max(n + (int)total_header_size, MIN_ETHERNET_FRAME_ON_WIRE);
        hdr_rtp *rh = hdr_rtp::access(p);
        rh->flags() = 0;
        rh->seqno() = ++seqno_;
        hdr_cmn::access(p)->timestamp() =
            (u_int32_t)(SAMPLERATE * local_time);
        // add "beginning of talkspurt" labels (tcl/ex/test-rcvr.tcl)
        if (flags && (0 == strcmp(flags, "NEW_BURST")))
            rh->flags() |= RTP_M;
        if (hdr_homa::access(p)->msgType_var() == HomaMsgType::REQUEST_MSG)
            slog::log5(debug_, homa_layer_->this_addr_,
                       "HomaAgent::sendmsg() of", new_hdr->appLevelId_var(), "to", hdr_ip::access(p)->daddr(), "data |2",
                       "iph->daddr():", hdr_ip::access(p)->daddr(),
                       "hdr_cmn::access(p)->size():", hdr_cmn::access(p)->size(),
                       "hdr_ip::access(p)->flowid():", hdr_ip::access(p)->flowid(),
                       "addr():", addr(),
                       "daddr():", daddr(),
                       "type:", new_hdr->msgType_var(),
                       "arrival:", homa_hdr->getArrivalTime(), "creation:", homa_hdr->getCreationTime(),
                       "msgID", homa_hdr->msgId_var());
        else
            slog::log5(debug_, homa_layer_->this_addr_,
                       "HomaAgent::sendmsg() of", new_hdr->appLevelId_var(), "to", hdr_ip::access(p)->daddr(), "cntrl |2",
                       "iph->daddr():", hdr_ip::access(p)->daddr(),
                       "hdr_cmn::access(p)->size():", hdr_cmn::access(p)->size(),
                       "hdr_ip::access(p)->flowid():", hdr_ip::access(p)->flowid(),
                       "addr():", addr(),
                       "daddr():", daddr(),
                       "type:", new_hdr->msgType_var(),
                       "arrival:", homa_hdr->getArrivalTime(), "creation:", homa_hdr->getCreationTime(),
                       "msgID", homa_hdr->msgId_var());
        if (hdr_cmn::access(p)->size() < MIN_ETHERNET_FRAME_ON_WIRE)
        {
            std::cout << "homa_hdr->headerSize() " << homa_hdr->headerSize() << " data_bytes: " << data_bytes << " total_header_size " << total_header_size << " headers_size_no_eth " << headers_size_no_eth << std::endl;
        }

        assert(hdr_cmn::access(p)->size() >= MIN_ETHERNET_FRAME_ON_WIRE);
        assert(hdr_cmn::access(p)->size() <= MAX_ETHERNET_FRAME_ON_WIRE);
        target_->recv(p);
    }

    idle();
}

void HomaAgent::recv(Packet *pkt, Handler *h)
{
    // hdr_cmn::access(pkt)->size() = hdr_cmn::access(pkt)->size() - HEADERS_SIZE;
    if (hdr_homa::access(pkt)->msgType_var() == HomaMsgType::REQUEST_MSG)
        slog::log5(debug_, homa_layer_->this_addr_, "HomaAgent::recv(). Received data packet of",
                   hdr_homa::access(pkt)->appLevelId_var(), "from", hdr_ip::access(pkt)->src().addr_,
                   "last byte", hdr_homa::access(pkt)->schedDataFields_var().lastByte,
                   "prio", hdr_ip::access(pkt)->prio(), "|", hdr_homa::access(pkt)->getPriority(),
                   "size()", hdr_cmn::access(pkt)->size(), "hdr_ip::access(p)->flowid():",
                   hdr_ip::access(pkt)->flowid());
    else
        slog::log5(debug_, homa_layer_->this_addr_, "HomaAgent::recv(). Received cntrl packet of", hdr_homa::access(pkt)->appLevelId_var(), "from", hdr_ip::access(pkt)->src().addr_, "hdr_ip::access(p)->flowid():", hdr_ip::access(pkt)->flowid());
    hdr_homa::access(pkt)->setArrivalTime(Scheduler::instance().clock());
    if (homa_layer_)
    {
        homa_layer_->recv(pkt, h);
    }
}

void HomaAgent::attach_homa_transport(HomaTransport *homa_transport)
{
    homa_layer_ = homa_transport;
    slog::log6(debug_, homa_layer_->this_addr_, "HomaAgent::attach_homa_transport()");
}
