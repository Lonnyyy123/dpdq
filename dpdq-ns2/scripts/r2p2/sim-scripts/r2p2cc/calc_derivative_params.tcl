set script_path [ file dirname [ file normalize [ info script ] ] ]

# Utilities
source "$script_path/../util.tcl"
# Needed for dict support in tcl 8.4
source "$script_path/dict.tcl" 
# Common variables
source "$script_path/../common.tcl"
set script_params_file [lindex $argv 0]

if {[file exists $script_params_file]} {
    source $script_params_file
} else {
    puts "A generated parameters file must be provided."
    exit 1
}



# ------------------- Load calculations -------------------

# Split load among incast and background
set incast_injection_rate_gbps [expr $incast_workload_ratio * $client_injection_rate_gbps]
set background_injection_rate_gbps [expr (1 - $incast_workload_ratio) * $client_injection_rate_gbps]

set enable_incast 0
if {$incast_workload_ratio > 0.0} {
    set enable_incast 1
}

set total_client_downlink_cap_gbps [expr $num_client_apps * $leaf_link_speed_gbps * 1.0]
set total_server_downlink_cap_gbps [expr $num_server_apps * $leaf_link_speed_gbps * 1.0]
puts "total_client_downlink_cap_gbps $total_client_downlink_cap_gbps"
puts "total_server_downlink_cap_gbps $total_server_downlink_cap_gbps"

# Application-level capacity
set per_server_capacity_rps [expr 1 / [expr $mean_service_time_us / 1000.0 / 1000.0]]
set total_server_capacity_rps [expr $per_server_capacity_rps * $num_server_apps]

puts "client_injection_rate_gbps $client_injection_rate_gbps"
puts "background_injection_rate_gbps $background_injection_rate_gbps"
set total_background_injection_rate_gbps [expr $background_injection_rate_gbps * $num_client_apps]
puts "total_background_injection_rate_gbps $total_background_injection_rate_gbps"
set total_incast_injection_rate_gbps [expr $incast_injection_rate_gbps * $num_client_apps]
puts "total_incast_injection_rate_gbps $total_incast_injection_rate_gbps"
set per_client_req_send_rate_rps [expr $background_injection_rate_gbps * 1000.0 * 1000.0 *1000.0 \
                            / [expr $mean_req_size_B * 8]]

set mean_per_client_req_interval_us 0
set mean_per_client_req_interval_s 0

if {$background_injection_rate_gbps > 0} {
    set mean_per_client_req_interval_us [expr 1 / $per_client_req_send_rate_rps * 1000.0 * 1000.0]   
    set mean_per_client_req_interval_s [expr 1 / $per_client_req_send_rate_rps]
}   
puts "mean_per_client_req_interval_us $mean_per_client_req_interval_us"    
puts "mean_per_client_req_interval_s $mean_per_client_req_interval_s"    

set mean_incast_interval_s 0
if {$incast_workload_ratio > 0.0} {
    set mean_incast_interval_s [expr ($incast_request_size_bytes * $incast_size * 8) / ($total_incast_injection_rate_gbps * 1000.0 * 1000.0 *1000.0)]
}
puts "mean_incast_interval_s $mean_incast_interval_s"    

set global_req_send_rate_rps [expr $per_client_req_send_rate_rps * $num_client_apps]
set global_resp_send_rate_rps $global_req_send_rate_rps
puts "global_req_send_rate_rps $global_req_send_rate_rps"
puts "global_resp_send_rate_rps $global_resp_send_rate_rps"
set per_server_resp_send_rate_rps [expr $global_resp_send_rate_rps / $num_server_apps]
puts "per_client_req_send_rate_rps $per_client_req_send_rate_rps"
puts "per_server_resp_send_rate_rps $per_server_resp_send_rate_rps"
set estim_send_rate_per_client_gbps_bgrnd [expr  $per_client_req_send_rate_rps \
        * $mean_req_size_B * 8.0 /1000.0/1000.0/1000.0]
puts "estimated estim_send_rate_per_client_gbps_bgrnd $estim_send_rate_per_client_gbps_bgrnd"

if {abs([expr $estim_send_rate_per_client_gbps_bgrnd - $background_injection_rate_gbps]) > 0.2} {
    puts "Error: estimated send_rate_per_client_gbps different to input client_injection_rate_gbps"
    exit 1
}

set estim_send_rate_per_server_gbps_bgrnd [expr $per_server_resp_send_rate_rps \
        * $mean_resp_size_B * 8.0 /1000.0/1000.0/1000.0]
puts "estimated send_rate_per_server_gbps_bgrnd $estim_send_rate_per_server_gbps_bgrnd"
set total_cl_send_rate_gbps [expr $estim_send_rate_per_client_gbps_bgrnd * $num_client_apps + $total_incast_injection_rate_gbps]
puts "total_cl_send_rate_gbps $total_cl_send_rate_gbps"
set total_sr_send_rate_gbps [expr $estim_send_rate_per_server_gbps_bgrnd * $num_server_apps]
puts "total_sr_send_rate_gbps $total_sr_send_rate_gbps"

if {[dict size $hosts] != $num_hosts } {
    puts "Error, num_hosts $num_hosts not equal hosts dictionart size ([dict size $hosts])"
    exit 1
}

if {$num_spines > 0} {
    puts "3-tier topology not implemented"
    exit 1
}

set num_tor_uplinks $num_aggr
set core_capacity_gbps [expr $num_tor_uplinks * $core_link_speed_gbps]

# ------------------------------------- Background Load calculation -------------------------------------
# TODO: if you want to uncomment, remember to factor in incast traffic
set approx_app_load [expr $global_req_send_rate_rps/$total_server_capacity_rps * 100]
set approx_req_nwrk_load_leaves [expr $total_cl_send_rate_gbps/$total_server_downlink_cap_gbps * 100]
set approx_reply_nwrk_load_leaves [expr $total_sr_send_rate_gbps/$total_client_downlink_cap_gbps * 100]
set approx_max_req_nwrk_load_core 0
set approx_max_reply_nwrk_load_core 0
if {$core_capacity_gbps > 0} {
    set approx_max_req_nwrk_load_core [expr $total_cl_send_rate_gbps/$core_capacity_gbps * 100]
    set approx_max_reply_nwrk_load_core [expr $total_sr_send_rate_gbps/$core_capacity_gbps * 100]
}


set approx_leaves_load [expr $approx_req_nwrk_load_leaves + $approx_reply_nwrk_load_leaves]

# Simulation duration
set simul_dur [expr $duration_modifier / [expr [max $approx_req_nwrk_load_leaves $approx_reply_nwrk_load_leaves] / 100.0]]
if {$req_interval_distr == "manual" || $req_interval_distr == "mlpm"} {
    set simul_dur $duration_modifier
}

set start_at 10.0
set general_queue_size_bytes [expr $mean_packet_size * $general_queue_size]
set after_dur [expr $simul_dur * 0.01 + 0.0001]
set stop_at [expr $start_at + $simul_dur]
set simul_termination [expr $start_at + $simul_dur + $after_dur]
set start_tracing_at [expr $start_at + $simul_dur * (1 - $trace_last_ratio)]
# ------------------------------------- Write Param file -------------------------------------
puts "Writing parameter file after calculating derivative parameters"
set out_params_file $script_params_file
puts "$out_params_file"

set out [open "${out_params_file}" a+]
puts $out "set simul_dur $simul_dur"
puts $out "set total_client_downlink_cap_gbps $total_client_downlink_cap_gbps"
puts $out "set total_server_downlink_cap_gbps $total_server_downlink_cap_gbps"
puts $out "set per_server_capacity_rps $per_server_capacity_rps"
puts $out "set total_server_capacity_rps $total_server_capacity_rps"
puts $out "set total_background_injection_rate_gbps $total_background_injection_rate_gbps"
puts $out "set per_client_req_send_rate_rps $per_client_req_send_rate_rps"
puts $out "set mean_per_client_req_interval_us $mean_per_client_req_interval_us"
puts $out "set mean_per_client_req_interval_s $mean_per_client_req_interval_s"
puts $out "set mean_incast_interval_s $mean_incast_interval_s"
puts $out "set enable_incast $enable_incast"
puts $out "set global_req_send_rate_rps $global_req_send_rate_rps"
puts $out "set global_resp_send_rate_rps $global_resp_send_rate_rps"
puts $out "set per_server_resp_send_rate_rps $per_server_resp_send_rate_rps"
puts $out "set estim_send_rate_per_client_gbps_bgrnd $estim_send_rate_per_client_gbps_bgrnd"
puts $out "set estim_send_rate_per_server_gbps_bgrnd $estim_send_rate_per_server_gbps_bgrnd"
puts $out "set total_cl_send_rate_gbps $total_cl_send_rate_gbps"
puts $out "set total_sr_send_rate_gbps $total_sr_send_rate_gbps"
puts $out "set num_tor_uplinks $num_tor_uplinks"
puts $out "set core_capacity_gbps $core_capacity_gbps"
puts $out "set approx_app_load $approx_app_load"
puts $out "set approx_req_nwrk_load_leaves $approx_req_nwrk_load_leaves"
puts $out "set approx_reply_nwrk_load_leaves $approx_reply_nwrk_load_leaves"
puts $out "set approx_max_req_nwrk_load_core $approx_max_req_nwrk_load_core"
puts $out "set approx_max_reply_nwrk_load_core $approx_max_reply_nwrk_load_core"
puts $out "set approx_leaves_load $approx_leaves_load"
puts $out "set general_queue_size_bytes $general_queue_size_bytes"
puts $out "set after_dur $after_dur"
puts $out "set start_at $start_at"
puts $out "set stop_at $stop_at"
puts $out "set simul_termination $simul_termination"
puts $out "set start_tracing_at $start_tracing_at"

close $out