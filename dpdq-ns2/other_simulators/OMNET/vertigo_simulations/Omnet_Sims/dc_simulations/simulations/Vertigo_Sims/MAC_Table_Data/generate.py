from pprint import pprint

def string_to_hex(mac_str):
    '''
    0A:AA:00:00:01:81 -> int
    '''
    clean = mac_str.replace(":", "")
    print(clean)
    return int(clean, 16)

def hex_to_string(mac_int):
    '''
    int -> 0A:AA:00:00:01:81
    '''
    mac_str = str(hex(mac_int)).split("0x")[1]
    if len(mac_str) != 12:
        mac_str = "0" + mac_str
    assert(len(mac_str) == 12)
    mac_str = mac_str.upper()
    ret = ""
    for i in range(0, len(mac_str), 2):
        ret = ret + mac_str[i:i+2] + ":"
    ret = ret[:-1]
    return ret

# I am still to underrstand how MAC addresses are allocated to servers...

# Ok, here is how to reverse engineer the base mac address for different topologies w/o understanding
# what is going on...

# Create a small topology with few servers.
# Then in BouncingIeee8021dRelay.cc, in BouncingIeee8021dRelay::handleAndDispatchFrame(Packet *packet)
# add a print before this line: throw cRuntimeError("1)Destination address not known. Broadcasting the frame. For DCs based on you're setting this shouldn't happen.");
# and print frame->getDest().
# The mac addrress printed is that of one of the servers (if there is one server, then this one's)
# THis is the base mac address..........
# This only works if the appropriate MAC .txt files are already generated. Once generated, run and the 
# based on the printed address, change BASE_MAC. I am not sure if the one printed is actually the correct base address. But it will fail if it is not.
# Keep trying the printed addresses until it works. At this point, it was probably worth understanding how this works...
# Then check that messages actually ggo to the right place..

def read():
    highest_mac = 0x0
    lowest_mac = 0xAAAAAAAAAAAAAAAAAAAAA
    with open("agg[3].txt") as f:
        lines = f.readlines()
        mac_to_if = {}
        for line in lines:
            elem = line.split("\t")
            mac = elem[1]
            max_hex = string_to_hex(mac)
            if (max_hex > highest_mac):
                highest_mac = max_hex
            if (max_hex < lowest_mac):
                lowest_mac = max_hex
            iface = elem[2].rstrip()
            print(f"{mac} -> {iface}")
            if mac not in mac_to_if:
                mac_to_if[mac] = []
            mac_to_if[mac].append(iface)
    pprint(mac_to_if)
    # print(BASE_MAC)
    # print(hex(BASE_MAC))
    # print(BASE_MAC+1)
    # print(hex(BASE_MAC+1))
    print(f"Lowest: {hex(lowest_mac)} Highest: {hex(highest_mac)}")


def generate_agg(name, num_hosts, num_agg, num_spines, switch_id):
    file_name = f"{name}[{switch_id}].txt"
    mac_offset = BASE_MAC
    with open(file_name, "w") as f:
        for current_target_agg in range(num_agg):
            if switch_id != current_target_agg:
                for host in range(num_hosts):
                    current_mac = mac_offset + host
                    for spine in range(num_spines):
                        f.write(f"0\t{hex_to_string(current_mac)}\t{BASE_IFACE+num_hosts+spine}\n")
            else:
                for host in range(num_hosts):
                    current_mac = mac_offset + host
                    f.write(f"0\t{hex_to_string(current_mac)}\t{BASE_IFACE+host}\n")
            current_mac += 1
            mac_offset = current_mac

def generate_spine(name, num_hosts, num_agg, num_spines, switch_id):
    file_name = f"{name}[{switch_id}].txt"
    mac_offset = BASE_MAC
    with open(file_name, "w") as f:
        for agg in range(num_agg):
            for host in range(num_hosts):
                current_mac = mac_offset + host
                f.write(f"0\t{hex_to_string(current_mac)}\t{BASE_IFACE+agg}\n")
            current_mac += 1
            mac_offset = current_mac

BASE_MAC = 0x0AAA000000A3 # For 1 Spines 9 agg 16 hosts
# BASE_MAC = 0x0AAA000000D9 # For 4 spines 9 agg 16 hosts
# BASE_MAC = 0x0AAA00000181 # For 4 spines 8 agg 40 hosts
BASE_IFACE = 100

def main():
    # string_to_hex("0A:AA:00:00:01:81")
    # hex_to_string(0xaaa00000181)
    # read()
    num_hosts_per_agg = 16
    num_agg = 9
    num_spines = 1
    name = f"agg_{num_spines}_{num_agg}_{num_hosts_per_agg}"
    for agg in range(num_agg):
        generate_agg(name, num_hosts_per_agg, num_agg, num_spines, agg)

    name = f"spine_{num_spines}_{num_agg}_{num_hosts_per_agg}"
    for spine in range(num_spines):
        generate_spine(name, num_hosts_per_agg, num_agg, num_spines, spine)

if __name__ == "__main__":
    main()
