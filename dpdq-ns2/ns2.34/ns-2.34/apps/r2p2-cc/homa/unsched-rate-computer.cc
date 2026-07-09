#include "homa-transport.h"
#include "simple-log.h"

/**
 * @file unsched-rate-computer.cc
 * @brief This file mostly copies https://github.com/PlatformLab/HomaSimulation/tree/omnet_simulations/RpcTransportDesign/OMNeT%2B%2BSimulation
 */

/**
 * Constructor of HomaTransport::ReceiveScheduler::UnschedRateComputer.
 *
 * \param homaConfig
 *      Collection of user specified config parameters for the transport.
 * \param computeAvgUnschRate
 *      True enables this modules.
 * \param minAvgTimeWindow
 *      Time interval over which this module computes the average unscheduled
 *      rate.
 */
HomaTransport::ReceiveScheduler::UnschedRateComputer::UnschedRateComputer(
    HomaConfigDepot *homaConfig, bool computeAvgUnschRate,
    double minAvgTimeWindow)
    : computeAvgUnschRate(computeAvgUnschRate),
      bytesRecvTime(),
      sumBytes(0),
      minAvgTimeWindow(minAvgTimeWindow),
      homaConfig(homaConfig)
{
    slog::log2(homaConfig->debug_, homaConfig->this_addr_, "UnschedRateComputer::UnschedRateComputer()");
}

/**
 * Interface to receive computed average unscheduled bytes rate.
 *
 * \return
 *      Returns the fraction of bw used by the unsched (eg. average unsched rate
 *      divided by nic link speed.)
 */
double
HomaTransport::ReceiveScheduler::UnschedRateComputer::getAvgUnschRate(
    double currentTime)
{
    slog::log6(homaConfig->debug_, homaConfig->this_addr_, "UnschedRateComputer::getAvgUnschRate()");
    if (!computeAvgUnschRate)
    {
        return 0;
    }

    if (bytesRecvTime.size() == 0)
    {
        return 0.0;
    }
    double timeDuration = currentTime - bytesRecvTime.front().second;
    if (timeDuration <= minAvgTimeWindow / 100)
    {
        return 0.0;
    }
    double avgUnschFractionRate =
        ((double)sumBytes * 8 / timeDuration) * 1e-9 / homaConfig->nicLinkSpeed;
    assert(avgUnschFractionRate < 1.0);
    return avgUnschFractionRate;
}

/**
 * Interface for accumulating the unsched bytes for unsched rate calculation.
 * This is called evertime an unscheduled packet arrives.
 *
 * \param arrivalTime
 *      Time at which unscheduled packet arrived.
 * \param bytesRecvd
 *      Total unsched bytes received including all headers and packet overhead
 *      bytes on wire.
 */
void HomaTransport::ReceiveScheduler::UnschedRateComputer::updateUnschRate(
    double arrivalTime, uint32_t bytesRecvd)
{
    slog::log6(homaConfig->debug_, homaConfig->this_addr_, "UnschedRateComputer::updateUnschRate()");
    if (!computeAvgUnschRate)
    {
        return;
    }

    bytesRecvTime.push_back(std::make_pair(bytesRecvd, arrivalTime));
    sumBytes += bytesRecvd;
    for (auto bytesTimePair = bytesRecvTime.begin();
         bytesTimePair != bytesRecvTime.end();)
    {
        double deltaTime = arrivalTime - bytesTimePair->second;
        if (deltaTime <= minAvgTimeWindow)
        {
            return;
        }

        if (deltaTime > minAvgTimeWindow)
        {
            sumBytes -= bytesTimePair->first;
            bytesTimePair = bytesRecvTime.erase(bytesTimePair);
        }
        else
        {
            ++bytesTimePair;
        }
    }
}