# DPDQ — Network Simulation with ns-2

Network simulation framework supporting multiple transport protocols: SIRD, DPDQ, DPDQ-CC, XPass, DCTCP, pfabric, Homa, and Swift.

Built on ns-2.34 with the pFabric patch. Partially integrated with OMNET++ for INET protocol support.

## Quick Start

### One-script Installation

```bash
./setup.sh
```

### Manual Build (after source changes)

```bash
./build.sh
```

This runs `./configure && make clean && make -j8` in `ns2.34/ns-2.34`.

### Environment

Add to `~/.bashrc` (adjust paths):

```bash
export PATH="$PATH:<repo>/ns2.34/bin:<repo>/ns2.34/tcl8.4.18/unix"
export LD_LIBRARY_PATH="<repo>/ns2.34/otcl-1.13:<repo>/ns2.34/lib"
export TCL_LIBRARY="<repo>/ns2.34/tcl8.4.18/library"
```

### Dependencies

```bash
sudo apt install build-essential autoconf automake libxmu-dev bc
sudo apt install python3-numpy python3-matplotlib python3-pip
pip3 install psutil
```

For Ubuntu 20.04, gcc-4.8 is needed for nam only — add bionic repos if required.

---

## Running Experiments

All simulations are launched from `scripts/r2p2/coord/`.

### Example: Leaf-Spine with DPDQ (W4 topology)

```bash
cd scripts/r2p2/coord/
./run config/examples/leaf-spine-dpdq-w4.sh 1 1 1 1 1 &> out.txt
```

Run arguments (5 flags): `<config> <simulate> <post_process> <timeseries> <plots> <delete_existing>`

For background execution:

```bash
nohup ./run config/examples/leaf-spine-dpdq-w4.sh 1 1 1 1 1 &> out.txt &
```

### Example: Incast with DPDQ (Dumbbell topology)

```bash
./run config/examples/dpdq-simple-incast-bounce.sh 1 1 1 1 0 &> out.txt
```

### Available Example Configs

| Config | Description |
|--------|-------------|
| `leaf-spine-dpdq-w4.sh` | DPDQ, 9-leaf 4-spine, W4 workload, load 6.0 |
| `leaf-spine-dpdq-w3.sh` | DPDQ, 9-leaf 4-spine, W3 workload, load 9.0 |
| `leaf-spine-dpdqcc-w4.sh` | DPDQ-CC, 9-leaf 4-spine, W4 workload |
| `leaf-spine-dpdqcc-w3.sh` | DPDQ-CC, 9-leaf 4-spine, W3 workload |
| `leaf-spine-sird-w4-test.sh` | SIRD, 9-leaf 4-spine, W4 workload |
| `leaf-spine-dpdq-w4-test.sh` | DPDQ, 9-leaf 4-spine, W4 workload |

---

## Key Configuration Parameters

### Topology

| Parameter | Description | Example |
|-----------|-------------|---------|
| `topology_file_l` | YAML topology file in `config/topologies/` | `9leaf-4spine.yaml` |
| `num_clients` | Number of client hosts | `144` |
| `num_tors` | Number of ToR (leaf) switches | `9` |
| `num_spines` | Number of spine switches | `4` |
| `machines_per_tor` | Hosts per ToR | `16` |
| `leaf_link_speed_gbps` | Host-ToR link speed | `25` |
| `core_link_speed_gbps` | ToR-Spine link speed | `40` |
| `router_latency_us` | Per-hop switching latency | `0.01` |
| `switch_queue_type_l` | Queue type: `"per-port"` or `"shared"` | `per-port` |

### Traffic

| Parameter | Description | Example |
|-----------|-------------|---------|
| `client_injection_rate_gbps_list` | Per-client load (sweep list) | `"6.0"` |
| `mean_req_size_B_l` | Mean request size in bytes | `129297` |
| `req_size_distr` | Request size distribution | `w3`, `w4`, `w5` |
| `req_interval_distr` | Inter-request interval distribution | `exponential` |
| `req_target_distr` | Target server selection | `uniform`, `manual` |
| `req_size_distr` | Request size distribution | `w3`, `w4`, `manual` |
| `duration_modifier_l` | Simulation duration knob | `0.017` |
| `mean_req_size_B_l` | Average request (flow) size | `500000` |
| `mean_resp_size_B` | Average response size | `20` |
| `lognormal_sigma` | Sigma for lognormal distributions | `1.8` |

### Incast / Burst

| Parameter | Description | Example |
|-----------|-------------|---------|
| `oneshot_incast_enable_l` | Enable incast traffic (0/1) | `0` |
| `incast_period_s_l` | Incast period in seconds | `0.005` |
| `incast_burst_type_l` | 0=64-to-1 incast, 1=8x8 all-to-all | `1` |
| `incast_size_l` | Number of incast senders | `30` |
| `incast_request_size_bytes_l` | Incast message size | `500000` |
| `burst_req_size_B_l` | Burst request size (benchmark) | `51200` |

### DPDQ Queue (Credit/Data Dual Queue)

| Parameter | Description | Example |
|-----------|-------------|---------|
| `dpdq_data_queue_bytes_l` | Data queue limit in bytes | `161500` |
| `dpdq_credit_queue_bytes_l` | Credit (control) queue limit in bytes | `1024` |
| `dpdq_xoff_bytes_l` | XOFF threshold: pause when data queue exceeds this | `141500` |
| `dpdq_xon_bytes_l` | XON threshold: resume when data queue drops below this | `138500` |

**DPDQ relationship rule:** `dpdq_xoff = dpdq_data_queue - 20000`, `dpdq_xon = dpdq_xoff - 3000`.

### PPass Flow Control

| Parameter | Description | Example |
|-----------|-------------|---------|
| `ppass_pthresh_l` | Priority threshold: pause low-prio traffic when queue exceeds this | `81250` |
| `ppass_rho_l` | PPass rho parameter | `1` |
| `ppass_egress_queue_thresh_bytes_l` | Egress queue threshold | `3076` |

**Relationship:** `ppass_pthresh_l` should be 0.5 × `dpdq_data_queue_bytes_l`.

### R2P2 / SIRD Congestion Control

| Parameter | Description | Example |
|-----------|-------------|---------|
| `r2p2_cc_scheme` | CC scheme: `micro`, `micro-hybrid`, `noop` | `micro-hybrid` |
| `r2p2_budgets_intra_max_bytes_l` | Budget B (max sender budget) | `243750` |
| `r2p2_elet_srpb_l` | Unscheduled threshold per sender-receiver pair | `162500` |
| `r2p2_unsolicited_thresh_bytes_l` | Unsolicited threshold | `162500` |
| `r2p2_sender_ecn_threshold_l` | SThresh: sender-side ECN marking threshold (pkts) | `542` |
| `r2p2_ecn_threshold_min_l` | NThresh min: network ECN marking threshold (pkts) | `162` |
| `r2p2_ecn_threshold_max_l` | NThresh max: network ECN marking threshold (pkts) | `162` |
| `r2p2_elet_receiver_policy_l` | Receiver scheduling: 0=TS, 1=FIFO-sender, 2=FIFO-msg, 3=SRPT | `3` |
| `r2p2_hybrid_sender_policy_l` | Sender scheduling policy | `0` |
| `r2p2_sender_policy_ratio_l` | Sender policy ratio | `0` |
| `r2p2_sender_mark_aimd_l` | AIMD-style sender marking | `0` |
| `r2p2_resend_timeout_s_l` | Resend timeout in seconds | `0.004` |
| `r2p2_ecn_at_core_l` | ECN only at core links (1) or everywhere (0) | `1` |
| `r2p2_pace_grants_l` | Pace grant sending (0/1) | `1` |
| `r2p2_host_uplink_prio_l` | Host uplink priority queuing (0/1) | `1` |
| `r2p2_uplink_deque_policy_l` | Uplink dequeue: 0=FCFS, 1=RR_MSG, 2=RR_RECEIVER, 3=HIGHEST_PRIO | `0` |

### Homa

| Parameter | Description | Example |
|-----------|-------------|---------|
| `homa_workload_type` | Homa workload type | `5` |
| `homa_rtt_bytes` | RTT in bytes (BDP) | `100000` |
| `homa_all_prio_l` | Number of priority levels | `8` |
| `homa_adaptive_sched_prio_levels_l` | Adaptive sched priority levels | `7` |
| `homa_default_unsched_bytes_l` | Default unscheduled bytes | `homa_rtt_bytes - homa_default_req_bytes` |

### DCTCP

| Parameter | Description | Example |
|-----------|-------------|---------|
| `dctcp_g_l` | EWMA gain for ECN estimation | `0.08` |
| `dctcp_K_l` | ECN marking threshold (packets) | `7` |
| `dctcp_init_cwnd_l` | Initial congestion window (packets) | `5` |
| `dctcp_min_rto` | Minimum RTO (seconds) | `0.000200` |
| `general_queue_size_l` | Switch buffer size (packets) | `300000` |

### Tracing & Monitoring

| Parameter | Description | Example |
|-----------|-------------|---------|
| `trace_last_ratio` | Fraction of sim duration to trace | `1.0` |
| `trace_cc` | Trace CC internals (0/1) | `0` |
| `trace_application` | Trace application layer (0/1) | `1` |
| `state_polling_ival_s` | Queue sampling interval (seconds) | `0.000008` |
| `capture_pkt_trace` | Full per-packet trace (expensive) | `0` |
| `capture_msg_trace` | Per-message trace | `0` |
| `global_debug` | Debug verbosity (0-7) | `0` |

### Transport Selection

```bash
transp_prots='micro-hybrid'   # DPDQ / DPDQ-CC / SIRD (r2p2 transport + hybrid CC)
transp_prots='dctcp'          # DCTCP
transp_prots='pfabric'        # pfabric
transp_prots='homa'           # Homa
transp_prots='xpass'          # XPass
```

---

## Results

Results are placed in `scripts/r2p2/coord/results/<experiment>/data/<simulation_name>/<load>/`.

### Key Output Files

| File/Dir | Description |
|----------|-------------|
| `output/qts/tor/` | Throughput time series (ToR→Spine, per link) |
| `output/qts/aggr/` | Throughput time series (Spine→ToR, per link) |
| `output/app.csv` | Application-level slowdown metrics by message size group |
| `output/q.csv` | Queue occupancy metrics at node level |
| `output/qlink.csv` | Queue occupancy at link level |
| `output/qhist.csv` | Queue length histograms |
| `parameters` | All simulation parameters used |
| `applications_trace.str` | Per-flow start/completion trace |
| `send_events.csv` | Per-packet send events |
| `dpdq_drop_events.csv` | DPDQ drop events with credit/data classification |

### Analysis Scripts

| Script | Purpose |
|--------|---------|
| `analyze_spine_leaf_utilization.py` | Per-timestamp average spine-leaf link utilization → CSV |
| `analyze_benchmark_slowdown.py` | Leaf-spine incast slowdown analysis (sin events) |
| `analyze_dumbbell_slowdown.py` | Dumbbell slowdown analysis (srq events) |
| `analyze_sird_motivation.py` | SIRD benchmark: slowdown + loss per workload class |
| `analyze_dpdq_motivation.py` | DPDQ benchmark: slowdown + credit/data loss + queue CSV |
| `analyze_xpass_motivation.py` | XPass benchmark: slowdown + credit/data loss + queue CSV |

---

## Architecture

```
scripts/r2p2/
├── coord/                        # Experiment orchestration
│   ├── run                       # Main launch script
│   ├── config/
│   │   ├── common.sh             # Default parameter values
│   │   ├── util.sh               # Parameter handling (_l lists, set_parameter)
│   │   ├── topology_parser.py    # YAML topology → TCL conversion
│   │   └── examples/             # Experiment config files
│   └── results/                  # Output + analysis scripts
├── sim-scripts/r2p2cc/
│   ├── simulation.tcl            # Main ns-2 TCL config (leaf-spine)
│   └── benchmark.tcl             # ns-2 TCL config (dumbbell benchmark)
└── post-process/                 # Python post-processing modules

ns2.34/ns-2.34/
├── apps/r2p2-cc/                 # Transport protocol implementations
│   ├── r2p2/cc/micro-rpcs/       # SIRD/DPDQ CC logic (r2p2-cc-hybrid.cc)
│   └── r2p2/core/                # R2P2 transport core (r2p2-client.cc)
└── queue/
    └── dpdq-queue.{cc,h}         # DPDQQueue: credit/data dual queue

```

### Parameter Mechanism

Parameters use a `_l` suffix convention:
- `param_l='val1 val2'` — list, one value per transport protocol or sweep point
- `param_l='val'` — single value used for all runs
- `set_parameter` in `util.sh` reads `param_l`, sets `param` for the current index
- `required_params` in `common.sh` lists all params written to TCL

---

## Adding New Source Files

When adding new `.cc`/`.h` files to `ns2.34/ns-2.34/`:

1. Edit **`Makefile.in`** (not `Makefile` — `./configure` regenerates `Makefile`)
2. Add the `.o` file to the `OBJ` list
3. Run `./build.sh` to recompile
