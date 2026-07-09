import helper as hlp
import numpy as np
import time
import multiprocessing
import worker_manager as workm

cc_cols_micro = {
    "timestamp": 0,
    "event": 1,
    "local_addr": 2,
    "uplink_msgs": 3,
    "num_uniq_reqid": 4,
    "num_dest": 5,
    "outbound_msgs": 6,
    "wildcard": 7,
    "ce_marked_ratio": 8,
    "grant_bkt_sz": 9,  # in sched
    "idle_while_work_cnt": 10,
    "poll_cnt": 11,
    "inter_budget_bytes": 12,  # in budgets
    "intra_budget_bytes": 13,
    "inter_backlog_bytes": 14,
    "intra_backlog_bytes": 15,
}

cc_cols_hybrid = {
    "timestamp": 0,
    "event": 1,
    "local_addr": 2,
    "wildcard": 3,
    "srpb_max_min": 4,
    "srpb_max_min_nw": 5,
    "srpb_max_min_host": 6,
    "budget_bytes": 7,
    "num_outbound_msg": 8,
    "num_inbound_msg": 9,
    "num_active_outbound": 10,
    "num_active_inbound": 11,
    "num_bytes_expected": 12,
    "bytes_in_flight_rec_sum": 13,
    "srpb_max_mean": 14,
    "srpb_max_mean_nw": 15,
    "srpb_max_mean_host": 16,
    "srpb_sum": 17,
    "avg_nw_marked_ratio_": 18,
    "avg_host_marked_ratio_": 19,
    "bytes_in_flight_sndr_sum": 20,
    "credit_backlog": 21,
    "markable_pkts_recvd_": 22,
    "marked_pkts_recvd_host": 23,
    "marked_pkts_recvd_nw": 24,
    "global_SRPT_pkt_sent_ratio": 25,
    "global_SRPT2_pkt_sent_ratio": 26,
    "global_SRPT3_pkt_sent_ratio": 27,
    "global_SRPT4_pkt_sent_ratio": 28,
    "global_SRPT5_pkt_sent_ratio": 29,
    "global_SRPT6_pkt_sent_ratio": 30,
    "global_SRPT7_pkt_sent_ratio": 31,
    "global_SRPT8_pkt_sent_ratio": 32,
    "global_SRPT9_pkt_sent_ratio": 33,
    "global_SRPT10_pkt_sent_ratio": 34,
    "global_budget_bytes": 35,
    "global_credit_backlog": 36,
    "global_bif_rec": 37,
    "global_avg_marked_ratio": 38,
    "global_SRPT_credit_sent_ratio": 39,
    "global_SRPT2_credit_sent_ratio": 40,
    "global_SRPT3_credit_sent_ratio": 41,
    "global_SRPT4_credit_sent_ratio": 42,
    "global_SRPT5_credit_sent_ratio": 43,
    "global_SRPT6_credit_sent_ratio": 44,
    "global_SRPT7_credit_sent_ratio": 45,
    "global_SRPT8_credit_sent_ratio": 46,
    "global_SRPT9_credit_sent_ratio": 47,
    "global_SRPT10_credit_sent_ratio": 48,
    "pacer_backlog_credit": 49,
    "pacer_backlog_data": 50,
    "global_ratio_uplink_busy": 51,
    "global_ratio_uplink_idle_while_data": 52,
    "srpb_max": 53,
    "credit_from_rcver_0": 54,
    "credit_from_rcver_1": 55,
    "credit_from_rcver_2": 56,
    "credit_from_rcver_3": 57,
    "credit_from_rcver_4": 58,
    "credit_to_sender_0": 59,
    "credit_to_sender_1": 60,
    "credit_to_sender_2": 61,
    "credit_to_sender_3": 62,
    "credit_to_sender_4": 63,
}

cc_cols = {"micro-hybrid": cc_cols_hybrid}

cc_events = {
    "pol": 0.0,  # periodic sampling (every 1000us or so)
    "snd": 1.0,
    "biv": 2.0,
    "rgr": 3.0,  # Receive grant (credit amount in wildcard)
    "rrr": 4.0,  # Receive reqrdy (credit amount in wildcard)
}


def cols(cc_scheme):
    """
    Returns a dictionary mapping column names to numbers
    """
    res = None
    if cc_scheme in cc_cols:
        res = cc_cols[cc_scheme]
    else:
        res = cc_cols_micro
    return res


def cc_convert(val):
    if val not in cc_events:
        print(f"key {val} not in cc trace events. Exiting.")
        exit(1)
    return cc_events[val]


def load_cc_data(path, cc_scheme, measurement_window, param_file):
    cc_data = hlp.load_data(
        f"{path}/cc_trace.str", converter=cc_convert, col=cols(cc_scheme)["event"]
    )
    measure_start, measure_stop = hlp.calc_measurment_start_stop(
        param_file, measurement_window
    )
    print(f"Measure start {measure_start}, measure stop {measure_stop}")
    # Data
    cc_data = hlp.trim_by_timestamp(
        cc_data, measure_start, measure_stop, cols(cc_scheme)["timestamp"]
    )
    return cc_data


scheme_to_params = {
    "micro-elet": [
        "sender_budget_bytes",
        "budget_bytes",
        "grant_pacer_backlog",
        "data_pacer_backlog",
        "budget_backlog_bytes",
        "num_outbound_msg",
        "num_inbound_msg",
        "num_queued_grant_req",
        "sum_inbound_credit_reqs_bytes",
        "num_active_outbound_msg",
        "num_grant_reqs_sent",
        "num_grants_sent",
        "sum_srpb_max",
        "sum_srpb",
        "avg_marked_ratio",
        "avg_elet_size",
        "sum_bytes_in_flight",
        "granted_bytes_at_sender",
        "num_active_receivers",
        "granted_bytes_queue_len",
        "num_target_receivers",
        "outbound_per_target",
        "outbound_active_per_target",
        "sum_idle_when_inbound",
        "sum_idle_when_outbound",
    ],
    "micro-hybrid": dict(
        filter(lambda pair: pair[1] > 3, cc_cols_hybrid.items())
    ).keys(),
    "noop": None,
}


def get_proc_fun(cc_scheme):
    """
    Returns the processing function suitable for cc_scheme
    """
    res = None
    if cc_scheme in scheme_to_proc_fun:
        res = scheme_to_proc_fun[cc_scheme]
    else:
        raise Exception(f"Unknown scheme {cc_scheme}")
    return res


def get_proc_params(cc_scheme):
    res = None
    if cc_scheme in scheme_to_params:
        res = scheme_to_params[cc_scheme]
    else:
        raise Exception(f"Unknown scheme {cc_scheme}")
    return res


# @profile


def process_elet_data(cc_data, params, prot, load, vars_to_treat):
    """
    Returns a dict: variable_name -> metric -> host_addr -> value(s)
    """
    # Create a process for every metric because this is slow..
    pool = []
    res = {v: {} for v in vars_to_treat}  # var_name -> results
    for var in vars_to_treat:
        res[var] = basic_variable_treatment(var, cc_data, params)
    return res


# @profile


def basic_variable_treatment(var_name, cc_data, params):
    """
    Returns a nested dict: metric -> host_addr -> value(s)
    """
    res = {}
    res["ts"] = {}
    host_addr = params["host_addr"]
    cc_scheme = params["r2p2_cc_scheme"][0]
    # Only keep poll events
    poll_data = cc_data[cc_data[:, cols(cc_scheme)["event"]] == cc_events["pol"]]
    # Only keep the timestamp, host_addr and var_name columns
    poll_data = poll_data[
        :,
        (
            cols(cc_scheme)["timestamp"],
            cols(cc_scheme)["local_addr"],
            cols(cc_scheme)[var_name],
        ),
    ]
    for host in host_addr:
        # Only keep the rows of host
        # comparison should be fine since both values are converted from integer to float64 (...)
        host = np.float64(host)
        tmp = poll_data[poll_data[:, 1] == host, :]
        if tmp.shape[0] == 0:
            continue
        if "global" in var_name:
            res["ts"]["global"] = tmp[:, (0, 2)]
            return res
        res["ts"][int(host)] = tmp[:, (0, 2)]
    return res


scheme_to_proc_fun = {"micro-hybrid": process_elet_data, "noop": None}


def write_time_series(data, this_out_dir, varname):
    """
    data is [(timestamp, value)]
    """
    hlp.make_dir(this_out_dir)
    file_name = f"{this_out_dir}/{varname}.csv"
    header = f"timestamp,{varname}"
    np.savetxt(
        file_name,
        data,
        delimiter=",",
        fmt=["%.12f", "%.2f"],
        header=header,
        comments="",
    )

def output_results_task(these_res, varname, metric_type, path, prot, create_plots):
    hlp.make_dir(path)
    if metric_type == "ts":
        if create_plots:
            hlp.plot_time_series(these_res, path, varname, prot)
        write_time_series(these_res, path, varname)

def output_results(results, cc_plot_path, prot, load, create_plots):
    """
    results are a nested dictionary: var_name-> metric_type-> host_addr-> value(s)
    They are converted to : var_name-> metric_type-> host_addr-> value(s)
    """
    # start_time = {}
    # proc_time = {}

    # start_time["conv"] = time.process_time()

    # convert
    tmpr = {}
    for varname in results:
        if varname not in tmpr:
            tmpr[varname] = {}
        for metric_type in results[varname]:
            if metric_type not in tmpr[varname]:
                tmpr[varname][metric_type] = {}
            for host_addr in results[varname][metric_type]:
                if host_addr not in tmpr[varname][metric_type]:
                    tmpr[varname][metric_type][host_addr] = {}
                tmpr[varname][metric_type][host_addr] = results[varname][metric_type][
                    host_addr
                ]

    # proc_time["conv"] = time.process_time() - start_time["conv"]

    # print(f"+++ After converting: {hlp.process_memory_GB()}GB")
    # print(f"cc mem: results size = {hlp.get_size(results)/1000.0/ 1000.0}MB")
    # print(f"cc mem: tmpr size = {hlp.get_size(tmpr)/1000.0/ 1000.0}MB")

    # start_time["print"] = time.process_time()
    pool = []
    for varname in tmpr:
        for metric_type in tmpr[varname]:
            for host_addr in tmpr[varname][metric_type]:
                these_res = tmpr[varname][metric_type][host_addr]
                path = f"{cc_plot_path}/{varname}/load_{load}/host_{host_addr}"

                pool.append(workm.Task(output_results_task, (these_res, varname, metric_type, path, prot, create_plots)))

    if len(pool) > 0:
        workm.add_tasks(pool, task_family="CC subpool", suggested_batch_size=20)

    # proc_time["print"] = time.process_time() - start_time["print"]

    # for key in proc_time:
    #     print(f"Processing stage {key} took {proc_time[key]} seconds")
