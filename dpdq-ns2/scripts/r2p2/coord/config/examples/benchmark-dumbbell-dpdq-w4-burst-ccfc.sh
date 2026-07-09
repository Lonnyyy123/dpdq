#!/bin/bash
# meant to be run from coord dir ..
source "config/examples/example-common.sh"
tcp_connections_per_thread_pair='1'

# ===================== General Settingss =====================
max_threads='4'
sim_file='benchmark.tcl'

# ===================== Topology =====================
topology_file_l='dumbbell34.yaml'

# ===================== Common Parameters =====================
trace_last_ratio='1.0'
trace_cc='1'
state_polling_ival_s='0.000008'

# s0-r0 uses w4 request distribution
client_injection_rate_gbps_list="10.0"
duration_modifier_l="0.04"
mean_req_size_B_l='129297'
req_size_distr='w4'
resp_size_distr='fixed'
mean_resp_size_B='4'

# Burst request size for other pairs (50kB / 100kB / 200kB)
burst_req_size_B_l='100000'

# ===================== R2P2 Parameters =====================
r2p2_budgets_intra_max_bytes_l='97500'
r2p2_elet_srpb_l='65000'
r2p2_unsolicited_thresh_bytes_l="1500"

r2p2_hybrid_sender_policy_l="0"
r2p2_sender_policy_ratio_l='0'
r2p2_elet_receiver_policy_l='3'
r2p2_sender_ecn_threshold_l="542"
r2p2_resend_timeout_s_l='0.004'

ppass_pthresh_l='32500'
ppass_rho_l='1'

ppass_egress_queue_thresh_bytes_l='-1'

dpdq_xoff_bytes_l='52000'
dpdq_xon_bytes_l='49000'
dpdq_credit_queue_bytes_l='1344'
dpdq_data_queue_bytes_l='64000'

incast_period_s_l='0.005'
incast_burst_type_l='0'
oneshot_incast_enable_l='0'
homa_workload_type=5
# ===================== Protocols =====================
transp_prots='micro-hybrid'
simulation_name_l='benchmark-dumbbell-dpdq-ccfc-100kB-h12kb'
