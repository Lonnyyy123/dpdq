import helper as hlp
import numpy as np
import app_hlp
from pprint import pprint
import topo_helper as thlp
from cfg import gparams
import time
import math

app_cols = {
    "timestamp": 0,
    "event": 1,
    "local_addr": 2,
    "remote_addr": 3,
    "thread_id": 4,
    "req_id": 5,
    "app_lvl_id": 6,
    "req_dur": 7,
    "req_size": 8,
    "resp_size": 9,
    "pending_tasks": 10,
    "pool_size": 11,
}

app_events = {
    "srq": 0.0,
    "rrq": 1.0,
    "srs": 2.0,
    "suc": 3.0,
    "sin": 4.0,
}

def app_convert(val):
    if val not in app_events:
        print(f"key {val} not in app trace events. Exiting.")
        exit(1)
    return app_events[val]

def load_app_data(path, exclude_req_size=None):
    data = hlp.load_data(path, converter=app_convert, col=app_cols["event"])
    data[:, app_cols["timestamp"]] = data[:, app_cols["timestamp"]] - 10
    if exclude_req_size:
        print(f"Excluding rows with request size: {exclude_req_size}")
        assert isinstance(exclude_req_size, int), "exclude_size is not an int"
        data = data[data[:, app_cols["req_size"]] != exclude_req_size]
    return data

def calculate_goodput(data, params, ts_col, event_col, addr_col, reqsz_col, trace_start, trace_end):
    '''
    Calculates application level throughput.
    Incomplete msgs will not appear here.
    (despite tf that some pkts may have been received by lower layers)
    data is rows of timestamp, event, req_size, local_addr
    '''
    host_addr = params["host_addr"]
    num_hosts = len(host_addr)
    data_snt = data[data[:, event_col] == app_hlp.app_events["srq"]]
    data_rec = data[data[:, event_col] == app_hlp.app_events["rrq"]]
    bytes_snt = np.sum(data_snt[:, reqsz_col])
    bytes_rec = np.sum(data_rec[:, reqsz_col])

    dur = trace_end - trace_start
    Gbps_sent = np.round((bytes_snt * 8.0 / 1000.0 / 1000.0 / 1000.0 / dur)/num_hosts, 4)
    Gbps_recv = np.round((bytes_rec * 8.0 / 1000.0 / 1000.0 / 1000.0 / dur)/num_hosts, 4)

    return Gbps_sent, Gbps_recv

def calculate_goodput_rolling_avg(data):
    # calculate moving averages
    data = data[:, (app_hlp.app_cols["event"], app_cols["timestamp"], app_cols["req_size"])]
    data_snt = data[data[:, 0] == app_hlp.app_events["srq"]][:, (1,2)]
    data_rcv = data[data[:, 0] == app_hlp.app_events["rrq"]][:, (1,2)]
    group_size = data_snt.shape[0]/100
    offered_v_time = hlp.calc_gpt(data_snt, group_size)
    goodput_v_time = hlp.calc_gpt(data_rcv, group_size)
    offered_v_time_dta = hlp.get_rolling_avg(offered_v_time[:, 1])
    goodput_v_time_dta = hlp.get_rolling_avg(goodput_v_time[:, 1])
    offered_v_time = np.column_stack((offered_v_time[:, 0], offered_v_time_dta/1000.0/1000.0/1000.0*8.0))
    goodput_v_time = np.column_stack((goodput_v_time[:, 0], goodput_v_time_dta/1000.0/1000.0/1000.0*8.0))
    return offered_v_time, goodput_v_time

def calculate_slowdown_moving_avg(slowdown, slowdown_col):
    '''
    slowdown is expected to be Nx3 (sorted by time) and only have rrqs
    (ts, msg size, slowdown)
    returns (ts, slowdown)
    '''
    sl_ra = hlp.get_rolling_avg(slowdown[:, 2])
    return np.column_stack((slowdown[:,0], sl_ra))


def is_intra_rack(local_addr, remote_addr, param_file, num_aggr, num_tor, num_host_per_tor):
    host_to_neighbors = thlp.get_host_to_rack_neighbors_dict(param_file)
    is_intra_rack = remote_addr in host_to_neighbors[local_addr]
    return is_intra_rack

def calculate_slowdown(params, data, param_file, ts_col, reqsz_col, remote_col, local_col, 
                       reqdur_col, is_intra_fun, num_aggr, num_tor, num_host_per_tor, sortBySize=False):
    '''
    Expects (ts_col, reqsz_col, remote_col, local_col, reqdur_col) columns. 
    Returns just three (ts, req_size, slowdown)
    Must only have rrq events
    '''
    leaf_link_speed_gbps = int(params["leaf_link_speed_gbps"][0])
    core_link_speed_gbps = int(params["core_link_speed_gbps"][0])
    core_link_latency_s = float(params["core_link_latency_ms"][0]) / 1000.0
    leaf_link_latency_s = float(params["leaf_link_latency_ms"][0]) / 1000.0
    slow_col = data.shape[1]

    print(f"Calculating slowdown with the following params: {leaf_link_speed_gbps} {core_link_speed_gbps} {core_link_latency_s} {leaf_link_latency_s}")

    data = np.append(data, np.zeros((data.shape[0], 1)), axis = 1)
    data = np.apply_along_axis(calc_per_msg_slowdown, 1, data, slow_col, param_file,
                               leaf_link_speed_gbps, core_link_speed_gbps,
                               core_link_latency_s, leaf_link_latency_s, reqsz_col, remote_col, local_col,
                            reqdur_col, is_intra_fun, num_aggr, num_tor, num_host_per_tor)
    data = data[:, (ts_col, reqsz_col, slow_col)]
    if sortBySize:
        data = data[data[:,1].argsort()]
    return data

def calculate_slowdown_each(params, data, param_file, ts_col, reqsz_col, remote_col, local_col, 
                       reqdur_col, is_intra_fun, num_aggr, num_tor, num_host_per_tor, sortBySize=False):
    '''
    Expects (ts_col, reqsz_col, remote_col, local_col, reqdur_col) columns. 
    Returns just three (ts, req_size, slowdown)
    Must only have rrq events
    '''
    leaf_link_speed_gbps = int(params["leaf_link_speed_gbps"][0])
    core_link_speed_gbps = int(params["core_link_speed_gbps"][0])
    core_link_latency_s = float(params["core_link_latency_ms"][0]) / 1000.0
    leaf_link_latency_s = float(params["leaf_link_latency_ms"][0]) / 1000.0
    slow_col = data.shape[1]

    print(f"Calculating slowdown with the following params: {leaf_link_speed_gbps} {core_link_speed_gbps} {core_link_latency_s} {leaf_link_latency_s}")

    data = np.append(data, np.zeros((data.shape[0], 1)), axis = 1)
    data = np.apply_along_axis(calc_per_msg_slowdown, 1, data, slow_col, param_file,
                               leaf_link_speed_gbps, core_link_speed_gbps,
                               core_link_latency_s, leaf_link_latency_s, reqsz_col, remote_col, local_col,
                            reqdur_col, is_intra_fun, num_aggr, num_tor, num_host_per_tor)
    
    local_addr_mod16 = data[:, local_col].astype(np.int64) % 16
    mask_local_r2p2 = local_addr_mod16 < 8
    data1 = data[mask_local_r2p2]   # local_addr % 16 in [0,7]
    data2 = data[~mask_local_r2p2]  # local_addr % 16 in [8,15]
    
    data1 = data1[:, (ts_col, reqsz_col, slow_col)]
    data2 = data2[:, (ts_col, reqsz_col, slow_col)]

    if sortBySize:
        data1 = data1[data1[:, 1].argsort()]
        data2 = data2[data2[:, 1].argsort()]
    return data1, data2

def calc_per_msg_slowdown(row, slow_col, param_file,
                          leaf_link_speed_gbps, core_link_speed_gbps,
                          core_link_latency_s, leaf_link_latency_s,
                          reqsz_col, remote_col, local_col, reqdur_col,
                          is_intra_fun, num_aggr, num_tor, num_host_per_tor):
    msg_sz = row[reqsz_col] # not considering headers because it is a protocol feature (so if a preotocl has huge headerrs, it should pay for it in slowdown)
    prop_delay = 0
    store_and_forward_delay = 0
    link_speed = 0
    remote_addr = str(int(row[remote_col]))
    local_addr = str(int(row[local_col]))
    if is_intra_fun(local_addr, remote_addr, param_file, num_aggr, num_tor, num_host_per_tor):
        prop_delay +=  2*leaf_link_latency_s
        link_speed = leaf_link_speed_gbps
        # It is likely that Swift uses cut-through switches
        # store_and_forward_delay += thlp.calc_transmission_latency(min(gparams["packet_size"],msg_sz), link_speed)
    else:
        # This assumes a 2 tier topology
        if num_aggr == 0:
            # True dumbbell: host -> tor -> tor -> host (3 hops)
            prop_delay += 2*leaf_link_latency_s + core_link_latency_s
        else:
            # Leaf-spine: host -> tor -> aggr -> tor -> host (4 hops)
            prop_delay += 2*leaf_link_latency_s + 2 * core_link_latency_s
        link_speed = min(leaf_link_speed_gbps, core_link_speed_gbps)
        # store_and_forward_delay += 2*thlp.calc_transmission_latency(min(gparams["packet_size"], msg_sz), core_link_speed_gbps)
        # store_and_forward_delay += thlp.calc_transmission_latency(min(gparams["packet_size"], msg_sz), leaf_link_speed_gbps)
    base_trans_delay = thlp.calc_transmission_latency(msg_sz, link_speed)
    total_base_delay = base_trans_delay + store_and_forward_delay + prop_delay
    sl = row[reqdur_col] / total_base_delay
    assert sl > 0.99, f"Size: {msg_sz} Slowdown: {sl}, lat: {row[reqdur_col]*1000*1000}, total_base_delay: {total_base_delay*1000*1000}\n"\
            + f"intra: {is_intra_fun(local_addr, remote_addr, param_file, num_aggr, num_tor, num_host_per_tor)} prop: {prop_delay*1000*1000} base_trans_delay: {base_trans_delay}, saf del: {store_and_forward_delay} "\
            + f"local: {local_addr} remote: {remote_addr}\n"\
            + f"ROW: {row}"
    row[slow_col] = sl
    return row

def get_avg_goodput_vs_size(data, rpc_size_col, goodput_col, bin_count=10):
    '''
    into $bin_count bins such that each bin has the same number of req-resp pairs.
    Then compute the average goodput
    '''
    if bin_count == 0:
        return None
    data = data[data[:,rpc_size_col].argsort()]
    if bin_count > data.shape[0]:
        bin_count = data.shape[0]
    values_per_bin = math.floor(data.shape[0] / bin_count)
    rem = data.shape[0] % bin_count
    median_bin_value_to_avg_goodput = {}
    for bin_num in range(bin_count):
        bin_start = bin_num * values_per_bin
        bin_end = (bin_num + 1) * values_per_bin - 1
        if bin_num == bin_count - 1:
            bin_end += rem
        median_bin_value = np.percentile(data[bin_start:bin_end+1, rpc_size_col], 50)
        median_bin_value_to_avg_goodput[median_bin_value] = np.average(data[bin_start:bin_end+1, goodput_col])
    return median_bin_value_to_avg_goodput
        
    

def get_slowdown_vs_size(data, rpc_size_col, slowdown_col, bin_count=10):
    '''
    into $bin_count bins such that each bin has the same number of req-resp pairs.
    Then plots the latency (mean, median, tail) of each bin vs bins at the given load ($load).
    '''
    if bin_count == 0:
        return None
    data = data[data[:,rpc_size_col].argsort()]
    if bin_count > data.shape[0]:
        bin_count = data.shape[0]
    values_per_bin = math.floor(data.shape[0] / bin_count)
    rem = data.shape[0] % bin_count
    print(f"Number of values per bin: {values_per_bin}")
    # Represent each bin with its median size and extract metrics (for each)
    # Add rem to the last bin
    median_bin_value_to_median_slowdown = {}
    median_bin_value_to_999_slowdown = {}
    median_bin_value_to_99_slowdown = {}
    median_bin_value_to_95_slowdown = {}
    median_bin_value_to_1_slowdown = {}
    median_bin_value_to_mean_slowdown = {}
    for bin_num in range(bin_count):
        bin_start = bin_num * values_per_bin
        bin_end = (bin_num + 1) * values_per_bin - 1
        if bin_num == bin_count - 1:
            bin_end += rem
        median_bin_value = np.percentile(data[bin_start:bin_end+1, rpc_size_col], 50)
        median_bin_value_to_median_slowdown[median_bin_value] = np.percentile(data[bin_start:bin_end+1, slowdown_col], 50)
        median_bin_value_to_99_slowdown[median_bin_value] = np.percentile(data[bin_start:bin_end+1, slowdown_col], 99)
        median_bin_value_to_999_slowdown[median_bin_value] = np.percentile(data[bin_start:bin_end+1, slowdown_col], 99.9)
        median_bin_value_to_95_slowdown[median_bin_value] = np.percentile(data[bin_start:bin_end+1, slowdown_col], 95)
        median_bin_value_to_1_slowdown[median_bin_value] = np.percentile(data[bin_start:bin_end+1, slowdown_col], 1)
        median_bin_value_to_mean_slowdown[median_bin_value] = np.mean(data[bin_start:bin_end+1, slowdown_col])

    return median_bin_value_to_median_slowdown, median_bin_value_to_99_slowdown, median_bin_value_to_999_slowdown, median_bin_value_to_95_slowdown, median_bin_value_to_1_slowdown, median_bin_value_to_mean_slowdown

def get_fct_vs_size(data, rpc_size_col, fct_col, bin_count=10):
    '''
    into $bin_count bins such that each bin has the same number of req-resp pairs.
    '''
    if bin_count == 0:
        return None
    data = data[data[:,rpc_size_col].argsort()]
    if bin_count > data.shape[0]:
        bin_count = data.shape[0]
    values_per_bin = math.floor(data.shape[0] / bin_count)
    rem = data.shape[0] % bin_count
    # Represent each bin with its median size and extract metrics (for each)
    # Add rem to the last bin
    median_bin_value_to_median_fct = {}
    median_bin_value_to_99_fct = {}
    median_bin_value_to_95_fct = {}
    for bin_num in range(bin_count):
        bin_start = bin_num * values_per_bin
        bin_end = (bin_num + 1) * values_per_bin - 1
        if bin_num == bin_count - 1:
            bin_end += rem
        median_bin_value = np.percentile(data[bin_start:bin_end+1, rpc_size_col], 50)
        median_bin_value_to_median_fct[median_bin_value] = np.percentile(data[bin_start:bin_end+1, fct_col], 50)
        median_bin_value_to_99_fct[median_bin_value] = np.percentile(data[bin_start:bin_end+1, fct_col], 99)
        median_bin_value_to_95_fct[median_bin_value] = np.percentile(data[bin_start:bin_end+1, fct_col], 95)
        # median_bin_value_to_mean_fct[median_bin_value] = np.mean(data[bin_start:bin_end+1, fct_col])

    return median_bin_value_to_median_fct, median_bin_value_to_95_fct, median_bin_value_to_99_fct


def plot_slowdown_vs_size(slowdown_v_sz, metric, prot, load, save_to):
        """
        Expects a dict: bin_median -> value (median/tail latency)
        """
        dir_name = f"{save_to}"
        hlp.make_dir(dir_name)
        x = {}
        y = {}
        xvals = slowdown_v_sz.keys()
        x[prot] = [str(size) for size in xvals]
        y[prot] = slowdown_v_sz.values()

        hlp.plot_1x1(x, y, f"{metric} - {load} Gbps", "Message size [bytes]",
                f"{metric}", save_to=dir_name, x_log=True, markers=True)# , ylim_bot=1) <- makes y range from 0.96 to 1.04 for some reason 