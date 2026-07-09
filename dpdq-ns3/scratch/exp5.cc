/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License version 2 as
* published by the Free Software Foundation;
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#undef PGO_TRAINING
#define PATH_TO_PGO_CONFIG "path_to_pgo_config"

#include <iostream>
#include <fstream>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <cctype>
#include <functional>
#include <random>
#include <sstream>
#include <time.h> 
#include "ns3/core-module.h"
#include "ns3/qbb-helper.h"
#include "ns3/point-to-point-helper.h"
#include "ns3/applications-module.h"
#include "ns3/internet-module.h"
#include "ns3/global-route-manager.h"
#include "ns3/ipv4-static-routing-helper.h"
#include "ns3/broadcom-node.h"
#include "ns3/packet.h"
#include "ns3/error-model.h"
#include <ns3/rdma.h>
#include <ns3/rdma-client.h>
#include <ns3/rdma-client-helper.h>
#include <ns3/rdma-driver.h>
#include <ns3/switch-node.h>
#include <ns3/sim-setting.h>
#include <ns3/assert.h>
#include <ns3/node-list.h>

using namespace ns3;
using namespace std;

NS_LOG_COMPONENT_DEFINE("GENERIC_SIMULATION");

uint32_t cc_mode = 1;
bool enable_qcn = true, enable_pfc = true, use_dynamic_pfc_threshold = true;
uint32_t packet_payload_size = 1466, l2_chunk_size = 0, l2_ack_interval = 0;
double pause_time = 671, simulator_stop_time = 3.01;  // pause_time = 65535*(64Bytes/50Gbps)
std::string data_rate, link_delay, topology_file, flow_file, trace_file, trace_output_file;
std::string fct_output_file = "fct.txt";
std::string pfc_output_file = "pfc.txt";
std::string timeout_output_file = "timeout.txt";
std::string bounced_output_file = "bounced.txt";
std::string pause_intervals_output_file = "";
std::string xoff_intervals_output_file = "";
std::string packet_loss_output_file = "/dev/null";
uint32_t timeout_mon_interval_ns = 100000; // 100us
uint32_t pfc_mon_interval_ns = 8000;       // 8us
uint32_t bounced_mon_interval_ns = 8000;   // 8us
// 0: output legacy ideal FCT only
// 1: output both legacy and new ideal FCTs
// Non-CB:
//   legacy = pairRtt + size/bw
//   new    = 2*pairDelay + size/bw
// CreditBouncer:
//   legacy = pairDelay + pairTxDelay + size/bw
//   new    = pairDelay + size/bw
uint32_t standalone_fct_mode = 0;
uint32_t l2_timeout_ns = 4000000;

double alpha_resume_interval = 55, rp_timer, ewma_gain = 1 / 16;
uint64_t rp_byte_reset = 10000000;
double rate_decrease_interval = 4;
uint32_t fast_recovery_times = 5;
std::string rate_ai, rate_hai, min_rate = "100Mb/s";
std::string dctcp_rate_ai = "1000Mb/s";

bool clamp_target_rate = false, l2_back_to_zero = false;
double error_rate_per_link = 0.0;
uint32_t has_win = 1;
uint32_t global_t = 1;
uint32_t mi_thresh = 5;
bool var_win = false, fast_react = true;
bool multi_rate = true;
bool sample_feedback = false;
double u_target = 0.95;
uint32_t int_multi = 1;
bool rate_bound = true;

uint32_t ack_high_prio = 0;
uint64_t link_down_time = 0;
uint32_t link_down_A = 0, link_down_B = 0;

uint32_t enable_trace = 1;

uint32_t buffer_size = 0; // 0 to set buffer size automatically
uint32_t switch_shared_buffer_per_port_bytes = 200 * 1000;

// Added from Here
string hpcc_workload;
double load = 0.1;
// Background flow generation mode:
// "count": generate exactly NUM_BG_FLOWS flows (legacy behavior).
// "load":  generate flows continuously based on LOAD until APP_STOP_TIME.
string bg_flow_gen_mode = "count";
double foreground_flow_ratio = 0.0, app_start_time = 0.01, app_stop_time = 100;
bool enable_foreground_incast = false;
double foreground_incast_start_time = 0.05;
uint32_t foreground_incast_rack_count = 7;
uint32_t foreground_incast_senders_per_rack = 5;
int32_t foreground_receiver_host_id = -1;
std::vector<uint32_t> foreground_source_leaf_ids;
bool foreground_use_first_hosts_per_leaf = false;
uint64_t foreground_incast_leaf_jitter_ns = 2000;
uint64_t foreground_incast_host_jitter_step_ns = 100;
bool foreground_incast_shuffle_leaf_order_per_round = true;
uint32_t incast_flow_size = 64000; // previously  dctcp_tx_bytes
uint32_t FOREGROUND_INCAST_FLOW_PER_HOST = 4;
int num_bg_flows= 0;
double last_background_flow_time = 0;
int enable_irn = 0, enable_tlt = 0;
double tlt_maxbytes_uip = 0;
bool irn_no_bdpfc = false;
uint32_t irn_bdp_bytes = 0; // 0 means use built-in branch defaults
int random_seed = 1;
bool enable_creditbouncer = false;
bool creditbouncer_cc_enable = true;
bool creditbouncer_csn_enable = false;
bool creditbouncer_fc_enable = true;
std::string creditbouncer_grant_sched_policy = "RR";
uint32_t creditbouncer_unsch_threshold_bytes = 16384;
uint32_t creditbouncer_global_bucket_bytes = 131072;
uint32_t creditbouncer_maxSrpb_bytes = 131072;
uint32_t creditbouncer_sender_threshold_bytes = 65536;
uint32_t creditbouncer_grant_granularity_bytes = 1466;
double creditbouncer_ai_step_bytes = 1000.0;
double creditbouncer_md_factor = 0.5;
double creditbouncer_ce_new_weight = 0.1;
uint32_t creditbouncer_grant_interval_ns = 300;
uint32_t creditbouncer_resend_interval_ns = 4000000;
bool creditbouncer_symmetric_routing = true;
bool creditbouncer_debug_log = false;
uint32_t creditbouncer_debug_max_logs = 200;
bool creditbouncer_route_log = false;
uint32_t creditbouncer_route_max_logs = 200;
bool creditbouncer_switch_cb_enable = true;
uint64_t creditbouncer_switch_phantom_threshold_bytes = 131072;
uint64_t creditbouncer_switch_xon_bytes = 65536;
uint64_t creditbouncer_switch_xoff_bytes = 131072;
uint64_t creditbouncer_switch_xecn_bytes = 98304;
bool creditbouncer_switch_dynamic_threshold = true;
double creditbouncer_switch_dynamic_alpha = 0.0625;
uint64_t creditbouncer_switch_dynamic_off_diff_bytes = 16;
bool creditbouncer_switch_state_log = false;
uint32_t creditbouncer_switch_state_max_logs = 200;
bool creditbouncer_switch_xon_explicit = false;
bool creditbouncer_switch_xoff_explicit = false;
bool creditbouncer_switch_xecn_explicit = false;
bool creditbouncer_dedicated_mmu_enable = false;
static constexpr uint32_t kCreditBouncerControlPacketSizeBytes = 60;
double creditbouncer_dedicated_credit_queue_multiplier =
	(64.0 * 1024.0) / static_cast<double>(kCreditBouncerControlPacketSizeBytes);
uint32_t creditbouncer_dedicated_credit_queue_bytes =
	static_cast<uint32_t>(creditbouncer_dedicated_credit_queue_multiplier *
		kCreditBouncerControlPacketSizeBytes + 0.5);
uint32_t creditbouncer_dedicated_max_total_buffer_per_port_bytes = 375 * 1000;
uint32_t creditbouncer_dedicated_data_headroom_bytes = 16 * 1024;
uint32_t creditbouncer_dedicated_data_guarantee_bytes = 1048;
uint32_t creditbouncer_dedicated_data_ecn_kmin_bytes = 64 * 1024;
uint32_t creditbouncer_dedicated_data_ecn_kmax_bytes = 96 * 1024;
double creditbouncer_dedicated_data_ecn_pmax = 1.0;
bool creditbouncer_dedicated_mmu_log = false;
uint32_t creditbouncer_dedicated_mmu_max_logs = 4000;

#define MAP_KEY_EXISTS(map,key) (((map).find(key)!=(map).end())) 

unordered_map<uint64_t, uint32_t> rate2kmax, rate2kmin;
unordered_map<uint64_t, double> rate2pmax;


uint64_t nic_rate;

uint64_t maxRtt, maxBdp;

struct Interface{
	uint32_t idx;
	bool up;
	uint64_t delay;
	uint64_t bw;

	Interface() : idx(0), up(false){}
};
map<Ptr<Node>, map<Ptr<Node>, Interface> > nbr2if;
// Mapping destination to next hop for each node: <node, <dest, <nexthop0, ...> > >
map<Ptr<Node>, map<Ptr<Node>, vector<Ptr<Node> > > > nextHop;
map<Ptr<Node>, map<Ptr<Node>, uint64_t> > pairDelay;
map<Ptr<Node>, map<Ptr<Node>, uint64_t> > pairTxDelay;
map<Ptr<Node>, map<Ptr<Node>, uint64_t> > pairBw;
map<Ptr<Node>, map<Ptr<Node>, uint64_t> > pairBdp;
map<Ptr<Node>, map<Ptr<Node>, uint64_t> > pairRtt;
static uint64_t g_rdma_timeout_window_count = 0;
static uint64_t g_cb_resend_timeout_window_count = 0;
static uint64_t g_pfc_window_count = 0;
static uint64_t g_bounced_window_count = 0;
static std::unordered_set<int32_t> g_foreground_flow_ids;
struct FlowTupleKey {
	uint32_t firstIp;
	uint32_t secondIp;
	uint16_t firstPort;
	uint16_t secondPort;

	bool operator==(const FlowTupleKey &other) const {
		return firstIp == other.firstIp &&
			secondIp == other.secondIp &&
			firstPort == other.firstPort &&
			secondPort == other.secondPort;
	}
};

struct FlowTupleKeyHash {
	std::size_t operator()(const FlowTupleKey &key) const {
		std::size_t h = static_cast<std::size_t>(key.firstIp);
		h = h * 1315423911u + static_cast<std::size_t>(key.secondIp);
		h = h * 1315423911u + static_cast<std::size_t>(key.firstPort);
		h = h * 1315423911u + static_cast<std::size_t>(key.secondPort);
		return h;
	}
};

struct PacketLossCounters {
	uint64_t fgCreditLoss = 0;
	uint64_t fgCreditTotal = 0;
	uint64_t fgDataLoss = 0;
	uint64_t fgDataTotal = 0;
	uint64_t bgCreditLoss = 0;
	uint64_t bgCreditTotal = 0;
	uint64_t bgDataLoss = 0;
	uint64_t bgDataTotal = 0;
};

static std::unordered_set<FlowTupleKey, FlowTupleKeyHash> g_foreground_flow_tuples;
static PacketLossCounters g_packet_loss_counters;

static void invoke_std_function(std::function<void()>* fn){
	(*fn)();
}

static inline uint32_t ip_to_node_id(Ipv4Address ip){
	// Host IP layout in this scenario: 11.(id/256).(id%256).1
	uint32_t raw = ip.Get();
	uint32_t a = (raw >> 24) & 0xff;
	uint32_t b = (raw >> 16) & 0xff;
	uint32_t c = (raw >> 8) & 0xff;
	uint32_t d = raw & 0xff;
	NS_ABORT_MSG_IF(a != 0x0b || d != 0x01, "ip_to_node_id: unexpected host IP format " << ip);
	return (b << 8) | c;
}

static std::vector<uint32_t> parse_uint_csv(const std::string &csv) {
	std::vector<uint32_t> values;
	std::stringstream ss(csv);
	std::string token;
	while (std::getline(ss, token, ',')) {
		NS_ABORT_MSG_IF(token.empty(), "Invalid empty token in CSV list: " << csv);
		values.push_back(static_cast<uint32_t>(std::stoul(token)));
	}
	return values;
}

static std::string join_uint_vector(const std::vector<uint32_t> &values) {
	std::ostringstream oss;
	for (size_t i = 0; i < values.size(); ++i) {
		if (i != 0)
			oss << ",";
		oss << values[i];
	}
	return oss.str();
}

static FlowTupleKey make_flow_tuple_key(uint32_t sip, uint32_t dip, uint16_t sport, uint16_t dport) {
	FlowTupleKey key;
	if (sip < dip || (sip == dip && sport <= dport)) {
		key.firstIp = sip;
		key.secondIp = dip;
		key.firstPort = sport;
		key.secondPort = dport;
	} else {
		key.firstIp = dip;
		key.secondIp = sip;
		key.firstPort = dport;
		key.secondPort = sport;
	}
	return key;
}

static void track_foreground_tuple(uint32_t sip, uint32_t dip, uint16_t sport, uint16_t dport) {
	g_foreground_flow_tuples.insert(make_flow_tuple_key(sip, dip, sport, dport));
}

void on_cb_packet_event(FILE *fout,
		uint32_t node_id,
		uint32_t out_dev,
		uint32_t sip,
		uint32_t dip,
		uint16_t sport,
		uint16_t dport,
		uint32_t class_id,
		uint32_t is_drop) {
	(void)node_id;
	(void)out_dev;
	if (fout == nullptr) {
		return;
	}
	bool is_foreground = g_foreground_flow_tuples.count(make_flow_tuple_key(sip, dip, sport, dport)) != 0;
	bool dropped = (is_drop != 0);
	if (class_id == 1) {
		if (is_foreground) {
			g_packet_loss_counters.fgCreditTotal++;
			if (dropped)
				g_packet_loss_counters.fgCreditLoss++;
		} else {
			g_packet_loss_counters.bgCreditTotal++;
			if (dropped)
				g_packet_loss_counters.bgCreditLoss++;
		}
	} else if (class_id == 2) {
		if (is_foreground) {
			g_packet_loss_counters.fgDataTotal++;
			if (dropped)
				g_packet_loss_counters.fgDataLoss++;
		} else {
			g_packet_loss_counters.bgDataTotal++;
			if (dropped)
				g_packet_loss_counters.bgDataLoss++;
		}
	} else {
		return;
	}
	(void)fout;
}

void monitor_packet_loss_count(FILE *packet_loss_output) {
	if (packet_loss_output == nullptr) {
		return;
	}
	uint64_t now = Simulator::Now().GetTimeStep();
	fprintf(packet_loss_output, "%lu %lu %lu %lu %lu %lu %lu %lu %lu\n",
		now,
		g_packet_loss_counters.fgCreditLoss,
		g_packet_loss_counters.fgCreditTotal,
		g_packet_loss_counters.fgDataLoss,
		g_packet_loss_counters.fgDataTotal,
		g_packet_loss_counters.bgCreditLoss,
		g_packet_loss_counters.bgCreditTotal,
		g_packet_loss_counters.bgDataLoss,
		g_packet_loss_counters.bgDataTotal);
	fflush(packet_loss_output);
	if (timeout_mon_interval_ns > 0) {
		Simulator::Schedule(NanoSeconds(timeout_mon_interval_ns), &monitor_packet_loss_count, packet_loss_output);
	}
}

void qp_finish(FILE* fout, Ptr<RdmaQueuePair> q){
	uint32_t sid = ip_to_node_id(q->sip), did = ip_to_node_id(q->dip);
	Ptr<Node> snode = NodeList::GetNode(sid);
	Ptr<Node> dnode = NodeList::GetNode(did);
	// pairRtt/pairBw/pairDelay are populated on one triangular direction only.
	// Normalize key order so lookup is direction-invariant.
	Ptr<Node> lo = snode, hi = dnode;
	if (sid > did){
		lo = dnode;
		hi = snode;
	}
	uint64_t base_rtt = pairRtt[lo][hi], b = pairBw[lo][hi];
	uint64_t one_way_prop_ns = pairDelay[lo][hi];
	uint64_t one_way_fixed_tx_ns = pairTxDelay[lo][hi];
	uint32_t total_bytes = q->m_size;
	uint64_t tx_data_ns = total_bytes * 8000000000lu / b;
	uint64_t ideal_fct_legacy_ns = 0;
	uint64_t ideal_fct_new_ns = 0;
	if (q->cb.m_enabled){
		// CreditBouncer completion is sender-local once the receiver has
		// consumed the full message, so use one-way baselines here.
		ideal_fct_legacy_ns = one_way_prop_ns + one_way_fixed_tx_ns + tx_data_ns;
		ideal_fct_new_ns = one_way_prop_ns + tx_data_ns;
	} else {
		ideal_fct_legacy_ns = base_rtt + tx_data_ns;
		// Baseline RTT uses only propagation (no fixed-packet txDelay term):
		// base_rtt = 2 * one-way pairDelay.
		uint64_t base_rtt_prop_only = 2 * one_way_prop_ns;
		ideal_fct_new_ns = base_rtt_prop_only + tx_data_ns;
	}
	uint64_t fct_ns = (Simulator::Now() - q->startTime).GetTimeStep();
	uint32_t is_foreground = g_foreground_flow_ids.count(q->m_flow_id) ? 1u : 0u;
	if (standalone_fct_mode == 1) {
		// sip, dip, sport, dport, size (B), start_time (ns), fct (ns),
		// ideal_fct_legacy_ns, ideal_fct_new_ns, flow_id, is_foreground
		fprintf(fout, "%08x %08x %u %u %lu %lu %lu %lu %d %u\n",
			q->sip.Get(), q->dip.Get(), q->sport, q->dport, q->m_size,
			q->startTime.GetTimeStep(), fct_ns, ideal_fct_new_ns,
			q->m_flow_id, is_foreground);
	} else {
		// sip, dip, sport, dport, size (B), start_time (ns), fct (ns),
		// ideal_fct_legacy_ns, flow_id, is_foreground
		fprintf(fout, "%08x %08x %u %u %lu %lu %lu %lu %d %u\n",
			q->sip.Get(), q->dip.Get(), q->sport, q->dport, q->m_size,
			q->startTime.GetTimeStep(), fct_ns, ideal_fct_legacy_ns,
			q->m_flow_id, is_foreground);
	}
	fflush(fout);
}

void get_pfc(FILE* fout, Ptr<QbbNetDevice> dev, uint32_t type){
	(void)fout;
	(void)dev;
	(void)type;
	g_pfc_window_count++;
}

void get_timeout(FILE* fout, Ptr<RdmaQueuePair> q, bool irn_enabled, uint64_t rto_ns){
	(void)fout;
	(void)q;
	(void)irn_enabled;
	(void)rto_ns;
	g_rdma_timeout_window_count++;
}

void get_creditbouncer_timeout(FILE* fout, Ptr<RdmaRxQueuePair> q, uint64_t resend_interval_ns){
	(void)fout;
	(void)q;
	(void)resend_interval_ns;
	g_cb_resend_timeout_window_count++;
}

void get_bounced_credit(FILE* fout, uint32_t in_dev, uint32_t out_dev, uint64_t credit_bytes){
	(void)fout;
	(void)in_dev;
	(void)out_dev;
	(void)credit_bytes;
	g_bounced_window_count++;
}

void monitor_timeout_count(FILE* timeout_output){
	uint64_t now = Simulator::Now().GetTimeStep();
	uint64_t timeout_count = enable_creditbouncer
		? g_cb_resend_timeout_window_count
		: g_rdma_timeout_window_count;
	fprintf(timeout_output, "%lu %lu\n", now, timeout_count);
	fflush(timeout_output);
	g_rdma_timeout_window_count = 0;
	g_cb_resend_timeout_window_count = 0;
	if (Simulator::Now().GetSeconds() < simulator_stop_time){
		Simulator::Schedule(NanoSeconds(timeout_mon_interval_ns), &monitor_timeout_count, timeout_output);
	}
}

void monitor_pfc_count(FILE* pfc_output){
	uint64_t now = Simulator::Now().GetTimeStep();
	fprintf(pfc_output, "%lu %lu\n", now, g_pfc_window_count);
	fflush(pfc_output);
	g_pfc_window_count = 0;
	if (Simulator::Now().GetSeconds() < simulator_stop_time){
		Simulator::Schedule(NanoSeconds(pfc_mon_interval_ns), &monitor_pfc_count, pfc_output);
	}
}

void monitor_bounced_count(FILE* bounced_output){
	uint64_t now = Simulator::Now().GetTimeStep();
	fprintf(bounced_output, "%lu %lu\n", now, g_bounced_window_count);
	fflush(bounced_output);
	g_bounced_window_count = 0;
	if (Simulator::Now().GetSeconds() < simulator_stop_time){
		Simulator::Schedule(NanoSeconds(bounced_mon_interval_ns), &monitor_bounced_count, bounced_output);
	}
}

struct PauseIntervalKey {
	uint32_t switch_id;
	uint32_t ifindex;
	uint32_t qindex;

	bool operator<(const PauseIntervalKey &other) const {
		if (switch_id != other.switch_id)
			return switch_id < other.switch_id;
		if (ifindex != other.ifindex)
			return ifindex < other.ifindex;
		return qindex < other.qindex;
	}
};

struct XoffIntervalKey {
	uint32_t switch_id;
	uint32_t out_dev;

	bool operator<(const XoffIntervalKey &other) const {
		if (switch_id != other.switch_id)
			return switch_id < other.switch_id;
		return out_dev < other.out_dev;
	}
};

static std::map<PauseIntervalKey, uint64_t> g_pause_interval_starts;
static std::map<XoffIntervalKey, uint64_t> g_xoff_interval_starts;

void record_pause_interval(FILE* fout, Ptr<QbbNetDevice> dev, uint32_t ifindex, uint32_t qindex, uint32_t type){
	if (fout == nullptr || dev == nullptr)
		return;
	Ptr<Node> node = dev->GetNode();
	if (node == nullptr || node->GetNodeType() != 1)
		return;
	PauseIntervalKey key{node->GetId(), ifindex, qindex};
	uint64_t now = Simulator::Now().GetTimeStep();
	if (type != 0){
		g_pause_interval_starts.emplace(key, now);
		return;
	}
	auto it = g_pause_interval_starts.find(key);
	if (it == g_pause_interval_starts.end())
		return;
	uint64_t start_ns = it->second;
	fprintf(fout, "%u %u %u %lu %lu %lu\n",
		key.switch_id, key.ifindex, key.qindex, start_ns, now, now - start_ns);
	fflush(fout);
	g_pause_interval_starts.erase(it);
}

void finalize_pause_intervals(FILE* fout){
	if (fout == nullptr)
		return;
	uint64_t now = Simulator::Now().GetTimeStep();
	for (const auto &entry : g_pause_interval_starts){
		const PauseIntervalKey &key = entry.first;
		uint64_t start_ns = entry.second;
		fprintf(fout, "%u %u %u %lu %lu %lu\n",
			key.switch_id, key.ifindex, key.qindex, start_ns, now, now - start_ns);
	}
	fflush(fout);
	g_pause_interval_starts.clear();
}

void record_xoff_interval(FILE* fout, Ptr<SwitchNode> sw, uint32_t out_dev, uint32_t old_state, uint32_t new_state){
	if (fout == nullptr || sw == nullptr)
		return;
	XoffIntervalKey key{sw->GetId(), out_dev};
	uint64_t now = Simulator::Now().GetTimeStep();
	if (old_state == 0 && new_state == 1){
		g_xoff_interval_starts.emplace(key, now);
		return;
	}
	if (!(old_state == 1 && new_state == 0))
		return;
	auto it = g_xoff_interval_starts.find(key);
	if (it == g_xoff_interval_starts.end())
		return;
	uint64_t start_ns = it->second;
	fprintf(fout, "%u %u %lu %lu %lu\n",
		key.switch_id, key.out_dev, start_ns, now, now - start_ns);
	fflush(fout);
	g_xoff_interval_starts.erase(it);
}

void finalize_xoff_intervals(FILE* fout){
	if (fout == nullptr)
		return;
	uint64_t now = Simulator::Now().GetTimeStep();
	for (const auto &entry : g_xoff_interval_starts){
		const XoffIntervalKey &key = entry.first;
		uint64_t start_ns = entry.second;
		fprintf(fout, "%u %u %lu %lu %lu\n",
			key.switch_id, key.out_dev, start_ns, now, now - start_ns);
	}
	fflush(fout);
	g_xoff_interval_starts.clear();
}

void CalculateRoute(Ptr<Node> host){
	// queue for the BFS.
	vector<Ptr<Node> > q;
	// Distance from the host to each node.
	map<Ptr<Node>, int> dis;
	map<Ptr<Node>, uint64_t> delay;
	map<Ptr<Node>, uint64_t> txDelay;
	map<Ptr<Node>, uint64_t> bw;
	// init BFS.
	q.push_back(host);
	dis[host] = 0;
	delay[host] = 0;
	txDelay[host] = 0;
	bw[host] = 0xfffffffffffffffflu;
	// BFS.
	for (int i = 0; i < (int)q.size(); i++){
		Ptr<Node> now = q[i];
		int d = dis[now];
		for (auto it = nbr2if[now].begin(); it != nbr2if[now].end(); it++){
			// skip down link
			if (!it->second.up)
				continue;
			Ptr<Node> next = it->first;
			// If 'next' have not been visited.
			if (dis.find(next) == dis.end()){
				dis[next] = d + 1;
				delay[next] = delay[now] + it->second.delay;
				txDelay[next] = txDelay[now] + packet_payload_size * 1000000000lu * 8 / it->second.bw;
				bw[next] = std::min(bw[now], it->second.bw);
				// we only enqueue switch, because we do not want packets to go through host as middle point
				if (next->GetNodeType() == 1)
					q.push_back(next);
			}
			// if 'now' is on the shortest path from 'next' to 'host'.
			if (d + 1 == dis[next]){
				nextHop[next][host].push_back(now);
			}
		}
	}
	for (auto it : delay)
		pairDelay[it.first][host] = it.second;
	for (auto it : txDelay)
		pairTxDelay[it.first][host] = it.second;
	for (auto it : bw)
		pairBw[it.first][host] = it.second;
}

void CalculateRoutes(NodeContainer &n){
	for (int i = 0; i < (int)n.GetN(); i++){
		Ptr<Node> node = n.Get(i);
		if (node->GetNodeType() == 0)
			CalculateRoute(node);
	}
}

void SetRoutingEntries(){
	// For each node.
	for (auto i = nextHop.begin(); i != nextHop.end(); i++){
		Ptr<Node> node = i->first;
		auto &table = i->second;
		for (auto j = table.begin(); j != table.end(); j++){
			// The destination node.
			Ptr<Node> dst = j->first;
			// The IP address of the dst.
			Ipv4Address dstAddr = dst->GetObject<Ipv4>()->GetAddress(1, 0).GetLocal();
			// The next hops towards the dst.
			vector<Ptr<Node> > nexts = j->second;
			for (int k = 0; k < (int)nexts.size(); k++){
				Ptr<Node> next = nexts[k];
				uint32_t interface = nbr2if[node][next].idx;
				if (node->GetNodeType() == 1)
					DynamicCast<SwitchNode>(node)->AddTableEntry(dstAddr, interface);
				else{
					node->GetObject<RdmaDriver>()->m_rdma->AddTableEntry(dstAddr, interface);
				}
			}
		}
	}
}

// take down the link between a and b, and redo the routing
void TakeDownLink(NodeContainer n, Ptr<Node> a, Ptr<Node> b){
	if (!nbr2if[a][b].up)
		return;
	// take down link between a and b
	nbr2if[a][b].up = nbr2if[b][a].up = false;
	nextHop.clear();
	CalculateRoutes(n);
	// clear routing tables
	for (uint32_t i = 0; i < n.GetN(); i++){
		if (n.Get(i)->GetNodeType() == 1)
			DynamicCast<SwitchNode>(n.Get(i))->ClearTable();
		else
			n.Get(i)->GetObject<RdmaDriver>()->m_rdma->ClearTable();
	}
	DynamicCast<QbbNetDevice>(a->GetDevice(nbr2if[a][b].idx))->TakeDown();
	DynamicCast<QbbNetDevice>(b->GetDevice(nbr2if[b][a].idx))->TakeDown();
	// reset routing table
	SetRoutingEntries();

	// redistribute qp on each host
	for (uint32_t i = 0; i < n.GetN(); i++){
		if (n.Get(i)->GetNodeType() == 0)
			n.Get(i)->GetObject<RdmaDriver>()->m_rdma->RedistributeQp();
	}
}

uint64_t get_nic_rate(NodeContainer &n){
	for (uint32_t i = 0; i < n.GetN(); i++)
		if (n.Get(i)->GetNodeType() == 0)
			return DynamicCast<QbbNetDevice>(n.Get(i)->GetDevice(1))->GetDataRate().GetBitRate();
}


bool load_workload(const char *workload_file,
	std::vector<std::pair<double, uint32_t>> *workload_cdf,
	double *avg_flow_size);
uint32_t sample_workload_size(const std::vector<std::pair<double, uint32_t>> &workload_cdf, double u);



int main(int argc, char *argv[])
{
	std::vector<std::pair<double, uint32_t>> workload_cdf;
	double avg_flow_size = 0.;

	clock_t begint, endt;
	begint = clock();
#ifndef PGO_TRAINING
	if (argc > 1)
#else
	if (true)
#endif
	{
		//Read the configuration file
		std::ifstream conf;
#ifndef PGO_TRAINING
		conf.open(argv[1]);
#else
		conf.open(PATH_TO_PGO_CONFIG);
#endif
		while (!conf.eof())
		{
			std::string key;
			conf >> key;

			//std::cerr << conf.cur << "\n";

			if (key.compare("ENABLE_PFC") == 0)
			{
				uint32_t v;
				conf >> v;
				enable_pfc = v;
				if (enable_pfc)
					std::cerr << "ENABLE_PFC\t\t\t" << "Yes" << "\n";
				else
					std::cerr << "ENABLE_PFC\t\t\t" << "No" << "\n";
			}
			else if (key.compare("ENABLE_QCN") == 0)
			{
				uint32_t v;
				conf >> v;
				enable_qcn = v;
				if (enable_qcn)
					std::cerr << "ENABLE_QCN\t\t\t" << "Yes" << "\n";
				else
					std::cerr << "ENABLE_QCN\t\t\t" << "No" << "\n";
			}
			else if (key.compare("USE_DYNAMIC_PFC_THRESHOLD") == 0)
			{
				uint32_t v;
				conf >> v;
				use_dynamic_pfc_threshold = v;
				if (use_dynamic_pfc_threshold)
					std::cerr << "USE_DYNAMIC_PFC_THRESHOLD\t" << "Yes" << "\n";
				else
					std::cerr << "USE_DYNAMIC_PFC_THRESHOLD\t" << "No" << "\n";
			}
			else if (key.compare("CLAMP_TARGET_RATE") == 0)
			{
				uint32_t v;
				conf >> v;
				clamp_target_rate = v;
				if (clamp_target_rate)
					std::cerr << "CLAMP_TARGET_RATE\t\t" << "Yes" << "\n";
				else
					std::cerr << "CLAMP_TARGET_RATE\t\t" << "No" << "\n";
			}
			else if (key.compare("PAUSE_TIME") == 0)
			{
				double v;
				conf >> v;
				pause_time = v;
				std::cerr << "PAUSE_TIME\t\t\t" << pause_time << "\n";
			}
			else if (key.compare("DATA_RATE") == 0)
			{
				std::string v;
				conf >> v;
				data_rate = v;
				std::cerr << "DATA_RATE\t\t\t" << data_rate << "\n";
			}
			else if (key.compare("LINK_DELAY") == 0)
			{
				std::string v;
				conf >> v;
				link_delay = v;
				std::cerr << "LINK_DELAY\t\t\t" << link_delay << "\n";
			}
			else if (key.compare("PACKET_PAYLOAD_SIZE") == 0)
			{
				uint32_t v;
				conf >> v;
				packet_payload_size = v;
				std::cerr << "PACKET_PAYLOAD_SIZE\t\t" << packet_payload_size << "\n";
			}
			else if (key.compare("L2_CHUNK_SIZE") == 0)
			{
				uint32_t v;
				conf >> v;
				l2_chunk_size = v;
				std::cerr << "L2_CHUNK_SIZE\t\t\t" << l2_chunk_size << "\n";
			}
			else if (key.compare("L2_ACK_INTERVAL") == 0)
			{
				uint32_t v;
				conf >> v;
				l2_ack_interval = v;
				std::cerr << "L2_ACK_INTERVAL\t\t\t" << l2_ack_interval << "\n";
			}
			else if (key.compare("L2_BACK_TO_ZERO") == 0)
			{
				uint32_t v;
				conf >> v;
				l2_back_to_zero = v;
				if (l2_back_to_zero)
					std::cerr << "L2_BACK_TO_ZERO\t\t\t" << "Yes" << "\n";
				else
					std::cerr << "L2_BACK_TO_ZERO\t\t\t" << "No" << "\n";
			}
			else if (key.compare("TOPOLOGY_FILE") == 0)
			{
				std::string v;
				conf >> v;
				topology_file = v;
				std::cerr << "TOPOLOGY_FILE\t\t\t" << topology_file << "\n";
			}
			else if (key.compare("FLOW_FILE") == 0)
			{
				std::string v;
				conf >> v;
				flow_file = v;
				std::cerr << "FLOW_FILE\t\t\t" << flow_file << "\n";
			}
			else if (key.compare("TRACE_FILE") == 0)
			{
				std::string v;
				conf >> v;
				trace_file = v;
				std::cerr << "TRACE_FILE\t\t\t" << trace_file << "\n";
			}
			else if (key.compare("TRACE_OUTPUT_FILE") == 0)
			{
				std::string v;
				conf >> v;
				trace_output_file = v;
				if (argc > 2)
				{
					trace_output_file = trace_output_file + std::string(argv[2]);
				}
				std::cerr << "TRACE_OUTPUT_FILE\t\t" << trace_output_file << "\n";
			}
			else if (key.compare("SIMULATOR_STOP_TIME") == 0)
			{
				double v;
				conf >> v;
				simulator_stop_time = v;
				std::cerr << "SIMULATOR_STOP_TIME\t\t" << simulator_stop_time << "\n";
			}
			else if (key.compare("ALPHA_RESUME_INTERVAL") == 0)
			{
				double v;
				conf >> v;
				alpha_resume_interval = v;
				std::cerr << "ALPHA_RESUME_INTERVAL\t\t" << alpha_resume_interval << "\n";
			}
			else if (key.compare("RP_TIMER") == 0)
			{
				double v;
				conf >> v;
				rp_timer = v;
				std::cerr << "RP_TIMER\t\t\t" << rp_timer << "\n";
			}
			else if (key.compare("L2_TIMEOUT_NS") == 0)
			{
				conf >> l2_timeout_ns;
				std::cerr << "L2_TIMEOUT_NS\t\t\t" << l2_timeout_ns << "\n";
			}
			else if (key.compare("RP_BYTE_RESET") == 0)
			{
				uint64_t v;
				conf >> v;
				rp_byte_reset = v;
				std::cerr << "RP_BYTE_RESET\t\t\t" << rp_byte_reset << "\n";
			}
			else if (key.compare("EWMA_GAIN") == 0)
			{
				double v;
				conf >> v;
				ewma_gain = v;
				std::cerr << "EWMA_GAIN\t\t\t" << ewma_gain << "\n";
			}
			else if (key.compare("FAST_RECOVERY_TIMES") == 0)
			{
				uint32_t v;
				conf >> v;
				fast_recovery_times = v;
				std::cerr << "FAST_RECOVERY_TIMES\t\t" << fast_recovery_times << "\n";
			}
			else if (key.compare("RATE_AI") == 0)
			{
				std::string v;
				conf >> v;
				rate_ai = v;
				std::cerr << "RATE_AI\t\t\t\t" << rate_ai << "\n";
			}
			else if (key.compare("RATE_HAI") == 0)
			{
				std::string v;
				conf >> v;
				rate_hai = v;
				std::cerr << "RATE_HAI\t\t\t" << rate_hai << "\n";
			}
			else if (key.compare("ERROR_RATE_PER_LINK") == 0)
			{
				double v;
				conf >> v;
				error_rate_per_link = v;
				std::cerr << "ERROR_RATE_PER_LINK\t\t" << error_rate_per_link << "\n";
			}
			else if (key.compare("CC_MODE") == 0){
				conf >> cc_mode;
				std::cerr << "CC_MODE\t\t" << cc_mode << '\n';
			}else if (key.compare("RATE_DECREASE_INTERVAL") == 0){
				double v;
				conf >> v;
				rate_decrease_interval = v;
				std::cerr << "RATE_DECREASE_INTERVAL\t\t" << rate_decrease_interval << "\n";
			}else if (key.compare("MIN_RATE") == 0){
				conf >> min_rate;
				std::cerr << "MIN_RATE\t\t" << min_rate << "\n";
			}else if (key.compare("FCT_OUTPUT_FILE") == 0){
				conf >> fct_output_file;
				std::cerr << "FCT_OUTPUT_FILE\t\t" << fct_output_file << '\n';
			}else if (key.compare("STANDALONE_FCT_MODE") == 0){
				conf >> standalone_fct_mode;
				std::cerr << "STANDALONE_FCT_MODE\t\t" << standalone_fct_mode << " (0=legacy_only,1=legacy_and_new)\n";
			}else if (key.compare("HAS_WIN") == 0){
				conf >> has_win;
				std::cerr << "HAS_WIN\t\t" << has_win << "\n";
			}else if (key.compare("GLOBAL_T") == 0){
				conf >> global_t;
				std::cerr << "GLOBAL_T\t\t" << global_t << '\n';
			}else if (key.compare("MI_THRESH") == 0){
				conf >> mi_thresh;
				std::cerr << "MI_THRESH\t\t" << mi_thresh << '\n';
			}else if (key.compare("VAR_WIN") == 0){
				uint32_t v;
				conf >> v;
				var_win = v;
				std::cerr << "VAR_WIN\t\t" << v << '\n';
			}else if (key.compare("FAST_REACT") == 0){
				uint32_t v;
				conf >> v;
				fast_react = v;
				std::cerr << "FAST_REACT\t\t" << v << '\n';
			}else if (key.compare("U_TARGET") == 0){
				conf >> u_target;
				std::cerr << "U_TARGET\t\t" << u_target << '\n';
			}else if (key.compare("INT_MULTI") == 0){
				conf >> int_multi;
				std::cerr << "INT_MULTI\t\t\t\t" << int_multi << '\n';
			}else if (key.compare("RATE_BOUND") == 0){
				uint32_t v;
				conf >> v;
				rate_bound = v;
				std::cerr << "RATE_BOUND\t\t" << rate_bound << '\n';
			}else if (key.compare("ACK_HIGH_PRIO") == 0){
				conf >> ack_high_prio;
				std::cerr << "ACK_HIGH_PRIO\t\t" << ack_high_prio << '\n';
			}else if (key.compare("DCTCP_RATE_AI") == 0){
				conf >> dctcp_rate_ai;
				std::cerr << "DCTCP_RATE_AI\t\t\t\t" << dctcp_rate_ai << "\n";
			}else if (key.compare("PFC_OUTPUT_FILE") == 0){
				conf >> pfc_output_file;
				std::cerr << "PFC_OUTPUT_FILE\t\t\t\t" << pfc_output_file << '\n';
			}else if (key.compare("TIMEOUT_OUTPUT_FILE") == 0){
				conf >> timeout_output_file;
				std::cerr << "TIMEOUT_OUTPUT_FILE\t\t\t" << timeout_output_file << '\n';
			}else if (key.compare("TIMEOUT_MON_INTERVAL_NS") == 0){
				conf >> timeout_mon_interval_ns;
				std::cerr << "TIMEOUT_MON_INTERVAL_NS\t\t\t" << timeout_mon_interval_ns << '\n';
			}else if (key.compare("PFC_MON_INTERVAL_NS") == 0){
				conf >> pfc_mon_interval_ns;
				std::cerr << "PFC_MON_INTERVAL_NS\t\t\t" << pfc_mon_interval_ns << '\n';
			}else if (key.compare("PAUSE_INTERVALS_OUTPUT_FILE") == 0){
				conf >> pause_intervals_output_file;
				std::cerr << "PAUSE_INTERVALS_OUTPUT_FILE\t\t" << pause_intervals_output_file << '\n';
			}else if (key.compare("BOUNCED_OUTPUT_FILE") == 0){
				conf >> bounced_output_file;
				std::cerr << "BOUNCED_OUTPUT_FILE\t\t\t" << bounced_output_file << '\n';
			}else if (key.compare("BOUNCED_MON_INTERVAL_NS") == 0){
				conf >> bounced_mon_interval_ns;
				std::cerr << "BOUNCED_MON_INTERVAL_NS\t\t\t" << bounced_mon_interval_ns << '\n';
			}else if (key.compare("XOFF_INTERVALS_OUTPUT_FILE") == 0){
				conf >> xoff_intervals_output_file;
				std::cerr << "XOFF_INTERVALS_OUTPUT_FILE\t\t" << xoff_intervals_output_file << '\n';
			}else if (key.compare("PACKET_LOSS_OUTPUT_FILE") == 0){
				conf >> packet_loss_output_file;
				std::cerr << "PACKET_LOSS_OUTPUT_FILE\t\t" << packet_loss_output_file << '\n';
			}else if (key.compare("LINK_DOWN") == 0){
				conf >> link_down_time >> link_down_A >> link_down_B;
				std::cerr << "LINK_DOWN\t\t\t\t" << link_down_time << ' '<< link_down_A << ' ' << link_down_B << '\n';
			}else if (key.compare("ENABLE_TRACE") == 0){
				conf >> enable_trace;
				std::cerr << "ENABLE_TRACE\t\t\t\t" << enable_trace << '\n';
			}else if (key.compare("KMAX_MAP") == 0){
				int n_k ;
				conf >> n_k;
				std::cerr << "KMAX_MAP\t\t\t\t";
				for (int i = 0; i < n_k; i++){
					uint64_t rate;
					uint32_t k;
					conf >> rate >> k;
					rate2kmax[rate] = k;
					std::cerr << ' ' << rate << ' ' << k;
				}
				std::cerr<<'\n';
			}else if (key.compare("KMIN_MAP") == 0){
				int n_k ;
				conf >> n_k;
				std::cerr << "KMIN_MAP\t\t\t\t";
				for (int i = 0; i < n_k; i++){
					uint64_t rate;
					uint32_t k;
					conf >> rate >> k;
					rate2kmin[rate] = k;
					std::cerr << ' ' << rate << ' ' << k;
				}
				std::cerr<<'\n';
			}else if (key.compare("PMAX_MAP") == 0){
				int n_k ;
				conf >> n_k;
				std::cerr << "PMAX_MAP\t\t\t\t";
				for (int i = 0; i < n_k; i++){
					uint64_t rate;
					double p;
					conf >> rate >> p;
					rate2pmax[rate] = p;
					std::cerr << ' ' << rate << ' ' << p;
				}
				std::cerr<<'\n';
			}else if (key.compare("BUFFER_SIZE") == 0){
				conf >> buffer_size;
				std::cerr << "BUFFER_SIZE\t\t\t\t" << buffer_size << '\n';
			}else if (key.compare("SWITCH_SHARED_BUFFER_PER_PORT_BYTES") == 0){
				conf >> switch_shared_buffer_per_port_bytes;
				std::cerr << "SWITCH_SHARED_BUFFER_PER_PORT_BYTES\t" << switch_shared_buffer_per_port_bytes << '\n';
			}else if (key.compare("MULTI_RATE") == 0){
				int v;
				conf >> v;
				multi_rate = v;
				std::cerr << "MULTI_RATE\t\t\t\t" << multi_rate << '\n';
			}else if (key.compare("SAMPLE_FEEDBACK") == 0){
				int v;
				conf >> v;
				sample_feedback = v;
				std::cerr << "SAMPLE_FEEDBACK\t\t\t\t" << sample_feedback << '\n';
            // Added From Here
				}else if (key.compare("HPCC_WORKLOAD") == 0 || key.compare("TCP_FLOW_FILE") == 0){
					std::string v;
					conf >> v;
					hpcc_workload = v;
					std::cerr << "HPCC_WORKLOAD(TCP_FLOW_FILE)\t\t" << hpcc_workload << '\n';
				} else if (key.compare("BG_FLOW_GEN_MODE") == 0 || key.compare("FLOW_GEN_MODE") == 0) {
					std::string v;
					conf >> v;
					std::string lv = v;
					std::transform(lv.begin(), lv.end(), lv.begin(),
						[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
					if (lv == "count" || lv == "0") {
						bg_flow_gen_mode = "count";
					} else if (lv == "load" || lv == "1") {
						bg_flow_gen_mode = "load";
					} else {
						NS_ABORT_MSG("Unknown BG_FLOW_GEN_MODE/FLOW_GEN_MODE: " << v << " (allowed: count/load or 0/1)");
					}
					std::cerr << "BG_FLOW_GEN_MODE\t\t" << bg_flow_gen_mode << "\n";
				} else if (key.compare("LOAD") == 0) {
					double v;
					conf >> v;
					load = v;
					std::cerr << "LOAD\t\t\t" << load << "\n";
			} else if (key.compare("FOREGROUND_RATIO") == 0) {
				double v;
				conf >> v;
				foreground_flow_ratio = 0.0;
				std::cerr << "FOREGROUND_RATIO\t\t" << v << " (deprecated, ignored)\n";
			} else if (key.compare("ENABLE_FOREGROUND_INCAST") == 0) {
				bool v;
				conf >> v;
				enable_foreground_incast = v;
				std::cerr << "ENABLE_FOREGROUND_INCAST\t" << enable_foreground_incast << "\n";
			} else if (key.compare("FOREGROUND_INCAST_START_TIME") == 0) {
				double v;
				conf >> v;
				foreground_incast_start_time = v;
				std::cerr << "FOREGROUND_INCAST_START_TIME\t" << foreground_incast_start_time << "\n";
			} else if (key.compare("FOREGROUND_INCAST_RACKS") == 0) {
				uint32_t v;
				conf >> v;
				foreground_incast_rack_count = v;
				std::cerr << "FOREGROUND_INCAST_RACKS\t" << foreground_incast_rack_count << "\n";
			} else if (key.compare("FOREGROUND_INCAST_SENDERS_PER_RACK") == 0) {
				uint32_t v;
				conf >> v;
				foreground_incast_senders_per_rack = v;
				std::cerr << "FOREGROUND_INCAST_SENDERS_PER_RACK\t" << foreground_incast_senders_per_rack << "\n";
				} else if (key.compare("FOREGROUND_RECEIVER_HOST_ID") == 0) {
					int32_t v;
					conf >> v;
					foreground_receiver_host_id = v;
					std::cerr << "FOREGROUND_RECEIVER_HOST_ID\t" << foreground_receiver_host_id << "\n";
				} else if (key.compare("FOREGROUND_SOURCE_LEAF_IDS") == 0) {
					std::string v;
					conf >> v;
					foreground_source_leaf_ids = parse_uint_csv(v);
					std::cerr << "FOREGROUND_SOURCE_LEAF_IDS\t" << join_uint_vector(foreground_source_leaf_ids) << "\n";
				} else if (key.compare("FOREGROUND_USE_FIRST_HOSTS_PER_LEAF") == 0) {
					bool v;
					conf >> v;
					foreground_use_first_hosts_per_leaf = v;
					std::cerr << "FOREGROUND_USE_FIRST_HOSTS_PER_LEAF\t" << foreground_use_first_hosts_per_leaf << "\n";
				} else if (key.compare("FOREGROUND_INCAST_LEAF_JITTER_NS") == 0) {
					uint64_t v;
					conf >> v;
					foreground_incast_leaf_jitter_ns = v;
					std::cerr << "FOREGROUND_INCAST_LEAF_JITTER_NS\t" << foreground_incast_leaf_jitter_ns << "\n";
				} else if (key.compare("FOREGROUND_INCAST_HOST_JITTER_STEP_NS") == 0) {
					uint64_t v;
					conf >> v;
					foreground_incast_host_jitter_step_ns = v;
					std::cerr << "FOREGROUND_INCAST_HOST_JITTER_STEP_NS\t" << foreground_incast_host_jitter_step_ns << "\n";
				} else if (key.compare("FOREGROUND_INCAST_SHUFFLE_LEAF_ORDER_PER_ROUND") == 0) {
					bool v;
					conf >> v;
					foreground_incast_shuffle_leaf_order_per_round = v;
					std::cerr << "FOREGROUND_INCAST_SHUFFLE_LEAF_ORDER_PER_ROUND\t" << foreground_incast_shuffle_leaf_order_per_round << "\n";
				} else if (key.compare("APP_START_TIME") == 0) {
					double v;
					conf >> v;
				app_start_time = v;
				std::cerr << "APP_START_TIME\t\t\t" << app_start_time << "\n";
			} else if (key.compare("APP_STOP_TIME") == 0) {
				double v;
				conf >> v;
				app_stop_time = v;
				std::cerr << "APP_STOP_TIME\t\t\t" << app_stop_time << "\n";
			} else if (key.compare("DCTCP_INCAST_SIZE") == 0 || key.compare("INCAST_FLOW_SIZE") == 0) {
				double v;
				conf >> v;
				incast_flow_size = v;
				std::cerr << "INCAST_FLOW_SIZE(DCTCP_INCAST_SIZE)\t\t" << incast_flow_size << "\n";
            }else if (key.compare("NUM_BG_FLOWS") == 0 || key.compare("TCP_FLOW_TOTAL") == 0){
				int v;
				conf >> v;
				num_bg_flows = v;
				std::cerr << "NUM_BG_FLOWS(TCP_FLOW_TOTAL)\t\t\t\t" << num_bg_flows << '\n';
            } else if (key.compare("ENABLE_IRN") == 0) {
				bool v;
				conf >> v;
				enable_irn = v;
				std::cerr << "ENABLE_IRN\t\t" << enable_irn << "\n";
            } else if (key.compare("ENABLE_TLT") == 0) {
				bool v;
				conf >> v;
				enable_tlt = v;
				std::cerr << "ENABLE_TLT\t\t" << enable_tlt << "\n";
			} else if (key.compare("IRN_NO_BDPFC") == 0) {
				bool v;
				conf >> v;
				irn_no_bdpfc = v;
				std::cerr << "IRN_NO_BDPFC\t\t" << irn_no_bdpfc << "\n";
			} else if (key.compare("IRN_BDP_BYTES") == 0) {
				uint32_t v;
				conf >> v;
				irn_bdp_bytes = v;
				std::cerr << "IRN_BDP_BYTES\t\t" << irn_bdp_bytes << "\n";
			} else if (key.compare("TLT_MAXBYTES_UIP") == 0) {
				double v;
				conf >> v;
				tlt_maxbytes_uip = v;
				std::cerr << "TLT_MAXBYTES_UIP\t\t" << tlt_maxbytes_uip << "\n";
			} else if (key.compare("FOREGROUND_INCAST_FLOW_PER_HOST") == 0) {
				uint32_t v;
				conf >> v;
				FOREGROUND_INCAST_FLOW_PER_HOST = v;
				foreground_incast_senders_per_rack = v;
				std::cerr << "FOREGROUND_INCAST_FLOW_PER_HOST\t\t" << FOREGROUND_INCAST_FLOW_PER_HOST
					  << " (alias of FOREGROUND_INCAST_SENDERS_PER_RACK)\n";
			} else if (key.compare("RANDOM_SEED") == 0) {
				int v;
				conf >> v;
				random_seed = v;
				std::cerr << "RANDOM_SEED\t\t\t" << random_seed << "\n";
			} else if (key.compare("ENABLE_CREDITBOUNCER") == 0) {
				bool v;
				conf >> v;
				enable_creditbouncer = v;
				std::cerr << "ENABLE_CREDITBOUNCER\t\t" << enable_creditbouncer << "\n";
			} else if (key.compare("CREDITBOUNCER_CC_ENABLE") == 0) {
				bool v;
				conf >> v;
				creditbouncer_cc_enable = v;
				std::cerr << "CREDITBOUNCER_CC_ENABLE\t\t" << creditbouncer_cc_enable << "\n";
			} else if (key.compare("CREDITBOUNCER_CSN_ENABLE") == 0) {
				bool v;
				conf >> v;
				creditbouncer_csn_enable = v;
				std::cerr << "CREDITBOUNCER_CSN_ENABLE\t\t" << creditbouncer_csn_enable << "\n";
			} else if (key.compare("CREDITBOUNCER_FC_ENABLE") == 0) {
				bool v;
				conf >> v;
				creditbouncer_fc_enable = v;
				std::cerr << "CREDITBOUNCER_FC_ENABLE\t\t" << creditbouncer_fc_enable << "\n";
			} else if (key.compare("CREDITBOUNCER_GRANT_SCHED_POLICY") == 0) {
				conf >> creditbouncer_grant_sched_policy;
				std::transform(creditbouncer_grant_sched_policy.begin(),
					       creditbouncer_grant_sched_policy.end(),
					       creditbouncer_grant_sched_policy.begin(),
					       [](unsigned char c) { return std::toupper(c); });
				std::cerr << "CREDITBOUNCER_GRANT_SCHED_POLICY\t"
					  << creditbouncer_grant_sched_policy << "\n";
			} else if (key.compare("CREDITBOUNCER_UNSCH_THRESHOLD_BYTES") == 0) {
				conf >> creditbouncer_unsch_threshold_bytes;
				std::cerr << "CREDITBOUNCER_UNSCH_THRESHOLD_BYTES\t" << creditbouncer_unsch_threshold_bytes << "\n";
			} else if (key.compare("CREDITBOUNCER_GLOBAL_BUCKET_BYTES") == 0) {
				conf >> creditbouncer_global_bucket_bytes;
				std::cerr << "CREDITBOUNCER_GLOBAL_BUCKET_BYTES\t" << creditbouncer_global_bucket_bytes << "\n";
			} else if (key.compare("CREDITBOUNCER_MAX_SRPB_BYTES") == 0) {
				conf >> creditbouncer_maxSrpb_bytes;
				std::cerr << "CREDITBOUNCER_MAX_SRPB_BYTES\t\t" << creditbouncer_maxSrpb_bytes << "\n";
			} else if (key.compare("CREDITBOUNCER_SENDER_THRESHOLD_BYTES") == 0) {
				conf >> creditbouncer_sender_threshold_bytes;
				std::cerr << "CREDITBOUNCER_SENDER_THRESHOLD_BYTES\t" << creditbouncer_sender_threshold_bytes << "\n";
			} else if (key.compare("CREDITBOUNCER_GRANT_GRANULARITY_BYTES") == 0) {
				conf >> creditbouncer_grant_granularity_bytes;
				std::cerr << "CREDITBOUNCER_GRANT_GRANULARITY_BYTES\t" << creditbouncer_grant_granularity_bytes << "\n";
			} else if (key.compare("CREDITBOUNCER_AI_STEP_BYTES") == 0) {
				conf >> creditbouncer_ai_step_bytes;
				std::cerr << "CREDITBOUNCER_AI_STEP_BYTES\t" << creditbouncer_ai_step_bytes << "\n";
			} else if (key.compare("CREDITBOUNCER_MD_FACTOR") == 0) {
				conf >> creditbouncer_md_factor;
				std::cerr << "CREDITBOUNCER_MD_FACTOR\t" << creditbouncer_md_factor << "\n";
			} else if (key.compare("CREDITBOUNCER_CE_NEW_WEIGHT") == 0) {
				conf >> creditbouncer_ce_new_weight;
				std::cerr << "CREDITBOUNCER_CE_NEW_WEIGHT\t" << creditbouncer_ce_new_weight << "\n";
			} else if (key.compare("CREDITBOUNCER_GRANT_INTERVAL_NS") == 0) {
				conf >> creditbouncer_grant_interval_ns;
				std::cerr << "CREDITBOUNCER_GRANT_INTERVAL_NS\t" << creditbouncer_grant_interval_ns << "\n";
			} else if (key.compare("CREDITBOUNCER_RESEND_INTERVAL_NS") == 0) {
				conf >> creditbouncer_resend_interval_ns;
				std::cerr << "CREDITBOUNCER_RESEND_INTERVAL_NS\t" << creditbouncer_resend_interval_ns << "\n";
			} else if (key.compare("CREDITBOUNCER_SYMMETRIC_ROUTING") == 0) {
				bool v;
				conf >> v;
				creditbouncer_symmetric_routing = v;
				std::cerr << "CREDITBOUNCER_SYMMETRIC_ROUTING\t" << creditbouncer_symmetric_routing << "\n";
			} else if (key.compare("CREDITBOUNCER_DEBUG_LOG") == 0) {
				bool v;
				conf >> v;
				creditbouncer_debug_log = v;
				std::cerr << "CREDITBOUNCER_DEBUG_LOG\t\t" << creditbouncer_debug_log << "\n";
			} else if (key.compare("CREDITBOUNCER_DEBUG_MAX_LOGS") == 0) {
				conf >> creditbouncer_debug_max_logs;
				std::cerr << "CREDITBOUNCER_DEBUG_MAX_LOGS\t" << creditbouncer_debug_max_logs << "\n";
			} else if (key.compare("CREDITBOUNCER_ROUTE_LOG") == 0) {
				bool v;
				conf >> v;
				creditbouncer_route_log = v;
				std::cerr << "CREDITBOUNCER_ROUTE_LOG\t\t" << creditbouncer_route_log << "\n";
			} else if (key.compare("CREDITBOUNCER_ROUTE_MAX_LOGS") == 0) {
				conf >> creditbouncer_route_max_logs;
				std::cerr << "CREDITBOUNCER_ROUTE_MAX_LOGS\t" << creditbouncer_route_max_logs << "\n";
			} else if (key.compare("CREDITBOUNCER_SWITCH_CB_ENABLE") == 0) {
				bool v;
				conf >> v;
				creditbouncer_switch_cb_enable = v;
				std::cerr << "CREDITBOUNCER_SWITCH_CB_ENABLE\t" << creditbouncer_switch_cb_enable << "\n";
			} else if (key.compare("CREDITBOUNCER_SWITCH_PHANTOM_THRESHOLD_BYTES") == 0) {
				conf >> creditbouncer_switch_phantom_threshold_bytes;
				uint64_t derived = creditbouncer_switch_phantom_threshold_bytes > 0
					? creditbouncer_switch_phantom_threshold_bytes
					: 1;
				uint64_t derivedXon = derived > 1 ? (derived / 2) : derived;
				if (!creditbouncer_switch_xon_explicit)
					creditbouncer_switch_xon_bytes = derivedXon > 0 ? derivedXon : 1;
				if (!creditbouncer_switch_xoff_explicit)
					creditbouncer_switch_xoff_bytes = derived;
				if (!creditbouncer_switch_xecn_explicit)
					creditbouncer_switch_xecn_bytes = derived;
				std::cerr << "CREDITBOUNCER_SWITCH_PHANTOM_THRESHOLD_BYTES\t" << creditbouncer_switch_phantom_threshold_bytes << "\n";
			} else if (key.compare("CREDITBOUNCER_SWITCH_XON_BYTES") == 0) {
				conf >> creditbouncer_switch_xon_bytes;
				creditbouncer_switch_xon_explicit = true;
				std::cerr << "CREDITBOUNCER_SWITCH_XON_BYTES\t" << creditbouncer_switch_xon_bytes << "\n";
			} else if (key.compare("CREDITBOUNCER_SWITCH_XOFF_BYTES") == 0) {
				conf >> creditbouncer_switch_xoff_bytes;
				creditbouncer_switch_xoff_explicit = true;
				std::cerr << "CREDITBOUNCER_SWITCH_XOFF_BYTES\t" << creditbouncer_switch_xoff_bytes << "\n";
			} else if (key.compare("CREDITBOUNCER_SWITCH_XECN_BYTES") == 0) {
				conf >> creditbouncer_switch_xecn_bytes;
				creditbouncer_switch_xecn_explicit = true;
				std::cerr << "CREDITBOUNCER_SWITCH_XECN_BYTES\t" << creditbouncer_switch_xecn_bytes << "\n";
			} else if (key.compare("CREDITBOUNCER_SWITCH_DYNAMIC_THRESHOLD") == 0) {
				bool v;
				conf >> v;
				creditbouncer_switch_dynamic_threshold = v;
				std::cerr << "CREDITBOUNCER_SWITCH_DYNAMIC_THRESHOLD\t" << creditbouncer_switch_dynamic_threshold << "\n";
			} else if (key.compare("CREDITBOUNCER_SWITCH_DYNAMIC_ALPHA") == 0) {
				conf >> creditbouncer_switch_dynamic_alpha;
				std::cerr << "CREDITBOUNCER_SWITCH_DYNAMIC_ALPHA\t" << creditbouncer_switch_dynamic_alpha << "\n";
			} else if (key.compare("CREDITBOUNCER_SWITCH_DYNAMIC_OFF_DIFF_BYTES") == 0) {
				conf >> creditbouncer_switch_dynamic_off_diff_bytes;
				std::cerr << "CREDITBOUNCER_SWITCH_DYNAMIC_OFF_DIFF_BYTES\t" << creditbouncer_switch_dynamic_off_diff_bytes << "\n";
			} else if (key.compare("CREDITBOUNCER_SWITCH_STATE_LOG") == 0) {
				bool v;
				conf >> v;
				creditbouncer_switch_state_log = v;
				std::cerr << "CREDITBOUNCER_SWITCH_STATE_LOG\t" << creditbouncer_switch_state_log << "\n";
			} else if (key.compare("CREDITBOUNCER_SWITCH_STATE_MAX_LOGS") == 0) {
				conf >> creditbouncer_switch_state_max_logs;
				std::cerr << "CREDITBOUNCER_SWITCH_STATE_MAX_LOGS\t" << creditbouncer_switch_state_max_logs << "\n";
			} else if (key.compare("CREDITBOUNCER_DEDICATED_MMU_ENABLE") == 0) {
				bool v;
				conf >> v;
				creditbouncer_dedicated_mmu_enable = v;
				std::cerr << "CREDITBOUNCER_DEDICATED_MMU_ENABLE\t" << creditbouncer_dedicated_mmu_enable << "\n";
			} else if (key.compare("CREDITBOUNCER_DEDICATED_CREDIT_QUEUE_MULTIPLIER") == 0) {
				conf >> creditbouncer_dedicated_credit_queue_multiplier;
				creditbouncer_dedicated_credit_queue_bytes = static_cast<uint32_t>(
					creditbouncer_dedicated_credit_queue_multiplier *
					kCreditBouncerControlPacketSizeBytes + 0.5);
				std::cerr << "CREDITBOUNCER_DEDICATED_CREDIT_QUEUE_MULTIPLIER\t"
					  << creditbouncer_dedicated_credit_queue_multiplier
					  << " control_pkt_bytes=" << kCreditBouncerControlPacketSizeBytes
					  << " queue_bytes=" << creditbouncer_dedicated_credit_queue_bytes << "\n";
			} else if (key.compare("CREDITBOUNCER_DEDICATED_CREDIT_QUEUE_BYTES") == 0) {
				conf >> creditbouncer_dedicated_credit_queue_bytes;
				creditbouncer_dedicated_credit_queue_multiplier =
					static_cast<double>(creditbouncer_dedicated_credit_queue_bytes) /
					static_cast<double>(kCreditBouncerControlPacketSizeBytes);
				std::cerr << "CREDITBOUNCER_DEDICATED_CREDIT_QUEUE_BYTES\t"
					  << creditbouncer_dedicated_credit_queue_bytes
					  << " control_pkt_bytes=" << kCreditBouncerControlPacketSizeBytes
					  << " multiplier=" << creditbouncer_dedicated_credit_queue_multiplier << "\n";
			} else if (key.compare("CREDITBOUNCER_DEDICATED_MAX_BUFFER_PER_PORT_BYTES") == 0) {
				conf >> creditbouncer_dedicated_max_total_buffer_per_port_bytes;
				std::cerr << "CREDITBOUNCER_DEDICATED_MAX_BUFFER_PER_PORT_BYTES\t" << creditbouncer_dedicated_max_total_buffer_per_port_bytes << "\n";
			} else if (key.compare("CREDITBOUNCER_DEDICATED_DATA_HEADROOM_BYTES") == 0) {
				conf >> creditbouncer_dedicated_data_headroom_bytes;
				std::cerr << "CREDITBOUNCER_DEDICATED_DATA_HEADROOM_BYTES\t" << creditbouncer_dedicated_data_headroom_bytes << "\n";
			} else if (key.compare("CREDITBOUNCER_DEDICATED_DATA_GUARANTEE_BYTES") == 0) {
				conf >> creditbouncer_dedicated_data_guarantee_bytes;
				std::cerr << "CREDITBOUNCER_DEDICATED_DATA_GUARANTEE_BYTES\t" << creditbouncer_dedicated_data_guarantee_bytes << "\n";
			} else if (key.compare("CREDITBOUNCER_DEDICATED_DATA_ECN_KMIN_BYTES") == 0) {
				conf >> creditbouncer_dedicated_data_ecn_kmin_bytes;
				std::cerr << "CREDITBOUNCER_DEDICATED_DATA_ECN_KMIN_BYTES\t" << creditbouncer_dedicated_data_ecn_kmin_bytes << "\n";
			} else if (key.compare("CREDITBOUNCER_DEDICATED_DATA_ECN_KMAX_BYTES") == 0) {
				conf >> creditbouncer_dedicated_data_ecn_kmax_bytes;
				std::cerr << "CREDITBOUNCER_DEDICATED_DATA_ECN_KMAX_BYTES\t" << creditbouncer_dedicated_data_ecn_kmax_bytes << "\n";
			} else if (key.compare("CREDITBOUNCER_DEDICATED_DATA_ECN_PMAX") == 0) {
				conf >> creditbouncer_dedicated_data_ecn_pmax;
				std::cerr << "CREDITBOUNCER_DEDICATED_DATA_ECN_PMAX\t" << creditbouncer_dedicated_data_ecn_pmax << "\n";
			} else if (key.compare("CREDITBOUNCER_DEDICATED_MMU_LOG") == 0) {
				bool v;
				conf >> v;
				creditbouncer_dedicated_mmu_log = v;
				std::cerr << "CREDITBOUNCER_DEDICATED_MMU_LOG\t" << creditbouncer_dedicated_mmu_log << "\n";
			} else if (key.compare("CREDITBOUNCER_DEDICATED_MMU_MAX_LOGS") == 0) {
				conf >> creditbouncer_dedicated_mmu_max_logs;
				std::cerr << "CREDITBOUNCER_DEDICATED_MMU_MAX_LOGS\t" << creditbouncer_dedicated_mmu_max_logs << "\n";
			}

			fflush(stdout);
		}
		conf.close();
        
		if(!load_workload(hpcc_workload.c_str(), &workload_cdf, &avg_flow_size)){
			std::cerr<< "Failed to open workload file " << hpcc_workload.c_str() << std::endl;
			return 1;
		}
	}
	else
	{
		std::cerr << "Error: require a config file\n";
		fflush(stdout);
		return 1;
	}

	if (creditbouncer_grant_sched_policy != "RR" &&
	    creditbouncer_grant_sched_policy != "SRPT") {
		std::cerr << "Error: CREDITBOUNCER_GRANT_SCHED_POLICY must be RR or SRPT, got "
			  << creditbouncer_grant_sched_policy << "\n";
		return 1;
	}

	RdmaHw::CreditBouncerSchedPolicy creditbouncerGrantSchedPolicy =
		(creditbouncer_grant_sched_policy == "SRPT")
			? RdmaHw::CB_SCHED_SRPT
			: RdmaHw::CB_SCHED_RR;


	SeedManager::SetSeed(random_seed);
	g_foreground_flow_ids.clear();
	g_foreground_flow_tuples.clear();
	g_packet_loss_counters = PacketLossCounters();
	bool dynamicth = use_dynamic_pfc_threshold;

	Config::SetDefault("ns3::QbbNetDevice::PauseTime", UintegerValue(pause_time));
	Config::SetDefault("ns3::QbbNetDevice::QcnEnabled", BooleanValue(enable_qcn));
	Config::SetDefault("ns3::QbbNetDevice::DynamicThreshold", BooleanValue(dynamicth));
	Config::SetDefault("ns3::QbbNetDevice::QbbEnabled", BooleanValue(enable_pfc));

	// set int_multi
	IntHop::multi = int_multi;
	// IntHeader::mode
	if (cc_mode == 7) // timely, use ts
		IntHeader::mode = 1;
	else if (cc_mode == 3) // hpcc, use int
		IntHeader::mode = 0;
	else // others, no extra header
		IntHeader::mode = 5;


	std::ifstream topof, tracef; //std::ifstream topof, flowf, tracef;
	topof.open(topology_file.c_str());
	// flowf.open(flow_file.c_str());
	tracef.open(trace_file.c_str());
	uint32_t node_num, switch_num, link_num, flow_num, trace_num;
	topof >> node_num >> switch_num >> link_num;
	// flowf >> flow_num;
	tracef >> trace_num;


	NodeContainer n;
	//n.Create(node_num);
	std::vector<uint32_t> node_type(node_num, 0);
	for (uint32_t i = 0; i < switch_num; i++)
	{
		uint32_t sid;
		topof >> sid;
		node_type[sid] = 1;
	}
	for (uint32_t i = 0; i < node_num; i++){
		if (node_type[i] == 0)
			n.Add(CreateObject<Node>());
		else{
			Ptr<SwitchNode> sw = CreateObject<SwitchNode>();
			n.Add(sw);
			sw->SetAttribute("EcnEnabled", BooleanValue(enable_qcn));
		}
	}


	NS_LOG_INFO("Create nodes.");

	InternetStackHelper internet;
	internet.Install(n);

	//
	// Assign IP to each server
	//
	std::vector<Ipv4Address> serverAddress;
	std::vector<uint32_t> host_ids;
	for (uint32_t i = 0; i < node_num; i++){
		if (n.Get(i)->GetNodeType() == 0){ // is server
			serverAddress.resize(i + 1);
			serverAddress[i] = Ipv4Address(0x0b000001 + ((i / 256) * 0x00010000) + ((i % 256) * 0x00000100));
			host_ids.push_back(i);
		}
	}

	NS_LOG_INFO("Create channels.");

	//
	// Explicitly create the channels required by the topology.
	//

	Ptr<RateErrorModel> rem = CreateObject<RateErrorModel>();
	Ptr<UniformRandomVariable> uv = CreateObject<UniformRandomVariable>();
	rem->SetRandomVariable(uv);
	uv->SetStream(50);
	rem->SetAttribute("ErrorRate", DoubleValue(error_rate_per_link));
	rem->SetAttribute("ErrorUnit", StringValue("ERROR_UNIT_PACKET"));

	FILE *pfc_file = nullptr;
	bool pfc_output_enabled = (pfc_output_file != "/dev/null");
	if (pfc_output_enabled) {
		pfc_file = fopen(pfc_output_file.c_str(), "w");
		NS_ABORT_MSG_IF(pfc_file == nullptr, "Failed to open PFC_OUTPUT_FILE");
		NS_ABORT_MSG_IF(pfc_mon_interval_ns == 0, "PFC_MON_INTERVAL_NS must be > 0");
		fprintf(pfc_file, "time_ns pfc_count\n");
		fflush(pfc_file);
	}

	FILE *pause_intervals_output = nullptr;
	bool pause_intervals_enabled = (!pause_intervals_output_file.empty() &&
		pause_intervals_output_file != "/dev/null");
	if (pause_intervals_enabled) {
		pause_intervals_output = fopen(pause_intervals_output_file.c_str(), "w");
		NS_ABORT_MSG_IF(pause_intervals_output == nullptr, "Failed to open PAUSE_INTERVALS_OUTPUT_FILE");
		fprintf(pause_intervals_output, "switch_id ifindex qindex start_ns end_ns duration_ns\n");
		fflush(pause_intervals_output);
	}

	FILE *bounced_file = nullptr;
	bool bounced_output_enabled = (!bounced_output_file.empty() &&
		bounced_output_file != "/dev/null" &&
		creditbouncer_dedicated_mmu_enable);
	if (bounced_output_enabled) {
		bounced_file = fopen(bounced_output_file.c_str(), "w");
		NS_ABORT_MSG_IF(bounced_file == nullptr, "Failed to open BOUNCED_OUTPUT_FILE");
		NS_ABORT_MSG_IF(bounced_mon_interval_ns == 0, "BOUNCED_MON_INTERVAL_NS must be > 0");
		fprintf(bounced_file, "time_ns bounced_count\n");
		fflush(bounced_file);
	} else if (!bounced_output_file.empty() && bounced_output_file != "/dev/null" &&
		!creditbouncer_dedicated_mmu_enable) {
		std::cerr << "BOUNCED_OUTPUT_FILE ignored because CREDITBOUNCER_DEDICATED_MMU_ENABLE is 0\n";
	}

	FILE *xoff_intervals_output = nullptr;
	bool xoff_intervals_enabled = (!xoff_intervals_output_file.empty() &&
		xoff_intervals_output_file != "/dev/null");
	if (xoff_intervals_enabled) {
		xoff_intervals_output = fopen(xoff_intervals_output_file.c_str(), "w");
		NS_ABORT_MSG_IF(xoff_intervals_output == nullptr, "Failed to open XOFF_INTERVALS_OUTPUT_FILE");
		fprintf(xoff_intervals_output, "switch_id out_dev start_ns end_ns duration_ns\n");
		fflush(xoff_intervals_output);
	}

	FILE *packet_loss_output = nullptr;
	bool packet_loss_output_enabled = (!packet_loss_output_file.empty() &&
		packet_loss_output_file != "/dev/null");
	if (packet_loss_output_enabled) {
		packet_loss_output = fopen(packet_loss_output_file.c_str(), "w");
		NS_ABORT_MSG_IF(packet_loss_output == nullptr, "Failed to open PACKET_LOSS_OUTPUT_FILE");
		fprintf(packet_loss_output,
			"time_ns fg_credit_loss fg_credit_total fg_data_loss fg_data_total bg_credit_loss bg_credit_total bg_data_loss bg_data_total\n");
		fflush(packet_loss_output);
		NS_ABORT_MSG_IF(timeout_mon_interval_ns == 0, "TIMEOUT_MON_INTERVAL_NS must be > 0 for PACKET_LOSS_OUTPUT_FILE");
		Simulator::Schedule(NanoSeconds(timeout_mon_interval_ns), &monitor_packet_loss_count, packet_loss_output);
	}

	QbbHelper qbb;
	Ipv4AddressHelper ipv4;
	std::unordered_map<uint32_t, uint32_t> host_to_leaf;
	std::unordered_map<uint32_t, std::vector<uint32_t> > leaf_to_hosts;
	auto bind_host_leaf = [&](uint32_t host_id, uint32_t leaf_id) {
		auto it = host_to_leaf.find(host_id);
		if (it == host_to_leaf.end()) {
			host_to_leaf[host_id] = leaf_id;
			leaf_to_hosts[leaf_id].push_back(host_id);
			return;
		}
		NS_ABORT_MSG_IF(it->second != leaf_id,
			"Host " << host_id << " connected to multiple leaves: " << it->second << " and " << leaf_id);
	};
	for (uint32_t i = 0; i < link_num; i++)
	{
		uint32_t src, dst;
		std::string data_rate, link_delay;
		double error_rate;
		topof >> src >> dst >> data_rate >> link_delay >> error_rate;

		Ptr<Node> snode = n.Get(src), dnode = n.Get(dst);
		if (snode->GetNodeType() == 0 && dnode->GetNodeType() == 1) {
			bind_host_leaf(src, dst);
		} else if (snode->GetNodeType() == 1 && dnode->GetNodeType() == 0) {
			bind_host_leaf(dst, src);
		}

		qbb.SetDeviceAttribute("DataRate", StringValue(data_rate));
		qbb.SetChannelAttribute("Delay", StringValue(link_delay));

		if (error_rate > 0)
		{
			Ptr<RateErrorModel> rem = CreateObject<RateErrorModel>();
			Ptr<UniformRandomVariable> uv = CreateObject<UniformRandomVariable>();
			rem->SetRandomVariable(uv);
			uv->SetStream(50);
			rem->SetAttribute("ErrorRate", DoubleValue(error_rate));
			rem->SetAttribute("ErrorUnit", StringValue("ERROR_UNIT_PACKET"));
			qbb.SetDeviceAttribute("ReceiveErrorModel", PointerValue(rem));
		}
		else
		{
			qbb.SetDeviceAttribute("ReceiveErrorModel", PointerValue(rem));
		}

		fflush(stdout);

		// Assigne server IP
		// Note: this should be before the automatic assignment below (ipv4.Assign(d)),
		// because we want our IP to be the primary IP (first in the IP address list),
		// so that the global routing is based on our IP
		NetDeviceContainer d = qbb.Install(snode, dnode);
		if (snode->GetNodeType() == 0){
			Ptr<Ipv4> ipv4 = snode->GetObject<Ipv4>();
			ipv4->AddInterface(d.Get(0));
			ipv4->AddAddress(1, Ipv4InterfaceAddress(serverAddress[src], Ipv4Mask(0xff000000)));
		}
		if (dnode->GetNodeType() == 0){
			Ptr<Ipv4> ipv4 = dnode->GetObject<Ipv4>();
			ipv4->AddInterface(d.Get(1));
			ipv4->AddAddress(1, Ipv4InterfaceAddress(serverAddress[dst], Ipv4Mask(0xff000000)));
		}

		// used to create a graph of the topology
		nbr2if[snode][dnode].idx = DynamicCast<QbbNetDevice>(d.Get(0))->GetIfIndex();
		nbr2if[snode][dnode].up = true;
		nbr2if[snode][dnode].delay = DynamicCast<QbbChannel>(DynamicCast<QbbNetDevice>(d.Get(0))->GetChannel())->GetDelay().GetTimeStep();
		nbr2if[snode][dnode].bw = DynamicCast<QbbNetDevice>(d.Get(0))->GetDataRate().GetBitRate();
		nbr2if[dnode][snode].idx = DynamicCast<QbbNetDevice>(d.Get(1))->GetIfIndex();
		nbr2if[dnode][snode].up = true;
		nbr2if[dnode][snode].delay = DynamicCast<QbbChannel>(DynamicCast<QbbNetDevice>(d.Get(1))->GetChannel())->GetDelay().GetTimeStep();
		nbr2if[dnode][snode].bw = DynamicCast<QbbNetDevice>(d.Get(1))->GetDataRate().GetBitRate();

		// This is just to set up the connectivity between nodes. The IP addresses are useless
		char ipstring[16];
		sprintf(ipstring, "10.%d.%d.0", i / 254 + 1, i % 254 + 1);
		ipv4.SetBase(ipstring, "255.255.255.0");
		ipv4.Assign(d);

		// setup PFC trace
		if (pfc_output_enabled) {
			DynamicCast<QbbNetDevice>(d.Get(0))->TraceConnectWithoutContext("QbbPfc", MakeBoundCallback (&get_pfc, pfc_file, DynamicCast<QbbNetDevice>(d.Get(0))));
			DynamicCast<QbbNetDevice>(d.Get(1))->TraceConnectWithoutContext("QbbPfc", MakeBoundCallback (&get_pfc, pfc_file, DynamicCast<QbbNetDevice>(d.Get(1))));
		}
		if (pause_intervals_enabled) {
			DynamicCast<QbbNetDevice>(d.Get(0))->TraceConnectWithoutContext("QbbPfcState",
				MakeBoundCallback(&record_pause_interval, pause_intervals_output, DynamicCast<QbbNetDevice>(d.Get(0))));
			DynamicCast<QbbNetDevice>(d.Get(1))->TraceConnectWithoutContext("QbbPfcState",
				MakeBoundCallback(&record_pause_interval, pause_intervals_output, DynamicCast<QbbNetDevice>(d.Get(1))));
		}
	}
	if (pfc_output_enabled)
		Simulator::Schedule(NanoSeconds(pfc_mon_interval_ns), &monitor_pfc_count, pfc_file);
	if (bounced_output_enabled)
		Simulator::Schedule(NanoSeconds(bounced_mon_interval_ns), &monitor_bounced_count, bounced_file);

	std::vector<uint32_t> leaf_ids;
	std::unordered_map<uint32_t, uint32_t> host_to_index_in_leaf;
	leaf_ids.reserve(leaf_to_hosts.size());
	uint32_t hosts_per_leaf = 0;
	for (auto &kv : leaf_to_hosts) {
		auto &hosts = kv.second;
		std::sort(hosts.begin(), hosts.end());
		leaf_ids.push_back(kv.first);
		if (hosts_per_leaf == 0) {
			hosts_per_leaf = hosts.size();
		} else {
			NS_ABORT_MSG_IF(hosts.size() != hosts_per_leaf,
				"Leaves have different host counts, cannot use same-index cross-leaf traffic pattern");
		}
		for (uint32_t idx = 0; idx < hosts.size(); ++idx) {
			host_to_index_in_leaf[hosts[idx]] = idx;
		}
	}
	std::sort(leaf_ids.begin(), leaf_ids.end());
	NS_ABORT_MSG_IF(leaf_ids.size() < 2, "Need at least two leaves for cross-leaf traffic pattern");
	for (uint32_t host_id : host_ids) {
		NS_ABORT_MSG_IF(host_to_leaf.find(host_id) == host_to_leaf.end(),
			"Host " << host_id << " is not connected to any leaf");
		NS_ABORT_MSG_IF(host_to_index_in_leaf.find(host_id) == host_to_index_in_leaf.end(),
			"Host " << host_id << " has no index-in-leaf mapping");
	}

	nic_rate = get_nic_rate(n);

	// config switch
	for (uint32_t i = 0; i < node_num; i++){
		if (n.Get(i)->GetNodeType() == 1){ // is switch
			Ptr<SwitchNode> sw = DynamicCast<SwitchNode>(n.Get(i));
			uint32_t shift = 3; // by default 1/8
			for (uint32_t j = 1; j < sw->GetNDevices(); j++){
				Ptr<QbbNetDevice> dev = DynamicCast<QbbNetDevice>(sw->GetDevice(j));
				// set ecn
				uint64_t rate = dev->GetDataRate().GetBitRate();
				NS_ASSERT_MSG(rate2kmin.find(rate) != rate2kmin.end(), "must set kmin for each link speed");
				NS_ASSERT_MSG(rate2kmax.find(rate) != rate2kmax.end(), "must set kmax for each link speed");
				NS_ASSERT_MSG(rate2pmax.find(rate) != rate2pmax.end(), "must set pmax for each link speed");
				sw->m_mmu->ConfigEcn(j, rate2kmin[rate], rate2kmax[rate], rate2pmax[rate]);
				// set pfc
				uint64_t delay = DynamicCast<QbbChannel>(dev->GetChannel())->GetDelay().GetTimeStep();
				uint32_t headroom = rate * delay / 8 / 1000000000 * 2 + 2 * sw->m_mmu->MTU;
				sw->m_mmu->ConfigHdrm(j, headroom);
			}
			sw->m_mmu->ConfigNPort(sw->GetNDevices()-1);
			sw->m_mmu->SetAttribute("MaxTotalBufferPerPort",
				UintegerValue(switch_shared_buffer_per_port_bytes));
			sw->m_mmu->ConfigBufferSize(buffer_size* 1024 * 1024);
			sw->m_mmu->node_id = sw->GetId();
			sw->m_mmu->SetAttribute("MaxBytesTltUip", DoubleValue(enable_tlt ? tlt_maxbytes_uip : 0));
			sw->m_mmu->SetAttribute("TltEnable", BooleanValue(enable_tlt));
			if (bounced_output_enabled) {
				sw->TraceConnectWithoutContext("CreditBouncerBounce",
					MakeBoundCallback(&get_bounced_credit, bounced_file));
			}
			if (xoff_intervals_enabled) {
				sw->TraceConnectWithoutContext("CreditBouncerFcState",
					MakeBoundCallback(&record_xoff_interval, xoff_intervals_output, sw));
			}
			if (packet_loss_output_enabled) {
				sw->TraceConnectWithoutContext("CreditBouncerPacketEvent",
					MakeBoundCallback(&on_cb_packet_event, packet_loss_output));
			}
			fprintf(stderr, "Node %u : Broadcom switch (%u ports / %gMB MMU)\n", i, sw->GetNDevices() - 1, sw->m_mmu->GetMmuBufferBytes() / 1000000.);
		}
	}

	#if ENABLE_QP
	FILE *fct_output = fopen(fct_output_file.c_str(), "w");
	FILE *timeout_output = fopen(timeout_output_file.c_str(), "w");
	NS_ABORT_MSG_IF(timeout_output == nullptr, "Failed to open TIMEOUT_OUTPUT_FILE");
	NS_ABORT_MSG_IF(timeout_mon_interval_ns == 0, "TIMEOUT_MON_INTERVAL_NS must be > 0");
	fprintf(timeout_output, "time_ns timeout_count\n");
	fflush(timeout_output);
	Simulator::Schedule(NanoSeconds(timeout_mon_interval_ns), &monitor_timeout_count, timeout_output);
	//
	// install RDMA driver
	//
	for (uint32_t i = 0; i < node_num; i++){
		if (n.Get(i)->GetNodeType() == 0){ // is server
			// create RdmaHw
			Ptr<RdmaHw> rdmaHw = CreateObject<RdmaHw>();
			rdmaHw->SetAttribute("ClampTargetRate", BooleanValue(clamp_target_rate));
			rdmaHw->SetAttribute("AlphaResumInterval", DoubleValue(alpha_resume_interval));
			rdmaHw->SetAttribute("RPTimer", DoubleValue(rp_timer));
			rdmaHw->SetAttribute("L2Timeout", TimeValue(NanoSeconds(l2_timeout_ns)));
			rdmaHw->SetAttribute("RPByteReset", UintegerValue(rp_byte_reset));
			rdmaHw->SetAttribute("FastRecoveryTimes", UintegerValue(fast_recovery_times));
			rdmaHw->SetAttribute("EwmaGain", DoubleValue(ewma_gain));
			rdmaHw->SetAttribute("RateAI", DataRateValue(DataRate(rate_ai)));
			rdmaHw->SetAttribute("RateHAI", DataRateValue(DataRate(rate_hai)));
			rdmaHw->SetAttribute("L2BackToZero", BooleanValue(l2_back_to_zero));
			rdmaHw->SetAttribute("L2ChunkSize", UintegerValue(l2_chunk_size));
			rdmaHw->SetAttribute("L2AckInterval", UintegerValue(l2_ack_interval));
			rdmaHw->SetAttribute("CcMode", UintegerValue(cc_mode));
			rdmaHw->SetAttribute("RateDecreaseInterval", DoubleValue(rate_decrease_interval));
			rdmaHw->SetAttribute("MinRate", DataRateValue(DataRate(min_rate)));
			rdmaHw->SetAttribute("Mtu", UintegerValue(packet_payload_size));
			rdmaHw->SetAttribute("MiThresh", UintegerValue(mi_thresh));
			rdmaHw->SetAttribute("VarWin", BooleanValue(var_win));
			rdmaHw->SetAttribute("FastReact", BooleanValue(fast_react));
			rdmaHw->SetAttribute("MultiRate", BooleanValue(multi_rate));
			rdmaHw->SetAttribute("SampleFeedback", BooleanValue(sample_feedback));
			rdmaHw->SetAttribute("TargetUtil", DoubleValue(u_target));
			rdmaHw->SetAttribute("RateBound", BooleanValue(rate_bound));
			rdmaHw->SetAttribute("DctcpRateAI", DataRateValue(DataRate(dctcp_rate_ai)));
			rdmaHw->SetAttribute("IrnEnable", BooleanValue(enable_irn));
			if (enable_irn && (cc_mode == 3 || irn_no_bdpfc)) {
				rdmaHw->SetAttribute("IrnRtoHigh", TimeValue(MicroSeconds(4000)));
				rdmaHw->SetAttribute("IrnRtoLow", TimeValue(MicroSeconds(4000)));
				rdmaHw->SetAttribute("IrnBdp", UintegerValue(irn_bdp_bytes > 0 ? irn_bdp_bytes : 100000000));  // no-bdpfc mode default
			} else {
				rdmaHw->SetAttribute("IrnRtoHigh", TimeValue(MicroSeconds(1930)));
				rdmaHw->SetAttribute("IrnRtoLow", TimeValue(MicroSeconds(454)));
				rdmaHw->SetAttribute("IrnBdp", UintegerValue(irn_bdp_bytes > 0 ? irn_bdp_bytes : 18750));  // legacy default
			}
			rdmaHw->SetAttribute("TltEnable", BooleanValue(enable_tlt));
			rdmaHw->SetAttribute("CreditBouncerEnable", BooleanValue(enable_creditbouncer));
			rdmaHw->SetAttribute("CreditBouncerCcEnable", BooleanValue(enable_creditbouncer && creditbouncer_cc_enable));
			rdmaHw->SetAttribute("CreditBouncerCsnEnable", BooleanValue(enable_creditbouncer && creditbouncer_csn_enable));
			rdmaHw->SetAttribute("CreditBouncerUnsolicitedThresholdBytes", UintegerValue(creditbouncer_unsch_threshold_bytes));
			rdmaHw->SetAttribute("CreditBouncerGlobalBucketBytes", UintegerValue(creditbouncer_global_bucket_bytes));
			rdmaHw->SetAttribute("CreditBouncerMaxSrpbBytes", UintegerValue(creditbouncer_maxSrpb_bytes));
			rdmaHw->SetAttribute("CreditBouncerSenderThresholdBytes", UintegerValue(creditbouncer_sender_threshold_bytes));
				rdmaHw->SetAttribute("CreditBouncerGrantGranularityBytes", UintegerValue(creditbouncer_grant_granularity_bytes));
				rdmaHw->SetAttribute("CreditBouncerAiStepBytes", DoubleValue(creditbouncer_ai_step_bytes));
				rdmaHw->SetAttribute("CreditBouncerMdFactor", DoubleValue(creditbouncer_md_factor));
			rdmaHw->SetAttribute("CreditBouncerCeNewWeight", DoubleValue(creditbouncer_ce_new_weight));
			rdmaHw->SetAttribute("CreditBouncerGrantIntervalNs", UintegerValue(creditbouncer_grant_interval_ns));
			rdmaHw->SetAttribute("CreditBouncerResendIntervalNs", UintegerValue(creditbouncer_resend_interval_ns));
			rdmaHw->SetAttribute("CreditBouncerGrantSchedPolicy", EnumValue(creditbouncerGrantSchedPolicy));
			rdmaHw->SetAttribute("CreditBouncerSymmetricRouting", BooleanValue(creditbouncer_symmetric_routing));
			rdmaHw->SetAttribute("CreditBouncerDebugLog", BooleanValue(creditbouncer_debug_log));
			rdmaHw->SetAttribute("CreditBouncerDebugMaxLogs", UintegerValue(creditbouncer_debug_max_logs));
			
			Simulator::Schedule(Seconds(simulator_stop_time - 0.01), &RdmaHw::PrintStat, rdmaHw);

			// create and install RdmaDriver
			Ptr<RdmaDriver> rdma = CreateObject<RdmaDriver>();
			Ptr<Node> node = n.Get(i);
			rdma->SetNode(node);
			rdma->SetRdmaHw(rdmaHw);

			node->AggregateObject (rdma);
			rdma->Init();
			rdma->TraceConnectWithoutContext("QpComplete", MakeBoundCallback (qp_finish, fct_output));
			rdmaHw->TraceConnectWithoutContext("TimeoutEvent", MakeBoundCallback(get_timeout, timeout_output));
			rdmaHw->TraceConnectWithoutContext("CreditBouncerResendTimeoutEvent",
				MakeBoundCallback(get_creditbouncer_timeout, timeout_output));
		}
	}
	#endif

	// set ACK priority on hosts
	if (ack_high_prio)
		RdmaEgressQueue::ack_q_idx = 0;
	else
		RdmaEgressQueue::ack_q_idx = 3;

	//
	// setup switch CC
	//
		const bool use_dedicated_fc_formula =
			enable_creditbouncer && creditbouncer_dedicated_mmu_enable &&
			creditbouncer_switch_dynamic_threshold;
		uint64_t effective_cb_xoff = std::max<uint64_t>(1, creditbouncer_switch_xoff_bytes);
		uint64_t effective_cb_xon = creditbouncer_switch_xon_bytes;
		if (effective_cb_xon == 0)
			effective_cb_xon = std::max<uint64_t>(1, effective_cb_xoff / 2);
		if (effective_cb_xon >= effective_cb_xoff)
			effective_cb_xon = effective_cb_xoff > 1 ? (effective_cb_xoff - 1) : 1;
		uint64_t effective_cb_xecn = creditbouncer_switch_xecn_bytes == 0
			? effective_cb_xoff
			: creditbouncer_switch_xecn_bytes;
		if (use_dedicated_fc_formula) {
			std::cerr << "CREDITBOUNCER_SWITCH_FC_FORMULA\t"
				<< "DEDICATED_MMU max(qd+qc,qp), Xoff=qd+qc+alpha*(Bs-sumQd)-3072, Xon=Xoff-3072\n";
			std::cerr << "CREDITBOUNCER_SWITCH_DYNAMIC_ALPHA\t"
				<< creditbouncer_switch_dynamic_alpha << " (used by dedicated FC formula)\n";
			std::cerr << "CREDITBOUNCER_DEDICATED_CREDIT_QUEUE_BYTES\t"
				<< creditbouncer_dedicated_credit_queue_bytes << " (used as fixed Qc.size reserve)\n";
			std::cerr << "CREDITBOUNCER_SWITCH_XON_BYTES\tignored_by_dedicated_fc_formula\n";
			std::cerr << "CREDITBOUNCER_SWITCH_XOFF_BYTES\tignored_by_dedicated_fc_formula\n";
			std::cerr << "CREDITBOUNCER_SWITCH_PHANTOM_THRESHOLD_BYTES\tignored_by_dedicated_fc_formula\n";
			std::cerr << "CREDITBOUNCER_SWITCH_DYNAMIC_OFF_DIFF_BYTES\tignored_by_dedicated_fc_formula\n";
		}

		for (uint32_t i = 0; i < node_num; i++){
			if (n.Get(i)->GetNodeType() == 1){ // switch
				Ptr<SwitchNode> sw = DynamicCast<SwitchNode>(n.Get(i));
				sw->SetAttribute("CcMode", UintegerValue(cc_mode));
				sw->SetAttribute("CreditBouncerSymmetricRouting", BooleanValue(creditbouncer_symmetric_routing));
				sw->SetAttribute("CreditBouncerRouteLog", BooleanValue(creditbouncer_route_log));
				sw->SetAttribute("CreditBouncerRouteMaxLogs", UintegerValue(creditbouncer_route_max_logs));
				sw->SetAttribute("CreditBouncerCBEnable", BooleanValue(enable_creditbouncer && creditbouncer_switch_cb_enable));
				sw->SetAttribute("CreditBouncerCcEnable", BooleanValue(enable_creditbouncer && creditbouncer_cc_enable));
				sw->SetAttribute("CreditBouncerFcEnable", BooleanValue(enable_creditbouncer && creditbouncer_fc_enable));
				sw->SetAttribute("CreditBouncerXonBytes", UintegerValue(effective_cb_xon));
				sw->SetAttribute("CreditBouncerXoffBytes", UintegerValue(effective_cb_xoff));
				sw->SetAttribute("CreditBouncerXecnBytes", UintegerValue(effective_cb_xecn));
				sw->SetAttribute("CreditBouncerDynamicThreshold", BooleanValue(creditbouncer_switch_dynamic_threshold));
				sw->SetAttribute("CreditBouncerDynamicAlpha", DoubleValue(creditbouncer_switch_dynamic_alpha));
				sw->SetAttribute("CreditBouncerDynamicOffDiffBytes", UintegerValue(creditbouncer_switch_dynamic_off_diff_bytes));
				sw->SetAttribute("CreditBouncerStateLog", BooleanValue(creditbouncer_switch_state_log));
				sw->SetAttribute("CreditBouncerStateMaxLogs", UintegerValue(creditbouncer_switch_state_max_logs));
				sw->SetAttribute("CreditBouncerDedicatedMmuEnable", BooleanValue(enable_creditbouncer && creditbouncer_dedicated_mmu_enable));
				sw->SetAttribute("CreditBouncerDedicatedCreditQueueBytes", UintegerValue(creditbouncer_dedicated_credit_queue_bytes));
				sw->SetAttribute("CreditBouncerDedicatedDataHeadroomBytes", UintegerValue(creditbouncer_dedicated_data_headroom_bytes));
				sw->SetAttribute("CreditBouncerDedicatedDataGuaranteeBytes", UintegerValue(creditbouncer_dedicated_data_guarantee_bytes));
				sw->SetAttribute("CreditBouncerDedicatedDataEcnKminBytes", UintegerValue(creditbouncer_dedicated_data_ecn_kmin_bytes));
				sw->SetAttribute("CreditBouncerDedicatedDataEcnKmaxBytes", UintegerValue(creditbouncer_dedicated_data_ecn_kmax_bytes));
				sw->SetAttribute("CreditBouncerDedicatedDataEcnPmax", DoubleValue(creditbouncer_dedicated_data_ecn_pmax));
				if (sw->m_cbMmu != 0) {
					sw->m_cbMmu->SetAttribute("NodeId", UintegerValue(sw->GetId()));
					sw->m_cbMmu->SetAttribute("MaxTotalBufferPerPort",
						UintegerValue(creditbouncer_dedicated_max_total_buffer_per_port_bytes));
					sw->m_cbMmu->SetAttribute("DebugLog", BooleanValue(creditbouncer_dedicated_mmu_log));
					sw->m_cbMmu->SetAttribute("DebugMaxLogs", UintegerValue(creditbouncer_dedicated_mmu_max_logs));
				}
			}
		}

	// setup routing
	CalculateRoutes(n);
	SetRoutingEntries();

	//
	// get BDP and delay
	//
	maxRtt = maxBdp = 0;
	for (uint32_t i = 0; i < node_num; i++){
		if (n.Get(i)->GetNodeType() != 0)
			continue;
		for (uint32_t j = i+1; j < node_num; j++){
			if (n.Get(j)->GetNodeType() != 0)
				continue;
			uint64_t delay = pairDelay[n.Get(i)][n.Get(j)];
			uint64_t txDelay = pairTxDelay[n.Get(i)][n.Get(j)];
			// Topology-level RTT/BDP uses propagation-only RTT.
			// This matches topology96 assumptions: RTT=40us, BDP=200kB at 40Gbps.
			// Keep txDelay available for legacy accounting and debug visibility.
			uint64_t rtt = delay * 2;
			uint64_t bw = pairBw[n.Get(i)][n.Get(j)];
			uint64_t bdp = rtt * bw / 1000000000/8; 
			pairBdp[n.Get(i)][n.Get(j)] = bdp;
			pairRtt[n.Get(i)][n.Get(j)] = rtt;
			if (bdp > maxBdp)
				maxBdp = bdp;
			if (rtt > maxRtt)
				maxRtt = rtt;
		}
	}
	fprintf(stderr, "maxRtt: %lu, maxBdp: %lu\n", maxRtt, maxBdp);

	//
	// add trace
	//

	NodeContainer trace_nodes;
	for (uint32_t i = 0; i < trace_num; i++)
	{
		uint32_t nid;
		tracef >> nid;
		if (nid >= n.GetN()){
			continue;
		}
		trace_nodes = NodeContainer(trace_nodes, n.Get(nid));
	}

	FILE *trace_output = fopen(trace_output_file.c_str(), "w");
	if (enable_trace)
		qbb.EnableTracing(trace_output, trace_nodes);
    if (!trace_output)
        perror("fopen");
	// dump link speed to trace file
	{
		SimSetting sim_setting;
		for (auto i: nbr2if){
			for (auto j : i.second){
				uint16_t node = i.first->GetId();
				uint8_t intf = j.second.idx;
				uint64_t bps = DynamicCast<QbbNetDevice>(i.first->GetDevice(j.second.idx))->GetDataRate().GetBitRate();
				sim_setting.port_speed[node][intf] = bps;
			}
		}
		sim_setting.win = maxBdp;
		sim_setting.Serialize(trace_output);
	}

	Ipv4GlobalRoutingHelper::PopulateRoutingTables();

	NS_LOG_INFO("Create Applications.");

	uint32_t packetSize = packet_payload_size;
	Ptr<ExponentialRandomVariable> exp_rv = CreateObject<ExponentialRandomVariable> ();
	Ptr<UniformRandomVariable> uniform_rv = CreateObject<UniformRandomVariable> ();
	exp_rv->SetStream(0); // deterministic
	uniform_rv->SetStream(0); // deterministic

	const double LINK_RATE_IN_GBPS = static_cast<double>(nic_rate) / 1e9;
	const double oversubscription_ratio = 2.0; // 4 uplinks, 8 downlinks
	uint32_t MTU_SIZE = packetSize + 48; // Updated to fit RDMA
	uint32_t MSS_SIZE = packetSize; // Updated to fit RDMA
	std::cerr << "Link Rate : " << LINK_RATE_IN_GBPS << "Gbps, Load : " << load
			  << ", AvgFlowSize: " << avg_flow_size
			  << ", workload: " << hpcc_workload << std::endl;
	int host_num = node_num - switch_num;

	double lambda = (double)LINK_RATE_IN_GBPS * 1e9 * load /
		(8. * avg_flow_size * ((double)MTU_SIZE) / MSS_SIZE) /
		oversubscription_ratio * host_num;
	std::cerr << "Background Lambda : " << lambda << " Hz" << std::endl;
	double next_bg_flow_time = app_start_time + 0.01;

	uint32_t flow_id = 0;
	uint64_t generated_bg_flows = 0;
	std::unordered_map<uint32_t, uint32_t> flows_per_host;

	// maintain port number for each host
	std::unordered_map<uint32_t, uint16_t> portNumder;
	std::unordered_map<uint32_t, uint16_t> dportNumder;
	for (uint32_t i = 0; i < node_num; i++) {
		if (n.Get(i)->GetNodeType() == 0) {
			portNumder[i] = 10000; // each host uses source ports starting from 10000
			dportNumder[i] = 100;
		}
	}

	auto install_flow = [&](uint32_t src, uint32_t dst, uint32_t flow_size,
			Time server_start, Time server_stop,
			Time client_start, Time client_stop,
			bool is_foreground) {
		NS_ASSERT(n.Get(src)->GetNodeType() == 0 && n.Get(dst)->GetNodeType() == 0);
		int32_t current_flow_id = static_cast<int32_t>(flow_id++);
		if (is_foreground)
			g_foreground_flow_ids.insert(current_flow_id);

		if (!MAP_KEY_EXISTS(flows_per_host, src))
			flows_per_host[src] = 0;
		flows_per_host[src]++;
		if (!MAP_KEY_EXISTS(flows_per_host, dst))
			flows_per_host[dst] = 0;
		flows_per_host[dst]++;

		uint16_t dport = dportNumder[dst]++;
		UdpServerHelper server0(dport);
		server0.SetAttribute("FlowSize", UintegerValue(flow_size));
		server0.SetAttribute("irn", BooleanValue(enable_irn));
		server0.SetAttribute("StatHostSrc", UintegerValue(src));
		server0.SetAttribute("StatHostDst", UintegerValue(dst));
		server0.SetAttribute("StatRxLen", UintegerValue(flow_size));
		server0.SetAttribute("StatFlowID", UintegerValue(current_flow_id));
		server0.SetAttribute("Port", UintegerValue(dport));

		ApplicationContainer apps0s = server0.Install(n.Get(dst));
		apps0s.Start(server_start);
		apps0s.Stop(server_stop);

		uint16_t sport = portNumder[src]++;
		if (is_foreground) {
			track_foreground_tuple(serverAddress[src].Get(), serverAddress[dst].Get(), sport, dport);
		}
		RdmaClientHelper clientHelper(
			3, serverAddress[src], serverAddress[dst], sport, dport, flow_size,
			has_win ? (global_t == 1 ? maxBdp : pairBdp[n.Get(src)][n.Get(dst)]) : 0,
			global_t == 1 ? maxRtt : pairRtt[n.Get(src)][n.Get(dst)]);
		clientHelper.SetAttribute("StatFlowID", IntegerValue(current_flow_id));

		ApplicationContainer appCon = clientHelper.Install(n.Get(src));
		appCon.Start(client_start);
		appCon.Stop(client_stop);
	};

	if (bg_flow_gen_mode != "count" && bg_flow_gen_mode != "load") {
		NS_ABORT_MSG("Invalid BG_FLOW_GEN_MODE: " << bg_flow_gen_mode << " (must be count or load)");
	}
	if (num_bg_flows < 0) {
		NS_ABORT_MSG("NUM_BG_FLOWS must be >= 0, got " << num_bg_flows);
	}
	if (lambda <= 0) {
		if (bg_flow_gen_mode == "load" || num_bg_flows > 0) {
			NS_ABORT_MSG("Background lambda must be > 0 when generating background flows. Check LOAD/mean_req_size/app settings.");
		}
	}

	auto generate_one_background_flow = [&](double now) {
		(void)now;
		uint32_t send_host_idx = static_cast<uint32_t>(uniform_rv->GetInteger(0, host_ids.size() - 1));
		uint32_t src = host_ids[send_host_idx];
		uint32_t src_leaf = host_to_leaf[src];
		uint32_t src_index = host_to_index_in_leaf[src];
		std::vector<uint32_t> candidate_dst_leaves;
		candidate_dst_leaves.reserve(leaf_ids.size());
		for (uint32_t leaf_id : leaf_ids) {
			if (leaf_id == src_leaf)
				continue;
			auto it = leaf_to_hosts.find(leaf_id);
			if (it != leaf_to_hosts.end() && src_index < it->second.size()) {
				candidate_dst_leaves.push_back(leaf_id);
			}
		}
		NS_ABORT_MSG_IF(candidate_dst_leaves.empty(),
			"No valid destination leaf for host " << src << " (leaf=" << src_leaf << ", index=" << src_index << ")");
		uint32_t dst_leaf_pick = static_cast<uint32_t>(uniform_rv->GetInteger(0, candidate_dst_leaves.size() - 1));
		uint32_t dst_leaf = candidate_dst_leaves[dst_leaf_pick];
		uint32_t dst = leaf_to_hosts[dst_leaf][src_index];

		uint32_t target_len = sample_workload_size(workload_cdf, uniform_rv->GetValue());
		if (target_len == 0)
			target_len = 1;

		double remaining_until_stop = app_stop_time - Simulator::Now().GetSeconds();
		NS_ABORT_MSG_IF(remaining_until_stop <= 0,
			"Background flow created at/after APP_STOP_TIME, this should not happen");
		install_flow(src, dst, target_len,
			Seconds(0), Seconds(remaining_until_stop),
			Seconds(0), Seconds(remaining_until_stop),
			false);
		generated_bg_flows++;
	};

	if (lambda > 0) {
		exp_rv->SetAttribute("Mean", DoubleValue(1. / lambda));
	}
	int64_t remaining_bg_flows = num_bg_flows;
	std::function<void()> schedule_background_flow;
	schedule_background_flow = [&]() {
		if (bg_flow_gen_mode == "count") {
			if (remaining_bg_flows <= 0) {
				return;
			}
		} else {
			if (next_bg_flow_time >= app_stop_time) {
				return;
			}
		}

		double now = next_bg_flow_time;
		generate_one_background_flow(now);

		if (bg_flow_gen_mode == "count") {
			remaining_bg_flows--;
		}
		double inter_arrival = exp_rv->GetValue();
		next_bg_flow_time += inter_arrival;
		Simulator::Schedule(Seconds(inter_arrival), &invoke_std_function, &schedule_background_flow);
	};
	if ((bg_flow_gen_mode == "count" && num_bg_flows > 0) ||
		(bg_flow_gen_mode == "load" && app_start_time + 0.01 < app_stop_time)) {
		Simulator::Schedule(Seconds(next_bg_flow_time), &invoke_std_function, &schedule_background_flow);
	}

	std::function<void()> report_background_generation = [&]() {
		last_background_flow_time = next_bg_flow_time;
		std::cerr << "Background mode=" << bg_flow_gen_mode
				  << ", generated_bg_flows=" << generated_bg_flows
				  << ", background flow ends at " << last_background_flow_time << std::endl;
	};
	Simulator::Schedule(Seconds(std::min(app_stop_time, simulator_stop_time)),
		&invoke_std_function, &report_background_generation);

	if (enable_foreground_incast) {
		NS_ABORT_MSG_IF(foreground_incast_start_time < app_start_time ||
			foreground_incast_start_time >= app_stop_time,
			"FOREGROUND_INCAST_START_TIME must be within [APP_START_TIME, APP_STOP_TIME)");
		const uint32_t kFixedSourceLeafCount = 11;
		const uint32_t kFixedSendersPerLeaf = 4;
		const uint32_t kFixedReceiverLeaf = 10;
		const double kForegroundRoundIntervalSec = 0.01;
		const double kForegroundLastStartTimeSec = 0.08;
		NS_ABORT_MSG_IF(leaf_ids.size() < (kFixedSourceLeafCount + 1),
			"Need at least 12 leaves for fixed 44->1 foreground incast");
		NS_ABORT_MSG_IF(leaf_to_hosts.find(kFixedReceiverLeaf) == leaf_to_hosts.end(),
			"Fixed receiver leaf is not present in topology: " << kFixedReceiverLeaf);
		NS_ABORT_MSG_IF(kForegroundLastStartTimeSec >= app_stop_time,
			"Foreground repeated incast requires APP_STOP_TIME > 0.08");
		NS_ABORT_MSG_IF(foreground_incast_start_time > kForegroundLastStartTimeSec,
			"FOREGROUND_INCAST_START_TIME must be <= 0.08 for repeated foreground incast");

		std::mt19937 foreground_rng(static_cast<uint32_t>(random_seed));
		std::vector<uint32_t> shuffled_leaves;
		shuffled_leaves.reserve(leaf_ids.size());
		for (uint32_t leaf_id : leaf_ids) {
			if (leaf_id != kFixedReceiverLeaf) {
				shuffled_leaves.push_back(leaf_id);
			}
		}
		NS_ABORT_MSG_IF(shuffled_leaves.size() < kFixedSourceLeafCount,
			"Not enough non-receiver leaves for fixed 44->1 foreground incast");
		std::shuffle(shuffled_leaves.begin(), shuffled_leaves.end(), foreground_rng);
		std::vector<uint32_t> source_leaves;
		source_leaves.reserve(kFixedSourceLeafCount);
		for (uint32_t i = 0; i < kFixedSourceLeafCount; ++i) {
			source_leaves.push_back(shuffled_leaves[i]);
		}
		uint32_t receiver_leaf = kFixedReceiverLeaf;
		NS_ABORT_MSG_IF(leaf_to_hosts.find(receiver_leaf) == leaf_to_hosts.end(),
			"Receiver leaf has no host list: " << receiver_leaf);
		auto receiver_hosts = leaf_to_hosts[receiver_leaf];
		NS_ABORT_MSG_IF(receiver_hosts.empty(),
			"Receiver leaf has no hosts: " << receiver_leaf);
		uint32_t receiver_host = receiver_hosts[0];

		Time fg_stop = Seconds(app_stop_time);
		uint32_t foreground_flow_count = 0;
		std::vector<double> foreground_round_starts;
		for (double round_start = foreground_incast_start_time;
			 round_start <= kForegroundLastStartTimeSec + 1e-12;
			 round_start += kForegroundRoundIntervalSec) {
			foreground_round_starts.push_back(round_start);
		}
		std::ostringstream foreground_round_starts_desc;
		for (size_t i = 0; i < foreground_round_starts.size(); ++i) {
			if (i != 0) {
				foreground_round_starts_desc << ",";
			}
			foreground_round_starts_desc << foreground_round_starts[i];
		}
		std::vector<uint32_t> selected_sender_hosts;
		selected_sender_hosts.reserve(source_leaves.size() * kFixedSendersPerLeaf);
		for (uint32_t src_leaf : source_leaves) {
			auto senders = leaf_to_hosts[src_leaf];
			NS_ABORT_MSG_IF(senders.size() < kFixedSendersPerLeaf,
				"Leaf " << src_leaf << " does not have enough hosts for foreground incast");
			for (uint32_t idx = 0; idx < kFixedSendersPerLeaf; ++idx) {
				selected_sender_hosts.push_back(senders[idx]);
			}
		}
		std::vector<uint32_t> round_leaf_order;
		round_leaf_order.reserve(source_leaves.size());
		for (double round_start : foreground_round_starts) {
			round_leaf_order = source_leaves;
			if (foreground_incast_shuffle_leaf_order_per_round && round_leaf_order.size() > 1) {
				std::shuffle(round_leaf_order.begin(), round_leaf_order.end(), foreground_rng);
			}
			uint64_t leaf_jitter_step_ns = 0;
			if (foreground_incast_leaf_jitter_ns > 0 && round_leaf_order.size() > 1) {
				leaf_jitter_step_ns =
					foreground_incast_leaf_jitter_ns / static_cast<uint64_t>(round_leaf_order.size() - 1);
			}
			for (size_t leaf_pos = 0; leaf_pos < round_leaf_order.size(); ++leaf_pos) {
				uint32_t src_leaf = round_leaf_order[leaf_pos];
				auto senders = leaf_to_hosts[src_leaf];
				NS_ABORT_MSG_IF(senders.size() < kFixedSendersPerLeaf,
					"Leaf " << src_leaf << " does not have enough hosts for foreground incast");
				for (uint32_t idx = 0; idx < kFixedSendersPerLeaf; ++idx) {
					uint64_t jitter_ns = leaf_pos * leaf_jitter_step_ns +
						static_cast<uint64_t>(idx) * foreground_incast_host_jitter_step_ns;
					Time fg_start = Seconds(round_start) + NanoSeconds(jitter_ns);
					install_flow(senders[idx], receiver_host, incast_flow_size,
						fg_start, fg_stop, fg_start, fg_stop, true);
					foreground_flow_count++;
				}
			}
		}
		std::cerr << "Foreground incast: receiver_host=" << receiver_host
				  << " receiver_leaf=" << receiver_leaf
				  << " source_racks=" << kFixedSourceLeafCount
				  << " source_leaf_ids=" << join_uint_vector(source_leaves)
				  << " senders_per_rack=" << kFixedSendersPerLeaf
				  << " sender_hosts=" << join_uint_vector(selected_sender_hosts)
				  << " sender_selection=first_k_per_leaf"
				  << " receiver_leaf_selection=fixed_leaf_10"
				  << " receiver_host_selection=first_host_in_leaf"
				  << " fixed_topology=44_to_1"
				  << " flow_size=" << incast_flow_size
				  << " leaf_jitter_ns=" << foreground_incast_leaf_jitter_ns
				  << " host_jitter_step_ns=" << foreground_incast_host_jitter_step_ns
				  << " shuffle_leaf_order_per_round=" << foreground_incast_shuffle_leaf_order_per_round
				  << " round_interval=" << kForegroundRoundIntervalSec
				  << " round_starts=" << foreground_round_starts_desc.str()
				  << " total_flows=" << foreground_flow_count << std::endl;
		}

	for(auto iter=flows_per_host.begin(); iter != flows_per_host.end(); ++iter) {
		std::cerr << "Host " << iter->first << " : Expected " << iter->second << " hosts" << std::endl;
	}

	topof.close();
	// flowf.close();
	tracef.close();

	// schedule link down
	if (link_down_time > 0){
		Simulator::Schedule(Seconds(2) + MicroSeconds(link_down_time), &TakeDownLink, n, n.Get(link_down_A), n.Get(link_down_B));
	}

	//
	// Now, do the actual simulation.
	//
	std::cerr << "Running Simulation.\n";
	fflush(stdout);
	NS_LOG_INFO("Run Simulation.");
	Simulator::Stop(Seconds(simulator_stop_time));
	Simulator::Run();
	finalize_pause_intervals(pause_intervals_output);
	finalize_xoff_intervals(xoff_intervals_output);
	Simulator::Destroy();
	NS_LOG_INFO("Done.");
	fclose(trace_output);
	fclose(timeout_output);
	fclose(fct_output);
	if (pfc_file != nullptr)
		fclose(pfc_file);
	if (pause_intervals_output != nullptr)
		fclose(pause_intervals_output);
	if (bounced_file != nullptr)
		fclose(bounced_file);
	if (xoff_intervals_output != nullptr)
		fclose(xoff_intervals_output);
	if (packet_loss_output != nullptr)
		fclose(packet_loss_output);

	endt = clock();
	std::cerr << (double)(endt - begint) / CLOCKS_PER_SEC << "\n";

}


uint32_t sample_workload_size(const std::vector<std::pair<double, uint32_t>> &workload_cdf, double u) {
  auto it = std::lower_bound(
      workload_cdf.begin(), workload_cdf.end(), u,
      [](const std::pair<double, uint32_t> &entry, double value) {
        return entry.first < value;
      });
  if (it == workload_cdf.end()) {
    return workload_cdf.back().second;
  }
  return it->second;
}

bool load_workload(const char *workload_file,
                   std::vector<std::pair<double, uint32_t>> *workload_cdf,
                   double *avg_flow_size) {
  FILE *f = fopen(workload_file, "r");
  if(!f) return false;
  double prev_prob = 0.;
  double exact_avg_flow_size = 0.;
  bool saw_cdf_point = false;
  char line[256];
  while (fgets(line, sizeof(line), f) != nullptr) {
    char flow_size_str[64];
    char value_str[64];
    char extra_str[2];
    if (sscanf(line, " %63s %63s %1s", flow_size_str, value_str, extra_str) != 2)
      continue;

    int flow_size;
    double value;
    char trailing[2];
    if (sscanf(flow_size_str, "%d%1s", &flow_size, trailing) != 1)
      continue;
    if (sscanf(value_str, "%lf%1s", &value, trailing) != 1)
      continue;

    exact_avg_flow_size += (value - prev_prob) * flow_size;
    prev_prob = value;
    workload_cdf->push_back(std::make_pair(value, static_cast<uint32_t>(flow_size)));
    saw_cdf_point = true;
  }
  if (!saw_cdf_point) {
    fclose(f);
    return false;
  }
  if (avg_flow_size != nullptr) {
    *avg_flow_size = exact_avg_flow_size;
  }
  fclose(f);
  return true;
}
