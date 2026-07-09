/* -*-	Mode:C++; c-basic-offset:8; tab-width:8; indent-tabs-mode:t -*- */
/*
 * Copyright (c) 1997 The Regents of the University of California.
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
 * 	This product includes software developed by the Network Research
 * 	Group at Lawrence Berkeley National Laboratory.
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

#include "connector.h"
#include "object.h"
#include "red.h"
#include "scheduler.h"
#include "tclcl.h"
#include <cstddef>
#include <cstdio>
#include <iomanip>
#include <cassert>
#include <cstdint>
#include <algorithm>
#ifndef lint
static const char rcsid[] =
    "@(#) $Header: /cvsroot/nsnam/ns-2/classifier/classifier-hash.cc,v 1.30 2005/09/18 23:33:31 tomh Exp $ (LBL)";
#endif

//
// a generalized classifier for mapping (src/dest/flowid) fields
// to a bucket.  "buckets_" worth of hash table entries are created
// at init time, and other entries in the same bucket are created when
// needed
//
//

extern "C" {
#include <tcl.h>
}

#include <stdlib.h>
#include <iostream>
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include "simple-tracer.h"
#include "config.h"
#include "packet.h"
#include "ip.h"
#include "classifier.h"
#include "classifier-hash.h"
#include "r2p2-hdr.h"
#include "r2p2.h"
#include "flags.h"
#include "net-interface.h"

namespace {
void append_line_to_file(const std::string &path, const std::string &line)
{
    if (path.empty()) {
        return;
    }
    std::ofstream out(path.c_str(), std::ios::out | std::ios::app);
    if (!out.is_open()) {
        std::cerr << "Could not open log file " << path << " for append\n";
        return;
    }
    out << line << std::endl;
}
}

/****************** HashClassifier Methods ************/

int HashClassifier::classify(Packet * p) {
	int slot= lookup(p);
	if (slot >= 0 && slot <=maxslot_)
		return (slot);
	else if (default_ >= 0)
		return (default_);
	return (unknown(p));
} // HashClassifier::classify

int HashClassifier::command(int argc, const char*const* argv)
{
	Tcl& tcl = Tcl::instance();
	/*
	 * $classifier set-hash $hashbucket src dst fid $slot
	 */

	if (argc == 7) {
		if (strcmp(argv[1], "set-hash") == 0) {
			//xxx: argv[2] is ignored for now
			nsaddr_t src = atoi(argv[3]);
			nsaddr_t dst = atoi(argv[4]);
			int fid = atoi(argv[5]);
			int slot = atoi(argv[6]);
			if (0 > set_hash(src, dst, fid, slot))
				return TCL_ERROR;
			return TCL_OK;
		}
	} else if (argc == 6) {
		/* $classifier lookup $hashbuck $src $dst $fid */
		if (strcmp(argv[1], "lookup") == 0) {
			nsaddr_t src = atoi(argv[3]);
			nsaddr_t dst = atoi(argv[4]);
			int fid = atoi(argv[5]);
			int slot= get_hash(src, dst, fid);
			if (slot>=0 && slot <=maxslot_) {
				tcl.resultf("%s", slot_[slot]->name());
				return (TCL_OK);
			}
			tcl.resultf("");
			return (TCL_OK);
		}
                // Added by Yun Wang to set rate for TBFlow or TSWFlow
                if (strcmp(argv[1], "set-flowrate") == 0) {
                        int fid = atoi(argv[2]);
                        nsaddr_t src = 0;  // only use fid
                        nsaddr_t dst = 0;  // to classify flows
                        int slot = get_hash( src, dst, fid );
                        if ( slot >= 0 && slot <= maxslot_ ) {
                                Flow* f = (Flow*)slot_[slot];
                                tcl.evalf("%u set target_rate_ %s",
                                        f, argv[3]);
                                tcl.evalf("%u set bucket_depth_ %s",
                                        f, argv[4]);
                                tcl.evalf("%u set tbucket_ %s",
                                        f, argv[5]);
                                return (TCL_OK);
                        }
                        else {
                          tcl.evalf("%s set-rate %u %u %u %u %s %s %s",
                          name(), src, dst, fid, slot, argv[3], argv[4],argv[5])
;
                          return (TCL_OK);
                        }
                }  
	} else if (argc == 5) {
		/* $classifier del-hash src dst fid */
		if (strcmp(argv[1], "del-hash") == 0) {
			nsaddr_t src = atoi(argv[2]);
			nsaddr_t dst = atoi(argv[3]);
			int fid = atoi(argv[4]);
			
			Tcl_HashEntry *ep= Tcl_FindHashEntry(&ht_, 
							     hashkey(src, dst,
								     fid)); 
			if (ep) {
				long slot = (long)Tcl_GetHashValue(ep);
				Tcl_DeleteHashEntry(ep);
				tcl.resultf("%lu", slot);
				return (TCL_OK);
			}
			return (TCL_ERROR);
		}
	}
	return (Classifier::command(argc, argv));
}

/**************  TCL linkage ****************/
static class SrcDestHashClassifierClass : public TclClass {
public:
	SrcDestHashClassifierClass() : TclClass("Classifier/Hash/SrcDest") {}
	TclObject* create(int, const char*const*) {
		return new SrcDestHashClassifier;
	}
} class_hash_srcdest_classifier;

static class FidHashClassifierClass : public TclClass {
public:
	FidHashClassifierClass() : TclClass("Classifier/Hash/Fid") {}
	TclObject* create(int, const char*const*) {
		return new FidHashClassifier;
	}
} class_hash_fid_classifier;

static class DestHashClassifierClass : public TclClass {
public:
	DestHashClassifierClass() : TclClass("Classifier/Hash/Dest") {}
	TclObject* create(int, const char*const*) {
		return new DestHashClassifier;
	}
} class_hash_dest_classifier;

static class SrcDestFidHashClassifierClass : public TclClass {
public:
	SrcDestFidHashClassifierClass() : TclClass("Classifier/Hash/SrcDestFid") {}
	TclObject* create(int, const char*const*) {
		return new SrcDestFidHashClassifier;
	}
} class_hash_srcdestfid_classifier;


// DestHashClassifier methods
int DestHashClassifier::classify(Packet *p)
{
	int slot= lookup(p);
    int ret_slot = slot;
	if (slot >= 0 && slot <= maxslot_)
		ret_slot = slot;
	else if (default_ >= 0)
		ret_slot = default_;
	else
		ret_slot = -1;

    /**
     * PPass logic
     */
    hdr_cmn* ch = hdr_cmn::access(p);
    if (enable_ppass_ && ch->ptype() == PT_UDP) {
        int iface_in = ch->iface();
        bool bounced = ppass_ingress(iface_in, p);
        if (bounced) {
            ret_slot = get_slot_for_iface_label(iface_in);
            if (ret_slot < 0) {
                ret_slot = lookup(p);
            }
        }

        NetworkInterface* out_iface = get_iface(ret_slot);
        if (out_iface != nullptr) {
            int iface_out = out_iface->intf_label();
            ppass_egress(iface_out, p, bounced);
        }
    }

    return ret_slot;
} // HashClassifier::classify

void DestHashClassifier::drain_phantom_queue(PPassPortStatus &status, double now) {
    double interval = now - status.last_time_;
    if (status.last_time_ == 0.0 || interval < 0.0) {
        interval = 0.0;
    }
    status.last_time_ = now;

    uint64_t delta = static_cast<uint64_t>(rho_ * interval * line_rate_gbps_ * 125e6);
    status.pqlen_ = (delta > status.pqlen_) ? 0 : status.pqlen_ - delta;
}

uint64_t DestHashClassifier::get_congestion_metric(const PPassPortStatus &status) const {
    uint64_t egress_qlen = 0;
    if (status.egress_queue_ != nullptr) {
        egress_qlen = status.egress_queue_->byteLength();
    }
    return std::max<uint64_t>(status.pqlen_, egress_qlen);
}

void DestHashClassifier::update_port_state(PPassPortStatus &status) {
    int xoff = (xoff_bytes_ > 0) ? xoff_bytes_ : pthr_;
    int xon = (xon_bytes_ >= 0) ? xon_bytes_ : std::max(0, xoff - MIN_ETHERNET_FRAME_ON_WIRE);
    if (xoff <= 0) {
        return;
    }

    bool was_paused = status.paused_;
    uint64_t metric = get_congestion_metric(status);
    if (!status.paused_ && metric > static_cast<uint64_t>(xoff)) {
        status.paused_ = true;
    } else if (status.paused_ && metric < static_cast<uint64_t>(xon)) {
        status.paused_ = false;
    }
    if (was_paused != status.paused_) {
        fprintf(stderr, "DPDQ PORT %d %s metric=%lu pqlen=%lu xoff=%d xon=%d\n",
                id_, status.paused_ ? "PAUSE" : "NORMAL",
                static_cast<unsigned long>(metric),
                static_cast<unsigned long>(status.pqlen_),
                xoff, xon);
        append_dpdq_event_log(status.paused_ ? "PAUSE" : "NORMAL",
                              status.peer_id_,
                              metric,
                              status.pqlen_,
                              xoff,
                              xon,
                              -1,
                              -1,
                              -1,
                              -1);
    }
}

void DestHashClassifier::clamp_phantom_queue(PPassPortStatus &status) {
    const uint64_t limit = static_cast<uint64_t>(pthr_) * 10;
    status.pqlen_ = std::min<uint64_t>(status.pqlen_, limit);
}

void DestHashClassifier::bounce_credit(Packet *p) {
    hdr_ip *ip_hdr = hdr_ip::access(p);
    hdr_r2p2 *r2p2_hdr = hdr_r2p2::access(p);
    nsaddr_t src = ip_hdr->saddr();
    nsaddr_t dst = ip_hdr->daddr();
    fprintf(stderr, "DPDQ BOUNCE switch=%d credit=%d seq=%d %d->%d\n",
            id_, r2p2_hdr->credit(), r2p2_hdr->seq(), src, dst);
    append_dpdq_event_log("BOUNCE",
                          -1,
                          0,
                          0,
                          xoff_bytes_,
                          xon_bytes_,
                          r2p2_hdr->credit(),
                          r2p2_hdr->seq(),
                          src,
                          dst);
    // A bounced credit returns to the receiver as a GRANT carrying the same credit amount.
    ip_hdr->saddr() = dst;
    ip_hdr->daddr() = src;
    r2p2_hdr->bounced_credit() = true;
}

void DestHashClassifier::append_dpdq_event_log(const std::string &event_type,
                                               int iface_label,
                                               uint64_t metric,
                                               uint64_t pqlen,
                                               int xoff,
                                               int xon,
                                               int credit,
                                               int seq,
                                               nsaddr_t src,
                                               nsaddr_t dst)
{
    if (dpdq_event_log_file_.empty()) {
        return;
    }
    double now = Scheduler::instance().clock();
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(10)
        << now
        << "," << id_
        << "," << event_type
        << "," << iface_label
        << "," << metric
        << "," << pqlen
        << "," << xoff
        << "," << xon
        << "," << credit
        << "," << seq
        << "," << src
        << "," << dst;
    append_line_to_file(dpdq_event_log_file_, oss.str());
}

int DestHashClassifier::get_slot_for_iface_label(int iface_label) {
    for (int slot = 0; slot <= maxslot_; ++slot) {
        NetworkInterface *iface = get_iface(slot);
        if (iface != nullptr && iface->intf_label() == iface_label) {
            return slot;
        }
    }
    return -1;
}

NetworkInterface* DestHashClassifier::get_iface(int slot) {
    if (slot < 0 || slot > maxslot_) {
        fprintf(stderr, "DestHashClassifier::get_iface: slot %d out of range\n", slot);
        return nullptr;
    }

    NsObject *obj = slot_[slot];
    if (obj == nullptr) {
        fprintf(stderr, "DestHashClassifier::get_iface: slot %d obj is nullptr\n", slot);
        return nullptr;
    }

    Connector *conn = dynamic_cast<Connector*>(obj);
    if (conn == nullptr || conn->target() == nullptr) {
        //fprintf(stderr, "DestHashClassifier::get_iface: slot %d conn is nullptr or conn->target() is nullptr\n", slot);
        return nullptr;
    }

    Connector *sq = dynamic_cast<Connector*>(conn->target());
    if (sq == nullptr || sq->target() == nullptr) {
        fprintf(stderr, "DestHashClassifier::get_iface: slot %d sq is nullptr or sq->target() is nullptr\n", slot);
        return nullptr;
    }

    NetworkInterface *nif = dynamic_cast<NetworkInterface*>(sq->target());
    return nif;
} // DestHashClassifier::get_iface

bool DestHashClassifier::ppass_ingress(int iface_label, Packet *p) {
    PPassPortStatus &status = port_status_map_[iface_label];
    hdr_r2p2 *r2p2_hdr = hdr_r2p2::access(p);
    double now = Scheduler::instance().clock();
    update_port_state(status);

    if (r2p2_hdr->msg_type() == hdr_r2p2::GRANT) {
        if (!r2p2_hdr->bounced_credit() && status.paused_) {
            // Once a port is paused, incoming credits are immediately reflected back.
            bounce_credit(p);
            if (status.pqlenInt_ != nullptr) {
                status.pqlenInt_->newPoint(now, status.pqlen_);
            }
            return true;
        }
        if (!r2p2_hdr->bounced_credit()) {
            status.pqlen_ += r2p2_hdr->credit();
        }
    }
    if (r2p2_hdr->msg_type() == hdr_r2p2::RESEND){
        //status.pqlen_ += r2p2_hdr->resend_bytes();
    }
    //clamp_phantom_queue(status);
    update_port_state(status);

    if (status.pqlenInt_ != nullptr) {
        status.pqlenInt_->newPoint(now, status.pqlen_);
    }

    return false;
} // DestHashClassifier::ppass_ingress

void DestHashClassifier::ppass_egress(int iface_label, Packet *p, bool just_bounced) {
    PPassPortStatus &status = port_status_map_[iface_label];

    hdr_cmn *ch = hdr_cmn::access(p);
    hdr_r2p2 *r2p2_hdr = hdr_r2p2::access(p);
    double now = Scheduler::instance().clock();
    drain_phantom_queue(status, now);

    if (r2p2_hdr->msg_type() == hdr_r2p2::GRANT && r2p2_hdr->bounced_credit() && !just_bounced) {
        // Revoke the reservation that was created while the original credit moved downstream.
        status.pqlen_ = (r2p2_hdr->credit() > static_cast<int>(status.pqlen_)) ? 0 : status.pqlen_ - r2p2_hdr->credit();
    } else if (r2p2_hdr->msg_type() == hdr_r2p2::GRANT || r2p2_hdr->is_unsol_pkt() > 0 || r2p2_hdr->msg_type() == hdr_r2p2::GRANT_REQ || r2p2_hdr->msg_type() == hdr_r2p2::RESEND) {
        status.pqlen_ += ch->size();
    }
    //clamp_phantom_queue(status);
    update_port_state(status);

    int egress_qlen = status.egress_queue_ != nullptr ? status.egress_queue_->byteLength() : 0;
    bool is_data_pkt = ((r2p2_hdr->msg_type() == hdr_r2p2::REQUEST || 
                         r2p2_hdr->msg_type() == hdr_r2p2::GRANT_REQ || 
                         r2p2_hdr->msg_type() == hdr_r2p2::REPLY));
    if (is_data_pkt && get_congestion_metric(status) > pthr_){
        hdr_flags::access(p)->ce() = 1;
    }

    if (status.pqlenInt_ != nullptr) {
        status.pqlenInt_->newPoint(now, status.pqlen_);
    }

} // DestHashClassifier::ppass_egress

void SampleTimer::expire(Event *e)
{
    hash_classifier_->sample_qlen();
} // SampleTimer::expire

void DestHashClassifier::sample_qlen()
{
    if (!sample_header_written_) {
        if (sample_stream_.is_open()) {
            sample_stream_ << "Timestamp(s)";
            for (const auto& entry : port_status_map_) {
                int peer_id = entry.second.peer_id_;
                sample_stream_ << ",peer_" << peer_id << "_qlen(Byte)";
            }
            sample_stream_ << std::endl;
        } else {
            std::cerr << "DestHashClassifier::sample_qlen sample_stream_ not open\n";
            return;
        }
        sample_header_written_ = true;
    }

    // reference: ns2.34/ns-2.34/tcl/lib/ns-link.tcl: sample-queue-size

    double now = Scheduler::instance().clock();

    if (sample_stream_.is_open()) {
        sample_stream_ << std::fixed << std::setprecision(10) << now - 10;
    } else {
        std::cerr << "DestHashClassifier::sample_qlen sample_stream_ not open\n";
        return;
    }

    for (const auto& entry : port_status_map_) {
        const PPassPortStatus &status = entry.second;

        status.pqlenInt_->newPoint(now, status.pqlenInt_->getLasty());
        double bsum = status.pqlenInt_->getSum();
        double mean_qlen = bsum / sample_interval_;
        status.pqlenInt_->setSum(0.0);
        sample_stream_ << "," << mean_qlen;
    }
    sample_stream_ << std::endl;

    sample_timer_->resched(sample_interval_);
} // DestHashClassifier::sample_qlen

void DestHashClassifier::do_install(char* dst, NsObject *target) {
	nsaddr_t d = atoi(dst);
	int slot = getnxt(target);
	install(slot, target); 
	if (set_hash(0, d, 0, slot) < 0)
		fprintf(stderr, "DestHashClassifier::set_hash from within DestHashClassifier::do_install returned value < 0");
}

int DestHashClassifier::command(int argc, const char*const* argv)
{
	if (argc == 4) {
		// $classifier install $dst $node
		if (strcmp(argv[1], "install") == 0) {
			char dst[SMALL_LEN];
			strcpy(dst, argv[2]);
			NsObject *node = (NsObject*)TclObject::lookup(argv[3]);
			//nsaddr_t dst = atoi(argv[2]);
			do_install(dst, node); 
			return TCL_OK;
			//int slot = getnxt(node);
			//install(slot, node);
			//if (set_hash(0, dst, 0, slot) >= 0)
			//return TCL_OK;
			//else
			//return TCL_ERROR;
		} // if

        if (strcmp(argv[1], "set-egress-queue") == 0) {
            int iface_label = atoi(argv[2]);
            PPassPortStatus &status = port_status_map_[iface_label];
            status.egress_queue_ = dynamic_cast<Queue*>(TclObject::lookup(argv[3]));
            if (status.egress_queue_ == nullptr) {
                std::cerr << "DestHashClassifier::command could not find REDQueue " << argv[3] << "\n";
                return TCL_ERROR;
            }
            return TCL_OK;
        }

        if (strcmp(argv[1], "set-peer-id") == 0) {
            int iface_label = atoi(argv[2]);
            PPassPortStatus &status = port_status_map_[iface_label];
            status.peer_id_ = atoi(argv[3]);
            return TCL_OK;
        }

        if (strcmp(argv[1], "set-pqlen-integrator") == 0) {
            int iface_label = atoi(argv[2]);
            PPassPortStatus &status = port_status_map_[iface_label];

            status.pqlenInt_ = (Integrator *)TclObject::lookup(argv[3]);
            if (status.pqlenInt_ == nullptr)
                return (TCL_ERROR);
            return (TCL_OK);
        }
	}
    if (argc == 2) {
        if (strcmp(argv[1], "enable-dpdq-flow-control") == 0 ||
            strcmp(argv[1], "enable-ppass") == 0) {
            enable_ppass_ = true;
            return TCL_OK;
        }
    }
	if (argc == 3) {
        if (strcmp(argv[1], "dpdq-event-log-file") == 0) {
            dpdq_event_log_file_ = argv[2];
            return TCL_OK;
        }
        if (strcmp(argv[1], "start-sample-pqlen") == 0) {
            if (!sample_timer_)
                return TCL_ERROR;
            sample_timer_->sched(atof(argv[2]));
            return TCL_OK;
        }
        if (strcmp(argv[1], "sample-file") == 0) {
            if (sample_stream_.is_open())
                sample_stream_.close();
            sample_file_ = argv[2];
            sample_stream_.open(sample_file_.c_str(),
                                std::ios::out | std::ios::trunc);
            if (!sample_stream_.is_open()) {
                std::cerr << "DestHashClassifier::command could not open " << sample_file_ << " for writing\n";
                return TCL_ERROR;
            }
            return (TCL_OK);
        }
    }
	return(HashClassifier::command(argc, argv));
} // command
