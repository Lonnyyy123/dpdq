#include <core.p4>
#include <tna.p4>

#include "common/headers.p4"
#include "common/parsers.p4"

// Strict XOFF/ECN comparisons use threshold + 1 as the trigger value.
const bit<32> DPDQ_XOFF_TRIGGER_BYTES = 162501;
const bit<32> DPDQ_XON_BYTES = 142500;
const bit<32> DPDQ_ECN_TRIGGER_BYTES = 81251;
const bit<19> DPDQ_XOFF_TRIGGER_CELLS = 2033;
const bit<19> DPDQ_XON_CELLS = 1782;
const bit<19> DPDQ_ECN_TRIGGER_CELLS = 1016;
const bit<32> DPDQ_MAX_ELAPSED_NS = 16383;
const bit<16> DPDQ_APPROX_DATA_BYTES = 1500;
const bit<16> DPDQ_APPROX_SIGNAL_BYTES = 64;

// drain_bytes = elapsed_ns << DPDQ_DRAIN_SHIFT. The default is 4 B/ns.
const bit<8> DPDQ_DRAIN_SHIFT = 2;
const bit<5> DPDQ_CONTROL_QID = 0;
const bit<5> DPDQ_DATA_QID = 1;
const bit<3> DPDQ_MIRROR_TYPE_E2E = 2;
const MirrorId_t DPDQ_SYNC_MIRROR_SESSION = 1;

const bit<16> DPDQ_META_SHIM = 0x0800;
const bit<16> DPDQ_META_CLASS_DATA = 0x0400;
const bit<16> DPDQ_META_CLASS_SIGNAL = 0x0200;
const bit<16> DPDQ_META_PORT_MASK = 0x01ff;

struct ingress_md_t {
    bit<1> credit_handled;
    bit<1> fc_state;
}

struct egress_md_t {
    bit<32> qp_bytes;
    bit<32> qp_xoff_min;
    bit<32> qp_xon_min;
    bit<32> qp_ecn_min;
    bit<19> physical_xoff_min;
    bit<19> physical_xon_min;
    bit<19> physical_ecn_min;
    bit<2> fc_command;
    bit<1> fc_old_state;
    bit<1> fc_changed;
    MirrorId_t sync_session;
    bit<1> qp_update_pending;
}

parser IngressParser(
        packet_in pkt,
        out headers_t hdr,
        out ingress_md_t meta,
        out ingress_intrinsic_metadata_t ig_intr_md) {
    TofinoIngressPortParser() tofino_parser;
    LayerParser() packet_parser;
    value_set<bit<9>>(1) dpdq_recirc_ports;

    state start {
        tofino_parser.apply(pkt, ig_intr_md);
        transition select((bit<9>)ig_intr_md.ingress_port) {
            dpdq_recirc_ports: check_sync;
            default: parse_packet;
        }
    }

    state check_sync {
        transition select(pkt.lookahead<bit<16>>()) {
            DPDQ_SYNC_MAGIC: parse_sync;
            default: parse_packet;
        }
    }

    state parse_sync {
        pkt.extract(hdr.dpdq_sync);
        transition accept;
    }

    state parse_packet {
        packet_parser.apply(pkt, hdr);
        transition accept;
    }
}

control Ingress(
        inout headers_t hdr,
        inout ingress_md_t meta,
        in ingress_intrinsic_metadata_t ig_intr_md,
        in ingress_intrinsic_metadata_from_parser_t ig_prsr_md,
        inout ingress_intrinsic_metadata_for_deparser_t ig_dprsr_md,
        inout ingress_intrinsic_metadata_for_tm_t ig_tm_md) {

    // Tofino register widths are kept at 8 bits; only bit zero is used.
    Register<bit<8>, bit<9>>(512, 0) Ingress_Fc_State_Reg;

    RegisterAction<bit<8>, bit<9>, bit<1>>(Ingress_Fc_State_Reg)
            read_ingress_fc_action = {
        void apply(inout bit<8> value, out bit<1> result) {
            result = value[0:0];
        }
    };

    RegisterAction<bit<8>, bit<9>, bit<1>>(Ingress_Fc_State_Reg)
            write_ingress_pause_action = {
        void apply(inout bit<8> value, out bit<1> result) {
            value = 1;
            result = 1;
        }
    };

    RegisterAction<bit<8>, bit<9>, bit<1>>(Ingress_Fc_State_Reg)
            write_ingress_normal_action = {
        void apply(inout bit<8> value, out bit<1> result) {
            value = 0;
            result = 0;
        }
    };

    action forward(PortId_t port, mac_addr_t src_mac, mac_addr_t dst_mac) {
        hdr.ipv4.ttl = hdr.ipv4.ttl - 1;
        hdr.ethernet.src_addr = src_mac;
        hdr.ethernet.dst_addr = dst_mac;
        ig_tm_md.ucast_egress_port = port;
    }

    action drop() {
        ig_dprsr_md.drop_ctl = 0x1;
    }

    table ipv4_forward {
        key = {
            hdr.ipv4.dst_addr: exact;
        }
        actions = {
            forward;
            drop;
        }
        const default_action = drop();
        size = 1024;
    }

    action bounce_credit_to(PortId_t port) {
        mac_addr_t old_src_mac;
        ipv4_addr_t old_src;
        old_src_mac = hdr.ethernet.src_addr;
        hdr.ethernet.src_addr = hdr.ethernet.dst_addr;
        hdr.ethernet.dst_addr = old_src_mac;
        old_src = hdr.ipv4.src_addr;
        hdr.ipv4.src_addr = hdr.ipv4.dst_addr;
        hdr.ipv4.dst_addr = old_src;
        hdr.dpdq.flags = hdr.dpdq.flags | DPDQ_FLAG_BOUNCED;
        hdr.dpdq.reserved = (bit<16>)port;
        meta.credit_handled = 1;
        hdr.udp.checksum = 0;
        ig_tm_md.ucast_egress_port = port;
        ig_tm_md.qid = DPDQ_CONTROL_QID;
    }

    action accept_credit() {
        hdr.dpdq.reserved =
            (bit<16>)ig_intr_md.ingress_port | DPDQ_META_SHIM;
        hdr.dpdq_internal.setValid();
        hdr.dpdq_internal.amount = hdr.dpdq.credit;
        hdr.dpdq_internal.subtract = 0;
        hdr.dpdq_internal.update_fc = 0;
        hdr.dpdq_internal.pad = 0;
        meta.credit_handled = 1;
        ig_tm_md.qid = DPDQ_CONTROL_QID;
    }

    action accept_remote_bounce() {
        hdr.dpdq.reserved =
            (bit<16>)ig_tm_md.ucast_egress_port | DPDQ_META_SHIM;
        hdr.dpdq_internal.setValid();
        hdr.dpdq_internal.amount = hdr.dpdq.credit;
        hdr.dpdq_internal.subtract = 1;
        hdr.dpdq_internal.update_fc = 1;
        hdr.dpdq_internal.pad = 0;
        meta.credit_handled = 1;
        ig_tm_md.qid = DPDQ_CONTROL_QID;
    }

    action classify_plain() {
        hdr.dpdq.reserved = (bit<16>)ig_tm_md.ucast_egress_port;
    }

    action classify_data() {
        hdr.dpdq.reserved =
            (bit<16>)ig_tm_md.ucast_egress_port | DPDQ_META_CLASS_DATA;
    }

    action classify_signal() {
        hdr.dpdq.reserved =
            (bit<16>)ig_tm_md.ucast_egress_port | DPDQ_META_CLASS_SIGNAL;
        ig_tm_md.qid = DPDQ_CONTROL_QID;
    }

    table packet_classify {
        key = {
            hdr.dpdq.msg_type: exact;
            hdr.dpdq.flags[1:1]: exact;
        }
        actions = {
            classify_plain;
            classify_data;
            classify_signal;
        }
        default_action = classify_plain();
        size = 8;
    }

    apply {
        bit<1> should_forward;

        meta.credit_handled = 0;
        meta.fc_state = 0;
        ig_tm_md.qid = DPDQ_DATA_QID;
        should_forward = 0;

        if (hdr.dpdq_sync.isValid()) {
            if (hdr.dpdq_sync.paused[0:0] == 1w1) {
                meta.fc_state = write_ingress_pause_action.execute(
                    hdr.dpdq_sync.port[8:0]);
            } else {
                meta.fc_state = write_ingress_normal_action.execute(
                    hdr.dpdq_sync.port[8:0]);
            }
            drop();
        }

        if (hdr.ipv4.isValid()) {
            should_forward = 1;
        }

        if (hdr.dpdq.isValid()) {
            if (hdr.dpdq.msg_type == DPDQ_GRANT &&
                    (hdr.dpdq.flags & DPDQ_FLAG_BOUNCED) == 0) {
                meta.fc_state = read_ingress_fc_action.execute(
                    (bit<9>)ig_intr_md.ingress_port);
                if (meta.fc_state == 1w1) {
                    bounce_credit_to(ig_intr_md.ingress_port);
                    should_forward = 0;
                } else {
                    accept_credit();
                }
            }
        }

        if (should_forward == 1w1 && hdr.ipv4.isValid()) {
            ipv4_forward.apply();
        }
        if (hdr.dpdq.isValid() &&
                meta.credit_handled == 0 &&
                hdr.dpdq.msg_type == DPDQ_GRANT &&
                (hdr.dpdq.flags & DPDQ_FLAG_BOUNCED) != 0) {
            accept_remote_bounce();
        }

        if (hdr.dpdq.isValid() &&
                meta.credit_handled == 0) {
            packet_classify.apply();
        }
    }
}

control IngressDeparser(
        packet_out pkt,
        inout headers_t hdr,
        in ingress_md_t meta,
        in ingress_intrinsic_metadata_for_deparser_t ig_dprsr_md) {
    Checksum() ipv4_checksum;

    apply {
        if (hdr.ipv4.isValid()) {
            hdr.ipv4.hdr_checksum = ipv4_checksum.update({
                hdr.ipv4.version,
                hdr.ipv4.ihl,
                hdr.ipv4.diffserv,
                hdr.ipv4.total_len,
                hdr.ipv4.identification,
                hdr.ipv4.flags,
                hdr.ipv4.frag_offset,
                hdr.ipv4.ttl,
                hdr.ipv4.protocol,
                hdr.ipv4.src_addr,
                hdr.ipv4.dst_addr
            });
        }
        pkt.emit(hdr);
    }
}

parser EgressParser(
        packet_in pkt,
        out headers_t hdr,
        out egress_md_t meta,
        out egress_intrinsic_metadata_t eg_intr_md) {
    LayerParser() packet_parser;

    state start {
        pkt.extract(eg_intr_md);
        packet_parser.apply(pkt, hdr);
        transition accept;
    }
}

control Egress(
        inout headers_t hdr,
        inout egress_md_t meta,
        in egress_intrinsic_metadata_t eg_intr_md,
        in egress_intrinsic_metadata_from_parser_t eg_prsr_md,
        inout egress_intrinsic_metadata_for_deparser_t eg_dprsr_md,
        inout egress_intrinsic_metadata_for_output_port_t eg_oport_md) {

    action compute_reduce_bytes() {
        hdr.dpdq_internal.amount =
            hdr.dpdq_internal.amount << DPDQ_DRAIN_SHIFT;
    }

    action cap_elapsed_time() {
        hdr.dpdq_internal.amount =
            min(DPDQ_MAX_ELAPSED_NS, hdr.dpdq_internal.amount);
    }

    action compute_data_net_add() {
        hdr.dpdq_internal.amount = (bit<32>)(
            DPDQ_APPROX_DATA_BYTES - hdr.dpdq_internal.amount[15:0]);
        hdr.dpdq_internal.subtract = 0;
    }

    action compute_data_net_subtract() {
        hdr.dpdq_internal.amount = (bit<32>)(
            hdr.dpdq_internal.amount[15:0] - DPDQ_APPROX_DATA_BYTES);
        hdr.dpdq_internal.subtract = 1;
    }

    action compute_signal_net_add() {
        hdr.dpdq_internal.amount = (bit<32>)(
            DPDQ_APPROX_SIGNAL_BYTES - hdr.dpdq_internal.amount[15:0]);
        hdr.dpdq_internal.subtract = 0;
    }

    action compute_signal_net_subtract() {
        hdr.dpdq_internal.amount = (bit<32>)(
            hdr.dpdq_internal.amount[15:0] - DPDQ_APPROX_SIGNAL_BYTES);
        hdr.dpdq_internal.subtract = 1;
    }

    action prepare_sync_constants() {
        meta.sync_session = DPDQ_SYNC_MIRROR_SESSION;
        hdr.dpdq_sync.setValid();
        hdr.dpdq_sync.magic = DPDQ_SYNC_MAGIC;
        hdr.dpdq_sync.pad = 0;
        eg_dprsr_md.mirror_type = DPDQ_MIRROR_TYPE_E2E;
    }

    action prepare_sync_port() {
        hdr.dpdq_sync.port = (bit<16>)hdr.dpdq.reserved[8:0];
    }

    action prepare_sync_state() {
        hdr.dpdq_sync.paused = (bit<8>)meta.fc_command[0:0];
    }

    table reduce_bytes_table {
        actions = {
            compute_reduce_bytes;
        }
        default_action = compute_reduce_bytes();
        size = 1;
    }

    table cap_elapsed_table {
        actions = {
            cap_elapsed_time;
        }
        default_action = cap_elapsed_time();
        size = 1;
    }

    table approximate_effect_table {
        key = {
            hdr.dpdq.reserved[10:9]: exact;
            hdr.dpdq_internal.amount[15:0]: range;
        }
        actions = {
            compute_data_net_add;
            compute_data_net_subtract;
            compute_signal_net_add;
            compute_signal_net_subtract;
            NoAction;
        }
        default_action = NoAction();
        size = 4;
    }

    action compute_threshold_mins() {
        meta.qp_xoff_min = min(DPDQ_XOFF_TRIGGER_BYTES, meta.qp_bytes);
        meta.qp_xon_min = min(DPDQ_XON_BYTES, meta.qp_bytes);
        meta.qp_ecn_min = min(DPDQ_ECN_TRIGGER_BYTES, meta.qp_bytes);
        meta.physical_xoff_min =
            min(DPDQ_XOFF_TRIGGER_CELLS, eg_intr_md.deq_qdepth);
        meta.physical_xon_min =
            min(DPDQ_XON_CELLS, eg_intr_md.deq_qdepth);
        meta.physical_ecn_min =
            min(DPDQ_ECN_TRIGGER_CELLS, eg_intr_md.deq_qdepth);
    }

    table threshold_min_table {
        actions = {
            compute_threshold_mins;
        }
        default_action = compute_threshold_mins();
        size = 1;
    }

    Register<bit<8>, bit<9>>(512, 0) Port_Fc_State_Reg;
    Register<bit<32>, bit<9>>(512, 0) Timestamp_Reg;
    Register<bit<32>, bit<9>>(512, 0) Port_Qp_Reg;

    RegisterAction<bit<32>, bit<9>, bit<32>>(Timestamp_Reg)
            update_timestamp_action = {
        void apply(inout bit<32> value, out bit<32> result) {
            if (value == 0) {
                result = 0;
            } else {
                result = eg_prsr_md.global_tstamp[31:0] - value;
            }
            value = eg_prsr_md.global_tstamp[31:0];
        }
    };

    RegisterAction<bit<32>, bit<9>, bit<32>>(Port_Qp_Reg)
            add_qp_action = {
        void apply(inout bit<32> value, out bit<32> result) {
            value = value + hdr.dpdq_internal.amount;
            result = value;
        }
    };

    RegisterAction<bit<32>, bit<9>, bit<32>>(Port_Qp_Reg)
            subtract_qp_action = {
        void apply(inout bit<32> value, out bit<32> result) {
            if (value < hdr.dpdq_internal.amount) {
                value = 0;
            } else {
                value = value - hdr.dpdq_internal.amount;
            }
            result = value;
        }
    };

    RegisterAction<bit<8>, bit<9>, bit<1>>(Port_Fc_State_Reg)
            update_fc_state_action = {
        void apply(inout bit<8> value, out bit<1> result) {
            result = value[0:0];
            if (meta.fc_command == 1) {
                value = 1;
            } else if (meta.fc_command == 2) {
                value = 0;
            }
        }
    };

    apply {
        meta.qp_bytes = 0;
        meta.qp_xoff_min = 0;
        meta.qp_xon_min = 0;
        meta.qp_ecn_min = 0;
        meta.physical_xoff_min = 0;
        meta.physical_xon_min = 0;
        meta.physical_ecn_min = 0;
        meta.fc_command = 0;
        meta.fc_old_state = 0;
        meta.fc_changed = 0;
        meta.sync_session = 0;
        meta.qp_update_pending = 0;

        if (hdr.ipv4.isValid() &&
                hdr.dpdq.isValid() &&
                hdr.dpdq_internal.isValid()) {
            meta.qp_update_pending = 1;
        } else if (hdr.ipv4.isValid() &&
                hdr.dpdq.isValid() &&
                !hdr.dpdq_internal.isValid() &&
                hdr.dpdq.msg_type != DPDQ_GRANT) {
            hdr.dpdq_internal.setValid();
            hdr.dpdq_internal.amount = update_timestamp_action.execute(
                hdr.dpdq.reserved[8:0]);
            hdr.dpdq_internal.subtract = 1;
            hdr.dpdq_internal.update_fc = 1;
            hdr.dpdq_internal.pad = 0;
            cap_elapsed_table.apply();
            reduce_bytes_table.apply();
            approximate_effect_table.apply();
            meta.qp_update_pending = 1;
        }

        if (meta.qp_update_pending == 1) {
            if (hdr.dpdq_internal.subtract == 1w1) {
                meta.qp_bytes = subtract_qp_action.execute(
                    hdr.dpdq.reserved[8:0]);
            } else {
                meta.qp_bytes = add_qp_action.execute(
                    hdr.dpdq.reserved[8:0]);
            }
            if (hdr.dpdq_internal.update_fc == 1w1) {
                threshold_min_table.apply();
                if (meta.qp_xoff_min == DPDQ_XOFF_TRIGGER_BYTES) {
                    meta.fc_command = 1;
                } else if (meta.physical_xoff_min ==
                        DPDQ_XOFF_TRIGGER_CELLS) {
                    meta.fc_command = 1;
                } else if (meta.qp_xon_min != DPDQ_XON_BYTES) {
                    if (meta.physical_xon_min != DPDQ_XON_CELLS) {
                        meta.fc_command = 2;
                    }
                }
                meta.fc_old_state = update_fc_state_action.execute(
                    hdr.dpdq.reserved[8:0]);
                if (meta.fc_command == 1 &&
                        meta.fc_old_state == 0) {
                    meta.fc_changed = 1;
                } else if (meta.fc_command == 2 &&
                        meta.fc_old_state == 1) {
                    meta.fc_changed = 1;
                }
                if (meta.fc_changed == 1w1) {
                    prepare_sync_constants();
                    prepare_sync_port();
                    prepare_sync_state();
                }

                if (hdr.dpdq.msg_type == DPDQ_REQUEST ||
                        hdr.dpdq.msg_type == DPDQ_REPLY) {
                    if (meta.qp_ecn_min == DPDQ_ECN_TRIGGER_BYTES) {
                        hdr.dpdq.flags = hdr.dpdq.flags |
                            DPDQ_FLAG_NETWORK_MARKED;
                        hdr.ipv4.diffserv[1:0] = 2w3;
                    } else if (meta.physical_ecn_min ==
                            DPDQ_ECN_TRIGGER_CELLS) {
                        hdr.dpdq.flags = hdr.dpdq.flags |
                            DPDQ_FLAG_NETWORK_MARKED;
                        hdr.ipv4.diffserv[1:0] = 2w3;
                    }
                }
            }
            hdr.dpdq_internal.setInvalid();
        }

        if (hdr.dpdq.isValid()) {
            hdr.dpdq.reserved = 0;
        }
    }
}

control EgressDeparser(
        packet_out pkt,
        inout headers_t hdr,
        in egress_md_t meta,
        in egress_intrinsic_metadata_for_deparser_t eg_dprsr_md) {
    Checksum() ipv4_checksum;
    Mirror() mirror;

    apply {
        if (eg_dprsr_md.mirror_type == DPDQ_MIRROR_TYPE_E2E) {
            mirror.emit<dpdq_sync_h>(meta.sync_session, {
                hdr.dpdq_sync.magic,
                hdr.dpdq_sync.port,
                hdr.dpdq_sync.paused,
                hdr.dpdq_sync.pad
            });
        }
        if (hdr.ipv4.isValid()) {
            hdr.ipv4.hdr_checksum = ipv4_checksum.update({
                hdr.ipv4.version,
                hdr.ipv4.ihl,
                hdr.ipv4.diffserv,
                hdr.ipv4.total_len,
                hdr.ipv4.identification,
                hdr.ipv4.flags,
                hdr.ipv4.frag_offset,
                hdr.ipv4.ttl,
                hdr.ipv4.protocol,
                hdr.ipv4.src_addr,
                hdr.ipv4.dst_addr
            });
        }
        // dpdq_sync is mirror metadata and must not prefix the original packet.
        pkt.emit(hdr.ethernet);
        pkt.emit(hdr.ipv4);
        pkt.emit(hdr.udp);
        pkt.emit(hdr.dpdq);
        pkt.emit(hdr.dpdq_internal);
    }
}

Pipeline(
    IngressParser(),
    Ingress(),
    IngressDeparser(),
    EgressParser(),
    Egress(),
    EgressDeparser()
) pipe;

Switch(pipe) main;
