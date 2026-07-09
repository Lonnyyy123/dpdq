#include <iostream>
#include <cstdlib>
#include <fstream>
#include "r2p2.h"
#include "simple-log.h"

static class R2p2Class : public TclClass
{
public:
    R2p2Class() : TclClass("R2P2") {}
    TclObject *create(int, const char *const *)
    {
        return (new R2p2);
    }
} class_r2p2;

// TODO: bind max_payload to tcl
// R2p2::R2p2() : max_payload_(1000),
//                r2p2_client_(new R2p2Client(this)),
//                r2p2_server_(new R2p2Server(this))
R2p2::R2p2() : r2p2_client_(new R2p2Client(this)),
               r2p2_server_(new R2p2Server(this))
{
    // bind("max_payload_", &max_payload_);
    bind("debug_", &debug_);
    // Warning, this will only work with "static" tcl max_payload_ definition like so:
    // R2P2 set max_payload_ $max_payload_size. If defined afterwords for a specific R2P2
    // object then the client and the server will not know abt it.
    // r2p2_client_->set_max_payload(max_payload_);
    // r2p2_server_->set_max_payload(max_payload_);
}

R2p2::~R2p2()
{
    slog::log2(debug_, r2p2_cc_->this_addr_, "R2p2::~R2p2()");
    delete r2p2_client_;
    delete r2p2_server_;
    for (auto it = thread_id_to_app_.begin();
         it != thread_id_to_app_.end(); ++it)
    {
        delete it->second;
    }
}

void R2p2::attach_r2p2_application(R2p2Application *r2p2_application)
{
    slog::log2(debug_, r2p2_cc_->this_addr_, "R2p2::attach_r2p2_application(): Attaching application with thread id:", r2p2_application->get_thread_id());
    thread_id_to_app_[r2p2_application->get_thread_id()] = r2p2_application;
}

void R2p2::r2p2_send_req(int payload, const RequestIdTuple &request_id_tuple)
{
    r2p2_client_->send_req(payload, request_id_tuple);
}

void R2p2::r2p2_send_response(int nbytes, const RequestIdTuple &request_id_tuple, int new_n)
{
    r2p2_server_->send_response(nbytes, request_id_tuple, new_n);
}

void R2p2::send_to_transport(hdr_r2p2 &r2p2_hdr, int payload, int32_t daddr)
{
    r2p2_cc_->send_to_transport(r2p2_hdr, payload, daddr);
}

void R2p2::send_to_application(hdr_r2p2 &r2p2_hdr, int req_resp_size)
{
    if (r2p2_hdr.msg_type() == hdr_r2p2::REQUEST)
    {
        slog::log3(debug_, r2p2_cc_->this_addr_, "R2p2::send_to_application() (request). r2p2_hdr.sr_thread_id()", r2p2_hdr.sr_thread_id(), "threads in this app:", thread_id_to_app_.size());
        // req_resp_size meaningless for now
        try
        {
            thread_id_to_app_.at(r2p2_hdr.sr_thread_id())->req_recv(req_resp_size, RequestIdTuple(r2p2_hdr.req_id(), r2p2_hdr.app_level_id(), r2p2_hdr.cl_addr(), r2p2_hdr.sr_addr(), r2p2_hdr.cl_thread_id(), r2p2_hdr.sr_thread_id(), r2p2_hdr.msg_creation_time()));
        }
        catch (const std::out_of_range &e)
        {
            slog::error(debug_, r2p2_cc_->this_addr_, "Did not find a server with thread id", r2p2_hdr.sr_thread_id());
            throw;
        }
    }
    else if (r2p2_hdr.msg_type() == hdr_r2p2::REPLY)
    {
        try
        {
            thread_id_to_app_.at(r2p2_hdr.cl_thread_id())->req_success(req_resp_size, RequestIdTuple(r2p2_hdr.req_id(), r2p2_hdr.app_level_id(), r2p2_hdr.cl_addr(), r2p2_hdr.sr_addr(), r2p2_hdr.cl_thread_id(), r2p2_hdr.sr_thread_id(), r2p2_hdr.msg_creation_time()));
        }
        catch (const std::out_of_range &e)
        {
            slog::error(debug_, r2p2_cc_->this_addr_, "Did not find a client with thread id", r2p2_hdr.cl_thread_id());
            throw;
        }
    }
}

void R2p2::recv(Packet *pkt, Handler *h)
{
    int pkt_size = hdr_cmn::access(pkt)->size();
    // TODO: we get a copy here. Necessary, needed for correctness?
    hdr_r2p2 r2p2_hdr = *hdr_r2p2::access(pkt);
    int msg_type = r2p2_hdr.msg_type();
    if (msg_type == hdr_r2p2::REQUEST)
    {
        r2p2_server_->handle_request_pkt(r2p2_hdr, pkt_size);
    }
    else if (msg_type == hdr_r2p2::REQRDY)
    {
        r2p2_client_->handle_req_rdy(r2p2_hdr, pkt_size);
    }
    else if (msg_type == hdr_r2p2::REPLY)
    {
        r2p2_client_->handle_reply_pkt(r2p2_hdr, pkt_size);
    }
    Packet::free(pkt);
}

int R2p2::command(int argc, const char *const *argv)
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
        if (strcmp(argv[1], "attach-cc-module") == 0)
        {
            R2p2CCGeneric *r2p2_cc_module = (R2p2CCGeneric *)TclObject::lookup(argv[2]);
            if (!r2p2_cc_module)
            {
                tcl.resultf("no such R2P2 CC module", argv[2]);
                return (TCL_ERROR);
            }
            attach_cc_module(r2p2_cc_module);
            return (TCL_OK);
        }
        else if (strcmp(argv[1], "start-tracing") == 0)
        {
            std::string file_path = argv[2];
            start_tracing(file_path);
            return (TCL_OK);
        }
    }
    return (R2p2Generic::command(argc, argv));
}

int32_t R2p2::get_local_addr()
{
    return r2p2_cc_->this_addr_;
}

void R2p2::attach_cc_module(R2p2CCGeneric *r2p2_cc_module)
{
    r2p2_cc_ = r2p2_cc_module;
    r2p2_cc_->attach_r2p2_layer(this);
}

void R2p2::start_tracing(std::string file_path)
{
    r2p2_server_->start_tracing(file_path + "servers_trace.str");
}

void R2p2::stop_tracing()
{
    r2p2_server_->stop_tracing();
}