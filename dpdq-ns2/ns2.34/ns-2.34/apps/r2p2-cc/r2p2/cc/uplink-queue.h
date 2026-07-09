#ifndef ns_r2p2_uplink_q_h
#define ns_r2p2_uplink_q_h

#include <map>
#include "r2p2-cc.h"

// NOTE: r2p2_hdr.prio() is ignored by droptail objects but is used by this class
class R2p2UplinkQueue;
class R2p2CCGeneric;

class DrainTimer : public TimerHandler
{
public:
    DrainTimer(R2p2UplinkQueue *q) : q_(q) {}

protected:
    void expire(Event *);
    R2p2UplinkQueue *q_;
};

class R2p2UplinkQueue
{
    friend class DrainTimer;

public:
    enum DeqPolicy
    {
        FCFS,
        RR_MSG,
        RR_DEST,
        HIGHEST_PRIO
    };

    R2p2UplinkQueue(R2p2CCGeneric *cc_module);
    R2p2UplinkQueue(R2p2CCGeneric *cc_module, double link_speed);
    virtual ~R2p2UplinkQueue();
    void enque(packet_info_t message, int extra_prio);
    void enque(packet_info_t message);
    void freeze(uniq_req_id_t req_id, double freeze_dur);
    void unfreeze(uniq_req_id_t req_id);
    void check_stopped();
    void set_deque_policy(DeqPolicy policy);
    void set_link_speed(double link_speed_bps);
    size_t size();
    size_t num_uniq_msgid();
    size_t num_destinations();

protected:
    struct QueueItem
    {
        QueueItem() : prio_(0), is_stopped_(false), stopped_indef_(false),
                      reply_(false), extra_prio_(-1) {}
        uniq_req_id_t req_id_;
        packet_info_t pkt_info_;
        int bytes_left_;
        int prio_;
        bool is_stopped_;
        double stopped_until_;
        double stopped_indef_;
        int pkt_id_;
        bool first_;
        bool urpc_first_;
        bool reply_;
        int extra_prio_;
    };

    void send();
    int next_message();
    int message_rr();
    int destination_rr();
    int find_highest_prio_message();
    inline double calc_trans_delay(int nbytes)
    {
        return (nbytes * 8) / link_speed_;
    }
    // TODO: pass through TCL
    int headers_size_ = HEADERS_SIZE;
    int pkt_size = PACKET_SIZE;
    int max_payload_size_ = (PACKET_SIZE - HEADERS_SIZE);
    bool uplink_occupied_;
    double link_speed_;
    DrainTimer drain_timer_;
    std::vector<QueueItem> q_;
    R2p2CCGeneric *cc_module_;
    DeqPolicy policy_;
    int last_index_;
    std::map<int32_t, int> msgs_per_dest_;
    int last_dest_indx_;
    std::map<uniq_req_id_t, int> msgs_per_uniqreqid_;
    int last_uniqreqid_indx_;
};

#endif