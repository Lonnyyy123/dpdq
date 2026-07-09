#include "r2p2-leaky-bucket.h"
#include "simple-log.h"

R2p2LeakyBucket::R2p2LeakyBucket(R2p2CCMicro *cc_module,
                                 double link_speed) : leak_timer_(this),
                                                      cc_module_(cc_module),
                                                      uplink_commited_(false),
                                                      link_speed_(link_speed),
                                                      leak_speed_(link_speed),
                                                      gap_size_ns_(0),
                                                      current_multiplier_(1.0)
{
    slog::log2(cc_module_->get_debug(), cc_module_->this_addr_, "R2p2LeakyBucket()", link_speed_, leak_speed_, gap_size_ns_);
}

R2p2LeakyBucket::R2p2LeakyBucket(R2p2CCMicro *cc_module,
                                 double link_speed,
                                 int gap_size_ns) : leak_timer_(this),
                                                    cc_module_(cc_module),
                                                    uplink_commited_(false),
                                                    link_speed_(link_speed),
                                                    leak_speed_(link_speed),
                                                    gap_size_ns_(gap_size_ns),
                                                    last_grant_time_(0.0),
                                                    next_grant_in_(0.0),
                                                    current_multiplier_(1.0)
{
    assert(gap_size_ns >= 0);
    slog::log2(cc_module_->get_debug(), cc_module_->this_addr_, "R2p2LeakyBucket()", link_speed_, leak_speed_, gap_size_ns_);
}

// TODO: add set_variables function or smthing
R2p2LeakyBucket::~R2p2LeakyBucket()
{
}

void R2p2LeakyBucket::add(const packet_info_t &message, int tokens_needed)
{
    hdr_r2p2 r2p2_hdr = std::get<0>(message);
    slog::log4(cc_module_->get_debug(), cc_module_->this_addr_, "R2p2LeakyBucket::add(). Tokens needed:", tokens_needed, "msg type", r2p2_hdr.msg_type(), "grant delay=", r2p2_hdr.grant_delay_s() * 1000.0 * 1000.0, "us");
    if (r2p2_hdr.msg_type() == hdr_r2p2::REQRDY || r2p2_hdr.msg_type() == hdr_r2p2::GRANT)
    {
        assert(r2p2_hdr.credit() > 0);
        // check that the delay is within reasonable bounds
        assert(r2p2_hdr.grant_delay_s() * 1000.0 * 1000.0 < 1000.0);
        assert(r2p2_hdr.grant_delay_s() * 1000.0 * 1000.0 > -0.00001);
        if (r2p2_hdr.msg_type() == hdr_r2p2::REQRDY)
        {
            assert(r2p2_hdr.grant_delay_s() * 1000.0 * 1000.0 < 0.00001);
        }
    }
    else
    {
        assert(r2p2_hdr.grant_delay_s() * 1000.0 * 1000.0 < 0.00001);
    }
    assert(tokens_needed > 0);
    if (r2p2_hdr.msg_type() == hdr_r2p2::GRANT && r2p2_hdr.grant_delay_s() * 1000.0 * 1000.0 > 0.00001)
    {
        // set delay to 0 and set a timer for this grant in the future. When the timer expires, the grant will be re-added
        double delay = r2p2_hdr.grant_delay_s();
        DeferredSendTimer *deffered_timer = new DeferredSendTimer(this, message, tokens_needed);
        std::get<0>(deffered_timer->pkt_info_).grant_delay_s() = 0.0;
        assert(std::get<0>(deffered_timer->pkt_info_).grant_delay_s() * 1000.0 * 1000.0 < 0.00001);
        deffered_timer->resched(delay);
        return;
    }
    BucketItem *bkt_item = new BucketItem();
    bkt_item->pkt_info_ = message;
    bkt_item->tokens_needed_ = tokens_needed;
    backlog_.push_back(bkt_item);
    if (!uplink_commited_)
        slog::log4(cc_module_->get_debug(), cc_module_->this_addr_, "Uplink was free, sending...");
    send();
}

void R2p2LeakyBucket::send()
{
    slog::log5(cc_module_->get_debug(), cc_module_->this_addr_, "R2p2LeakyBucket::send()");
    if (!backlog_.empty())
    {
        BucketItem *bkt_item = backlog_.front();
        slog::log5(cc_module_->get_debug(), cc_module_->this_addr_, "R2p2LeakyBucket sending now, tokens_needed_ =", bkt_item->tokens_needed_, "gap_size_ns_:", gap_size_ns_);
        uplink_commited_ = true;
        // Then again, classes using this, to this point (08/2021), will give overestimations of
        // the crefits neede because they round up to full packets.
        assert(gap_size_ns_ >= 0);
        next_grant_in_ = next_send_time(bkt_item->tokens_needed_);
        assert(next_grant_in_ > 0);
        leak_timer_.resched(next_grant_in_);
        last_grant_time_ = Scheduler::instance().clock();
        cc_module_->forward_to_transport(bkt_item->pkt_info_, MsgTracerLogs());
        backlog_.pop_front();
        slog::log4(cc_module_->get_debug(), cc_module_->this_addr_, "R2p2LeakyBucket sent item that needed", bkt_item->tokens_needed_, "tokens. Next item in", next_grant_in_, "seconds (leak speed:", leak_speed_, ")");
        delete bkt_item;
    }
    else
    {
        uplink_commited_ = false;
    }
}

void R2p2LeakyBucket::set_link_speed_multiplier(double multiplier)
{
    slog::log6(cc_module_->get_debug(), cc_module_->this_addr_, "R2p2LeakyBucket::set_link_speed_multiplier() to:", multiplier);
    leak_speed_ = link_speed_ * multiplier;
    current_multiplier_ = multiplier;
    if (uplink_commited_)
    {
        double now = Scheduler::instance().clock();
        double time_since_sched = now - last_grant_time_;
        // approximation - is not correct when multiplier is reset to 1
        double new_next_grant_in = next_grant_in_ / multiplier;
        slog::log6(cc_module_->get_debug(), cc_module_->this_addr_, time_since_sched, last_grant_time_, new_next_grant_in);
        if (new_next_grant_in <= time_since_sched)
        {
            // change in rate means that the grant should be sent in the past.
            leak_timer_.resched(now + 0.00000001);
        }
        else
        {
            leak_timer_.resched(new_next_grant_in - time_since_sched);
        }
        next_grant_in_ = new_next_grant_in;
    }
}

void R2p2LeakyBucket::set_link_speed(double link_speed_bps)
{
    slog::log2(cc_module_->get_debug(), cc_module_->this_addr_, "R2p2LeakyBucket::set_link_speed() to:", link_speed_bps);
    link_speed_ = link_speed_bps;
    leak_speed_ = link_speed_;
}

double R2p2LeakyBucket::get_link_speed_multiplier(void)
{
    return current_multiplier_;
}

void R2p2LeakyBucket::set_idle_gap(int gap_ns)
{
    slog::log4(cc_module_->get_debug(), cc_module_->this_addr_, "R2p2LeakyBucket::set_idle_gap() to:", gap_ns);
    assert(gap_ns >= 0);
    gap_size_ns_ = gap_ns;
}

size_t R2p2LeakyBucket::size()
{
    return backlog_.size();
}

void LeakTimer::expire(Event *e)
{
    lb_->send();
}

void DeferredSendTimer::expire(Event *e)
{
    slog::log4(lb_->cc_module_->get_debug(), lb_->cc_module_->this_addr_, "DeferredSendTimer timer has expired");
    assert(std::get<0>(pkt_info_).grant_delay_s() * 1000.0 * 1000.0 < 0.000001);
    lb_->add(pkt_info_, tokens_needed_);
}