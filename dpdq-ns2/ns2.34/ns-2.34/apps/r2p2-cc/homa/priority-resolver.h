/*
 * PriorityResolver.h
 *
 *  Created on: Dec 23, 2015
 *      Author: behnamm
 */

#ifndef PRIORITYRESOLVER_H_
#define PRIORITYRESOLVER_H_

/**
 * @brief This file is taken almost as-is from https://github.com/PlatformLab/HomaSimulation/tree/omnet_simulations/RpcTransportDesign/OMNeT%2B%2BSimulation
 */
// #include "workload-estimator.h"
#include "homa-hdr.h"
#include "homa-config-depot.h"
#include "homa-transport.h"
#include "workload-estimator.h"

class PriorityResolver
{
public:
    typedef HomaTransport::InboundMessage InboundMessage;
    typedef HomaTransport::OutboundMessage OutboundMessage;
    enum PrioResolutionMode
    {
        STATIC_CDF_UNIFORM,
        STATIC_CBF_UNIFORM,
        STATIC_CBF_GRADUATED,
        EXPLICIT,
        FIXED_UNSCHED,
        INVALID_PRIO_MODE // Always the last value
    };

    explicit PriorityResolver(HomaConfigDepot *homaConfig,
                              WorkloadEstimator *distEstimator);
    std::vector<uint32_t> getUnschedPktsPrio(const OutboundMessage *outbndMsg);
    uint32_t getSchedPktPrio(const InboundMessage *inbndMsg);
    void setPrioCutOffs();
    static void printCbfCdf(WorkloadEstimator::CdfVector *vec);
    PrioResolutionMode strPrioModeToInt(const char *prioResMode);

    // Used for comparing double values. returns true if a smaller than b,
    // within an epsilon bound.
    inline bool isLess(double a, double b)
    {
        return a + 1e-6 < b;
    }

    // Used for comparing double values. returns true if a and b are only within
    // some epsilon value from each other.
    inline bool isEqual(double a, double b)
    {
        return fabs(a - b) < 1e-6;
    }

private:
    uint32_t maxSchedPktDataBytes;
    const WorkloadEstimator::CdfVector *cdf;
    const WorkloadEstimator::CdfVector *cbf;
    const WorkloadEstimator::CdfVector *cbfLastCapBytes;
    const WorkloadEstimator::CdfVector *remainSizeCbf;
    std::vector<uint32_t> prioCutOffs;
    WorkloadEstimator *distEstimator;
    PrioResolutionMode prioResMode;
    HomaConfigDepot *homaConfig;

protected:
    void recomputeCbf(uint32_t cbfCapMsgSize, uint32_t boostTailBytesPrio);
    uint32_t getMesgPrio(uint32_t size);
    friend class HomaTransport;
};
#endif /* PRIORITYRESOLVER_H_ */
