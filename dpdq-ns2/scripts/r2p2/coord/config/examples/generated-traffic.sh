#!/bin/bash
# meant to be run from coord dir ..
source "config/examples/example-common.sh"

# ===================== Temporary Replacements =====================

# ===================== General Settingss =====================
max_threads='24'
topology_file_l='4-hosts.yaml'
trace_last_ratio='0.2' # the % of the total simulation duration that will be traced / monitored
trace_cc='1'

# ===================== Common Parameters =====================
client_injection_rate_gbps_list="40 60"
duration_modifier_l="0.1"
mean_req_size_B_l='121848'
req_size_distr='w4'
req_interval_distr='exponential'
req_target_distr='uniform' # manual or uniform

# ===================== DCTCP Parameters =====================
dctcp_K_l="82 50 -1" # DCTCP's ECN marking threshold in packets

# ===================== HOMA Parametrs =====================
homa_workload_type=$(echo $req_size_distr | sed 's/[^0-9]*//g')

# ===================== Protocols =====================
transp_prots='dctcp dctcp micro-hybrid'
simulation_name_l='DCTCP-82 DCTCP-50 SIRD'

