import helper as hlp


def get_tor_to_hosts_dict(param_file):
    """
    Returns tor_addr -> [host_addr]
    """
    params = hlp.import_param_file(param_file)
    tor_addrs = params["tor_addr"]
    tor_to_hosts = {}
    for tor_addr in tor_addrs:
        tor_to_hosts[tor_addr] = params[f"tor_{tor_addr}"]
    return tor_to_hosts

def get_host_to_tor_dict(param_file):
    """
    Returns host_addr -> tor_addr
    """
    host_to_tor = {}
    tor_to_hosts = get_tor_to_hosts_dict(param_file)
    for tor in tor_to_hosts:
        for host in tor_to_hosts[tor]:
            host_to_tor[host] = tor
    return host_to_tor


def get_host_to_rack_neighbors_dict(param_file):
    """
    Returns host_addr -> [host_addr of hosts in the same rack]
    The list includes the key.
    """
    tor_to_hosts = get_tor_to_hosts_dict(param_file)
    host_to_tor = get_host_to_tor_dict(param_file)
    host_to_neighbors = {}
    for host in host_to_tor:
        tor = host_to_tor[host]
        host_to_neighbors[host] = tor_to_hosts[tor]
    return host_to_neighbors

def calc_transmission_latency(size_bytes, link_speed_gbps):
    """
    Returns the transmission latency in seconds
    """
    return ((size_bytes * 8.0) / (link_speed_gbps)) / 1000.0 / 1000.0 / 1000.0