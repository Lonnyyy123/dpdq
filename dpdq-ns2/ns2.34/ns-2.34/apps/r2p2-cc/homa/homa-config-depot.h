/* Copyright (c) 2015-2016 Stanford University
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR(S) DISCLAIM ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL AUTHORS BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef ns_homa_config_depot_h
#define ns_homa_config_depot_h

#include "homa-transport.h"
#include <vector>
/**
 * @brief This class is taken almost as-is from https://github.com/PlatformLab/HomaSimulation/tree/omnet_simulations/RpcTransportDesign/OMNeT%2B%2BSimulation
 */

class HomaTransport;

// This class is the ultimate place for storing all config parameters. All other
// classes will get a pointer to this class and can read params from this
class HomaConfigDepot
{
public:
    HomaConfigDepot(HomaTransport *ownerTransport);
    ~HomaConfigDepot() {}

    /**
     * Corresponding enum values for SenderScheme config param.
     */
    enum SenderScheme
    {
        OBSERVE_PKT_PRIOS = 0, // Highest prio homa pkt first
        SRBF,                  // Pkt of shortest remaining mesg first
        UNDEFINED              // never used in original code
    };

public:
    HomaTransport *ownerTransport;

    // NIC link speed (in Gb/s) connected to this host.
    int nicLinkSpeed;

    // RTT is computed as travel time of a full ethernet frame in one way and a
    // short grant packet in the other way, on the longest path in the topology.
    double rtt;
    // RTT in bytes for the topology, given as a configuration parameter.
    uint32_t rttBytes;

    // This parameter is read from the omnetpp.ini config file and provides an
    // upper bound on the total allowed outstanding bytes. It is necessary for
    // the rxScheduler to check that the total outstanding bytes is smaller than
    // this value every time a new grant is to be sent.
    uint32_t maxOutstandingRecvBytes;

    // udp ports assigned to this transprt
    // int localPort;
    // int destPort;

    // Maximum possible data bytes allowed in grant
    uint32_t grantMaxBytes;

    // Total number of available priorities
    uint32_t allPrio;

    // Total priority levels available for adaptive scheduling when adaptive
    // scheduling is enabled.
    uint32_t adaptiveSchedPrioLevels;

    // Number of senders that receiver tries to keeps granted cuncurrently to
    // avoid its bandwidth wasted in one or more of the these senders stops
    // sending. This number is at most as large as adaptiveSchedPrioLevels
    uint32_t numSendersToKeepGranted;

    // priod intervals at which the transport fires signals that are
    // collected by GlobalSignalListener.
    // double signalEmitPeriod;

    // If true, network traffic bytes from grant packets will be accounted in
    // when computing CBF and unscheduled priority cutoff sizes.
    bool accountForGrantTraffic;

    // Total number of priorities that PrioResolver would use to resolve
    // priorities.
    uint32_t prioResolverPrioLevels;

    // Specifies which priority resolution mode should be used for unscheduled
    // packets. Resolution modes are defined in PrioResolver class.
    const char *unschedPrioResolutionMode;

    // Specifies for the unsched prio level, how many times prio p_i will be
    // used comparing to p_i-1 (p_i is higher prio). Cutoff weight are used
    // in PrioResolver class.
    double unschedPrioUsageWeight;

    // If unschedPrioResolutionMode is set to EXPLICIT, then this string defines
    // the priority cutoff points of unsched bytes for the remaining message
    // sizes. Example would be "100 1500 9000"
    std::vector<uint32_t> explicitUnschedPrioCutoff;

    // Defines the type of logic sender uses for transmitting messages and pkts
    const char *senderScheme;

    // Specifies the scheduler type. True, means round robin scheduler and false
    // is for SRBF scheduler.
    bool isRoundRobinScheduler;

    // If receiver inbound link is idle for longer than (link speed X
    // bwCheckInterval), while there are senders waiting for grants, we consider
    // receiver bw is wasted. -1 means don't check for the bw-waste.
    int linkCheckBytes;

    // Specifies that only first cbfCapMsgSize bytes of a message must be used
    // in computing the cbf function.
    uint32_t cbfCapMsgSize;

    // This value is in bytes and determines that this number of last scheduled
    // bytes of the message will be send at priority equal to unscheduled
    // priorites. The default is 0 bytes.
    uint32_t boostTailBytesPrio;

    // Number data bytes to be packed in request packet.
    uint32_t defaultReqBytes;

    // Maximum number of unscheduled data bytes that will be sent in unsched.
    // packets except the data bytes in request packet.
    uint32_t defaultUnschedBytes;

    // True means that scheduler would use
    // (1-avgUnscheduledRate)*maxOutstandingRecvBytes as the cap for number of
    // scheduled bytes allowed outstanding.
    bool useUnschRateInScheduler;

    // Only for simulation purpose, we allow the transport to be aware of
    // workload type. The value must be similar to WorkloadSynthesizer
    // workloadType.
    const char *workloadType;

    // easy way to pass around debug_ flag
    int debug_;

    // easy way to pass around the addr of this host
    int32_t this_addr_;

    void print_all();

private:
    // Enum-ed corresponding value for SenderScheme param. Provided for doing
    // faster runtime checks based on SenderScheme values.
    SenderScheme sxScheme;

public:
    /**
     * getter method for SenderScheme enum-ed param.
     */
    const SenderScheme
    getSenderScheme()
    {
        return sxScheme;
    }

    // Made it publig. Ugly, no time.
    void paramToEnum();
};
#endif
