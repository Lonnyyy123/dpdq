/**
 * @brief This file is taken almost as-is from https://github.com/PlatformLab/HomaSimulation/tree/omnet_simulations/RpcTransportDesign/OMNeT%2B%2BSimulation
 */

#include <iostream>
#include <sstream>
#include "app-message.h"
#include "scheduler.h"

// Template rule which fires if a struct or class doesn't have operator<<
template <typename T>
inline std::ostream &operator<<(std::ostream &out, const T &) { return out; }

AppMessage::AppMessage() : HomaEvent(HomaEventType::APP_MSG)
{
    this->reqId_var = 0;
    this->appLevelId_var = -1;
    this->msgType_var = HomaMsgType::UNDEF_HOMA_MSG_TYPE;
    this->clientThreadId_var = -1;
    this->serverThreadId_var = -1;
    this->transportSchedDelay_var = 0;
    this->transportSchedPreemptionLag_var = 0;
    this->msgBytesOnWire_var = 0;
    this->byteLength_var = 0;
    this->msgCreationTime_var = Scheduler::instance().clock();
}

AppMessage::AppMessage(const AppMessage &other)
{
    copy(other);
}

AppMessage::~AppMessage()
{
}

AppMessage &AppMessage::operator=(const AppMessage &other)
{
    if (this == &other)
        return *this;
    // ::cPacket::operator=(other);
    copy(other);
    return *this;
}

void AppMessage::copy(const AppMessage &other)
{
    this->reqId_var = other.reqId_var;
    this->appLevelId_var = other.appLevelId_var;
    this->msgType_var = other.msgType_var;
    this->clientThreadId_var = other.clientThreadId_var;
    this->serverThreadId_var = other.serverThreadId_var;
    this->destAddr_var = other.destAddr_var;
    this->srcAddr_var = other.srcAddr_var;
    this->msgCreationTime_var = other.msgCreationTime_var;
    this->transportSchedDelay_var = other.transportSchedDelay_var;
    this->transportSchedPreemptionLag_var = other.transportSchedPreemptionLag_var;
    this->msgBytesOnWire_var = other.msgBytesOnWire_var;
    this->byteLength_var = other.byteLength_var;
}

// addresses in ns-2 are int32_t
int32_t &AppMessage::getDestAddr()
{
    return destAddr_var;
}

void AppMessage::setDestAddr(const int32_t &destAddr)
{
    this->destAddr_var = destAddr;
}

int32_t &AppMessage::getSrcAddr()
{
    return srcAddr_var;
}

void AppMessage::setSrcAddr(const int32_t &srcAddr)
{
    this->srcAddr_var = srcAddr;
}

double AppMessage::getMsgCreationTime() const
{
    return msgCreationTime_var;
}

void AppMessage::setMsgCreationTime(double msgCreationTime)
{
    this->msgCreationTime_var = msgCreationTime;
}

double AppMessage::getTransportSchedDelay() const
{
    return transportSchedDelay_var;
}

void AppMessage::setTransportSchedDelay(double transportSchedDelay)
{
    this->transportSchedDelay_var = transportSchedDelay;
}

double AppMessage::getTransportSchedPreemptionLag() const
{
    return transportSchedPreemptionLag_var;
}

void AppMessage::setTransportSchedPreemptionLag(double transportSchedPreemptionLag)
{
    this->transportSchedPreemptionLag_var = transportSchedPreemptionLag;
}

long AppMessage::getMsgBytesOnWire() const
{
    return msgBytesOnWire_var;
}

void AppMessage::setMsgBytesOnWire(long msgBytesOnWire)
{
    this->msgBytesOnWire_var = msgBytesOnWire;
}

uint32_t AppMessage::getByteLength() const
{
    return byteLength_var;
}

void AppMessage::setByteLength(uint32_t byteLength)
{
    this->byteLength_var = byteLength;
}

HomaMsgType AppMessage::getMsgType() const
{
    return msgType_var;
}

void AppMessage::setMsgType(HomaMsgType msgType)
{
    this->msgType_var = msgType;
}

request_id AppMessage::getReqId() const
{
    return reqId_var;
}

void AppMessage::setReqId(request_id req_id)
{
    this->reqId_var = req_id;
}

long AppMessage::getAppLevelId() const
{
    return appLevelId_var;
}

void AppMessage::setAppLevelId(long id)
{
    this->appLevelId_var = id;
}

int AppMessage::getClientThreadId() const
{
    return clientThreadId_var;
}

void AppMessage::setClientThreadId(int thread_id)
{
    this->clientThreadId_var = thread_id;
}

int AppMessage::getServerThreadId() const
{
    return serverThreadId_var;
}

void AppMessage::setServerThreadId(int thread_id)
{
    this->serverThreadId_var = thread_id;
}
