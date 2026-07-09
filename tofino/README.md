# DPDQ on Intel Tofino 1

This directory contains a TNA/P4_16 implementation of the DPDQ switch-side
logic for Intel Tofino 1. The code is a compact research prototype: it keeps
the main DPDQ control loop while making the approximations needed to fit a
single Tofino 1 pipeline.

The implementation has been compiled and installed on a Tofino 1 testbed with
Intel/Barefoot SDE 9.13.3. It uses TNA intrinsic metadata, Tofino register
semantics, mirror sessions, parser value sets, and traffic-manager queues; it is
not portable to BMv2 or PSA targets without rewriting those target-specific
parts.

## Overview

DPDQ maintains a per-port phantom queue (`Qp`) and compares it with the
hardware egress queue depth (`Qd`) to make three decisions:

- bounce incoming credit packets when the ingress-side PAUSE replica says the
  ingress port is paused;
- switch a port between PAUSE and NORMAL using independent XOFF/XON thresholds;
- mark data packets with ECN using an independent ECN threshold.

This implementation deliberately has only one authoritative `Qp` state:

- `Egress.Port_Qp_Reg` is the only phantom-queue register;
- ingress keeps only a replicated PAUSE bit in `Ingress.Ingress_Fc_State_Reg`;
- egress synchronizes PAUSE transitions back to ingress with a short E2E mirror
  clone sent to a configured recirculation port.

This avoids the consistency problems of maintaining both ingress and egress
phantom queues. A locally bounced credit never carries an internal Qp update,
so credit that was not accepted cannot later subtract from `Qp`.

## Repository Layout

```text
tofino/dpdq/
|-- dpdq.p4              # TNA ingress/egress pipeline
|-- common/
|   |-- headers.p4       # Ethernet/IPv4/UDP/DPDQ header definitions
|   `-- parsers.p4       # parser helpers and target metadata
`-- README.md
```

## Pipeline Behavior

### Ingress

Ingress performs parsing, IPv4 forwarding, credit bounce decisions, and packet
classification.

For an original `GRANT` packet:

- if the ingress PAUSE replica is PAUSED, ingress swaps the packet back to the
  sender, sets `DPDQ_FLAG_BOUNCED`, sends it through control queue 0, and emits
  no internal Qp operation;
- if the ingress PAUSE replica is NORMAL, ingress forwards the packet normally
  and emits an internal shim saying `Qp[ingress_port] += credit`.

For a bounced `GRANT` packet received from the network:

- ingress forwards it normally;
- egress performs a saturated `Qp[egress_port] -= credit` on the packet's
  actual output port.

For data and signal packets:

- ingress classifies unscheduled data and signal packets for approximate
  transmit-side Qp accounting;
- control packets (`GRANT`, `GRANT_REQ`, `RESEND`) use logical queue 0;
- data packets use logical queue 1.

### Egress

Egress owns all phantom-queue state and flow-control state.

On each packet carrying a DPDQ internal shim, egress:

- updates the selected `Qp` register entry;
- optionally evaluates XOFF/XON/ECN for the actual egress port;
- emits a PAUSE synchronization clone only when the port state changes;
- removes internal metadata before the packet leaves the switch.

Business packets are not recirculated. Only PAUSE synchronization clones use
the configured recirculation port.

## Thresholds

The default thresholds are defined in `dpdq.p4`:

| Decision | Phantom queue threshold | Physical queue threshold |
| --- | ---: | ---: |
| XOFF | `> 162,500 bytes` | `>= 2,033 cells` |
| XON | `< 142,500 bytes` | `< 1,782 cells` |
| ECN | `> 81,250 bytes` | `>= 1,016 cells` |

XON is 20,000 bytes below XOFF to preserve hysteresis. ECN is approximately
half of XOFF and remains independent of PAUSE transitions.

Threshold checks use `min(threshold, value)` and compare the result with the
constant threshold. This preserves separate XOFF, XON, and ECN semantics while
avoiding unsupported dynamic-wide comparisons on Tofino 1.

## Packet Format

DPDQ packets are carried after UDP destination port `9001`.

| Field | Width |
| --- | ---: |
| `msg_type` | 8 bits |
| `flags` | 8 bits |
| `reserved` | 16 bits |
| `credit` | 32 bits |
| `seq` | 32 bits |
| `resend_bytes` | 32 bits |
| `flow_id` | 32 bits |

Only the prefix through `credit` is parsed into PHV. The remaining fields stay
in packet payload and are preserved on the wire.

Message type values:

| Name | Value |
| --- | ---: |
| `REQUEST` | `0` |
| `REPLY` | `1` |
| `GRANT` | `8` |
| `GRANT_REQ` | `9` |
| `RESEND` | `10` |

Flag values:

| Name | Value |
| --- | ---: |
| `DPDQ_FLAG_BOUNCED` | `0x01` |
| `DPDQ_FLAG_UNSCHEDULED` | `0x02` |
| `DPDQ_FLAG_NETWORK_MARKED` | `0x04` |

Host-side serialization must match this compact header layout.

## Build

Use the compiler shipped with the target switch's P4 Studio SDE.

```bash
cd tofino/dpdq
mkdir -p build

$SDE_INSTALL/bin/bf-p4c \
  --std p4-16 \
  --target tofino \
  --arch tna \
  -I . \
  -o build/dpdq \
  dpdq.p4
```

Some deployments use a CMake/Make wrapper from P4 Studio instead of invoking
`bf-p4c` directly. The tested setup used SDE 9.13.3 on Tofino 1.

## Control-Plane Setup

Before sending traffic, configure the following switch state.

1. Populate `Ingress.ipv4_forward` with destination IPv4 address, egress port,
   and MAC rewrite parameters.
2. Add the dedicated recirculation ingress port to
   `IngressParser.dpdq_recirc_ports`. Only this port should parse sync headers.
3. Configure E2E mirror session `1` to output to the recirculation port. A
   short clone length is sufficient because ingress consumes only the sync
   header and drops the clone.
4. Populate `Ingress.packet_classify`:
   `REQUEST`/`REPLY` with `DPDQ_FLAG_UNSCHEDULED` select `classify_data`;
   `GRANT_REQ`/`RESEND` select `classify_signal`;
   all other cases use `classify_plain`.
5. Populate `Egress.approximate_effect_table` with four range entries:
   DATA and drain `0..1499` select `compute_data_net_add`;
   DATA and drain `1500..65535` select `compute_data_net_subtract`;
   SIGNAL and drain `0..63` select `compute_signal_net_add`;
   SIGNAL and drain `64..65535` select `compute_signal_net_subtract`.
6. Configure TM logical queue 0 as strict priority above logical queue 1.
   Setting `qid` in P4 only chooses the queue; it does not configure TM
   scheduling.
7. Initialize `Ingress_Fc_State_Reg`, `Port_Fc_State_Reg`, `Timestamp_Reg`, and
   `Port_Qp_Reg` to zero.

## Tofino-Specific Notes

Ingress and egress stateful memories cannot be updated atomically from the same
packet-processing step. This implementation therefore treats egress as the
owner of real flow-control state and mirrors only PAUSE transitions back to
ingress.

The ingress PAUSE replica can lag by one mirror-loop delay:

- a stale NORMAL can accept one extra credit;
- a stale PAUSE can bounce one extra credit.

Neither case corrupts `Qp`: accepted credits carry an add operation, while
locally bounced credits carry no Qp operation.

`dpdq_internal { amount, subtract, update_fc }` is the only internal
description of a Qp update. `update_fc=0` is used for accepted credit because
the packet updates `Qp[ingress_port]` while it may physically leave through a
different egress port. Transmit-side updates use `update_fc=1`, compare the
actual egress port's Qp and physical queue depth, and synchronize that port's
PAUSE state if needed.

## Approximation Boundaries

This is not a cycle-accurate software model of DPDQ. The following compromises
are deliberate hardware-fit choices:

- elapsed time is capped at `16,383 ns`;
- drain is computed as `elapsed_ns << DPDQ_DRAIN_SHIFT`;
- `DPDQ_DRAIN_SHIFT=2` means `4 B/ns` and must be adjusted for the target link
  rate and chosen `rho`;
- unscheduled data is accounted as `1,500 bytes`;
- signal packets are accounted as `64 bytes`;
- the single net update approximates `max(0, Qp - drain) + packet_size`, and
  differs when `Qp` is smaller than the computed drain;
- accepted credits and locally bounced credits omit separate transmit-side
  packet-size accounting to avoid credit recirculation;
- the phantom queue uses a 32-bit register and intentionally has no artificial
  saturation cap.

These trade-offs keep the program within Tofino 1 resource and language
constraints while preserving the core DPDQ control loop.

## License

No license is declared in this directory yet. Add a repository-level license
before publishing if you want others to reuse the code.
