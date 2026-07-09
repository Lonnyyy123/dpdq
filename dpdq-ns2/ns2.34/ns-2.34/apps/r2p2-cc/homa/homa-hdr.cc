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

/**
 * @file homa-hdr.cc
 * @brief This file is heavily inspired from / copies https://github.com/PlatformLab/HomaSimulation/tree/omnet_simulations/RpcTransportDesign/OMNeT%2B%2BSimulation
 *
 */
#include "homa-hdr.h"
#include <stdexcept>
#include "homa-transport.h"

int hdr_homa::offset_;

class HomaHeaderClass : public PacketHeaderClass
{
public:
    HomaHeaderClass() : PacketHeaderClass("PacketHeader/HOMA",
                                          sizeof(hdr_homa))
    {
        bind_offset(&hdr_homa::offset_);
    }
} class_homahdr;

UnschedFields::UnschedFields()
{
    msgByteLen = 0;
    msgCreationTime = SIMTIME_ZERO;
    totalUnschedBytes = 0;
    firstByte = 0;
    lastByte = 0;
    prioUnschedBytes = nullptr;
}

GrantFields::GrantFields()
{
    grantBytes = 0;
    offset = 0;
    schedPrio = 0;
}

SchedDataFields::SchedDataFields()
{
    firstByte = 0;
    lastByte = 0;
}

hdr_homa::~hdr_homa()
{
    if (unschedFields_var_.prioUnschedBytes)
    {
        delete unschedFields_var_.prioUnschedBytes;
    }
}

uint32_t
hdr_homa::headerSize()
{
    uint32_t size = sizeof(msgId_var_);
    switch (pktType_var())
    {
    case PktType::REQUEST:
    case PktType::UNSCHED_DATA:
        size += (sizeof(unschedFields_var().msgByteLen) +
                 sizeof(unschedFields_var().totalUnschedBytes) +
                 sizeof(unschedFields_var().firstByte) +
                 sizeof(unschedFields_var().lastByte) + 6); //+
                                                            // sizeof(decltype(*unschedFields_var().prioUnschedBytes)::value_type) * 2;
        break;
    case PktType::GRANT:
        size += (sizeof(grantFields_var().grantBytes) +
                 sizeof(grantFields_var().offset) +
                 sizeof(grantFields_var().schedPrio) +
                 grantFields_var().sizeReqBytesPrio.size() *
                     sizeof(decltype(grantFields_var().sizeReqBytesPrio)::value_type) +
                 grantFields_var().sizeUnschedBytesPrio.size() *
                     sizeof(decltype(grantFields_var().sizeUnschedBytesPrio)::value_type));
        break;

    case PktType::SCHED_DATA:
        size += (sizeof(schedDataFields_var().firstByte) +
                 sizeof(schedDataFields_var().lastByte));
        break;
    default:
        break;
    }
    // std::cout << "hdr_homa::headerSize() returns size:" << size << " for pkt type: " << pktType_var() << std::endl;
    return size;
}

uint32_t
hdr_homa::getDataBytes()
{
    // std::cout << "hdr_homa::getDataBytes()" << std::endl;
    switch (pktType_var())
    {
    case PktType::REQUEST:
    case PktType::UNSCHED_DATA:
        return unschedFields_var().lastByte -
               unschedFields_var().firstByte + 1;
    case PktType::SCHED_DATA:
        return schedDataFields_var().lastByte -
               schedDataFields_var().firstByte + 1;
    case PktType::GRANT:
        return 0;
    default:
        throw std::runtime_error("PktType not defined");
    }
    return 0;
}

uint32_t
hdr_homa::getFirstByte() const
{
    // std::cout << "hdr_homa::getFirstByte()" << std::endl;
    switch (getPktType())
    {
    case PktType::SCHED_DATA:
        return getSchedDataFields().firstByte;
    case PktType::UNSCHED_DATA:
    case PktType::REQUEST:
        return getUnschedFields().firstByte;
    default:
        return 0;
    }
}

// =============== GETTERS(/SETTERS) (for compat with existing code) =================
uint32_t hdr_homa::getPriority() const
{
    return priority_var_;
}

// addresses in ns-2 are int32_t
int32_t hdr_homa::getSrcAddr() const
{
    return srcAddr_var_;
}

uint64_t hdr_homa::getMsgId() const
{
    return msgId_var_;
}

int hdr_homa::getPktType() const
{
    return pktType_var_;
}

SchedDataFields &hdr_homa::getSchedDataFields()
{
    return schedDataFields_var_;
}

GrantFields &hdr_homa::getGrantFields()
{
    return grantFields_var_;
}

UnschedFields &hdr_homa::getUnschedFields()
{
    return unschedFields_var_;
}

// --------------- OMNET++ functions in cMessage ----------------
double hdr_homa::getArrivalTime() const
{
    assert(arrivalTime_ >= 0);
    // must first set it at the link layer
    return arrivalTime_;
}

void hdr_homa::setArrivalTime(const double &arrival_time)
{
    arrivalTime_ = arrival_time;
}

double hdr_homa::getCreationTime() const
{
    return creation_time_;
}

void hdr_homa::setCreationTime(const double &creation_time)
{
    creation_time_ = creation_time;
}

uint32_t hdr_homa::getByteLength() const
{
    return byteLength_var_;
}

void hdr_homa::setByteLength(uint32_t byteLength)
{
    byteLength_var_ = byteLength;
}

/**
 *
 * For a message of size numDataBytes comprised of packets of type homaPktType,
 * this function returns the actual bytes transmitted on the wire. Both data
 * bytes packed in the packet and the header size of the packet are cosidered in
 * the calculation.
 */
uint32_t
hdr_homa::getBytesOnWire(uint32_t numDataBytes, PktType homaPktType)
{
    hdr_homa homaPkt = hdr_homa();
    homaPkt.pktType_var() = homaPktType;

    // FIXME: Using this function can cause bugs in general since HomaPkt
    // header size can be different for two pkts of same type (eg.
    // grant pkts). Care must be taken when using this function. Solution:
    // Better to have a version of this function that takes in a poiter to the
    // pkt and uses the real header size of the packet to do calcualtion when
    // possible. This requires to find all invokations of this function and
    // replace them with the new function when possible.
    uint32_t homaPktHdrSize = homaPkt.headerSize();

    uint32_t bytesOnWire = 0;

    uint32_t maxDataInHomaPkt = MAX_ETHERNET_PAYLOAD - IP_HEADER_SIZE - UDP_HEADER_SIZE - homaPktHdrSize;

    uint32_t numFullPkts = numDataBytes / maxDataInHomaPkt;
    bytesOnWire += numFullPkts * (MAX_ETHERNET_PAYLOAD + ETHERNET_HEADER_SIZE + ETHERNET_PREAMBLE_SIZE + INTER_PKT_GAP_SIZE);

    uint32_t numPartialBytes = numDataBytes - numFullPkts * maxDataInHomaPkt;

    // if all numDataBytes fit in full pkts, we should return at this point
    if (numFullPkts > 0 && numPartialBytes == 0)
    {
        // std::cout << "hdr_homa::getBytesOnWire() returns:" << bytesOnWire << " for type: " << homaPktType << std::endl;
        return bytesOnWire;
    }

    numPartialBytes += homaPktHdrSize + IP_HEADER_SIZE +
                       UDP_HEADER_SIZE;

    if (numPartialBytes < MIN_ETHERNET_PAYLOAD)
    {
        numPartialBytes = MIN_ETHERNET_PAYLOAD;
    }

    uint32_t numPartialBytesOnWire = numPartialBytes + ETHERNET_HEADER_SIZE + ETHERNET_PREAMBLE_SIZE + INTER_PKT_GAP_SIZE;

    bytesOnWire += numPartialBytesOnWire;

    // std::cout << "hdr_homa::getBytesOnWire() returns:" << bytesOnWire << std::endl;
    return bytesOnWire;
}

/**
 * Strictly order packets. Orders first based on their priorities, then arrival
 * time at module (eg. queue), then creation times. For packets belong to same
 * sender, then order based on msgId as msgIds are monotonically increasing.
 * Then for packets belong to same message, also order based on index of data
 * bytes in packet.
 */
bool operator>(const hdr_homa &lhs, const hdr_homa &rhs)
{
    if (lhs.getPriority() > rhs.getPriority() ||
        (lhs.getPriority() == rhs.getPriority() &&
         lhs.getArrivalTime() > rhs.getArrivalTime()) ||

        (lhs.getPriority() == rhs.getPriority() &&
         lhs.getArrivalTime() == rhs.getArrivalTime() &&
         lhs.getCreationTime() > rhs.getCreationTime()) ||

        (lhs.getPriority() == rhs.getPriority() &&
         lhs.getArrivalTime() == rhs.getArrivalTime() &&
         lhs.getCreationTime() == rhs.getCreationTime() &&
         lhs.getSrcAddr() == rhs.getSrcAddr() &&
         lhs.getMsgId() > rhs.getMsgId()) ||

        (lhs.getPriority() == rhs.getPriority() &&
         lhs.getArrivalTime() == rhs.getArrivalTime() &&
         lhs.getCreationTime() == rhs.getCreationTime() &&
         lhs.getSrcAddr() == rhs.getSrcAddr() &&
         lhs.getMsgId() == rhs.getMsgId() &&
         lhs.getFirstByte() > rhs.getFirstByte()))
    {
        // The last two set of conditions are necessary for ordering pkts belong
        // to same message before they are sent to network.
        return true;
    }
    else
    {
        return false;
    }
}