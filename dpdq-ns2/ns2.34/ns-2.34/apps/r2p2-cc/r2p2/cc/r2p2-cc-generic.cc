#include "r2p2-cc.h"
#include "simple-log.h"

R2p2CCGeneric::R2p2CCGeneric() {}

R2p2CCGeneric::~R2p2CCGeneric() {}

void R2p2CCGeneric::recv(Packet *pkt, Handler *h)
{
    if (do_trace_)
        trace_state("rcv");
    r2p2_layer_->recv(pkt, h);
}

// TODO: adapt to forward_to_transport
void R2p2CCGeneric::send_to_transport(hdr_r2p2 &r2p2_hdr, int payload, int32_t daddr)
{
    if (do_trace_)
        trace_state("snd");
    if (daddr == -1)
    {
        R2p2Agent *router_agent = r2p2_agents_.at(r2p2_router_addr_);
        // set the client address to this node's address - not important for router
        r2p2_hdr.cl_addr() = router_agent->addr();
        forward_to_transport(std::make_tuple(r2p2_hdr, payload, r2p2_router_addr_), MsgTracerLogs());
    }
    else
    {
        forward_to_transport(std::make_tuple(r2p2_hdr, payload, daddr), MsgTracerLogs());
    }
}

void R2p2CCGeneric::forward_to_transport(packet_info_t pkt_info, MsgTracerLogs &&logs)
{
    try
    {
        r2p2_agents_.at(std::get<2>(pkt_info))->sendmsg(std::get<1>(pkt_info), std::get<0>(pkt_info), MsgTracerLogs(), -1, -1, -1, -1, 0);
    }
    catch (const std::out_of_range &e)
    {
        slog::error(debug_, this_addr_, "R2p2CCGeneric::forward_to_transport() cannot find agent for remote address:", std::get<2>(pkt_info));
        throw;
    }
}

int R2p2CCGeneric::command(int argc, const char *const *argv)
{
    Tcl &tcl = Tcl::instance();
    if (argc == 2)
    {
        if (strcmp(argv[1], "stop-tracing") == 0)
        {
            stop_tracing();
            return (TCL_OK);
        }
    }
    else if (argc == 3)
    {
        if (strcmp(argv[1], "attach-router-agent") == 0)
        {
            R2p2Agent *r2p2_router_agent = (R2p2Agent *)TclObject::lookup(argv[2]);
            if (!r2p2_router_agent)
            {
                tcl.resultf("no such R2P2 agent", argv[2]);
                return (TCL_ERROR);
            }
            attach_r2p2_router_agent(r2p2_router_agent);
            return (TCL_OK);
        }
        else if (strcmp(argv[1], "start-tracing") == 0)
        {
            std::string file_path = argv[2];
            start_tracing(file_path + "/cc_trace.str");
            return (TCL_OK);
        }
    }
    return (R2p2Transport::command(argc, argv));
}

void R2p2CCGeneric::attach_r2p2_router_agent(R2p2Agent *r2p2_router_agent)
{
    r2p2_router_addr_ = r2p2_router_agent->daddr();
    R2p2Transport::attach_r2p2_agent(r2p2_router_agent);
}

void R2p2CCGeneric::attach_r2p2_layer(R2p2 *r2p2_layer)
{
    r2p2_layer_ = r2p2_layer;
}