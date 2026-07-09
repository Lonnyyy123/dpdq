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
 * @file homa-send-controller.cc
 * @brief This file is heavily inspired / copies https://github.com/PlatformLab/HomaSimulation/tree/omnet_simulations/RpcTransportDesign/OMNeT%2B%2BSimulation
 */

#include "homa-transport.h"
#include "simple-log.h"
#include "unsched-byte-allocator.h"
#include "homa-udp.h"

/**
 * Predicate operator for comparing OutboundMessages. This function
 * by design return strict ordering on messages meaning that no two
 * distinct message ever compare equal. (ie result of !(a,b)&&!(b,a)
 * is always false)
 *
 * \param msg1
 *      outbound message 1 to be compared.
 * \param msg2
 *      outbound message 2 to be compared.
 * \return
 *      true if msg1 is compared greater than msg2 (ie. pkts of msg1
 *      have higher priority for transmission than pkts of msg2)
 */
bool HomaTransport::SendController::OutbndMsgSorter::operator()(
    OutboundMessage *msg1, OutboundMessage *msg2)
{

    switch (msg1->homaConfig->getSenderScheme())
    {
    case HomaConfigDepot::SenderScheme::OBSERVE_PKT_PRIOS:
    {
        auto &q1 = msg1->getTxPktQueue();
        auto &q2 = msg2->getTxPktQueue();
        if (q2.empty())
        {
            if (q1.empty())
            {
                return (msg1->getMsgCreationTime() <
                        msg2->getMsgCreationTime()) ||
                       (msg1->getMsgCreationTime() ==
                            msg2->getMsgCreationTime() &&
                        msg1->getMsgId() < msg2->getMsgId());
            }
            else
            {
                return true;
            }
        }
        else if (q1.empty())
        {
            return false;
        }
        else
        {
            hdr_homa *pkt1 = q1.top();
            hdr_homa *pkt2 = q2.top();
            return (pkt1->priority_var() < pkt2->priority_var()) ||
                   (pkt1->priority_var() == pkt2->priority_var() &&
                    pkt1->getCreationTime() < pkt2->getCreationTime()) ||
                   (pkt1->priority_var() == pkt2->priority_var() &&
                    pkt1->getCreationTime() == pkt2->getCreationTime() &&
                    msg1->getMsgCreationTime() <
                        msg2->getMsgCreationTime()) ||
                   (pkt1->priority_var() == pkt2->priority_var() &&
                    pkt1->getCreationTime() == pkt2->getCreationTime() &&
                    msg1->getMsgCreationTime() ==
                        msg2->getMsgCreationTime() &&
                    msg1->getMsgId() < msg2->getMsgId());
        }
    }
    case HomaConfigDepot::SenderScheme::SRBF:
        return msg1->getBytesLeft() < msg2->getBytesLeft() ||
               (msg1->getBytesLeft() == msg2->getBytesLeft() &&
                msg1->getMsgCreationTime() < msg2->getMsgCreationTime()) ||
               (msg1->getBytesLeft() == msg2->getBytesLeft() &&
                msg1->getMsgCreationTime() == msg2->getMsgCreationTime() &&
                msg1->getMsgId() < msg2->getMsgId());

    default:
        throw std::runtime_error("Undefined SenderScheme parameter");
    }
}

/**
 * Constructor for HomaTransport::SendController.
 *
 * \param transport
 *      Back pointer to the HomaTransport instance that owns this
 *      SendController.
 */
HomaTransport::SendController::SendController(HomaTransport *transport)
    : bytesLeftToSend(0),
      bytesNeedGrant(0),
      msgId(0),
      sentPkt(),
      sentPktDuration(SIMTIME_ZERO),
      outboundMsgMap(),
      rxAddrMsgMap(),
      unschedByteAllocator(NULL),
      prioResolver(NULL),
      outbndMsgSet(),
      transport(transport),
      homaConfig(NULL),
      activePeriodStart(SIMTIME_ZERO),
      sentBytesPerActivePeriod(0),
      sumGrantsInGap(0)
{
}

/**
 * Initializing the SendController after the simulation network is setup. This
 * function is to be called by HomaTransport::intialize() method.
 *
 * \param homaConfig
 *      Homa config container that keeps user specified parameters for the
 *      transport.
 * \param prioResolver
 *      This class is used to assign priorities of unscheduled packets this
 *      sender needs to send.
 */
void HomaTransport::SendController::initSendController(HomaConfigDepot *homaConfig,
                                                       PriorityResolver *prioResolver)
{
    this->homaConfig = homaConfig;
    this->prioResolver = prioResolver;
    bytesLeftToSend = 0;
    bytesNeedGrant = 0;
    std::random_device rd;
    std::mt19937_64 merceneRand(rd());
    std::uniform_int_distribution<uint64_t> dist(0, UINTMAX_MAX);
    msgId = dist(merceneRand);
    unschedByteAllocator = new UnschedByteAllocator(homaConfig);
}

/**
 * Destructor of the HomaTransport::SendController.
 */
HomaTransport::SendController::~SendController()
{
    slog::log4(transport->get_debug(), transport->get_local_addr(), "SendController::~SendController()");
    delete unschedByteAllocator;
    while (!outGrantQueue.empty())
    {
        hdr_homa *head = outGrantQueue.top();
        outGrantQueue.pop();
        // if (head->unschedFields_var().prioUnschedBytes)
        //     delete head->unschedFields_var().prioUnschedBytes;
        delete head;
    }
}

/**
 * For every new grant that arrives from the receiver, this method is called to
 * handle that grant (ie. send scheduled packet for that grant).
 *
 * \param rxPkt
 *      receiverd grant packet from the network.
 */
void HomaTransport::SendController::processReceivedGrant(hdr_homa *rxPkt)
{
    slog::log6(transport->get_debug(), transport->get_local_addr(), "SendController::processReceivedGrant(). Grant q len", outGrantQueue.size());
    slog::log6(transport->get_debug(), transport->get_local_addr(), "Received a grant from",
               rxPkt->getSrcAddr(), "for  msgId", rxPkt->getMsgId(), "for", rxPkt->getGrantFields().grantBytes,
               "Bytes");

    sumGrantsInGap += hdr_homa::getBytesOnWire(rxPkt->getDataBytes(),
                                               (PktType)rxPkt->getPktType());

    OutboundMessage *outboundMsg = &(outboundMsgMap.at(rxPkt->getMsgId()));
    assert(rxPkt->getPktType() == PktType::GRANT &&
           rxPkt->destAddr_var() == outboundMsg->srcAddr &&
           rxPkt->getSrcAddr() == outboundMsg->destAddr);
    size_t numRemoved = outbndMsgSet.erase(outboundMsg);
    assert(numRemoved <= 1); // At most one item removed

    int bytesToSchedOld = outboundMsg->bytesToSched;
    int bytesToSchedNew = outboundMsg->prepareSchedPkt(
        rxPkt->getGrantFields().offset, rxPkt->getGrantFields().grantBytes,
        rxPkt->getGrantFields().schedPrio);
    int numBytesSent = bytesToSchedOld - bytesToSchedNew;
    bytesNeedGrant -= numBytesSent;
    assert(numBytesSent > 0 && bytesLeftToSend >= 0 && bytesNeedGrant >= 0);
    auto insResult = outbndMsgSet.insert(outboundMsg);
    assert(insResult.second); // check insertion took place
    // delete rxPkt; // done by caller (frees packet as a whole)
    sendOrQueue();
}

/**
 * This method is called after a packet is fully serialized onto the network
 * from sender's NIC. It is the place to perform the logics related to
 * the packet serialization end.
 */
void HomaTransport::SendController::handlePktTransmitEnd()
{
    slog::log6(transport->get_debug(), transport->get_local_addr(), "SendController::handlePktTransmitEnd()");
    // When there are more than one packet ready for transmission and the sender
    // chooses one for transmission over the others, the sender has imposed a
    // delay in transmitting the other packets. Here we collect the statistics
    // related those trasmssion delays.
    int32_t lastDestAddr = sentPkt.destAddr_var();
    for (auto it = rxAddrMsgMap.begin(); it != rxAddrMsgMap.end(); ++it)
    {
        int32_t rxAddr = it->first;
        if (rxAddr != lastDestAddr)
        {
            auto allMsgToRxAddr = it->second;
            uint64_t totalTrailingUnsched = 0;
            double oldestSchedPkt = Scheduler::instance().clock();
            double currentTime = Scheduler::instance().clock();
            for (auto itt = allMsgToRxAddr.begin();
                 itt != allMsgToRxAddr.end(); ++itt)
            {
                OutboundMessage *msg = *itt;
                if (msg->msgSize > msg->bytesLeft)
                {
                    // Unsched delay at sender may waste rx bw only if msg has
                    // already sent at least one request packet.
                    totalTrailingUnsched += msg->unschedBytesLeft;
                }
                auto &schedPkts = msg->getTxSchedPkts();
                // Find oldest sched pkt creation time
                for (auto ittt = schedPkts.begin(); ittt != schedPkts.end();
                     ++ittt)
                {
                    double pktCreationTime = (*ittt)->getCreationTime();
                    oldestSchedPkt = (pktCreationTime < oldestSchedPkt
                                          ? pktCreationTime
                                          : oldestSchedPkt);
                }
            }

            double transmitDelay;
            if (totalTrailingUnsched > 0)
            {
                double totalUnschedTxTimeLeft = (hdr_homa::getBytesOnWire(totalTrailingUnsched,
                                                                          PktType::UNSCHED_DATA) *
                                                 8.0 / homaConfig->nicLinkSpeed) /
                                                1000.0 / 1000.0 / 1000.0;
                transmitDelay = sentPktDuration < totalUnschedTxTimeLeft
                                    ? sentPktDuration
                                    : totalUnschedTxTimeLeft;
                // transport->emit(sxUnschedPktDelaySignal, transmitDelay);
            }

            if (oldestSchedPkt != currentTime)
            {
                transmitDelay =
                    sentPktDuration < currentTime - oldestSchedPkt
                        ? sentPktDuration
                        : currentTime - oldestSchedPkt;
                // transport->emit(sxSchedPktDelaySignal, transmitDelay);
            }
        }
    }
}

/**
 * This method is the single interface for transmitting packets. It is called by
 * the Transport::ReceiveScheduler to send grant packets or by the
 * HomaTransport::SendController when a new data packet is ready for
 * transmission. If the NIC tx link is idle, this method sends a packet out.
 * Otherwise, to avoid buffer build up in the NIC queue, it will wait until
 * until the NIC link goes idle. This method always prioritize sending grants
 * over the data packets.
 *
 * \param msg
 *      Any of the three cases: 1. a grant packet to be transmitted out to the
 *      network. 2. The send timer that signals the NIC tx link has gone idle
 *      and we should send next ready packet.  3. NULL if a data packet has
 *      become ready and buffered in the HomaTransport::SendController queue and
 *      we want to check if we can send the packet immediately in case tx link
 *      is idle.
 */
void HomaTransport::SendController::sendOrQueue(Event *e)
{
    slog::log6(transport->get_debug(), transport->get_local_addr(), "SendController::sendOrQueue(). e is nullptr?", e == nullptr);
    HomaEvent *msg = nullptr;
    if (e != nullptr)
    {
        msg = dynamic_cast<HomaEvent *>(e);
        assert(msg != nullptr);
    }

    hdr_homa *sxPkt = nullptr;
    if (msg && msg->event_type_ == HomaEventType::SEND_TIMER_EVENT)
    {
        slog::log6(transport->get_debug(), transport->get_local_addr(), "msg->event_type_ == HomaEventType::SEND_TIMER_EVENT");
        // Send timer fired and it's time to check if we can send a data packet.
        if (!outGrantQueue.empty())
        {
            // send queued grant packets if there is any.
            slog::log6(transport->get_debug(), transport->get_local_addr(), "!outGrantQueue.empty()");
            sxPkt = outGrantQueue.top();
            outGrantQueue.pop();
            slog::log6(transport->get_debug(), transport->get_local_addr(),
                       "poped grant of", sxPkt->getMsgId(), "len", outGrantQueue.size());
            sendPktAndScheduleNext(sxPkt);
            delete sxPkt;
            return;
        }

        if (!outbndMsgSet.empty())
        {
            slog::log6(transport->get_debug(), transport->get_local_addr(), "!outbndMsgSet.empty()");
            OutboundMessage *highPrioMsg = *(outbndMsgSet.begin());
            size_t numRemoved = outbndMsgSet.erase(highPrioMsg);
            assert(numRemoved == 1); // check only one msg is removed
            bool hasMoreReadyPkt = highPrioMsg->getTransmitReadyPkt(&sxPkt);
            sendPktAndScheduleNext(sxPkt);

            if (highPrioMsg->getBytesLeft() <= 0)
            {
                assert(!hasMoreReadyPkt);
                slog::log6(transport->get_debug(), transport->get_local_addr(), "!hasMoreReadyPkt");
                msgTransmitComplete(highPrioMsg);
                delete sxPkt;
                return;
            }

            if (hasMoreReadyPkt)
            {
                slog::log6(transport->get_debug(), transport->get_local_addr(), "hasMoreReadyPkt");
                auto insResult = outbndMsgSet.insert(highPrioMsg);
                assert(insResult.second); // check insertion took place
                delete sxPkt;
                return;
            }
            delete sxPkt;
        }
        return;
    }

    // When this function is called to send a grant packet.
    if (msg)
    {
        // // sxPkt = dynamic_cast<hdr_homa *>(msg);
        // Packet *pkt = dynamic_cast<Packet *>(msg);
        // assert(pkt != nullptr);
        // sxPkt = hdr_homa::access(pkt);
        // assert(sxPkt != nullptr); // not sure how to check for correctness here
        assert(msg->homa_hdr_);
        sxPkt = msg->homa_hdr_;
        if (sxPkt)
        {
            slog::log6(transport->get_debug(), transport->get_local_addr(), "sxPkt != nullptr");
            assert(sxPkt->getPktType() == PktType::GRANT);
            if (transport->sendTimer->isScheduled())
            {
                // NIC tx link is busy sending another packet
                slog::log6(transport->get_debug(), transport->get_local_addr(),
                           "Grant timer is scheduled! Grant of",
                           sxPkt->appLevelId_var(),
                           "to", sxPkt->destAddr_var(),
                           ", mesgId:",
                           sxPkt->getMsgId(),
                           "gQ", outGrantQueue.size(), "is queued!");

                outGrantQueue.push(sxPkt);
                slog::log6(transport->get_debug(), transport->get_local_addr(),
                           "pushed grant of", sxPkt->getMsgId(), "len", outGrantQueue.size());
                return;
            }
            else
            {
                assert(outGrantQueue.empty());
                slog::log6(transport->get_debug(), transport->get_local_addr(),
                           "Send! Grant of",
                           sxPkt->appLevelId_var(),
                           "to", sxPkt->destAddr_var(),
                           ", mesgId: ",
                           sxPkt->getMsgId(),
                           "gQ", outGrantQueue.size(),
                           ", prio: ",
                           sxPkt->getGrantFields().schedPrio);
                sendPktAndScheduleNext(sxPkt);
                slog::log6(transport->get_debug(), transport->get_local_addr(),
                           "pushed not grant of", sxPkt->getMsgId(), "len", outGrantQueue.size());
                delete sxPkt;
                return;
            }
        }
    }

    // When a data packet has become ready and we should check if we can send it
    // out.
    assert(!msg);
    if (transport->sendTimer->isScheduled())
    {
        slog::log6(transport->get_debug(), transport->get_local_addr(),
                   "Returning because the uplink is busy");
        return;
    }

    slog::log6(transport->get_debug(), transport->get_local_addr(), "outGrantQueue empty?", outGrantQueue.empty(), "outbndMsgSet.size =", outbndMsgSet.size());
    assert(outGrantQueue.empty() && outbndMsgSet.size() == 1);
    OutboundMessage *highPrioMsg = *(outbndMsgSet.begin());
    size_t numRemoved = outbndMsgSet.erase(highPrioMsg);
    assert(numRemoved == 1); // check that the message is removed
    bool hasMoreReadyPkt = highPrioMsg->getTransmitReadyPkt(&sxPkt);
    sendPktAndScheduleNext(sxPkt);
    if (highPrioMsg->getBytesLeft() <= 0)
    {
        assert(!hasMoreReadyPkt);
        msgTransmitComplete(highPrioMsg);
        delete sxPkt;
        return;
    }

    if (hasMoreReadyPkt)
    {
        slog::log6(transport->get_debug(), transport->get_local_addr(),
                   "hasMoreReadyPkt");
        auto insResult = outbndMsgSet.insert(highPrioMsg);
        assert(insResult.second); // check insertion took place
        delete sxPkt;
        return;
    }
    if (sxPkt)
        delete sxPkt;
}

/**
 * The actual interface for transmitting packets to the network. This method is
 * only called by the sendOrQueue() method.
 *
 * \param sxPkt
 *      packet to be transmitted.
 */
void HomaTransport::SendController::sendPktAndScheduleNext(hdr_homa *sxPkt)
{
    slog::log6(transport->get_debug(), transport->get_local_addr(), "SendController::sendPktAndScheduleNext()");
    PktType pktType = (PktType)sxPkt->getPktType();
    uint32_t numDataBytes = sxPkt->getDataBytes();
    uint32_t bytesSentOnWire = hdr_homa::getBytesOnWire(numDataBytes, pktType);
    double currentTime = Scheduler::instance().clock();
    double sxPktDuration = (bytesSentOnWire * 8.0 /
                            homaConfig->nicLinkSpeed) /
                           1000.0 / 1000.0 / 1000.0;
    // double sxPktDuration = SimTime(1e-9 * (bytesSentOnWire * 8.0 /
    //                                        homaConfig->nicLinkSpeed));

    slog::log6(transport->get_debug(), transport->get_local_addr(),
               "Calculated sxPktDuration to be:", sxPktDuration,
               "For bytes on wire:", bytesSentOnWire,
               "For numDataBytes:", numDataBytes);
    switch (pktType)
    {
    case PktType::REQUEST:
    case PktType::UNSCHED_DATA:
    case PktType::SCHED_DATA:
        assert(bytesLeftToSend >= numDataBytes);
        bytesLeftToSend -= numDataBytes;
        sentBytesPerActivePeriod += bytesSentOnWire;
        if (bytesLeftToSend == 0)
        {
            // This is end of the active period, so record stats
            double activePeriod = currentTime - activePeriodStart +
                                  sxPktDuration;
            // transport->emit(sxActiveBytesSignal, sentBytesPerActivePeriod);
            // transport->emit(sxActiveTimeSignal, activePeriod);

            // reset stats tracking variables
            sentBytesPerActivePeriod = 0;
            activePeriodStart = currentTime;
        }
        slog::log6(transport->get_debug(), transport->get_local_addr(),
                   "sending data of", sxPkt->getMsgId(), "type", pktType, "last", sxPkt->schedDataFields_var().lastByte);
        break;

    case PktType::GRANT:
        // Add the length of the grant packet to the bytes
        // sent in active period.
        if (bytesLeftToSend > 0)
        {
            // SendController is already in an active period. No need to
            // record stats.
            sentBytesPerActivePeriod += bytesSentOnWire;
        }
        // else
        // {
        // transport->emit(sxActiveBytesSignal, bytesSentOnWire);
        // transport->emit(sxActiveTimeSignal, sxPktDuration);
        // }
        slog::log6(transport->get_debug(), transport->get_local_addr(),
                   "sending grant of", sxPkt->getMsgId(), "len", outGrantQueue.size());
        break;
    default:
        throw std::runtime_error("SendController::sendPktAndScheduleNext: "
                                 "packet to send has unknown pktType" +
                                 pktType);
    }

    // double nextSendTime = sxPktDuration + currentTime;
    sentPkt = *sxPkt;
    sentPktDuration = sxPktDuration;
    // transport->socket.sendTo(sxPkt, sxPkt->getDestAddr(), homaConfig->destPort);
    // transport->scheduleAt(nextSendTime, transport->sendTimer);
    try
    {
        slog::log6(transport->get_debug(), transport->get_local_addr(),
                   "Sending packet to:", sxPkt->destAddr_var(),
                   "getDataBytes() =", sxPkt->getDataBytes());
        // for (auto &it : transport->homa_agents_)
        // {
        //     slog::log6(transport->get_debug(), transport->get_local_addr(),
        //                "key", it.first,
        //                "daddr", it.second->daddr(),
        //                "addr", it.second->addr());
        //     Agent *ag = dynamic_cast<Agent *>(it.second->target());
        //     if (it.second->target() == nullptr)
        //         std::cout << "Somthrds" << std::endl;
        // }
        transport->homa_agents_.at(sxPkt->destAddr_var())->sendmsg(sxPkt, sxPkt->getPriority());
    }
    catch (const std::exception &e)
    {
        slog::error(transport->get_debug(), transport->get_local_addr(), "Did not find homa agent for destination:", sxPkt->destAddr_var(), "\n", e.what());
        throw;
    }

    transport->sendTimer->resched(sxPktDuration);
}

/**
 * This method cleans up a message whose tranmission is complete.
 *
 * \param msg
 *      Message that's been completely transmitted.
 */
void HomaTransport::SendController::msgTransmitComplete(OutboundMessage *msg)
{
    slog::log4(transport->get_debug(), transport->get_local_addr(), "SendController::msgTransmitComplete()");
    int32_t destIp = msg->destAddr;
    rxAddrMsgMap.at(destIp).erase(msg);
    if (rxAddrMsgMap.at(destIp).empty())
    {
        rxAddrMsgMap.erase(destIp);
    }
    outboundMsgMap.erase(msg->getMsgId());
    // transport->testAndEmitStabilitySignal();
    return;
}

/**
 * This method handles new messages from the application that needs to be
 * transmitted over the network.
 *
 * KP: Note: this function is used both for requests and replies.
 *     Whether the msg is a request or a reply only interests the app layer.
 *     This information is carried in AppMessage.msgType
 * \param sendMsg
 *      Application messsage that needs to be transmitted by the transport.
 */
void HomaTransport::SendController::processSendMsgFromApp(AppMessage *sendMsg)
{
    slog::log4(transport->get_debug(), transport->get_local_addr(), "SendController::processSendMsgFromApp()");
    int32_t destAddr = sendMsg->getDestAddr();
    uint32_t msgSize = sendMsg->getByteLength();
    std::vector<uint32_t> reqUnschedDataVec =
        unschedByteAllocator->getReqUnschedDataPkts(destAddr, msgSize);
    slog::log5(transport->get_debug(), transport->get_local_addr(), "num of UnchedDataPkts=", reqUnschedDataVec.size());

    outboundMsgMap.emplace(std::piecewise_construct,
                           std::forward_as_tuple(msgId),
                           std::forward_as_tuple(sendMsg, this, msgId, reqUnschedDataVec));
    slog::log5(transport->get_debug(), transport->get_local_addr(), "num of outbound msgs=", outboundMsgMap.size());
    OutboundMessage *outboundMsg = &(outboundMsgMap.at(msgId));
    rxAddrMsgMap[destAddr].insert(outboundMsg);
    slog::log5(transport->get_debug(), transport->get_local_addr(), "num of rx destinations", rxAddrMsgMap.size());

    if (bytesLeftToSend == 0)
    {
        // We have just entered in an active transmit period.
        activePeriodStart = Scheduler::instance().clock();
        assert(sentBytesPerActivePeriod == 0);
        sentBytesPerActivePeriod = 0;
    }
    bytesLeftToSend += outboundMsg->getBytesLeft();
    slog::log5(transport->get_debug(), transport->get_local_addr(), "bytesLeftToSend", bytesLeftToSend);
    outboundMsg->prepareRequestAndUnsched();
    auto insResult = outbndMsgSet.insert(outboundMsg);
    assert(insResult.second); // check insertion took place
    slog::log5(transport->get_debug(), transport->get_local_addr(), "outbndMsgSet size", outbndMsgSet.size());
    bytesNeedGrant += outboundMsg->bytesToSched;
    msgId++;
    slog::log5(transport->get_debug(), transport->get_local_addr(), "bytesNeedGrant", bytesNeedGrant, "new msgId=", msgId);
    delete sendMsg;
    sendOrQueue();
}