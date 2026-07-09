#include "dpdq-queue.h"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>

#include "flags.h"
#include "msg-tracer.h"
#include "node.h"
#include "r2p2.h"
#include "scheduler.h"

namespace {
void append_dpdq_drop_line(const std::string &path, const std::string &line)
{
	if (path.empty())
		return;
	std::ofstream out(path.c_str(), std::ios::out | std::ios::app);
	if (!out.is_open()) {
		std::cerr << "Could not open DPDQ drop log file " << path << " for append\n";
		return;
	}
	out << line << std::endl;
}
}

static class DPDQQueueClass : public TclClass
{
public:
	DPDQQueueClass() : TclClass("Queue/DPDQQueue") {}
	TclObject *create(int, const char *const *)
	{
		return (new DPDQQueue);
	}
} class_dpdq_queue;

DPDQQueue::DPDQQueue()
	: credit_q_(new PacketQueue()),
	  data_q_(new PacketQueue()),
	  aggregate_q_(new DPDQAggregatePacketQueue(credit_q_, data_q_)),
		  qib_(0),
		  mean_pktsize_(0),
		  summarystats_(1),
		  credit_limit_(0),
		  data_limit_(0),
		  owner_id_(-1),
		  peer_id_(-1),
		  iface_label_(-1)
{
	pq_ = aggregate_q_;
	bind_bool("queue_in_bytes_", &qib_);
	bind("mean_pktsize_", &mean_pktsize_);
	bind_bool("summarystats_", &summarystats_);
	bind("credit_limit_", &credit_limit_);
	bind("data_limit_", &data_limit_);
}

DPDQQueue::~DPDQQueue()
{
	delete aggregate_q_;
	delete credit_q_;
	delete data_q_;
}

void DPDQQueue::reset()
{
	Queue::reset();
}

int DPDQQueue::command(int argc, const char *const *argv)
{
	if (argc == 3) {
		if (strcmp(argv[1], "drop-log-file") == 0) {
			drop_log_file_ = argv[2];
			return TCL_OK;
		}
		if (strcmp(argv[1], "set-owner-id") == 0) {
			owner_id_ = atoi(argv[2]);
			return TCL_OK;
		}
		if (strcmp(argv[1], "set-peer-id") == 0) {
			peer_id_ = atoi(argv[2]);
			return TCL_OK;
		}
		if (strcmp(argv[1], "set-iface-label") == 0) {
			iface_label_ = atoi(argv[2]);
			return TCL_OK;
		}
	}
	return Queue::command(argc, argv);
}

void DPDQQueue::attach_node(Node *node)
{
	if (node == nullptr)
	{
		throw std::runtime_error("attach_node() node is nullptr");
	}
	node_ = node;
}

bool DPDQQueue::is_control_packet(Packet *pkt) const
{
	if (hdr_cmn::access(pkt)->ptype() != PT_UDP)
	{
		return false;
	}
	hdr_r2p2 *r2p2_hdr = hdr_r2p2::access(pkt);
	return r2p2_hdr->msg_type() == hdr_r2p2::GRANT ||
		   r2p2_hdr->msg_type() == hdr_r2p2::GRANT_REQ ||
		   r2p2_hdr->msg_type() == hdr_r2p2::RESEND;
}

void DPDQQueue::normalize_enque_size(Packet *pkt, double now) const
{
	if ((now >= 10.0) && pkt != nullptr)
	{
		assert(now <= 10.0 || hdr_cmn::access(pkt)->size() >= MIN_ETHERNET_FRAME_ON_WIRE);
		assert(now <= 10.0 || hdr_cmn::access(pkt)->size() <= MAX_ETHERNET_FRAME_ON_WIRE);
		hdr_cmn::access(pkt)->size() -= (INTER_PKT_GAP_SIZE + ETHERNET_PREAMBLE_SIZE);
		assert(now <= 10.0 || hdr_cmn::access(pkt)->size() >= MIN_ETHERNET_FRAME);
		assert(now <= 10.0 || hdr_cmn::access(pkt)->size() <= MAX_ETHERNET_FRAME);
	}
}

void DPDQQueue::restore_deque_size(Packet *pkt, double now) const
{
	if ((now >= 10.0) && pkt != nullptr)
	{
		assert(now <= 10.0 || hdr_cmn::access(pkt)->size() <= MAX_ETHERNET_FRAME);
		hdr_cmn::access(pkt)->size() += (INTER_PKT_GAP_SIZE + ETHERNET_PREAMBLE_SIZE);
		assert(now <= 10.0 || hdr_cmn::access(pkt)->size() <= MAX_ETHERNET_FRAME_ON_WIRE);
	}
}

void DPDQQueue::log_drop_event(Packet *pkt, bool is_control) const
{
	if (pkt == nullptr || drop_log_file_.empty())
		return;

	hdr_cmn *cmn = hdr_cmn::access(pkt);
	hdr_r2p2 *r2p2_hdr = nullptr;
	bool is_r2p2 = cmn->ptype() == PT_UDP;
	if (is_r2p2) {
		r2p2_hdr = hdr_r2p2::access(pkt);
	}
	const int32_t cl_addr = is_r2p2 ? r2p2_hdr->cl_addr() : -1;
	const long app_level_id = is_r2p2 ? r2p2_hdr->app_level_id() : -1;

	std::ostringstream oss;
	oss << std::fixed << std::setprecision(10)
	    << Scheduler::instance().clock()
	    << "," << owner_id_
	    << "," << iface_label_
	    << "," << peer_id_
	    << "," << (is_control ? "control" : "data")
	    << "," << (is_r2p2 ? r2p2_hdr->msg_type() : -1)
	    << "," << cmn->size()
	    << "," << credit_q_->byteLength()
	    << "," << data_q_->byteLength()
	    << "," << aggregate_q_->byteLength()
	    << "," << credit_q_->length()
	    << "," << data_q_->length()
	    << "," << aggregate_q_->length()
	    << "," << credit_limit_
	    << "," << data_limit_
		<< "," << cl_addr
		<< "," << hdr_ip::access(pkt)->src().addr_
		<< "," << hdr_ip::access(pkt)->dst().addr_
		<< "," << app_level_id;
	append_dpdq_drop_line(drop_log_file_, oss.str());
}

bool DPDQQueue::should_drop(Packet *pkt, PacketQueue *target_q, int target_limit) const
{
	if (pkt == nullptr)
	{
		throw std::runtime_error("DPDQQueue::should_drop() pkt is nullptr");
	}

	if (target_limit > 0)
	{
		if ((!qib_ && (target_q->length() + 1) >= target_limit) ||
			(qib_ && (target_q->byteLength() + hdr_cmn::access(pkt)->size()) >= target_limit))
		{
			return true;
		}
	}

	if (switch_shared_buffer_ == 1)
	{
		if (node_ == nullptr)
		{
			throw std::runtime_error("DPDQQueue::should_drop() node_ is nullptr");
		}
		return node_->should_drop(hdr_cmn::access(pkt)->size(), mean_pktsize_, qib_);
	}
	if (switch_shared_buffer_ != 0)
	{
		throw std::invalid_argument("Invalid argument for switch_shared_buffer_: " + std::to_string(switch_shared_buffer_));
	}
	return false;
}

void DPDQQueue::enque(Packet *pkt)
{
	if (pkt == nullptr)
	{
		return;
	}

	double now = Scheduler::instance().clock();
	MsgTracer::timestamp_pkt(pkt, MSG_TRACER_ENQUE,
							 hdr_ip::access(pkt)->ttl() == 32 ? MSG_TRACER_HOST_NIC : MSG_TRACER_TOR,
							 MsgTracerLogs());
	normalize_enque_size(pkt, now);

	if (summarystats_)
	{
		Queue::updateStats(qib_ ? aggregate_q_->byteLength() : aggregate_q_->length());
	}

	bool is_control = is_control_packet(pkt);
	PacketQueue *target_q = is_control ? credit_q_ : data_q_;
	int target_limit = is_control ? credit_limit_ : data_limit_;
	if (should_drop(pkt, target_q, target_limit))
	{
		log_drop_event(pkt, is_control);
		drop(pkt);
		return;
	}

	target_q->enque(pkt);
}

Packet *DPDQQueue::deque()
{
	if (summarystats_ && &Scheduler::instance() != NULL)
	{
		Queue::updateStats(qib_ ? aggregate_q_->byteLength() : aggregate_q_->length());
	}

	Packet *pkt = nullptr;
	if (credit_q_->length() > 0)
	{
		pkt = credit_q_->deque();
	}
	else
	{
		pkt = data_q_->deque();
	}

	restore_deque_size(pkt, Scheduler::instance().clock());
	return pkt;
}
