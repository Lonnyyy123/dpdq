#ifndef ns_dpdq_queue_h
#define ns_dpdq_queue_h

#include <string>

#include "queue.h"

class Node;

class DPDQAggregatePacketQueue : public PacketQueue
{
public:
	DPDQAggregatePacketQueue(PacketQueue *credit_q, PacketQueue *data_q)
		: credit_q_(credit_q), data_q_(data_q) {}

	int length() const override
	{
		return credit_q_->length() + data_q_->length();
	}

	int byteLength() const override
	{
		return credit_q_->byteLength() + data_q_->byteLength();
	}

private:
	PacketQueue *credit_q_;
	PacketQueue *data_q_;
};

class DPDQQueue : public Queue
{
public:
	DPDQQueue();
	~DPDQQueue() override;

	void reset();
	int command(int argc, const char *const *argv) override;
	void enque(Packet *pkt) override;
	Packet *deque() override;
	void attach_node(Node *node) override;

private:
	bool is_control_packet(Packet *pkt) const;
	bool should_drop(Packet *pkt, PacketQueue *target_q, int target_limit) const;
	void normalize_enque_size(Packet *pkt, double now) const;
	void restore_deque_size(Packet *pkt, double now) const;
	void log_drop_event(Packet *pkt, bool is_control) const;

	PacketQueue *credit_q_;
	PacketQueue *data_q_;
	DPDQAggregatePacketQueue *aggregate_q_;

	int qib_;
	int mean_pktsize_;
	int summarystats_;
	int credit_limit_;
	int data_limit_;
	int owner_id_;
	int peer_id_;
	int iface_label_;
	std::string drop_log_file_;
};

#endif
