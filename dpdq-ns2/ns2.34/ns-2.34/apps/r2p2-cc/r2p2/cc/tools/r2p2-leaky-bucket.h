#ifndef ns_r2p2_leaky_bucket_h
#define ns_r2p2_leaky_bucket_h

#include "r2p2-cc.h"
#include "r2p2-cc-micro.h"

// TODO: rename so that it is clear that this is a control msg pacer (paces based on tokens not on
// the actual msgs size)
// It does not forward each bucket item in fragments (eg packets)

class R2p2LeakyBucket;
class R2p2CCMicro;

class LeakTimer : public TimerHandler
{
public:
    LeakTimer(R2p2LeakyBucket *lb) : lb_(lb) {}

protected:
    void expire(Event *);
    R2p2LeakyBucket *lb_;
};

class DeferredSendTimer : public TimerHandler
{
    friend class R2p2LeakyBucket;
public:
    DeferredSendTimer(R2p2LeakyBucket *lb, packet_info_t pkt_info, int tokens_needed) : lb_(lb), pkt_info_(pkt_info), tokens_needed_(tokens_needed) {}

protected:
    void expire(Event *);
    R2p2LeakyBucket *lb_;
    packet_info_t pkt_info_;
    int tokens_needed_;
};

class R2p2LeakyBucket
{
    friend class LeakTimer;
    friend class DeferredSendTimer;

public:
    R2p2LeakyBucket(R2p2CCMicro *cc_module, double link_speed_);
    R2p2LeakyBucket(R2p2CCMicro *cc_module, double link_speed_, int gap_size_ns);
    virtual ~R2p2LeakyBucket();
    virtual void add(const packet_info_t &message, int tokens_needed);
    virtual void set_link_speed(double link_speed_bps);
    virtual void set_link_speed_multiplier(double multiplier);
    virtual double get_link_speed_multiplier();
    virtual void set_idle_gap(int gap_ns);
    virtual size_t size();

protected:
    struct BucketItem
    {
        packet_info_t pkt_info_;
        int tokens_needed_;
    };

    virtual void send();
    /* Returns seconds */
    inline double next_send_time(int nbytes)
    {
        assert(leak_speed_ > 0);
        return (nbytes * 8.0) / ((double)leak_speed_) + ns_to_s(gap_size_ns_);
    }
    inline double ns_to_s(int ns)
    {
        return ns / 1000.0 / 1000.0 / 1000.0;
    }
    std::list<BucketItem *> backlog_;

    LeakTimer leak_timer_;
    R2p2CCMicro *cc_module_;
    bool uplink_commited_;
    double link_speed_; // bps
    double leak_speed_; // bps
    int gap_size_ns_;
    double last_grant_time_;
    double next_grant_in_;
    double current_multiplier_;
};

#endif