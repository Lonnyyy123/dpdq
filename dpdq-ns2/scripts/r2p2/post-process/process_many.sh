#!/bin/bash

# to_process=(\
#             "w3-dcpim" "w3-swift" "w3-sird" "w3-homa" "w3-dctcp"\
#             "w4-dcpim" "w4-swift" "w4-sird" "w4-homa" "w4-dctcp"\
#             "w5-dcpim" "w5-swift" "w5-sird" "w5-homa" "w5-dctcp"\
#             "w3-dcpim-oversub" "w3-swift-oversub" "w3-sird-oversub" "w3-homa-oversub" "w3-dctcp-oversub"\
#             "w4-dcpim-oversub" "w4-swift-oversub" "w4-sird-oversub" "w4-homa-oversub" "w4-dctcp-oversub"\
#             "w5-dcpim-oversub" "w5-swift-oversub" "w5-sird-oversub" "w5-homa-oversub" "w5-dctcp-oversub"\
#             "w3-dcpim-incast" "w3-swift-incast" "w3-sird-incast" "w3-homa-incast" "w3-dctcp-incast"\
#             "w4-dcpim-incast" "w4-swift-incast" "w4-sird-incast" "w4-homa-incast" "w4-dctcp-incast"\
#             "w5-dcpim-incast" "w5-swift-incast" "w5-sird-incast" "w5-homa-incast" "w5-dctcp-incast"\ 
#             "w4-B" "w5-B" "w4-B-recreate"\
#             "w4-sird-prio" w4-unsol\
#             "w4-dctcp-init")

# to_process=("w4-dctcp-init")
# to_process=("w4-sird-prio")
# to_process=("w5-B")
# to_process=("w3-sird" "w3-homa")
# to_process=("w3-dcpim" "w3-swift" "w3-sird" "w3-homa" "w3-dctcp")
to_process=("w5-homa")

result_path=$1

# Unit: python3 process_results.py /home/prasopou/scripts/epfl_nas/prasopou/nwsim_results/w5-sird-incast/ 0 0 &> sird_incast.txt
#./process_many.sh /home/prasopou/scripts/epfl_nas/prasopou/nwsim_results
for sim in "${to_process[@]}"
do
    echo "===================================== $sim ====================================="
    args="/home/prasopou/scripts/epfl_nas/prasopou/nwsim_results/${sim}/ 0 0"
    echo "sim: ||${sim}||"
    echo "args: ${args}"
    python3 process_results.py ${args} &> process_many_out.txt
done