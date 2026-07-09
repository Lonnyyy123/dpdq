#ifndef ns_ppass_queue_h
#define ns_ppass_queue_h

#include "queue.h"

class Packet;
class PPassState;

class PPassQueue : public Queue {
public:
  PPassQueue();
  ~PPassQueue() override;

  void reset() override;
  void enque(Packet *pkt) override;
  Packet *deque() override;
  int command(int argc, const char *const *argv) override;
  void attach_node(Node *node) override;

private:
  PacketQueue *q_0_;
  PacketQueue *q_1_;
  PPassState *shared_state_;
  int qib_;          /* bool: queue measured in bytes? */
  int mean_pktsize_; /* configured mean packet size in bytes */
  double bw_gbps_;
  double rho_;
  int PThr_;

  int summarystats;
};

#endif // ns_ppass_queue_h
