# Holds variables common to all simulations

# TODO: pfab_K = Pointless -> pfabric does not mark -  no need for marking threshold.
set prio_scheme 2
set prob_cap 5

# Multipath
set enable_multipath 1
set per_flow_mp 1

# if this is set to 1538, there is packet loss before the sim starts 
# (qlim_ is negative in drop-tail). Do not know why
set mean_packet_size 1500
set packet_size_bytes 1538
set r2p2_headers_size 60
set udp_common_headers_size 46
set tcp_common_headers_size 58
set inter_pkt_and_preamble 20

# xpass
# 0.5 or 0.0625? The paper says that the sweet spot is 0.0625 and implies it's the default (though the fat tree topology tcl file used 0.5)
set xpass_alpha 0.0625 
set xpass_w_init 0.0625
set xpass_creditBuffer [expr 84*8]
set xpass_maxCreditBurst [expr 84*2]
set xpass_minJitter -0.1
set xpass_maxJitter 0.1
set xpass_minEthernetSize 84
set xpass_maxEthernetSize 1538
set xpass_minCreditSize 76
set xpass_maxCreditSize 92
set xpass_xpassHdrSize 78
set xpass_maxPayload [expr $xpass_maxEthernetSize-$xpass_xpassHdrSize]
set xpass_avgCreditSize [expr ($xpass_minCreditSize+$xpass_maxCreditSize)/2.0]


set ns [new Simulator]

if {$enable_multipath == 1} {
    $ns rtproto DV
    Agent/rtProto/DV set advertInterval	[expr 200000000]  
    Node set multiPath_ 1 
    if {$per_flow_mp != 0} {
	    Classifier/MultiPath set perflow_ 1
    }
}
