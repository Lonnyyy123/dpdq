#include "homa-transport.h"
#include "simple-log.h"

/**
 * Constructor for HomaTransport::ReceiveScheduler.
 *
 * \param transport
 *      back pointer to the transport that owns the ReceiveScheduler.
 */
HomaTransport::ReceiveScheduler::ReceiveScheduler(HomaTransport *transport)
    : transport(transport),
      homaConfig(NULL),
      unschRateComp(NULL),
      ipSendersMap(),
      grantTimersMap(),
      schedSenders(NULL),
      schedBwUtilTimer(NULL),
      bwCheckInterval(0),
      inflightUnschedPerPrio(),
      inflightSchedPerPrio(),
      inflightSchedBytes(0),
      inflightUnschedBytes(0),
      bytesRecvdPerPrio(),
      scheduledBytesPerPrio(),
      unschedToRecvPerPrio(),
      allBytesRecvd(0),
      unschedBytesToRecv(0),
      activePeriodStart(SIMTIME_ZERO),
      rcvdBytesPerActivePeriod(0),
      oversubPeriodStart(SIMTIME_ZERO),
      oversubPeriodStop(SIMTIME_ZERO),
      inOversubPeriod(false),
      rcvdBytesPerOversubPeriod(0),
      numActiveScheds(0),
      grantTimerId(0),
      schedChangeTime(Scheduler::instance().clock()),
      lastRecvTime(SIMTIME_ZERO)

{
    slog::log2(2, transport->get_local_addr(), "ReceiveScheduler::ReceiveScheduler()");
}

/**
 * This method is to perform setup steps ReceiveScheduler that are required to
 * be done after the simulation network is setup. This should be called from
 * the HomaTransport::initialize() function.
 *
 * \param homaConfig
 *      Collection of all user specified parameters for the transport.
 * \param prioResolver
 *      Transport instance of PriorityResolver that is used for determining
 *      scheduled packet priority that is specified in the grants that this
 *      ReceiveScheduler sends.
 */
void HomaTransport::ReceiveScheduler::initialize(HomaConfigDepot *homaConfig,
                                                 PriorityResolver *prioResolver)
{
    slog::log2(homaConfig->debug_, transport->get_local_addr(), "ReceiveScheduler::initialize()");
    this->homaConfig = homaConfig;
    this->unschRateComp = new UnschedRateComputer(homaConfig,
                                                  homaConfig->useUnschRateInScheduler, 0.1);
    this->schedSenders = new SchedSenders(homaConfig, transport, this);
    inflightUnschedPerPrio.resize(this->homaConfig->allPrio);
    inflightSchedPerPrio.resize(this->homaConfig->allPrio);
    bytesRecvdPerPrio.resize(homaConfig->allPrio); // initialize bytes per prio counter
    scheduledBytesPerPrio.resize(homaConfig->allPrio);
    unschedToRecvPerPrio.resize(homaConfig->allPrio);
    std::fill(inflightUnschedPerPrio.begin(), inflightUnschedPerPrio.end(), 0);
    std::fill(inflightSchedPerPrio.begin(), inflightSchedPerPrio.end(), 0);
    std::fill(bytesRecvdPerPrio.begin(), bytesRecvdPerPrio.end(), 0);
    std::fill(scheduledBytesPerPrio.begin(), scheduledBytesPerPrio.end(), 0);
    std::fill(unschedToRecvPerPrio.begin(), unschedToRecvPerPrio.end(), 0);
    schedBwUtilTimer = new HomaSchedBwUtilTimer(transport, new HomaEvent(HomaEventType::BW_UTIL_EVENT));
    // schedBwUtilTimer->setKind(SelfMsgKind::BW_CHECK); // not needed
    if (homaConfig->linkCheckBytes < 0)
    {
        // bwCheckInterval = SimTime::getMaxTime();
        bwCheckInterval = MAXTIME;
    }
    else
    {
        bwCheckInterval = (homaConfig->linkCheckBytes * 8.0 /
                           (homaConfig->nicLinkSpeed * 1e9));
    }
}

/**
 * ReceiveScheduler destructor.
 */
HomaTransport::ReceiveScheduler::~ReceiveScheduler()
{
    slog::log2(homaConfig->debug_, transport->get_local_addr(), "ReceiveScheduler::~ReceiveScheduler()");
    // Iterate through all incomplete messages and delete them
    for (auto it = grantTimersMap.begin(); it != grantTimersMap.end(); it++)
    {
        HomaGrantTimer *grantTimer = it->first;
        // cancelAndDelete calls cancelEvent() first (which only does somthing if the timer is pending)
        // transport->cancelAndDelete(grantTimer);
        grantTimer->cancel_if_pending();
        delete grantTimer;
    }

    for (auto it = ipSendersMap.begin(); it != ipSendersMap.end(); it++)
    {
        SenderState *s = it->second;
        for (auto itt = s->incompleteMesgs.begin();
             itt != s->incompleteMesgs.end(); ++itt)
        {
            InboundMessage *mesg = itt->second;
            delete mesg;
        }
        delete s;
    }
    delete unschRateComp;
    delete schedSenders;
    schedBwUtilTimer->cancel_if_pending();
    delete schedBwUtilTimer;
}

/**
 * Given a data packet just received from the network, this method finds the
 * associated receiving message that this packet belongs to.
 *
 * \param rxPkt
 *      Received data packet for which this function finds the corresponding
 *      inbound message.
 * \return
 *      The inbound message that rxPkt is sent for from the sender or NULL if
 *      no such InboundMessage exists (ie. this is the first received packet of
 *      the message.)
 */
HomaTransport::InboundMessage *
HomaTransport::ReceiveScheduler::lookupInboundMesg(hdr_homa *rxPkt) const
{
    slog::log6(homaConfig->debug_, transport->get_local_addr(), "ReceiveScheduler::lookupInboundMesg()");
    // Get the SenderState collection for the sender of this pkt
    // inet::IPv4Address srcIp = rxPkt->getSrcAddr().toIPv4();
    int32_t srcIp = rxPkt->getSrcAddr();
    auto iter = ipSendersMap.find(srcIp);
    if (iter == ipSendersMap.end())
    {
        return NULL;
    }
    SenderState *s = iter->second;

    // Find inboundMesg in s
    auto mesgIt = s->incompleteMesgs.find(rxPkt->getMsgId());
    if (mesgIt == s->incompleteMesgs.end())
    {
        return NULL;
    }
    return mesgIt->second;
}

/**
 * This method is to handle data packets arriving at the receiver's transport.
 *
 * \param rxPkts
 *      Received data packet (ie. REQUEST, UNSCHED_DATA, or SCHED_DATA).
 */
void HomaTransport::ReceiveScheduler::processReceivedPkt(hdr_homa *rxPkt)
{
    slog::log6(homaConfig->debug_, transport->get_local_addr(), "ReceiveScheduler::processReceivedPkt()");
    uint32_t pktLenOnWire = hdr_homa::getBytesOnWire(rxPkt->getDataBytes(),
                                                     (PktType)rxPkt->getPktType());
    double pktDuration =
        (pktLenOnWire * 8.0 / homaConfig->nicLinkSpeed) / 1000.0 / 1000.0 / 1000.0;
    slog::log6(homaConfig->debug_, transport->get_local_addr(), "pktLenOnWire:", pktLenOnWire, "pktDuration:", pktDuration);
    double timeNow = Scheduler::instance().clock();

    // Check if receiver bw is wasted may have been as a result receiver not
    // sending enough timely grants to some sched messages
    double sinceLastPkt = Scheduler::instance().clock() - lastRecvTime;

    // SimTime::getScaleExp(); Returns the scale exponent, which is an integer in the range -18..0.
    //  * For example, for microsecond resolution it returns -6.
    // assert(sinceLastPkt - pktDuration >= -pow(10, SimTime::getScaleExp()));
    assert(sinceLastPkt - pktDuration >= -pow(10, 0)); // check that the packet was received in time that is more than the minimum possible
    uint64_t *sumGrantsInGap = &(transport->sxController.sumGrantsInGap);
    double rxGrantsTime =
        (*sumGrantsInGap) * 8.0 / homaConfig->nicLinkSpeed / 1000.0 / 1000.0 / 1000.0;
    slog::log6(homaConfig->debug_, transport->get_local_addr(), "sumGrantsInGap:", *sumGrantsInGap, "rxGrantsTime:", rxGrantsTime);
    if (sinceLastPkt > (pktDuration + rxGrantsTime) &&
        schedSenders->numSenders)
    {
        // find the earliest grant time
        double maxEarliestGrant = SIMTIME_ZERO;
        for (SenderState *sxState : schedSenders->senders)
        {
            if (!sxState || sxState->mesgsToGrant.empty())
                continue;
            InboundMessage *topMesg = *(sxState->mesgsToGrant.begin());
            if (topMesg->inflightGrants.empty())
            {
                maxEarliestGrant = Scheduler::instance().clock();
                break;
            }
            double earliestGrant =
                std::get<2>(*topMesg->inflightGrants.begin());
            maxEarliestGrant = std::max(maxEarliestGrant, earliestGrant);
        }

        // Check if rxScheduler could have avoided the bw waste by sending
        // erlier grants to some messages. Two values of higher and lower
        // overestimate for self inflicted receiver wasted bw will be reported.
        double highOverEst;
        double lowrOverEst;
        double wastedGap = sinceLastPkt - pktDuration;

        if (maxEarliestGrant <= lastRecvTime - homaConfig->rtt)
        {
            highOverEst = SIMTIME_ZERO;
            lowrOverEst = SIMTIME_ZERO;
        }
        else if (maxEarliestGrant <= lastRecvTime)
        {
            highOverEst = std::min(wastedGap,
                                   maxEarliestGrant + homaConfig->rtt - lastRecvTime);
            lowrOverEst = highOverEst;
        }
        else
        {
            highOverEst = wastedGap;
            lowrOverEst = std::min(
                maxEarliestGrant - lastRecvTime + homaConfig->rtt, wastedGap);
        }
        assert(rxGrantsTime <= wastedGap);
        // prorate the grants received in the wastedGap
        highOverEst -= (rxGrantsTime / wastedGap * highOverEst);
        lowrOverEst -= (rxGrantsTime / wastedGap * lowrOverEst);
        // transport->emit(higherRxSelfWasteSignal, highOverEst);
        // transport->emit(lowerRxSelfWasteSignal, lowrOverEst);
    }
    lastRecvTime = Scheduler::instance().clock();
    *sumGrantsInGap = 0;

    // check and update states for oversubscription time and bytes.
    if (schedSenders->numSenders > schedSenders->numToGrant)
    {
        // Already in an oversubcription period
        assert(inOversubPeriod && oversubPeriodStop == MAXTIME &&
               oversubPeriodStart < timeNow - pktDuration);
        rcvdBytesPerOversubPeriod += pktLenOnWire;
    }
    else if (oversubPeriodStop != MAXTIME)
    {
        // an oversubscription period has recently been marked ended and we need
        // to emit signal to record the stats.
        assert(!inOversubPeriod && oversubPeriodStop <= timeNow);
        double oversubDuration = oversubPeriodStop - oversubPeriodStart;
        double oversubOffset = oversubPeriodStop - timeNow + pktDuration;
        if (oversubOffset > 0)
        {
            oversubDuration += oversubOffset;
            rcvdBytesPerOversubPeriod += (uint64_t)(oversubOffset * homaConfig->nicLinkSpeed * 1e9 / 8);
        }
        // transport->emit(oversubTimeSignal, oversubDuration);
        // transport->emit(oversubBytesSignal, rcvdBytesPerOversubPeriod);

        // reset the states and variables
        rcvdBytesPerOversubPeriod = 0;
        oversubPeriodStart = MAXTIME;
        oversubPeriodStop = MAXTIME;
        inOversubPeriod = false;
    }

    // Get the SenderState collection for the sender of this pkt
    int32_t srcIp = rxPkt->getSrcAddr();
    auto iter = ipSendersMap.find(srcIp);
    SenderState *s;
    if (iter == ipSendersMap.end())
    {
        // Construct new SenderState if pkt arrived from new sender
        char timerName[50];
        sprintf(timerName, "senderIP%d", srcIp);
        // cMessage *grantTimer = new cMessage(timerName);
        HomaGrantTimer *grantTimer = new HomaGrantTimer(transport, numActiveScheds++, new HomaEvent(HomaEventType::GRANT_TIMER_EVENT));
        // grantTimer->setKind(SelfMsgKind::GRANT); // not needed
        s = new SenderState(srcIp, this, grantTimer, homaConfig);
        grantTimersMap[grantTimer] = s;
        ipSendersMap[srcIp] = s;
    }
    else
    {
        s = iter->second;
    }

    // At each new pkt arrival, the order of senders in the schedSenders list
    // can change. So, remove s from the schedSender, create a SchedState for s,
    // handle the packet.
    SchedSenders::SchedState old;
    old.setVar(schedSenders->numToGrant, schedSenders->headIdx,
               schedSenders->numSenders, s, 0);
    int sIndOld = schedSenders->remove(s);
    old.sInd = sIndOld;
    slog::log6(homaConfig->debug_, transport->get_local_addr(),
               "\n#################New packet arrived################\n\n");

    // process received packet
    auto msgCompHandle = s->handleInboundPkt(rxPkt);

    // Handling a received packet can change the state of the scheduled senders.
    // We need to check for message completion and see if new grant should be
    // transmitted.
    SchedSenders::SchedState cur;
    auto ret = schedSenders->insPoint(s);
    cur.setVar(schedSenders->numToGrant, std::get<1>(ret), std::get<2>(ret),
               s, std::get<0>(ret));
    slog::log6(homaConfig->debug_, transport->get_local_addr(), ">Num sched senders:",
               schedSenders->numSenders, "Num active senders:", schedSenders->numActiveSenders(),
               "numToGrant: ", schedSenders->numToGrant);
    slog::log6(homaConfig->debug_, transport->get_local_addr(),
               ">Pkt arrived, SchedState before handling pkt:\n\t", old);
    slog::log6(homaConfig->debug_, transport->get_local_addr(),
               ">SchedState after handling pkt:\n\t", cur);

    schedSenders->handleMesgRecvCompletionEvent(msgCompHandle, old, cur);

    slog::log6(homaConfig->debug_, transport->get_local_addr(),
               "SchedState before mesgCompletHandler:\n\t", old);
    slog::log6(homaConfig->debug_, transport->get_local_addr(),
               "SchedState after mesgCompletHandler:\n\t", cur);

    schedSenders->handlePktArrivalEvent(old, cur);
    slog::log6(homaConfig->debug_, transport->get_local_addr(),
               "SchedState before handlePktArrival:\n\t", old);
    slog::log6(homaConfig->debug_, transport->get_local_addr(),
               "SchedState after handlePktArrival:\n\t", cur);

    // check and update states for oversubscription time and bytes.
    if (schedSenders->numSenders <= schedSenders->numToGrant)
    {
        if (inOversubPeriod)
        {
            // Oversubscription period ended. Emit signals and record stats.
            // transport->emit(oversubTimeSignal, timeNow - oversubPeriodStart);
            // transport->emit(oversubBytesSignal, rcvdBytesPerOversubPeriod);
        }
        // reset the states and variables
        rcvdBytesPerOversubPeriod = 0;
        inOversubPeriod = false;
        oversubPeriodStart = MAXTIME;
        oversubPeriodStop = MAXTIME;
    }
    else if (!inOversubPeriod)
    {
        // mark the start of a oversubcription period
        assert(rcvdBytesPerOversubPeriod == 0 &&
               oversubPeriodStart == MAXTIME && oversubPeriodStop == MAXTIME);
        inOversubPeriod = true;
        oversubPeriodStart = timeNow - pktDuration;
        oversubPeriodStop = MAXTIME;
        rcvdBytesPerOversubPeriod += pktLenOnWire;
    }

    // check if number of active sched senders has changed and we should emit
    // signal for activeSenders
    tryRecordActiveMesgStats(timeNow);

    // Each new data packet arrival is a hint that recieve link is being
    // utilized and we need to cancel/reset the schedBwUtilTimer. The code block
    // below does this job.
    // if (bwCheckInterval == SimTime::getMaxTime())
    if (bwCheckInterval == MAXTIME)
    {
        // bw-waste detection is disabled in the config file. No need to set
        // schedBwUtilTimer for checking wasted bandwidth.
        return;
    }

    if (!schedSenders->numSenders)
    {
        // If no sender is waiting for grants, then there's no point to
        // track sched bw utilization.
        // transport->cancelEvent(schedBwUtilTimer);
        // KP: cancelEvent first checks whether the timer is scheduled
        schedBwUtilTimer->cancel_if_pending();
        return;
    }

    if (!schedBwUtilTimer->isScheduled())
    {
        // transport->scheduleAt(bwCheckInterval + timeNow,
        //                       schedBwUtilTimer);
        schedBwUtilTimer->resched(bwCheckInterval);
        slog::log6(homaConfig->debug_, transport->get_local_addr(),
                   "Scheduled schedBwUtilTimer at ", schedBwUtilTimer->getArrivalTime());
        return;
    }
    /**
     * getArrivalTime():
     * Returns the simulation time this event object has been last scheduled for
     * (regardless whether it is currently scheduled), or zero if the event
     * hasn't been scheduled yet.
     */
    double schedTime = schedBwUtilTimer->getArrivalTime();
    // transport->cancelEvent(schedBwUtilTimer);
    schedBwUtilTimer->cancel_if_pending();
    // transport->scheduleAt(std::max(schedTime, bwCheckInterval + timeNow),
    //                       schedBwUtilTimer);
    schedBwUtilTimer->resched(std::max(schedTime, bwCheckInterval + timeNow) - timeNow);
    slog::log6(homaConfig->debug_, transport->get_local_addr(),
               "Scheduled schedBwUtilTimer at ", schedBwUtilTimer->getArrivalTime());
    return;
}

/**
 * This method handles pacing grant transmissions. Every time a grant is sent
 * for a sender, a grant timer specific to that sender is set to signal the next
 * grant (if needed) at one packet serialization time later. This method
 * dispatches next grant timer to the right message.
 *
 * \param grantTimer
 *      Grant pacer that just fired and is to be handled.
 */
void HomaTransport::ReceiveScheduler::processGrantTimers(HomaGrantTimer *grantTimer)
{
    slog::log6(homaConfig->debug_, transport->get_local_addr(), "ReceiveScheduler::processGrantTimers()");
    slog::log6(homaConfig->debug_, transport->get_local_addr(),
               "\n\n################Process Grant Timer################\n\n");
    SenderState *s = grantTimersMap[grantTimer];
    schedSenders->handleGrantTimerEvent(s);
    double timeNow = Scheduler::instance().clock();

    // schedSenders->remove(s);
    // auto ret = schedSenders->insPoint(s);
    // int sInd = ret.first;
    // int headInd = ret.second;
    // schedSenders->handleGrantRequest(s, sInd, headInd);
    if (inOversubPeriod &&
        schedSenders->numSenders <= schedSenders->numToGrant)
    {
        // Receiver was in an oversubscriped period which is now ended after
        // sending the most recent grant. Mark the end of oversubscription
        // period. Later we dump the statistics in the next packet arrival
        // event.
        inOversubPeriod = false;
        oversubPeriodStop = timeNow;
    }

    // check if number of active sched senders has changed and we should emit
    // signal for activeSenders.
    tryRecordActiveMesgStats(timeNow);
}

/**
 * For every packet that arrives, this function is called to update
 * stats-tracking variables in ReceiveScheduler.
 *
 * \param pktType
 *      Which kind of packet has arrived: REQUEST, UNSCHED_DATA, SCHED_DATA,
 *      GRANT.
 * \param prio
 *      priority of the received packet.
 * \param dataBytesInPkt
 *      lenght of the data portion of the packet in bytes.
 */
void HomaTransport::ReceiveScheduler::addArrivedBytes(PktType pktType, uint32_t prio,
                                                      uint32_t dataBytesInPkt)
{
    slog::log6(homaConfig->debug_, transport->get_local_addr(), "ReceiveScheduler::addArrivedBytes() bytesRecvdPerPrio size:",
               bytesRecvdPerPrio.size(), "prio:", prio, "dataBytesInPkt:", dataBytesInPkt);
    uint32_t arrivedBytesOnWire =
        hdr_homa::getBytesOnWire(dataBytesInPkt, pktType);
    allBytesRecvd += arrivedBytesOnWire;
    bytesRecvdPerPrio.at(prio) += arrivedBytesOnWire;
}

/**
 * For every grant packet sent to the sender, this function is called to update
 * the variables tracking statistics fo outstanding grants, scheduled packets,
 * and bytes.
 *
 * \param prio
 *      Scheduled packet priority specified in the grant packet.
 * \param grantedBytes
 *      Scheduled bytes granted in this the grant packet.
 */
void HomaTransport::ReceiveScheduler::addPendingGrantedBytes(uint32_t prio,
                                                             uint32_t grantedBytes)
{
    slog::log6(homaConfig->debug_, transport->get_local_addr(), "ReceiveScheduler::addPendingGrantedBytes()");
    uint32_t schedBytesOnWire =
        hdr_homa::getBytesOnWire(grantedBytes, PktType::SCHED_DATA);
    scheduledBytesPerPrio.at(prio) += schedBytesOnWire;
    inflightSchedBytes += schedBytesOnWire;
    inflightSchedPerPrio.at(prio) += schedBytesOnWire;
}

/**
 * For every message that arrives at the receiver, this function is called to
 * update the variables tracking the statistics of inflight unscheduled
 * bytes to arrive.
 *
 * \param pktType
 *      The king of the packet unscheduled data will arrive in. Either REQUEST
 *      or UNSCHED_DATA.
 * \param prio
 *      Priority of the packet the unscheduled data arrive in.
 * \param bytesToArrive
 *      The size of unscheduled data in the packet carrying it.
 */
void HomaTransport::ReceiveScheduler::addPendingUnschedBytes(PktType pktType,
                                                             uint32_t prio, uint32_t bytesToArrive)
{
    slog::log6(homaConfig->debug_, transport->get_local_addr(), "ReceiveScheduler::addPendingUnschedBytes()");
    uint32_t unschedBytesOnWire =
        hdr_homa::getBytesOnWire(bytesToArrive, pktType);
    unschedToRecvPerPrio.at(prio) += unschedBytesOnWire;
    inflightUnschedBytes += unschedBytesOnWire;
    inflightUnschedPerPrio.at(prio) += unschedBytesOnWire;
}

/**
 * For every packet that we have expected to arrive and called either of
 * addPendingUnschedBytes() or addPendingGrantedBytes() methods for them, we
 * call this function to at the reception of the packet at receiver. Call to
 * this function updates the variables that track statistics of outstanding
 * bytes.
 *
 * \param pktType
 *      Kind of the packet that has arrived.
 * \param prio
 *      Priority of the arrived packet.
 * \param dataBytesInPkt
 *      Length of the data delivered in the packet.
 */
void HomaTransport::ReceiveScheduler::pendingBytesArrived(PktType pktType,
                                                          uint32_t prio, uint32_t dataBytesInPkt)
{
    slog::log6(homaConfig->debug_, transport->get_local_addr(), "ReceiveScheduler::pendingBytesArrived()");
    uint32_t arrivedBytesOnWire =
        hdr_homa::getBytesOnWire(dataBytesInPkt, pktType);
    switch (pktType)
    {
    case PktType::REQUEST:
    case PktType::UNSCHED_DATA:
        inflightUnschedBytes -= arrivedBytesOnWire;
        inflightUnschedPerPrio.at(prio) -= arrivedBytesOnWire;
        break;
    case PktType::SCHED_DATA:
        inflightSchedBytes -= arrivedBytesOnWire;
        inflightSchedPerPrio.at(prio) -= arrivedBytesOnWire;
        break;
    default:
        throw std::runtime_error("Unknown pktType " + pktType);
    }
}

void HomaTransport::ReceiveScheduler::tryRecordActiveMesgStats(double timeNow)
{
    slog::log6(homaConfig->debug_, transport->get_local_addr(), "ReceiveScheduler::tryRecordActiveMesgStats()");
    uint32_t newActiveSx = schedSenders->numActiveSenders();
    if (newActiveSx != numActiveScheds)
    {
        ActiveScheds activeScheds;
        activeScheds.numActiveSenders = numActiveScheds;
        activeScheds.duration = timeNow - schedChangeTime;
        // transport->emit(activeSchedsSignal, &activeScheds);

        // update the tracker variables
        numActiveScheds = newActiveSx;
        schedChangeTime = timeNow;
    }
}
