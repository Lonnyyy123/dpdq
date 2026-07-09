import numpy as np
import helper as hlp
import qmon_hlp
import csv
import re
import os
import subprocess
from enum import Enum

class ResultCols(Enum):
    FLOWID = 0
    SIZE = 1
    SRC = 2
    DST = 3
    START = 4
    FINISH = 5
    FCT = 6
    ORCT = 7
    SLDN = 8

class qCols(Enum):
    TS = 0
    SRC_TYPE = 1
    DST_TYPE = 2
    SRC_ID = 3
    DST_ID = 4
    QLEN_BYTES = 5
    ARRIVALS_BYTES = 6
    DEPARTURES_BYTES = 7
    DROPS_BYTES = 8


# Not using proper numpy loading because the output format seems arbitrary and may include comments..
def get_app_data(file):
    data = None
    # total_sent_packets = 0
    # total_pkt = 0
    # finish_time = 0
    # reach_check_point = 0
    s_time = 0

    # num_diffs = 0
    # large_diff = 0

    rows_added = 0

    with open(file) as f:
        lines = f.readlines()
        num_rows = 9
        data = np.zeros((len(lines), num_rows))
        for i in range(len(lines) - 1):
            line = lines[i]
            fields = line.split()

            if "queue pos" in line:
                continue
            if "queue " in line:
                continue
            if "qs" == fields[0] or fields[0] == "KPR:":
                continue
            if fields[0] == "##":
                continue
                # total_sent_packets = int(fields[9]) - int(fields[3])
                # total_pkt = int(fields[9])
                # finish_time = float(fields[1])
                # reach_check_point += 1
            else:
                flowId = int(fields[ResultCols.FLOWID.value])
                size = float(fields[ResultCols.SIZE.value])
                src = int(fields[ResultCols.SRC.value])
                dst = int(fields[ResultCols.DST.value])
                start_time = float(fields[ResultCols.START.value])
                end_time = float(fields[ResultCols.FINISH.value])
                fct = float(fields[ResultCols.FCT.value]) / 1000000.0
                sim_orct = float(fields[ResultCols.ORCT.value])
                sim_ratio = float(fields[ResultCols.SLDN.value])
                # orct = get_oracle_fct(src, dst, size)
                # ratio = fct / orct

                row = [flowId, size, src, dst, start_time, end_time, fct, sim_orct, sim_ratio]
                data[rows_added, :] = np.array(row)
                rows_added += 1
                # CONCLUSION: Using the simulator provided slowdown
                # if ratio - sim_ratio > 0.1:
                #     num_diffs += 1
                #     if ratio - sim_ratio > 0.5:
                #         print(f"DIFF {num_diffs}. Large diff: {large_diff} {ratio} {sim_ratio}")
                #         large_diff += 1
                # if ratio < 1.0:
                    # print(f"SOSSOSSOSSOSSOSSOSSOSSOSSOSSOSSOSSOS ratio < 1.0 SOSSOSSOSSOSSOSSOSSOSSOSSOSSOSSOSSOSSOS (calc ratio {ratio}, provided ratio {sim_ratio})")
                    # print(row)
                    # assert False. Assertion failed out of the box.
                if flowId == 0:
                    s_time = start_time / 1000000.0

    data = data[:rows_added, :]
    sim_end = np.max(data[:, ResultCols.START.value])
    sim_start = data[0, ResultCols.START.value]
    return data, sim_start, sim_end

def get_trace_interval(params, sim_start, sim_end):
    sim_dur = sim_end - sim_start

    ns2_tracing_duration = float(params["start_tracing_at"][0]) - float(params["sim_start"][0])
    ns2_sim_dur = float(params["sim_dur"][0])

    trace_ratio_last = ns2_tracing_duration / ns2_sim_dur

    trace_start = sim_start + sim_dur * trace_ratio_last
    trace_end = sim_end
    return trace_start, trace_end

def node_type_to_int(node_type: str):
    if node_type == "h" or node_type == "host":
        return 0
    elif node_type == "t" or node_type == "tor":
        return 1
    elif node_type == "a" or node_type == "aggr":
        return 2
    else:
        raise RuntimeError(f"Unknown node_type: {node_type}")
    
def int_to_node_type(node_type_id: int):
    if node_type_id == 0:
        return "host"
    elif node_type_id == 1:
        return "tor"
    elif node_type_id == 2:
        return "aggr"
    else:
        raise RuntimeError(f"Unknown node_type_id: {node_type_id}")

def get_queue_data(file):
    data = None
    rows_added = 0

    with open(file) as f:
        lines = f.readlines()
        num_rows = 9
        data = np.zeros((len(lines), num_rows))
        for i in range(len(lines) - 1):
            line = lines[i]
            fields = line.split()

            if "qs" == fields[0]:
                ts = float(fields[qCols.TS.value+1])*1000*1000
                src_type = node_type_to_int(fields[qCols.SRC_TYPE.value+1])
                dst_type = node_type_to_int(fields[qCols.DST_TYPE.value+1])
                src = int(fields[qCols.SRC_ID.value+1])
                dst = int(fields[qCols.DST_ID.value+1])
                qlen = int(fields[qCols.QLEN_BYTES.value+1])
                arrivals = int(fields[qCols.ARRIVALS_BYTES.value+1])
                departures = int(fields[qCols.DEPARTURES_BYTES.value+1])
                drops = int(fields[qCols.DROPS_BYTES.value+1])

                row = [ts, src_type, dst_type, src, dst, qlen, arrivals, departures, drops]
                data[rows_added, :] = np.array(row)
                rows_added += 1
    
    data = data[:rows_added, :]
    return data

def group_by_origin(data):
    '''
    Input: numpy array with qCols
    Output: origin -> link -> data
    Example:
        aggr_34': {'aggr_34_32': [2D array with timeseries data for specific link]

    words:
    host, tor, aggr
    '''
    orig_to_data = {} # origin -> link -> data
    for row in data:
        src = int(row[qCols.SRC_ID.value])
        dst = int(row[qCols.DST_ID.value])
        src_type = int_to_node_type(row[qCols.SRC_TYPE.value])
        dst_type = int_to_node_type(row[qCols.DST_TYPE.value])

        areaorig = f"{src_type}_{src}"
        link = f"{areaorig}_{dst}"
        if areaorig not in orig_to_data:
            orig_to_data[areaorig] = {}
        if link in orig_to_data[areaorig]:
            continue # data already incorporated. Wasteful
        relevant_view = data[data[:, qCols.SRC_TYPE.value] == node_type_to_int(src_type), :]
        relevant_view = relevant_view[relevant_view[:, qCols.DST_TYPE.value] == node_type_to_int(dst_type), :]
        relevant_view = relevant_view[relevant_view[:, qCols.SRC_ID.value] == src, :]
        relevant_view = relevant_view[relevant_view[:, qCols.DST_ID.value] == dst, :]
        orig_to_data[areaorig][link] = relevant_view[:, (qCols.TS.value, qCols.QLEN_BYTES.value, qCols.DEPARTURES_BYTES.value)]

    return orig_to_data

def touch_dcpim_file(path):
    rc = subprocess.call(f"touch {path}/this-is-a-dcpim-dir", shell=True)
    if rc < 0:
        raise Exception(f"touch {path}/this-is-a-dcpim-dir failed with error code {rc}")