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
 * @file homa-outbound-message.c
 * @brief This file is heavily inspired from / copies https://github.com/PlatformLab/HomaSimulation/tree/omnet_simulations/RpcTransportDesign/OMNeT%2B%2BSimulation
 */

#include "homa-transport.h"
#include "simple-log.h"
#include "priority-resolver.h"

/**
 * Default constructor for OutboundMessage.
 */
HomaTransport::OutboundMessage::OutboundMessage()
    : msgType(HomaMsgType::UNDEF_HOMA_MSG_TYPE),
      reqId(0),
      appLevelId(-1),
      clientThreadId(-1),
      serverThreadId(-1),
      msgId(0),
      msgSize(0),
      bytesToSched(0),
      bytesLeft(0),
      unschedBytesLeft(0),
      nextExpectedUnschedTxTime(SIMTIME_ZERO),
      reqUnschedDataVec({0}),
      destAddr(),
      srcAddr(),
      msgCreationTime(SIMTIME_ZERO),
      txPkts(),
      txSchedPkts(),
      sxController(NULL),
      homaConfig(NULL)
{
}

/**
 * Construcor for HomaTransport::OutboundMessage.
 *
 * \param outMsg
 *      Application message that corresponds to this OutboundMessage.
 * \param sxController
 *      Back pointer to the SendController that handles transmission of packets
 *      for every OutboundMessage in this transport..
 * \param msgId
 *      an message id to uniquely identify this OutboundMessage from every other
 *      message in this transport.
 */
HomaTransport::OutboundMessage::OutboundMessage(AppMessage *outMsg,
                                                SendController *sxController,
                                                uint64_t msgId,
                                                std::vector<uint32_t> reqUnschedDataVec)
    : msgType(outMsg->getMsgType()),
      reqId(outMsg->getReqId()),
      appLevelId(outMsg->getAppLevelId()),
      clientThreadId(outMsg->getClientThreadId()),
      serverThreadId(outMsg->getServerThreadId()),
      msgId(msgId),
      msgSize(outMsg->getByteLength()),
      bytesToSched(outMsg->getByteLength()),
      bytesLeft(outMsg->getByteLength()),
      unschedBytesLeft(0),
      nextExpectedUnschedTxTime(SIMTIME_ZERO),
      reqUnschedDataVec(reqUnschedDataVec),
      destAddr(outMsg->getDestAddr()),
      srcAddr(outMsg->getSrcAddr()),
      msgCreationTime(outMsg->getMsgCreationTime()),
      txPkts(),
      txSchedPkts(),
      sxController(sxController),
      homaConfig(sxController->homaConfig)
{
    slog::log6(sxController->transport->get_debug(), sxController->transport->get_local_addr(),
               "OutboundMessage::OutboundMessage()");
}

/**
 * Destructor for OutboundMessage.
 */
HomaTransport::OutboundMessage::~OutboundMessage()
{
    slog::log6(sxController->transport->get_debug(), sxController->transport->get_local_addr(),
               "OutboundMessage::~OutboundMessage()");
    while (!txPkts.empty())
    {
        hdr_homa *head = txPkts.top();
        txPkts.pop();
        // if (head->unschedFields_var().prioUnschedBytes)
        //     delete head->unschedFields_var().prioUnschedBytes;
        delete head;
    }
}

/**
 * The copy constructor for this OutboundMessage.
 *
 * \param other
 *      The OutboundMessage to make a copy of.
 */
HomaTransport::OutboundMessage::OutboundMessage(const OutboundMessage &other)
{
    copy(other);
}

/**
 * Assignment operator for OutboundMessage.
 *
 * \prama other
 *      Assignment OutboundMessage source.
 *
 * \return
 *      A reference to the destination OutboundMessage variable.
 */
HomaTransport::OutboundMessage &
HomaTransport::OutboundMessage::operator=(const OutboundMessage &other)
{
    if (this == &other)
        return *this;
    copy(other);
    return *this;
}

/**
 * The copy method
 *
 * \param other
 *      The source OutboundMessage to be copied over.
 */
void HomaTransport::OutboundMessage::copy(const OutboundMessage &other)
{
    slog::log6(sxController->transport->get_debug(), sxController->transport->get_local_addr(),
               "OutboundMessage::copy()");
    this->sxController = other.sxController;
    this->homaConfig = other.homaConfig;
    this->msgType = other.msgType;
    this->reqId = other.reqId;
    this->appLevelId = other.appLevelId;
    this->clientThreadId = other.clientThreadId;
    this->serverThreadId = other.serverThreadId;
    this->msgId = other.msgId;
    this->msgSize = other.msgSize;
    this->bytesToSched = other.bytesToSched;
    this->bytesLeft = other.bytesLeft;
    this->unschedBytesLeft = other.unschedBytesLeft;
    this->nextExpectedUnschedTxTime = other.nextExpectedUnschedTxTime;
    this->reqUnschedDataVec = other.reqUnschedDataVec;
    this->destAddr = other.destAddr;
    this->srcAddr = other.srcAddr;
    this->msgCreationTime = other.msgCreationTime;
    this->txPkts = other.txPkts;
    this->txSchedPkts = other.txSchedPkts;
}

/**
 * For every newly created OutboundMessage, calling this method will construct
 * request and unscheduled packets to be transmitted for the message and set the
 * appropriate priority for the packets.
 * KP note: in this port, this function is also called for replies (original code does not distinguish reqs and reps)
 */
void HomaTransport::OutboundMessage::prepareRequestAndUnsched()
{
    slog::log5(sxController->transport->get_debug(), sxController->transport->get_local_addr(),
               "OutboundMessage::prepareRequestAndUnsched()");
    std::vector<uint32_t> unschedPrioVec =
        sxController->prioResolver->getUnschedPktsPrio(this);

    uint32_t totalUnschedBytes = 0;
    std::vector<uint32_t> *prioUnschedBytes = new std::vector<uint32_t>();
    uint32_t prio = UINT16_MAX;
    for (size_t i = 0; i < this->reqUnschedDataVec.size(); i++)
    {
        if (unschedPrioVec[i] != prio)
        {
            prio = unschedPrioVec[i];
            prioUnschedBytes->push_back(prio);
            prioUnschedBytes->push_back(reqUnschedDataVec[i]);
        }
        else
        {
            prioUnschedBytes->back() += reqUnschedDataVec[i];
        }
        totalUnschedBytes += reqUnschedDataVec[i];
    }
    unschedBytesLeft = totalUnschedBytes;

    // Index of the next byte to be transmitted for this msg. Always
    // initialized to zero.
    uint32_t nextByteToSched = 0;

    // First pkt, always request
    PktType pktType = PktType::REQUEST;
    size_t i = 0;
    do
    {
        // HomaPkt *unschedPkt = new HomaPkt(sxController->transport);
        hdr_homa *unschedPkt = new hdr_homa();
        unschedPkt->msg_creation_time_var() = msgCreationTime;
        unschedPkt->ownerTransport = sxController->transport;
        unschedPkt->pktType_var() = pktType;
        assert(msgType != HomaMsgType::UNDEF_HOMA_MSG_TYPE);
        assert(msgType != HomaMsgType::OTHER_MSG);
        unschedPkt->msgType_var() = msgType;
        unschedPkt->reqId_var() = reqId;
        unschedPkt->appLevelId_var() = appLevelId;
        unschedPkt->clientThreadId_var() = clientThreadId;
        unschedPkt->serverThreadId_var() = serverThreadId;

        // set homa fields
        unschedPkt->destAddr_var() = this->destAddr;
        unschedPkt->srcAddr_var() = this->srcAddr;
        unschedPkt->msgId_var() = this->msgId;
        unschedPkt->priority_var() = unschedPrioVec[i];

        slog::log6(sxController->transport->get_debug(), sxController->transport->get_local_addr(),
                   "Created a packet. hdr fields for pkt(?)", i, ":",
                   "destAddr_var:", unschedPkt->destAddr_var(),
                   "srcAddr_var:", unschedPkt->srcAddr_var(),
                   "msgId_var:", unschedPkt->msgId_var(),
                   "priority_var:", unschedPkt->priority_var(),
                   "getCreationTime:", unschedPkt->getCreationTime(),
                   "msgType:", unschedPkt->msgType_var(),
                   "req_id:", unschedPkt->reqId_var(),
                   "app_id:", unschedPkt->appLevelId_var(),
                   "client thread:", unschedPkt->clientThreadId_var(),
                   "server thread:", unschedPkt->serverThreadId_var());

        // fill up unschedFields
        UnschedFields unschedFields;
        unschedFields.msgByteLen = msgSize;
        unschedFields.msgCreationTime = msgCreationTime;
        unschedFields.totalUnschedBytes = totalUnschedBytes;
        unschedFields.firstByte = nextByteToSched;
        unschedFields.lastByte =
            nextByteToSched + this->reqUnschedDataVec[i] - 1;
        unschedFields.prioUnschedBytes = new std::vector<uint32_t>(*prioUnschedBytes); // copy vector
        for (size_t j = 0;
             j < unschedFields.prioUnschedBytes->size(); j += 2)
        {
            if ((*unschedFields.prioUnschedBytes)[j] == unschedPkt->priority_var())
            {
                // find the two elements in this prioUnschedBytes vec that
                // corresponds to the priority of this packet and subtract
                // the bytes in this packet from prioUnschedBytes.
                (*unschedFields.prioUnschedBytes)[j + 1] -= this->reqUnschedDataVec[i];
                if ((*unschedFields.prioUnschedBytes)[j + 1] <= 0)
                {
                    // Delete this element if unsched bytes on this prio is
                    // zero
                    unschedFields.prioUnschedBytes
                        ->erase(unschedFields.prioUnschedBytes
                                        ->begin() +
                                    j,
                                unschedFields.prioUnschedBytes->begin() + j + 2);
                }
                break;
            }
        }
        unschedPkt->unschedFields_var() = unschedFields;
        unschedPkt->setByteLength(unschedPkt->headerSize() +
                                  unschedPkt->getDataBytes());
        slog::log5(sxController->transport->get_debug(), sxController->transport->get_local_addr(),
                   "unsched fields for pkt(?)", i, ":",
                   unschedPkt->unschedFields_var().firstByte,
                   unschedPkt->unschedFields_var().lastByte,
                   unschedPkt->unschedFields_var().msgByteLen,
                   unschedPkt->unschedFields_var().msgCreationTime,
                   unschedPkt->unschedFields_var().totalUnschedBytes,
                   unschedPkt->unschedFields_var().prioUnschedBytes->size());

        txPkts.push(unschedPkt);
        /**
        EV << "Unsched pkt with msgId " << this->msgId << " ready for"
            " transmit from " << this->srcAddr.str() << " to " <<
            this->destAddr.str() << endl;
        **/

        // update the OutboundMsg for the bytes sent in this iteration of loop
        this->bytesToSched -= unschedPkt->getDataBytes();
        nextByteToSched += unschedPkt->getDataBytes();

        // all packet except the first one are UNSCHED
        pktType = PktType::UNSCHED_DATA;
        i++;
    } while (i < reqUnschedDataVec.size());
    delete prioUnschedBytes;
}

/**
 * Creates a new scheduled packet for this message and stores it in the set
 * scheduled packets ready to be transmitted for this message.
 *
 * \param numBytes
 *      number of data bytes in the scheduled packet.
 * \param schedPrio
 *      priority value assigned to this sched packet by the receiver.
 * \return
 *      remaining scheduled bytes to be sent for this message.
 */
uint32_t
HomaTransport::OutboundMessage::prepareSchedPkt(uint32_t offset,
                                                uint32_t numBytes, uint32_t schedPrio)
{
    assert(this->bytesToSched > 0);
    uint32_t bytesToSend = std::min((uint32_t)numBytes, this->bytesToSched);

    // create a data pkt and push it txPkts queue for
    hdr_homa *dataPkt = new hdr_homa();
    dataPkt->msg_creation_time_var() = msgCreationTime;
    dataPkt->ownerTransport = sxController->transport;
    dataPkt->pktType_var() = PktType::SCHED_DATA;
    dataPkt->msgType_var() = msgType;
    dataPkt->reqId_var() = reqId;
    dataPkt->appLevelId_var() = appLevelId;
    dataPkt->clientThreadId_var() = clientThreadId;
    dataPkt->serverThreadId_var() = serverThreadId;
    dataPkt->srcAddr_var() = this->srcAddr;
    dataPkt->destAddr_var() = this->destAddr;
    dataPkt->msgId_var() = this->msgId;
    dataPkt->priority_var() = schedPrio;
    SchedDataFields dataFields;
    dataFields.firstByte = offset;
    dataFields.lastByte = dataFields.firstByte + bytesToSend - 1;
    dataPkt->schedDataFields_var() = dataFields;
    dataPkt->setByteLength(bytesToSend + dataPkt->headerSize());
    txPkts.push(dataPkt);
    txSchedPkts.insert(dataPkt);

    slog::log6(sxController->transport->get_debug(),
               sxController->transport->get_local_addr(), "OutboundMessage::prepareSchedPkt() of", appLevelId, "to", destAddr, "bytesToSend",
               bytesToSend);

    // update outbound messgae
    this->bytesToSched -= bytesToSend;
    slog::log6(sxController->transport->get_debug(), sxController->transport->get_local_addr(),
               "Prepared", bytesToSend, "bytes for transmission from msgId", this->msgId, "to destination",
               this->destAddr, "(", this->bytesToSched, "bytes left.)");
    return this->bytesToSched;
    return 0;
}

/**
 * A utility predicate for comparing HomaPkt instances
 * based on pkt information and senderScheme param.
 *
 * \param pkt1
 *      first pkt for comparison
 * \param pkt2
 *      second pkt for comparison
 * \return
 *      true if pkt1 is compared greater than pkt2.
 */
bool HomaTransport::OutboundMessage::OutbndPktSorter::operator()(const hdr_homa *pkt1,
                                                                 const hdr_homa *pkt2)
{
    HomaConfigDepot::SenderScheme txScheme =
        pkt1->ownerTransport->homaConfig->getSenderScheme();
    switch (txScheme)
    {
    case HomaConfigDepot::SenderScheme::OBSERVE_PKT_PRIOS:
    case HomaConfigDepot::SenderScheme::SRBF:
        return (
            pkt1->getFirstByte() > pkt2->getFirstByte() ||
            (pkt1->getFirstByte() == pkt2->getFirstByte() &&
             pkt1->getCreationTime() > pkt2->getCreationTime()) ||
            (pkt1->getFirstByte() == pkt2->getFirstByte() &&
             pkt1->getCreationTime() == pkt2->getCreationTime() &&
             pkt1->getPriority() > pkt2->getPriority()));
    default:
        throw std::runtime_error("Undefined SenderScheme parameter");
    }
    return true;
}

/**
 * Among all packets ready for transmission for this message, this function
 * retrieves highest priority packet from sender's perspective for this message.
 * Call this function only when ready to send the packet out onto the wire.
 *
 * \return outPkt
 *      The packet to be transmitted for this message
 * \return
 *      True if the returned pkt is not the last ready for transmission
 *      packet for this msg and this message hase more pkts queue up and ready
 *      for transmission.
 */
bool HomaTransport::OutboundMessage::getTransmitReadyPkt(hdr_homa **outPkt)
{
    slog::log6(sxController->transport->get_debug(), sxController->transport->get_local_addr(), "OutboundMessage::getTransmitReadyPkt()");
    assert(!txPkts.empty());
    hdr_homa *head = txPkts.top();
    PktType outPktType = (PktType)head->getPktType();
    txPkts.pop();
    if (outPktType == PktType::SCHED_DATA)
    {
        txSchedPkts.erase(head);
    }

    *outPkt = head;
    uint32_t numDataBytes = head->getDataBytes();
    bytesLeft -= numDataBytes;

    if (outPktType == PktType::REQUEST || outPktType == PktType::UNSCHED_DATA)
    {
        unschedBytesLeft -= numDataBytes;
    }

    // KP. I expect that these are data packets
    assert((*outPkt)->msgType_var() != HomaMsgType::UNDEF_HOMA_MSG_TYPE);
    assert((*outPkt)->msgType_var() != HomaMsgType::OTHER_MSG);

    return !txPkts.empty();
}