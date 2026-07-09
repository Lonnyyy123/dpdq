//
// Copyright (C) 2004 Andras Varga
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU Lesser General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this program; if not, see <http://www.gnu.org/licenses/>.
//

#include "./MultiSocketBasicClient.h"
#include "inet/applications/common/SocketTag_m.h"

#include "inet/applications/tcpapp/GenericAppMsg_m.h"
#include "inet/common/ModuleAccess.h"
#include "inet/common/lifecycle/ModuleOperations.h"
#include "inet/common/packet/Packet.h"
#include "inet/common/TimeTag_m.h"
#include <iostream>
#include <fstream>
#include <sqlite3.h>
#include <list>
#include <random>

using namespace inet;

#define MSGKIND_CONNECT 0
#define MSGKIND_SEND 1
#define MSGKIND_INCAST 2

#define MAX_PORT_NUM 65450 // 65535 - 85

#define WARMUP_GAP_US 100.0

Define_Module(MultiSocketBasicClient);

simsignal_t MultiSocketBasicClient::flowEndedSignal = registerSignal("flowEnded");
simsignal_t MultiSocketBasicClient::flowEndedQueryIDSignal = registerSignal("flowEndedQueryID");
simsignal_t MultiSocketBasicClient::flowStartedSignal = registerSignal("flowStarted");
simsignal_t MultiSocketBasicClient::actualFlowStartedTimeSignal = registerSignal("actualFlowStartedTime");
simsignal_t MultiSocketBasicClient::requestSentSignal = registerSignal("requestSent");
simsignal_t MultiSocketBasicClient::requestSizeSignal = registerSignal("requestSize");
simsignal_t MultiSocketBasicClient::notJitteredRequestSentSignal = registerSignal("notJitteredRequestSent");
simsignal_t MultiSocketBasicClient::replyLengthsSignal = registerSignal("replyLengths");
simsignal_t MultiSocketBasicClient::chunksReceivedLengthSignal = registerSignal("chunksReceivedLength");
simsignal_t MultiSocketBasicClient::chunksReceivedTotalLengthSignal = registerSignal("chunksReceivedTotalLength");

MultiSocketBasicClient::~MultiSocketBasicClient()
{
    delete send_interval;
    delete dst_thread_gen;
    delete dst_ids;
    delete req_size;
    delete resp_size;
    for (auto it = dst_to_queued_requests.begin(); it != dst_to_queued_requests.end(); it++)
    {
        delete it->second;
    }
}

void MultiSocketBasicClient::initialize(int stage)
{
    MultiSocketTcpAppBase::initialize(stage);
    if (stage == INITSTAGE_LOCAL)
    {
        earlySend = false; // TBD make it parameter
        WATCH(earlySend);

        startTime = par("startTime");
        stopTime = par("stopTime");
        sendTime = par("sendTime");
        activeClient = par("activeClient");
        logicalIdx = par("logicalIdx");

        // -------------------- Additions ---------------------------
        // Workload parameters
        request_size_B = par("request_size_B");
        response_size_B = par("response_size_B");
        request_interval_sec = par("request_interval_sec");
        num_client_apps = par("num_client_apps");
        num_server_apps = par("num_server_apps");
        tcp_connections_per_thread_pair = par("tcp_connections_per_thread_pair");
        incast_size = par("incast_size");
        incast_request_size_bytes = par("incast_request_size_bytes");
        incast_interval_sec = par("incast_interval_sec");
        enable_incast = par("enable_incast");

        checkParams();

        // -------------------- Additions ---------------------------

        is_bursty = par("is_bursty");
        is_mice_background = par("is_mice_background");

        if (int(is_bursty) + int(is_mice_background) > 1)
            throw cRuntimeError("Two roles chosen for one app.");

        background_inter_arrival_time_multiplier = par("background_inter_arrival_time_multiplier");
        background_flow_size_multiplier = par("background_flow_size_multiplier");
        bursty_inter_arrival_time_multiplier = par("bursty_inter_arrival_time_multiplier");
        bursty_flow_size_multiplier = par("bursty_flow_size_multiplier");

        if (stopTime >= SIMTIME_ZERO && stopTime < startTime)
            throw cRuntimeError("Invalid startTime/stopTime parameters");
        repetition_num = atoi(getEnvir()->getConfigEx()->getVariable(CFGVAR_REPETITION));
        app_index = getIndex();
        parent_index = getParentModule()->getIndex();

        num_requests_per_burst = par("num_requests_per_burst");
    }
}

void MultiSocketBasicClient::checkParams()
{
    std::cout << "Parameters provided to MultiSocketBasicClient: " << request_size_B
              << " " << response_size_B
              << " " << request_interval_sec
              << " " << num_client_apps
              << " " << num_server_apps
              << " " << tcp_connections_per_thread_pair
              << " " << incast_size
              << " " << incast_request_size_bytes
              << " " << incast_interval_sec
              << " " << enable_incast
              << " " << std::endl;
    if (request_size_B == 0 ||
        response_size_B == 0 ||
        // request_interval_sec <= 0.000000000001 || // can be 0 if incast is 100%
        num_client_apps == -1 ||
        num_server_apps == -1 ||
        tcp_connections_per_thread_pair == -1 ||
        incast_size == -1 ||
        incast_request_size_bytes == -1 ||
        enable_incast == -1)
    {
        throw std::invalid_argument("Some parameter is not initialized");
    }
}

void MultiSocketBasicClient::handleStartOperation(LifecycleOperation *operation)
{
    if (!activeClient)
        return;
    // std::cout << simTime() << " SEPEHR: handleStartOperation called." << std::endl;

    std::string req_interval_distr = par("req_interval_distr");
    std::string req_target_distr = par("req_target_distr");
    request_target_distr = req_target_distr;
    std::string manual_req_interval_file = par("manual_req_interval_file");
    std::string req_size_distr = par("req_size_distr");
    std::string resp_size_distr = par("resp_size_distr");

    std::cout << parent_index << " active client: " << activeClient << " " << simTime() << " Creating a workload with the following parameters: "
              << startTime << " "
              << stopTime << " "
              << sendTime << " "
              << req_interval_distr << " "
              << req_target_distr << " "
              << manual_req_interval_file << " "
              << req_size_distr << " "
              << resp_size_distr << " "
              << logicalIdx << " "
              << std::endl;
    std::string distributions_base_root = par("distibutions_base_root");

    // Sever targets
    std::string serverTargets = par("targetServers");

    // std::cout << "serverTargets " << serverTargets << std::endl;

    if (serverTargets == "")
        throw cRuntimeError("Server targets not specified");

    dst_ids = new std::vector<int32_t>();
    size_t pos = 0;
    int server;
    std::string delim = "_";
    serverTargets += delim;
    while ((pos = serverTargets.find(delim)) != std::string::npos)
    {
        server = std::stoi(serverTargets.substr(0, pos));
        serverTargets.erase(0, pos + delim.length());
        if (server == getParentModule()->getIndex())
            continue;
        // std::cout << "Pushing back server " << server << std::endl;
        dst_ids->push_back(server); // 0 - 143 (excl self)
    }

    // create a pool of connections to servers (if active client)
    if (activeClient)
    {
        simtime_t now = simTime();
        double lower_bound = 0;
        double upper_bound = WARMUP_GAP_US;
        std::uniform_real_distribution<double> unif(lower_bound, upper_bound);
        std::mt19937 gen;
        gen.seed(parent_index);
        for (int server : *dst_ids)
        {
            for (size_t i = 0; i < tcp_connections_per_thread_pair; i++)
            {
                cMessage *msg = new cMessage("pool");
                msg->setKind(MSGKIND_CONNECT);
                msg->addPar("dest_server_idx");
                msg->addPar("connect_port");
                msg->par("dest_server_idx").setLongValue(server);
                msg->par("connect_port").setLongValue(80);
                double noise = unif(gen); // in us
                // std::cout << "Noise: " << noise << std::endl;
                scheduleAt(simTime().dbl() + (i + 1) * (WARMUP_GAP_US / 1000.0 / 1000.0) + (noise / 1000.0 / 1000.0), msg);
            }
            // create request queue
            L3Address destination;
            // std::cout << "Pool est, asking for conversion of " << server << std::endl;
            idxToL3Addr(server, destination);
            if (dst_to_queued_requests.find(destination.str()) == dst_to_queued_requests.end())
            {
                // std::cout << parent_index << " Creating request queue for " << destination.str() << std::endl;
                queued_requests_t *req_q = new queued_requests_t();
                dst_to_queued_requests[destination.str()] = req_q;
            }
        }
    }

    // Request interval distribution
    if (req_interval_distr == "exponential")
    {
        std::cout << "Setting exp distr for parent_idx " << parent_index << ", log idx: " << logicalIdx << " to " << logicalIdx + logicalIdx * 133 + 1 << std::endl;
        send_interval = new ExpDistr(request_interval_sec.dbl(), logicalIdx + logicalIdx * 133 + 1);
    }
    else if (req_interval_distr == "fixed")
    {
        send_interval = new FixedDistr(request_interval_sec.dbl());
    }
    else if (req_interval_distr == "manual")
    {
        send_interval = new ManualDistr(manual_req_interval_file.c_str(), logicalIdx, 0);
    }
    else
    {
        throw std::invalid_argument("not supported request interval distribution: " + req_interval_distr);
    }

    // Target server distribution
    if (req_target_distr == "uniform")
    {
        // TODO!!: I am getting segfault if not all hosts are servers
        // TODO: This is likely not general enough. ns-2 uses the actual addresses of all servers (located in dst_ids_) and then selects a random
        // index from the vector (ie a rand address). Therefore the servers can have addresses 5,6,7,8 (they don't have to start at 0).
        // The server addresses are available as a list in the generated tcl parameter file (assuming the same address formating is used)
        std::cout << "Setting uni target distr for parent_idx " << parent_index << ", log idx: " << logicalIdx << " to " << logicalIdx << std::endl;
        dst_thread_gen = new UnifTargetIntDistr(dst_ids->size() - 1, dst_ids, logicalIdx);
    }
    else if (req_target_distr == "manual")
    {
        dst_thread_gen = new ManualDistr(manual_req_interval_file.c_str(), logicalIdx, 1);
    }
    else
    {
        throw std::invalid_argument("not supported request target distribution: " + req_target_distr);
    }

    // Request size distribution
    if (req_size_distr == "fixed")
    {
        req_size = new FixedDistr(request_size_B);
    }
    else if (req_size_distr == "w1")
    {
        req_size = new W1Distr(logicalIdx + logicalIdx * 133 + 1, request_size_B);
    }
    else if (req_size_distr == "w2")
    {
        req_size = new W2Distr(logicalIdx + logicalIdx * 133 + 1, request_size_B);
    }
    else if (req_size_distr == "w3")
    {
        req_size = new W3Distr(logicalIdx + logicalIdx * 133 + 1, request_size_B);
    }
    else if (req_size_distr == "w4")
    {
        std::cout << "Setting w4 distr for parent_idx " << parent_index << ", log idx: " << logicalIdx << " to " << logicalIdx + logicalIdx * 133 + 1 << std::endl;
        req_size = new W4Distr(logicalIdx + logicalIdx * 133 + 1, request_size_B);
    }
    else if (req_size_distr == "w5")
    {
        req_size = new W5Distr(logicalIdx + logicalIdx * 133 + 1, request_size_B);
    }
    else if (req_size_distr == "manual")
    {
        req_size = new ManualDistr(manual_req_interval_file.c_str(), logicalIdx, 2);
    }

    // Response size distribution - deprecated
    resp_size = new FixedDistr(2);

    // -------------------------------------------------------------------------------

    // common bursty and bg values
    std::string rep_num_string = std::to_string(repetition_num);
    std::string application_category = par("application_category");
    std::string flow_multiplier;
    std::string inter_multiplier;
    std::string inter_arrival_db_name;
    std::string flow_size_db_name;

    // bursty values
    std::string num_requests_per_burst_string;
    std::string reply_length_string;
    std::string bursty_server_db_name;
    std::string bursty_flow_ids_db_name;
    std::string bursty_query_ids_db_name;

    // background values
    std::string background_server_db_name;
    std::string background_flow_ids_db_name;

    // common bursty and bg values
    flow_multiplier = std::to_string(background_flow_size_multiplier);
    inter_multiplier = std::to_string(background_inter_arrival_time_multiplier);

    // bursty values
    num_requests_per_burst_string = "";
    reply_length_string = "";

    // background values
    // none

    // do nothing if the app was indicated as bursty but then changed to not bursty by reading from stay_bursty_db
    if (!is_bursty && !is_mice_background)
        return;

    simtime_t start = 0;
    if (is_bursty && inter_arrival_times.empty())
        return;
    double next = startTime.dbl() + send_interval->get_next();

    // std::cout << "start connect_for_background_request. next sched at " << next << " par inx " << parent_index << std::endl;
    if (((stopTime < SIMTIME_ZERO) || (start < stopTime) || (start == stopTime && startTime == stopTime)))
    {
        if (request_interval_sec > 0.0)
        {
            // std::cout << "scheduling initial request dispatch" << std::endl;
            rescheduleOrDeleteTimer(next, MSGKIND_SEND);
        }
        if (enable_incast)
        {
            IncastGenerator::init(num_client_apps, num_server_apps, incast_request_size_bytes, incast_size);
            double next_incast = startTime.dbl() + incast_interval_sec.dbl();
            // std::cout << "FIRST NEXT INCAST " << next_incast << " startTime.dbl() " << startTime.dbl() << " simTime().dbl() " << simTime().dbl() << " incast_interval_sec.dbl() " << incast_interval_sec.dbl() << std::endl;
            rescheduleOrDeleteTimer(next_incast, MSGKIND_INCAST);
        }
    }
}

int MultiSocketBasicClient::get_local_port()
{
    int local_port_pad = getParentModule()->par("local_padding");
    getParentModule()->par("local_padding") = (local_port_pad + 1) % MAX_PORT_NUM;
    int local_port_test = getParentModule()->par("local_padding");
    if (local_port_test != (local_port_pad + 1) % MAX_PORT_NUM)
    {
        throw cRuntimeError("The improbable situation has happened!. Multiple threads reading local_pad and increasing"
                            "at the same time.");
    }
    // std::cout << "Using local port " << 80 + local_port_pad << " for server[" << getParentModule()->getIndex() << "].app[" << getIndex() << "]" << std::endl;
    // EV << "Using local port " << 80 + local_port_pad << " for server["
    //    << getParentModule()->getIndex() << "].app[" << getIndex() << "]" << endl;
    return 80 + local_port_pad;
}

void MultiSocketBasicClient::connect_for_background_request()
{
    throw cRuntimeError("connect_for_background_request() should not be called");
}

void MultiSocketBasicClient::handleTimer(cMessage *msg)
{
    switch (msg->getKind())
    {
    case MSGKIND_CONNECT:
    {
        if (simTime() > startTime)
        {
            throw cRuntimeError("Attempted to create pool connection after the workload starts");
        }
        int server = msg->par("dest_server_idx").longValue();
        int connect_port = msg->par("connect_port").longValue();
        connect(get_local_port(), server, connect_port, startTime);
        delete msg;
        break;
    }
    case MSGKIND_SEND:
    {
        // std::cout << "received MSGKIND_SEND" << std::endl;
        sendRequest();
        delete msg;
        break;
    }
    case MSGKIND_INCAST:
    {
        sendIncast();
        delete msg;
        break;
    }
    default:
        throw cRuntimeError("Invalid timer msg: kind=%d", msg->getKind());
    }
}

void MultiSocketBasicClient::socketEstablished(TcpSocket *socket)
{
    // std::cout << simTime() << " socketEstablished" << std::endl;
    MultiSocketTcpAppBase::socketEstablished(socket);
    if (simTime() > startTime)
    {
        throw cRuntimeError("MultiSocketBasicClient::socketEstablished() should not happen after the warmup time");
    }
}

void MultiSocketBasicClient::handleStopOperation(LifecycleOperation *operation)
{
    // EV << "MultiSocketBasicClient::handleStopOperation called." << endl;
    // std::cout << "MultiSocketBasicClient::handleStopOperation called." << std::endl;
    for (auto socket_pair : socket_map.getMap())
    {
        TcpSocket *socket = check_and_cast_nullable<TcpSocket *>(socket_pair.second);
        if (socket->getState() == TcpSocket::CONNECTED || socket->getState() == TcpSocket::CONNECTING || socket->getState() == TcpSocket::PEER_CLOSED)
            close(socket->getSocketId());
    }
}

void MultiSocketBasicClient::handleCrashOperation(LifecycleOperation *operation)
{
    // EV << "MultiSocketBasicClient::handleCrashOperation." << endl;
    if (operation->getRootModule() != getContainingNode(this))
    {
        for (auto socket_pair : socket_map.getMap())
        {
            TcpSocket *socket = check_and_cast_nullable<TcpSocket *>(socket_pair.second);
            socket->destroy();
        }
    }
}

void MultiSocketBasicClient::sendIncast()
{
    IncastGenerator::BurstInfo binfo = IncastGenerator::should_send(logicalIdx);
    // std::cout << "INCAST TIMER " << logicalIdx << " | " << binfo.incast_target_ << " " << binfo.req_size_ << " " << binfo.should_send_ << std::endl;
    if (binfo.should_send_)
    {
        RequestIdTuple ridt = RequestIdTuple(binfo.incast_target_, binfo.req_size_, 4, true);
        sendRequest(&ridt, true);
    }

    double next_incast = simTime().dbl() + incast_interval_sec.dbl();
    // std::cout << "NEXT INCAST " << next_incast << " startTime.dbl() " << startTime.dbl() << " simTime().dbl() " << simTime().dbl() << " incast_interval_sec.dbl() " << incast_interval_sec.dbl() << std::endl;
    rescheduleOrDeleteTimer(next_incast, MSGKIND_INCAST);
}

void MultiSocketBasicClient::sendRequest(RequestIdTuple *rit, bool incast)
{
    int target;
    long request_length;
    long reply_length;

    if (incast)
    {
        if (!rit)
            throw std::runtime_error("Meant to send incast request byt rit is null");
        target = rit->target;
        request_length = rit->req_len;
        reply_length = rit->rep_len;
    }
    else if (rit != nullptr)
    {
        // This is a dequeued request
        target = rit->target;
        request_length = rit->req_len;
        reply_length = rit->rep_len;
        delete rit;
    }
    else
    {
        target = dst_thread_gen->get_next();
        request_length = req_size->get_next();
        reply_length = resp_size->get_next();
    }
    // std::cout << logicalIdx << " SEND REQ to " << target << std::endl;

    L3Address destination;
    // convert from 0,1,2,3 to location-specific index
    int physical_target;
    try
    {
        // special case hack
        // For this to work, in manual distr, all hosts must be designated as servers and as clients.
        // It should not matter since the send instructions are manual anyway
        if (request_target_distr == "manual")
        {
            if (target > logicalIdx)
            {
                physical_target = dst_ids->at(target - 1);
            }
            else if (target < logicalIdx)
            {
                physical_target = dst_ids->at(target);
            }
            else
            {
                throw cRuntimeError("Manual target distribution attempted to send to self");
            }
        }
        else
        {
            physical_target = target; // target comes from dst_ids (dst_ids holds physical indexes)
        }
    }
    catch (const std::out_of_range &e)
    {
        std::cerr << "Out of range when accessing dst_ids. Size " << dst_ids->size() << " target: " << target << " logicalIdx: " << logicalIdx << " local idx: " << parent_index << " request_target_distr: " << request_target_distr << ". " << e.what() << '\n';
        throw;
    }

    // std::cout << simTime() << " " << parent_index << " sendRequest, physical target " << physical_target << " target: " << target << std::endl;
    idxToL3Addr(physical_target, destination);
    // get socket from pool
    TcpSocket *socket = removeSocketFromPool(destination.str());
    if (socket == nullptr)
    {
        // enqueue request until a connection to specific destination is free
        RequestIdTuple tup = RequestIdTuple(target, request_length, reply_length, incast);
        queued_requests_t *rq = nullptr;
        try
        {
            rq = dst_to_queued_requests.at(destination.str());
        }
        catch (const std::out_of_range &e)
        {
            std::cerr << "Failed to find req queue for destination: " << destination.str() << e.what() << std::endl;
            throw;
        }

        rq->push(tup);
        // std::cout << "enqueued request. Q len is " << rq->size() << std::endl;
        return;
    }
    long socket_id = socket->getSocketId();
    // std::cout << getFullPath() << " SEND REQUEST to " << physical_target << " . Socket id: " << socket_id << std::endl;

    if (request_length < 1)
        throw cRuntimeError("request_length < 1, are you sure?");
    if (reply_length < 1)
        throw cRuntimeError("reply_length < 1, are you sure?");

    const auto &payload = makeShared<GenericAppMsg>();
    Packet *packet = new Packet("data");
    packet->addTag<SocketInd>()->setSocketId(socket_id);

    payload->setChunkLength(B(request_length));
    payload->setExpectedReplyLength(B(reply_length));
    payload->setServerClose(false);
    payload->addTag<CreationTimeTag>()->setCreationTime(simTime());
    payload->setRequesterID(MultiSocketTcpAppBase::requestsSent++);
    payload->setQuery_id(parent_index); // Hijacking query_id to provide src index. It is a bit confusing as it but I don't want to tucj RequesterID.
    payload->setRequested_time(simTime());
    payload->setIs_micro_burst_flow(is_bursty);
    packet->insertAtBack(payload);

    // EV << "sending request with " << request_length << " bytes, expected reply length " << reply_length << " bytes\n";
    // std::cout << parent_index << " sending request with " << request_length << " bytes, expected reply length " << reply_length << " bytes" << std::endl;
    // EV << "SEPEHR: sending request with request ID: " << payload->getRequesterID() << endl;
    // std::cout << "SEPEHR: sending request with request ID: " << payload->getRequesterID() << std::endl;

    emit(requestSentSignal, payload->getRequesterID());
    emit(requestSizeSignal, request_length);
    emit(replyLengthsSignal, reply_length);

    if (!incast)
    {
        // schedule next request
        // std::cout << "Scheduling non incast request" << std::endl;
        double next = simTime().dbl() + send_interval->get_next();
        rescheduleOrDeleteTimer(next, MSGKIND_SEND);
    }
    // if (is_bursty)
    //     emit(flowEndedQueryIDSignal, payload->getQuery_id());
    // std::cout << "SEND REQUEST request_length:" << request_length << std::endl;

    sendPacket(packet);
}

void MultiSocketBasicClient::rescheduleOrDeleteTimer(simtime_t d, short int msgKind, long socket_id)
{
    if (stopTime < SIMTIME_ZERO || d < stopTime)
    {
        cMessage *timeoutMsg = new cMessage("timer");
        timeoutMsg->setKind(msgKind);
        if (socket_id >= 0)
            timeoutMsg->addPar("socket_id") = socket_id;
        // EV << "SEPEHR: rescheduleOrDeleteTimer is called to schedule to send a packet at " << d << "s" << endl;
        scheduleAt(d, timeoutMsg);
    }
}

void MultiSocketBasicClient::socketDataArrived(TcpSocket *socket, Packet *msg, bool urgent)
{
    // EV << "SEPEHR: Message rcved: " << msg << endl;
    // std::cout << "MultiSocketBasicClient::socketDataArrived()" << std::endl;
    simtime_t region_creation_time = msg->getTag<RegionCreationTimeTag>()->getRegionCreationTime();
    auto msg_dup = msg->dup();
    auto chunk = msg_dup->removeAtFront<SliceChunk>();
    bool should_close = false;
    int socket_id = socket->getSocketId();
    while (true)
    {
        auto chunk_length = chunk->getLength();
        auto chunk_offset = chunk->getOffset();
        auto main_chunk = chunk->getChunk();
        auto total_length = main_chunk->getChunkLength();
        auto history_found = chunk_length_keeper.find(socket_id);
        auto total_length_found = total_length_keeper.find(socket_id);
        if (history_found != chunk_length_keeper.end())
        {
            // Some data has been received for this chunk before
            if (total_length_found == total_length_keeper.end())
                throw cRuntimeError("How do we have a record for a chunk "
                                    "but not a record for its total data length!");
            history_found->second += chunk_length;
        }
        else
        {
            // This is the first data received for this chunk
            if (total_length_found != total_length_keeper.end())
                throw cRuntimeError("How do we have a record for the total data length of a chunk "
                                    "but not a record of the chunk itself!");
            chunk_length_keeper.insert(std::pair<long, b>(socket_id, chunk_length));
            total_length_keeper.insert(std::pair<long, b>(socket_id, total_length));
        }
        if (chunk_offset == b(0))
        {
            Packet *temp = new Packet();
            temp->insertAtBack(main_chunk);
            auto payload = temp->popAtFront<GenericAppMsg>();
            // start time should actually be the time that the first packet (whether re-ordered or not is received)
            if (payload->getRequested_time() > region_creation_time)
                throw cRuntimeError("How can the response be received before request is sent.");
            simtime_t actual_start_time = std::min(region_creation_time, simTime());
            // std::cout << "region_creation_time: " << region_creation_time << " simTime() " << simTime() << std::endl;
            emit(flowStartedSignal, payload->getRequesterID());
            emit(actualFlowStartedTimeSignal, actual_start_time);
            delete temp;
        }
        if (chunk_length + chunk_offset == total_length)
        {
            Packet *temp = new Packet();
            temp->insertAtBack(main_chunk);
            auto payload = temp->popAtFront<GenericAppMsg>();
            // End time should be exactly the time the packet is received even if there was a reordering
            emit(flowEndedSignal, payload->getRequesterID());

            auto chunk_found = chunk_length_keeper.find(socket_id);
            auto total_length_found = total_length_keeper.find(socket_id);
            if (chunk_found == chunk_length_keeper.end() || total_length_found == total_length_keeper.end())
                throw cRuntimeError("chunk_found == chunk_length_keeper.end() || "
                                    "total_length_found == total_length_keeper.end()");
            emit(chunksReceivedLengthSignal, chunk_found->second.get());
            emit(chunksReceivedTotalLengthSignal, total_length_found->second.get());
            chunk_length_keeper.erase(socket_id);
            total_length_keeper.erase(socket_id);
            // std::cout << "The whole response of total len " << total_length << " has been received" << std::endl;
            should_close = true;
            delete temp;
        }
        else if (chunk_length + chunk_offset > total_length)
            throw cRuntimeError("chunk_length + chunk_offset > total_length");
        if (msg_dup->getByteLength() == 0)
            break;
        chunk = msg_dup->removeAtFront<SliceChunk>();
    }

    delete msg_dup;
    MultiSocketTcpAppBase::socketDataArrived(socket, msg, urgent);
    EV_INFO << "reply arrived\n";
    if (should_close)
    {
        // EV << "SEPEHR: Last packet arrived. Closing the socket." << endl;
        // std::cout << "SEPEHR: Last packet arrived. Adding the socket back to the pool" << std::endl;
        std::string remote_addr = socket->getRemoteAddress().str();
        returnSocketToPool(remote_addr, socket);

        // if there are queued requests waiting for a socket, send them
        queued_requests_t *rq = nullptr;
        try
        {
            rq = dst_to_queued_requests.at(remote_addr);
        }
        catch (const std::out_of_range &e)
        {
            std::cerr << "Failed to find request queue for destination: " << remote_addr << " " << e.what() << std::endl;
            throw;
        }
        if (rq->empty())
            return;
        RequestIdTuple *tup = new RequestIdTuple(rq->front());
        rq->pop();
        sendRequest(tup, tup->incast);
    }
    else if (simTime() >= stopTime && socket->getState() != TcpSocket::LOCALLY_CLOSED)
    {
        EV_INFO << "reply to last request arrived, closing session\n";
        // std::cout << "reply to last request arrived, closing session" << std::endl;
        close(socket->getSocketId());
    }
}

void MultiSocketBasicClient::close(int socket_id)
{
    // EV << "SEPEHR: closing the socket in server[" << parent_index << "].app["
    //    << app_index << "] with socket ID " << socket_id
    //    << ". Time = " << simTime() << endl;
    // std::cout << "SEPEHR: closing the socket in server[" << parent_index << "].app["<< app_index << "] with socket ID " << socket_id<< ". Time = " << simTime() << std::endl;

    if (simTime() < stopTime)
        throw cRuntimeError("I don't want sockets to close during the experiment");
    MultiSocketTcpAppBase::close(socket_id);
}

void MultiSocketBasicClient::finish()
{
    for (std::unordered_map<long, b>::iterator it = chunk_length_keeper.begin();
         it != chunk_length_keeper.end(); it++)
    {
        auto total_length_found = total_length_keeper.find(it->first);
        if (total_length_found == total_length_keeper.end())
        {
            throw cRuntimeError("Mismatch between chunk_length_keeper and total_length_keeper");
        }
        emit(chunksReceivedLengthSignal, it->second.get());
        emit(chunksReceivedTotalLengthSignal, total_length_found->second.get());
    }
    chunk_length_keeper.clear();
    total_length_keeper.clear();
    MultiSocketTcpAppBase::finish();
}

void MultiSocketBasicClient::socketClosed(TcpSocket *socket)
{
    EV << "SEPEHR: Socket closed in server[" << parent_index << "].app["
       << app_index << "] with socket ID " << socket->getSocketId()
       << ". Time = " << simTime() << endl;
    MultiSocketTcpAppBase::socketClosed(socket);
}

void MultiSocketBasicClient::socketFailure(TcpSocket *socket, int code)
{
    MultiSocketTcpAppBase::socketFailure(socket, code);
}
