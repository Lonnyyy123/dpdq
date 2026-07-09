#ifndef _DPDQ_HEADERS_P4_
#define _DPDQ_HEADERS_P4_

const bit<16> ETHERTYPE_IPV4 = 0x0800;
const bit<8> IP_PROTOCOL_UDP = 17;
const bit<16> DPDQ_UDP_PORT = 9001;
const bit<16> DPDQ_SYNC_MAGIC = 0xd5c1;

const bit<8> DPDQ_REQUEST = 0;
const bit<8> DPDQ_REPLY = 1;
const bit<8> DPDQ_GRANT = 8;
const bit<8> DPDQ_GRANT_REQ = 9;
const bit<8> DPDQ_RESEND = 10;

const bit<8> DPDQ_FLAG_BOUNCED = 0x01;
const bit<8> DPDQ_FLAG_UNSCHEDULED = 0x02;
const bit<8> DPDQ_FLAG_NETWORK_MARKED = 0x04;

typedef bit<48> mac_addr_t;
typedef bit<32> ipv4_addr_t;

header ethernet_h {
    mac_addr_t dst_addr;
    mac_addr_t src_addr;
    bit<16> ether_type;
}

header ipv4_h {
    bit<4> version;
    bit<4> ihl;
    bit<8> diffserv;
    bit<16> total_len;
    bit<16> identification;
    bit<3> flags;
    bit<13> frag_offset;
    bit<8> ttl;
    bit<8> protocol;
    bit<16> hdr_checksum;
    ipv4_addr_t src_addr;
    ipv4_addr_t dst_addr;
}

header udp_h {
    bit<16> src_port;
    bit<16> dst_port;
    bit<16> hdr_length;
    bit<16> checksum;
}

// Parsed DPDQ prefix. The remaining 12 wire bytes stay in packet payload.
header dpdq_h {
    bit<8> msg_type;
    bit<8> flags;
    bit<16> reserved;
    bit<32> credit;
}

header dpdq_internal_h {
    bit<32> amount;
    bit<1> subtract;
    bit<1> update_fc;
    bit<6> pad;
}

// Prepended only to an egress mirror clone used to synchronize PAUSE state.
header dpdq_sync_h {
    bit<16> magic;
    bit<16> port;
    bit<8> paused;
    bit<8> pad;
}

struct headers_t {
    dpdq_sync_h dpdq_sync;
    ethernet_h ethernet;
    ipv4_h ipv4;
    udp_h udp;
    dpdq_h dpdq;
    dpdq_internal_h dpdq_internal;
}

#endif
