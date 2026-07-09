#include "ppass-queue.h"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>

#include "flags.h"
#include "packet.h"
#include "ppass-state.h"
#include "r2p2-hdr.h"
#include "r2p2.h"

static class PPassQueueClass : public TclClass {
public:
  PPassQueueClass() : TclClass("Queue/PPassQueue") {}
  TclObject *create(int, const char *const *) { return (new PPassQueue()); }
} class_ppass_queue;

PPassQueue::PPassQueue()
    : q_0_(new PacketQueue()), q_1_(new PacketQueue()), shared_state_(nullptr),
      qib_(0), summarystats(1) {
  pq_ = nullptr;
  bind_bool("queue_in_bytes_", &qib_);
  bind("mean_pktsize_", &mean_pktsize_);
  bind("bw_gbps_", &bw_gbps_);
  bind("rho_", &rho_);
  bind("PThr_", &PThr_);
}

PPassQueue::~PPassQueue() {
  delete q_0_;
  delete q_1_;
}

void PPassQueue::reset() { Queue::reset(); }

int PPassQueue::command(int argc, const char *const *argv) {
  Tcl &tcl = Tcl::instance();

  if (argc == 2) {
    if (strcmp(argv[1], "reset") == 0) {
      reset();
      return TCL_OK;
    }
  } else if (argc == 3) {
    if (!strcmp(argv[1], "set-ppass-state")) {
      PPassState *state = (PPassState *)TclObject::lookup(argv[2]);
      if (state == nullptr) {
        tcl.resultf("no object %s", argv[2]);
        return TCL_ERROR;
      }
      shared_state_ = state;
      return TCL_OK;
    }
  }

  return Queue::command(argc, argv);
}

void PPassQueue::attach_node(Node *node) {
  if (node == nullptr) {
    throw std::runtime_error("attach_node() node is nullptr");
  }
  node_ = node;
}

void PPassQueue::enque(Packet *pkt) {
  if (shared_state_ == nullptr) {
    throw std::runtime_error("PPassQueue::enque() shared_state_ is nullptr");
  }
  if (pkt == nullptr)
    return;
  double now = Scheduler::instance().clock();
  if ((now >= 10.0) && pkt) {
    // std::cout << "hdr_cmn::access(pkt)->size() " <<
    // hdr_cmn::access(pkt)->size() << std::endl;
    assert(now <= 10.0 ||
           hdr_cmn::access(pkt)->size() >= MIN_ETHERNET_FRAME_ON_WIRE);
    assert(now <= 10.0 ||
           hdr_cmn::access(pkt)->size() <= MAX_ETHERNET_FRAME_ON_WIRE);
    hdr_cmn::access(pkt)->size() -=
        (INTER_PKT_GAP_SIZE +
         ETHERNET_PREAMBLE_SIZE); // the inter pkt gap size is not queued
    assert(now <= 10.0 || hdr_cmn::access(pkt)->size() >= MIN_ETHERNET_FRAME);
    assert(now <= 10.0 || hdr_cmn::access(pkt)->size() <= MAX_ETHERNET_FRAME);
    // assert(qlim_ > 0);
  }

  if (summarystats) {
    Queue::updateStats(qib_ ? q_0_->byteLength() + q_1_->byteLength()
                            : q_0_->length() + q_1_->length());
  }
  int qlimBytes = qlim_ * mean_pktsize_;

  // check packet type
  hdr_r2p2 *r2p2_hdr = hdr_r2p2::access(pkt);
  if (r2p2_hdr->msg_type() == hdr_r2p2::GRANT) {
    uint64_t credit = r2p2_hdr->credit();
    shared_state_->increase_qlen(credit);
    q_0_->enque(pkt);
    std::cout << Scheduler::instance().clock() << "PPassQueue::enque()->q_0_, "
              << shared_state_->qlen() << ", " << this << ", " << shared_state_
              << std::endl;
  } else { // not grant pkt
    bool do_drop = (!qib_ && (q_1_->length() + 1) >= qlim_) ||
                   (qib_ && (q_1_->byteLength() +
                             hdr_cmn::access(pkt)->size()) >= qlimBytes);
    if (do_drop) {
      drop(pkt);
      std::cout << Scheduler::instance().clock()
                << " Dropped packet (d) when q_1_ length was: "
                << q_1_->length() << "/" << qlim_
                << " In bytes: " << q_1_->byteLength() << "/" << qlimBytes
                << std::endl;
    } else {
      // unscheduled packet
      if (hdr_r2p2::access(pkt)->unsol_credit() > 0) {
        shared_state_->increase_qlen(hdr_cmn::access(pkt)->size());
      }

      double interval = now - shared_state_->get_lasttime();
      uint64_t delta = static_cast<uint64_t>(
          rho_ * interval * (bw_gbps_ * 1000 * 1000 * 1000) * 1460 / 1500);
      uint64_t Qlen = shared_state_->qlen();
      if (delta > Qlen) {
        shared_state_->reset_qlen();
      } else {
        shared_state_->decrease_qlen(delta);
      }
      shared_state_->set_lasttime(now);
      if (shared_state_->qlen() > PThr_ * mean_pktsize_) {
        hdr_flags *hf = hdr_flags::access(pkt);
        hf->ce() = 1;
      }
      q_1_->enque(pkt);
      std::cout << Scheduler::instance().clock()
                << "PPassQueue::enque()->q_1_, " << shared_state_->qlen()
                << ", " << this << ", " << shared_state_ << std::endl;
    }
  }
}

Packet *PPassQueue::deque() {
  if (shared_state_ == nullptr) {
    throw std::runtime_error("PPassQueue::deque() shared_state_ is nullptr");
  }
  if (summarystats && &Scheduler::instance() != NULL) {
    Queue::updateStats(qib_ ? q_0_->byteLength() + q_1_->byteLength()
                            : q_0_->length() + q_1_->length());
  }
  Packet *pkt = nullptr;
  if (q_0_->length() > 0) {
    pkt = q_0_->deque();
  } else {
    pkt = q_1_->deque();
  }
  double now = Scheduler::instance().clock();
  if ((now >= 10.0) && pkt) {
    // assert(now <= 10.0 || hdr_cmn::access(p)->size() >= MIN_ETHERNET_FRAME);
    assert(now <= 10.0 || hdr_cmn::access(pkt)->size() <= MAX_ETHERNET_FRAME);
    hdr_cmn::access(pkt)->size() +=
        (INTER_PKT_GAP_SIZE +
         ETHERNET_PREAMBLE_SIZE); // the inte pkt gap size is not queued
    // assert(now <= 10.0 || hdr_cmn::access(p)->size() >=
    // MIN_ETHERNET_FRAME_ON_WIRE);
    assert(now <= 10.0 ||
           hdr_cmn::access(pkt)->size() <= MAX_ETHERNET_FRAME_ON_WIRE);
  }
  return pkt;
}
