# Sets defaults for some experiment parameters. Will be overriden by 
# parameters set in the configuration file (see run script).
# WILL COMMENT OUT PARAMETERS THAT SHOULD USUALLY BE OVERWRITTEN (will cause fail-fast behavior)

# *_l parameters are lists of parameters. The lenght of the list must be the
# the same as the number of transp_prots. Alternatively, the length can be one, in which
# case, all simulations use the same value (goal is to eventually have all variables be *_l).
#
# For example:
# client_injection_rate_gbps_list='5 10 15 20 25 30 35' 
# transp_prots='micro micro dctcp dctcp'
# oversubscription_mul_l='1 2 4 8' (note: this param is deprecated)
#
# mean_req_size_B up to 40Megs
# The following experiments will be run (see "run" script)
# micro with 1:1 oversubscription at loads 5 10 15 20 25 30 35 (gbps per client)
# micro with 2:1 oversubscription at loads 5 10 15 20 25 30 35 (gbps per client)
# dctcp with 4:1 oversubscription at loads 5 10 15 20 25 30 35 (gbps per client)
# dctcp with 8:1 oversubscription at loads 5 10 15 20 25 30 35 (gbps per client)
# (not that this makes much sense)
# (client_injection_rate_gbps_list is handled uniquely - the rest (*_l) are combined using the same list index)

# client_injection_rate_gbps_list is only relevant when req_interval_distr is not 'manual'.
# In the latter case, it still functions in the same way but the numbers don't mean anything for the simulation

# These parameters are required. They are writen to a tcl file and simulation.tcl reads them (see write_tcl_params_to_file)
# If one of these is not defined, either in common.sh or in the specific config file, the simulation won't start
required_params="simulation_name root_dir vertigo_mac_files experiment_id_prefix experiment_id results_path transport_protocol mean_service_time_us
                 mean_req_size_B mean_resp_size_B req_interval_distr req_target_distr manual_req_interval_file resp_proc_time_distr 
                 req_size_distr resp_size_distr simple_trace_all trace_r2p2_layer_cl trace_r2p2_layer_sr trace_router 
                 trace_application trace_cc general_queue_size r2p2_cc_scheme cc_pkt_cap_sr_thresh router_latency_us 
                 queue_size_pfab tcp_connections_per_thread_pair pfab_g pfab_K pfab_min_rto client_injection_rate_gbps 
                 router_op_mode duration_modifier lognormal_sigma state_polling_ival_s 
                 q_mon_results_path urpc_sz_bytes single_path_per_msg dctcp_g dctcp_K 
                 dctcp_min_rto dctcp_init_cwnd tcp_ack_ratio r2p2_host_uplink_prio r2p2_busy_thresh_us 
                 r2p2_idle_thresh_us r2p2_throttle_mul r2p2_boost_mul r2p2_pace_uplink grant_gap_ns 
                 r2p2_uplink_deque_policy traffic_matrix_type traffic_matrix_ratio_interrack
                 r2p2_budgets_intra_max_bytes r2p2_ecn_at_core r2p2_ecn_threshold_min drop_tail_num_prios
                 r2p2_ecn_threshold_max r2p2_pace_grants r2p2_ce_new_weight 
                 r2p2_ecn_mechanism_influence r2p2_ecn_init_slash_mul r2p2_ecn_min_mul r2p2_ecn_min_mul_nw r2p2_pooled_sender_credit_bytes 
                 r2p2_sender_budget_pooled r2p2_elet_srpb r2p2_elet_receiver_policy r2p2_elet_account_unsched r2p2_unsolicited_thresh_bytes
                 r2p2_unsolicited_limit_senders r2p2_unsolicited_burst_when_idle
                 capture_pkt_trace homa_rtt_bytes homa_grant_max_bytes homa_all_prio homa_adaptive_sched_prio_levels 
                 homa_account_for_grant_traffic homa_unsched_prio_usage_weight homa_is_round_robin_scheduler 
                 homa_link_check_bytes homa_default_req_bytes homa_boost_tail_bytes_prio homa_use_unsch_rate_in_scheduler
                 global_debug app_debug full_tcp_debug r2p2_tranport_debug r2p2_udp_debug r2p2_router_debug r2p2_cc_debug
                 homa_transport_debug homa_udp_debug r2p2_sender_ecn_threshold homa_workload_type
                 homa_in_network_prios load_pattern lp_period lp_step_high_ratio lp_high_value_mul lp_low_value_mul
                 homa_default_unsched_bytes r2p2_hybrid_reset_after_x_rtt r2p2_hybrid_pace_grants
                 r2p2_hybrid_additive_incr_mul r2p2_hybrid_sender_policy r2p2_hybrid_sender_algo r2p2_hybrid_in_network_prios trace_last_ratio capture_msg_trace
                 r2p2_hybrid_eta r2p2_hybrid_wai r2p2_hybrid_max_stage switch_queue_type r2p2_priority_flow r2p2_data_prio r2p2_sender_mark_aimd r2p2_sender_policy_ratio
                 swift_base_target_delay swift_max_scaling_range swift_per_hop_scaling_factor swift_max_cwnd_target_scaling
                 swift_min_cwnd_target_scaling swift_additive_increase_constant swift_multiplicative_decrease_constant swift_max_mdf
                 oversub_topo incast_workload_ratio incast_size incast_request_size_bytes oneshot_incast_enable incast_period_s incast_burst_type ppass_pthresh ppass_rho ppass_egress_queue_thresh_bytes dpdq_xoff_bytes dpdq_xon_bytes dpdq_credit_queue_bytes dpdq_data_queue_bytes
                 r2p2_resend_timeout_s burst_req_size_B"


# ------------- Topology Parameters -------------
topology_file_l='default.yaml'
# traffic matrix: uniform: all-to-all, bimodal: inter-rack traffic percentage
# set independently (then uniformly random whithin each domain (rack, inter-rack))
traffic_matrix_type_l='uniform'
# Only valid with 'bimodal' trafic matrix type
traffic_matrix_ratio_interrack_l='0.8'
num_clients='144'
num_servers=$num_clients
num_tors='9'
num_spines='4'
# num_clients / machines_per_tor  must equal num_tors (integers)
machines_per_tor='16'
router_op_mode='random'

# ------------- PPass / DPDQ Flow Control -------------
ppass_pthresh_l='1'
ppass_rho_l='1'
ppass_egress_queue_thresh_bytes_l='1'
dpdq_xoff_bytes_l='1'
dpdq_xon_bytes_l='1'
dpdq_credit_queue_bytes_l='-1'
dpdq_data_queue_bytes_l='-1'


# load_pattern="fixed" 
lp_period="1"
lp_step_high_ratio="0.5"
lp_high_value_mul="1.5"
lp_low_value_mul="0.5"

# client_injection_rate_gbps_list should have integers
# ------------- R2P2 Parameters -------------
# (Note: udp dumps all the bytes in this queue at once)
# in packets

cc_pkt_cap_sr_thresh='25'
router_latency_us='0.01'
r2p2_pace_uplink_l='0' # Whether the uplinks of r2p2 hosts are paced using r2p2_uplink_deque_policy_l
r2p2_uplink_deque_policy_l='0' # 0 FCFS, 1 RR_MSG, 2 RR_RECEIVER, 3 HIGHEST_PRIO
r2p2_resend_timeout_s_l='-1'
incast_period_s_l='0'
incast_burst_type_l='0'
burst_req_size_B_l='51200'
r2p2_sender_mark_aimd_l='1'

# urpc_sz_bytes_l='30000'
# single_path_per_msg_l='0' # 1: all uRPCs follow the same path
# Host uplinks will dequeue_prio_ and keep_order_ (only DropTail)
r2p2_host_uplink_prio_l='0'
r2p2_pace_grants_l='1'
# R2P2_CC_MICRO/SCHD_DWNLNK/IDLE params
r2p2_busy_thresh_us_l='8'
r2p2_idle_thresh_us='8'
r2p2_throttle_mul_l='1.0'
r2p2_boost_mul='1.0'
grant_gap_ns_l='0' # extra gap between grants (additional to pacing)
# R2P2_CC_MICRO/SCHD_DWNLNK/IDLE/BUDGETS params
r2p2_sender_budget_pooled_l='1'  # Never implemented
r2p2_ecn_at_core_l='1' # 0 to disable - 1 to only mark at core links (tor<->spine)
# r2p2_ecn_threshold_min_l='35' # packets
# r2p2_ecn_threshold_max_l='35' # packets
# r2p2_sender_ecn_threshold_l='35' # packets
# r2p2_ce_new_weight_l='0.1'
# r2p2_ecn_mechanism_influence_l='1.0'
# r2p2_ecn_init_slash_mul_l='0.5'
# r2p2_ecn_min_mul_l='0.1'
r2p2_pooled_sender_credit_bytes_l='400000' # never implemented
# r2p2_elet_srpb_l='40000' # Per sender-receiver pair budget at receiver
# r2p2_elet_receiver_policy_l='0' # 0: TS among msgs, 1: fifo among senders, 2: fifo among messages

# ------------- TCP Paraeters -------------
tcp_ack_ratio='1'

# ------------- Pfabric Parameters -------------
# queue_size_pfab='24'
# pfab_g='0.0625'
pfab_K='10000' # useless, pfabric does not mark
# pfab_min_rto='0.000045'

# ------------- DCTCP Parametrs -------------
# DCTCP parameter configuration
# https://docs.google.com/document/d/1CcboZ8qyhrGq_kS1OLCgqa8YLnb6U17UoAPNZ4U6uWI/edit?usp=sharing
# general_queue_size_l='300' #pkts
# dctcp_g_l='0.0625'
# dctcp_K_l='35' #Packets - after tuning - see DCTCP config doc
# dctcp_min_rto='0.000100' 
# dctcp_init_cwnd_l='50' #(pkts) the bdp at 40Gbpps is 50pkts (RTT=15us)

# tcp_connections_per_thread_pair='10'

# ------------- HOMA Parametrs -------------
# homa_rtt_bytes='40000'
# homa_grant_max_bytes='1472'
# homa_all_prio='8'
# homa_adaptive_sched_prio_levels='6'
# homa_account_for_grant_traffic='1'
# homa_unsched_prio_usage_weight='1'
# homa_is_round_robin_scheduler='0'
# homa_link_check_bytes_l='-1'
# homa_default_req_bytes='1442'  # based on homa sim config
# homa_boost_tail_bytes_prio_l="$homa_rtt_bytes"
# homa_use_unsch_rate_in_scheduler='0'

# ------------- Monitoring/Tracing Parametrs -------------
simple_trace_all='0'
# 0.00001 10us || -1 to disable
state_polling_ival_s='0.001'



capture_pkt_trace='0' # capture every packet at every node. Expensive for large sims
