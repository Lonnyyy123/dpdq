/* -*- Mode:C++; c-basic-offset:8; tab-width:8; indent-tabs-mode:t
			  -*- */

/*
 * Copyright (C) 1997 by the University of Southern California
 * $Id: classifier-mpath.cc,v 1.10 2005/08/25 18:58:01 johnh Exp $
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 59 Temple Place, Suite 330, Boston, MA 02111-1307, USA.
 *
 *
 * The copyright of this module includes the following
 * linking-with-specific-other-licenses addition:
 *
 * In addition, as a special exception, the copyright holders of
 * this module give you permission to combine (via static or
 * dynamic linking) this module with free software programs or
 * libraries that are released under the GNU LGPL and with code
 * included in the standard release of ns-2 under the Apache 2.0
 * license or under otherwise-compatible licenses with advertising
 * requirements (or modified versions of such code, with unchanged
 * license).  You may copy and distribute such a system following the
 * terms of the GNU GPL for this module and the licenses of the
 * other code concerned, provided that you include the source code of
 * that other code when and as the GNU GPL requires distribution of
 * source code.
 *
 * Note that people who make modified versions of this module
 * are not obligated to grant this special exception for their
 * modified versions; it is their choice whether to do so.  The GNU
 * General Public License gives permission to release a modified
 * version without this exception; this exception also makes it
 * possible to release a modified version which carries forward this
 * exception.
 *
 */

#include "classifier-hash.h"
#include "net-interface.h"
#include "r2p2-hdr.h"
#include <utility>
#ifndef lint
static const char rcsid[] =
	"@(#) $Header: /cvsroot/nsnam/ns-2/classifier/classifier-mpath.cc,v 1.10 2005/08/25 18:58:01 johnh Exp $ (USC/ISI)";
#endif

#include "classifier.h"
#include "ip.h"

static int slotcmp(const void *a, const void *b)
{
	return (int)(*(unsigned int *)a - *(unsigned int *)b);
}

class MultiPathForwarder : public Classifier
{
public:
	MultiPathForwarder() : ns_(0), nodeid_(0), nodetype_(0), symmetric_(0), sorted_maxslot_(-1), 
                           perflow_(0), checkpathid_(0), dest_hash_classifier_(nullptr)
	{
		bind("nodeid_", &nodeid_);
		bind("nodetype_", &nodetype_);
		bind_bool("symmetric_", &symmetric_); // expressPass
		bind("perflow_", &perflow_);
		bind("checkpathid_", &checkpathid_);
	}
    int command(int argc, const char*const* argv) {
        if (argc == 3) {
            if (strcmp(argv[1], "set-dest-hash-classifier") == 0) {
                dest_hash_classifier_ = dynamic_cast<DestHashClassifier*>(TclObject::lookup(argv[2]));
                if (dest_hash_classifier_ == nullptr) {
                    fprintf(stderr, "MultiPathForwarder::command: "
                                    "failed to set dest_hash_classifier_\n");
                    return TCL_ERROR;
                }
                return TCL_OK;
            }
        }
        return Classifier::command(argc, argv);
    }
	virtual int classify(Packet *p)
	{
		int cl;
		hdr_ip *h = hdr_ip::access(p);
		// Mohammad: multipath support
		// fprintf(stdout, "perflow_ = %d, rcv packet in classifier. maxslot_ = %d\n", perflow_, maxslot_);
		if (symmetric_)
		{
			// If there exists at least one slot and
			// not yet sorted
			if (sorted_maxslot_ == -1 || maxslot_ > sorted_maxslot_)
			{
				qsort(slot_, maxslot_ + 1, sizeof(NsObject *), slotcmp);
				sorted_maxslot_ = maxslot_;
			}
			hdr_ip *iph = hdr_ip::access(p);

			struct hkey
			{
				int fid;
				int nodetype;
				nsaddr_t lower_addr, higher_addr;
			};
			struct hkey buf_;
			nsaddr_t src = mshift(iph->saddr());
			nsaddr_t dst = mshift(iph->daddr());
			int *bufInteger;
			int bufLength;
			unsigned int ms_;

			buf_.nodetype = nodetype_;
			buf_.lower_addr = (src < dst) ? src : dst;
			buf_.higher_addr = (src > dst) ? src : dst;
			buf_.fid = iph->flowid();

			bufInteger = (int *)&buf_;
			bufLength = sizeof(hkey) / sizeof(int);

			ms_ = (unsigned int)HashString(bufInteger, bufLength);
			ms_ %= (maxslot_ + 1);
			unsigned int fail = ms_;
			do
			{
				cl = ms_++;
				ms_ %= (maxslot_ + 1);
			} while (slot_[cl] == 0 && ms_ != fail);
		}
		else if (perflow_ || checkpathid_)
		{
			/*if (h->flowid() >= 10000000) {
			int fail = ns_;
			do {
			  cl = ns_++;
			  ns_ %= (maxslot_ + 1);
			} while (slot_[cl] == 0 && ns_ != fail);
			return cl;
			}*/

			struct hkey
			{
				int nodeid;
				nsaddr_t src, dst;
				int fid;
			};
			struct hkey buf_;
			buf_.nodeid = nodeid_;
			buf_.src = mshift(h->saddr());
			buf_.dst = mshift(h->daddr());
			buf_.fid = h->flowid();
			/*if (checkpathid_)
			buf_.prio = h->prio();
		else
		buf_.prio = 0;*/
			char *bufString = (char *)&buf_;
			int length = sizeof(hkey);

			unsigned int ms_ = (unsigned int)HashString(bufString, length);
			if (checkpathid_)
			{
				fprintf(stdout, "checkpathid_ is true");
				int pathNum = h->prio();
				int pathDig;
				for (int i = 0; i < nodetype_; i++)
				{
					pathDig = pathNum % 8;
					pathNum /= 8;
				}
				// printf("%d: %d->%d\n", nodetype_, h->prio(), pathDig);
				ms_ += h->prio(); // pathDig;
			}
			ms_ %= (maxslot_ + 1);
			// printf("nodeid = %d, pri = %d, ms = %d\n", nodeid_, buf_.prio, ms_);
			int fail = ms_;
			do
			{
				cl = ms_++;
				ms_ %= (maxslot_ + 1);
			} while (slot_[cl] == 0 && ms_ != fail);
			// printf("nodeid = %d, pri = %d, cl = %d\n", nodeid_, h->prio(), cl);
		}

		else
		{
			// hdr_ip* h = hdr_ip::access(p);
			// if (h->flowid() == 45) {
			// cl = h->prio() % (maxslot_ + 1);
			// }
			// else {
			int fail = ns_;
			do
			{
				cl = ns_++;
				ns_ %= (maxslot_ + 1);
			} while (slot_[cl] == 0 && ns_ != fail);
		}
		//}

        if (dest_hash_classifier_ != nullptr) {
            hdr_cmn* ch = hdr_cmn::access(p);
            if (ch->ptype() == PT_UDP) {
               
                hdr_r2p2* r2p2_hdr = hdr_r2p2::access(p);
                hdr_ip* ip_hdr = hdr_ip::access(p);
                int msg_type = r2p2_hdr->msg_type();
                int src = ip_hdr->src().addr_;
                int dst = ip_hdr->dst().addr_;
                int seq = r2p2_hdr->seq();

                // symmetric hash for GRANT and REQUEST packets
                if (msg_type == hdr_r2p2::GRANT || msg_type == hdr_r2p2::REQUEST) {
                    struct __attribute__ ((packed)) Key{
                        int src;
                        int dst;
                        int seq;
                    } key;

                    // GRANT packet
                    key.seq = seq;
                    key.src = src;
                    key.dst = dst;

                    // REQUEST packet, swap src and dst
                    if (msg_type == hdr_r2p2::REQUEST ||
                        (msg_type == hdr_r2p2::GRANT && r2p2_hdr->bounced_credit())) {
                        key.src = dst;
                        key.dst = src;
                    }

                    uint32_t hash = crc32(0, (const uint8_t*)&key, sizeof(Key));
                    cl = hash % (maxslot_ + 1);
                }
                
                NetworkInterface* iface = get_iface(cl);
                if (iface != nullptr) {
                    int iface_label = iface->intf_label();
                    dest_hash_classifier_->ppass_egress(iface_label, p, false);
                } else {
                    fprintf(stderr, "MultiPathForwarder::classify: get_iface returned nullptr for slot %d\n", cl);
                }
            }
        }
		return cl;
	}

    NetworkInterface* get_iface(int slot) {
        if (slot < 0 || slot > maxslot_) {
            fprintf(stderr, "MultiPathForwarder::get_iface: slot %d out of range\n", slot);
            return nullptr;
        }

        NsObject *obj = slot_[slot];
        if (obj == nullptr) {
            fprintf(stderr, "MultiPathForwarder::get_iface: slot %d obj is nullptr\n", slot);
            return nullptr;
        }

        Connector *conn = dynamic_cast<Connector*>(obj);
        if (conn == nullptr || conn->target() == nullptr) {
            fprintf(stderr, "MultiPathForwarder::get_iface: slot %d conn is nullptr or conn->target() is nullptr\n", slot);
            return nullptr;
        }

        Connector *sq = dynamic_cast<Connector*>(conn->target());
        if (sq == nullptr || sq->target() == nullptr) {
            fprintf(stderr, "MultiPathForwarder::get_iface: slot %d sq is nullptr or sq->target() is nullptr\n", slot);
            return nullptr;
        }

        NetworkInterface *nif = dynamic_cast<NetworkInterface*>(sq->target());
        return nif;
    }

    uint32_t crc32(uint32_t crc, const uint8_t* data, size_t len) {
        crc = ~crc;
        for (size_t i = 0; i < len; i++) {
            crc ^= data[i];
            for (int j = 0; j < 8; j++)
                crc = (crc >> 1) ^ (0xEDB88320 & (-(crc & 1)));
        }
        return ~crc;
    }

private:
	int ns_;
	int nodeid_;
	int nodetype_;
	// Symmetric Routing
	// "True" for symmetric routing,
	// "False" for asymmetric routing (default)
	int symmetric_;
	int sorted_maxslot_;
	// Mohamamd: adding support for perflow multipath
	int perflow_;
	int checkpathid_;

    DestHashClassifier* dest_hash_classifier_;

	static unsigned int
	HashString(register const char *bytes, int length)
	{
		register unsigned int result;
		register int i;

		result = 0;
		for (i = 0; i < length; i++)
		{
			result += (result << 3) + *bytes++;
		}
		return result;
	}

	// below is the ExpressPass version
	static unsigned int
	HashString(register const int *ints, int length)
	{
		register unsigned int result;
		register int i;

		result = 0;
		for (i = 0; i < length; i++)
		{
			srand(*ints++);
			int ran = rand();
			result = result ^ ran;
		}
		srand(result);
		result = rand();
		return result;
	}
};

static class MultiPathClass : public TclClass
{
public:
	MultiPathClass() : TclClass("Classifier/MultiPath") {}
	TclObject *create(int, const char *const *)
	{
		return (new MultiPathForwarder());
	}
} class_multipath;
