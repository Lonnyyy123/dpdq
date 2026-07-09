#include "homa-transport.h"
#include "simple-log.h"
#include <algorithm>

/**
 * Constructor for HomaTransport::ReceiveScheduler::SchedSenders.
 *
 * \param homaConfig
 *      Collection of all user specified config parameters for the transport
 */
HomaTransport::ReceiveScheduler::SchedSenders::SchedSenders(
    HomaConfigDepot *homaConfig, HomaTransport *transport,
    ReceiveScheduler *rxScheduler)
    : transport(transport), rxScheduler(rxScheduler), senders(), schedPrios(homaConfig->adaptiveSchedPrioLevels), numToGrant(homaConfig->numSendersToKeepGranted), headIdx(schedPrios), numSenders(0), homaConfig(homaConfig)
{
    slog::log6(homaConfig->debug_, transport->get_local_addr(), "SchedSenders::SchedSenders()");
    assert(schedPrios >= numToGrant);
    for (size_t i = 0; i < schedPrios; i++)
    {
        senders.push_back(NULL);
    }
}

/*
void
HomaTransport::ReceiveScheduler::SchedSenders::handleGrantRequest(
    SenderState* s, int sInd, int headInd)
{
    if (sInd < 0)
        return;

    if (sInd - headInd >= numToGrant) {
        insert(s);
        return;
    }

    s->sendAndScheduleGrant(
        sInd + homaConfig->allPrio - homaConfig->adaptiveSchedPrioLevels);
    auto ret = insPoint(s);
    int sIndNew = ret.first;
    int headIndNew = ret.second;
    if (sIndNew >= 0 && sIndNew - headIndNew < numToGrant) {
        insert(s);
        return;
    }

    // The grant sent, was the last grant of the 1st preferred sched message of
    // s. The 2nd preffered sched message of s, ranks s beyond number of senders
    // we can grant.
    if (sIndNew > 0) {
        insert(s);
    }
    int indToGrant = headIdx + numToGrant - 1;
    s = removeAt(indToGrant);
    if (!s)
        return;

    auto retNew = insPoint(s);
    int indToGrantNew = retNew.first;
    int headIndNewNew = retNew.second;
    handleGrantRequest(s, indToGrantNew, headIndNewNew);
}
*/

/**
 * Removes a SenderState from sendrs list.
 *
 * \param s
 *      SenderState to be removed.
 * \return
 *      Negative value means the mesg didn't belong to the schedSenders so can't
 *      be removed. Otherwise, it returns the index at which s was removed from.
 */
int HomaTransport::ReceiveScheduler::SchedSenders::remove(SenderState *s)
{
    slog::log6(homaConfig->debug_, transport->get_local_addr(), "SchedSenders::remove()");
    if (numSenders == 0)
    {
        senders.clear();
        headIdx = schedPrios;
        for (size_t i = 0; i < schedPrios; i++)
        {
            senders.push_back(NULL);
        }
        assert(s->mesgsToGrant.empty());
        return -1;
    }

    if (s->mesgsToGrant.empty())
    {
        return -1;
    }

    CompSched cmpSched;
    std::vector<SenderState *>::iterator headIt = headIdx + senders.begin();
    std::vector<SenderState *>::iterator lastIt = headIt + numSenders;
    std::vector<SenderState *>::iterator nltIt =
        std::lower_bound(headIt, lastIt, s, cmpSched);
    if (nltIt != lastIt && (!cmpSched(*nltIt, s) && !cmpSched(s, *nltIt)))
    {
        assert(s == *nltIt);
        numSenders--;
        int ind = nltIt - senders.begin();
        if (ind == (int)headIdx &&
            (ind + std::min(numSenders, (uint32_t)numToGrant) < schedPrios))
        {
            senders[ind] = NULL;
            headIdx++;
        }
        else
        {
            senders.erase(nltIt);
        }
        s->lastIdx = ind;
        return ind;
    }
    else
    {
        throw std::runtime_error("Trying to remove an unlisted sched sender!");
    }
}

/**
 * Remove a SenderState at a specified index in senders list.
 *
 * \param rmIdx
 *      Index of SenderState instace in senders list that we want to remove.
 * \return
 *      Null if no element exists at rmIdx. Otherwise, it return the removed
 *      element.
 */
HomaTransport::ReceiveScheduler::SenderState *
HomaTransport::ReceiveScheduler::SchedSenders::removeAt(uint32_t rmIdx)
{
    slog::log6(homaConfig->debug_, transport->get_local_addr(), "SchedSenders::removeAt()");
    if (rmIdx < headIdx || rmIdx >= (headIdx + numSenders))
    {
        slog::log6(homaConfig->debug_, transport->get_local_addr(), "Objects are within range [",
                   headIdx, ", ", headIdx + numSenders, "). Can't remove at rmIdx ");
        return NULL;
    }
    numSenders--;
    SenderState *rmvd = senders[rmIdx];
    assert(rmvd);
    if (rmIdx == headIdx &&
        (rmIdx + std::min(numSenders, (uint32_t)numToGrant) < schedPrios))
    {
        senders[rmIdx] = NULL;
        headIdx++;
    }
    else
    {
        senders.erase(senders.begin() + rmIdx);
    }
    rmvd->lastIdx = rmIdx;
    return rmvd;
}

/**
 * Insert a SenderState into the senders list. The caller must make sure that
 * the SenderState is not already in the list (ie. by calling remove) and s
 * infact belong to the senders list (ie. sender of s has messages that need
 * grant).  Element s then will be inserted at the position insId returned from
 * insPoint method and headIdx will be updated.
 *
 * \param s
 *      SenderState to be inserted in the senders list.
 */
void HomaTransport::ReceiveScheduler::SchedSenders::insert(SenderState *s)
{
    slog::log6(homaConfig->debug_, transport->get_local_addr(), "SchedSenders::insert()");
    assert(!s->mesgsToGrant.empty());
    auto ins = insPoint(s);
    size_t insIdx = std::get<0>(ins);
    size_t newHeadIdx = std::get<1>(ins);

    for (int i = 0; i < (int)headIdx - 1; i++)
    {
        assert(!senders[i]);
    }
    assert(newHeadIdx <= headIdx);
    size_t leftShift = headIdx - newHeadIdx;
    senders.erase(senders.begin(), senders.begin() + leftShift);
    numSenders++;
    headIdx = newHeadIdx;
    // uint64_t mesgSize = (*s->mesgsToGrant.begin())->msgSize;
    // uint64_t toGrant = (*s->mesgsToGrant.begin())->bytesToGrant;
    // std::cout << (*s->mesgsToGrant.begin())->msgSize << ", "
    //<<(*s->mesgsToGrant.begin())->bytesToGrant << std::endl;
    senders.insert(senders.begin() + insIdx, s);
    return;
}

/**
 * Given a SenderState s, this method find the index of senders list at which s
 * should be inserted. The caller should make sure that s is not already in the
 * list (ie. by calling remove()).
 *
 * \param s
 *      SenderState to be inserted.
 * \return
 *      returns a tuple of (insertedIndex, headIndex, numSenders) as if s is
 *      inserted in the list. insertedIndex negative, means s doesn't belong to
 *      the schedSenders.
 */
std::tuple<int, int, int>
HomaTransport::ReceiveScheduler::SchedSenders::insPoint(SenderState *s)
{
    slog::log6(homaConfig->debug_, transport->get_local_addr(), "SchedSenders::insPoint()");
    CompSched cmpSched;
    if (s->mesgsToGrant.empty())
    {
        return std::make_tuple(-1, headIdx, numSenders);
    }
    std::vector<SenderState *>::iterator headIt = headIdx + senders.begin();
    auto insIt = std::upper_bound(headIt, headIt + numSenders, s, cmpSched);
    size_t insIdx = insIt - senders.begin();
    uint32_t hIdx = headIdx;
    uint32_t numSxAfterIns = numSenders;

    slog::log6(homaConfig->debug_, transport->get_local_addr(), "insIdx",
               insIdx, "hIdx", hIdx, ", last index: ", s->lastIdx);
    if (insIdx == hIdx)
    {
        if (insIdx > 0 && insIdx != s->lastIdx)
        {
            hIdx--;
            insIdx--;
            assert(senders[hIdx] == NULL);
        }
    }

    numSxAfterIns++;
    int leftShift = (int)std::min(numSxAfterIns,
                                  (uint32_t)std::min(numToGrant, schedPrios)) -
                    (schedPrios - hIdx);

    slog::log6(homaConfig->debug_, transport->get_local_addr(), "numSenders",
               numSxAfterIns, "numToGrant:", numToGrant, ", insIdx: ", insIdx,
               ", hIdx: ", hIdx, ", Left shift: ", leftShift);

    if (leftShift > 0)
    {
        assert(!senders[hIdx - leftShift] && (int)hIdx >= leftShift);
        hIdx -= leftShift;
        insIdx -= leftShift;
    }
    return std::make_tuple(insIdx, hIdx, numSxAfterIns);
}

/**
 * Depending on the numToGrant and numSenders, the number of senders that
 * are actively getting grants changes. Call to function returns exactly how
 * many senders are being granted at this point.
 *
 * \return
 *    Returns the number of scheduled senders that are currently receiving
 *    grants.
 */
uint32_t
HomaTransport::ReceiveScheduler::SchedSenders::numActiveSenders()
{
    slog::log6(homaConfig->debug_, transport->get_local_addr(), "SchedSenders::numActiveSenders()");
    // Runtime check to verify senders data-struct is flawless
    for (auto i = headIdx; i < headIdx + numSenders - 1; i++)
    {
        assert(senders[i]);
    }

    if (numToGrant >= numSenders)
    {
        // Runtime check to verify senders data-struct is flawless
        for (int i = 0; i < (int)headIdx - 1; i++)
        {
            assert(!senders[i]);
        }
        return numSenders;
    }

    if (numSenders >= schedPrios && headIdx)
    {
        if (numToGrant >= schedPrios)
        {
            throw std::runtime_error("num sched senders gt. schedPrios but receiver not"
                                     " granting on all sched prios! This can only happen if numToGrant is"
                                     " lt. schedPrios.");
        }
    }
    return numToGrant;
}

/**
 * This function implements receiver's logic for the scheduler in reaction of
 * message completions, when a packet arrives. Based on the current logic, every
 * time a message completes, the overcommittment level will be decremented if
 * it's been incremented in the past.
 *
 * \param msgCompHandle
 *      Handle as returned from the SchedSenders::handleInboundPkt method.
 * \param old
 *      The state prior to arrival of the packet that triggered the call to this
 *      function.
 * \param cur
 *      The state variable after the packet arrival event that triggered the
 *      call to this method.
 */
void HomaTransport::ReceiveScheduler::SchedSenders::handleMesgRecvCompletionEvent(
    const std::pair<bool, int> &msgCompHandle, SchedState &old, SchedState &cur)
{
    slog::log6(homaConfig->debug_, transport->get_local_addr(), "SchedSenders::handleMesgRecvCompletionEvent()");
    bool schedMesgFin = msgCompHandle.first;
    int mesgRemain = msgCompHandle.second;
    if (!schedMesgFin || mesgRemain >= 0)
    {
        // If a scheduled mesg is not completed before the call to this method,
        // there's nothing to do here; just return;
        return;
    }

    assert((uint32_t)cur.numToGrant == numToGrant && (uint32_t)old.numToGrant == numToGrant);
    if (numToGrant == homaConfig->numSendersToKeepGranted)
    {
        // if numToGrant is at its lowest possible value, there's nothing to do
        // here; just return;
        return;
    }

    numToGrant--;
    cur.numToGrant--;
    return;
}

/**
 * This function implements the logics of the receiver scheduler for reacting
 * to the received packets. At every packet arrival the state (sender's ranking,
 * etc.) can changes in the view of the receiver. So, this method is called to
 * check if a grant should be sent for that sender or any other senderand this
 * function then sends the grant if needed.  N.B. the sender must have been
 * removed from schedSenders prior to the call to this method. The side effect
 * of this method invokation is that it also inserts the SenderState into the
 * schedSenders list if it needs more grants.
 *
 * \param old
 *      The receiver scheduler state before arrival of the packet.
 *
 * \param cur
 *      The receiver scheduler state after arrival of the packet.
 */
void HomaTransport::ReceiveScheduler::SchedSenders::handlePktArrivalEvent(
    SchedState &old, SchedState &cur)
{
    slog::log6(homaConfig->debug_, transport->get_local_addr(), "SchedSenders::handlePktArrivalEvent()");
    if (cur.sInd < 0)
    {

        if (old.sInd < 0 || old.sInd >= (old.numToGrant + old.headIdx))
        {
            return;
        }

        if (old.sInd >= old.headIdx &&
            old.sInd < old.numToGrant + old.headIdx)
        {
            int indToGrant;
            SenderState *sToGrant;
            if (cur.numSenders < cur.numToGrant)
            {
                indToGrant = cur.headIdx + cur.numSenders - 1;
                sToGrant = removeAt(indToGrant);
            }
            else
            {
                indToGrant = cur.headIdx + cur.numToGrant - 1;
                sToGrant = removeAt(indToGrant);
            }

            old = cur;
            old.s = sToGrant;
            old.sInd = indToGrant;

            sToGrant->sendAndScheduleGrant(getPrioForMesg(old));

            auto ret = insPoint(sToGrant);
            cur.numToGrant = numToGrant;
            cur.sInd = std::get<0>(ret);
            cur.headIdx = std::get<1>(ret);
            cur.numSenders = std::get<2>(ret);
            cur.s = sToGrant;
            handleGrantSentEvent(old, cur);

            return;
        }
        throw std::runtime_error("invalid old position for sender in the receiver's"
                                 " preference list");
    }

    if (cur.sInd >= cur.headIdx + cur.numToGrant)
    {
        if (old.sInd < 0 || old.sInd >= old.headIdx + old.numToGrant)
        {
            insert(cur.s);
            return;
        }

        if (old.sInd >= old.headIdx && old.sInd < old.numToGrant + old.headIdx)
        {
            insert(cur.s);
            int indToGrant = cur.headIdx + cur.numToGrant - 1;
            SenderState *sToGrant = removeAt(indToGrant);

            old = cur;
            old.s = sToGrant;
            old.sInd = indToGrant;

            sToGrant->sendAndScheduleGrant(getPrioForMesg(old));

            auto ret = insPoint(sToGrant);
            cur.numToGrant = numToGrant;
            cur.sInd = std::get<0>(ret);
            cur.headIdx = std::get<1>(ret);
            cur.numSenders = std::get<2>(ret);
            cur.s = sToGrant;
            handleGrantSentEvent(old, cur);
            return;
        }
        throw std::runtime_error("invalid old position for sender in the receiver's"
                                 " preference list.");
    }

    if (cur.sInd < cur.headIdx + cur.numToGrant)
    {
        old = cur;
        cur.s->sendAndScheduleGrant(getPrioForMesg(old));
        auto ret = insPoint(cur.s);
        cur.sInd = std::get<0>(ret);
        cur.headIdx = std::get<1>(ret);
        cur.numSenders = std::get<2>(ret);
        handleGrantSentEvent(old, cur);
        return;
    }

    assert(cur.sInd >= 0 && cur.sInd < cur.headIdx);
    throw std::runtime_error("Error, sender has invalid position in receiver's "
                             "preference list.");
}

/**
 * Sending a grant is an event that can change the state of a receive scheduler
 * and should be handled. This function is called every time a grant is sent to
 * handle the state change and react to it.
 *
 * \param old
 *      The receive scheduler state prior to grant transmission.
 * \param cur
 *      The receive scheduler state after grant transmission.
 */
void HomaTransport::ReceiveScheduler::SchedSenders::handleGrantSentEvent(
    SchedState &old, SchedState &cur)
{
    slog::log6(homaConfig->debug_, transport->get_local_addr(), "SchedSenders::handleGrantSentEvent()");
    assert(old.sInd >= old.headIdx && old.sInd < old.headIdx + old.numToGrant);
    if (old.sInd == cur.sInd)
    {
        assert(cur.sInd < cur.headIdx + cur.numToGrant);
        insert(cur.s);

        slog::log6(homaConfig->debug_, transport->get_local_addr(),
                   "SchedState before handleGrantSent:\n\t", old);
        slog::log6(homaConfig->debug_, transport->get_local_addr(),
                   "SchedState after handleGrantSent:\n\t", cur);
        return;
    }

    if (cur.sInd >= 0 && cur.sInd < cur.headIdx)
    {
        throw std::runtime_error("Error, sender has invalid position in receiver's "
                                 "preference list.");
    }

    if (cur.sInd >= 0)
    {
        insert(cur.s);
    }

    if ((cur.sInd < 0 || cur.sInd >= cur.numToGrant + cur.headIdx) &&
        cur.numSenders >= cur.numToGrant)
    {
        int indToGrant = cur.headIdx + cur.numToGrant - 1;
        SenderState *sToGrant = removeAt(indToGrant);
        old = cur;
        old.s = sToGrant;
        old.sInd = indToGrant;

        sToGrant->sendAndScheduleGrant(getPrioForMesg(old));

        auto ret = insPoint(sToGrant);
        cur.numToGrant = numToGrant;
        cur.sInd = std::get<0>(ret);
        cur.headIdx = std::get<1>(ret);
        cur.numSenders = std::get<2>(ret);
        cur.s = sToGrant;
        handleGrantSentEvent(old, cur);
    }
    slog::log6(homaConfig->debug_, transport->get_local_addr(),
               "SchedState before handleGrantSent:\n\t", old);
    slog::log6(homaConfig->debug_, transport->get_local_addr(),
               "SchedState after handleGrantSent:\n\t", cur);

    return;
}

/**
 * This method process per sender timers for sending grants. If a grant timer
 * was scheduled for a sender
 */
void HomaTransport::ReceiveScheduler::SchedSenders::handleGrantTimerEvent(
    SenderState *s)
{
    slog::log6(homaConfig->debug_, transport->get_local_addr(), "SchedSenders::handleGrantTimerEvent()");
    remove(s);
    auto ret = insPoint(s);
    int sInd = std::get<0>(ret);
    int headInd = std::get<1>(ret);
    int sendersNum = std::get<2>(ret);

    if (sInd < 0)
        return;

    // sInd - headInd can be negative (empirically observed)
    if (sInd - headInd >= (int)numToGrant)
    {
        insert(s);
        return;
    }

    SchedState old;
    SchedState cur;

    old.numToGrant = numToGrant;
    old.headIdx = headInd;
    old.numSenders = sendersNum;
    old.s = s;
    old.sInd = sInd;

    s->sendAndScheduleGrant(getPrioForMesg(old));

    ret = insPoint(s);
    cur.numToGrant = numToGrant;
    cur.s = s;
    cur.sInd = std::get<0>(ret);
    cur.headIdx = std::get<1>(ret);
    cur.numSenders = std::get<2>(ret);
    handleGrantSentEvent(old, cur);
    return;
}

/**
 * Processes the schedBwUtilTimer triggers and implements the mechanism to
 * properly react to wasted receiver bandwidth when scheduled senders don't send
 * scheduled packets in a timely fashion.
 *
 * \param timer
 *      Timer object that triggers when bw wastage is detected (ie. the receiver
 *      expected scheduled packets but they weren't received as expected.)
 */
void HomaTransport::ReceiveScheduler::SchedSenders::handleBwUtilTimerEvent(
    HomaSchedBwUtilTimer *timer)
{

    slog::log6(homaConfig->debug_, transport->get_local_addr(), "SchedSenders::handleBwUtilTimerEvent()");
    // if bwCheckInterval is set to max, it means schedBwUtilTimer is disabled
    // and the timer should never fire and this method should never have been
    // invoked.
    if (rxScheduler->bwCheckInterval == MAXTIME)
    {
        throw std::runtime_error("Error: Func handleBwUtilTimerEvent is called while"
                                 " schedBwUtilTimer is disabled!");
    }

    slog::log6(homaConfig->debug_, transport->get_local_addr(),
               "\n\n################Process BwUtil Timer###############\n\n");
    double timeNow = Scheduler::instance().clock();
    SchedState old;
    SchedState cur;

    // Bubble detected and no sender has been previously promoted, so promote
    // next ungranted sched sender and send grants for it.
    if (numSenders <= numToGrant)
    {
        // All sched senders are currently being granted. No need to increament
        // numToGrant
        return;
    }
    numToGrant++;
    if (headIdx)
    {
        // Runtime checks with asserts
        for (size_t i = 0; i < headIdx; i++)
        {
            assert(!senders[i]);
        } // End checking
        headIdx--;
        senders.erase(senders.begin());
    }

    uint32_t indToGrant = headIdx + numToGrant - 1;
    SenderState *lowPrioSx = senders[indToGrant];
    old.setVar(numToGrant, headIdx, numSenders, lowPrioSx, indToGrant);
    auto sxToGrant = removeAt(indToGrant);

    // Runtime checks with assert
    assert(sxToGrant == lowPrioSx);
    auto topMesgIt = lowPrioSx->mesgsToGrant.begin();
    assert(topMesgIt != lowPrioSx->mesgsToGrant.end());
    InboundMessage *topMesg = *topMesgIt;
    assert(topMesg->bytesToGrant > 0);
    // End runtime checks

    lowPrioSx->sendAndScheduleGrant(getPrioForMesg(old));

    // we have previously incremented overcommittment level
    lowPrioSx->sendAndScheduleGrant(getPrioForMesg(old));
    auto ret = insPoint(lowPrioSx);
    cur.setVar(numToGrant, std::get<1>(ret), std::get<2>(ret),
               lowPrioSx, std::get<0>(ret));
    handleGrantSentEvent(old, cur);

    // Enable timer for the next bubble detection. The next timer should be set
    // for at least one RTT later because we don't expect to receive any packet
    // from the newly inducted sender anytime earlier than one RTT later.
    // transport->scheduleAt(timeNow + homaConfig->rtt,
    //                       rxScheduler->schedBwUtilTimer);
    timer->resched(homaConfig->rtt);

    // check if number of active sched senders has changed and we should emit
    // signal for activeSenders
    rxScheduler->tryRecordActiveMesgStats(timeNow);
}

uint32_t
HomaTransport::ReceiveScheduler::SchedSenders::getPrioForMesg(SchedState &st)
{
    slog::log6(homaConfig->debug_, transport->get_local_addr(), "SchedSenders::getPrioForMesg()");
    int grantPrio =
        st.sInd + homaConfig->allPrio - homaConfig->adaptiveSchedPrioLevels;
    grantPrio = std::min(static_cast<int>(homaConfig->allPrio - 1), grantPrio);
    slog::log6(homaConfig->debug_, transport->get_local_addr(), "Get prio for mesg. sInd: ",
               st.sInd, ", grantPrio: ", grantPrio, ", allPrio: ", homaConfig->allPrio,
               ", schedPrios: ", homaConfig->adaptiveSchedPrioLevels);
    return static_cast<uint32_t>(grantPrio);
}
