#ifndef ns_r2p2_hdr_h
#define ns_r2p2_hdr_h

#include "ip.h"

#define GRANT_REQ_MSG_SIZE 4
#define GRANT_MSG_SIZE 4
#define REQRDY_MSG_SIZE 1
#define REQ0_MSG_SIZE 14
#define RESEND_MSG_SIZE 4

typedef u_int32_t request_id, packet_id;
struct UniqReqId;

// Remember to edit ns-2.34/tcl/lib/ns-packet.tcl if you want to add a new header type
struct hdr_r2p2
{
public:
    struct UniqReqId
    {
        UniqReqId() {}
        UniqReqId(int32_t claddr, int clthread, request_id reqid) : claddr_(claddr), clthread_(clthread), reqid_(reqid) {}
        int32_t claddr_;
        int clthread_;
        request_id reqid_;
    };

private:
    int msg_type_;
    int seq_;
    int msg_offset_;
    int policy_;
    bool first_;
    bool last_;
    // defined in types.h (typedef short int)
    request_id req_id_;
    int pkt_id_;

    // Additional, non-r2p2 fields (normally in payload or sim-specific)
    int32_t cl_addr_;
    int32_t sr_addr_;
    int cl_thread_id_;
    int sr_thread_id_;
    // for feedback msges
    int n_;
    long reqs_served_;
    long app_level_id_;
    long msg_bytes_; // for pfabric
    // for switch qos (hack) 0-7 (7 = high priority)
    int prio_;
    // for freezing
    double freeze_dur_;
    UniqReqId uniq_req_id_; // repeats same info - messy
    // for uRPC
    bool first_urpc_; // The first packet of a uRPC
    // int msg_size_bytes_; // total msg size in bytes - normally the pkt_id_ of the first pkt of a msg
    // carries this info (in pkts) -> having it in bytes is better but I am not changing it to not
    // break non uRPC code.
    int credit_;           // If grant: the number of packets this grant packet credits (ncluding headers)
    uint32_t resend_bytes_; // Length of a resend request in payload bytes.
    int credit_pad_;       // padding to meet minimum ethernet size. Like many, this is not needed for the real implementation.
    uint64_t credit_req_;  // The number of packets/bytes the sender requests to send.
    long umsg_id_;         // Identifies a micro message in the context of a single host. Useful for
                           // sending all packets of the same uMsg with the same flow id (thus they
                           // shall follow the same path in the core).
    double grant_delay_s_; // used by the grant pacer to delay sending grants by the specified amount of time.
    double msg_creation_time_;
    bool is_pfabric_app_msg_;
    bool sender_marked_;     // ECN marked at sender
    bool bounced_credit_;    // credit packet has been bounced by a paused switch port
    bool is_unsol_pkt_;      // whether this packet was sent unsolicited
    int unsol_credit_;       // amount of self-created credit at the sender for this message.
    int unsol_credit_data_;  // amount of self-created credit at the sender for this message (w/o headers).
    bool used_unsol_credit_; // whether sending this packet consumed unsolicited credit.
    bool has_scheduled_part_;
    // Fields for using HPCC's algorithm at the sender
    uint32_t qlen_; // in bytes
    uint32_t tx_bytes_;
    double ts_;
    double B_;           // in Gbps
    double tx_rate_Bps_; // Tx rate at the sender.
    double dt_;          // distance between this and previous packet.

    bool priority_flow_;
    double bw_ratio_;

public:
    hdr_r2p2() : first_urpc_(false), credit_(0), resend_bytes_(0), credit_pad_(0), credit_req_(0),
                 umsg_id_(-1), grant_delay_s_(0.0), msg_creation_time_(-1.0), is_pfabric_app_msg_(false),
                 sender_marked_(false), bounced_credit_(false), is_unsol_pkt_(false), unsol_credit_(0), unsol_credit_data_(0),
                 used_unsol_credit_(false), has_scheduled_part_(true), qlen_(0), tx_bytes_(0), ts_(0.0), B_(0.0),
                 tx_rate_Bps_(0.0), dt_(0.0), priority_flow_(false), bw_ratio_(-1.0) {}
    hdr_r2p2(const hdr_r2p2 &other) { *this = other; }
    hdr_r2p2 &operator=(const hdr_r2p2 &other)
    {
        if (this == &other)
        {
            return *this;
        }

        msg_type_ = other.msg_type_;
        seq_ = other.seq_;
        msg_offset_ = other.msg_offset_;
        policy_ = other.policy_;
        first_ = other.first_;
        last_ = other.last_;
        req_id_ = other.req_id_;
        pkt_id_ = other.pkt_id_;
        cl_addr_ = other.cl_addr_;
        sr_addr_ = other.sr_addr_;
        cl_thread_id_ = other.cl_thread_id_;
        sr_thread_id_ = other.sr_thread_id_;
        n_ = other.n_;
        reqs_served_ = other.reqs_served_;
        app_level_id_ = other.app_level_id_;
        msg_bytes_ = other.msg_bytes_;
        prio_ = other.prio_;
        freeze_dur_ = other.freeze_dur_;
        uniq_req_id_ = other.uniq_req_id_;
        first_urpc_ = other.first_urpc_;
        credit_ = other.credit_;
        resend_bytes_ = other.resend_bytes_;
        credit_pad_ = other.credit_pad_;
        credit_req_ = other.credit_req_;
        umsg_id_ = other.umsg_id_;
        grant_delay_s_ = other.grant_delay_s_;
        msg_creation_time_ = other.msg_creation_time_;
        is_pfabric_app_msg_ = other.is_pfabric_app_msg_;
        sender_marked_ = other.sender_marked_;
        bounced_credit_ = other.bounced_credit_;
        is_unsol_pkt_ = other.is_unsol_pkt_;
        unsol_credit_ = other.unsol_credit_;
        unsol_credit_data_ = other.unsol_credit_data_;
        used_unsol_credit_ = other.used_unsol_credit_;
        has_scheduled_part_ = other.has_scheduled_part_;
        qlen_ = other.qlen_;
        tx_bytes_ = other.tx_bytes_;
        ts_ = other.ts_;
        B_ = other.B_;
        tx_rate_Bps_ = other.tx_rate_Bps_;
        dt_ = other.dt_;
        priority_flow_ = other.priority_flow_;
        bw_ratio_ = other.bw_ratio_;
        return *this;
    }
    enum MsgTypes
    {
        REQUEST,
        REPLY,
        REQRDY,
        R2P2_FEEDBACK,
        DROP,
        SACK,
        FREEZE,
        UNFREEZE,
        GRANT,
        GRANT_REQ, // REQ0 msgs work as implicit grant requests for requests
        RESEND,
        NUMBER_OF_TYPES
    };

    static const char *get_pkt_type(int type);

    enum Policies
    {
        UNRESTRICTED,
        STICKY
    };

    int &msg_type() { return msg_type_; }
    int &seq() { return seq_; }
    int &msg_offset() { return msg_offset_; }
    int &policy() { return policy_; }
    bool &first() { return first_; }
    bool &last() { return last_; }
    request_id &req_id() { return req_id_; }
    int &pkt_id() { return pkt_id_; }

    int32_t &cl_addr() { return cl_addr_; }
    int32_t &sr_addr() { return sr_addr_; }
    int &cl_thread_id() { return cl_thread_id_; }
    int &sr_thread_id() { return sr_thread_id_; }
    int &n() { return n_; }
    long &reqs_served() { return reqs_served_; }
    long &app_level_id() { return app_level_id_; }
    long &msg_bytes() { return msg_bytes_; }
    int &prio() { return prio_; }
    double &freeze_dur() { return freeze_dur_; }
    UniqReqId &uniq_req_id() { return uniq_req_id_; }
    bool &first_urpc() { return first_urpc_; }
    int &credit() { return credit_; }
    uint32_t &resend_bytes() { return resend_bytes_; }
    int &credit_pad() { return credit_pad_; }
    uint64_t &credit_req() { return credit_req_; }
    long &umsg_id() { return umsg_id_; }
    double &grant_delay_s() { return grant_delay_s_; }
    double &msg_creation_time() { return msg_creation_time_; }
    bool &is_pfabric_app_msg() { return is_pfabric_app_msg_; }
    bool &sender_marked() { return sender_marked_; }
    bool &bounced_credit() { return bounced_credit_; }
    bool &is_unsol_pkt() { return is_unsol_pkt_; }
    int &unsol_credit() { return unsol_credit_; }
    int &unsol_credit_data() { return unsol_credit_data_; }
    bool &used_unsol_credit() { return used_unsol_credit_; }
    bool &has_scheduled_part() { return has_scheduled_part_; }
    uint32_t &qlen() { return qlen_; }
    uint32_t &tx_bytes() { return tx_bytes_; }
    double &ts() { return ts_; }
    double &B() { return B_; }
    double &tx_rate_Bps() { return tx_rate_Bps_; }
    double &dt() { return dt_; }
    bool &priority_flow() { return priority_flow_; }
    double &bw_ratio() { return bw_ratio_; }
    static int offset_;
    inline static int &offset() { return offset_; }
    inline static hdr_r2p2 *access(const Packet *p)
    {
        return (hdr_r2p2 *)p->access(offset_);
    }
};

struct RequestIdTuple
{
    RequestIdTuple(request_id req_id,
                   long app_level_id,
                   int32_t cl_addr,
                   int32_t sr_addr,
                   int cl_thread_id,
                   int sr_thread_id,
                   double ts) : req_id_(req_id),
                                app_level_id_(app_level_id),
                                cl_addr_(cl_addr),
                                sr_addr_(sr_addr),
                                cl_thread_id_(cl_thread_id),
                                sr_thread_id_(sr_thread_id),
                                ts_(ts) {}
    RequestIdTuple(long app_level_id,
                   int32_t cl_addr,
                   int32_t sr_addr,
                   int cl_thread_id,
                   int sr_thread_id,
                   double ts) : app_level_id_(app_level_id),
                                cl_addr_(cl_addr),
                                sr_addr_(sr_addr),
                                cl_thread_id_(cl_thread_id),
                                sr_thread_id_(sr_thread_id),
                                ts_(ts) {}
    RequestIdTuple(long app_level_id, int cl_thread_id) : app_level_id_(app_level_id),
                                                          cl_thread_id_(cl_thread_id) {}
    RequestIdTuple(request_id req_id, int app_level_id) : req_id_(req_id),
                                                          app_level_id_(app_level_id) {}
    RequestIdTuple(){};
    request_id req_id_;
    long app_level_id_;
    int32_t cl_addr_;
    int32_t sr_addr_;
    int cl_thread_id_;
    int sr_thread_id_;
    // for pfabric app
    int msg_bytes_;
    bool is_request_;     // else it is a reply
    int32_t client_port_; // used to figure out which connection to use to send reply
    double ts_;
};

#endif
