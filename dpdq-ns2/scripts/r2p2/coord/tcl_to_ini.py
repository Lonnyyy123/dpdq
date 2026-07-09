import sys
from pprint import pprint
import common
from dataclasses import dataclass
from typing import List, Set, Dict, Tuple
import re

'''
This script takes a final parameter file (the one imported by simulation.tcl) and produces a .ini
file to be used by OMNET. This ini file does not have any paramater ranges in it.
'''
def usage():
    print("Usage: tcl_to_ini.py <parameter file> <output file>")

@dataclass
class IniParam:
    name: str
    var_type: type
    unit: str
    section: str
    prefix: str
    affix: str
    value: str
    offset: float
    no_quotes: bool
    set_var: str

    def __init__(self, name, var_type, unit, section, prefix, affix="", value="", offset=0.0, no_quotes=False, set_var=""):
        self.name = name
        self.var_type = var_type
        self.unit = unit
        self.section = section
        self.prefix = prefix
        self.affix = affix # hax
        self.value = value
        self.offset = offset
        self.no_quotes = no_quotes
        self.set_var = set_var

@dataclass
class TclParam:
    name: str
    value: list
    isList: bool

    def __init__(self, name, value, isList):
        self.name = name
        self.value = value
        self.isList = isList

def import_param_file(param_file):
    '''
    Parses TCL script and returns a dict where keys are param names and 
    values are param values (can be a list). Ignores TCL dictionaries.
    '''
    res = dict()
    with open(param_file) as f:
        lines = f.readlines()
        for line in lines:
            ln = line.split()
            if ln[0] != "set":
                continue
            if "dict" in ln:
                continue
            if "{" in ln:
                # List
                name = ln[1]
                vals = [ln[x] for x in range(3, len(ln)-1)]

                res[name] = TclParam(name, vals, True)
            else:
                # scalar
                name = ln[1]
                val = [ln[2].strip('\"')]
                res[name] = TclParam(name, val, False)
            # line_items = line.split()
            # res[line_items[0]] = line_items[1:]
    return res


# Sections
GENERAL_SECTION="General"
DCTCP_SECTION="Config DCTCP_ECMP"
SWIFT_SECTION="Config Swift_ECMP"

# Class prefixes
PREFIX_NONE=""
PREFIX_ALL="**."
PREFIX_APP="**.server[*].app[1..]."
PREFIX_MAC_AGG="**.agg[*].macTable."
PREFIX_MAC_SPINE="**.spine[*].macTable."
PREFIX_MAC_QUEUE_AGG="**.agg[*].eth[*].mac.queue."
PREFIX_MAC_QUEUE_SPINE="**.spine[*].eth[*].mac.queue."
PREFIX_ALL_QUEUES="**.**.eth[*].mac.queue."
PREFIX_TCP="**.tcp."

# Hardwired
# TOPO_DESCRIPTOR="_4_8_40"
TOPO_DESCRIPTOR="_${numSpines}_9_16" # The hax have peaked..
# NUM_SPINE=int(TOPO_DESCRIPTOR.split("_")[1])
NUM_TOR=int(TOPO_DESCRIPTOR.split("_")[2])
HOSTS_PER_TOR=int(TOPO_DESCRIPTOR.split("_")[3])

# parameters to map
ptm = {
    "simul_termination": [IniParam("sim-time-limit", float, "s", GENERAL_SECTION, PREFIX_NONE)],
    "start_at": [IniParam("startTime", float, "s", GENERAL_SECTION, PREFIX_APP)],
    "stop_at": [IniParam("stopTime", float, "s", GENERAL_SECTION, PREFIX_APP)],
    "dctcp_K": [IniParam("dctcp_thresh", int, "", DCTCP_SECTION, PREFIX_MAC_QUEUE_AGG),
                IniParam("dctcp_thresh", int, "", DCTCP_SECTION, PREFIX_MAC_QUEUE_SPINE)],
    "start_tracing_at": [IniParam("warmup-period", float, "s", GENERAL_SECTION, PREFIX_NONE),
                         IniParam("warmup_period", float, "s", GENERAL_SECTION, PREFIX_ALL_QUEUES)],
    "dctcp_init_cwnd": [IniParam("customIWMult", int, "", GENERAL_SECTION, PREFIX_TCP)],
    "tcp_connections_per_thread_pair": [IniParam("tcp_connections_per_thread_pair", int, "", GENERAL_SECTION, PREFIX_APP)],
    "state_polling_ival_s": [IniParam("queue_sample_interval_s", float, "s", GENERAL_SECTION, PREFIX_ALL_QUEUES)],
    "general_queue_size_bytes": [IniParam("dataCapacity", int, "B", GENERAL_SECTION, PREFIX_MAC_QUEUE_AGG),
                           IniParam("dataCapacity", int, "B", GENERAL_SECTION, PREFIX_MAC_QUEUE_SPINE)],
    "mean_req_size_B": [IniParam("request_size_B", int, "B", GENERAL_SECTION, PREFIX_APP)],
    "mean_resp_size_B": [IniParam("response_size_B", int, "B", GENERAL_SECTION, PREFIX_APP)],
    "mean_per_client_req_interval_s": [IniParam("request_interval_sec", float, "s", GENERAL_SECTION, PREFIX_APP)],
    "incast_size": [IniParam("incast_size", int, "", GENERAL_SECTION, PREFIX_APP)],
    "mean_incast_interval_s": [IniParam("incast_interval_sec", float, "s", GENERAL_SECTION, PREFIX_APP)],
    "incast_request_size_bytes" : [IniParam("incast_request_size_bytes", int, "B", GENERAL_SECTION, PREFIX_APP)],
    "enable_incast" : [IniParam("enable_incast", int, "", GENERAL_SECTION, PREFIX_APP)],
    "req_interval_distr": [IniParam("req_interval_distr", str, "", GENERAL_SECTION, PREFIX_APP)],
    "req_target_distr": [IniParam("req_target_distr", str, "", GENERAL_SECTION, PREFIX_APP)],
    "manual_req_interval_file": [IniParam("manual_req_interval_file", str, "", GENERAL_SECTION, PREFIX_APP)],
    "req_size_distr": [IniParam("req_size_distr", str, "", GENERAL_SECTION, PREFIX_APP)],
    "resp_size_distr": [IniParam("resp_size_distr", str, "", GENERAL_SECTION, PREFIX_APP)],
    "num_client_apps": [IniParam("num_client_apps", int, "", GENERAL_SECTION, PREFIX_APP)],
    "num_server_apps": [IniParam("num_server_apps", int, "", GENERAL_SECTION, PREFIX_APP)],
    "results_path": [IniParam("cmdenv-output-file", str, "", GENERAL_SECTION, PREFIX_NONE, "/${numSpines}_spines_${numAggs}_aggs_${numServers}_servers_${numBurstyApps}_burstyapps_${numMiceBackgroundFlowAppsInEachServer}_mice_${numReqPerBurst}_reqPerBurst_${bgInterArrivalMultiplier}_bgintermult_${bgFlowSizeMultiplier}_bgfsizemult_${burstyInterArrivalMultiplier}_burstyintermult_${burstyFlowSizeMultiplier}_burstyfsizemult_${ttl}_ttl_${repetition}_rep_${aggRandomPowerFactor}_rndfwfactor_${aggBounceRandomlyPowerFactor}_rndbouncefactor_${incastFlowSize}_incastfsize_${markingTimer}_mrktimer_${orderingTimer}_ordtimer.out"),
                     IniParam("output-scalar-file", str, "", GENERAL_SECTION, PREFIX_NONE, "/${numSpines}_spines_${numAggs}_aggs_${numServers}_servers_${numBurstyApps}_burstyapps_${numMiceBackgroundFlowAppsInEachServer}_mice_${numReqPerBurst}_reqPerBurst_${bgInterArrivalMultiplier}_bgintermult_${bgFlowSizeMultiplier}_bgfsizemult_${burstyInterArrivalMultiplier}_burstyintermult_${burstyFlowSizeMultiplier}_burstyfsizemult_${ttl}_ttl_${repetition}_rep_${aggRandomPowerFactor}_rndfwfactor_${aggBounceRandomlyPowerFactor}_rndbouncefactor_${incastFlowSize}_incastfsize_${markingTimer}_mrktimer_${orderingTimer}_ordtimer.sca"),
                     IniParam("output-vector-file", str, "", GENERAL_SECTION, PREFIX_NONE, "/${numSpines}_spines_${numAggs}_aggs_${numServers}_servers_${numBurstyApps}_burstyapps_${numMiceBackgroundFlowAppsInEachServer}_mice_${numReqPerBurst}_reqPerBurst_${bgInterArrivalMultiplier}_bgintermult_${bgFlowSizeMultiplier}_bgfsizemult_${burstyInterArrivalMultiplier}_burstyintermult_${burstyFlowSizeMultiplier}_burstyfsizemult_${ttl}_ttl_${repetition}_rep_${aggRandomPowerFactor}_rndfwfactor_${aggBounceRandomlyPowerFactor}_rndbouncefactor_${incastFlowSize}_incastfsize_${markingTimer}_mrktimer_${orderingTimer}_ordtimer.vec"),
                     IniParam("eventlog-file", str, "", GENERAL_SECTION, PREFIX_NONE, "/${numSpines}_spines_${numAggs}_aggs_${numServers}_servers_${numBurstyApps}_burstyapps_${numMiceBackgroundFlowAppsInEachServer}_mice_${numReqPerBurst}_reqPerBurst_${bgInterArrivalMultiplier}_bgintermult_${bgFlowSizeMultiplier}_bgfsizemult_${burstyInterArrivalMultiplier}_burstyintermult_${burstyFlowSizeMultiplier}_burstyfsizemult_${ttl}_ttl_${repetition}_rep_${aggRandomPowerFactor}_rndfwfactor_${aggBounceRandomlyPowerFactor}_rndbouncefactor_${incastFlowSize}_incastfsize_${markingTimer}_mrktimer_${orderingTimer}_ordtimer.elog"),
                     IniParam("snapshot-file", str, "", GENERAL_SECTION, PREFIX_NONE, "/${numSpines}_spines_${numAggs}_aggs_${numServers}_servers_${numBurstyApps}_burstyapps_${numMiceBackgroundFlowAppsInEachServer}_mice_${numReqPerBurst}_reqPerBurst_${bgInterArrivalMultiplier}_bgintermult_${bgFlowSizeMultiplier}_bgfsizemult_${burstyInterArrivalMultiplier}_burstyintermult_${burstyFlowSizeMultiplier}_burstyfsizemult_${ttl}_ttl_${repetition}_rep_${aggRandomPowerFactor}_rndfwfactor_${aggBounceRandomlyPowerFactor}_rndbouncefactor_${incastFlowSize}_incastfsize_${markingTimer}_mrktimer_${orderingTimer}_ordtimer.sna"),
                    ],
    "vertigo_mac_files": [IniParam("addressTableFile", str, "", GENERAL_SECTION, PREFIX_MAC_AGG, f"/agg{TOPO_DESCRIPTOR}[\" + string(parentIndex()) + \"].txt"),
                          IniParam("addressTableFile", str, "", GENERAL_SECTION, PREFIX_MAC_SPINE, f"/spine{TOPO_DESCRIPTOR}[\" + string(parentIndex()) + \"].txt")],
    "num_aggr": [IniParam("num_spines", int, "", SWIFT_SECTION, PREFIX_ALL, set_var="${numSpines = "), IniParam("num_spines", int, "", DCTCP_SECTION, PREFIX_ALL, set_var="${numSpines = ")],
    # Swift
    "swift_base_target_delay": [IniParam("base_target_delay", float, "", SWIFT_SECTION, PREFIX_TCP)],
    "swift_max_scaling_range": [IniParam("max_scaling_range", float, "", SWIFT_SECTION, PREFIX_TCP)],
    "swift_per_hop_scaling_factor": [IniParam("per_hop_scaling_factor", float, "", SWIFT_SECTION, PREFIX_TCP)],
    "swift_max_cwnd_target_scaling": [IniParam("max_cwnd_target_scaling", float, "", SWIFT_SECTION, PREFIX_TCP)],
    "swift_min_cwnd_target_scaling": [IniParam("min_cwnd_target_scaling", float, "", SWIFT_SECTION, PREFIX_TCP)],
    "swift_additive_increase_constant": [IniParam("additive_increase_constant", float, "", SWIFT_SECTION, PREFIX_TCP)],
    "swift_multiplicative_decrease_constant": [IniParam("multiplicative_decrease_constant", float, "", SWIFT_SECTION, PREFIX_TCP)],
    "swift_max_mdf": [IniParam("max_mdf", float, "", SWIFT_SECTION, PREFIX_TCP)],
}

# -------------------------------------------------------------------------------------------

def derive_host_index(host_count: int, tor: int):
    actual_idx = HOSTS_PER_TOR*tor + host_count
    assert actual_idx < NUM_TOR * HOSTS_PER_TOR # 143 < 144
    return actual_idx


def set_active_clients(tclParam: TclParam):
    # set tor_1_clients { 2 3 }
    tor = tclParam.name.split("_")[1]
    res = []
    hosts_placed = 0
    for client_ind in tclParam.value:
        actual_client_loc = derive_host_index(hosts_placed, int(tor))
        prefix = f"**.server[{actual_client_loc}].app[1..]."
        res.append(IniParam("activeClient", bool, "", DCTCP_SECTION, prefix, value="true")) # this cannot be in general section for some reason
        res.append(IniParam("activeClient", bool, "", SWIFT_SECTION, prefix, value="true")) # this cannot be in general section for some reason
        hosts_placed += 1

        # add another param that maps the physical index of the client to the logical one
        res.append(IniParam("logicalIdx", int, "", DCTCP_SECTION, prefix, value=client_ind))
        res.append(IniParam("logicalIdx", int, "", SWIFT_SECTION, prefix, value=client_ind))
    return res

current_target_string = "" # hax

def set_targets(tclParam: TclParam):
    global current_target_string
    tor = tclParam.name.split("_")[1]
    prefix = "**.server[*].app[1..]."
    hosts_placed = 0
    for server_ind in tclParam.value:
        actual_server_loc = derive_host_index(hosts_placed, int(tor))
        if current_target_string == "":
            current_target_string+=f"{actual_server_loc}"
        else:
            current_target_string+=f"_{actual_server_loc}"
        hosts_placed += 1

    return [IniParam("targetServers", str, "", GENERAL_SECTION, prefix, value=current_target_string)]

def set_network(tclParam: TclParam):
    topo="-"
    if tclParam.value[0] == '0':
        topo = "Base"
    elif tclParam.value[0] == "1":
        topo = "Base21"
    else:
        raise ValueError(f"Unsupported core link rate of {tclParam.value[0]}")
    return IniParam("network", str, "", GENERAL_SECTION, PREFIX_NONE, value=topo, no_quotes=True)

# Special parameters
# set client_apps { 0 1 2 }
# set server_apps { 0 1 2 }
# set tor_0_clients { 0 1 }
# set tor_1_clients { 2 }
# set tor_2_clients { 3 } 
sptm = {
    "tor_[0-9]+_clients": set_active_clients,
    "tor_[0-9]+_servers": set_targets,
    "oversub_topo": set_network,
}

# -------------------------------------------------------------------------------------------

def map_param(name: str, tclParam: TclParam) -> Tuple[IniParam, str]:
    '''
    Returns the .ini string for the tcl param input.
    Uses a dictionary and only maps parameters that are defined there 
    (for incremental support of this translation and because this is fairly manual..)
    '''
    ret_name = [] # IniParam objects
    ret_text = [] # Ini file formatted string
    if (not tclParam.isList):
        # Not a list
        if name in ptm:
            for ini_param in ptm[name]:
                ret_name.append(ini_param)
                ret_text.append(def_ini_param(ini_param, tclParam.value[0]))
            return ret_name, ret_text
        for pattern in sptm: # will match first
            if re.search(pattern, name):
                fun = sptm[pattern]
                ret = fun(tclParam)
                ret_name.append(ret)
                ret_text.append(def_ini_param(ret, ret.value))
                return ret_name, ret_text
    else:
        # list
        for pattern in sptm: # will match first
            if re.search(pattern, name):
                fun = sptm[pattern]
                ret = fun(tclParam)
                for ini_param in ret:
                    ret_name.append(ini_param)
                    ret_text.append(def_ini_param(ini_param, ini_param.value))
                return ret_name, ret_text
    return None, None

def def_ini_param(ini_param: IniParam, value: str):
    if ini_param.value != "":
        if not(type(ini_param.value) is str):
            raise Exception(f"value given ({ini_param.value}) must be string but is {type(value)}")
        value = ini_param.value
    quotes_in_str_val = (ini_param.affix == "")

    close_brace = ""
    if ini_param.set_var != "":
        close_brace = "}"
    if quotes_in_str_val: # super hax..
            
        if ini_param.no_quotes:
            return f"{ini_param.prefix}{ini_param.name} = {ini_param.set_var}{common.from_string(value, ini_param.var_type, False, offset=ini_param.offset)}{ini_param.affix}{ini_param.unit}{close_brace}"
        else:
            return f"{ini_param.prefix}{ini_param.name} = {ini_param.set_var}{common.from_string(value, ini_param.var_type, quotes_in_str_val, offset=ini_param.offset)}{ini_param.affix}{ini_param.unit}{close_brace}"
    else:
        if ini_param.no_quotes:
            return f"{ini_param.prefix}{ini_param.name} = {ini_param.set_var}{common.from_string(value, ini_param.var_type, False, offset=ini_param.offset)}{ini_param.affix}{ini_param.unit}{close_brace}"
        else:
            return f"{ini_param.prefix}{ini_param.name} = \"{ini_param.set_var}{common.from_string(value, ini_param.var_type, quotes_in_str_val, offset=ini_param.offset)}{ini_param.affix}{ini_param.unit}{close_brace}\""
        

def write_ini_file(out_file, ini_params):
    with open(out_file, "w") as f:
        
        for section in ini_params:
            # Write section
            f.write(f"\n[{section}]\n")
            # Write parameters
            for ini_param_text in ini_params[section]:
                f.write(f"{ini_param_text}\n")

        # File must include the base .ini file - added last so that previous params take precedence
        f.write(f"\ninclude ../base.ini\n")

# hack
def remove_duplicates(ini_params: list, new_param: str):
    for existing_param in ini_params:
        if "targetServers" in existing_param:
            existing_prefix = existing_param.split(" = ")[0]
            new_prefix = new_param.split(" = ")[0]
            if existing_prefix == new_prefix:
                # remove existing prefix
                ini_params.remove(existing_param)
                break


def main():
    if len(sys.argv) != 3:
        usage()
        exit(1)
    
    param_file = sys.argv[1] # tcl parameter file input
    tcl_params = import_param_file(param_file) # 'key' -> ['value']
    # experiment_id_prefix = tcl_params["experiment_id_prefix"][0]#.split(".sh")[0]
    experiment_id = tcl_params["experiment_id"].value[0]
    print(f"EXPERIMENT ID {experiment_id}")
    out_file_dir = f"{sys.argv[2]}"
    print(f"Creating directory: {out_file_dir}")
    common.make_dir(out_file_dir)
    out_file_name = f"{experiment_id}.ini"
    out_file_path = f"{out_file_dir}/{out_file_name}"
    ini_params = {} # section -> [param text]
    for name, tclParam in tcl_params.items():
        param_data, param_text = map_param(name,tclParam) # lists
        if param_text:
            for ini_obj, ini_text in zip(param_data, param_text):
                if ini_obj.section not in ini_params:
                    ini_params[ini_obj.section] = []
                remove_duplicates(ini_params[ini_obj.section], ini_text)
                ini_params[ini_obj.section].append(ini_text)
    write_ini_file(out_file_path, ini_params)




if __name__ == "__main__":
    main()
