# To be used for modularization of simulation.tcl

proc finish {} {
    global ns tf cwnd_f simul_dur start_at after_dur transport_protocol approx_leaves_load approx_core_load \
              r2p2_cc_scheme
    puts "FINISH | $transport_protocol $r2p2_cc_scheme -l: [expr {floor($approx_leaves_load)}]%"
    stop-simple-tracing
    $ns flush-trace
    close $tf
    if {[info exists cwnd_f]} {
        close $cwnd_f
    }
    exit 0
}

proc set-req-target-distr {host_idx host_addr num_workers {override ""}} {
    global req_target_distr clientApplication manual_req_interval_file
    if {$override != ""} {
        set target_distr $override
    } else {
        set target_distr $req_target_distr
    }
    # puts "Setting request target distribution to $target_distr"
    if {$target_distr == "manual"} {
        $clientApplication($host_idx) set-target-distr $target_distr $manual_req_interval_file $host_addr
    } elseif {$target_distr == "uniform"} {
        $clientApplication($host_idx) set-target-distr $target_distr $num_workers
    }
}

proc start-simple-tracing {} {
    global transport_protocol trace_r2p2_layer_sr simple_trace_all num_hosts \
    rdTransportLayer results_path trace_cc r2p2CC trace_application \
    num_client_apps clientApplication num_server_apps serverApplication num_tors classifier_tor
    if {$transport_protocol == "r2p2"} {
        if {$trace_r2p2_layer_sr || $simple_trace_all} {
            for {set i 0} {$i < $num_hosts} {incr i} {
                $rdTransportLayer($i) start-tracing $results_path
            }
        }
        if {$trace_cc || $simple_trace_all} {
            for {set j 0} {$j < $num_hosts} {incr j} {
                $r2p2CC($j) start-tracing $results_path
            }
            for {set k 0} {$k < $num_tors} {incr k} {
                $classifier_tor($k) start-tracing $results_path
            }
        }
        # if {$trace_router || $simple_trace_all} {
        #     $r2p2LayerRouter start-tracing $results_path
        # }
    }
    if {$trace_application || $simple_trace_all} {
        for {set j 0} {$j < $num_client_apps} {incr j} {
            $clientApplication($j) start-tracing $results_path
        }
        for {set j 0} {$j < $num_server_apps} {incr j} {
            $serverApplication($j) start-tracing $results_path
        }
    }
}

proc stop-simple-tracing {} {
    global num_hosts num_hosts r2p2Layer trace_r2p2_layer_sr simple_trace_all \
    trace_application serverApplication clientApplication num_client_apps num_server_apps \
    trace_router r2p2LayerRouter trace_cc r2p2CC transport_protocol num_tors classifier_tor
    if {$transport_protocol == "r2p2"} {
        if {$trace_r2p2_layer_sr || $simple_trace_all} {
            for {set i 0} {$i < $num_hosts} {incr i} {
                $r2p2Layer($i) stop-tracing
            }
        } 
        if {$trace_cc || $simple_trace_all} {
            for {set j 0} {$j < $num_hosts} {incr j} {
                $r2p2CC($j) stop-tracing
            }
            for {set k 0} {$k < $num_tors} {incr k} {
                $classifier_tor($k) stop-tracing
            }
        } 
        # if {$trace_router || $simple_trace_all} {
        #     $r2p2LayerRouter stop-tracing
        # }
    }
    if {$trace_application || $simple_trace_all} {
        for {set app 0} {$app < $num_server_apps} {incr app} {
            $serverApplication($app) stop-tracing
        }
        for {set app 0} {$app < $num_client_apps} {incr app} {
            $clientApplication($app) stop-tracing
        }
    }  
}

proc max args {
    set res [lindex $args 0]
    foreach element [lrange $args 1 end] {
        if {$element > $res} {set res $element}
    }
    return $res
}

proc min args {
    set res [lindex $args 0]
    foreach element [lrange $args 1 end] {
        if {$element < $res} {set res $element}
    }
    return $res
}


# state_polling_ival_s: -1 if switch queues should not be traced. 0.00001 -> 10us
proc start_q_monitoring {sample_ival do_init} {
    global ns num_aggr num_tors aggr_node tor_node q_mon_results_path\
    state_polling_ival_s num_hosts hosts host_node \
    results_path \   

    set num_hf_aggrs $num_hosts
    set num_hf_tors $num_hosts
    set num_hf_hosts $num_hosts

    # Start it for TOR <-> aggr links
    for {set i 0} {$i < $num_aggr} {incr i} {
        if { $num_hf_aggrs > 0} {
            set hf_aggr_addr($i) [$aggr_node($i) node-addr]
        }
        for {set j 0} {$j < $num_tors} {incr j} {       
            set aggr_addr [$aggr_node($i) node-addr]
            set tor_addr [$tor_node($j) node-addr]
            set interval $state_polling_ival_s
            if { $num_hf_aggrs > 0} {
                set interval $sample_ival
            }

            # aggr to TOR
            set link [$ns link $aggr_node($i) $tor_node($j)]
            if { $do_init == 1} {
                set qf_sp_tor($i,$j) [open "${q_mon_results_path}/aggr_tor/${aggr_addr}_${tor_addr}.q" w]
                set qmon_sp_tor($i,$j) [$ns monitor-queue $aggr_node($i) $tor_node($j) $qf_sp_tor($i,$j) $interval]
            } else {
                $link queue-sample-timeout
            }

            # TOR to aggr
            set link [$ns link $tor_node($j) $aggr_node($i)]
            if { $do_init == 1} {
                set qf_tor_sp($j,$i) [open "${q_mon_results_path}/tor_aggr/${tor_addr}_${aggr_addr}.q" w]
                # is a QueueMonitor instance
                set qmon_tor_sp($j,$i) [$ns monitor-queue $tor_node($j) $aggr_node($i) $qf_tor_sp($j,$i) $interval]
            } else {
                $link queue-sample-timeout
            }
        }
        incr num_hf_aggrs -1
    }

    # Start it for TOR <-> TOR links (dumbbell mode)
    if {$num_aggr == 0} {
        for {set j 0} {$j < $num_tors} {incr j} {
            for {set k [expr {$j + 1}]} {$k < $num_tors} {incr k} {
                set tor_j_addr [$tor_node($j) node-addr]
                set tor_k_addr [$tor_node($k) node-addr]
                set interval $state_polling_ival_s

                # tor_j to tor_k
                set link [$ns link $tor_node($j) $tor_node($k)]
                if { $do_init == 1} {
                    set qf_tor_tor($j,$k) [open "${q_mon_results_path}/tor_tor/${tor_j_addr}_${tor_k_addr}.q" w]
                    set qmon_tor_tor($j,$k) [$ns monitor-queue $tor_node($j) $tor_node($k) $qf_tor_tor($j,$k) $interval]
                } else {
                    $link queue-sample-timeout
                }

                # tor_k to tor_j
                set link [$ns link $tor_node($k) $tor_node($j)]
                if { $do_init == 1} {
                    set qf_tor_tor($k,$j) [open "${q_mon_results_path}/tor_tor/${tor_k_addr}_${tor_j_addr}.q" w]
                    set qmon_tor_tor($k,$j) [$ns monitor-queue $tor_node($k) $tor_node($j) $qf_tor_tor($k,$j) $interval]
                } else {
                    $link queue-sample-timeout
                }
            }
        }
    }

    # Start it for TOR <-> host links
    set old_tor [dict get [dict get $hosts 0] tor]
    foreach host [dict keys $hosts] {
        set ht_interval $state_polling_ival_s
        set th_interval $state_polling_ival_s
        set attributes [dict get $hosts $host]
        set tor [dict get $attributes tor]
        if {$tor != $old_tor} {
           incr num_hf_tors -1
        }
        set old_tor $tor
        if { $num_hf_tors > 0} {
            set hf_tor_addr($tor) [$tor_node($tor) node-addr]
            set th_interval $sample_ival
        }
        if { $num_hf_hosts > 0} {
            set hf_host_addr($host) [$host_node($host) node-addr]
            set ht_interval $sample_ival
            incr num_hf_hosts -1
        }
        set host_addr [$host_node($host) node-addr]
        set tor_addr [$tor_node($tor) node-addr]
        
        # TOR to Host
        set link [$ns link $tor_node($tor) $host_node($host)]
        if { $do_init == 1} {
            set qf_tor_node($tor,$host) [open "${q_mon_results_path}/tor_host/${tor_addr}_${host_addr}.q" w]
            set qmon_tor_node($tor,$host) [$ns monitor-queue $tor_node($tor) $host_node($host) $qf_tor_node($tor,$host) $th_interval]
            # set to_print "Inital value of interval is: [$link set sampleInterval_]"
            # puts $to_print
        } else {
            $link queue-sample-timeout
            # $link set sampleInterval_ $th_interval
            # set to_print "Final value of interval is: [$link set sampleInterval_]"
            # puts $to_print
        }
        
        # Host to TOR
        set link [$ns link $host_node($host) $tor_node($tor)]
        if { $do_init == 1} {
            set qf_node_tor($host,$tor) [open "${q_mon_results_path}/host_tor/${host_addr}_${tor_addr}.q" w]
            set qmon_node_tor($host,$tor) [$ns monitor-queue $host_node($host) $tor_node($tor) $qf_node_tor($host,$tor) $ht_interval]
        } else {
            $link queue-sample-timeout
        }
    }

    # Write the high frequency aggrs and tors and hosts to param file
    set parmtr_file [open "${results_path}/parameters" a+]
    puts -nonewline $parmtr_file "hf_aggr_addr "
    for {set i 0} {$i < [array size hf_aggr_addr]} {incr i} {
        puts -nonewline $parmtr_file "$hf_aggr_addr($i) "
    }
    puts $parmtr_file " "
    puts -nonewline $parmtr_file "hf_tor_addr "
    for {set i 0} {$i < [array size hf_tor_addr]} {incr i} {
        puts -nonewline $parmtr_file "$hf_tor_addr($i) "
    }
    puts $parmtr_file " "
    puts -nonewline $parmtr_file "hf_host_addr "
    for {set i 0} {$i < [array size hf_host_addr]} {incr i} {
        puts -nonewline $parmtr_file "$hf_host_addr($i) "
    }
    puts $parmtr_file " "
    close $parmtr_file
}