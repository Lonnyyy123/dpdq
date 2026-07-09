/*
 * PriorityResolver.cc
 *
 *  Created on: Dec 23, 2015
 *      Author: behnamm
 */

#include "priority-resolver.h"
#include "simple-log.h"

PriorityResolver::PriorityResolver(HomaConfigDepot *homaConfig,
                                   WorkloadEstimator *distEstimator)
    : cdf(&distEstimator->cdfFromFile), cbf(&distEstimator->cbfFromFile), cbfLastCapBytes(&distEstimator->cbfLastCapBytesFromFile), remainSizeCbf(&distEstimator->remainSizeCbf), prioCutOffs(), distEstimator(distEstimator), prioResMode(), homaConfig(homaConfig)
{
    slog::log2(homaConfig->debug_, homaConfig->this_addr_,
               "PriorityResolver::PriorityResolver(HomaConfigDepot, WorkloadEstimator)");
    distEstimator->getCbfFromCdf(distEstimator->cdfFromFile,
                                 homaConfig->cbfCapMsgSize, homaConfig->boostTailBytesPrio);
    // sets remainSizeCbf
    distEstimator->getRemainSizeCdfCbf(distEstimator->cdfFromFile,
                                       homaConfig->cbfCapMsgSize, homaConfig->boostTailBytesPrio);
    prioResMode = strPrioModeToInt(homaConfig->unschedPrioResolutionMode);
    setPrioCutOffs();
}

uint32_t
PriorityResolver::getMesgPrio(uint32_t msgSize)
{
    slog::log6(homaConfig->debug_, homaConfig->this_addr_,
               "PriorityResolver::getMesgPrio()");
    size_t mid, high, low;
    low = 0;
    high = prioCutOffs.size() - 1;
    while (low < high)
    {
        mid = (high + low) / 2;
        if (msgSize <= prioCutOffs.at(mid))
        {
            high = mid;
        }
        else
        {
            low = mid + 1;
        }
    }
    slog::log6(homaConfig->debug_, homaConfig->this_addr_,
               "PriorityResolver::getMesgPrio() --- prio found : ", high);
    return high;
}

// KP function description:
// using STATIC_CBF_GRADUATED:
//
std::vector<uint32_t>
PriorityResolver::getUnschedPktsPrio(const OutboundMessage *outbndMsg)
{
    slog::log6(homaConfig->debug_, homaConfig->this_addr_,
               "PriorityResolver::getUnschedPktsPrio()");
    uint32_t msgSize = outbndMsg->msgSize;
    switch (prioResMode)
    {
    case PrioResolutionMode::FIXED_UNSCHED:
    {
        std::vector<uint32_t> unschedPktsPrio(
            outbndMsg->reqUnschedDataVec.size(), 0);
        return unschedPktsPrio;
    }
    case PrioResolutionMode::EXPLICIT:
    case PrioResolutionMode::STATIC_CBF_GRADUATED:
    {
        std::vector<uint32_t> unschedPktsPrio;
        for (auto &pkt : outbndMsg->reqUnschedDataVec)
        {
            unschedPktsPrio.push_back(getMesgPrio(msgSize));
            assert(msgSize >= pkt);
            msgSize -= pkt;
        }
        return unschedPktsPrio;
    }
    case PrioResolutionMode::STATIC_CDF_UNIFORM:
    case PrioResolutionMode::STATIC_CBF_UNIFORM:
    {
        uint32_t prio = getMesgPrio(msgSize);
        std::vector<uint32_t> unschedPktsPrio(
            outbndMsg->reqUnschedDataVec.size(), prio);
        return unschedPktsPrio;
    }
    default:
        throw std::invalid_argument("Invalid priority mode for : prioMode() "
                                    "for unscheduled packets.");
    }
}

uint32_t
PriorityResolver::getSchedPktPrio(const InboundMessage *inbndMsg)
{
    slog::log6(homaConfig->debug_, homaConfig->this_addr_,
               "PriorityResolver::getSchedPktPrio()");
    uint32_t msgSize = inbndMsg->msgSize;
    uint32_t bytesToGrant = inbndMsg->bytesToGrant;
    uint32_t bytesToGrantOnWire = hdr_homa::getBytesOnWire(bytesToGrant,
                                                           PktType::SCHED_DATA);
    uint32_t bytesTreatedUnsched = homaConfig->boostTailBytesPrio;
    switch (prioResMode)
    {
    case PrioResolutionMode::EXPLICIT:
    case PrioResolutionMode::STATIC_CBF_GRADUATED:
    {
        if (bytesToGrantOnWire < bytesTreatedUnsched)
        {
            return getMesgPrio(bytesToGrant);
        }
        else
        {
            return homaConfig->allPrio - 1;
        }
    }
    case PrioResolutionMode::STATIC_CDF_UNIFORM:
    {
    case PrioResolutionMode::STATIC_CBF_UNIFORM:
        if (bytesToGrantOnWire < bytesTreatedUnsched)
        {
            return getMesgPrio(msgSize);
        }
        else
        {
            return homaConfig->allPrio - 1;
        }
    }
    default:
        throw std::runtime_error("Invalid priority mode: prioMode(%d) for"
                                 " scheduled packets" +
                                 prioResMode);
    }
    return homaConfig->allPrio - 1;
}

void PriorityResolver::recomputeCbf(uint32_t cbfCapMsgSize,
                                    uint32_t boostTailBytesPrio)
{
    slog::log6(homaConfig->debug_, homaConfig->this_addr_,
               "PriorityResolver::recomputeCbf");
    distEstimator->getCbfFromCdf(distEstimator->cdfFromFile,
                                 cbfCapMsgSize, boostTailBytesPrio);
    distEstimator->getRemainSizeCdfCbf(distEstimator->cdfFromFile,
                                       cbfCapMsgSize, boostTailBytesPrio);
    setPrioCutOffs();
}

void PriorityResolver::setPrioCutOffs()
{
    slog::log6(homaConfig->debug_, homaConfig->this_addr_,
               "PriorityResolver::setPrioCutOffs");
    prioCutOffs.clear();
    const WorkloadEstimator::CdfVector *vecToUse = NULL;
    if (prioResMode == PrioResolutionMode::EXPLICIT)
    {
        prioCutOffs = homaConfig->explicitUnschedPrioCutoff;
        if (prioCutOffs.back() != UINT32_MAX)
        {
            prioCutOffs.push_back(UINT32_MAX);
        }
        return;
    }

    if (prioResMode == PrioResolutionMode::STATIC_CDF_UNIFORM)
    {
        // std::cout << "Cutoff mode: STATIC_CDF_UNIFORM" << std::endl;
        vecToUse = cdf;
    }
    else if (prioResMode == PrioResolutionMode::STATIC_CBF_UNIFORM)
    {
        // std::cout << "Cutoff mode: STATIC_CBF_UNIFORM" << std::endl;
        vecToUse = cbfLastCapBytes;
    }
    else if (prioResMode == PrioResolutionMode::STATIC_CBF_GRADUATED)
    {
        // std::cout << "Cutoff mode: STATIC_CBF_GRADUATED" << std::endl;
        vecToUse = remainSizeCbf;
    }
    else
    {
        throw std::runtime_error("Invalid priority mode: prioMode(%d) for"
                                 " scheduled packets" +
                                 prioResMode);
    }

    assert(isEqual(vecToUse->at(vecToUse->size() - 1).second, 1.00));

    double probMax = 1.0;
    double fac = homaConfig->unschedPrioUsageWeight;
    double probStep;
    if (fac == 1)
    {
        assert(homaConfig->prioResolverPrioLevels > 0);
        probStep = probMax / homaConfig->prioResolverPrioLevels;
    }
    else
    {
        assert(1 - pow(fac, (int)(homaConfig->prioResolverPrioLevels)) > 0);
        probStep = probMax * (1.0 - fac) /
                   (1 - pow(fac, (int)(homaConfig->prioResolverPrioLevels)));
    }
    // std::cout << "--------- vec to use -------------" << std::endl;
    // for (auto v : *vecToUse)
    // {
    //     std::cout << "bytes: " << v.first << " chance: " << v.second << std::endl;
    // }
    // std::cout << "prob step: " << probStep << std::endl;
    size_t i = 0;
    uint32_t prevCutOffSize = UINT32_MAX;
    for (double prob = probStep; isLess(prob, probMax); prob += probStep)
    {
        probStep *= fac;
        for (; i < vecToUse->size(); i++)
        {
            // std::cout << "prob: " << prob << " vecToUse->at(i).first: " << vecToUse->at(i).first << " prevCutOffSize: " << prevCutOffSize << std::endl;
            if (vecToUse->at(i).first == prevCutOffSize)
            {
                // Do not add duplicate sizes to cutOffSizes vector
                continue;
            }
            if (vecToUse->at(i).second >= prob)
            {
                // std::cout << ">>>> prob: " << prob << " vecToUse->at(i).second: " << vecToUse->at(i).second << " size: " << vecToUse->at(i).first << " prevCutOffSize: " << prevCutOffSize << std::endl;
                prioCutOffs.push_back(vecToUse->at(i).first);
                prevCutOffSize = vecToUse->at(i).first;
                break;
            }
        }
    }
    prioCutOffs.push_back(UINT32_MAX);
    slog::log2(homaConfig->debug_, homaConfig->this_addr_, "--- Prio Cutoffs --- Total:", prioCutOffs.size());
    for (uint32_t prio : prioCutOffs)
    {
        slog::log2(homaConfig->debug_, homaConfig->this_addr_, prio);
    }
    slog::log2(homaConfig->debug_, homaConfig->this_addr_, "-------------------------");
}

PriorityResolver::PrioResolutionMode
PriorityResolver::strPrioModeToInt(const char *prioResMode)
{
    slog::log6(homaConfig->debug_, homaConfig->this_addr_,
               "PriorityResolver::strPrioModeToInt()");
    if (strcmp(prioResMode, "STATIC_CDF_UNIFORM") == 0)
    {
        return PrioResolutionMode::STATIC_CDF_UNIFORM;
    }
    else if (strcmp(prioResMode, "STATIC_CBF_UNIFORM") == 0)
    {
        return PrioResolutionMode::STATIC_CBF_UNIFORM;
    }
    else if (strcmp(prioResMode, "STATIC_CBF_GRADUATED") == 0)
    {
        return PrioResolutionMode::STATIC_CBF_GRADUATED;
    }
    else if (strcmp(prioResMode, "EXPLICIT") == 0)
    {
        return PrioResolutionMode::EXPLICIT;
    }
    else if (strcmp(prioResMode, "FIXED_UNSCHED") == 0)
    {
        return PrioResolutionMode::STATIC_CBF_GRADUATED;
    }
    else
    {
        return PrioResolutionMode::INVALID_PRIO_MODE;
    }
}

void PriorityResolver::printCbfCdf(WorkloadEstimator::CdfVector *vec)
{
    for (auto &elem : *vec)
    {
        std::cout << elem.first << " : " << elem.second << std::endl;
    }
}
