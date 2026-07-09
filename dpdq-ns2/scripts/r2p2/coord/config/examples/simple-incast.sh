#!/bin/bash
# meant to be run from coord dir ..
source "config/examples/example-common.sh"

# ===================== General Settingss =====================
max_threads='24'

# ===================== Common Parameters =====================
# topology_file_l='4-hosts.yaml'
topology_file_l='dumbbell64.yaml'
trace_last_ratio='1.0' # the % of the total simulation duration that will be traced / monitored
trace_cc='1'

state_polling_ival_s='0.000001' # sample switch queue lengths every 1us

# req_interval_distr='manual'
# req_target_distr='manual' # manual or uniform
# req_size_distr='manual'
# manual_req_interval_file_l='simple-incast-6pairs-dumbbell.csv'

client_injection_rate_gbps_list="10"
duration_modifier_l="0.09"
mean_req_size_B_l='2927.354'
req_size_distr='w3'
# duration_modifier_l="0.4"
# mean_req_size_B_l='121848'
# req_size_distr='w4'

# ===================== R2P2 Parameters =====================
# B = 1.5BDP = 3222
r2p2_budgets_intra_max_bytes_l='3222'
r2p2_elet_srpb_l='2148'
# UnschT = 1BDP = 2148
r2p2_unsolicited_thresh_bytes_l="2148"
r2p2_hybrid_sender_policy_l="1"
r2p2_sender_policy_ratio_l='0.5'
# SThresh = 5BDP = 10740
r2p2_sender_ecn_threshold_l="7"
# NThresh = 1.25BDP = 2685
r2p2_ecn_threshold_min_l='2' # packets
r2p2_ecn_threshold_max_l='2' # packets

# PThresh = 1.25BDP = 2685
ppass_pthresh_l='2685'
# ===================== DCTCP Parameters =====================

# ===================== HOMA Parametrs =====================
homa_workload_type=5

# ===================== Protocols =====================
transp_prots='micro-hybrid'
simulation_name_l='ppass-w3'
