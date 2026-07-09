error () {
    echo $* 1>&2
    exit 1
}

list_length () {
    echo $(wc -w <<< "$@")
}

set_parameter () {
    param_name=$1
    indx=$2
    param_name_l="${param_name}_l"
    param_l_val=${!param_name_l}

    

    if [ $(list_length $param_l_val) -gt 1 ]; then
        [ $(list_length $transp_prots) -ne $(list_length $param_l_val) ] && error "len(param_l_val) ($param_name) must equal len(transp_prots) or 1"
        arr=($param_l_val)
        read $param_name <<< ${arr[$indx]}
    elif [ $(list_length $param_l_val) -eq 0 ]; then
        error "A list must be created for parameter $param_name (like so: $param_name_l='1' or $param_name_l='0 1')"
    else
        read $param_name <<< $param_l_val
    fi
}

set_parameters () {
    i=$1

    [ $(list_length $transp_prots) -ne $(list_length $simulation_name_l) ] && error "len(transp_prots) must equal len(simulation_name_l)"
    # TODO: do in for loop. util should not know abt the parameters
    set_parameter single_path_per_msg $i
    set_parameter simulation_name $i
    set_parameter urpc_sz_bytes $i
    set_parameter r2p2_host_uplink_prio $i
    set_parameter r2p2_throttle_mul $i
    set_parameter r2p2_busy_thresh_us $i
    set_parameter r2p2_pace_uplink $i
    set_parameter dctcp_g $i
    set_parameter dctcp_K $i
    set_parameter dctcp_init_cwnd $i
    set_parameter grant_gap_ns $i
    set_parameter general_queue_size $i
    set_parameter r2p2_uplink_deque_policy $i
    set_parameter duration_modifier $i
    set_parameter traffic_matrix_type $i
    set_parameter traffic_matrix_ratio_interrack $i
    set_parameter r2p2_budgets_intra_max_bytes $i
    set_parameter r2p2_ecn_at_core $i
    set_parameter r2p2_ecn_threshold_min $i
    set_parameter drop_tail_num_prios $i
    set_parameter r2p2_ecn_threshold_max $i
    set_parameter r2p2_pace_grants $i
    set_parameter r2p2_ce_new_weight $i
    set_parameter r2p2_ecn_mechanism_influence $i
    set_parameter r2p2_ecn_init_slash_mul $i
    set_parameter r2p2_ecn_min_mul $i
    set_parameter r2p2_ecn_min_mul_nw $i
    set_parameter r2p2_pooled_sender_credit_bytes $i
    set_parameter r2p2_sender_budget_pooled $i
    set_parameter topology_file $i
    set_parameter r2p2_elet_srpb $i
    set_parameter r2p2_unsolicited_thresh_bytes $i
    set_parameter r2p2_unsolicited_limit_senders $i
    set_parameter r2p2_unsolicited_burst_when_idle $i
    set_parameter r2p2_elet_receiver_policy $i
    set_parameter r2p2_elet_account_unsched $i
    set_parameter r2p2_sender_ecn_threshold $i
    set_parameter homa_all_prio $i
    set_parameter homa_adaptive_sched_prio_levels $i
    set_parameter mean_req_size_B $i
    set_parameter manual_req_interval_file $i
    set_parameter homa_link_check_bytes $i
    set_parameter homa_in_network_prios $i
    set_parameter homa_default_unsched_bytes $i
    set_parameter r2p2_hybrid_reset_after_x_rtt $i
    set_parameter r2p2_hybrid_pace_grants $i
    set_parameter r2p2_hybrid_additive_incr_mul $i
    set_parameter r2p2_hybrid_sender_policy $i
    set_parameter homa_boost_tail_bytes_prio $i
    set_parameter r2p2_hybrid_in_network_prios $i
    set_parameter r2p2_hybrid_sender_algo $i
    set_parameter r2p2_hybrid_eta $i
    set_parameter r2p2_hybrid_wai $i
    set_parameter r2p2_hybrid_max_stage $i
    set_parameter r2p2_priority_flow $i
    set_parameter r2p2_data_prio $i
    set_parameter r2p2_sender_mark_aimd $i
    set_parameter r2p2_sender_policy_ratio $i
    set_parameter switch_queue_type $i
    set_parameter incast_workload_ratio $i
    set_parameter incast_size $i
    set_parameter incast_request_size_bytes $i
    set_parameter oneshot_incast_enable $i
    set_parameter incast_period_s $i
    set_parameter incast_burst_type $i

    set_parameter swift_base_target_delay $i
    set_parameter swift_max_scaling_range $i
    set_parameter swift_per_hop_scaling_factor $i
    set_parameter swift_max_cwnd_target_scaling $i
    set_parameter swift_min_cwnd_target_scaling $i
    set_parameter swift_additive_increase_constant $i
    set_parameter swift_multiplicative_decrease_constant $i
    set_parameter swift_max_mdf $i
    set_parameter ppass_pthresh $i
    set_parameter ppass_rho $i
    set_parameter ppass_egress_queue_thresh_bytes $i
    set_parameter dpdq_xoff_bytes $i
    set_parameter dpdq_xon_bytes $i
    set_parameter dpdq_credit_queue_bytes $i
    set_parameter dpdq_data_queue_bytes $i
    set_parameter r2p2_resend_timeout_s $i
    set_parameter burst_req_size_B $i
}

# Expects a list of variable names (not the variables themselves)
write_tcl_params_to_file () {
    out_file=$1
    shift
    printf "# This file has been automatically generated\n" > $out_file
    echo "Number of parameters: $#"
    echo "Writing them to $out_file"
    printf "set ${1} \"${!1}\"\n" >> $out_file
    shift
    while [ $# -gt 0 ]; do
        printf "set ${1} \"${!1}\"\n" >> $out_file
        shift
    done
}

# Expects a list of variable names (not the variables themselves)
check_variable_set () {
    while [ $# -gt 0 ]; do
        var_name="$1"
        if [ -v "$var_name" ]; then
            echo "Variable \"$var_name\" is set to \"${!var_name}\""
        else
            error "Variable \"$var_name\" has not been set. Exiting.."
        fi
        shift
    done
}

set_r2p2_cc_scheme () {
    # takes transp_prot and sets r2p2_cc_scheme and transport_protocol
    [ $# -eq 1 ] || error "set_r2p2_cc_scheme takes one argument"
    transport=$1
    if [ $transport == 'r2p2-rnd' ]
	then
		transport_protocol='r2p2'
		r2p2_cc_scheme='noop'
	elif [ $transport == 'pfabric' ]
	then
		transport_protocol='pfabric'
		r2p2_cc_scheme='noop'
	elif [ $transport == 'dctcp' ]
	then
		transport_protocol='dctcp'
		r2p2_cc_scheme='noop'
    elif [ $transport == 'xpass' ]
	then
		transport_protocol='xpass'
		r2p2_cc_scheme='noop'
    elif [ $transport == 'homa' ]
	then
		transport_protocol='homa'
		r2p2_cc_scheme='noop'
	elif [ $transport == 'micro' ]
	then
		transport_protocol='r2p2'
		r2p2_cc_scheme='micro'
    elif [ $transport == 'micro-hybrid' ]
	then
		transport_protocol='r2p2'
		r2p2_cc_scheme='micro-hybrid'
    elif [ $transport == 'omnet-dctcp' ]
    then
        transport_protocol='omnet-dctcp'
        r2p2_cc_scheme='noop'
    elif [ $transport == 'omnet-swift' ]
    then
        transport_protocol='omnet-swift'
        r2p2_cc_scheme='noop'
    elif [ $transport == 'dcpim' ]
    then
        transport_protocol='dcpim'
        r2p2_cc_scheme='noop'
    else
        echo "Unknown protocol $transport. Exiting"
        exit 1
	fi
}
