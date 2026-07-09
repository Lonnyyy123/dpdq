import numpy as np
import sys
import os
import psutil

import matplotlib
import matplotlib.pyplot as plt

from os import listdir
from os.path import isfile, join

# As imported by load_pkt() in pkt_processing()
# Full field is:
# event, time, from node, to node, pkt_type, pkt_size, flags, fid, src addr, dst addr, sec num, pkt id
pkt_cols = {
    "event": 0,
    "timestamp": 1,
    "src": 2,
    "dst": 3,
    "proto": 4,
    "pkt_size": 5,
}

# pkt trace
pkt_events = {
    "+": 0.0,
    "-": 1.0,
    "r": 2.0,
    "d": 3.0
}

pkt_protos = {
    "rtProtoDV": 0.0,
    "udp": 1.0,
    "tcp": 2.0,
    "ack": 3.0,
}


def get_size(obj, seen=None):
    """Recursively finds size of objects"""
    size = sys.getsizeof(obj)
    if seen is None:
        seen = set()
    obj_id = id(obj)
    if obj_id in seen:
        return 0
    # Important mark as seen *before* entering recursion to gracefully handle
    # self-referential objects
    seen.add(obj_id)
    if isinstance(obj, dict):
        size += sum([get_size(v, seen) for v in obj.values()])
        size += sum([get_size(k, seen) for k in obj.keys()])
    elif hasattr(obj, '__dict__'):
        size += get_size(obj.__dict__, seen)
    elif hasattr(obj, '__iter__') and not isinstance(obj, (str, bytes, bytearray)):
        size += sum([get_size(i, seen) for i in obj])
    return size


def pkt_event_convert(val):
    if val not in pkt_events:
        raise Exception(f"key {val} not in a known pkt trace event. Exiting.")
    return pkt_events[val]


def pkt_proto_convert(val):
    if val not in pkt_protos:
        raise Exception(
            f"key {val} not in a known pkt trace protocol. Exiting.")
    return pkt_protos[val]


def keep_some_rows(data, percentage):
    n = int(data.shape[0] * percentage)
    index = np.random.choice(data.shape[0], n, replace=False)
    return data[index]


def round_list(list, decimals):
    return [round(x, decimals) for x in list]


def get_subdirs(path):
    return [f.path for f in os.scandir(path) if f.is_dir()]


def get_files(path):
    return [f for f in listdir(path) if isfile(join(path, f))]


def load_data(data_file, delimiter=' ', skiprows=0, dtype=float, converter=None, col=None):
    '''
    Will convert string column col to a float column based on the converter.
    '''
    def iter_func():
        with open(data_file, 'r') as infile:
            for _ in range(skiprows):
                next(infile)
            for line in infile:
                line = line.rstrip().split(delimiter)
                if converter:
                    line[col] = converter(line[col])
                for item in line:
                    yield dtype(item)
        load_data.row_length = len(line)

    data = np.fromiter(iter_func(), dtype=dtype)
    data = data.reshape((-1, load_data.row_length))
    return data


def import_param_file(param_file):
    '''
    Returns the experiment parameters loaded from <file> as a dtct: param_name -> [val]
    Example lines of parameter file:
    router_op_mode random
    spine_addr 0 1 2 3
    '''
    res = dict()
    with open(param_file) as f:
        lines = f.readlines()
        for line in lines:
            line_items = line.split()
            res[line_items[0]] = line_items[1:]
    return res


def calc_gpt(data, group_sz):
    '''
    Returns momentary throughput timeseries (timestamp, thrpt_value)
    Expects a Nx2 ndarray (timestamps and byte departures (NOT cumulative))
    Groups the data into data.len / group_sz and calculates the avg goodput for each group.
    '''
    groups = np.array_split(data, group_sz)
    thrpt = np.zeros((len(groups), 2))
    for i, group in enumerate(groups):
        thrpt[i, 0] = group[0, 0]
        dur = group[-1, 0] - group[0, 0]
        thrpt[i, 1] = np.sum(group[:, 1]) / dur
    return thrpt


def calc_measurment_start_stop(param_file, window):
    '''
    param_file must have sim_start and sim_dur
    Returns the measurement start and stop time
    '''
    params = import_param_file(param_file)
    sim_start = float(params["sim_start"][0])
    sim_dur = float(params["sim_dur"][0])
    sim_stop = sim_start + sim_dur
    measure_start = sim_start + \
        ((1.0 - window) / 2.0) * sim_dur
    measure_stop = sim_stop - \
        ((1.0 - window) / 2.0) * sim_dur
    return measure_start, measure_stop


def trim_by_timestamp(data, start, stop, ts_col):
    '''
    Trims rows before start and after stop. Timestamps are in the
    ts_col column of data. data is a numpy array
    '''
    data = data[data[:, ts_col] >= start]
    data = data[data[:, ts_col] <= stop]
    return data


def extract_aggregate_metrics(data, incld_sum=False):
    '''
    Expects a dictionary.
    Returns a dict of aggr_metric->value. The metrics are produced from the input dict's values
    '''
    data = np.array(list(data.values()))
    res = {}
    if incld_sum:
        res["sum"] = np.sum(data)
    res["mean"] = np.mean(data)
    res["median"] = np.percentile(data, 50)
    res["99"] = np.percentile(data, 99)
    res["min"] = np.min(data)
    res["max"] = np.max(data)
    res["std"] = np.std(data)
    return res


# def print_failed_reqs(app_data_snt, app_data_suc):
#     '''
#     Finds and prints the requests that where sent but now response
#     was received for them
#     '''
#     # Finding the unsuccessfull requests - using app_lvl_id
#     adr = app_data_snt[:, app_cols["local_addr"]]
#     ids = app_data_snt[:, app_cols["app_lvl_id"]]
#     set_of_sent = set(zip(adr, ids))

#     adr = app_data_suc[:, app_cols["local_addr"]]
#     ids = app_data_suc[:, app_cols["app_lvl_id"]]
#     set_of_suc = set(zip(adr, ids))

#     diff = set_of_sent.difference(set_of_suc)
#     num_failed_reqs = len(diff)
#     if num_failed_reqs == 0:
#         print("There was a response for each request.")
#     else:
#         print(
#             f"!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!! There were {num_failed_reqs} requests for which there was no response.")
#         # print("Address Global_request_id request_size")
#         # for ad, id in diff:
#         #     tmp = app_data_snt[app_data_snt[:, app_cols["local_addr"]] == ad]
#         #     tmp = tmp[tmp[:, app_cols["app_lvl_id"]] == id]

#         #     print(tmp[:, (app_cols["local_addr"],
#         #                   app_cols["app_lvl_id"], app_cols["req_size"])])


def plot_moving_avg(column, prot, save_in):
    mvng_avg = moving_average(column, 5000)
    if (mvng_avg.shape[0] > 10):
        max = np.max(mvng_avg[:int(len(mvng_avg)/2)])
        mvng_avg = np.array(
            [item for item in mvng_avg if item < 3*max])
    y = {prot: mvng_avg*1000.0*1000.0}
    plot_title = "RPC moving average latency vs request count"
    plot_1x1(list(range(len(mvng_avg))), y, plot_title,
             "RPC count", "us", save_to=save_in)


def plot_time_series(data, path, varname, prot, y_axis_title="Unspecified", x_axis_title="Time [ms]"):
    '''
    data is timeseries of (timestamp, value)
    '''
    smoothing_windows = [
        1, max(1, int(data[:, 0].shape[0]/20)), max(1, int(data[:, 0].shape[0]/100))]
    # support data with multiple value columns: data[:,0]=ts, data[:,1:]=series
    num_series = data.shape[1] - 1
    if num_series < 1:
        raise ValueError("plot_time_series: data must have at least 2 columns (ts + value)")

    # determine labels for each series
    if isinstance(prot, (list, tuple)) and len(prot) == num_series:
        labels = list(prot)
    else:
        if num_series == 1:
            labels = [prot]
        else:
            # Try to infer common DCTCP/R2P2 labeling if y_axis_title mentions them
            if "DCTCP" in y_axis_title and "R2P2" in y_axis_title and num_series == 2:
                labels = ["DCTCP", "R2P2"]
            else:
                labels = [f"series_{i}" for i in range(num_series)]

    # smoothing_window -> label -> Nx1
    timestamps = {sm: None for sm in smoothing_windows}
    values = {sm: {} for sm in smoothing_windows}
    for sm in smoothing_windows:
        timestamps[sm] = data[sm - 1:, 0]
        for i in range(num_series):
            vals = moving_average(data[:, i + 1], sm)
            values[sm][labels[i]] = vals

    for sm in smoothing_windows:
        title = f"{varname} vs time ({sm} MA)"
        plot_1x1([x * 1000.0 for x in timestamps[sm]], values[sm], title,
                 x_axis_title, y_axis_title, save_to=path, markers=False)


def plot_1x1(x, y, title, x_label, y_label, y_log=False,
             x_log=False, ylim=None, ylim_bot=None, xlim=None, xlim_bot=None, save_to=None,
             markers=False, marker_size=0.7, legend_loc=None, x2=None, x2_label=None, x2_ref=None,
             categorical_x=False, just_points=False, include_moving_avg=False):
    """
    Expects x, x2 and y to be dicts with the same keys (key->[value])
    x, x2 can also be a list or a numpy array
    x2_ref must be a list..
    """
    matplotlib.rcParams['agg.path.chunksize'] = 10000  # to avoid OverflowError: In draw_path: Exceeded cell block limit
    font_size = 15
    matplotlib.rc('font', size=font_size)
    matplotlib.rc('axes', titlesize=font_size)
    matplotlib.rc('axes', labelsize=font_size)
    matplotlib.rc('xtick', labelsize=font_size)
    matplotlib.rc('ytick', labelsize=font_size)
    matplotlib.rc('figure', titlesize=font_size)

    fig, ax1 = plt.subplots(nrows=1, ncols=1, figsize=(20, 10))
    fig.subplots_adjust(bottom=0.01)
    ax1.margins(x=0)
    if ylim:
        ax1.set_ylim(top=ylim)
    if ylim_bot:
        ax1.set_ylim(bottom=ylim_bot)
    if xlim:
        ax1.set_xlim(right=xlim)
    if xlim_bot:
        ax1.set_xlim(left=xlim_bot)
    if y_log:
        ax1.set_yscale("log")
    if x_log:
        ax1.set_xscale("log")
    marker_list = ["s", "o", "v", "P", "*"]
    i = 0
    added_x2 = False

    colors = ['tab:blue', 'tab:orange', 'tab:green', 'tab:red', 'tab:purple',
              'tab:brown', 'tab:pink', 'tab:gray', 'tab:olive', 'tab:cyan']
    for key, value in y.items():
        if isinstance(x, list) or isinstance(x, np.ndarray):
            x_axis = x
        else:
            x_axis = x[key]

        # meh
        if not categorical_x:
            x_axis = [float(x) for x in x_axis]
        if not markers and not just_points:
            # , color=colors[i])
            ax1.plot(x_axis, value, label=key, linewidth=1.5)
        elif just_points:
            ax1.plot(x_axis, value, label=key, linestyle='None',
                     marker=marker_list[i], markersize=marker_size)  # , color=colors[i])
        else:
            ax1.plot(x_axis, value, label=key,
                     linewidth=1.5, marker=marker_list[i], markersize=marker_size)  # , color=colors[i])

        if include_moving_avg:
            window = int(value.shape[0]/50)
            if window > 1:
                mv_avg = moving_average(value, window)
                ax1.plot(x_axis[window-1:], mv_avg, linewidth=0.7,
                         label=f"Moving Avg {window}")  # , color=colors[i])

        i += 1
        i %= len(marker_list)

        # TODO: Hacky and arbitrary way... idealy the calleer provides a list. Must address
        if x2 and not added_x2:
            if isinstance(x2, list) or isinstance(x2, np.ndarray):
                x_axis2 = [str(val) for val in x2]
            else:
                x_axis2 = [str(val) for val in x2[key]]

            ax2 = ax1.twiny()
            if x_log:
                ax2.set_xscale("log")

            if not x2_ref:
                x2_ref = x_axis
            # Fml.. I don't get it but the following works while this: ( https://pythonmatplotlibtips.blogspot.com/2018/01/add-second-x-axis-below-first-x-axis-python-matplotlib-pyplot.html ) does not
            min_x = x2_ref[0]
            max_x = x2_ref[-1]
            ax1.set_xlim(left=min_x, right=max_x)
            x_range = max_x - min_x
            if len(x_axis2) > 1:
                proportional_ticks = [((x-min_x)/x_range) for x in x2_ref]
            else:
                proportional_ticks = [1]
            ax2.set_xticks(proportional_ticks)
            ax2.set_xticklabels(x_axis2)
            ax2.set_xlabel(f"{x2_label} - for {key}")
            ax2.xaxis.set_ticks_position('bottom')
            ax2.xaxis.set_label_position('bottom')
            ax2.spines["bottom"].set_position(("axes", -0.15))
            added_x2 = True

    ax1.set_title(title)
    if legend_loc is None:
        ax1.legend(loc='upper left')
    else:
        ax1.legend(loc=legend_loc)
    ax1.set_xlabel(x_label)
    ax1.set_ylabel(y_label)
    ax1.grid(which='major', linestyle="-")
    ax1.grid(which='minor', linestyle='--')

    fig.tight_layout()
    if not save_to:
        print("Not save to")
        save_path = os.getcwd() + title
    else:
        save_path = f"{save_to}/{title}"

    # fig.savefig(f"{save_path}.svg", format="svg")
    fig.savefig(f"{save_path}.png", format="png")
    # fig.clf()
    plt.close()  # to avoid memory explosion as more figures are created
    # gc.collect()


def get_rolling_avg(data):
    '''
    data is expected to be a vector
    '''
    data = np.cumsum(data)
    data = data / (1+np.indices(data.shape))
    return np.transpose(data)


def make_dir(path):
    try:
        os.makedirs(path, exist_ok=True)
    except OSError:
        print("Failed to create directory")


def moving_average(a, n):
    assert n > 0, "Moving average window ust be > 0"
    ret = np.cumsum(a, dtype=float)
    ret[n:] = ret[n:] - ret[:-n]
    return ret[n - 1:] / n


def weighted_moving_average(a, n, w):
    assert n > 0, "Moving average window ust be > 0"
    weighted = a * w
    weighted /= np.sum(w)
    print(weighted)
    return moving_average(weighted, n)


def has_prefix(word, prefixes):
    for prefix in prefixes:
        if word.startswith(prefix):
            return True
    return False


def substrings_in_word(word, substrings):
    '''
    Returns True if any substring is "in" the word
    example: substrings = ["hi", "mom"], word = hill -> True
    '''
    for s in substrings:
        if s in word:
            return True
    return False


def process_memory_GB():
    process = psutil.Process(os.getpid())
    mem_info = process.memory_info()
    return mem_info.rss / 1000.0 / 1000.0 / 1000.0


def plot_cdf(data, title, x_title, y_title, save_in, weights=None, x_log=False, plot_above=0.0):
    '''
    Expects data and weights to be dicts (name -> 1Dnumpy_array)
    the keys for the correspoinding values of both must be the same
    Will plot one line per name
    Weights must be the same lenght as data
    '''
    x_axis = {}
    y_axis = {}

    if not isinstance(data, dict):
        data = {y_title: data}
    if weights is None:
        weights = {}
        for key in data:
            weights[key] = np.ones(data[key].shape[0])
    if not isinstance(weights, dict):
        weights = {y_title: np.ones(data[y_title].shape[0])}

    for series in data:
        series_data = data[series]
        series_weights = weights[series]
        if series_data.shape != series_weights.shape:
            raise Exception(
                f"get_cdf: data.shape != weights.shape ({series_data.shape} != {series_weights.shape})")
        series_data = series_data.reshape(series_data.shape[0], 1)
        series_weights = series_weights.reshape(series_weights.shape[0], 1)
        series_data = np.concatenate((series_data, series_weights), axis=1)
        series_data = series_data.astype(float)
        series_data = series_data[series_data[:, 0].argsort()]
        total_weight = np.sum(series_data[:, 1])
        series_data[:, 1] = series_data[:, 1]/total_weight
        series_data[:, 1] = np.cumsum(series_data[:, 1])
        x_axis[series] = series_data[:, 0]
        y_axis[series] = series_data[:, 1]
    plot_1x1(x_axis, y_axis, title, x_title, y_title, save_to=save_in,
             x_log=x_log, legend_loc="lower right", ylim_bot=plot_above)

def get_histogram(series, bin_size, calc_probs=False):
    max_val = np.max(series)+0.000001
    bins = np.arange(0,max_val,bin_size)
    bins = np.append(bins, (bins[-1]+bin_size))
    bins = bins.astype(int)
    hist, bin_edges = np.histogram(series, bins)

    if calc_probs:
        N = np.sum(hist)
        hist = hist / N
        assert np.sum(hist) - 1.0 < 0.00001, f"Expected histogram sum was {np.sum(hist)} instead of 1"
    return bins[1:], hist