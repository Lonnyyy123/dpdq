#include "r2p2-udp.h"
#include "r2p2.h"
#include "rtp.h"
#include "random.h"
#include "simple-log.h"
#include "flags.h"
#include "r2p2-cc-micro.h"
#include "msg-tracer.h"
#include <array>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <unordered_set>


// OTcl linkange class
static class R2p2AgentClass : public TclClass
{
public:
	R2p2AgentClass() : TclClass("Agent/UDP/R2P2") {}
	TclObject *create(int, const char *const *)
	{
		return (new R2p2Agent());
	}
} class_r2p2_agent;

std::string R2p2Agent::packet_stats_file_;
std::unordered_set<std::string> R2p2Agent::foreground_messages_;
std::array<std::array<unsigned long long, 2>, 2> R2p2Agent::packet_counts_{{{0, 0}, {0, 0}}};
bool R2p2Agent::packet_stats_written_ = false;

namespace {
const char *kWorkloadNames[2] = {"background", "foreground"};
const char *kPacketKindNames[2] = {"data", "credit"};
}

R2p2Agent::R2p2Agent() : UdpAgent()
{
}

R2p2Agent::R2p2Agent(packet_t type) : UdpAgent(type)
{
}

int R2p2Agent::command(int argc, const char *const *argv)
{
	if (argc == 2)
	{
		if (strcmp(argv[1], "write-packet-stats") == 0)
		{
			write_packet_stats();
			return TCL_OK;
		}
	}
	if (argc == 3)
	{
		if (strcmp(argv[1], "set-send-log-file") == 0)
		{
			send_log_file_ = argv[2];
			return TCL_OK;
		}
		if (strcmp(argv[1], "set-packet-stats-file") == 0)
		{
			packet_stats_file_ = argv[2];
			packet_stats_written_ = false;
			return TCL_OK;
		}
	}
	return UdpAgent::command(argc, argv);
}

bool R2p2Agent::is_credit_type(int msg_type)
{
	return msg_type == hdr_r2p2::GRANT;
}

std::string R2p2Agent::make_message_key(int32_t cl_addr, long app_level_id)
{
	return std::to_string(cl_addr) + ":" + std::to_string(app_level_id);
}

bool R2p2Agent::is_foreground_message(hdr_r2p2 *r2p2_hdr)
{
	return foreground_messages_.find(make_message_key(r2p2_hdr->cl_addr(), r2p2_hdr->app_level_id())) != foreground_messages_.end();
}

void R2p2Agent::register_foreground_message(int32_t cl_addr, long app_level_id)
{
	foreground_messages_.insert(make_message_key(cl_addr, app_level_id));
}

void R2p2Agent::write_packet_stats()
{
	if (packet_stats_written_ || packet_stats_file_.empty())
	{
		return;
	}
	std::ofstream out(packet_stats_file_.c_str(), std::ios::out | std::ios::trunc);
	if (!out.is_open())
	{
		return;
	}
	out << "workload_class,packet_kind,packets_sent\n";
	for (int workload = 0; workload < 2; ++workload)
	{
		for (int kind = 0; kind < 2; ++kind)
		{
			out << kWorkloadNames[workload] << ","
			    << kPacketKindNames[kind] << ","
			    << packet_counts_[workload][kind] << "\n";
		}
	}
	packet_stats_written_ = true;
}

void R2p2Agent::log_send_event(Packet *p) const
{
	if (p == nullptr)
		return;

	hdr_cmn *cmn = hdr_cmn::access(p);
	if (cmn->ptype() != PT_UDP)
		return;
	hdr_r2p2 *r2p2 = hdr_r2p2::access(p);

	const bool is_credit = is_credit_type(r2p2->msg_type());
	const int packet_kind = is_credit ? 1 : 0;
	const int workload_class = is_foreground_message(r2p2) ? 1 : 0;
	packet_counts_[workload_class][packet_kind]++;

	if (send_log_file_.empty())
		return;

	const char *kind = is_credit ? "credit" : "data";
	const char *flow_class = workload_class == 1 ? "foreground" : "background";

	std::ofstream out(send_log_file_.c_str(), std::ios::out | std::ios::app);
	if (!out.is_open())
		return;

	std::ostringstream oss;
	oss << std::fixed << std::setprecision(10)
		<< Scheduler::instance().clock()
		<< "," << r2p2_layer_->this_addr_
		<< "," << flow_class
		<< "," << kind
		<< "," << r2p2->msg_type()
		<< "," << cmn->ptype()
		<< "," << cmn->size()
		<< "," << r2p2->cl_addr()
		<< "," << hdr_ip::access(p)->src().addr_
		<< "," << hdr_ip::access(p)->dst().addr_
		<< "," << r2p2->app_level_id();
	out << oss.str() << std::endl;
}

void R2p2Agent::sendmsg(int nbytes, hdr_r2p2 &r2p2_hdr, MsgTracerLogs &&logs, int fid, int prio, int current_ttl, int32_t source_addr, int ecn_capable, const char *flags)
{
	assert(r2p2_hdr.msg_creation_time() > 0.0);
	Packet *p;
	int n;
	n = nbytes / MAX_R2P2_PAYLOAD;
	if (nbytes == -1)
	{
		printf("Error:  sendmsg() for UDP should not be -1\n");
		return;
	}
	if (prio == NO_PRIO)
	{
		prio = DEFAULT_PRIO;
	}
	int granted_bytes = 0;
	bool should_mark = false;
	uint32_t ecn_thresh_bytes = 1000000000;

	R2p2CCHybrid *cc_layer = dynamic_cast<R2p2CCHybrid *>(r2p2_layer_);
	if (cc_layer != nullptr)
	{
		if (r2p2_hdr.msg_type() == hdr_r2p2::REQUEST || r2p2_hdr.msg_type() == hdr_r2p2::REPLY)
		{
			ecn_thresh_bytes = cc_layer->ecn_thresh_pkts_ * MAX_R2P2_PAYLOAD; // comparisons with ecn_thresh_bytes ar with header-free byte counts
			granted_bytes = cc_layer->get_granted_bytes_queue_len();		  // get_granted_bytes_queue_len is updated periodically
			should_mark = true;
		}
	}
	else
	{
		throw std::runtime_error("cc_layer is not of type R2p2CCHybrid"); // handle this - this should not happen if cc_layer is Hybrid, but it is ok for other protocols.
	}

	packet_id pkt_id = r2p2_hdr.pkt_id();

	double local_time = Scheduler::instance().clock();
	while (n-- > 0)
	{
		p = allocpkt();

		r2p2_hdr.pkt_id() = pkt_id;
		// checking if this packet's last field should be set. If this was a single
		// packet RPC, then both F and L will be already set by the r2p2Client
		// if (n == 1 \
		// 	&& r2p2_hdr.msg_type() == hdr_r2p2::REQUEST \
		// 	&& nbytes % size_ == 0) {
		// 		r2p2_hdr.last() = true;
		// 	}
		// shallow copy should be ok since no pointers - FIX
		(*hdr_r2p2::access(p)) = r2p2_hdr;
		if (ecn_capable == 1)
		{
			hdr_flags::access(p)->ect() = 1;
			if (should_mark)
			{
				bool tmp_decision = false;
				if ((uint32_t)granted_bytes > ecn_thresh_bytes)
				{
					tmp_decision = true;
				}
				else
				{
					tmp_decision = false;
				}
				hdr_r2p2::access(p)->sender_marked() = cc_layer->actually_mark(r2p2_hdr, tmp_decision);
				granted_bytes -= MAX_R2P2_PAYLOAD;
				assert(granted_bytes >= -(2 * MAX_R2P2_PAYLOAD));
			}
		}
		else
		{
			hdr_flags::access(p)->ect() = 0;
		}
		// set the flow id for multipath
		if (fid == FID_TO_REQID)
		{
			hdr_ip::access(p)->flowid() = (int)r2p2_hdr.req_id();
		}
		else
		{
			hdr_ip::access(p)->flowid() = fid;
		}
		hdr_ip::access(p)->prio() = prio;
		if (current_ttl != NO_CURRENT_TTL)
		{
			hdr_ip::access(p)->ttl() = current_ttl;
		}
		if (source_addr != NO_SRC_REPLACEMENT)
		{
			// This is done so that requests forwarded by routers can appear to originate from client
			// TODO: NOTE: this is not spoofing the source PORT, only the address.
			hdr_ip::access(p)->src().addr_ = source_addr;
		}

		hdr_cmn::access(p)->size() = MAX_ETHERNET_FRAME_ON_WIRE;
		hdr_rtp *rh = hdr_rtp::access(p);
		rh->flags() = 0;
		rh->seqno() = ++seqno_;
		hdr_cmn::access(p)->timestamp() =
			(u_int32_t)(SAMPLERATE * local_time);
		// add "beginning of talkspurt" labels (tcl/ex/test-rcvr.tcl)
		if (flags && (0 == strcmp(flags, "NEW_BURST")))
			rh->flags() |= RTP_M;
		cc_layer->total_bytes_sent_ += hdr_cmn::access(p)->size();
		hdr_r2p2::access(p)->qlen() = cc_layer->get_outbound_vq_len(hdr_ip::access(p)->dst().addr_); // TODO: do we want this or the raw value?
		hdr_r2p2::access(p)->B() = cc_layer->link_speed_gbps_;
		slog::log6(debug_, r2p2_layer_->this_addr_, "R2p2Agent::sendmsg(). Sending packet to addr:",
				   hdr_ip::access(p)->dst().addr_, "total_bytes_sent:", cc_layer->total_bytes_sent_,
				   "TTL:", hdr_ip::access(p)->ttl(), "prio:", prio);

		MsgTracer::timestamp_pkt(p, MSG_TRACER_SEND, MSG_TRACER_HOST, std::move(logs));
		log_send_event(p);
		target_->recv(p);

		pkt_id++;
		// only the first packet should have the FIRST FLAG set no matter what
		r2p2_hdr.first() = false;
		r2p2_hdr.first_urpc() = false; // same applies to uRPCs
		r2p2_hdr.credit_req() = 0;
	}
	n = nbytes % MAX_R2P2_PAYLOAD;

	if (n > 0)
	{
		p = allocpkt();

		r2p2_hdr.pkt_id() = pkt_id;
		// TODO:
		// NOT CORRECT FOR single pkt RPCs -> not using last() for now
		// if (r2p2_hdr.msg_type() == hdr_r2p2::REQUEST) {
		// 	r2p2_hdr.last() = true;
		// }
		(*hdr_r2p2::access(p)) = r2p2_hdr;
		if (ecn_capable == 1)
		{
			hdr_flags::access(p)->ect() = 1;
			if (should_mark)
			{
				bool tmp_decision = false;
				if ((uint32_t)granted_bytes > ecn_thresh_bytes)
				{
					tmp_decision = true;
				}
				else
				{
					tmp_decision = false;
				}
				hdr_r2p2::access(p)->sender_marked() = cc_layer->actually_mark(r2p2_hdr, tmp_decision);
				granted_bytes -= MAX_R2P2_PAYLOAD;
				assert(granted_bytes >= -(MAX_R2P2_PAYLOAD));
			}
		}
		else
		{
			hdr_flags::access(p)->ect() = 0;
		}

		if (fid == FID_TO_REQID)
		{
			hdr_ip::access(p)->flowid() = (int)r2p2_hdr.req_id();
		}
		else
		{
			hdr_ip::access(p)->flowid() = fid;
		}
		hdr_ip::access(p)->prio() = prio;
		if (current_ttl != NO_CURRENT_TTL)
		{
			hdr_ip::access(p)->ttl() = current_ttl;
		}
		if (source_addr != NO_SRC_REPLACEMENT)
		{
			// This is done so that requests forwarded by routers can appear to originate from client
			// TODO: NOTE: this is not spoofing the source PORT, only the address.
			hdr_ip::access(p)->src().addr_ = source_addr;
		}

		hdr_cmn::access(p)->size() = n + R2P2_ALL_HEADERS_SIZE + INTER_PKT_GAP_SIZE + ETHERNET_PREAMBLE_SIZE;
		if (hdr_cmn::access(p)->size() < MIN_ETHERNET_FRAME_ON_WIRE)
		{
			hdr_cmn::access(p)->padding() = MIN_ETHERNET_FRAME_ON_WIRE - hdr_cmn::access(p)->size();
			hdr_cmn::access(p)->size() = MIN_ETHERNET_FRAME_ON_WIRE;
		}
		assert(hdr_cmn::access(p)->size() >= MIN_ETHERNET_FRAME_ON_WIRE);
		assert(hdr_cmn::access(p)->size() <= MAX_ETHERNET_FRAME_ON_WIRE);
		hdr_rtp *rh = hdr_rtp::access(p);
		rh->flags() = 0;
		rh->seqno() = ++seqno_;
		hdr_cmn::access(p)->timestamp() =
			(u_int32_t)(SAMPLERATE * local_time);
		// add "beginning of talkspurt" labels (tcl/ex/test-rcvr.tcl)
		if (flags && (0 == strcmp(flags, "NEW_BURST")))
			rh->flags() |= RTP_M;
		cc_layer->total_bytes_sent_ += hdr_cmn::access(p)->size();
		hdr_r2p2::access(p)->qlen() = cc_layer->get_outbound_vq_len(hdr_ip::access(p)->dst().addr_); // TODO: do we want this or the raw value?
		hdr_r2p2::access(p)->B() = cc_layer->link_speed_gbps_;

		slog::log6(debug_, r2p2_layer_->this_addr_, "R2p2Agent::sendmsg().",
				   "total_bytes_sent:", cc_layer->total_bytes_sent_,
				   "Sending last packet to addr:",
				   hdr_ip::access(p)->dst().addr_, "TTL:",
				   hdr_ip::access(p)->ttl(), "SIZE:", hdr_cmn::access(p)->size(),
				   "prio:", prio);
		MsgTracer::timestamp_pkt(p, MSG_TRACER_SEND, MSG_TRACER_HOST, std::move(logs));
		log_send_event(p);
		target_->recv(p);
	}

	idle();
}

void R2p2Agent::recv(Packet *pkt, Handler *h)
{
	int size = hdr_cmn::access(pkt)->size();
	int padding = hdr_cmn::access(pkt)->padding();
	assert(size <= MAX_ETHERNET_FRAME_ON_WIRE);
	assert(size >= MIN_ETHERNET_FRAME_ON_WIRE);
	hdr_cmn::access(pkt)->size() = size - INTER_PKT_GAP_SIZE - ETHERNET_PREAMBLE_SIZE - R2P2_ALL_HEADERS_SIZE;
	assert(hdr_cmn::access(pkt)->size() >= MIN_R2P2_PAYLOAD);
	assert(hdr_cmn::access(pkt)->size() <= MAX_R2P2_PAYLOAD);
	hdr_cmn::access(pkt)->size() -= padding;
	assert(padding < MIN_R2P2_PAYLOAD);
	slog::log6(debug_, r2p2_layer_->this_addr_, "R2p2Agent::recv(). Received packet from addr:",
			   hdr_ip::access(pkt)->src().addr_, "TTL:", hdr_ip::access(pkt)->ttl(),
			   //    "tx_bytes:", hdr_r2p2::access(pkt)->tx_bytes(),
			   //    "ts:", hdr_r2p2::access(pkt)->ts(),
			   "B:", hdr_r2p2::access(pkt)->B(),
			   "qlen:", hdr_r2p2::access(pkt)->qlen());
	if (r2p2_layer_)
	{
		r2p2_layer_->recv(pkt, h);
	}
}

void R2p2Agent::attach_r2p2_transport(R2p2Transport *r2p2_transport)
{
	r2p2_layer_ = r2p2_transport;
	slog::log6(debug_, r2p2_layer_->this_addr_, "R2p2Agent::attach_r2p2_transport()");
}