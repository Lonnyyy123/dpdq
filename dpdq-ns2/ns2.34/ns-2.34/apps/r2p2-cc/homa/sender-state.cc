#include "homa-transport.h"
#include "priority-resolver.h"
#include "simple-log.h"

/**
 * @file sender-state.cc
 * @brief This file mostly copies https://github.com/PlatformLab/HomaSimulation/tree/omnet_simulations/RpcTransportDesign/OMNeT%2B%2BSimulation
 */

/**
 * Constructor of HomaTransport::ReceiveScheduler::SenderState.
 *
 * \param srcAddr
 *      Address of the sender corresponding to this SenderState.
 * \param rxScheduler
 *      The ReceiveScheduler that handles received packets for this SenderState.
 * \param grantTimer
 *      The timer that paces grants for the sender corresponding to this
 *      SenderState.
 * \param homaConfig
 *      Collection of user-specified config parameters for this transport
 */
HomaTransport::ReceiveScheduler::SenderState::SenderState(
    int32_t srcAddr, ReceiveScheduler *rxScheduler,
    HomaGrantTimer *grantTimer, HomaConfigDepot *homaConfig)
    : homaConfig(homaConfig),
      rxScheduler(rxScheduler),
      senderAddr(srcAddr),
      grantTimer(grantTimer),
      mesgsToGrant(),
      incompleteMesgs(),
      lastGrantPrio(homaConfig->allPrio),
      lastIdx(homaConfig->adaptiveSchedPrioLevels)
{
}

/**
 * Compute a lower bound for the next time that the receiver can send a grant
 * for the sender of this SenderState.
 *
 * \param currentTime
 *      Time at which the most recent grant packet is sent for the center.
 * \param grantSize
 *      Number of bytes granted in the most recent grant sent.
 */
double
HomaTransport::ReceiveScheduler::SenderState::getNextGrantTime(uint32_t grantSize)
{
    // ns-2 schedulers work with relative times, not absolute.
    uint32_t grantedPktSizeOnWire =
        hdr_homa::getBytesOnWire(grantSize, PktType::SCHED_DATA);

    double nextGrantTime = ((grantedPktSizeOnWire * 8.0 / homaConfig->nicLinkSpeed) / 1000.0 / 1000.0 / 1000.0);
    return nextGrantTime;
}

/**
 * This method handles packets received from the sender corresponding to
 * SenderState. It finds the sender's InboundMessage that this belongs to and
 * invoked InboundMessage methods to fills in the delivered packet data in the
 * message.
 *
 * \param rxPkt
 *      The data packet received from the sender.
 * \return
 *      returns a pair <scheduledMesg?, incompletMesg?>.
 *      schedMesg is True if rxPkt belongs to a scheduled message (ie. a message
 *      that needs at least one grant when the receiver first know about it).
 *
 *      schedMesg is False if rxPkt belongs to a fully unscheduled message (a
 *      message that all of its bytes are received in unscheduled packets).
 *
 *      incompleteMesg is -1 if rxPkt is the last packet of an inbound message and
 *      reception of the rxPkt completes the reception of the message.
 *
 *      incompleteMesg is 0 if scheduledMesg is True and the mesg rxPkt belongs to
 *      is not complete (ie. has more inbound inflight packets) but it doesn't
 *      any more grants.
 *
 *      incompleteMesg is a positive rank number if scheduledMesg is True and
 *      the mesg rxPkt belongs to needs more grants. In which case the positive
 *      rank number is 1 if the mesg is the most prefered mesg among all
 *      scheduled message of the sender. Otherwise, rank number is 2.
 *
 *      incompleteMesg is the positive remaining bytes of the mesg yet to be
 *      received if scheduledMesg is False.
 */
std::pair<bool, int>
HomaTransport::ReceiveScheduler::SenderState::handleInboundPkt(hdr_homa *rxPkt)
{
    std::pair<bool, int> ret;
    slog::log6(rxScheduler->transport->get_debug(), rxScheduler->transport->get_local_addr(),
               "SenderState::handleInboundPkt()");
    uint32_t pktData = rxPkt->getDataBytes();
    PktType pktType = (PktType)rxPkt->getPktType();
    InboundMessage *inboundMesg = NULL;
    auto mesgIt = incompleteMesgs.find(rxPkt->getMsgId());
    rxScheduler->addArrivedBytes(pktType, rxPkt->getPriority(), pktData);
    if (mesgIt == incompleteMesgs.end())
    {
        slog::log6(rxScheduler->transport->get_debug(), rxScheduler->transport->get_local_addr(),
                   "create new message");
        assert(rxPkt->getPktType() == PktType::REQUEST ||
               rxPkt->getPktType() == PktType::UNSCHED_DATA);
        inboundMesg = new InboundMessage(rxPkt, this->rxScheduler, homaConfig);
        inboundMesg->msgCreationTime = rxPkt->msg_creation_time_var();
        assert(inboundMesg->msgCreationTime >= 0.0);
        incompleteMesgs[rxPkt->getMsgId()] = inboundMesg;

        // There will be other unsched pkts arriving after this unsched. pkt
        // that we need to account for them.
        std::vector<uint32_t> *prioUnschedBytes =
            rxPkt->unschedFields_var().prioUnschedBytes;
        for (size_t i = 0; i < prioUnschedBytes->size(); i += 2)
        {
            uint32_t prio = (*prioUnschedBytes)[i];
            uint32_t unschedBytesInPrio = (*prioUnschedBytes)[i + 1];
            rxScheduler->addPendingUnschedBytes(pktType, prio,
                                                unschedBytesInPrio);
        }
        // rxScheduler->transport->emit(totalOutstandingBytesSignal,
        //                              rxScheduler->getInflightBytes());
        inboundMesg->updatePerPrioStats();
    }
    else
    {
        slog::log6(rxScheduler->transport->get_debug(), rxScheduler->transport->get_local_addr(),
                   "message exists, updating variables");
        inboundMesg = mesgIt->second;
        if (inboundMesg->bytesToGrant > 0)
        {
            size_t numRemoved = mesgsToGrant.erase(inboundMesg);
            assert(numRemoved == 1);
        }
        // this is a packet that we expected to be received and has previously
        // accounted for in inflightBytes.
        rxScheduler->pendingBytesArrived(pktType, rxPkt->getPriority(),
                                         pktData);
        // rxScheduler->transport->emit(
        //     totalOutstandingBytesSignal, rxScheduler->getInflightBytes());
    }

    assert(rxPkt->getMsgId() == inboundMesg->msgIdAtSender &&
           rxPkt->getSrcAddr() == senderAddr);

    // Append the unsched data to the inboundMesg and update variables
    // tracking inflight bytes and total bytes
    if (pktType == PktType::REQUEST || pktType == PktType::UNSCHED_DATA)
    {
        inboundMesg->fillinRxBytes(rxPkt->getUnschedFields().firstByte,
                                   rxPkt->getUnschedFields().lastByte, pktType);
    }
    else if (pktType == PktType::SCHED_DATA)
    {
        inboundMesg->fillinRxBytes(rxPkt->getSchedDataFields().firstByte,
                                   rxPkt->getSchedDataFields().lastByte, pktType);
    }
    else
    {
        throw std::runtime_error("PktType %d is not recongnized" +
                                 rxPkt->getPktType());
    }

    bool isMesgSched =
        (inboundMesg->msgSize - inboundMesg->totalUnschedBytes > 0);
    ret.first = isMesgSched;
    if (!isMesgSched)
    {
        assert(inboundMesg->msgSize == inboundMesg->totalUnschedBytes);
    }

    if (inboundMesg->bytesToReceive == 0)
    {
        assert(inboundMesg->bytesToGrant == 0);
        assert(inboundMesg->inflightGrants.empty());

        // this message is complete, so send it to the application
        AppMessage *rxMsg = inboundMesg->prepareRxMsgForApp();

#if TESTING
        delete rxMsg;
#else
        // rxScheduler->transport->send(rxMsg, "appOut", 0);
        // assertion will make sure that there is only one attached application (or one client and one server)
        assert(rxScheduler->transport->thread_id_to_app_.size() <= 2);
        assert(rxPkt->msgType_var() != HomaMsgType::UNDEF_HOMA_MSG_TYPE);
        assert(rxMsg->getMsgType() != HomaMsgType::UNDEF_HOMA_MSG_TYPE);
        for (auto it = rxScheduler->transport->thread_id_to_app_.begin(); // should only loop once
             it != rxScheduler->transport->thread_id_to_app_.end(); ++it)
        {
            if (rxMsg->getMsgType() == HomaMsgType::REQUEST_MSG)
            {
                // thread_id == 0
                // don't give request to the client app
                // This is bad because it couples the front and back ends in unexpected ways
                if (it->first != 0)
                    continue;
                slog::log6(rxScheduler->transport->get_debug(), rxScheduler->transport->get_local_addr(),
                           "Forwarding complete REQUEST to application",
                           "size(?) =", rxMsg->getByteLength(),
                           "src (client):", rxMsg->getSrcAddr(),
                           "dst (server):", rxMsg->getDestAddr(),
                           "req_id:", rxMsg->getReqId(),
                           "app_lvl_id:", rxMsg->getAppLevelId(),
                           "client thread:", rxMsg->getClientThreadId(),
                           "server thread:", rxMsg->getServerThreadId(),
                           "bytes on wire:", rxMsg->getMsgBytesOnWire(),
                           "Creation time:", rxMsg->getMsgCreationTime());
                it->second->req_recv(rxMsg->getByteLength(),
                                     RequestIdTuple(rxMsg->getReqId(),
                                                    rxMsg->getAppLevelId(),
                                                    rxMsg->getSrcAddr(),
                                                    rxMsg->getDestAddr(),
                                                    rxMsg->getClientThreadId(),
                                                    rxMsg->getServerThreadId(),
                                                    rxMsg->getMsgCreationTime()));
            }
            else if (rxMsg->getMsgType() == HomaMsgType::REPLY_MSG)
            {
                // don't give request to the server app
                if (it->first == 0)
                    continue;
                slog::log6(rxScheduler->transport->get_debug(), rxScheduler->transport->get_local_addr(),
                           "Forwarding complete REPLY to application",
                           "size(?) =", rxMsg->getByteLength(),
                           "src (client):", rxMsg->getSrcAddr(),
                           "dst (server):", rxMsg->getDestAddr(),
                           "req_id:", rxMsg->getReqId(),
                           "app_lvl_id:", rxMsg->getAppLevelId(),
                           "client thread:", rxMsg->getClientThreadId(),
                           "server thread:", rxMsg->getServerThreadId(),
                           "bytes on wire:", rxMsg->getMsgBytesOnWire(),
                           "Creation time:", rxMsg->getMsgCreationTime());
                it->second->req_success(rxMsg->getByteLength(),
                                        RequestIdTuple(rxMsg->getReqId(),
                                                       rxMsg->getAppLevelId(),
                                                       rxMsg->getSrcAddr(),
                                                       rxMsg->getDestAddr(),
                                                       rxMsg->getClientThreadId(),
                                                       rxMsg->getServerThreadId(),
                                                       Scheduler::instance().clock()));
            }
            else
            {
                throw std::runtime_error("Tried to forward invalid message type to application layer. Type: " + rxMsg->getMsgType());
            }
        }

#endif

        // remove this message from the incompleteRxMsgs
        incompleteMesgs.erase(inboundMesg->msgIdAtSender);
        delete inboundMesg;
        delete rxMsg;
        ret.second = -1;
    }
    else if (inboundMesg->bytesToGrant == 0)
    {
        // no grants needed for this mesg but mesg is not complete yet and we
        // need to keep the message aroudn until all of its packets are arrived.
        if (isMesgSched)
        {
            ret.second = 0;
        }
        else
        {
            ret.second = inboundMesg->bytesToReceive;
        }
    }
    else
    {
        auto resPair = mesgsToGrant.insert(inboundMesg);
        assert(resPair.second);
        ret.second = (resPair.first == mesgsToGrant.begin()) ? 1 : 2;
    }
    return ret;
}

/**
 * ReceiveScheduler calls this method to send a grant for a sender's highest
 * priority message of this receiver. As a side effect, this method also
 * schedules the next grant for that sender at one grant serialization time
 * later in the future. This time is a lower bound for next grant time and
 * ReceiveScheduler will decide if next grant should be sent at that time or
 * not.
 *
 * \param grantPrio
 *      Priority of the scheduled packet that sender will send for this grant.
 * \return
 *      1) Return 0 if a grant cannot be sent (ie. the outstanding bytes for
 *      this sender are more than one RTTBytes or a grant timer is scheduled for
 *      the future.). 2) Sends a grant and returns remaining bytes to grant for
 *      the most preferred message of the sender. 3) Sends a grant and returns
 *      -1 if the sender has no more outstanding messages that need grants.
 */
int HomaTransport::ReceiveScheduler::SenderState::sendAndScheduleGrant(
    uint32_t grantPrio)
{
    double currentTime = Scheduler::instance().clock();
    auto topMesgIt = mesgsToGrant.begin();
    assert(topMesgIt != mesgsToGrant.end());
    InboundMessage *topMesg = *topMesgIt;
    assert(topMesg->bytesToGrant > 0);

    slog::log6(rxScheduler->transport->get_debug(), rxScheduler->transport->get_local_addr(),
               "init topMsg", topMesg->msgIdAtSender, "to", topMesg->destAddr,
               "mesgsToGrant", mesgsToGrant.size(), "inflight", topMesg->totalBytesInFlight, "togrant", topMesg->bytesToGrant);
    // if prioresolver gives a better prio, use that one
    uint32_t resolverPrio =
        rxScheduler->transport->prioResolver->getSchedPktPrio(topMesg);
    grantPrio = std::min(grantPrio, (uint32_t)resolverPrio);
    slog::log6(rxScheduler->transport->get_debug(), rxScheduler->transport->get_local_addr(),
               "resolver topMsg", topMesg->msgIdAtSender, "to", topMesg->destAddr,
               "mesgsToGrant", mesgsToGrant.size(), "inflight", topMesg->totalBytesInFlight, "togrant", topMesg->bytesToGrant);
    if (grantTimer->isScheduled())
    {
        slog::log6(rxScheduler->transport->get_debug(), rxScheduler->transport->get_local_addr(),
                   "grantTimer->isScheduled()");
        if (lastGrantPrio <= grantPrio)
        {
            slog::log6(rxScheduler->transport->get_debug(), rxScheduler->transport->get_local_addr(),
                       "lastGrantPrio <= grantPrio");
            return 0;
        }
        // rxScheduler->transport->cancelEvent(grantTimer);
        grantTimer->cancel_if_pending();
    }

    if (topMesg->totalBytesInFlight >= homaConfig->maxOutstandingRecvBytes)
    {
        slog::log6(rxScheduler->transport->get_debug(), rxScheduler->transport->get_local_addr(),
                   "totalBytesInFlight >= homaConfig->maxOutstandingRecvBytes");
        return 0;
    }
    slog::log6(rxScheduler->transport->get_debug(), rxScheduler->transport->get_local_addr(),
               "final topMsg", topMesg->msgIdAtSender, "to", topMesg->destAddr,
               "mesgsToGrant", mesgsToGrant.size(), "inflight", topMesg->totalBytesInFlight, "togrant", topMesg->bytesToGrant);

    uint32_t grantSize =
        std::min(topMesg->bytesToGrant, homaConfig->grantMaxBytes);
    hdr_homa *grantPkt = topMesg->prepareGrant(grantSize, grantPrio);
    lastGrantPrio = grantPrio;
    slog::log6(rxScheduler->transport->get_debug(), rxScheduler->transport->get_local_addr(),
               "SenderState::sendAndScheduleGrant() of", grantPkt->getMsgId(), "to", grantPkt->destAddr_var(),
               "mesgsToGrant", mesgsToGrant.size());
    // update stats and send a grant
    grantSize = grantPkt->getGrantFields().grantBytes;
    uint32_t prio = grantPkt->getGrantFields().schedPrio;
    rxScheduler->addPendingGrantedBytes(prio, grantSize);
    topMesg->updatePerPrioStats();
    HomaEvent event = HomaEvent(HomaEventType::HOMA_HEADER);
    event.homa_hdr_ = grantPkt;
    rxScheduler->transport->sxController.sendOrQueue(&event);
    // rxScheduler->transport->emit(outstandingGrantBytesSignal,
    //                              rxScheduler->transport->outstandingGrantBytes);
    if (topMesg->bytesToGrant > 0)
    {
        if (topMesg->totalBytesInFlight < homaConfig->maxOutstandingRecvBytes)
        {
            // rxScheduler->transport->scheduleAt(
            //     getNextGrantTime(currentTime, grantSize), grantTimer);
            grantTimer->resched(getNextGrantTime(grantSize));
        }
        return topMesg->bytesToGrant;
    }
    mesgsToGrant.erase(topMesgIt);
    if (mesgsToGrant.empty())
    {
        return -1;
    }
    topMesgIt = mesgsToGrant.begin();
    assert(topMesgIt != mesgsToGrant.end());
    InboundMessage *newTopMesg = *topMesgIt;
    assert(newTopMesg->bytesToGrant > 0);

    if (newTopMesg->totalBytesInFlight < homaConfig->maxOutstandingRecvBytes)
    {
        // rxScheduler->transport->scheduleAt(
        //     getNextGrantTime(simTime(), grantSize), grantTimer);
        grantTimer->resched(getNextGrantTime(grantSize));
    }
    return newTopMesg->bytesToGrant;
}