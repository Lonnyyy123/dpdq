#!/bin/bash
# meant to be run from coord dir ..
source "config/nsdi25/large-scale-common.default"

# ===================== Temporary Replacements =====================
r2p2_budgets_intra_max_bytes_l='100000 125000 150000 200000 300000 400000 100000 125000 150000 200000 300000 400000 100000 125000 150000 200000 300000 400000'
r2p2_sender_ecn_threshold_l="33 33 33 33 33 33 66 66 66 66 66 66 99999 99999 99999 99999 99999 99999"

# ===================== General Settingss =====================
max_threads='128'

# ===================== Common Parameters =====================
client_injection_rate_gbps_list="50 60 95"
duration_modifier_l="${w5_dur}"
mean_req_size_B_l='2515857.4'
req_size_distr='w5'

# ===================== HOMA Parametrs =====================
homa_workload_type=$(echo $req_size_distr | sed 's/[^0-9]*//g')

# ===================== Protocols =====================
transp_prots='micro-hybrid micro-hybrid micro-hybrid micro-hybrid micro-hybrid micro-hybrid micro-hybrid micro-hybrid micro-hybrid micro-hybrid micro-hybrid micro-hybrid micro-hybrid micro-hybrid micro-hybrid micro-hybrid micro-hybrid micro-hybrid'
simulation_name_l='1.0x_0.5 1.25x_0.5 1.5x_0.5 2x_0.5 3x_0.5 4x_0.5 1.0x_1.0 1.25x_1.0 1.5x_1.0 2x_1.0 3x_1.0 4x_1.0 1.0x_inf 1.25x_inf 1.5x_inf 2x_inf 3x_inf 4x_inf'
