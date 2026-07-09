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

#include <sstream>
#include "homa-config-depot.h"

#define MAX_STR_LEN

HomaConfigDepot::HomaConfigDepot(HomaTransport *ownerTransport)
    : ownerTransport(ownerTransport)
{
    // only relevant if unschedPrioResolutionMode="EXPLICIT"
    // read in explicitUnschedPrioCutoff
    std::istringstream ss(
        std::string("100 1500 9000"));
    uint32_t cutoffSize;
    while (ss >> cutoffSize)
    {
        explicitUnschedPrioCutoff.push_back(cutoffSize);
    }
}

void HomaConfigDepot::paramToEnum()
{
    if (strcmp(senderScheme, "OBSERVE_PKT_PRIOS") == 0)
    {
        sxScheme = SenderScheme::OBSERVE_PKT_PRIOS;
    }
    else if (strcmp(senderScheme, "SRBF") == 0)
    {
        sxScheme = SenderScheme::SRBF;
    }
    else
    {
        throw std::invalid_argument("Unknown SenderScheme");
    }
}

void HomaConfigDepot::print_all()
{
    std::cout << " -------- HomaConfigDepot::print_all() start --------" << std::endl;
    std::cout << "nicLinkSpeed: " << nicLinkSpeed << std::endl;
    std::cout << "rtt: " << rtt << std::endl;
    std::cout << "rttBytes: " << rttBytes << std::endl;
    std::cout << "maxOutstandingRecvBytes: " << maxOutstandingRecvBytes << std::endl;
    std::cout << "grantMaxBytes: " << grantMaxBytes << std::endl;
    std::cout << "allPrio: " << allPrio << std::endl;
    std::cout << "adaptiveSchedPrioLevels: " << adaptiveSchedPrioLevels << std::endl;
    std::cout << "numSendersToKeepGranted: " << numSendersToKeepGranted << std::endl;
    std::cout << "accountForGrantTraffic: " << accountForGrantTraffic << std::endl;
    std::cout << "prioResolverPrioLevels: " << prioResolverPrioLevels << std::endl;
    std::cout << "unschedPrioResolutionMode: " << unschedPrioResolutionMode << std::endl;
    std::cout << "unschedPrioUsageWeight: " << unschedPrioUsageWeight << std::endl;
    std::cout << "senderScheme: " << senderScheme << std::endl;
    std::cout << "isRoundRobinScheduler: " << isRoundRobinScheduler << std::endl;
    std::cout << "linkCheckBytes: " << linkCheckBytes << std::endl;
    std::cout << "cbfCapMsgSize: " << cbfCapMsgSize << std::endl;
    std::cout << "boostTailBytesPrio: " << boostTailBytesPrio << std::endl;
    std::cout << "defaultReqBytes: " << defaultReqBytes << std::endl;
    std::cout << "defaultUnschedBytes: " << defaultUnschedBytes << std::endl;
    std::cout << "useUnschRateInScheduler: " << useUnschRateInScheduler << std::endl;
    std::cout << "workloadType: " << workloadType << std::endl;
    std::cout << "debug_: " << debug_ << std::endl;
    std::cout << "this_addr_: " << this_addr_ << " (ok if bad)" << std::endl;
    std::cout << " -------- HomaConfigDepot::print_all() end --------" << std::endl;
}
