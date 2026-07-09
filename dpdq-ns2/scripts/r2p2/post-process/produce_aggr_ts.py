import numpy as np
import csv

# Local files
import helper as hlp
import qmon_hlp
import cc_hlp
import app_hlp
import omnet_helper as ohlp
import dcpim_helper as dchlp
import cfg
from pprint import pprint
import multiprocessing
import worker_manager as workm
import time
import os

###############################################################################################################
###############################################################################################################
###############################################################################################################
############################################# Slowdown ########################################################
###############################################################################################################
###############################################################################################################
###############################################################################################################


def aggr_app_data_ts(directory, prot, load, is_omnet=False, create_plots=False):
    """
    Output a csv that reports slowdown percentiles for different message sizes.
    """

    def write_goodput_data(this_out_dir, avg_goodput):
        hlp.make_dir(this_out_dir)
        file_name = f"{this_out_dir}/goodput_vs_size.csv"
        header = "Message_Size_Bytes,Average_Goodput_Gbps"

        with open(file_name, 'w') as f:
            f.write(header + '\n')

            sorted_sizes = sorted(avg_goodput.keys())

            for size in sorted_sizes:
                goodput = avg_goodput[size]

                f.write(f"{size:.2f},{goodput:.6f}\n")
                

    def write_slowdown_data(this_out_dir, p1, p50, p95, p99, p999):
        """
        p* args are dictionaries: bin value (msg size) -> slowdown
        The bin values should be the same across p* arguments.
        """
        size_col = np.array(list(p1.keys())).reshape((len(p1.keys()), 1))
        p1col = np.array(list(p1.values())).reshape((len(p1.keys()), 1))
        p50col = np.array(list(p50.values())).reshape((len(p1.keys()), 1))
        p95col = np.array(list(p95.values())).reshape((len(p1.keys()), 1))
        p99col = np.array(list(p99.values())).reshape((len(p1.keys()), 1))
        p999col = np.array(list(p999.values())).reshape((len(p1.keys()), 1))

        data = np.concatenate(
            (size_col, p1col, p50col, p95col, p99col, p999col), axis=1
        )
        hlp.make_dir(this_out_dir)
        file_name = f"{this_out_dir}/slowdown.csv"
        header = "size,p1,p50,p95,p99,p999"
        np.savetxt(file_name, data, delimiter=",", fmt=[
                   "%.2f", "%.2f", "%.2f", "%.2f", "%.2f", "%.2f"], header=header, comments="")
        
    def write_fct_data(this_out_dir, p50, p95, p99):
        """
        p* args are dictionaries: bin value (msg size) -> fct
        The bin values should be the same across p* arguments.
        """
        size_col = np.array(list(p50.keys())).reshape((len(p50.keys()), 1))
        p50col = np.array(list(p50.values())).reshape((len(p50.keys()), 1))
        p95col = np.array(list(p95.values())).reshape((len(p50.keys()), 1))
        p99col = np.array(list(p99.values())).reshape((len(p50.keys()), 1))

        data = np.concatenate(
            (size_col, p50col, p95col, p99col), axis=1
        )
        hlp.make_dir(this_out_dir)
        file_name = f"{this_out_dir}/fct.csv"
        header = "size,p50,p95,p99"
        np.savetxt(file_name, data, delimiter=",", fmt=[
                   "%f", "%f", "%f", "%f"], header=header, comments="")

    def write_completed_flow_sizes(this_out_dir, flows):
        """
        flows: Nx2 array -> (req_id, req_size)
        """
        if flows.size == 0:
            return
        hlp.make_dir(this_out_dir)
        file_name = f"{this_out_dir}/completed_flows.csv"
        header = "req_size,req_dur,slowdown,goodput_gbps"
        np.savetxt(file_name, flows, delimiter=",", fmt=["%d", "%f", "%f", "%f"], header=header, comments="")

    def process_load_lvl(ldir, out_dir, prot, load, param_file):
        params = hlp.import_param_file(param_file)
        app_file = f"{ldir}/{cfg.APP_FILE}"
        data = app_hlp.load_app_data(app_file)
        if data.shape[0] == 0:
            return
        # The loaded timestamps start at 0s (and not 10s)
        start = 0
        trace_end = float(params["sim_dur"][0])
        data = hlp.trim_by_timestamp(
            data, start, trace_end, app_hlp.app_cols["timestamp"])
        rrq_rows = data[data[:, app_hlp.app_cols["event"]]
                        == app_hlp.app_events["rrq"]]

        sldwn_view = rrq_rows[:, (app_hlp.app_cols["timestamp"],
                                    app_hlp.app_cols["req_size"],
                                    app_hlp.app_cols["remote_addr"],
                                    app_hlp.app_cols["local_addr"],
                                    app_hlp.app_cols["req_dur"])]

        # 1: r2p2 2: dctcp
        slowdown_1 = app_hlp.calculate_slowdown(params, sldwn_view, param_file,
                                              # with this "is_intra_rack" funtion, the next 3 arguments don't matter
                                              0, 1, 2, 3, 4, app_hlp.is_intra_rack,
                                              0, 0, 0,
                                              sortBySize=True)
        # bin_count = min(int(slowdown.shape[0] / 500), 50)
        bin_count = int(slowdown_1.shape[0] / 500)
        if bin_count == 0:
            print("Not enough datapoints to produce slowdown_1 vs msg_size plot. Returning")
            return
        median, tail99, tail999, tail95, tail1, mean = app_hlp.get_slowdown_vs_size(slowdown_1, 1, 2, bin_count)
        out_dir_1 = out_dir + '/r2p2'
        write_slowdown_data(out_dir_1, tail1, median, tail95, tail99, tail999)
        
        # bin_count = int(slowdown_2.shape[0] / 500)
        # if bin_count == 0:
        #     print("Not enough datapoints to produce slowdown_2 vs msg_size plot. Returning")
        #     return
        # median, tail99, tail999, tail95, tail1, mean = app_hlp.get_slowdown_vs_size(slowdown_2, 1, 2, bin_count)
        # out_dir_2 = out_dir + '/dctcp'
        # write_slowdown_data(out_dir_2, tail1, median, tail95, tail99, tail999)


        # # Returns columns (ts, req_size, slowdown)
        # slowdown = app_hlp.calculate_slowdown(params, sldwn_view, param_file,
        #                                       # with this "is_intra_rack" funtion, the next 3 arguments don't matter
        #                                       0, 1, 2, 3, 4, app_hlp.is_intra_rack,
        #                                       0, 0, 0,
        #                                       sortBySize=True)
        
        # goodput_view = rrq_rows[:, (app_hlp.app_cols["timestamp"],
        #                             app_hlp.app_cols["req_size"],
        #                             app_hlp.app_cols["req_dur"])]
        # completed_flow_sizes = rrq_rows[:, (app_hlp.app_cols["req_size"],
        #                                     app_hlp.app_cols["req_dur"])]
        
        # goodput_col = goodput_view[:, 1]* 8.0 / 1000.0 / 1000.0 / 1000.0 / goodput_view[:, 2] 
        # goodput = np.hstack((goodput_view, goodput_col[:, np.newaxis]))


        # # slowdown: (ts, req_size, slowdown)
        # # goodput : (ts, req_size, req_dur, goodput)

        # slowdown_col = slowdown[:, 2]
        # goodput_only_col = goodput[:, 3]

        # completed_flow_sizes = np.hstack((
        #     completed_flow_sizes,                 # (N, 2)
        #     slowdown_col[:, np.newaxis],          # (N, 1)
        #     goodput_only_col[:, np.newaxis]       # (N, 1)
        # ))


        # # bin_count = min(int(slowdown.shape[0] / 500), 50)
        # bin_count = int(slowdown.shape[0] / 500)
        # if bin_count == 0:
        #     print("Not enough datapoints to produce slowdown vs msg_size plot. Returning")
        #     return
        # median, tail99, tail999, tail95, tail1, mean = app_hlp.get_slowdown_vs_size(slowdown, 1, 2, bin_count)
        # if create_plots:
        #     app_hlp.plot_slowdown_vs_size(tail1, "1th% Slowdown", prot, load, out_dir)
        #     app_hlp.plot_slowdown_vs_size(median, "50th% Slowdown", prot, load, out_dir)
        #     app_hlp.plot_slowdown_vs_size(tail95, "95th% Slowdown", prot, load, out_dir)
        #     app_hlp.plot_slowdown_vs_size(tail99, "99th% Slowdown", prot, load, out_dir)
        #     app_hlp.plot_slowdown_vs_size(tail999, "99.9th% Slowdown", prot, load, out_dir)

        # avg_goodput = app_hlp.get_avg_goodput_vs_size(goodput, 1, 3, bin_count)
        # median_fct, tail95_fct, tail99_fct = app_hlp.get_fct_vs_size(goodput_view, 1, 2, bin_count)
        
        # # Write to csv
        # write_slowdown_data(out_dir, tail1, median, tail95, tail99, tail999)
        # write_goodput_data(out_dir, avg_goodput)
        # write_fct_data(out_dir, median_fct, tail95_fct, tail99_fct)
        # write_completed_flow_sizes(out_dir, completed_flow_sizes)
    out_dir = f"{directory}/{cfg.OUT_DIR_NAME}/app"
    hlp.make_dir(out_dir)
    param_file = f"{directory}/parameters"
    if is_omnet:
        data_dir = f"{directory}/extracted_results"
        raise Exception("aggr_app_data_ts() not implemented for OMNET")
        # process_load_lvl_omnet(data_dir, out_dir, prot, load, param_file)
    else:
        process_load_lvl(directory, out_dir, prot, load, param_file)


###############################################################################################################
###############################################################################################################
###############################################################################################################
######################################## Queuing and Throughput ###############################################
###############################################################################################################
###############################################################################################################
###############################################################################################################


def plot_qts_data(this_out_dir, data, origin_area, origin, end_area, end, prot):
    out_dir_q = f"{this_out_dir}/plots/qing"
    out_dir_t = f"{this_out_dir}/plots/throughput"
    out_dir_t_each = f"{this_out_dir}/plots/throughput_each"
    out_dir_ppass_pkt = f"{this_out_dir}/plots/ppass_packet_throughput"
    name = f"{origin_area}_{origin}_{end_area}_{end}"
    hlp.make_dir(out_dir_q)
    hlp.make_dir(out_dir_t)
    hlp.make_dir(out_dir_t_each)
    hlp.make_dir(out_dir_ppass_pkt)
    # Plot Qing
    hlp.plot_time_series(data[:, (0, 2)], out_dir_q, f"{name}", prot, "Queuing (B)")

    # Plot Thrpt
    hlp.plot_time_series(
        data[:, (0, 1)], out_dir_t, f"{name}", prot, "Throughput (Gbps)"
    )
    # Plot Thrpt dctcp/r2p2
    hlp.plot_time_series(
        data[:, (0, 3, 4)], out_dir_t_each, f"{name}", prot, "DCTCP/PPASS Throughput (Gbps)"
    )
    hlp.plot_time_series(
        data[:, (0, 5, 6, 7, 8, 9)],
        out_dir_ppass_pkt,
        f"{name}_ppass_packet_throughput",
        ["grant_req_gbps", "grant_gbps", "reply_gbps", "scheduled_data_gbps", "unscheduled_gbps"],
        "PPASS Packet Throughput (Gbps)"
    )


def write_qts_data(this_out_dir, data, origin_area, origin, end_area, end):
    hlp.make_dir(this_out_dir)
    file_name = f"{this_out_dir}/qts_{origin_area}_{origin}_{end_area}_{end}.csv"
    header = "timestamp(s),throughput_gbps,queueing_B,dctcp_gbps,ppass_gbps,grant_req_gbps,grant_gbps,reply_gbps,scheduled_data_gbps,unscheduled_gbps"
    np.savetxt(file_name, data, delimiter=",", fmt=[
               "%.12f", "%.2f", "%d", "%.6f", "%.6f", "%.6f", "%.6f", "%.6f", "%.6f", "%.6f"], header=header, comments="")

def aggr_q_data_ts_task(file_name, adir, params, out_dir, origin_area, end_area, create_plots, prot):
    file_path = f"{adir}/{file_name}"
    origin = file_name.split("_")[0]
    end = file_name.split("_")[1].split(".")[0]
    stop = float(params["sim_dur"][0])
    data = qmon_hlp.load_and_trim_qmon_data(file_path, stop)
    thrpt_data = data[:,
                        (qmon_hlp.qmon_cols["timestamp"],
                        qmon_hlp.qmon_cols["byte_departures"])]

    thrpt_data = ohlp.calc_thrpt(thrpt_data)

    q_data = data[:,
                    (qmon_hlp.qmon_cols["timestamp"],
                    qmon_hlp.qmon_cols["q_sz_bytes"])]
    q_data = q_data[1:, :]
    q_data[:, 1] = np.rint(q_data[:, 1]).astype(int)
    
    thrpt_each_data = data[:,
                        (qmon_hlp.qmon_cols["timestamp"],
                        qmon_hlp.qmon_cols["tcp_gbps"],
                        qmon_hlp.qmon_cols["r2p2_gbps"],
                        qmon_hlp.qmon_cols["grant_req_gbps"],
                        qmon_hlp.qmon_cols["grant_gbps"],
                        qmon_hlp.qmon_cols["reply_gbps"],
                        qmon_hlp.qmon_cols["scheduled_data_gbps"],
                        qmon_hlp.qmon_cols["unscheduled_gbps"])]
    thrpt_each_data = thrpt_each_data[1:, :]

    # Write one file per link
    this_out_dir = f"{out_dir}/{origin_area}"
    data = thrpt_data
    data = np.append(data, q_data[:, 1].reshape(
        q_data.shape[0], 1), axis=1)
    # [timestamp, calculated_thrpt, q_sz_bytes, tcp_gbps, r2p2_gbps, grant_req_gbps, grant_gbps, reply_gbps, scheduled_data_gbps, unscheduled_gbps]
    data = np.append(data, thrpt_each_data[:, 1:], axis=1)
    write_qts_data(this_out_dir, data,
                    origin_area, origin, end_area, end)
    if create_plots:
        plot_qts_data(this_out_dir, data,
                        origin_area, origin, end_area, end, prot)
        
def aggr_throughput_all_avg_ts_task(file_name, adir, params, out_dir, create_plots, prot):
    if not create_plots:
        return
    if not file_name.endswith(".csv"):
        return

    file_path = f"{adir}/{file_name}"
    out_dir_t = f"{out_dir}/plots/throughput_all_avg"
    hlp.make_dir(out_dir_t)

    base = os.path.splitext(file_name)[0]
    parts = base.split("_")
    # e.g., qts_aggr_all_tor_all -> aggr_tor
    if len(parts) >= 5:
        name = f"{parts[1]}_{parts[3]}"
    else:
        name = base

    data = np.loadtxt(file_path, delimiter=",", skiprows=1)
    if data.size == 0:
        return
    data = data[data[:, 0].argsort()]

    hlp.plot_time_series(
        data[:, (0, 1, 2)],
        out_dir_t,
        name,
        ["dctcp_gbps", "ppass_gbps"],
        y_axis_title="DCTCP/PPASS Throughput (Gbps)",
        x_axis_title="Time (ms)",
    )
    

def aggr_q_data_ts(directory, prot, load, simulator, create_plots=False):
    def process_load_lvl(ldir, out_dir, prot, load, param_file):
        print(f"Qts Processing {ldir}")
        data_dir = f"{ldir}/qmon"
        params = hlp.import_param_file(param_file)
        area_subdirs = hlp.get_subdirs(data_dir)

        pool = []

        for adir in area_subdirs:
            area = adir.split("/")[-1]
            # Keep tor->aggr, aggr->tor, and tor->tor links (+ aggregated throughput dirs)
            if area not in ("tor_aggr", "aggr_tor", "tor_tor", "throughput_all_avg"):
                continue

            origin_area = None
            end_area = None
            if area in ("tor_aggr", "aggr_tor", "tor_tor"):
                origin_area, end_area = area.split("_", 1)

            files = hlp.get_files(adir)

            for file_name in files:
                if file_name.endswith(".q"):
                    pool.append(workm.Task(aggr_q_data_ts_task, (file_name, adir, params, out_dir, origin_area, end_area, create_plots, prot)))
                elif file_name.endswith(".csv"):
                    pool.append(workm.Task(aggr_throughput_all_avg_ts_task, (file_name, adir, params, out_dir, create_plots, prot)))
        if len(pool) > 0:
            workm.add_tasks(pool, task_family="Qts subpool", suggested_batch_size=5)


    def process_load_lvl_omnet(data_dir, out_dir, prot, load, param_file):
        print(f"Qts omnet Processing {data_dir}")
        params = hlp.import_param_file(param_file)
        byte_dir = f"{data_dir}/{ohlp.OMNET_BYTE_SAMPLE}"
        q_dir = f"{data_dir}/{ohlp.OMNET_Q_SAMPLE}"

        ifaces, q_data, byte_data, num_aggr, num_tor, num_host_per_tor = ohlp.get_qlen_and_byte_data(
            byte_dir, q_dir)

        # Each two columns map to one interface. So for each iface, calculate thrpt and queueing
        for i, iface in enumerate(ifaces):
            idx = 2 * i

            origin, origin_area, end, end_area, throughput, qing = ohlp.get_qlen_and_throughput_single_origin(
                byte_data, q_data, idx, params, iface, num_aggr, num_tor, num_host_per_tor)
            if throughput is None:
                continue
            this_out_dir = f"{out_dir}/{origin_area}"
            qing[:, 1] = qing[:, 1] / 1000.0
            data = throughput
            data = np.append(data, qing[:, 1].reshape(qing.shape[0], 1), axis=1)

            # this only writes aggr and tor data (no host)
            write_qts_data(this_out_dir, data, origin_area, origin, end_area, end)
    
    def process_load_lvl_dcpim(data_dir, out_dir, prot, param_file):
        print(f"Qts dcpim Processing {data_dir}")
        params = hlp.import_param_file(param_file)
        q_file = f"{data_dir}/{cfg.DCPIM_Q_FILE}"
        data = dchlp.get_queue_data(q_file)

        _, sim_start, sim_end = dchlp.get_app_data(f"{data_dir}/{cfg.DCPIM_APP_FILE}")
        trace_start, trace_end = dchlp.get_trace_interval(params, sim_start, sim_end)
        data = hlp.trim_by_timestamp(data, trace_start, trace_end, dchlp.qCols.TS.value)
        print(f"DELETEME: trace_start {trace_start} trace_end {trace_end}")

        # Group data by link
        orig_to_data = {} # origin -> link -> timeseries (ts,qlen,departures)
        orig_to_data = dchlp.group_by_origin(data)

        # Plot qing timeseries
        for origin in orig_to_data:
            origin_area = origin.split("_")[0]
            for link in orig_to_data[origin]:
                out_dir_q = f"{out_dir}/{origin_area}/plots/qing"
                hlp.make_dir(out_dir_q)
                name = f"{origin}_{link}"
                hlp.plot_time_series(orig_to_data[origin][link][:, (0, 1)], out_dir_q, f"{name}", prot, "Queuing (B)", x_axis_title="Time (s)")



    out_dir = f"{directory}/{cfg.OUT_DIR_NAME}/qts"
    hlp.make_dir(out_dir)
    param_file = f"{directory}/parameters"
    if simulator == cfg.OMNET_SIMULATOR:
        data_dir = f"{directory}/extracted_results"
        process_load_lvl_omnet(data_dir, out_dir, prot, load, param_file)
    elif simulator == cfg.NS2_SIMULATOR:
        process_load_lvl(directory, out_dir, prot, load, param_file)
    elif simulator == cfg.DCPIM_SIMULATOR:
        process_load_lvl_dcpim(directory, out_dir, prot, param_file)
    else:
        raise ValueError(f"Unknown simulator {simulator}")


###############################################################################################################
###############################################################################################################
###############################################################################################################
#################################### Transport Layer data #####################################################
###############################################################################################################
###############################################################################################################
###############################################################################################################


def aggr_cc_data_ts(directory, prot, load, is_omnet=False, create_plots=False):
    """
    extract the timeseries of every column of cc_trace.str
    So for every protocol, load combination, there are num_col timeseries.
    Only keep "pol" events
    """

    def process_load_lvl(ldir, out_dir, prot, load, param_file):
        print(f"CC Processing {ldir}")
        data_dir = f"{ldir}/cc_trace.str"
        params = hlp.import_param_file(param_file)
        cc_scheme = "micro-hybrid"

        # start_time = {}
        # proc_time = {}
        # start_time["proc"] = time.process_time()
        # Each row corresponds to a specific event type and a specific host. Filter and separate.
        proc_fun = cc_hlp.get_proc_fun(cc_scheme)
        params_to_proc = cc_hlp.get_proc_params(cc_scheme)
        if proc_fun is None:
            # There is no function to process cc data for $cc_scheme
            return
        data = cc_hlp.load_cc_data(ldir, "micro-hybrid", 1.0, param_file)
        results = proc_fun(data, params, prot, load, params_to_proc)
        print("Results processed. Writing out")
        # proc_time["proc"] = time.process_time() - start_time["proc"]
        # start_time["out"] = time.process_time()

        cc_hlp.output_results(results, out_dir, prot, load, create_plots)
        # proc_time["out"] = time.process_time() - start_time["out"]

        # for key in proc_time:
        #     print(f"Processing stage {key} took {proc_time[key]} seconds")

    out_dir = f"{directory}/{cfg.OUT_DIR_NAME}/cc"
    hlp.make_dir(out_dir)
    param_file = f"{directory}/parameters"
    if is_omnet:
        raise Exception("aggr_cc_data_ts() not supported for omnet simulations")
    else:
        # TEMP: skip CC timeseries to avoid processing 16GB cc_trace.str when only qmon link is needed
        print("Skipping CC timeseries (temp override)")
        return
        process_load_lvl(directory, out_dir, prot, load, param_file)

if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser(description="Get aggr_app_data_ts")
    parser.add_argument("directory", help="results directory")
    parser.add_argument("--prot", default="ppass-w4", help="protocol name")
    parser.add_argument("--load", default="10", help="load label")
    args = parser.parse_args()
    aggr_app_data_ts(args.directory, args.prot, args.load, is_omnet=False, create_plots=False)
