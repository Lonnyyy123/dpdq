#!/bin/bash
# meant to be run from coord dir ..
source "config/examples/example-common.sh"

# ===================== General Settingss =====================
max_threads='24'

# ===================== Common Parameters =====================
topology_file_l='dumbbell64.yaml'
trace_last_ratio='1.0' # the % of the total simulation duration that will be traced / monitored
trace_cc='1'

state_polling_ival_s='0.000001' # sample switch queue lengths every 1us

req_interval_distr='manual'
req_target_distr='manual' # manual or uniform
req_size_distr='manual'
manual_req_interval_file_l='simple-incast-64pairs-dumbbell.csv'
client_injection_rate_gbps_list='60' # value is meaningful only when req_interval_distr is not 'manual'
duration_modifier_l='0.0004' # when manual, this specifies the sim duration in secods. For 3 1MB flows at 100Gbps, it takes approx 3*80us = 0.00024sec
mean_req_size_B_l='121848' # only meanigful if req_size_distr is not manual

# ===================== DCTCP Parameters =====================
dctcp_K_l="6" # DCTCP's ECN marking threshold in packets

# ===================== HOMA Parametrs =====================
homa_workload_type=5

# ===================== Protocols =====================
transp_prots='dctcp'
simulation_name_l='DCTCP-6'
