#!/bin/bash
# meant to be run from coord dir ..
source "config/examples/example-common.sh"
tcp_connections_per_thread_pair='1'

# ===================== General Settingss =====================
max_threads='24'

# ===================== Common Parameters =====================
# topology_file_l='4-hosts.yaml'
topology_file_l='9leaf-4spine.yaml'
trace_last_ratio='1.0' # the % of the total simulation duration that will be traced / monitored
trace_cc='0'

state_polling_ival_s='0.000008'

# req_interval_distr='manual'
# req_target_distr='manual' # manual or uniform
# req_size_distr='manual'

client_injection_rate_gbps_list="9.0" # host<->tor load=0.9
duration_modifier_l="0.017"
mean_req_size_B_l='129297'
req_size_distr='w4'
general_queue_size_l='106'
# global_debug='0'
#r2p2_cc_debug='4'

# ===================== R2P2 Parameters =====================
# B = 1.5BDP = 243750B
r2p2_budgets_intra_max_bytes_l='243750'
# UnschT = 1BDP = 162500B
r2p2_elet_srpb_l='162500'
r2p2_unsolicited_thresh_bytes_l="162500"

r2p2_hybrid_sender_policy_l="0"
r2p2_sender_policy_ratio_l='0'
r2p2_elet_receiver_policy_l='3'
# SThresh = 5BDP = 812500B = 542pkt
r2p2_sender_ecn_threshold_l="542"

# this is invalid for UDP ppass packets
# NThresh = 1.5BDP = 243750B = 162pkts
r2p2_ecn_threshold_min_l='162' # packets
r2p2_ecn_threshold_max_l='162' # packets
r2p2_resend_timeout_s_l='0.004'

# PThresh = 1.5BDP = 243750B
ppass_pthresh_l='81250'
ppass_rho_l='1'

# 2pkt
ppass_egress_queue_thresh_bytes_l='3076'
# ===================== DCTCP Parameters =====================
# dctcp_K_l="7" # DCTCP's ECN marking threshold in packets
# dctcp_init_cwnd_l='108' # in packet
dpdq_xoff_bytes_l='159500000'
dpdq_xon_bytes_l='156500000'
r2p2_sender_mark_aimd_l='0'
dpdq_credit_queue_bytes_l='1024'
dpdq_data_queue_bytes_l='161500'
# ===================== HOMA Parametrs =====================
homa_workload_type=5

# ===================== Protocols =====================
transp_prots='micro-hybrid'
simulation_name_l='dpdqcc-ls-w4-fgata0.004-srpt'
oneshot_incast_enable_l='1'
incast_period_s_l='0.005' 
incast_burst_type_l='1'