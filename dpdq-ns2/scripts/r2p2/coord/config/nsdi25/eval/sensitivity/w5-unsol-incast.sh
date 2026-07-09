#!/bin/bash
# meant to be run from coord dir ..
source "config/nsdi25/large-scale-common.default"

# ===================== Temporary Replacements =====================
r2p2_unsolicited_thresh_bytes_l="1400 100000 200000 400000 1600000 -1"
incast_workload_ratio_l="0.07" # 0.07
# ===================== General Settingss =====================
max_threads='64'

# ===================== Common Parameters =====================
# client_injection_rate_gbps_list="25 50 60 70 95"
client_injection_rate_gbps_list="50 60 70 95"
duration_modifier_l="${w5_dur}"
mean_req_size_B_l='2515857.4'
req_size_distr='w5'

# ===================== HOMA Parametrs =====================
homa_workload_type=$(echo $req_size_distr | sed 's/[^0-9]*//g')

# ===================== Protocols =====================
transp_prots='micro-hybrid micro-hybrid micro-hybrid micro-hybrid micro-hybrid micro-hybrid'
simulation_name_l='PKT BDP 2xBDP 4xBDP 16xBDP all'
