//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this program.  If not, see http://www.gnu.org/licenses/.
//

/*
 * UnschedByteAllocator.cc
 *
 *  Created on: Oct 15, 2015
 *      Author: behnamm
 */

/**
 * @brief This file is taken almost as-is from https://github.com/PlatformLab/HomaSimulation/tree/omnet_simulations/RpcTransportDesign/OMNeT%2B%2BSimulation
 */
#include <cmath>
#include "unsched-byte-allocator.h"
#include "r2p2.h" // for macros. TODO: fix
#include "simple-log.h"
#include "homa-config-depot.h"

UnschedByteAllocator::UnschedByteAllocator(HomaConfigDepot *homaConfig)
    : rxAddrUnschedbyteMap(), rxAddrReqbyteMap(), homaConfig(homaConfig)

{
    slog::log2(homaConfig->debug_, homaConfig->this_addr_, "UnschedByteAllocator::UnschedByteAllocator()");
    assert(homaConfig != nullptr);
    hdr_homa reqPkt = hdr_homa();
    reqPkt.pktType_var() = PktType::REQUEST;
    // Originally: 1500 - 20 - 8 - reqPkt.headerSize()
    maxReqPktDataBytes = MAX_ETHERNET_PAYLOAD - IP_HEADER_SIZE - UDP_HEADER_SIZE - reqPkt.headerSize();

    hdr_homa unschedPkt = hdr_homa();
    unschedPkt.pktType_var() = PktType::UNSCHED_DATA;
    maxUnschedPktDataBytes = MAX_ETHERNET_PAYLOAD -
                             IP_HEADER_SIZE - UDP_HEADER_SIZE - unschedPkt.headerSize();

    slog::log2(homaConfig->debug_, homaConfig->this_addr_, "UnschedByteAllocator::UnschedByteAllocator(). maxReqPktDataBytes=", maxReqPktDataBytes, "maxUnschedPktDataBytes=", maxUnschedPktDataBytes);
}

UnschedByteAllocator::~UnschedByteAllocator() {}

void UnschedByteAllocator::initReqBytes(int32_t rxAddr)
{
    slog::log6(homaConfig->debug_, homaConfig->this_addr_, "UnschedByteAllocator::initReqBytes()");
    if (homaConfig->defaultReqBytes > maxReqPktDataBytes)
    {
        throw std::invalid_argument("Config Param defaultReqBytes set: " + std::to_string(homaConfig->defaultReqBytes) + "is larger than" +
                                    " max possible bytes:" + std::to_string(maxReqPktDataBytes));
    }
    rxAddrReqbyteMap[rxAddr].clear();
    rxAddrReqbyteMap[rxAddr] = {{UINT32_MAX, homaConfig->defaultReqBytes}};
}

uint32_t
UnschedByteAllocator::getReqDataBytes(int32_t rxAddr, uint32_t msgSize)
{
    slog::log6(homaConfig->debug_, homaConfig->this_addr_, "UnschedByteAllocator::getReqDataBytes()");
    auto reqBytesMap = rxAddrReqbyteMap.find(rxAddr);
    if (reqBytesMap == rxAddrReqbyteMap.end())
    {
        initReqBytes(rxAddr);
        reqBytesMap = rxAddrReqbyteMap.find(rxAddr);
    }
    return std::min(reqBytesMap->second.lower_bound(msgSize)->second, msgSize);
}

void UnschedByteAllocator::initUnschedBytes(int32_t rxAddr)
{
    slog::log6(homaConfig->debug_, homaConfig->this_addr_, "UnschedByteAllocator::initUnschedBytes() homaConfig->defaultUnschedBytes: ", homaConfig->defaultUnschedBytes);
    rxAddrUnschedbyteMap[rxAddr].clear();
    rxAddrUnschedbyteMap[rxAddr] = {{UINT32_MAX,
                                     homaConfig->defaultUnschedBytes}};
}

uint32_t
UnschedByteAllocator::getUnschedBytes(int32_t rxAddr, uint32_t msgSize)
{
    slog::log6(homaConfig->debug_, homaConfig->this_addr_, "UnschedByteAllocator::getUnschedBytes()");
    auto unschedBytesMap = rxAddrUnschedbyteMap.find(rxAddr);
    if (unschedBytesMap == rxAddrUnschedbyteMap.end())
    {
        initUnschedBytes(rxAddr);
        unschedBytesMap = rxAddrUnschedbyteMap.find(rxAddr);
    }

    uint32_t reqDataBytes = getReqDataBytes(rxAddr, msgSize);
    if (msgSize <= reqDataBytes)
    {
        return 0;
    }
    return std::min(msgSize - reqDataBytes,
                    unschedBytesMap->second.lower_bound(msgSize)->second);
}

/**
 * Return value must be a a vector of at least size 1.
 */
std::vector<uint32_t>
UnschedByteAllocator::getReqUnschedDataPkts(int32_t rxAddr, uint32_t msgSize)
{
    slog::log6(homaConfig->debug_, homaConfig->this_addr_, "UnschedByteAllocator::getReqUnschedDataPkts()");
    uint32_t reqData = getReqDataBytes(rxAddr, msgSize);
    std::vector<uint32_t> reqUnschedPktsData = {};
    reqUnschedPktsData.push_back(reqData);
    uint32_t unschedData = getUnschedBytes(rxAddr, msgSize);
    while (unschedData > 0)
    {
        uint32_t unschedInPkt = std::min(unschedData, maxUnschedPktDataBytes);
        reqUnschedPktsData.push_back(unschedInPkt);
        unschedData -= unschedInPkt;
    }
    return reqUnschedPktsData;
}

// not implemented in original code too
void UnschedByteAllocator::updateReqDataBytes(hdr_homa *grantPkt)
{
    slog::log6(homaConfig->debug_, homaConfig->this_addr_, "UnschedByteAllocator::updateReqDataBytes()");
    assert(0);
}

// not implemented in original code too
void UnschedByteAllocator::updateUnschedBytes(hdr_homa *grantPkt)
{
    slog::log6(homaConfig->debug_, homaConfig->this_addr_, "UnschedByteAllocator::updateUnschedBytes()");
    assert(0);
}
