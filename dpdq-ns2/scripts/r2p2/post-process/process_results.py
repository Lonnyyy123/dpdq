import os
import multiprocessing
import sys

# Local files
import produce_aggr
import produce_aggr_ts
import helper as hlp
import omnet_helper as ohlp
import dcpim_helper as dchlp
import cfg
import worker_manager as workm

def extract_all_results(prots, gbps_per_host_l, data_path, thread_count, extract_timeseries=False, create_plots=False):

    ##########################################################################
    ########################### Slowdown / Goodput ###########################
    ##########################################################################
    """
    Speed: fast
    Aggregate results and produce scalar outputs for latency and goodput
    """
    pool = []
    for prot in prots:
        for load in gbps_per_host_l:
            path = f"{data_path}/{prot}/{load}"
            if os.path.exists(f"{path}/this-is-an-omnet-dir"):
                ohlp.touch_omnet_file(f"{path}/{cfg.OUT_DIR_NAME}")
                pool.append(workm.Task(produce_aggr.aggr_app_data, (path, prot, load, cfg.OMNET_SIMULATOR, create_plots)))
                pool.append(workm.Task(produce_aggr_ts.aggr_app_data_ts, (path, prot, load, True, create_plots)))
            elif os.path.exists(f"{path}/this-is-a-dcpim-dir"):
                dchlp.touch_dcpim_file(f"{path}/{cfg.OUT_DIR_NAME}")
                pool.append(workm.Task(produce_aggr.aggr_app_data, (path, prot, load, cfg.DCPIM_SIMULATOR, create_plots)))
            else:
                pool.append(workm.Task(produce_aggr.aggr_app_data, (path, prot, load, cfg.NS2_SIMULATOR, create_plots)))
                pool.append(workm.Task(produce_aggr_ts.aggr_app_data_ts, (path, prot, load, False, create_plots)))
    if len(pool) > 0:
        workm.add_tasks(pool, task_family="Slowdown Pool")

    ##########################################################################
    ################################ Queueing ################################
    ##########################################################################
    """
    Speed: fast
    Aggregate results and produce scalar outputs for queueing
    """
    pool = []
    for prot in prots:
        for load in gbps_per_host_l:
            path = f"{data_path}/{prot}/{load}"
            if os.path.exists(f"{path}/this-is-an-omnet-dir"):
                p = workm.Task(produce_aggr.aggr_q_data, (path, prot, load, cfg.OMNET_SIMULATOR))
                ohlp.touch_omnet_file(f"{path}/{cfg.OUT_DIR_NAME}")
                pool.append(p)
            elif os.path.exists(f"{path}/this-is-a-dcpim-dir"):
                p = workm.Task(produce_aggr.aggr_q_data, (path, prot, load, cfg.DCPIM_SIMULATOR))
                dchlp.touch_dcpim_file(f"{path}/{cfg.OUT_DIR_NAME}")
                pool.append(p)
            else:
                p = workm.Task(produce_aggr.aggr_q_data, (path, prot, load, cfg.NS2_SIMULATOR))
                pool.append(p)
    if len(pool) > 0:
        workm.add_tasks(pool, task_family="Queuing Pool")

    ##########################################################################
    ################################ Timeseries ##############################
    ##########################################################################
    """
    Speed: slow
    Aggregate results and produce timeseries outputs for latency and queueing
    """
    if extract_timeseries:
        pool = []
        for prot in prots:
            for load in gbps_per_host_l:
                path = f"{data_path}/{prot}/{load}"
                params = hlp.import_param_file(f"{path}/parameters")
                scheme = params["r2p2_cc_scheme"][0]
                if os.path.exists(f"{path}/this-is-an-omnet-dir"):
                    ohlp.touch_omnet_file(f"{path}/{cfg.OUT_DIR_NAME}")
                    pool.append(workm.Task(produce_aggr_ts.aggr_q_data_ts, (path, prot, load, cfg.OMNET_SIMULATOR, create_plots)))
                elif os.path.exists(f"{path}/this-is-a-dcpim-dir"):
                    dchlp.touch_dcpim_file(f"{path}/{cfg.OUT_DIR_NAME}")
                    pool.append(workm.Task(produce_aggr_ts.aggr_q_data_ts, (path, prot, load, cfg.DCPIM_SIMULATOR, create_plots)))
                else:
                    pool.append(workm.Task(produce_aggr_ts.aggr_q_data_ts, (path, prot, load, cfg.NS2_SIMULATOR, create_plots)))
                    # CC timeseries for SIRD only
                    if "hybrid" in scheme and os.path.exists(f"{path}/cc_trace.str"):
                        pool.append(workm.Task(produce_aggr_ts.aggr_cc_data_ts, (path, prot, load, False, create_plots)))
        if len(pool) > 0:
            workm.add_tasks(pool, task_family="Timeseries Pool")

def usage():
    print("Usage: python3 process_results.py <extract timeseries (0,1)> <create plots (0,1)>")

def main():
    if len(sys.argv) != 4:
        usage()
        exit(1)
    base_path = sys.argv[1]
    extract_timeseries = bool(int(sys.argv[2]))
    create_plots = bool(int(sys.argv[3]))
    data_path = f"{base_path}/data"

    with open(base_path + "/injected_gbps_per_client", "r") as ld:
        gbps_per_host_l = ld.readline().split()
    with open(base_path + "/prots", "r") as pr:
        prots = pr.readline().split()

    # will only affect omnet results
    ohlp.extract_omnet_results(prots, gbps_per_host_l, data_path)

    extract_all_results(prots, gbps_per_host_l, data_path,
                        multiprocessing.cpu_count(), extract_timeseries=extract_timeseries,
                        create_plots=create_plots)
    workm.run_loop(int(multiprocessing.cpu_count()/2))
    # workm.run_loop(multiprocessing.cpu_count())

if __name__ == "__main__":
    main()
