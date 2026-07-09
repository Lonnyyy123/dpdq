/* -*-	Mode:C++; c-basic-offset:8; tab-width:8; indent-tabs-mode:t -*- */
/*
 * Copyright (c) 1994 Regents of the University of California.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the Computer Systems
 *	Engineering Group at Lawrence Berkeley Laboratory.
 * 4. Neither the name of the University nor of the Laboratory may be used
 *    to endorse or promote products derived from this software without
 *    specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifndef lint
static const char rcsid[] =
	"@(#) $Header: /cvsroot/nsnam/ns-2/queue/drop-tail.cc,v 1.17 2004/10/28 23:35:37 haldar Exp $ (LBL)";
#endif

#include "drop-tail.h"
#include <iostream>
#include "flags.h"
#include "r2p2.h"
#include "homa-hdr.h"
#include "msg-tracer.h"
#include "node.h"

static class DropTailClass : public TclClass
{
public:
	DropTailClass() : TclClass("Queue/DropTail") {}
	TclObject *create(int, const char *const *)
	{
		return (new DropTail);
	}
} class_drop_tail;

void DropTail::reset()
{
	Queue::reset();
}

int DropTail::command(int argc, const char *const *argv)
{
	if (argc == 2)
	{
		if (strcmp(argv[1], "printstats") == 0)
		{
			print_summarystats();
			return (TCL_OK);
		}
		if (strcmp(argv[1], "shrink-queue") == 0)
		{
			shrink_queue();
			return (TCL_OK);
		}
	}
	if (argc == 3)
	{
		if (!strcmp(argv[1], "packetqueue-attach"))
		{
			delete q_;
			if (!(q_ = (PacketQueue *)TclObject::lookup(argv[2])))
				return (TCL_ERROR);
			else
			{
				pq_ = q_;
				return (TCL_OK);
			}
		}
	}
	return Queue::command(argc, argv);
}

/*
 * drop-tail
 */
void DropTail::enque(Packet *p)
{
	// Temporary check - remove if in the future
	// printf("qlim_ = %d, qib_ = %d, mean_pktsize_ = %d\n", qlim_, qib_, mean_pktsize_);
	double now = Scheduler::instance().clock();
	hdr_homa *homa_hdr = hdr_homa::access(p);
	if (p)
		MsgTracer::timestamp_pkt(p, MSG_TRACER_ENQUE, hdr_ip::access(p)->ttl() == 32 ? MSG_TRACER_HOST_NIC : MSG_TRACER_TOR, MsgTracerLogs()); // TODO: MSG_TRACER_TOR is a hack as it makes assumptions about the distribution of queue types

	if (do_print_)
	{
		std::cout << Scheduler::instance().clock() << " enque " << homa_hdr->appLevelId_var() << " sz " << hdr_cmn::access(p)->size() << " q sz " << q_->byteLength() << " last " << homa_hdr->schedDataFields_var().lastByte << " ttl " << hdr_ip::access(p)->ttl() << std::endl;
	}
	// if (hdr_cmn::access(p)->size() < MIN_ETHERNET_FRAME_ON_WIRE)
	// {
	// 	std::cout << "Droptail: A packet smaller than 84 bytes has been enqueued. If this happened before 10.0 seconds, it's the routing protocol." << std::endl;
	// 	std::cout << now << "  size: " << hdr_cmn::access(p)->size() << std::endl;
	// }
	if ((now >= 10.0) && p)
	{
		// std::cout << "hdr_cmn::access(p)->size() " << hdr_cmn::access(p)->size() << std::endl;
		assert(now <= 10.0 || hdr_cmn::access(p)->size() >= MIN_ETHERNET_FRAME_ON_WIRE);
		assert(now <= 10.0 || hdr_cmn::access(p)->size() <= MAX_ETHERNET_FRAME_ON_WIRE);
		hdr_cmn::access(p)->size() -= (INTER_PKT_GAP_SIZE + ETHERNET_PREAMBLE_SIZE); // the inter pkt gap size is not queued
		assert(now <= 10.0 || hdr_cmn::access(p)->size() >= MIN_ETHERNET_FRAME);
		assert(now <= 10.0 || hdr_cmn::access(p)->size() <= MAX_ETHERNET_FRAME);
		// assert(qlim_ > 0);
	}

	if (summarystats)
	{
		Queue::updateStats(qib_ ? q_->byteLength() : q_->length());
	}
	int qlimBytes = qlim_ * mean_pktsize_;

	bool do_drop = false;
	if (switch_shared_buffer_ == 0)
	{
		do_drop = (!qib_ && (q_->length() + 1) >= qlim_) ||
				  (qib_ && (q_->byteLength() + hdr_cmn::access(p)->size()) >= qlimBytes);
	}
	else if (switch_shared_buffer_ == 1)
	{
		if (p == nullptr)
		{
			throw std::runtime_error("p is nullptr");
		}
		if (node_ == nullptr)
		{
			throw std::runtime_error("node_ is nullptr");
		}
		do_drop = node_->should_drop(hdr_cmn::access(p)->size(), mean_pktsize_, qib_);
	}
	else
	{
		throw std::invalid_argument("Invalid argument for switch_shared_buffer_: " + std::to_string(switch_shared_buffer_));
	}

	if (do_drop)
	{
		// if the queue would overflow if we added this packet...
		if (drop_front_)
		{			   /* remove from head of queue */
			assert(0); // Not supported with shared buffer
			q_->enque(p);
			Packet *pp = q_->deque();
			drop(pp);
			std::cout << Scheduler::instance().clock() << "  3: Dropped packet (d) when q length was: " << q_->length() << "/" << qlim_ << " In bytes: " << q_->byteLength() << "/" << qlimBytes << std::endl;
		}
		else if (drop_prio_)
		{
			assert(0); // Not supported with shared buffer
			Packet *max_pp = p;
			int max_prio = 0;

			q_->enque(p);
			q_->resetIterator();
			for (Packet *pp = q_->getNext(); pp != 0; pp = q_->getNext())
			{
				if (!qib_ || (q_->byteLength() - hdr_cmn::access(pp)->size() < qlimBytes))
				{
					hdr_ip *h = hdr_ip::access(pp);
					int prio = h->prio();
					if (prio >= max_prio)
					{
						max_pp = pp;
						max_prio = prio;
					}
				}
			}
			q_->remove(max_pp);
			drop(max_pp);
			std::cout << Scheduler::instance().clock() << "  2: Dropped packet (d) when q length was: " << q_->length() << "/" << qlim_ << " In bytes: " << q_->byteLength() << "/" << qlimBytes << std::endl;
		}
		else if (drop_smart_)
		{
			assert(0); // Not supported with shared buffer
			Packet *max_pp = p;
			int max_count = 0;

			q_->enque(p);
			q_->resetIterator();
			for (Packet *pp = q_->getNext(); pp != 0; pp = q_->getNext())
			{
				hdr_ip *h = hdr_ip::access(pp);
				FlowKey fkey;
				fkey.src = h->saddr();
				fkey.dst = h->daddr();
				fkey.fid = h->flowid();

				char *fkey_buf = (char *)&fkey;
				int length = sizeof(fkey);
				string fkey_string(fkey_buf, length);

				std::tr1::hash<string> string_hasher;
				size_t signature = string_hasher(fkey_string);

				if (sq_counts_.find(signature) != sq_counts_.end())
				{
					int count = sq_counts_[signature];
					if (count > max_count)
					{
						max_count = count;
						max_pp = pp;
					}
				}
			}
			q_->remove(max_pp);
			/*hdr_ip* h = hdr_ip::access(p);
				FlowKey fkey;
				fkey.src = h->saddr();
				fkey.dst = h->daddr();
				fkey.fid = h->flowid();

				char* fkey_buf = (char*) &fkey;
				int length = sizeof(fkey);
				string fkey_string(fkey_buf, length);

				std::tr1::hash<string> string_hasher;
				size_t p_sig = string_hasher(fkey_string);
				h = hdr_ip::access(max_pp);
				fkey.src = h->saddr();
				fkey.dst = h->daddr();
				fkey.fid = h->flowid();

				string fkey_string2(fkey_buf, length);
				size_t maxpp_sig = string_hasher(fkey_string2);

			 printf("%s, enqueued %d, dropped %d instead\n", this->name(), p_sig, maxpp_sig);*/
			drop(max_pp);
			std::cout << Scheduler::instance().clock() << "  1: Dropped packet (d) when q length was: " << q_->length() << "/" << qlim_ << " In bytes: " << q_->byteLength() << "/" << qlimBytes << std::endl;
		}
		else
		{
			drop(p);
			std::cout << Scheduler::instance().clock() << "  0: Dropped packet (d) when q length was: " << q_->length() << "/" << qlim_ << " In bytes: " << q_->byteLength() << "/" << qlimBytes << std::endl;
		}
	}
	else
	{
		q_->enque(p);
	}
}

// AG if queue size changes, we drop excessive packets...
void DropTail::shrink_queue()
{
	int qlimBytes = qlim_ * mean_pktsize_;
	if (debug_)
		printf("shrink-queue: time %5.2f qlen %d, qlim %d\n",
			   Scheduler::instance().clock(),
			   q_->length(), qlim_);
	while ((!qib_ && q_->length() > qlim_) ||
		   (qib_ && q_->byteLength() > qlimBytes))
	{
		if (drop_front_)
		{ /* remove from head of queue */
			Packet *pp = q_->deque();
			drop(pp);
		}
		else
		{
			Packet *pp = q_->tail();
			q_->remove(pp);
			drop(pp);
		}
	}
}

// prio ranges from 0 to 7 (0 being highest)
// for w4 at 100Gbps, 7 prios go to scheduled and 1 to unscheduled.
// This merges the 7 scheduled priority levels into one to simulate 2 priority levels being available,
// one for scheudled and one for unscheduled but with the overcommitment level remaining high to allow
// high goodput
static int prio_mapper(int prio, int num_prios)
{
	if (num_prios == 2)
	{
		if (prio == 0)
		{
			return prio;
		}
		else
		{
			return 1;
		}
	}
	else if (num_prios == 1)
	{
		return 0;
	}
}

Packet *DropTail::deque()
{
	// printf("qlim_ = %d, qib_ = %d, mean_pktsize_ = %d\n", qlim_, qib_, mean_pktsize_);
	// std::cout << "deque()" << std::endl;
	// assert(qlim_ > 0);
	if (summarystats && &Scheduler::instance() != NULL)
	{
		Queue::updateStats(qib_ ? q_->byteLength() : q_->length());
	}

	/*Shuang: deque the packet with the highest priority */
	// i.e., the smallest prio()
	if (deque_prio_)
	{
		q_->resetIterator();
		Packet *p = q_->getNext();
		int highest_prio_;
		if (p != 0)
			highest_prio_ = hdr_ip::access(p)->prio();
		else
			return 0;
		int cnt = 0;
		int highest_prio_pkt = 0;
		for (Packet *pp = q_->getNext(); pp != 0; pp = q_->getNext())
		{
			hdr_ip *h = hdr_ip::access(pp);
			int prio = h->prio();
			if (num_prios_ == 2 || num_prios_ == 1)
			{
				prio = prio_mapper(prio, num_prios_);
			}
			if (prio < highest_prio_)
			{
				p = pp;
				highest_prio_ = prio;
				highest_prio_pkt = cnt;
			}
			cnt++;
		}
		if (do_print_)
		{
			std::cout << Scheduler::instance().clock() << " droptail prio deque " << hdr_homa::access(p)->appLevelId_var() << " sz " << hdr_cmn::access(p)->size() << " q sz " << q_->byteLength()
					  << " last " << hdr_homa::access(p)->schedDataFields_var().lastByte << " prio: " << hdr_ip::access(p)->prio() << " ttl " << hdr_ip::access(p)->ttl() << std::endl;
		}

		if (keep_order_)
		{
			q_->resetIterator();
			hdr_ip *hp = hdr_ip::access(p);
			for (Packet *pp = q_->getNext(); pp != p; pp = q_->getNext())
			{
				hdr_ip *h = hdr_ip::access(pp);
				if (h->saddr() == hp->saddr() && h->daddr() == hp->daddr() && h->flowid() == hp->flowid())
				{
					p = pp;
					break;
				}
			}
		}

		q_->remove(p);
		double now = Scheduler::instance().clock();
		if ((now >= 10.0) && p)
		{
			assert(hdr_cmn::access(p)->size() >= MIN_ETHERNET_FRAME);
			assert(now <= 10.0 || hdr_cmn::access(p)->size() <= MAX_ETHERNET_FRAME);
			hdr_cmn::access(p)->size() += (INTER_PKT_GAP_SIZE + ETHERNET_PREAMBLE_SIZE); // the inte pkt gap size is not queued
			assert(now <= 10.0 || hdr_cmn::access(p)->size() >= MIN_ETHERNET_FRAME_ON_WIRE);
			assert(now <= 10.0 || hdr_cmn::access(p)->size() <= MAX_ETHERNET_FRAME_ON_WIRE);
		}
		if (p)
		{
			do_r2p2_ops(p);
		}
		return p;
	}
	else if (drop_smart_)
	{
		Packet *p = q_->deque();
		if (p)
		{
			hdr_ip *h = hdr_ip::access(p);
			FlowKey fkey;
			fkey.src = h->saddr();
			fkey.dst = h->daddr();
			fkey.fid = h->flowid();

			char *fkey_buf = (char *)&fkey;
			int length = sizeof(fkey);
			string fkey_string(fkey_buf, length);

			std::tr1::hash<string> string_hasher;
			size_t signature = string_hasher(fkey_string);
			sq_queue_.push(signature);

			if (sq_counts_.find(signature) != sq_counts_.end())
			{
				sq_counts_[signature]++;
				// printf("%s packet with signature %d, count = %d, qsize = %d\n", this->name(), signature, sq_counts_[signature], sq_queue_.size());
			}
			else
			{
				sq_counts_[signature] = 1;
				// printf("%s first packet with signature %d, count = %d, qsize = %d\n", this->name(), signature, sq_counts_[signature], sq_queue_.size());
			}

			if (sq_queue_.size() > sq_limit_)
			{
				// printf("%s we are full %d %d\n", this->name(), sq_counts_.size(), sq_queue_.size());
				size_t temp = sq_queue_.front();
				sq_queue_.pop();
				sq_counts_[temp]--;
				if (sq_counts_[temp] == 0)
					sq_counts_.erase(temp);

				// printf("%s removed front sig = %d, no longer full %d %d\n", this->name(), temp, sq_counts_.size(), sq_queue_.size());
			}
		}
		// Temporary check - remove if in the future
		double now = Scheduler::instance().clock();
		if ((now >= 10.0) && p)
		{
			assert(now <= 10.0 || hdr_cmn::access(p)->size() >= MIN_ETHERNET_FRAME);
			assert(now <= 10.0 || hdr_cmn::access(p)->size() <= MAX_ETHERNET_FRAME);
			hdr_cmn::access(p)->size() += (INTER_PKT_GAP_SIZE + ETHERNET_PREAMBLE_SIZE); // the inte pkt gap size is not queued
			assert(now <= 10.0 || hdr_cmn::access(p)->size() >= MIN_ETHERNET_FRAME_ON_WIRE);
			assert(now <= 10.0 || hdr_cmn::access(p)->size() <= MAX_ETHERNET_FRAME_ON_WIRE);
		}
		if (p)
			MsgTracer::timestamp_pkt(p, MSG_TRACER_DEQUE, hdr_ip::access(p)->ttl() == 32 ? MSG_TRACER_HOST_NIC : MSG_TRACER_TOR, MsgTracerLogs()); // TODO: MSG_TRACER_TOR is a hack as it makes assumptions about the distribution of queue types
		return p;
	}
	else
	{
		Packet *p = q_->deque();
		double now = Scheduler::instance().clock();
		if ((now >= 10.0) && p)
		{
			// assert(now <= 10.0 || hdr_cmn::access(p)->size() >= MIN_ETHERNET_FRAME);
			assert(now <= 10.0 || hdr_cmn::access(p)->size() <= MAX_ETHERNET_FRAME);
			hdr_cmn::access(p)->size() += (INTER_PKT_GAP_SIZE + ETHERNET_PREAMBLE_SIZE); // the inte pkt gap size is not queued
			// assert(now <= 10.0 || hdr_cmn::access(p)->size() >= MIN_ETHERNET_FRAME_ON_WIRE);
			assert(now <= 10.0 || hdr_cmn::access(p)->size() <= MAX_ETHERNET_FRAME_ON_WIRE);
		}
		if (p)
		{
			do_r2p2_ops(p);
		}
		if (do_print_)
		{
			std::cout << Scheduler::instance().clock() << " droptail deque " << hdr_homa::access(p)->appLevelId_var() << " sz " << hdr_cmn::access(p)->size() << " q sz " << q_->byteLength()
					  << " last " << hdr_homa::access(p)->schedDataFields_var().lastByte << " prio: " << hdr_ip::access(p)->prio() << " ttl " << hdr_ip::access(p)->ttl() << std::endl;
		}
		return p;
	}
}

void DropTail::attach_node(Node *node)
{
	if (node == nullptr)
	{
		throw std::runtime_error("attach_node() node is nullptr");
	}
	// std::cout << "Attaching node " << node << " addr: " << node->address() << " to DropTail queue " << this << std::endl;
	node_ = node;
}

void DropTail::print_summarystats()
{
	// double now = Scheduler::instance().clock();
	printf("True average queue: %5.3f", true_ave_);
	if (qib_)
		printf(" (in bytes)");
	printf(" time: %5.3f\n", total_time_);
}

void DropTail::do_r2p2_ops(Packet *p)
{
	MsgTracer::timestamp_pkt(p, MSG_TRACER_DEQUE, hdr_ip::access(p)->ttl() == 32 ? MSG_TRACER_HOST_NIC : MSG_TRACER_TOR, MsgTracerLogs()); // TODO: MSG_TRACER_TOR is a hack as it makes assumptions about the distribution of queue types
	double now = Scheduler::instance().clock();

	// FOR MULTI BIT ALGO

	if (hdr_r2p2::access(p)->ts() == 0.0 && now >= 10.0) // only want this processing to happen once (at the sender uplink)
	{
		// calculate TxRate in bytes / sec
		double tx_rate = (bytes_sent_ + hdr_cmn::access(p)->size() - bytes_sent_);
		double tx_time = hdr_cmn::access(p)->size() * 8.0 / (bw_gbps_ * 1000.0 * 1000.0 * 1000.0);
		double this_packet_done_in = now + tx_time;
		double dt = (this_packet_done_in - prev_deq_);
		prev_deq_ = this_packet_done_in;
		bytes_sent_ += hdr_cmn::access(p)->size();

		tx_rate = tx_rate / dt;
		// std::cout << "TMP RTAS: tx_rate (gbps): " << tx_rate * 8.0 / 1000000000.0 << " dt (ns): " << dt * 1000.0 * 1000.0 * 1000.0 << " hdr_cmn::access(p)->size(): "
		// 		  << hdr_cmn::access(p)->size() << " tx_time (ns): " << tx_time * 1000.0 * 1000.0 * 1000.0 << " bw_gbps_:" << bw_gbps_ << std::endl;
		assert(tx_rate > 0);
		// std::cout << "tx_rate * 8.0 / 1000000000.0 " << tx_rate * 8.0 / 1000000000.0 << std::endl;
		// std::cout << "1.1 * bw_gbps_ " << 1.1 * bw_gbps_ << std::endl;
		// std::cout << "dt " << dt << std::endl;
		// std::cout << "this_packet_done_in " << this_packet_done_in << std::endl;
		// std::cout << "prev_deq_ " << prev_deq_ << std::endl;
		// std::cout << "now " << now << std::endl;
		assert(tx_rate * 8.0 / 1000000000.0 <= 1.1 * bw_gbps_); // this will fail if link speed is set above 100Gbps. Adapt it.
		hdr_r2p2::access(p)->tx_rate_Bps() = tx_rate;
		hdr_r2p2::access(p)->dt() = dt;

		// set the dispatch timestamp if it is not set (i.e., the sender's nic sets it)
		hdr_r2p2::access(p)->ts() = Scheduler::instance().clock();
		assert(hdr_r2p2::access(p)->tx_bytes() == 0);
		hdr_r2p2::access(p)->tx_bytes() = bytes_sent_;
	}
}
