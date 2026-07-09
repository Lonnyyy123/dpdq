#include "homa-transport.h"
#include "simple-log.h"

/**
 * Constructor for HomaTransport::TrackRTTs
 */
HomaTransport::TrackRTTs::TrackRTTs(HomaTransport *transport)
    : sendersRTT(), maxRTT(), transport(transport)
{
    slog::log2(2, transport->get_local_addr(), "TrackRTTs::TrackRTTs()");
    maxRTT = std::make_pair(0, SIMTIME_ZERO);
}

/**
 * The rxScheduler calls this method for every new updated RTT value for a
 * specific sender. This function in turn, updates its internal datastruct and
 * also updates HomaConfigDepot::rtt with the max over the minimum RTT
 * observation of all senders.
 */
void HomaTransport::TrackRTTs::updateRTTSample(int32_t senderIP, double rttVal)
{
    slog::log6(transport->homaConfig->debug_, transport->get_local_addr(), "TrackRTTs::updateRTTSample()");
    auto it = sendersRTT.find(senderIP);
    if (it == sendersRTT.end())
    {
        sendersRTT[senderIP] = rttVal;
        if (rttVal > maxRTT.second)
        {
            maxRTT.first = senderIP;
            maxRTT.second = rttVal;
            // transport->homaConfig->rtt = rttVal;
        }
        return;
    }

    if (rttVal >= it->second)
    {
        // When the rtt observation is greater than the recorded value, no
        // action is necessary.
        return;
    }

    if (it->first != maxRTT.first)
    {
        // The RTT observation is different and smaller than recorded maxRTT.
        // So, we only need to update the rttVal for senderIP and return;
        assert(rttVal <= maxRTT.second);
        it->second = rttVal;
        return;
    }

    // Update the maxRTT datastruct and find the new max.
    it->second = rttVal;
    maxRTT.second = rttVal;
    for (auto ipRTT : sendersRTT)
    {
        if (ipRTT.second > maxRTT.second)
        {
            maxRTT.first = ipRTT.first;
            maxRTT.second = ipRTT.second;
            // transport->homaConfig->rtt = rttVal;
        }
    }
}
