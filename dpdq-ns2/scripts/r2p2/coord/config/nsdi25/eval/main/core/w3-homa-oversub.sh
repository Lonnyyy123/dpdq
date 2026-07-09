#!/bin/bash
# meant to be run from coord dir ..
source "config/nsdi25/large-scale-common-oversub.default"

# ===================== Temporary Replacements =====================
homa_adaptive_sched_prio_levels_l='5'

# ===================== General Settingss =====================
max_threads='24'

# ===================== Common Parameters =====================
client_injection_rate_gbps_list="$w3_loads"
duration_modifier_l="${w3_dur}"
mean_req_size_B_l='2927.354'
req_size_distr='w3'

# ===================== HOMA Parametrs =====================
homa_workload_type=$(echo $req_size_distr | sed 's/[^0-9]*//g')

# ===================== Protocols =====================
transp_prots='homa'
simulation_name_l='Homa'
