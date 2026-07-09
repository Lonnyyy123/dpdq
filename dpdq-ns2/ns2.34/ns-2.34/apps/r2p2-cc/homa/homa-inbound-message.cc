#include "homa-transport.h"
#include "simple-log.h"

/**
 * @file homa-inbound-message.cc
 * @brief This file mostly copies https://github.com/PlatformLab/HomaSimulation/tree/omnet_simulations/RpcTransportDesign/OMNeT%2B%2BSimulation
 */

/**
 * Default Constructor of HomaTransport::InboundMessage.
 */
HomaTransport::InboundMessage::InboundMessage()
    : msgType(HomaMsgType::UNDEF_HOMA_MSG_TYPE),
      reqId(0),
      appLevelId(-1),
      clientThreadId(-1),
      serverThreadId(-1),
      rxScheduler(NULL),
      homaConfig(NULL),
      srcAddr(),
      destAddr(),
      msgIdAtSender(0),
      bytesToGrant(0),
      offset(0),
      bytesGrantedInFlight(0),
      bytesToReceive(0),
      msgSize(0),
      totalBytesOnWire(0),
      totalUnschedBytes(0),
      msgCreationTime(SIMTIME_ZERO),
      reqArrivalTime(Scheduler::instance().clock()),
      lastGrantTime(Scheduler::instance().clock()),
      inflightGrants(),
      bytesRecvdPerPrio(),
      scheduledBytesPerPrio(),
      unschedToRecvPerPrio()
{
}

HomaTransport::InboundMessage::~InboundMessage()
{
}

HomaTransport::InboundMessage::InboundMessage(const InboundMessage &other)
{
    copy(other);
}

/**
 * Constructor of InboundMessage.
 *
 * \param rxPkt
 *      First packet received for a message at the receiver. The packet type is
 *      either REQUEST or UNSCHED_DATA.
 * \param rxScheduler
 *      ReceiveScheduler that handles scheduling, priority assignment, and
 *      reception of packets for this message.
 * \param homaConfig
 *      Collection of user provided config parameters for the transport.
 */
HomaTransport::InboundMessage::InboundMessage(hdr_homa *rxPkt,
                                              ReceiveScheduler *rxScheduler,
                                              HomaConfigDepot *homaConfig)
    : msgType(rxPkt->msgType_var()),
      reqId(rxPkt->reqId_var()),
      appLevelId(rxPkt->appLevelId_var()),
      clientThreadId(rxPkt->clientThreadId_var()),
      serverThreadId(rxPkt->serverThreadId_var()),
      rxScheduler(rxScheduler),
      homaConfig(homaConfig),
      srcAddr(rxPkt->getSrcAddr()),
      destAddr(rxPkt->destAddr_var()),
      msgIdAtSender(rxPkt->getMsgId()),
      bytesToGrant(0),
      offset(0),
      bytesGrantedInFlight(0),
      totalBytesInFlight(0),
      bytesToReceive(0),
      msgSize(0),
      schedBytesOnWire(0),
      totalBytesOnWire(0),
      totalUnschedBytes(0),
      msgCreationTime(SIMTIME_ZERO),
      reqArrivalTime(Scheduler::instance().clock()),
      lastGrantTime(Scheduler::instance().clock()),
      inflightGrants(),
      bytesRecvdPerPrio(),
      scheduledBytesPerPrio(),
      unschedToRecvPerPrio()
{
    assert(rxPkt->msgType_var() != HomaMsgType::UNDEF_HOMA_MSG_TYPE);
    switch (rxPkt->getPktType())
    {
    case PktType::REQUEST:
    case PktType::UNSCHED_DATA:
        bytesToGrant = rxPkt->getUnschedFields().msgByteLen -
                       rxPkt->getUnschedFields().totalUnschedBytes;
        offset = rxPkt->getUnschedFields().totalUnschedBytes;
        bytesToReceive = rxPkt->getUnschedFields().msgByteLen;
        msgSize = rxPkt->getUnschedFields().msgByteLen;
        totalUnschedBytes = rxPkt->getUnschedFields().totalUnschedBytes;
        msgCreationTime = rxPkt->getUnschedFields().msgCreationTime;
        totalBytesInFlight = hdr_homa::getBytesOnWire(
            totalUnschedBytes, PktType::UNSCHED_DATA);
        inflightGrants.push_back(std::make_tuple(0, totalUnschedBytes,
                                                 std::min(Scheduler::instance().clock() - homaConfig->rtt,
                                                          msgCreationTime - homaConfig->rtt / 2)));
        break;
    default:
        throw std::runtime_error("Can't create inbound message "
                                 "from received packet type: " +
                                 rxPkt->getPktType());
    }
}

/**
 * Add a received chunk of bytes to the message.
 *
 * \param byteStart
 *      Offset index in the message at which the chunk of bytes should be added.
 * \param byteEnd
 *      Index of last byte of the chunk in the message.
 * \param pktType
 *      Kind of the packet that has arrived at the receiver and carried the
 *      chunck of data bytes.
 */
void HomaTransport::InboundMessage::fillinRxBytes(uint32_t byteStart,
                                                  uint32_t byteEnd, PktType pktType)
{
    slog::log6(rxScheduler->transport->get_debug(), rxScheduler->transport->get_local_addr(),
               "InboundMessage::fillinRxBytes()");

    // std::cout << "starts of scheduled " << this->rxScheduler->transport->rttBytes << std::endl;
    uint32_t bytesReceived = byteEnd - byteStart + 1;
    uint32_t bytesReceivedOnWire =
        hdr_homa::getBytesOnWire(bytesReceived, pktType);
    bytesToReceive -= bytesReceived;
    totalBytesInFlight -= bytesReceivedOnWire;
    totalBytesOnWire += bytesReceivedOnWire;

    if (pktType == PktType::UNSCHED_DATA || pktType == PktType::REQUEST)
    {
        // update inflight grants list
        GrantList::iterator grantZero = inflightGrants.begin();
        assert(std::get<0>(*grantZero) == 0);
        assert(std::get<1>(*grantZero) >= bytesReceived);
        std::get<1>(*grantZero) -= bytesReceived;
        if (std::get<1>(*grantZero) == 0)
        {
            inflightGrants.erase(grantZero);
        }
    }

    if (pktType == PktType::SCHED_DATA)
    {
        assert(bytesReceived > 0);
        // update inflight grants list
        // std::cout << "first byte: " << byteStart <<
        //    ", last byte: " << byteEnd  << std::endl;
        GrantList::iterator grant;
        // std::cout << " list sz " << inflightGrants.size() << std::endl;
        for (grant = inflightGrants.begin(); grant != inflightGrants.end();
             grant++)
        {

            // std::cout << "grant offset: " << std::get<0>(*grant) << std::endl;
            // std::cout << "Checking grant " << std::get<0>(*grant) << "  " << std::get<1>(*grant) << "  " << std::get<2>(*grant) << " list sz " << inflightGrants.size() << std::endl;
            if (std::get<0>(*grant) == byteStart)
            {
                // std::cout << "Breaking " << byteStart << std::endl;
                break;
            }
        }
        assert(grant != inflightGrants.end());
        assert(std::get<1>(*grant) == bytesReceived);
        slog::log6(rxScheduler->transport->get_debug(), rxScheduler->transport->get_local_addr(), "bytesReceivedOnWire:", bytesReceivedOnWire, "==? hdr_homa::maxEthFrameSize()", hdr_homa::maxEthFrameSize());
        if (bytesReceivedOnWire == hdr_homa::maxEthFrameSize())
        {
            rxScheduler->transport->trackRTTs.updateRTTSample(srcAddr,
                                                              Scheduler::instance().clock() - std::get<2>(*grant));
        }
        inflightGrants.erase(grant);

        bytesGrantedInFlight -= bytesReceived;
        rxScheduler->transport->outstandingGrantBytes -= bytesReceivedOnWire;
    }
    slog::log6(rxScheduler->transport->get_debug(), rxScheduler->transport->get_local_addr(),
               "Received:", bytesReceived,
               "bytes from", srcAddr,
               "for msgId", msgIdAtSender,
               "(", bytesToReceive,
               "bytes left to receive)");
}

/**
 * Creates a grant packet for this message and update the internal structure of
 * the message.
 *
 * \param grantSize
 *      Size of the scheduled data bytes that will specified in the grant
 *      packet.
 * \param schedPrio
 *      Priority of the scheduled packet that this grant packet is sent for.
 * \return
 *      Grant packet that is to be sent on the wire to the sender of this
 *      message.
 */
hdr_homa *
HomaTransport::InboundMessage::prepareGrant(uint32_t grantSize,
                                            uint32_t schedPrio)
{
    slog::log6(rxScheduler->transport->get_debug(), rxScheduler->transport->get_local_addr(),
               "InboundMessage::prepareGrant()");
    // prepare a grant
    hdr_homa *grantPkt = new hdr_homa();
    grantPkt->msg_creation_time_var() = Scheduler::instance().clock();
    grantPkt->ownerTransport = rxScheduler->transport;
    grantPkt->pktType_var() = PktType::GRANT;
    grantPkt->msgType_var() = HomaMsgType::OTHER_MSG;
    GrantFields grantFields;
    grantFields.offset = offset;
    grantFields.grantBytes = grantSize;
    grantFields.schedPrio = schedPrio;
    grantPkt->grantFields_var() = grantFields;
    grantPkt->destAddr_var() = this->srcAddr;
    grantPkt->srcAddr_var() = this->destAddr;
    grantPkt->msgId_var() = this->msgIdAtSender;
    grantPkt->setByteLength(grantPkt->headerSize());
    grantPkt->priority_var() = 0;
    // grant packet should not care about reqId, appLvlId, and thread ids

    int grantedBytesOnWire = hdr_homa::getBytesOnWire(grantSize,
                                                      PktType::SCHED_DATA);
    rxScheduler->transport->outstandingGrantBytes += grantedBytesOnWire;

    // update internal structure
    this->inflightGrants.push_back(
        std::make_tuple(offset, grantSize, Scheduler::instance().clock()));
    this->bytesToGrant -= grantSize;
    this->offset += grantSize;
    this->schedBytesOnWire += grantedBytesOnWire;
    this->bytesGrantedInFlight += grantSize;
    this->lastGrantTime = Scheduler::instance().clock();

    this->totalBytesInFlight += grantedBytesOnWire;

    // Check updated offset value agrees with the bytesToGrant update.
    assert(this->offset == this->msgSize - this->bytesToGrant);
    return grantPkt;
}

/**
 * Create an application message once this InboundMessage is completely
 * received.
 *
 * \return
 *      Message constructed for the application and is to be passed to it.
 */
AppMessage *
HomaTransport::InboundMessage::prepareRxMsgForApp()
{
    slog::log6(rxScheduler->transport->get_debug(), rxScheduler->transport->get_local_addr(),
               "InboundMessage::prepareRxMsgForApp()");
    AppMessage *rxMsg = new AppMessage();
    rxMsg->setMsgType(this->msgType);
    rxMsg->setDestAddr(this->destAddr);
    rxMsg->setSrcAddr(this->srcAddr);
    rxMsg->setByteLength(this->msgSize);
    rxMsg->setMsgCreationTime(this->msgCreationTime);
    rxMsg->setMsgBytesOnWire(this->totalBytesOnWire);
    rxMsg->setReqId(this->reqId);
    rxMsg->setAppLevelId(this->appLevelId);
    rxMsg->setClientThreadId(this->clientThreadId);
    rxMsg->setServerThreadId(this->serverThreadId);

    slog::log6(rxScheduler->transport->get_debug(), rxScheduler->transport->get_local_addr(),
               "Prepeared an AppMessage with the following fields:",
               "msgType", rxMsg->getMsgType(),
               "destAddr", rxMsg->getDestAddr(),
               "srcAddr", rxMsg->getSrcAddr(),
               "msgCreationTime", rxMsg->getMsgCreationTime(),
               "totalBytesOnWire", rxMsg->getMsgBytesOnWire(),
               "reqId", rxMsg->getReqId(),
               "appLevelId", rxMsg->getAppLevelId(),
               "clientThreadId", rxMsg->getClientThreadId(),
               "serverThreadId", rxMsg->getServerThreadId());

    // calculate scheduling delay
    if (totalUnschedBytes >= msgSize)
    {
        // no grant was expected for this message
        assert(reqArrivalTime == lastGrantTime && bytesToGrant == 0 &&
               bytesToReceive == 0);
        rxMsg->setTransportSchedDelay(SIMTIME_ZERO);
    }
    else
    {
        assert(reqArrivalTime <= lastGrantTime && bytesToGrant == 0 &&
               bytesToReceive == 0);
        double totalSchedTime = lastGrantTime - reqArrivalTime;
        double minSchedulingTime;
        uint32_t bytesGranted = msgSize - totalUnschedBytes;
        uint32_t bytesGrantedOnWire = hdr_homa::getBytesOnWire(bytesGranted,
                                                               PktType::SCHED_DATA);
        int minSchedTimeInBytes = (int)bytesGrantedOnWire - hdr_homa::maxEthFrameSize();
        minSchedulingTime = SIMTIME_ZERO;
        if (minSchedTimeInBytes > 0)
        {
            minSchedulingTime =
                (minSchedTimeInBytes * 8.0 / homaConfig->nicLinkSpeed) / 1000.0 / 1000.0 / 1000.0;
        }
        double schedDelay = totalSchedTime - minSchedulingTime;
        rxMsg->setTransportSchedDelay(schedDelay);
    }
    return rxMsg;
}

/**
 * Copy over the statistics in the InboundMessge that mirror the corresponding
 * stats in the ReceiveScheduler.
 */
void HomaTransport::InboundMessage::updatePerPrioStats()
{
    slog::log6(rxScheduler->transport->get_debug(), rxScheduler->transport->get_local_addr(),
               "InboundMessage::updatePerPrioStats()");
    this->bytesRecvdPerPrio = rxScheduler->bytesRecvdPerPrio;
    this->scheduledBytesPerPrio = rxScheduler->scheduledBytesPerPrio;
    this->unschedToRecvPerPrio = rxScheduler->unschedToRecvPerPrio;
    this->sumInflightUnschedPerPrio = rxScheduler->getInflightUnschedPerPrio();
    this->sumInflightSchedPerPrio = rxScheduler->getInflightSchedPerPrio();
}

/**
 * Getter method of granted but not yet received bytes (ie. outstanding
 * granted bytes) for this message.
 *
 * \return
 *      Outstanding granted bytes.
 */
uint32_t
HomaTransport::InboundMessage::schedBytesInFlight()
{
    slog::log6(rxScheduler->transport->get_debug(), rxScheduler->transport->get_local_addr(),
               "InboundMessage::schedBytesInFlight()");
    return bytesGrantedInFlight;
}

/**
 * Getter method of bytes sent by the sender but not yet received bytes (ie.
 * outstanding bytes) for this message.
 *
 * \return
 *      Outstanding bytes.
 */
uint32_t
HomaTransport::InboundMessage::unschedBytesInFlight()
{
    slog::log6(rxScheduler->transport->get_debug(), rxScheduler->transport->get_local_addr(),
               "InboundMessage::unschedBytesInFlight()");
    assert(bytesToReceive >= bytesToGrant + bytesGrantedInFlight);
    return bytesToReceive - bytesToGrant - schedBytesInFlight();
}

void HomaTransport::InboundMessage::copy(const InboundMessage &other)
{
    this->msgType = other.msgType;
    this->reqId = other.reqId;
    this->appLevelId = other.appLevelId;
    this->clientThreadId = other.clientThreadId;
    this->serverThreadId = other.serverThreadId;
    this->rxScheduler = other.rxScheduler;
    this->homaConfig = other.homaConfig;
    this->srcAddr = other.srcAddr;
    this->destAddr = other.destAddr;
    this->msgIdAtSender = other.msgIdAtSender;
    this->bytesToGrant = other.bytesToGrant;
    this->offset = other.offset;
    this->schedBytesOnWire = other.schedBytesOnWire;
    this->bytesGrantedInFlight = other.bytesGrantedInFlight;
    this->totalBytesInFlight = other.totalBytesInFlight;
    this->bytesToReceive = other.bytesToReceive;
    this->msgSize = other.msgSize;
    this->totalBytesOnWire = other.totalBytesOnWire;
    this->totalUnschedBytes = other.totalUnschedBytes;
    this->msgCreationTime = other.msgCreationTime;
    this->reqArrivalTime = other.reqArrivalTime;
    this->lastGrantTime = other.lastGrantTime;
    this->inflightGrants = other.inflightGrants;
    this->bytesRecvdPerPrio = other.bytesRecvdPerPrio;
    this->scheduledBytesPerPrio = other.scheduledBytesPerPrio;
    this->unschedToRecvPerPrio = other.unschedToRecvPerPrio;
    this->sumInflightUnschedPerPrio = other.sumInflightUnschedPerPrio;
    this->sumInflightSchedPerPrio = other.sumInflightSchedPerPrio;
}