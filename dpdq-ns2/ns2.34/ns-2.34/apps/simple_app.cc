#include <stdio.h>
#include <string.h>
#include "random.h"
#include "tcp.h"
#include "simple_app.h"

//extern double tcplib_telnet_interarrival();

static class SimpleAppClass : public TclClass {
 public:
	SimpleAppClass() : TclClass("Application/Simple") {}
	TclObject* create(int, const char*const*) {
		return (new SimpleApp);
	}
} class_app_simple;


SimpleApp::SimpleApp() : running_(0), timer_(this)
{
	bind("load_", &load_);
	bind("packetSize_", &packetSize_);
}


void SimpleAppTimer::expire(Event*)
{
    t_->timeout();
}


void SimpleApp::start()
{
    running_ = 1;
    // hardcoded for a server response time of 100us 
    // if load is 1 then 1 packet will be sent (on avg) every 100us
	meanInterval_ = (1.0/load_)*0.0001;
	int queries_received_ = 0;
	int responses_sent_ = 0;
	int queries_sent_ = 0;
	int responses_received_ = 0;
	int bytes_sent_ = 0;
	int bytes_received_ = 0;
	timer_.sched(next());

}

void SimpleApp::stop()
{
    running_ = 0;
}

void SimpleApp::recv(int nbytes)
{
	// times_rcvd_ ++;
	// bytes_rcvd_ += nbytes;
}

void SimpleApp::timeout()
{
	// Called by scheduler on poisson times
    if (running_) {
        /* call the TCP advance method */
		agent_->sendmsg(packetSize_);
		/* reschedule the timer */
		timer_.resched(next());
	}
}

double SimpleApp::next()
{
    return Random::exponential(meanInterval_);
}

int SimpleApp::command(int argc, const char*const* argv) {
      if(argc == 2) {
           if(strcmp(argv[1], "print_state_sender") == 0) {
                  print_state_sender();
                  return(TCL_OK);
           }
           if(strcmp(argv[1], "print_state_receiver") == 0) {
                  print_state_receiver();
                  return(TCL_OK);
           }
      }
      return(Application::command(argc, argv));
}

void SimpleApp::print_state_sender()
{
	Tcl& tcl = Tcl::instance();
    tcl.eval("puts \"Message From Sender\"");
    // tcl.evalf("puts \" Queries Sent  = %d. Received %d bytes\"", times_rcvd_, bytes_rcvd_);
    // tcl.evalf("puts \" Responses received  = %d. Received %d bytes\"", times_rcvd_, bytes_rcvd_);
}

void SimpleApp::print_state_receiver()
{
	Tcl& tcl = Tcl::instance();
    tcl.eval("puts \"Message From Receiver\"");
    // tcl.evalf("puts \" Queries received  = %d. Received %d bytes\"", times_rcvd_, bytes_rcvd_);
    // tcl.evalf("puts \" Responses Sent  = %d. Received %d bytes\"", times_rcvd_, bytes_rcvd_);
}
