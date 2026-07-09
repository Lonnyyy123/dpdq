import numpy as np
import csv

# Local files
import helper as hlp
import qmon_hlp
import app_hlp
import omnet_helper as ohlp
import dcpim_helper as dchlp
import cfg
from pprint import pprint

###############################################################################################################
###############################################################################################################
###############################################################################################################
####################################### Application Data ######################################################
###############################################################################################################
###############################################################################################################
###############################################################################################################

def write_app_data(out_dir, prot, load, offered, achieved, tail99, tail999, median, mean, initiated, completed):
    file_name = f"{out_dir}/app.csv"
    row = [prot, load, offered, achieved, initiated, completed]
    with open(file_name, 'a') as f:
        writer = csv.writer(f)
        groups = tail99.keys()
        for group in groups:
            row.append(round(tail99[group],3))
            row.append(round(tail999[group],3))
            row.append(round(median[group],3))
            row.append(round(mean[group],3))
        print(f"write_app_data() writting row {row}")
        writer.writerow(row)

def write_app_header(out_dir):
    file_name = f"{out_dir}/app.csv"
    header = ["protocol", "load", "offered", "achieved", "msgs_initiated", "msgs_completed"]
    for group in cfg.SIZE_GROUPS:
        group = f"{group[0]}-{group[1]}"
        header.append(f"{group}|tail99")
        header.append(f"{group}|tail999")
        header.append(f"{group}|median")
        header.append(f"{group}|mean")

    with open(file_name, 'w') as f:
        writer = csv.writer(f)
        writer.writerow(header)

def export_range_slowdown_cdf(out_dir, data, range, prot, load, create_plots=False):
    '''
    data is Nx3. Cols: (ts, req_size, slowdown)
    '''
    def write_app_cdf_header(file_name, percentiles):
        header = ["protocol", "load"]
        ptiles = [str(x) for x in percentiles]
        header += ptiles
        with open(file_name, "w") as f:
            writer = csv.writer(f)
            writer.writerow(header)

    def write_app_cdf_data(file_name, prot, load, cdf):
        row = [prot, load]
        row += cdf
        with open(file_name, "a") as f:
            writer = csv.writer(f)
            writer.writerow(row)

    percentiles = list(np.arange(1, 100))
    percentiles += [99.9, 99.99]
    slowdown_cdf = []
    for ptile in percentiles:
        slowdown_cdf.append(np.percentile(data[:, 2], ptile))
    assert len(slowdown_cdf) == len(percentiles)
    out_dir = f"{out_dir}/app/CDFs"
    hlp.make_dir(out_dir)
    file_name = f"{out_dir}/cdf_{range}.csv"
    write_app_cdf_header(file_name, percentiles)
    write_app_cdf_data(file_name, prot, load, slowdown_cdf)
    if create_plots:
        plot_data = {prot: np.array(slowdown_cdf)}
        hlp.plot_cdf(plot_data, f"CDF-{range}", "Slowdown", "", out_dir)

def aggr_app_data(directory, prot, load, simulator_source, create_plots=False):

    def sample_rrqs(data, target_millions=10):
        target_reqs = int(target_millions * 1000 * 1000)
        num_requests = data.shape[0]
        if (num_requests < target_reqs):
            return data
        else:
            return data[np.random.choice(num_requests, target_reqs, replace=False), :]



    def calc_slowdown_and_write(params, sldwn_view, param_file, msgs_initiated, msgs_completed,
                                offered, achieved, out_dir, prot, load, is_intra_func, num_aggr,
                                num_tor, num_host_per_tor):
        '''
        sldwn_view expected columns are : (ts_col, reqsz_col, remote_col, local_col, reqdur_col).
        '''
        print(f"Initial sample count: {sldwn_view.shape}")
        msgs_completed = sldwn_view.shape[0]
        sldwn_view = sample_rrqs(sldwn_view)
        print(f"Sampled down to: {sldwn_view.shape}")
        # slowdown is (ts, req_size, slowdown_value)
        slowdown = app_hlp.calculate_slowdown(params, sldwn_view, param_file, 
                                              0, 1, 2, 3, 4, is_intra_func,
                                              num_aggr, num_tor, num_host_per_tor,
                                              sortBySize=True)
        tail99 = {}
        tail999 = {}
        median = {}
        mean = {}
        
        for group in cfg.SIZE_GROUPS:
            cut_start = group[0]
            cutoff = group[1]
            relevant_msgs = slowdown[slowdown[:, 1] >= cut_start]
            relevant_msgs = relevant_msgs[relevant_msgs[:, 1] < cutoff]
            range = f"{cut_start}-{cutoff}"
            num_msgs = relevant_msgs.shape[0]
            print(f"There are {num_msgs} for group size {group}")

            skip_group = False
            if num_msgs < 100:
                skip_group = True
                print(f"Skipping group {group} because it only has {num_msgs} messages")
                if num_msgs > 0:
                    print(f"Here are the slowdowns for this group (ts, req_size, slowdown_value)")
                    print(relevant_msgs)

            if not skip_group:
                # For this range of sizes, create a cdf of the slowdowns
                export_range_slowdown_cdf(out_dir, relevant_msgs, range, prot, load, create_plots)

            if skip_group:
                tail99[range] = -1
                tail999[range] = -1
                median[range] = -1
                mean[range] = -1
            else:
                tail99[range] = np.percentile(relevant_msgs[:, 2], 99)
                tail999[range] = np.percentile(relevant_msgs[:, 2], 99.9)
                median[range] = np.percentile(relevant_msgs[:, 2], 50)
                mean[range] = np.mean(relevant_msgs[:, 2])
        write_app_data(out_dir, prot, load, offered, achieved, tail99, tail999, median, mean, msgs_initiated, msgs_completed)

    def process_load_lvl(ldir, out_dir, prot, param_file, load):

        # NOTE: IF THERE ARE INCAST MESSAGES, offered load will be underestimated. Incast message dispathces are logged as "sin" instead of "srq"
        params = hlp.import_param_file(param_file)
        app_file = f"{ldir}/{cfg.APP_FILE}"
        print(f"App Processing {app_file}")
        data = app_hlp.load_app_data(app_file)
        if data.shape[0] == 0:
            print(f"No requests received. Returning")
            return

        # The loaded timestamps start at 0s (and not 10s)
        start = 0
        trace_end = float(params["sim_dur"][0])
        print(f"After trimming: {data.shape}")
        data = hlp.trim_by_timestamp(data, start, trace_end, app_hlp.app_cols["timestamp"])
        print(f"After trimming: {data.shape}")
        # Goodput
        view = data[:, (app_hlp.app_cols["timestamp"],app_hlp.app_cols["event"],app_hlp.app_cols["req_size"],app_hlp.app_cols["local_addr"])]
        trace_start = float(params["start_tracing_at"][0]) - 10.0

        # Calculate overall goodput (including any incast traffic). Then, remove incast msgs and calculate latency.
        # Offered will be undercalculated because incast messages are marked as "sin" instead of as "srq".
        # Too unimportant to fix. Offered is not used.
        offered, achieved = app_hlp.calculate_goodput(view, params, 0, 1, 3, 2, trace_start, trace_end)
        
        print(f"Used {data.shape[0]} messages (all) to calculate goodput. Now removing incast messages")
        # Remove message sizes the correspond to incast messages
        data = data[data[:, app_hlp.app_cols["req_size"]] != cfg.gparams["incast_size"]]
        print(f"The new number of messages is {data.shape[0]}")

        if data.shape[0] == 0:
            print("No messages left after removing incast msgs. Exiting")
            return
        sldwn_view = data[data[:, app_hlp.app_cols["event"]] == app_hlp.app_events["rrq"]]
        sldwn_view = sldwn_view[:, (app_hlp.app_cols["timestamp"], 
                                              app_hlp.app_cols["req_size"],
                                              app_hlp.app_cols["remote_addr"],
                                              app_hlp.app_cols["local_addr"],
                                              app_hlp.app_cols["req_dur"])]
        msgs_initiated = 0
        msgs_completed = 0
        # Latency
        msgs_initiated = data[data[:, app_hlp.app_cols["event"]] == app_hlp.app_events["srq"]].shape[0]
        msgs_completed = data[data[:, app_hlp.app_cols["event"]] == app_hlp.app_events["rrq"]].shape[0]
        calc_slowdown_and_write(params, sldwn_view, param_file, msgs_initiated, msgs_completed, offered, achieved, out_dir, prot, load, app_hlp.is_intra_rack, 0, 0, 0)
        
    def process_load_lvl_dcpim(ldir, out_dir, prot, param_file, load):
        print(f"App dcPIM Processing {ldir}")
        params = hlp.import_param_file(param_file)
        app_file = f"{ldir}/{cfg.DCPIM_APP_FILE}"
        data, sim_start, sim_end = dchlp.get_app_data(app_file)

        print(f"LOADED dcpim app data. Shape: {data.shape}")

        # For ns-2, we do set `stop_at [expr $start_at + $simul_dur]`.
        # Then simul dur (sim_dur) is used to trim. Therefore, request completions after the
        # load generating phase are ignored (which makes sense as the conditions are not stable nor comparable to during
        # load generation).
        # Reqs generated     |... .. .. ... .[.. . ... .]|
        # Reqs completed     |     . .. ... .[. .. .. ..]|... .. .
        # This code ignores both the start of the simulation and the end and only measures within the brackets.
        # Note: for ns-2 sims, the start is not sampled at all. For dcPIM, it's trimmed.
        # Note: for ns-2 sims, flow initializations and completions are logged in separate rows.
        # The right bracket ] is defined by the start time of the last injected flow. Flows that _complete_ after that are ignored
        # The left bracket [ is defined by an input parameter. flows that _complete_ before that are ignored.
        # TODO: trim start.

        # find last injected request. It will signal the end of the load-generating part of the sim.
        sim_dur = sim_end - sim_start

        trace_start, trace_end = dchlp.get_trace_interval(params, sim_start, sim_end)

        # Create different rows for flow injection and completion events (input data are sorted by completion time).
        data = np.repeat(data, repeats=2, axis=0)
        print(f"Initial data shape: {data.shape}")
        # Add an event type column (1: injection, 0: completion)
        data = np.append(data, np.zeros((data.shape[0], 1)), axis=1)
        data[::2, data.shape[1]-1] = 1
        print(f"data shape after appending event column: {data.shape}")

        # Add a timestamp column (it shows the start time for injections and the finish time for completions)
        data = np.append(data, np.zeros((data.shape[0], 1)), axis=1)
        print(f"data shape after appending timestamp column: {data.shape}")
        data[:, data.shape[1] - 1] = np.where(data[:, data.shape[1] - 2] == 1, data[:, dchlp.ResultCols.START.value], data[:, dchlp.ResultCols.FINISH.value])
        print(f"sim starts at (s) {sim_start/1000/1000} ends at (s) {sim_end/1000/1000}, sim_dur (s) {sim_dur/1000/1000}. Trace start (s) {trace_start/1000/1000} - {data.shape}")

        print(f"Before trimming: {data.shape}")
        ts_col = 10
        data = hlp.trim_by_timestamp(data, trace_start, trace_end, ts_col)

        print(f"After trimming: {data.shape}")

        # Goodput
        event_col = 9
        num_hosts = int(params["num_hosts"][0])
        data_snt = data[data[:, event_col] == 1] 
        data_rec = data[data[:, event_col] == 0] 
        bytes_snt = np.sum(data_snt[:, dchlp.ResultCols.SIZE.value])
        bytes_rec = np.sum(data_rec[:, dchlp.ResultCols.SIZE.value])
        Gbps_sent = np.round((bytes_snt * 8.0 / 1000.0 / 1000.0 / 1000.0 / ((sim_end - trace_start)/1000/1000))/num_hosts, 4)
        Gbps_recv = np.round((bytes_rec * 8.0 / 1000.0 / 1000.0 / 1000.0 / ((sim_end - trace_start)/1000/1000))/num_hosts, 4)

        # Latency (slowdown is already calculated)
        msgs_initiated = data_snt.shape[0]
        msgs_completed = data_rec.shape[0]

        # Before calculating slowdown, remove requests with size equal to the incast size.
        # (ts_col, reqsz_col, remote_col, local_col, reqdur_col)
        print_cnt = 1
        print(f"Will provide DCPIM application data of shape {data_rec.shape} to calculate slowdown. Here are the first {print_cnt} rows:")
        #499320 for websearch, 500000 for hadoop and google
        size_to_exclude = cfg.gparams["incast_size_dcpim"]
        if "w5" in ldir:
            size_to_exclude = cfg.gparams["incast_size_dcpim_websearch"]
        for i in range(print_cnt):
            print(data_rec[i,:])
        data_rec = data_rec[data_rec[:, dchlp.ResultCols.SIZE.value] != size_to_exclude]
        print(f"Removing incast (size: {size_to_exclude}) messages yields shape: {data_rec.shape}")

        # slowdown is (ts, req_size, slowdown)
        slowdown = data_rec[:, (dchlp.ResultCols.START.value, dchlp.ResultCols.SIZE.value, dchlp.ResultCols.SLDN.value)]

        tail99 = {}
        tail999 = {}
        median = {}
        mean = {}

        for group in cfg.SIZE_GROUPS:
            cut_start = group[0]
            cutoff = group[1]
            relevant_msgs = slowdown[slowdown[:, dchlp.ResultCols.SIZE.value] >= cut_start]
            relevant_msgs = relevant_msgs[relevant_msgs[:, dchlp.ResultCols.SIZE.value] < cutoff]
            msg_range = f"{cut_start}-{cutoff}"

            print(f"There are {relevant_msgs.shape[0]} for group size {group}")
            
            num_msgs = relevant_msgs.shape[0]
            skip_group = False
            if num_msgs < 100:
                skip_group = True

            if not skip_group:
                export_range_slowdown_cdf(out_dir, relevant_msgs, msg_range, prot, load, create_plots)
            else:
                print(f"Not plotting slowdown CDFs as there are {relevant_msgs.shape[0]} relevant messages")

            if skip_group:
                tail99[msg_range] = -1
                tail999[msg_range] = -1
                median[msg_range] = -1
                mean[msg_range] = -1
            else:
                tail99[msg_range] = np.percentile(relevant_msgs[:, 2], 99)
                tail999[msg_range] = np.percentile(relevant_msgs[:, 2], 99.9)
                median[msg_range] = np.percentile(relevant_msgs[:, 2], 50)
                mean[msg_range] = np.mean(relevant_msgs[:, 2])
        write_app_data(out_dir, prot, load, Gbps_sent, Gbps_recv, tail99, tail999, median, mean, msgs_initiated, msgs_completed)

    def process_load_lvl_omnet(ldir, out_dir, prot, param_file, load):
        print(f"App Omnet Processing {ldir}")
        params = hlp.import_param_file(param_file)
        src_dir = f"{data_dir}/{ohlp.OMNET_REQUEST_SOURCE}"
        size_dir = f"{data_dir}/{ohlp.OMNET_REQUEST_SIZE}"
        dur_dir = f"{data_dir}/{ohlp.OMNET_REQUEST_DURATION}"
        ifaces, src_data, size_data, dur_data, num_aggr, num_tor, num_host_per_tor = ohlp.get_app_data(src_dir, size_dir, dur_dir)

        sendsize_dir = f"{data_dir}/{ohlp.OMNET_SENDER_REQUEST_SIZE}"
        snd_ifaces, sendsize_data, num_aggr, num_tor, num_host_per_tor = ohlp.get_sender_app_data(sendsize_dir)
        # Each two columns map to one interface. So for each iface, calculate thrpt and queueing

        all_rec_data = np.empty((0,0))
        for i, iface in enumerate(ifaces):
            idx = 2*i
            # data: (ts, size [B], remote, local, duration [s])
            origin, data = ohlp.get_app_data_single_origin(src_data, size_data, dur_data, idx, params, iface)
            if all_rec_data.shape[0] == 0:
                all_rec_data = data
            else:
                all_rec_data = np.concatenate((all_rec_data, data), axis=0)
        
        if all_rec_data.shape[0] == 0:
            print(f"No requests received. Returning")
            return
        
        all_snd_data = np.empty((0,0))
        for i, iface in enumerate(snd_ifaces):
            idx = 2*i
            # data row is: (ts, size [B])
            snd_origin, snd_data = ohlp.get_sender_app_data_single_origin(sendsize_data, idx, params, iface)
            if all_snd_data.shape[0] == 0:
                all_snd_data = snd_data
            else:
                all_snd_data = np.concatenate((all_snd_data, snd_data), axis=0)

        trace_end = float(params["sim_dur"][0])
        trace_start = float(params["start_tracing_at"][0]) - 10.0
        # Sort data based on timestamp
        all_snd_data = all_snd_data[all_snd_data[:,0].argsort()]
        all_rec_data = all_rec_data[all_rec_data[:,0].argsort()]

        all_servers = [int(iface.split("server[")[1].split("]")[0]) for iface in snd_ifaces]
        
        all_servers = all_servers + [int(iface.split("server[")[1].split("]")[0]) for iface in ifaces]
        all_servers = list(set(all_servers))

        offered = ohlp.calculate_goodput(all_snd_data, 0, 1, len(all_servers), trace_start, trace_end)
        achieved = ohlp.calculate_goodput(all_rec_data, 0, 1, len(all_servers), trace_start, trace_end)

        # Now remove messages with size corresponding to incast
        # (ts_col, reqsz_col, remote_col, local_col, reqdur_col)
        print(f"Will provide OMNET application data of shape {all_rec_data.shape} to calculate slowdown. Here is the first row:")
        print(all_rec_data[0,:])

        all_rec_data = all_rec_data[all_rec_data[:, 1] != cfg.gparams["incast_size"]]

        print(f"Removed incast messages. New shape is: {all_rec_data.shape}")
        if all_rec_data.shape[0] == 0:
            print("No messages left after removing incast msgs. Returing without calculating latency")
            return

        msgs_initiated = all_snd_data.shape[0]
        msgs_completed = all_rec_data.shape[0]
        calc_slowdown_and_write(params, all_rec_data, param_file, msgs_initiated, msgs_completed, offered, achieved, out_dir, prot, load, ohlp.is_intra_rack, num_aggr, num_tor, num_host_per_tor)

    out_dir = f"{directory}/{cfg.OUT_DIR_NAME}"
    hlp.make_dir(out_dir)
    write_app_header(out_dir)

    param_file = f"{directory}/parameters"
    if simulator_source == cfg.OMNET_SIMULATOR:
        data_dir=f"{directory}/extracted_results"
        process_load_lvl_omnet(data_dir, out_dir, prot, param_file, load)
    elif simulator_source == cfg.DCPIM_SIMULATOR:
        data_dir=f"{directory}"
        process_load_lvl_dcpim(directory, out_dir, prot, param_file, load)
    elif simulator_source == cfg.NS2_SIMULATOR:
        process_load_lvl(directory, out_dir, prot, param_file, load)
    else:
        print(f"Unsuported simulator source {simulator_source}")
        exit(1)



###############################################################################################################
###############################################################################################################
###############################################################################################################
########################################## Queuing Data #######################################################
###############################################################################################################
###############################################################################################################

def write_q_data(out_dir, prot, load, tail99, tail999, max, median, mean, node):
    file_name = f"{out_dir}/q.csv"
    row = [prot, load, node, tail99, tail999, max, median, mean]
    mode = 'a'
    with open(file_name, mode) as f:
        writer = csv.writer(f)
        writer.writerow(row)

def write_q_header(out_dir):
    file_name = f"{out_dir}/q.csv"
    header = ["protocol", "load", "node", "tail99", "tail999", "max", "median", "mean"]
    with open(file_name, 'w') as f:
        writer = csv.writer(f)
        writer.writerow(header)

def write_qhist_data(out_dir, prot, load, node, bin_hist):
    file_name = f"{out_dir}/qhist.csv"
    row = [prot, load, node]
    hist_str = [f"{b}:{v}" for b, v in bin_hist]
    hist_str = f"{'|'.join(hist_str)}" + "|"
    row.append(hist_str)
    mode = 'a'
    with open(file_name, mode) as f:
        writer = csv.writer(f)
        writer.writerow(row)

def write_qhist_header(out_dir):
    file_name = f"{out_dir}/qhist.csv"
    header = ["protocol", "load", "node", "histogram"]
    with open(file_name, 'w') as f:
        writer = csv.writer(f)
        writer.writerow(header)

def write_qlinkhist_data(out_dir, prot, load, node, link, bin_hist):
    file_name = f"{out_dir}/qlinkhist.csv"
    row = [prot, load, node, link]
    hist_str = [f"{b}:{v}" for b, v in bin_hist]
    hist_str = f"{'|'.join(hist_str)}" + "|"
    row.append(hist_str)
    mode = 'a'
    with open(file_name, mode) as f:
        writer = csv.writer(f)
        writer.writerow(row)

def write_qlinkhist_header(out_dir):
    file_name = f"{out_dir}/qlinkhist.csv"
    header = ["protocol", "load", "node", "link", "histogram"]
    with open(file_name, 'w') as f:
        writer = csv.writer(f)
        writer.writerow(header)

qlink_percentiles = list(np.arange(1, 100))
qlink_percentiles += [99.9, 99.99, 99.999, 99.9999]

def write_q_link_data(out_dir, prot, load, max, median, mean, link, ptiles):
    file_name = f"{out_dir}/qlink.csv"
    row = [prot, load, link, max, median, mean]
    row += ptiles
    mode = 'a'
    with open(file_name, mode) as f:
        writer = csv.writer(f)
        writer.writerow(row)

def write_q_link_header(out_dir):
    file_name = f"{out_dir}/qlink.csv"
    header = ["protocol", "load", "link", "max", "median", "mean"]
    ptlies = [f"tail{str(p)}" for p in qlink_percentiles]
    header += ptlies
    with open(file_name, 'w') as f:
        writer = csv.writer(f)
        writer.writerow(header)

def aggr_q_data(directory, prot, load, simulator_source):

    ##########################################################
    def report_per_link_data(orig_to_data, out_dir):
        '''
        orig_to_data is  origin -> link -> data where origin is the id/addr of the source,
        link is link = f"{origin}_{end}" and data is at least ndarray((timestamp, queue_len))
        '''
        for origin in orig_to_data:
            for link in orig_to_data[origin]:
                # Select percentiles
                max = round(np.max(orig_to_data[origin][link][:, 1]) / 1000.0, 2)
                median = round(np.percentile(orig_to_data[origin][link][:, 1], 50) / 1000.0, 2)
                mean = round(np.mean(orig_to_data[origin][link][:, 1]) / 1000.0, 2)
                ptiles = []
                for ptile in qlink_percentiles:
                    ptiles.append(np.percentile(orig_to_data[origin][link][:, 1] / 1000.0, ptile))
                write_q_link_data(out_dir, prot, load, max, median, mean, link, ptiles)

        for origin in orig_to_data:
            if "host" in origin:
                continue
            for link in orig_to_data[origin]:
                # Histogram                
                bin_size = 5 * 1000 # 5 KB
                bins, histogram = hlp.get_histogram(orig_to_data[origin][link][:,1], bin_size, calc_probs=True)
                write_qlinkhist_data(out_dir, prot, load, origin, link, zip(bins, histogram))


    ##########################################################
    def process_grouped_data(orig_to_data, out_dir):
        '''
        orig_to_data is  origin -> link -> data where origin is the id/addr of the source,
        link is link = f"{origin}_{end}" and data is at least ndarray((timestamp, queue_len))
        '''
        print(f"Processing grouped data to {out_dir}")

        orig_to_sum = {}
        for origin in orig_to_data:
            for link in orig_to_data[origin]:
                if origin not in orig_to_sum:
                    orig_to_sum[origin] = np.copy(orig_to_data[origin][link][:, (0,1)])
                else:
                    orig_to_sum[origin][:,1] = orig_to_sum[origin][:,1] + orig_to_data[origin][link][:,1]


        for origin in orig_to_sum:
            if "host" in origin:
                continue
            bin_size = 20 * 1000 # 20 KB
            bins, histogram = hlp.get_histogram(orig_to_sum[origin][:,1], bin_size, calc_probs=True)
            write_qhist_data(out_dir, prot, load, origin, zip(bins, histogram))

        tail99 = 0
        tail999 = 0
        max = 0
        median = 0
        mean = 0
        for origin in orig_to_sum:
            tail99 = round(np.percentile(orig_to_sum[origin][:, 1], 99) / 1000.0, 2)
            tail999 = round(np.percentile(orig_to_sum[origin][:, 1], 99.9) / 1000.0, 2)
            max = round(np.max(orig_to_sum[origin][:, 1]) / 1000.0, 2)
            median = round(np.percentile(orig_to_sum[origin][:, 1], 50) / 1000.0, 2)
            mean = round(np.mean(orig_to_sum[origin][:, 1]) / 1000.0, 2)
            write_q_data(out_dir, prot, load, tail99, tail999, max, median, mean, origin)


    ##########################################################

    def process_load_lvl(ldir, out_dir, prot, param_file, load):
        print(f"Q Processing {ldir}")
        params = hlp.import_param_file(param_file)
        data_dir = f"{ldir}/qmon"
        area_subdirs = hlp.get_subdirs(data_dir)
        orig_to_data = {} # origin -> link -> data
        for adir in area_subdirs:
            if "throughput_all_avg" in adir:
                continue
            area = adir.split("/")[-1]
            area = area.split("_")[0]
            files = hlp.get_files(adir)
            for file_name in files:
                file_path = f"{adir}/{file_name}"
                origin = file_name.split("_")[0]
                areaorig = f"{area}_{origin}"
                end = file_name.split("_")[1].split(".")[0]
                link = f"{areaorig}_{end}"

                stop = float(params["sim_dur"][0])
                data = qmon_hlp.load_and_trim_qmon_data(file_path, stop)
                data = data[:,
                        (qmon_hlp.qmon_cols["timestamp"],
                            qmon_hlp.qmon_cols["q_sz_bytes"],
                            qmon_hlp.qmon_cols["byte_departures"])] 
                

                if areaorig not in orig_to_data:
                    orig_to_data[areaorig] = {}
                orig_to_data[areaorig][link] = data

        process_grouped_data(orig_to_data, out_dir)
        report_per_link_data(orig_to_data, out_dir)

    ##########################################################

    def process_load_lvl_dcpim(ldir, out_dir, prot, param_file, load):
        print(f"Q dcPIM Processing {ldir}")
        params = hlp.import_param_file(param_file)
        q_file = f"{ldir}/{cfg.DCPIM_Q_FILE}"
        data = dchlp.get_queue_data(q_file)

        # Trim data consistently with app data and other simulators
        _, sim_start, sim_end = dchlp.get_app_data(f"{ldir}/{cfg.DCPIM_APP_FILE}")
        trace_start, trace_end = dchlp.get_trace_interval(params, sim_start, sim_end)
        data = hlp.trim_by_timestamp(data, trace_start, trace_end, dchlp.qCols.TS.value)

        # Count total dropped bytes fmi
        max_dropped_bytes = np.max(data[:, dchlp.qCols.DROPS_BYTES.value])
        print(f"Dropped bytes = {max_dropped_bytes}")
        assert max_dropped_bytes < 10, "There where packet drops in this simulation. Are they considered properly?"

        

        # Group data by link
        orig_to_data = {} # origin -> link -> data
        orig_to_data = dchlp.group_by_origin(data)

        # the timeseries now contain ts,qlen,departures

        # print(orig_to_data.keys())
        # print("----=")
        # print(orig_to_data["tor_0"].keys())
        # print("----=")
        # print(orig_to_data["tor_0"]["tor_0_1"])

        process_grouped_data(orig_to_data, out_dir)
        report_per_link_data(orig_to_data, out_dir)


    ##########################################################
    
    def process_load_lvl_omnet(ldir, out_dir, prot, param_file, load):
        print(f"Q Omnet Processing {ldir}")
        params = hlp.import_param_file(param_file)
        byte_dir = f"{data_dir}/{ohlp.OMNET_BYTE_SAMPLE}"
        q_dir = f"{data_dir}/{ohlp.OMNET_Q_SAMPLE}"

        orig_to_data = {} # origin -> link -> data (origin is f"{origin_area}{origin}"")

        # TODO: create dataclass
        ifaces, q_data, byte_data, num_aggr, num_tor, num_host_per_tor = ohlp.get_qlen_and_byte_data(byte_dir, q_dir)
        # Each two columns map to one interface. So for each iface, calculate thrpt and queueing
        for i, iface in enumerate(ifaces):
            idx = 2*i
            origin, origin_area, end, end_area, throughput, qing = ohlp.get_qlen_and_throughput_single_origin(byte_data, q_data, idx, params, iface, num_aggr, num_tor, num_host_per_tor)
            if throughput is None:
                continue
            areaorig = f"{origin_area}_{origin}"
            data = qing
            data = np.append(data, throughput[:,1].reshape(throughput.shape[0], 1), axis=1)

            link = f"{areaorig}_{end}"
            if areaorig not in orig_to_data:
                orig_to_data[areaorig] = {}
            orig_to_data[areaorig][link] = data
        process_grouped_data(orig_to_data, out_dir)
        report_per_link_data(orig_to_data, out_dir)

    ##########################################################

    out_dir = f"{directory}/{cfg.OUT_DIR_NAME}"
    hlp.make_dir(out_dir)
    write_q_header(out_dir)
    write_qhist_header(out_dir)
    write_q_link_header(out_dir)
    write_qlinkhist_header(out_dir)
    param_file = f"{directory}/parameters"
    if simulator_source == cfg.OMNET_SIMULATOR:
        data_dir=f"{directory}/extracted_results"
        process_load_lvl_omnet(data_dir, out_dir, prot, param_file, load)
    elif simulator_source == cfg.DCPIM_SIMULATOR:
        process_load_lvl_dcpim(directory, out_dir, prot, param_file, load)
    elif simulator_source == cfg.NS2_SIMULATOR:
        process_load_lvl(directory, out_dir, prot, param_file, load)
    else:
        print(f"Unsuported simulator source {simulator_source}")
        exit(1)


###############################################################################################################
###############################################################################################################
###############################################################################################################
###############################################################################################################
###############################################################################################################
###############################################################################################################