import yaml
import sys
import common

'''
This script parses yaml files from topologies/ and converst the topology to a tcl parameter file 
which is fed to the main tcl simulation script.
Specifically, this script appends topolgy related tcl parameters to the provided output file (see directory "parameter_files").
'''

valid_keys = {"num_hosts", "num_tors", "num_aggr", "num_spines",
            "host_placement", "hosts", "leaf_link_speed_gbps",
            "core_link_speed_gbps",
            "core_link_latency_ms", "leaf_link_latency_ms"}


def write_tcl_params(file, topo):
    with open(file, "a") as open_file:
        for key in topo:
            print(f"{key} -> {topo[key]}")
            # TODO: use pattern matching (version 3.10)
            # TODO: add check that hosts are either server or client or both
            if key == "hosts":
                num_client_apps = 0
                num_server_apps = 0
                clients = []
                servers = []
                tor_to_hosts = {}
                tor_to_clients = {}
                tor_to_servers = {}
                hosts = topo[key]
                # Optional per-host protocol overrides. Only hosts that specify
                # a "transport" key will be added to this dict, the rest will
                # fall back to the global transport_protocol at runtime.
                host_transport = {}
                to_write = f"set hosts [dict create "
                for host in hosts:
                    to_write += f"{host} [dict create"
                    attributes = hosts[host]
                    for attribute in attributes:
                        if attribute == "client" and attributes[attribute]:
                            clients.append(host)
                            if this_tor not in tor_to_clients:
                                tor_to_clients[this_tor] = []
                            tor_to_clients[this_tor].append(host)
                        if attribute == "server" and attributes[attribute]:
                            servers.append(host)
                            if this_tor not in tor_to_servers:
                                tor_to_servers[this_tor] = []
                            tor_to_servers[this_tor].append(host)
                        if attribute == "tor":
                            this_tor = attributes[attribute]
                            if this_tor not in tor_to_hosts:
                                tor_to_hosts[this_tor] = []
                            tor_to_hosts[this_tor].append(host)
                        if attribute == "transport":
                            host_transport[host] = attributes[attribute]
                        to_write += f" {attribute} {attributes[attribute]}"
                    to_write += "] "
                to_write += "]"
                open_file.write(f"{to_write}\n")
                if host_transport:
                    to_write = "set host_transport [dict create "
                    for host, transport in host_transport.items():
                        to_write += f"{host} {transport} "
                    to_write += "]"
                    open_file.write(f"{to_write}\n")
                    # Keep a helper list for validation in the simulator
                    unique_transports = sorted(list(set(host_transport.values())))
                    open_file.write(common.set_tcl_list("host_transport_used", unique_transports))
                for tor in tor_to_hosts:
                    open_file.write(common.set_tcl_list(f"tor_{tor}_hosts", tor_to_hosts[tor]))
                    if tor in tor_to_clients:
                        open_file.write(common.set_tcl_list(f"tor_{tor}_clients", tor_to_clients[tor]))
                    if tor in tor_to_servers:
                        open_file.write(common.set_tcl_list(f"tor_{tor}_servers", tor_to_servers[tor]))
                open_file.write(common.set_simple_tcl_var("num_client_apps", len(clients)))
                open_file.write(common.set_simple_tcl_var("num_server_apps", len(servers)))
                open_file.write(common.set_tcl_list("client_apps", clients))
                open_file.write(common.set_tcl_list("server_apps", servers))
            else:
                open_file.write(common.set_simple_tcl_var(key, topo[key]))

def check_valid_hosts(topo):
    assert topo["num_hosts"] == len(topo["hosts"]), f"mismatch between num_hosts and defined hosts ({topo['num_hosts']} vs {len(topo['hosts'])})"
    hosts = topo["hosts"]
    for host in hosts:
        assert hosts[host]["tor"] < topo["num_tors"], f"host {host} placed in tor ({hosts[host]['tor']}) that does not exist"

def keys_are_valid(topo):
    for key in topo:
        if key not in valid_keys:
            print(f"Invalid key: {key}")
            return False
    return True

def merge_topos(old, new):
    '''
    old and new must be topology dictionaries (yaml imports)
    '''
    for new_key in new:
        old[new_key] = new[new_key]
    return old

def usage():
    print("Usage: topology_parser.py <topology file> <output file>")

# TODO: check values for validity
def main():
    if len(sys.argv) != 3:
        usage()
        exit(1)
    
    topo_file = sys.argv[1]
    out_file = sys.argv[2]
    # Read default topo first
    with open("./config/topologies/default.yaml") as file:
        default_topo = yaml.load(file, Loader=yaml.FullLoader)
    if not keys_are_valid(default_topo):
        exit(1)


    # Read actual topology and substitute
    with open(topo_file) as file:
        topo = yaml.load(file, Loader=yaml.FullLoader)
    if not keys_are_valid(topo):
        exit(1)

    topo = merge_topos(default_topo, topo)
    # print("TOPOLOGY:")
    # print(topo)
    if topo["host_placement"] == "manual":
        check_valid_hosts(topo)
    write_tcl_params(out_file, topo)



if __name__ == "__main__":
    main()
