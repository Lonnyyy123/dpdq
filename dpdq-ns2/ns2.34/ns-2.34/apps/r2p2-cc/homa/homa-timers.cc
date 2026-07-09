#include "homa-timers.h"
#include "homa-transport.h"
#include <stdlib.h> // abort()

// ===================== Homa Timer Handler ===================

void HomaTimerHandler::cancel()
{
    if (status_ != PENDING)
    {
        fprintf(stderr,
                "Attempting to cancel timer at %p which is not scheduled\n",
                reinterpret_cast<void *>(this));
        abort();
    }
    _cancel();
    status_ = IDLE;
}

void HomaTimerHandler::cancel_if_pending()
{
    if (status_ == PENDING)
    {
        _cancel();
    }
    else
    {
        assert(status_ == IDLE);
    }
    status_ = IDLE;
}

/* sched checks the state of the timer before shceduling the
 * event. It the timer is already set, abort is called.
 * This is different than the OTcl timers in tcl/ex/timer.tcl,
 * where sched is the same as reshced, and no timer state is kept.
 */
void HomaTimerHandler::sched(double delay)
{
    if (status_ != IDLE)
    {
        fprintf(stderr, "HomaTimerHandler::sched(): Couldn't schedule timer because it is not IDLE\n");
        abort();
    }
    _schedule(delay);
    status_ = PENDING;
}

void HomaTimerHandler::resched(double delay)
{
    if (status_ == PENDING)
    {
        _cancel();
    }
    _schedule(delay);
    status_ = PENDING;
}

void HomaTimerHandler::handle(Event *e)
{
    // printf("HomaTimerHandler::handle() %d at %f\n", status_, Scheduler::instance().clock());
    if (status_ != PENDING) // sanity check
        abort();
    status_ = HANDLING;
    expire(e);
    // if it wasn't rescheduled, it's done
    if (status_ == HANDLING)
        status_ = IDLE;
}

bool HomaTimerHandler::isScheduled()
{
    return (status_ == TimerStatus::PENDING);
}

/**
 * getArrivalTime():
 * Returns the simulation time this event object has been last scheduled for
 * (regardless whether it is currently scheduled), or zero if the event
 * hasn't been scheduled yet.
 */
double HomaTimerHandler::getArrivalTime()
{
    return lastScheduledFor_;
}

// ===================== Homa Timer Handler END ===================

void HomaSendTimer::expire(Event *event)
{
    HomaEvent *e = dynamic_cast<HomaEvent *>(event);
    assert(e != nullptr);
    // std::cout << "HomaSendTimer::expire. event type:" << e->event_type_ << std::endl;
    transport_->sxController.sendOrQueue(event);
}

void HomaGrantTimer::expire(Event *event)
{
    // std::cout << "HomaGrantTimer::expire()" << std::endl;
    transport_->rxScheduler.processGrantTimers(this);
}

void HomaSchedBwUtilTimer::expire(Event *event)
{
    // std::cout << "HomaSchedBwUtilTimer::expire()" << std::endl;
    transport_->rxScheduler.schedSenders->handleBwUtilTimerEvent(this);
}
