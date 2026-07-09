#include "homa-transport.h"
#include "priority-resolver.h"
#include "simple-log.h"
#include "homa-udp.h"
#include "flags.h"

static class HomaTransportClass : public TclClass
{
public:
    HomaTransportClass() : TclClass("HOMA") {}
    TclObject *create(int, const char *const *)
    {
        return (new HomaTransport);
    }
} class_homa_transport;

HomaTransport::HomaTransport() : sxController(this),
                                 rxScheduler(this),
                                 trackRTTs(this),
                                 sendTimer(nullptr),
                                 outstandingGrantBytes(0)
{
    init();
}

// based on HomaTransport::initialize()
void HomaTransport::init()
{
    // ------------- PARAMETERS --------------------
    bind("debug_", &debug_);
    bind("nicLinkSpeed", &nicLinkSpeed);
    bind("rttBytes", &rttBytes);
    bind("maxOutstandingRecvBytes", &maxOutstandingRecvBytes);
    bind("grantMaxBytes", &grantMaxBytes);
    bind("allPrio", &allPrio);
    bind("adaptiveSchedPrioLevels", &adaptiveSchedPrioLevels);
    bind("numSendersToKeepGranted", &numSendersToKeepGranted);
    bind("accountForGrantTraffic", &accountForGrantTraffic);
    bind("prioResolverPrioLevels", &prioResolverPrioLevels);
    bind("unschedPrioUsageWeight", &unschedPrioUsageWeight);
    bind("isRoundRobinScheduler", &isRoundRobinScheduler);
    bind("linkCheckBytes", &linkCheckBytes);
    bind("cbfCapMsgSize", &cbfCapMsgSize);
    bind("boostTailBytesPrio", &boostTailBytesPrio);
    bind("defaultReqBytes", &defaultReqBytes);
    bind("defaultUnschedBytes", &defaultUnschedBytes);
    bind("useUnschRateInScheduler", &useUnschRateInScheduler);
    bind("workloadType", &workloadType);

    // This way of setting homa depot params is obviously terrible but it is quick to implement (...)
    // variables used in bind() must be class members
    slog::log2(debug_, get_local_addr(), "HomaTransport::init()", debug_, workloadType);
    homaConfig = new HomaConfigDepot(this);
    homaConfig->nicLinkSpeed = nicLinkSpeed;
    homaConfig->rtt = rttBytes * 8.0 / nicLinkSpeed / 1000.0 / 1000.0 / 1000.0;
    homaConfig->rttBytes = static_cast<uint32_t>(rttBytes);
    homaConfig->maxOutstandingRecvBytes = static_cast<uint32_t>(maxOutstandingRecvBytes);
    homaConfig->grantMaxBytes = static_cast<uint32_t>(grantMaxBytes);
    homaConfig->allPrio = static_cast<uint32_t>(allPrio);
    homaConfig->adaptiveSchedPrioLevels = static_cast<uint32_t>(adaptiveSchedPrioLevels);
    homaConfig->numSendersToKeepGranted = static_cast<uint32_t>(numSendersToKeepGranted);
    homaConfig->accountForGrantTraffic = static_cast<bool>(accountForGrantTraffic);
    homaConfig->prioResolverPrioLevels = static_cast<uint32_t>(prioResolverPrioLevels);
    homaConfig->unschedPrioResolutionMode = "STATIC_CBF_GRADUATED"; // Using the same as homa eperiments STATIC_CBF_GRADUATED (produces weird results)
    homaConfig->unschedPrioUsageWeight = static_cast<double>(unschedPrioUsageWeight);
    homaConfig->senderScheme = "SRBF"; // Pkt of shortest remaining message first (not prio based) as in original simulations
    homaConfig->paramToEnum();
    homaConfig->isRoundRobinScheduler = static_cast<bool>(isRoundRobinScheduler);
    homaConfig->linkCheckBytes = linkCheckBytes;
    homaConfig->cbfCapMsgSize = static_cast<uint32_t>(cbfCapMsgSize);
    homaConfig->boostTailBytesPrio = static_cast<uint32_t>(boostTailBytesPrio);
    homaConfig->defaultReqBytes = static_cast<uint32_t>(defaultReqBytes);
    homaConfig->defaultUnschedBytes = static_cast<uint32_t>(defaultUnschedBytes);
    homaConfig->useUnschRateInScheduler = static_cast<bool>(useUnschRateInScheduler);
    homaConfig->workloadType = map_workload(workloadType);
    homaConfig->debug_ = debug_;
    homaConfig->this_addr_ = this_addr_;

    homaConfig->print_all();

    // ------------- TIMERS --------------------
    sendTimer = new HomaSendTimer(this, new HomaEvent(HomaEventType::SEND_TIMER_EVENT));

    // ------------- OTHER --------------------
    hdr_homa dataPkt = hdr_homa();
    dataPkt.pktType_var() = PktType::SCHED_DATA;
    uint32_t maxDataBytes = MAX_ETHERNET_PAYLOAD -
                            IP_HEADER_SIZE - UDP_HEADER_SIZE - dataPkt.headerSize();
    slog::log3(debug_, get_local_addr(), "homaConfig->grantMaxBytes:", homaConfig->grantMaxBytes, ">? maxDataBytes:", maxDataBytes);
    if (homaConfig->grantMaxBytes > maxDataBytes)
    {
        homaConfig->grantMaxBytes = maxDataBytes;
    }

    distEstimator = new WorkloadEstimator(homaConfig);
    prioResolver = new PriorityResolver(homaConfig, distEstimator);
    rxScheduler.initialize(homaConfig, prioResolver);
    sxController.initSendController(homaConfig, prioResolver);
    outstandingGrantBytes = 0;
}

HomaTransport::~HomaTransport()
{
    delete sendTimer;
    delete prioResolver;
    // delete distEstimator;
    delete homaConfig;
}

void HomaTransport::attach_r2p2_application(R2p2Application *r2p2_application)
{
    slog::log2(debug_, get_local_addr(), "HomaTransport::attach_r2p2_application(): Attaching application with thread id:", r2p2_application->get_thread_id());
    thread_id_to_app_[r2p2_application->get_thread_id()] = r2p2_application;
}

/**
 * @brief Called by application
 * closest Homa sim equivalent: processSendMsgFromApp()
 *
 * @param payload
 * @param request_id_tuple
 */
void HomaTransport::r2p2_send_req(int payload, const RequestIdTuple &request_id_tuple)
{
    slog::log4(debug_, get_local_addr(), "HomaTransport::r2p2_send_req(). payload =", payload, "static_cast<uint32_t>(payload)=", static_cast<uint32_t>(payload));
    assert(payload > 0);
    AppMessage *appMessage = new AppMessage();
    appMessage->setMsgType(HomaMsgType::REQUEST_MSG);
    appMessage->setByteLength(static_cast<uint32_t>(payload));

    // appMessage->setReqId(request_id_tuple.req_id_); req_id_ is not set by the app
    appMessage->setReqId(0);
    appMessage->setAppLevelId(request_id_tuple.app_level_id_);
    appMessage->setClientThreadId(request_id_tuple.cl_thread_id_);
    appMessage->setServerThreadId(request_id_tuple.sr_thread_id_);
    appMessage->setSrcAddr(request_id_tuple.cl_addr_);
    appMessage->setDestAddr(request_id_tuple.sr_addr_);
    appMessage->setMsgCreationTime(request_id_tuple.ts_);
    slog::log5(debug_, get_local_addr(),
               "Sending new request",
               "size(?) =", appMessage->getByteLength(),
               "src:", appMessage->getSrcAddr(),
               "dst:", appMessage->getDestAddr(),
               "req_id:", appMessage->getReqId(),
               "app_lvl_id:", appMessage->getAppLevelId(),
               "client thread:", appMessage->getClientThreadId(),
               "server thread:", appMessage->getServerThreadId(),
               "bytes on wire:", appMessage->getMsgBytesOnWire(),
               "Creation time:", appMessage->getMsgCreationTime());
    sxController.processSendMsgFromApp(appMessage);
}

/**
 * @brief Same as r2p2_send_req but for responses
 *
 * @param payload
 * @param request_id_tuple
 * @param new_n
 */
void HomaTransport::r2p2_send_response(int payload, const RequestIdTuple &request_id_tuple, int new_n)
{
    slog::log4(debug_, get_local_addr(), "HomaTransport::r2p2_send_response()");
    assert(payload > 0);
    AppMessage *appMessage = new AppMessage();
    appMessage->setMsgType(HomaMsgType::REPLY_MSG);
    appMessage->setByteLength(static_cast<uint32_t>(payload));
    appMessage->setReqId(0);
    appMessage->setAppLevelId(request_id_tuple.app_level_id_);
    appMessage->setClientThreadId(request_id_tuple.cl_thread_id_);
    appMessage->setServerThreadId(request_id_tuple.sr_thread_id_);
    appMessage->setSrcAddr(request_id_tuple.sr_addr_);
    appMessage->setDestAddr(request_id_tuple.cl_addr_);
    slog::log5(debug_, get_local_addr(),
               "Sending new reply",
               "size(?) =", appMessage->getByteLength(),
               "src:", appMessage->getSrcAddr(),
               "dst:", appMessage->getDestAddr(),
               "req_id:", appMessage->getReqId(),
               "app_lvl_id:", appMessage->getAppLevelId(),
               "client thread:", appMessage->getClientThreadId(),
               "server thread:", appMessage->getServerThreadId(),
               "bytes on wire:", appMessage->getMsgBytesOnWire(),
               "Creation time:", appMessage->getMsgCreationTime());
    sxController.processSendMsgFromApp(appMessage);
}

// This corresponds to HomaTransport::handleRecvdPkt(cPacket* pkt) from original code
// called when packets arrive from the network
void HomaTransport::recv(Packet *pkt, Handler *h)
{
    // Enter_Method_Silent();
    // hdr_homa *rxPkt = check_and_cast<hdr_homa *>(pkt);
    hdr_homa *rxPkt = hdr_homa::access(pkt);
    assert(rxPkt);
    slog::log5(debug_, get_local_addr(), "HomaTransport::recv() msg type:", rxPkt->msgType_var());
    if (rxPkt->pktType_var() == PktType::GRANT)
        slog::log6(debug_, get_local_addr(), "|Received: grant pkt for", rxPkt->appLevelId_var(),
                   "from", hdr_ip::access(pkt)->saddr(), "msg id", rxPkt->msgId_var(), "grant bytes", rxPkt->getGrantFields().grantBytes,
                   "offset",
                   rxPkt->getGrantFields().offset, "schedprio", rxPkt->getGrantFields().schedPrio);
    if (rxPkt->pktType_var() == PktType::REQUEST)
        slog::log6(debug_, get_local_addr(), "|Received: request pkt");
    if (rxPkt->pktType_var() == PktType::SCHED_DATA)
        slog::log6(debug_, get_local_addr(), "|Received: scheduled data packet");
    if (rxPkt->pktType_var() == PktType::UNSCHED_DATA)
        slog::log6(debug_, get_local_addr(), "|Received: un-scheduled data packet");
    // check and set the localAddr
    // if (get_local_addr == inet::L3Address())
    // {
    //     localAddr = rxPkt->getDestAddr();
    // }
    // else
    // {
    //     assert(localAddr == rxPkt->getDestAddr());
    // }

    // update the owner transport for this packet
    rxPkt->ownerTransport = this;
    // emit(priorityStatsSignals[rxPkt->getPriority()], rxPkt);

    uint32_t pktLenOnWire = hdr_homa::getBytesOnWire(rxPkt->getDataBytes(),
                                                     (PktType)rxPkt->getPktType());
    double pktDuration =
        (pktLenOnWire * 8.0 / homaConfig->nicLinkSpeed) / 1000.0 / 1000.0 / 1000.0;
    slog::log5(debug_, get_local_addr(), "pktLenOnWire =", pktLenOnWire, "pktDuration =", pktDuration);

    // update active period stats
    if (rxScheduler.getInflightBytes() == 0)
    {
        // We were not in an active period prior to this packet but entered in
        // one starting this packet.
        rxScheduler.activePeriodStart = Scheduler::instance().clock() - pktDuration;
        assert(rxScheduler.rcvdBytesPerActivePeriod == 0);
        rxScheduler.rcvdBytesPerActivePeriod = 0;
    }
    rxScheduler.rcvdBytesPerActivePeriod += pktLenOnWire;
    slog::log6(debug_, get_local_addr(), "rcvdBytesPerActivePeriod =", rxScheduler.rcvdBytesPerActivePeriod);

    // handle data or grant packets appropriately
    switch (rxPkt->getPktType())
    {
    case PktType::REQUEST:
    case PktType::UNSCHED_DATA:
    case PktType::SCHED_DATA:
        rxScheduler.processReceivedPkt(rxPkt);
        break;

    case PktType::GRANT:
        sxController.processReceivedGrant(rxPkt);
        break;

    default:
        throw std::runtime_error("Received packet type is not valid. type:" +
                                 rxPkt->getPktType());
    }
    slog::log6(debug_, get_local_addr(), "before freeing received pkt");

    if (hdr_homa::access(pkt)->unschedFields_var().prioUnschedBytes)
        delete hdr_homa::access(pkt)->unschedFields_var().prioUnschedBytes;
    Packet::free(pkt); // This is instead of deleting the header in processReceivedPkt() or processReceivedGrant()

    // Check if this is the end of active period and we should dump stats for
    // wasted bandwidth.
    if (rxScheduler.getInflightBytes() == 0)
    {
        // This is end of a active period, so we should dump the stats and reset
        // varibles that track next active period.
        // emit(rxActiveTimeSignal, simTime() - rxScheduler.activePeriodStart);
        // emit(rxActiveBytesSignal, rxScheduler.rcvdBytesPerActivePeriod);
        rxScheduler.rcvdBytesPerActivePeriod = 0;
        rxScheduler.activePeriodStart = Scheduler::instance().clock();
    }
}

void HomaTransport::handleMessage()
{
    slog::log4(debug_, get_local_addr(), "HomaTransport::handleMessage()");
    assert(0); // Will most likely not use this and instead call functions directly form timer expire() functions
    // Enter_Method_Silent();
    // if (msg->isSelfMessage())
    // {
    //     switch (msg->getKind())
    //     {
    //     case SelfMsgKind::START: // not needed
    //         processStart();
    //         break;
    //     case SelfMsgKind::GRANT: // covered (see grantTimer)
    //         rxScheduler.processGrantTimers(msg);
    //         break;
    //     case SelfMsgKind::SEND: // covered (see sendTimer)
    //         ASSERT(msg == sendTimer);
    //         sxController.handlePktTransmitEnd();
    //         sxController.sendOrQueue(msg);
    //         break;
    //     case SelfMsgKind::EMITTER: // not needed
    //         ASSERT(msg == emitSignalTimer);
    //         testAndEmitStabilitySignal();
    //         break;
    //     case SelfMsgKind::BW_CHECK: // covered (see schedBwUtilTimer)
    //         ASSERT(msg == rxScheduler.schedBwUtilTimer);
    //         rxScheduler.schedSenders->handleBwUtilTimerEvent(msg);
    //         break;
    //     }
    // }
    // else
    // {
    //     if (msg->arrivedOn("appIn")) // covered by R2p2 API functions
    //     {
    //         AppMessage *outMsg = check_and_cast<AppMessage *>(msg);
    //         // check and set the localAddr of this transport if this is the
    //         // first message arriving from applications.
    //         if (localAddr == inet::L3Address())
    //         {
    //             localAddr = outMsg->getSrcAddr();
    //         }
    //         else
    //         {
    //             ASSERT(localAddr == outMsg->getSrcAddr());
    //         }
    //         sxController.processSendMsgFromApp(outMsg);
    //     }
    //     else if (msg->arrivedOn("udpIn")) // covered by recv()
    //     {
    //         handleRecvdPkt(check_and_cast<cPacket *>(msg));
    //     }
    // }
}

int32_t HomaTransport::get_local_addr()
{
    return this_addr_;
}

int HomaTransport::command(int argc, const char *const *argv)
{
    Tcl &tcl = Tcl::instance();
    if (argc == 3)
    {
        if (strcmp(argv[1], "attach-agent") == 0)
        {
            HomaAgent *homa_agent = (HomaAgent *)TclObject::lookup(argv[2]);
            if (!homa_agent)
            {
                tcl.resultf("no such Homa agent", argv[2]);
                return (TCL_ERROR);
            }
            attach_homa_agent(homa_agent);
            return (TCL_OK);
        }
    }
    return (NsObject::command(argc, argv));
}

void HomaTransport::attach_homa_agent(HomaAgent *homa_agent)
{
    slog::log2(debug_, -1, "HomaTransport::attach_homa_agent() with saddr:", homa_agent->addr(), "and daddr:", homa_agent->daddr());
    // slog::log2(debug_, -1, "HomaTransport::attach_homa_agent() target saddr", dynamic_cast<HomaAgent *>(homa_agent->target())->addr(), "and target daddr:", dynamic_cast<HomaAgent *>(homa_agent->target())->daddr());
    assert(homa_agents_.count(homa_agent->daddr()) == 0);
    homa_agents_[homa_agent->daddr()] = homa_agent;
    homa_agent->attach_homa_transport(this);
    this_addr_ = homa_agent->addr();
    homaConfig->this_addr_ = this_addr_;
}

// Send a request according to Homa code.
// make sendcontroller work and outboundMsg *work
// start from processSendMsgFromApp - last action was getting the unscheduledByteAllocator working in line
// 119 of homa-send-controller.cc

// Must set vaues in config depot

// Start from adding PrioirtyResolver (line 63 of homa-send-controller.cc)
// Goal is to use InitSendController (nothing done on this other than copying it in (and copied prio resolver code))