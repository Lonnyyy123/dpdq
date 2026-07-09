#ifndef ns_udp_r2p2_h
#define ns_udp_r2p2_h

#include "udp.h"
#include "r2p2-generic.h"
#include "r2p2-hdr.h"
#include "msg-tracer.h"
#include <array>
#include <iostream>
#include <string>
#include <unordered_set>

#define FID_TO_REQID -1
#define DEFAULT_PRIO 7
#define NO_PRIO -1
#define NO_CURRENT_TTL -1
#define NO_SRC_REPLACEMENT -1
#define ECN_CAPABLE 1

class R2p2Transport;

class R2p2Agent : public UdpAgent
{
public:
    R2p2Agent();
    R2p2Agent(packet_t);
    // hack. if fid==-1, set fid to r2p2 req_id,
    // prio 0-7 -> 0 = high priority
    // ECN capable if ecn_capable==1, else not.
    void sendmsg(int nbytes, hdr_r2p2 &r2p2_hdr, MsgTracerLogs &&logs, int fid = FID_TO_REQID, int prio = NO_PRIO, int current_ttl = NO_CURRENT_TTL, int32_t source_addr = NO_SRC_REPLACEMENT, int ecn_capable = ECN_CAPABLE, const char *flags = 0);
    void recv(Packet *, Handler *);

    void attach_r2p2_transport(R2p2Transport *r2p2_transport);
    int command(int argc, const char *const *argv) override;
    static void register_foreground_message(int32_t cl_addr, long app_level_id);

private:
    static std::string packet_stats_file_;
    static std::unordered_set<std::string> foreground_messages_;
    static std::array<std::array<unsigned long long, 2>, 2> packet_counts_;
    static bool packet_stats_written_;

    R2p2Transport *r2p2_layer_;
    std::string send_log_file_;

    static bool is_credit_type(int msg_type);
    static std::string make_message_key(int32_t cl_addr, long app_level_id);
    static bool is_foreground_message(hdr_r2p2 *r2p2_hdr);
    static void write_packet_stats();
    void log_send_event(Packet *p) const;

};

#endif