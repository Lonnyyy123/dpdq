import numpy as np
import helper as hlp
import qmon_hlp
import csv
import re
import os
import subprocess

OMNET_Q_SAMPLE = "SAMPLED_QUEUE_LENGTH"
OMNET_BYTE_SAMPLE = "SAMPLED_DEPARTED_BYTES"
OMNET_REQUEST_SENT = "REQUEST_SENT"
OMNET_REQUEST_SIZE = "REQUEST_SIZE"
OMNET_REQUEST_SOURCE = "REQUEST_SOURCE"
OMNET_REQUEST_DURATION = "REQUEST_DURATION"
OMNET_SENDER_REQUEST_SIZE = "SENDER_REQUEST_SIZE"

def load_extracted_data(file, num_cols_expected):
    data = np.genfromtxt(file, delimiter=",", skip_header=True)
    if data.ndim == 1:
        data = data.reshape((1, num_cols_expected))
    return data

omnet_to_ns2_area = {"agg": "tor", "spine": "aggr", "server": "host"}


def calculate_goodput(data, ts_col, reqsz_col, num_hosts, trace_start, trace_end):
    bytes = np.sum(data[:, reqsz_col])
    dur = trace_end - trace_start
    gp_Gbps = np.round((bytes * 8.0 / 1000.0 / 1000.0 / 1000.0 / dur)/num_hosts, 4)
    return gp_Gbps


def get_app_data(src_dir, size_dir, dur_dir):
    files = hlp.get_files(src_dir)
    assert len(files) == 1
    src_file = f"{src_dir}/{files[0]}"

    files = hlp.get_files(size_dir)
    assert len(files) == 1
    size_file = f"{size_dir}/{files[0]}"

    files = hlp.get_files(dur_dir)
    assert len(files) == 1
    dur_file = f"{dur_dir}/{files[0]}"

    ifaces, num_aggr, num_tor, num_host_per_tor = get_header_info(src_file, type="server")
    # now that iface mapping is known, load up the data
    # The header appears to be in the same order for q len sampling too. Load up
    src_data = load_extracted_data(src_file, len(ifaces)*2)
    size_data = load_extracted_data(size_file, len(ifaces)*2)
    dur_data = load_extracted_data(dur_file, len(ifaces)*2)

    return ifaces, src_data, size_data, dur_data, num_aggr, num_tor, num_host_per_tor

# TODO: duplication
def get_sender_app_data(sendsize_dir):
    files = hlp.get_files(sendsize_dir)
    assert len(files) == 1
    sendsize_file = f"{sendsize_dir}/{files[0]}"

    ifaces, num_aggr, num_tor, num_host_per_tor = get_header_info(sendsize_file, type="server")

    return ifaces, load_extracted_data(sendsize_file, len(ifaces)*2), num_aggr, num_tor, num_host_per_tor

def get_qlen_and_byte_data(byte_dir, q_dir):
    files = hlp.get_files(byte_dir)
    assert len(files) == 1
    byte_file = f"{byte_dir}/{files[0]}"
    files = hlp.get_files(q_dir)
    assert len(files) == 1
    q_file = f"{q_dir}/{files[0]}"
    ifaces, num_aggr, num_tor, num_host_per_tor = get_header_info(byte_file)
    # now that iface mapping is known, load up the data
    # The header appears to be in the same order for q len sampling too. Load up
    byte_data = load_extracted_data(byte_file, len(ifaces)*2)
    q_data = load_extracted_data(q_file, len(ifaces)*2)

    return ifaces, q_data, byte_data, num_aggr, num_tor, num_host_per_tor

def calc_thrpt(data):
    '''
    Expecting an Nx2 ndarray (ts, byte departures)
    Returns momentary thrpt in gbps
    '''
    data = qmon_hlp.calc_thrpt(data)
    # Gbps
    data[:,1] = data[:,1]/1000/1000/1000
    return data

def is_intra_rack(local_addr, remote_addr, param_file, num_aggr, num_tor, num_host_per_tor):
    '''
    In omnet, host ids are predictable (currently). So tor0 will always have hosts 0 to num_hosts_per_tor-1 etc
    '''
    local_tor = int(int(local_addr) / num_host_per_tor)
    remote_tor = int(int(remote_addr) / num_host_per_tor)
    return local_tor == remote_tor

# TODO: meh, create dataclass at least
def get_qlen_and_throughput_single_origin(byte_data, q_data, idx, params, iface, num_aggr, num_tor, num_host_per_tor):
    stop = float(params["sim_dur"][0])
    byte_data_view = get_extracted_data_view(byte_data, idx, stop)
    q_data_view = get_extracted_data_view(q_data, idx, stop)

    origin_area = omnet_to_ns2_area[iface.split("[")[0]]
    origin = int(iface.split("[")[1].split("]")[0])
    iface_id = int(iface.split("eth[")[1].split("]")[0])
    throughput = calc_thrpt(byte_data_view)
    if np.sum(throughput[:,1]) < 0.000001:
        return None, None, None, None, None, None
    qing = q_data_view[1:,:]
    end, end_area = get_dst_and_dst_area_omnet(origin, origin_area, iface_id, num_aggr, num_tor, num_host_per_tor)
    return origin, origin_area, end, end_area, throughput, qing

def get_app_data_single_origin(src_data, size_data, dur_data, idx, params, iface):
    stop = float(params["sim_dur"][0])
    src_data_view = get_extracted_data_view(src_data, idx, stop)
    size_data_view = get_extracted_data_view(size_data, idx, stop)
    dur_data_view = get_extracted_data_view(dur_data, idx, stop)

    origin = int(iface.split("[")[1].split("]")[0])

    data = size_data_view
    data = np.append(data, src_data_view[:,1].reshape(src_data_view.shape[0], 1), axis=1)
    local = np.ones((data.shape[0], 1)) * origin
    data = np.append(data, local.reshape(local.shape[0], 1), axis=1)
    data = np.append(data, dur_data_view[:,1].reshape(dur_data_view.shape[0], 1), axis=1)

    return origin, data

def get_sender_app_data_single_origin(sendsize_data, idx, params, iface):
    stop = float(params["sim_dur"][0])
    sendsize_data_view = get_extracted_data_view(sendsize_data, idx, stop)
    origin = int(iface.split("[")[1].split("]")[0])
    return origin, sendsize_data_view

def get_iface(row1, idx, type):
    if type == "queue":
        return (re.split('Base[^.]*\.', row1[idx])[1]).split('.mac')[0]
    elif type == "server":
        return (re.split('Base[^.]*\.', row1[idx])[1]).split(' (')[0]
    else:
        raise ValueError(f"{type} not supported by get_iface()")


def get_header_info(file, type="queue"):
    ifaces = []
    num_aggr = 0
    num_tor = 0
    num_host_per_tor = 0
    with open(file, 'rt') as csv_file:
        csv_reader = csv.reader(csv_file, delimiter=',')
        for row in csv_reader: # just to get first row
            row1 = row # row1 is a list with one item per network interface (actually item by item)
            for k in range(0, len(row), 2):
                # byteDepartures:vector Base.agg[0].eth[1].mac.queue ($ttl=250, $closeInsteadOfWait=true, $FRsDisabled=false, $usingECMP=true, $usingPowerOfNLB=false, $aggRandomPowerFactor=2, $spineRandomPowerFactor=2, $ecnWill=true, $shouldUseV2Marking=false, $markingType="SRPT", $markingTimer=0.00120, $hasOrderingLayer=false, $orderingTimer=0.00120, $numSpines=4, $numAggs=9, $numServers=16, $numBurstyApps=0, $numReqPerBurst=40, $numMiceBackgroundFlowAppsInEachServer=1, $numElephantBackgroundFlowAppsInEachServer=0, $incastFlowSize=20000, $serverApplicationCategory=index() < 1 + 1 ? "cache" : "web", $bgInterArrivalMultiplier=19.75, $bgFlowSizeMultiplier=1, $burstyInterArrivalMultiplier=0.5, $burstyFlowSizeMultiplier=1, $aggQueueType="V2PIFO", $aggUseV2Pifo=true, $aggDropperType="FIFO", $aggSchedulerType="FIFO", $aggQueueSizeDCTCPThresh=20, $spineQueueType="V2PIFO", $spineUseV2Pifo=true, $spineDropperType="FIFO", $spineSchedulerType="FIFO", $spineQueueSizeDCTCPThresh=20, $aggMacTypeName="AugmentedEtherMac", $spineMacTypeName="AugmentedEtherMac", $aggRelayTypeName="BouncingIeee8021dRelay", $spineRelayTypeName="BouncingIeee8021dRelay", $aggBounceRandomly=false, $spineBounceRandomly=false, $aggBounceRandomlyPowerOfN=false, $spineBounceRandomlyPowerOfN=false, $aggBounceRandomlyPowerFactor=2, $spineBounceRandomlyPowerFactor=2, $aggDenomRet=2, $spineDenomRet=2, $server0NumApps=1 + 1 + 0, $server0IsBursty=index() < 1 + 1 ? false : true, $serverIsMiceBackground=index() < 1 + 1 ? true : false, #0 - DCTCP_ECMP-0-20230131-20:19:10-601133)
                iface = get_iface(row1, k, type)
                # $numSpines=4, $numAggs=9, $numServers=16,
                num_aggr = int(row1[k].split('numSpines=')[1].split(",")[0])
                num_tor = int(row1[k].split('numAggs=')[1].split(",")[0])
                num_host_per_tor = int(row1[k].split('numServers=')[1].split(",")[0])
                ifaces.append(iface)

            break
    return ifaces, num_aggr, num_tor, num_host_per_tor

def get_extracted_data_view(data, colum_offset, last_timestamp):
    '''
    data is meant to be structured as follows:
    the timeseries for each simulation object occupies 2 columns of data.
    The first col is timestamp data, the other is value data.
    '''
    ret = data[:,(colum_offset, colum_offset+1)]
    ret[:, 0] = ret[:, 0] - 10.0
    return hlp.trim_by_timestamp(ret, 0, last_timestamp, 0)

def get_dst_and_dst_area_omnet(origin: int, origin_area: str, iface_id: int, num_aggr: int, num_tor: int, num_host_per_tor: int):
    '''
    Expect OMNET indexes
    '''
    ret_area = ""
    ret_dst = ""
    if origin_area == "tor":
        if iface_id < num_host_per_tor:
            ret_area = "host"
            # Temporary -> ideally, we want logical address, not physical
            ret_dst = origin * num_host_per_tor + iface_id
        else:
            ret_area = "aggr"
            # Temporary -> ideally, we want logical address, not physical
            ret_dst = iface_id
        assert iface_id < (num_host_per_tor + num_aggr) # 19<20
    elif origin_area == "aggr":
        ret_area = "tor"
        ret_dst = iface_id
        assert iface_id < num_tor, f"iface_id {iface_id} num_aggr {num_aggr}" # 8 < 9
    elif origin_area == "host":
        ret_area = "tor"
        ret_dst = int(iface_id / num_host_per_tor)
    else:
        raise RuntimeError(f"origin area {origin_area} not supported")
    
    return ret_dst, ret_area


# TODO: if slow parallelize the extraction process (it's ok)
def extract_omnet_results(prots, gbps_per_host_l, data_path):
    base_dir = os.getcwd()
    for prot in prots:
        for load in gbps_per_host_l:
            path = f"{data_path}/{prot}/{load}"
            if os.path.exists(f"{path}/this-is-an-omnet-dir"):
                os.chdir(path)

                rc = subprocess.call("bash dir_creator.sh", shell=True)
                if rc < 0:
                    raise Exception(f"dir_creator.sh failed with error code {rc}")
                rc = subprocess.call("python3 extractor_shell_creator.py DCTCP_ECMP", shell=True)
                if rc < 0:
                    raise Exception(f"extractor_shell_creator failed with error code {rc}")
                rc = subprocess.call("bash extractor.sh", shell=True)
                if rc < 0:
                    raise Exception(f"extractor_shell_creator failed with error code {rc}")
                os.chdir(base_dir)

def touch_omnet_file(path):
    rc = subprocess.call(f"touch {path}/this-is-an-omnet-dir", shell=True)
    if rc < 0:
        raise Exception(f"touch {path}/this-is-an-omnet-dir failed with error code {rc}")