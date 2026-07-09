import helper as hlp
import numpy as np

qmon_cols = {
    "timestamp": 0,
    "from": 1,
    "to": 2,
    "q_sz_bytes": 3,
    "q_sz_pkts": 4,
    "pkt_arrivals": 5,  # all below are cumulative
    "pkt_departures": 6,
    "pkt_drops": 7,
    "byte_arrivals": 8,
    "byte_departures": 9,
    "byte_drops": 10,
    "tcp_gbps": 11,
    "r2p2_gbps": 12,
    "grant_req_gbps": 13,
    "grant_gbps": 14,
    "reply_gbps": 15,
    "scheduled_data_gbps": 16,
    "unscheduled_gbps": 17
}

at = "aggr_tor"
ht = "host_tor"
ta = "tor_aggr"
th = "tor_host"
tt = "tor_tor"
nw_areas = [at, ht, ta, th, tt]

nwarea_to_param_name_origin = {
    at : "aggr_addr",
    ht : "host_addr",
    ta : "tor_addr",
    th : "tor_addr",
    tt : "tor_addr"
}

nwarea_to_param_name_destination = {
    at : "tor_addr",
    ht : "tor_addr",
    ta : "aggr_addr",
    th : "host_addr",
    tt : "tor_addr"
}

nwarea_to_origin_type = {
    at : "aggr",
    ht : "host",
    ta : "tor",
    th : "tor",
    tt : "tor"
}

def load_qmon_data(path):
    data = hlp.load_data(path)
    data[:, qmon_cols["timestamp"]] = data[:, qmon_cols["timestamp"]] - 10
    return data

def load_and_trim_qmon_data(path, last_timestamp):
    return hlp.trim_by_timestamp(load_qmon_data(path), 0, last_timestamp, qmon_cols["timestamp"])

def calc_thrpt(data):
    '''
    Returns momentary throughput timeseries (timestamp, thrpt_value)
    Expects a Nx2 ndarray (timestamps and byte departures)
    '''
    thrpt = np.zeros((data.shape[0]-1, 2))
    for i in range(1, data.shape[0]):
        thrpt[i-1, 0] = data[i, 0]
        thrpt[i-1, 1] = (data[i, 1] - data[i-1, 1]) * 8 / (data[i, 0] - data[i-1, 0])
    return thrpt

