#include "r2p2-generic.h"

R2p2Generic::R2p2Generic() {}

R2p2Generic::~R2p2Generic() {}

// TODO: I don't think this should be here.
void R2p2Generic::recv(Packet *p, Handler *h) {}

int R2p2Generic::command(int argc, const char *const *argv)
{
    return (NsObject::command(argc, argv));
}

R2p2Transport::R2p2Transport() {}

R2p2Transport::~R2p2Transport() {}

void R2p2Transport::recv(Packet *p, Handler *h) {}

int R2p2Transport::command(int argc, const char *const *argv)
{
    Tcl &tcl = Tcl::instance();
    if (argc == 3)
    {
        if (strcmp(argv[1], "attach-agent") == 0)
        {
            R2p2Agent *r2p2_agent = (R2p2Agent *)TclObject::lookup(argv[2]);
            if (!r2p2_agent)
            {
                tcl.resultf("no such R2P2 agent", argv[2]);
                return (TCL_ERROR);
            }
            attach_r2p2_agent(r2p2_agent);
            return (TCL_OK);
        }
    }
    return (NsObject::command(argc, argv));
}

void R2p2Transport::attach_r2p2_agent(R2p2Agent *r2p2_agent)
{
    assert(r2p2_agents_.count(r2p2_agent->daddr()) == 0);
    r2p2_agents_[r2p2_agent->daddr()] = r2p2_agent;
    r2p2_agent->attach_r2p2_transport(this);
    this_addr_ = r2p2_agent->addr();
}
