#ifndef _DPDQ_PARSERS_P4_
#define _DPDQ_PARSERS_P4_

parser TofinoIngressPortParser(
        packet_in pkt,
        out ingress_intrinsic_metadata_t ig_intr_md) {
    state start {
        pkt.extract(ig_intr_md);
        transition select(ig_intr_md.resubmit_flag) {
            1: parse_resubmit;
            0: parse_port_metadata;
        }
    }

    state parse_resubmit {
        pkt.advance(64);
        transition accept;
    }

    state parse_port_metadata {
        // Tofino 1 prepends 64 bits of port metadata after intrinsic metadata.
        pkt.advance(64);
        transition accept;
    }
}

parser LayerParser(packet_in pkt, out headers_t hdr) {
    state start {
        transition parse_ethernet;
    }

    state parse_ethernet {
        pkt.extract(hdr.ethernet);
        transition select(hdr.ethernet.ether_type) {
            ETHERTYPE_IPV4: parse_ipv4;
            default: accept;
        }
    }

    state parse_ipv4 {
        pkt.extract(hdr.ipv4);
        transition select(hdr.ipv4.protocol) {
            IP_PROTOCOL_UDP: parse_udp;
            default: accept;
        }
    }

    state parse_udp {
        pkt.extract(hdr.udp);
        transition select(hdr.udp.dst_port) {
            DPDQ_UDP_PORT: parse_dpdq;
            default: accept;
        }
    }

    state parse_dpdq {
        pkt.extract(hdr.dpdq);
        transition select(hdr.dpdq.reserved[11:11]) {
            1: parse_dpdq_internal;
            default: accept;
        }
    }

    state parse_dpdq_internal {
        pkt.extract(hdr.dpdq_internal);
        transition accept;
    }
}

#endif
