# These are not precise and must be updated. See r2p2.h
gparams = {
    "packet_size" : 1500,
    "headers_size" : 40,
    "incast_size" : 500000,
    "incast_size_dcpim" : 500000,
    "incast_size_dcpim_websearch" : 499320,
}

NS2_SIMULATOR = "ns-2"
OMNET_SIMULATOR = "OMNET++"
DCPIM_SIMULATOR = "dcPIM"

DCPIM_APP_FILE = "data.txt"
DCPIM_Q_FILE = DCPIM_APP_FILE

APP_FILE = "applications_trace.str"
PARAM_FILE = "parameters"
OUT_DIR_NAME = "output"
BDP = 95000
PKT = 1450

SIZE_GROUPS = [(0,PKT),
               (0,BDP),
               (0,2*BDP),
               (0,4*BDP),
               (0,99999999999),
               (PKT,BDP),
               (PKT,2*BDP),
               (PKT,4*BDP),
               (PKT,99999999999),
               (BDP,2*BDP),
               (BDP,4*BDP),
               (BDP,8*BDP),
               (BDP,16*BDP),
               (BDP,99999999999),
               (2*BDP,4*BDP),
               (2*BDP,8*BDP),
               (2*BDP,16*BDP),
               (2*BDP,99999999999),
               (4*BDP,8*BDP),
               (4*BDP,16*BDP),
               (4*BDP,99999999999),
               (8*BDP,16*BDP),
               (8*BDP,99999999999),
               (16*BDP,32*BDP),
               (16*BDP, 99999999999),
               ]