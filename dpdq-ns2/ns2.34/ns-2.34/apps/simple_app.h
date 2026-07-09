#ifndef ns_simple_app_h
#define ns_simple_app_h

#include "timer-handler.h"
#include "app.h"

class TcpAgent;
class TfrcAgent;
class SimpleApp;

class SimpleAppTimer : public TimerHandler {
 public:
	SimpleAppTimer(SimpleApp* t) : TimerHandler(), t_(t) {}
	inline virtual void expire(Event*);
 protected:
	SimpleApp* t_;
};


class SimpleApp : public Application {
 public:
	SimpleApp();
	void timeout();
	void recv(int nbytes);
 protected:
	void start();
	void stop();
	int command(int argc, const char*const* argv);
	void print_state_sender();
	void print_state_receiver();
	inline double next();

	double load_;
	double meanInterval_;
	int packetSize_;
	int running_;
	bool isClient_;
	bool isServer_;
	SimpleAppTimer timer_;

	int queries_received_;
	int responses_sent_;

	int queries_sent_;
	int responses_received_;

	int bytes_sent_;
	int bytes_received_;


};

#endif