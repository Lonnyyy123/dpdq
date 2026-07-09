//
// Generated file, do not edit! Created by nedtool 4.6 from application/AppMessage.msg.
//
#ifndef _APPMESSAGE_H_
#define _APPMESSAGE_H_

#include <config.h> // for int32_t
#include <homa-timers.h>
#include "r2p2-hdr.h"

enum HomaMsgType
{
    UNDEF_HOMA_MSG_TYPE = 0, // error - default for AppMessage and hdr_homa
    REQUEST_MSG = 1,
    REPLY_MSG = 2,
    OTHER_MSG = 3 // eg, control
};

/**
 * @brief This file is taken almost as-is from https://github.com/PlatformLab/HomaSimulation/tree/omnet_simulations/RpcTransportDesign/OMNeT%2B%2BSimulation
 */

class AppMessage : public HomaEvent
{
protected:
    int32_t destAddr_var;
    int32_t srcAddr_var;
    double msgCreationTime_var;
    double transportSchedDelay_var;
    double transportSchedPreemptionLag_var;
    long msgBytesOnWire_var;
    uint32_t byteLength_var; // from inet/src/inet/networklayer/ipv6/IPv6ExtensionHeaders.cc

    // Added for r2p2-app compatibility
    request_id reqId_var;
    long appLevelId_var;
    int clientThreadId_var;
    int serverThreadId_var;

    HomaMsgType msgType_var;

private:
    void copy(const AppMessage &other);

protected:
    // protected and unimplemented operator==(), to prevent accidental usage
    bool operator==(const AppMessage &);

public:
    AppMessage();
    AppMessage(const AppMessage &other);
    virtual ~AppMessage();
    AppMessage &operator=(const AppMessage &other);
    virtual AppMessage *dup() const { return new AppMessage(*this); }
    // virtual void parsimPack(cCommBuffer *b);
    // virtual void parsimUnpack(cCommBuffer *b);

    // addresses in ns-2 are int32_t
    // field getter/setter methods
    virtual int32_t &getDestAddr();
    virtual const int32_t &getDestAddr() const { return const_cast<AppMessage *>(this)->getDestAddr(); }
    virtual void setDestAddr(const int32_t &destAddr);
    virtual int32_t &getSrcAddr();
    virtual const int32_t &getSrcAddr() const { return const_cast<AppMessage *>(this)->getSrcAddr(); }
    virtual void setSrcAddr(const int32_t &srcAddr);
    virtual double getMsgCreationTime() const;
    virtual void setMsgCreationTime(double msgCreationTime);
    virtual double getTransportSchedDelay() const;
    virtual void setTransportSchedDelay(double transportSchedDelay);
    virtual double getTransportSchedPreemptionLag() const;
    virtual void setTransportSchedPreemptionLag(double transportSchedPreemptionLag);
    virtual long getMsgBytesOnWire() const;
    virtual void setMsgBytesOnWire(long msgBytesOnWire);
    virtual uint32_t getByteLength() const;
    virtual void setByteLength(uint32_t byteLength);
    virtual HomaMsgType getMsgType() const;
    virtual void setMsgType(HomaMsgType msgType);
    virtual request_id getReqId() const;
    virtual void setReqId(request_id req_id);
    virtual long getAppLevelId() const;
    virtual void setAppLevelId(long id);
    virtual int getClientThreadId() const;
    virtual void setClientThreadId(int thread_id);
    virtual int getServerThreadId() const;
    virtual void setServerThreadId(int thread_id);
};
// inline void doPacking(cCommBuffer *b, AppMessage &obj) { obj.parsimPack(b); }
// inline void doUnpacking(cCommBuffer *b, AppMessage &obj) { obj.parsimUnpack(b); }

#endif // ifndef _APPMESSAGE_H_
