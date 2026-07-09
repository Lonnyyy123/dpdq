#ifndef ns_msg_tracer_h
#define ns_msg_tracer_h

#include <stdint.h>
#include <stdio.h>
#include <vector>
#include <map>
#include <string>
#include "r2p2-hdr.h" // ideally, the tracer would be protocol agnostic. Must figure out a generic pkt identifier etc..

/* Operations */
#define MSG_TRACER_SEND 0
#define MSG_TRACER_RECEIVE 1
#define MSG_TRACER_ENQUE 2
#define MSG_TRACER_DEQUE 3

/* Host types */
#define MSG_TRACER_HOST 0
#define MSG_TRACER_HOST_NIC 1
#define MSG_TRACER_SWITCH 2
#define MSG_TRACER_TOR 3
#define MSG_TRACER_SPINE 4

#define NUM_NW_TS 12 /* preallocated because PktTracerEntry is a field in hdr_cmn. */

class Packet;
class PktTracerEntry;
struct MsgTracerLogs;

typedef std::pair<uint64_t, int32_t> MsgUid;           // app_lvl_id and msg source address
typedef std::map<int, PktTracerEntry *> MsgTracerPkts; // pkt uid (from cmn header) to pkt info

struct MsgTracerEntry
{
    MsgTracerEntry(uint32_t msg_size,
                   uint64_t msg_uid,
                   int32_t saddr,
                   int32_t daddr,
                   std::string type) : msg_size_(msg_size),
                                       msg_start_(0.0),
                                       msg_end_(0.0),
                                       msg_uid_(msg_uid),
                                       saddr_(saddr),
                                       daddr_(daddr),
                                       msg_type_(type) {}
    ~MsgTracerEntry();

    uint32_t msg_size_;
    double msg_start_;
    double msg_end_;
    uint64_t msg_uid_;
    int32_t saddr_;
    int32_t daddr_;
    std::string msg_type_;
    double slowdown_;
    double grant_req_delay_; // time since msg was initiated and the first pkt was sent
    MsgTracerPkts packets_;  // the packets making this message
};

struct MsgTracerLog
{
    MsgTracerLog() : name_("Undefined"), value_("Undefined") {}
    MsgTracerLog(std::string name, std::string value) : name_(name), value_(value) {}
    std::string name_;
    std::string value_;
};

struct MsgTracerLogs
{
    std::vector<MsgTracerLog> logs_;
};

struct TimestampTracerEntry
{
    TimestampTracerEntry() : ts_(-1.0), node_(-1), node_type_(0), operation_(0) {}
    TimestampTracerEntry(double ts,
                         int32_t node,
                         uint8_t type,
                         uint8_t op) : ts_(ts),
                                       node_(node),
                                       node_type_(type),
                                       operation_(op) {}

    ~TimestampTracerEntry() {}

    double ts_;
    /* Node that timestamped */
    int32_t node_;
    uint8_t node_type_;
    /* Name of the operation related to the timestamp*/
    uint8_t operation_;
    MsgTracerLogs logs_;
};

struct PktTracerEntry
{
    PktTracerEntry() : pkt_id_(-1),
                       pkt_type_(0),
                       pkt_start_(0.0),
                       pkt_end_(0.0),
                       ts_count_(0) {}
    ~PktTracerEntry();
    void add_ts(TimestampTracerEntry ts);

    int pkt_id_;
    uint8_t pkt_type_;
    double pkt_start_;
    double pkt_end_;
    TimestampTracerEntry timestamps[NUM_NW_TS];
    size_t ts_count_;
};

class MsgTracer
{
public:
    /**
     * Call to initialize the tracer
     */
    static void init_tracer(int debug);
    /**
     * Call to wrap up
     */
    static void finish();
    /**
     * Call to initialize the message when the message is created at the appliction layer.
     * Client address along with msg_uid globally identify the message transaction (saddr changes for control packets flowing in the opposite direction)
     */
    static void app_init_msg(uint64_t msg_uid, int32_t client_addr, int32_t saddr, int32_t daddr, uint32_t msg_size, std::string type);
    /**
     * Call to timestamp a packet when an operation happens in a network node.
     */
    static void timestamp_pkt(Packet *pkt, uint8_t op, uint8_t node_type, MsgTracerLogs &&logs);
    /**
     * Call when the application has completely received the message.
     */
    static void complete_msg(uint64_t msg_uid, int32_t client_addr, int32_t saddr);

    static void set_out_file(std::string file)
    {
        file_path_ = file;
    }
    static int do_trace_;

private:
    MsgTracer(){};
    static std::map<MsgUid, MsgTracerEntry *> pending_;
    static void init_json();
    /**
     * Call when a message is complete and it should be written to the json file.
     */
    static void output_msg_json(MsgTracerEntry *entry);
    static void finish_json();

    static void print_pkt_detail(MsgTracerEntry *entry);

    static std::string file_path_;
    static int debug_;
    static bool inited_;
    static bool finished_;
    static bool first_entry_;
};

#endif
