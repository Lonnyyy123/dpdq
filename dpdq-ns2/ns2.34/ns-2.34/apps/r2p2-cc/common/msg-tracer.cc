#include "msg-tracer.h"
#include "simple-log.h"
#include "packet.h"
#include "json.hpp"
#include <cassert>
#include <iostream>
#include <fstream>

// TODO: provide via front end
#define LATENCY_MUL_THRESHOLD 1
#define PROP_DELAY_US 2.5
#define LINK_SPEED_GBPS 100
#define SIZE_START 0
#define SIZE_END 200000000
// #define SIZE_START 100000
// #define SIZE_END 400000

#define MSG_TO_PRINT_IN_DETAIL 224

using json = nlohmann::json;

int MsgTracer::debug_ = 1;
int MsgTracer::do_trace_ = 0;
std::map<MsgUid, MsgTracerEntry *> MsgTracer::pending_ = std::map<MsgUid, MsgTracerEntry *>();
bool MsgTracer::inited_ = false;
bool MsgTracer::finished_ = false;
bool MsgTracer::first_entry_ = true;
std::string MsgTracer::file_path_ = "";

static std::string tracer_op_text[] = {"Send",
                                       "Receive",
                                       "Enque",
                                       "Deque"};

std::string tracer_host_text[] = {"Host",
                                  "NIC",
                                  "Switch",
                                  "ToR",
                                  "Spine"};

MsgTracerEntry::~MsgTracerEntry()
{
    for (auto const &x : packets_)
    {
        delete x.second;
    }
}

PktTracerEntry::~PktTracerEntry() {}

void PktTracerEntry::add_ts(TimestampTracerEntry ts)
{
    if (ts_count_ < NUM_NW_TS)
    {
        timestamps[ts_count_] = ts;
        ts_count_++;
    }
    else
    {
        throw std::runtime_error("Attempted to add msg tracer timestamp to a full timstamp array");
    }
}

void MsgTracer::init_tracer(int debug)
{
    debug_ = debug;
    if (!inited_)
    {
        init_json();
        inited_ = true;
    }
}

void MsgTracer::finish()
{
    if (!finished_)
    {
        finish_json();
        finished_ = true;
    }
}

void MsgTracer::app_init_msg(uint64_t msg_uid, int32_t client_addr, int32_t saddr, int32_t daddr, uint32_t msg_size, std::string msg_type)
{
    if (MsgTracer::do_trace_)
    {
        slog::log6(debug_, -1, "MsgTracer::init_msg(). Num pending:", pending_.size());
        MsgUid uid = std::make_pair(msg_uid, client_addr);
        assert(MsgTracer::pending_.find(uid) == MsgTracer::pending_.end());
        MsgTracerEntry *entry = new MsgTracerEntry(msg_size, msg_uid, saddr, daddr, msg_type);
        entry->msg_start_ = Scheduler::instance().clock();
        MsgTracer::pending_[uid] = entry;
    }
}

void MsgTracer::timestamp_pkt(Packet *pkt, uint8_t op, uint8_t node_type, MsgTracerLogs &&logs)
{
    if (!MsgTracer::do_trace_)
        return;

    slog::log6(debug_, -1, "MsgTracer::timestamp_pkt(). Num pending:", pending_.size());
    assert(pkt != nullptr);
    hdr_cmn *hdr = hdr_cmn::access(pkt);
    hdr_r2p2 *r2p2_hdr = hdr_r2p2::access(pkt);

    int pkt_uid = hdr->uid();

    // Find message (protocol specific.. TODO: bad)
    uint64_t msg_uid = r2p2_hdr->app_level_id();
    int32_t client_addr = r2p2_hdr->cl_addr();
    MsgUid uid = std::make_pair(msg_uid, client_addr);
    MsgTracerEntry *msg_entry = nullptr;
    auto entry_it = pending_.find(uid);
    if (entry_it != pending_.end())
    {
        msg_entry = entry_it->second;
    }
    else
    {
        slog::log7(debug_, -1, "Did not find trace entry, ignoring");
        // replies will end up here since the entry will be already deleted.
        return;
    }
    assert(msg_entry != nullptr);

    PktTracerEntry *pkt_entry = nullptr;
    // Find or create packet
    auto pkt_entry_it = msg_entry->packets_.find(pkt_uid);
    if (pkt_entry_it != msg_entry->packets_.end())
    {
        // packet has been timestamped before.
        pkt_entry = pkt_entry_it->second;
        pkt_entry->pkt_end_ = Scheduler::instance().clock();
    }
    else
    {
        // create packet entry
        pkt_entry = new PktTracerEntry();
        pkt_entry->pkt_id_ = pkt_uid;
        pkt_entry->pkt_type_ = r2p2_hdr->msg_type();
        pkt_entry->pkt_start_ = Scheduler::instance().clock();
        pkt_entry->pkt_end_ = Scheduler::instance().clock();
        msg_entry->packets_[pkt_uid] = pkt_entry;
    }

    // Create timestamp
    TimestampTracerEntry ts_entry = TimestampTracerEntry(Scheduler::instance().clock(), -1, node_type, op);
    // Check if the timestamp is concurrent with the previous timestamp. If yes, add 1 nanosecond to clarify ordering (== should work, right?)
    int prev_ts_idx = pkt_entry->ts_count_ - 1;
    if ((pkt_entry->ts_count_ > 0) && (ts_entry.ts_ <= pkt_entry->timestamps[prev_ts_idx].ts_))
    {
        ts_entry.ts_ = pkt_entry->timestamps[prev_ts_idx].ts_ + 1.0 / (1000.0 * 1000.0 * 1000.0);
        assert(ts_entry.ts_ > pkt_entry->timestamps[prev_ts_idx].ts_);
    }
    ts_entry.logs_ = logs;
    pkt_entry->add_ts(ts_entry);
}

void MsgTracer::print_pkt_detail(MsgTracerEntry *entry)
{
    std::cout << "All packet timestamps" << std::endl;
    for (auto const &kv : entry->packets_)
    {
        PktTracerEntry *pktTracerEntry = kv.second;
        for (size_t j = 0; j < pktTracerEntry->ts_count_; ++j)
        {
            std::cout << pktTracerEntry->timestamps[j].ts_ << ",";
        }
        std::cout << hdr_r2p2::get_pkt_type(pktTracerEntry->pkt_type_);
        std::cout << std::endl;
    }
}

void MsgTracer::complete_msg(uint64_t msg_uid, int32_t client_addr, int32_t saddr)
{
    if (!MsgTracer::do_trace_)
    {
        return;
    }

    slog::log6(debug_, -1, "MsgTracer::complete_msg(). Num pending:", pending_.size());
    MsgUid uid = std::make_pair(msg_uid, client_addr);
    MsgTracerEntry *entry = nullptr;
    auto entry_it = pending_.find(uid);
    if (entry_it != pending_.end())
    {
        slog::log6(debug_, -1, "Found entry, closing message");
        entry = entry_it->second;
        entry->msg_end_ = Scheduler::instance().clock();
    }
    else
    {
        throw std::runtime_error("Could not find entry when attempting to complete message");
    }
    assert(entry);

    double trans_delay = (double)(entry->msg_size_ * 8.0) / ((double)(LINK_SPEED_GBPS)*1000.0 * 1000.0 * 1000.0);
    double slowdown = ((entry->msg_end_ - entry->msg_start_)) / ((trans_delay + (PROP_DELAY_US / 1000.0 / 1000.0)));
    entry->slowdown_ = slowdown;
    double cutoff = LATENCY_MUL_THRESHOLD * (trans_delay + (PROP_DELAY_US / 1000.0 / 1000.0));
    if ((entry->msg_end_ - entry->msg_start_ > cutoff) &&
        (entry->msg_size_ >= SIZE_START) &&
        (entry->msg_size_ <= SIZE_END))
    {
        std::cout << "Was Sampled. " << (entry->msg_end_ - entry->msg_start_) * 1000.0 * 1000.0 << " > "
                  << cutoff * 1000.0 * 1000.0 << ". expected ~ " << cutoff / LATENCY_MUL_THRESHOLD * 1000.0 * 1000.0
                  << " Approx slowdown: " << ((entry->msg_end_ - entry->msg_start_) * 1000.0 * 1000.0) / (cutoff / LATENCY_MUL_THRESHOLD * 1000.0 * 1000.0)
                  << " size: " << entry->msg_size_ << std::endl;
        std::cout << "trans delay = " << trans_delay * 1000.0 * 1000.0 << std::endl;

        std::cout << "\n------------ Info -------------" << std::endl;
        std::cout << Scheduler::instance().clock() << " COMPLETE msg. app_lvl_id: " << msg_uid << " sender: " << saddr << std::endl;
        std::cout << std::fixed << std::setprecision(9) << "Message started at: " << entry->msg_start_ << std::endl;
        std::cout << std::fixed << std::setprecision(9) << "Message ended at: " << entry->msg_end_ << std::endl;

        // if (msg_uid == MSG_TO_PRINT_IN_DETAIL)
        // {
        std::cout << "\n------------ Per packet intervals -------------" << std::endl;
        print_pkt_detail(entry);
        // }
        std::cout << "\n----------------------- End --------------------" << std::endl;
    }
    else
    {
        // if ((entry->msg_size_ >= SIZE_START) &&
        //     (entry->msg_size_ <= SIZE_END))
        // {
            std::cout << "NOT Sampled. " << entry->msg_size_ << " lat: " << 1000.0 * 1000.0 * (entry->msg_end_ - entry->msg_start_) << " cutoff: " << 1000.0 * 1000.0 * (cutoff) << std::endl;
        // }
        delete entry;
        MsgTracer::pending_.erase(uid);
        return;
    }

    output_msg_json(entry);
    delete entry;
    MsgTracer::pending_.erase(uid);
}

void MsgTracer::init_json()
{
    if (MsgTracer::file_path_ == "")
    {
        throw std::invalid_argument("MsgTracer::file_path_ has not been set. Exiting");
    }
    std::ofstream out_file;
    out_file.open(MsgTracer::file_path_);
    out_file << "[" << std::endl;
    out_file.close();
}

void MsgTracer::finish_json()
{
    if (MsgTracer::file_path_ == "")
    {
        throw std::invalid_argument("MsgTracer::file_path_ has not been set. Exiting");
    }
    std::ofstream out_file;
    out_file.open(MsgTracer::file_path_, std::ios_base::app);
    out_file << "]" << std::endl;
    out_file.flush();
    out_file.close();
}

void MsgTracer::output_msg_json(MsgTracerEntry *entry)
{
    if (MsgTracer::file_path_ == "")
    {
        throw std::invalid_argument("MsgTracer::file_path_ has not been set. Exiting");
    }
    // Create JSON
    json j;
    j["id"] = entry->msg_uid_;
    j["type"] = entry->msg_type_;
    j["start"] = entry->msg_start_;
    j["end"] = entry->msg_end_;
    j["size"] = entry->msg_size_;
    j["src"] = entry->saddr_;
    j["dst"] = entry->daddr_;
    j["sldn"] = entry->slowdown_;

    json packet_list = json::array();
    for (auto const &x : entry->packets_)
    {
        int packet_uid = x.first;
        PktTracerEntry *pkt = x.second;
        json jpkt;
        jpkt["id"] = packet_uid;
        jpkt["type"] = hdr_r2p2::get_pkt_type(pkt->pkt_type_); // perhaps pass the static converter function to the struct..
        jpkt["start"] = pkt->pkt_start_;
        jpkt["end"] = pkt->pkt_end_;

        // Add list of timestamps of each packet
        json timestamp_list = json::array();
        for (size_t i = 0; i < pkt->ts_count_; ++i)
        {
            json jts;
            TimestampTracerEntry *tsentry = &pkt->timestamps[i];
            jts["ts"] = tsentry->ts_;
            jts["node"] = tsentry->node_;
            jts["type"] = tracer_host_text[tsentry->node_type_];
            jts["op"] = tracer_op_text[tsentry->operation_];
            for (MsgTracerLog log : tsentry->logs_.logs_)
            {
                jts["logs"][log.name_] = log.value_;
            }
            timestamp_list.push_back(jts);
        }

        jpkt["timestamps"] = timestamp_list;
        packet_list.push_back(jpkt);
    }
    j["packets"] = packet_list;

    // Write JSON
    std::string jser = j.dump();
    std::ofstream out_file;
    // Too many opens and closes probably..
    out_file.open(MsgTracer::file_path_, std::ios_base::app);
    if (first_entry_)
    {
        first_entry_ = false;
    }
    else
    {
        out_file << ",";
    }
    out_file << jser << std::endl;
    out_file.close();
}
