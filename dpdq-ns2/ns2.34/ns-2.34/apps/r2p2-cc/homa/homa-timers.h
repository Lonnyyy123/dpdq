/**
 * @file homa-timers.h
 * @author Konstantinos Praspoulos
 * @brief Declares all the timers used by the homa port
 * @date 2022-03-31
 *
 *
 */

#ifndef ns_homa_timers_h
#define ns_homa_timers_h

#include "scheduler.h"
#include <iostream>
#include <assert.h>

class HomaTransport;
struct hdr_homa;

enum HomaEventType
{
    UNDEF_HOMA_EVENT = 0,
    SEND_TIMER_EVENT = 1,
    APP_MSG = 2,
    HOMA_HEADER = 3,
    GRANT_TIMER_EVENT = 4,
    BW_UTIL_EVENT = 5,
};

class HomaEvent : public Event
{
public:
    HomaEvent() : event_type_(HomaEventType::UNDEF_HOMA_EVENT),
                  homa_hdr_(nullptr) {}
    HomaEvent(HomaEventType event_type) : event_type_(event_type) {}
    virtual ~HomaEvent() {}
    HomaEventType event_type_;
    hdr_homa *homa_hdr_;
};

// ===================== Homa Timer Handler ===================
// Mostly done to have a pointer to Event instead of Event to be able to downcast to HomaEvent

class HomaTimerHandler : public Handler
{
public:
    HomaTimerHandler(Event *event) : event_(event), status_(IDLE), lastScheduledFor_(0.0) {}
    virtual ~HomaTimerHandler()
    {
        delete event_;
    }

    void sched(double delay);   // cannot be pending
    void resched(double delay); // may or may not be pending
                                // if you don't know the pending status
                                // call resched()
    void cancel();              // must be pending
    void cancel_if_pending();

    // ---- Homa compatible functions ----
    virtual bool isScheduled();

    /**
     * From OMNET++: getArrivalTime():
     * Returns the simulation time this event object has been last scheduled for
     * (regardless whether it is currently scheduled), or zero if the event
     * hasn't been scheduled yet.
     * KP: interpretation clarifications.
     * - If the timer was never scheduled, return 0.
     * - If the timer was scheduled but then cancelled, return the time the timer was supposed to fire at.
     * Check cSimpleModule::arrived (OMNET codebase) for msg->setArrivalTime(t); (not sure)
     */
    double getArrivalTime();

    // ---- Homa compatible functions end ----

    inline void force_cancel()
    { // cancel!
        if (status_ == PENDING)
        {
            _cancel();
            status_ = IDLE;
        }
    }
    enum TimerStatus
    {
        IDLE,
        PENDING,
        HANDLING
    };
    int status() { return status_; };
    Event *event_;

protected:
    virtual void expire(Event *) = 0; // must be filled in by client
    // Should call resched() if it wants to reschedule the interface.

    virtual void handle(Event *);
    int status_;
    // Time at which this timer has last been scheduled for - absolute time.
    double lastScheduledFor_;

private:
    inline void _schedule(double delay)
    {
        lastScheduledFor_ = Scheduler::instance().clock() + delay;
        // assert(event_);
        (void)Scheduler::instance().schedule(this, event_, delay);
    }
    inline void _cancel()
    {
        (void)Scheduler::instance().cancel(event_);
    }
};

// ====================== Timers =========================

class HomaSendTimer : public HomaTimerHandler
{
public:
    HomaSendTimer(HomaTransport *transport, Event *event) : HomaTimerHandler(event),
                                                            transport_(transport) {}

protected:
    void expire(Event *event) override;
    HomaTransport *transport_;
};

class HomaGrantTimer : public HomaTimerHandler
{
public:
    HomaGrantTimer(HomaTransport *transport, uint64_t timer_id, Event *e) : HomaTimerHandler(e),
                                                                            timer_id_(timer_id),
                                                                            transport_(transport) {}
    uint64_t timer_id_; // will be used as a key - is meant to be unique - could be the address of the sender
    bool operator==(const HomaGrantTimer &other) const
    {
        return timer_id_;
    }

    class HomaGrantTimerHash
    {
    public:
        // Use sum of lengths of first and last names
        // as hash function.
        size_t operator()(const HomaGrantTimer *timer) const
        {
            return timer->timer_id_;
        }
    };

protected:
    void expire(Event *event) override;
    HomaTransport *transport_;
};

class HomaSchedBwUtilTimer : public HomaTimerHandler
{
public:
    HomaSchedBwUtilTimer(HomaTransport *transport, Event *e) : HomaTimerHandler(e),
                                                               transport_(transport) {}

protected:
    void expire(Event *event) override;
    HomaTransport *transport_;
};

#endif