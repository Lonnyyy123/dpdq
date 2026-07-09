#!/bin/bash
# meant to be run from coord dir ..
source "config/examples/example-common.sh"

# ===================== General Settings =====================
max_threads='24'

# ===================== Common Parameters =====================
topology_file_l='dumbbell6.yaml'
trace_last_ratio='1'
trace_cc='1'
state_polling_ival_s='0.000008'
global_debug='0'
r2p2_cc_debug='5'

client_injection_rate_gbps_list="10"
duration_modifier_l="0.00005"

mean_req_size_B_l='2927.354'
req_size_distr='w3'
general_queue_size_l='2'

# ===================== R2P2 / DPDQ Parameters =====================
r2p2_budgets_intra_max_bytes_l='3222'
r2p2_elet_srpb_l='2148'
r2p2_unsolicited_thresh_bytes_l="2148"
r2p2_hybrid_sender_policy_l="1"
r2p2_sender_policy_ratio_l='0.5'
r2p2_sender_ecn_threshold_l="7"
r2p2_ecn_threshold_min_l='2'
r2p2_ecn_threshold_max_l='2'
r2p2_resend_timeout_s_l='-1'

ppass_pthresh_l='26850'
ppass_rho_l='1'
ppass_egress_queue_thresh_bytes_l='3076'
dpdq_xoff_bytes_l='1000000000'
dpdq_xon_bytes_l='999999999'

# ===================== HOMA Parameters =====================
homa_workload_type=5

# ===================== Protocols =====================
transp_prots='micro-hybrid'
simulation_name_l='dpdq-resend-smoke'
