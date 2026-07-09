#!/bin/bash
# meant to be run from coord dir ..
source "config/nsdi25/large-scale-common.default"

# ===================== Temporary Replacements =====================
r2p2_hybrid_in_network_prios_l="0 1 1"
r2p2_data_prio_l="0 0 1"
# ===================== General Settingss =====================
max_threads='24'

# ===================== Common Parameters =====================
client_injection_rate_gbps_list="50 60 70 95"
duration_modifier_l="${w4_dur}"
mean_req_size_B_l='121848'
req_size_distr='w4'

# ===================== HOMA Parametrs =====================
homa_workload_type=$(echo $req_size_distr | sed 's/[^0-9]*//g')

# ===================== Protocols =====================
transp_prots='micro-hybrid micro-hybrid micro-hybrid'
simulation_name_l='SIRD-no-prio SIRD-control-prio SIRD-data-prio'
