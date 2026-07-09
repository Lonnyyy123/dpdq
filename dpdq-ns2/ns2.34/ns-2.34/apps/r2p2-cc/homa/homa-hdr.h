//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this program.  If not, see http://www.gnu.org/licenses/.
//

#ifndef ns_homa_hdr_h
#define ns_homa_hdr_h

#include "ip.h"
#include <vector>
#include "app-message.h"
#include "r2p2.h"

/**
 * @brief Parts copied from homa sim:: HomaPkt
 *
 */

// #define GRANT_MSG_SIZE 10

typedef std::vector<uint32_t> UnsignedVec;
class HomaTransport;

struct UnschedFields
{
    UnschedFields();
    uint32_t msgByteLen;
    double msgCreationTime;
    uint32_t totalUnschedBytes;
    uint32_t firstByte;
    uint32_t lastByte;
    UnsignedVec *prioUnschedBytes;
};

struct GrantFields
{
    GrantFields();
    uint32_t grantBytes;
    uint32_t offset;
    uint32_t schedPrio;
    UnsignedVec sizeReqBytesPrio;
    UnsignedVec sizeUnschedBytesPrio;
};

struct SchedDataFields
{
    SchedDataFields();
    uint32_t firstByte;
    uint32_t lastByte;
};

enum PktType
{
    // I think that this is only for the first packet in a msg (remember homa does not distinguish reqs from reps)
    REQUEST = 0,
    GRANT = 1,
    SCHED_DATA = 2,
    UNSCHED_DATA = 3
};

// Remember to edit ns-2.34/tcl/lib/ns-packet.tcl if you want to add a new header type
struct hdr_homa
{
private:
    int32_t srcAddr_var_;
    int32_t destAddr_var_;
    uint64_t msgId_var_;
    uint32_t priority_var_;
    int pktType_var_;
    UnschedFields unschedFields_var_;
    GrantFields grantFields_var_;
    SchedDataFields schedDataFields_var_;
    // I this packet part of a request or a reply (or a control pkt)? Not present in original code
    // Not to be confused with PktType
    // This one is needed to know if a message is a request or a reply.
    HomaMsgType msgType_var_;

    // for compatibility with R2p2App (this info needs to be carried)
    request_id reqId_var_;
    long appLevelId_var_;
    int clientThreadId_var_;
    int serverThreadId_var_;
    double msg_creation_time_;

public:
    hdr_homa()
    {
        creation_time_ = Scheduler::instance().clock();
        msgType_var_ = HomaMsgType::UNDEF_HOMA_MSG_TYPE;
        arrivalTime_ = Scheduler::instance().clock();
        msg_creation_time_ = -1.0;                    // to check if it has been set
    }
    ~hdr_homa();
    HomaTransport *ownerTransport;
    int32_t &srcAddr_var() { return srcAddr_var_; }
    int32_t &destAddr_var() { return destAddr_var_; }
    uint64_t &msgId_var() { return msgId_var_; }
    uint32_t &priority_var() { return priority_var_; }
    int &pktType_var() { return pktType_var_; }
    UnschedFields &unschedFields_var() { return unschedFields_var_; }
    GrantFields &grantFields_var() { return grantFields_var_; }
    SchedDataFields &schedDataFields_var() { return schedDataFields_var_; }
    HomaMsgType &msgType_var() { return msgType_var_; }
    request_id &reqId_var() { return reqId_var_; }
    long &appLevelId_var() { return appLevelId_var_; }
    int &clientThreadId_var() { return clientThreadId_var_; }
    int &serverThreadId_var() { return serverThreadId_var_; }
    double &msg_creation_time_var() { return msg_creation_time_; }

    static int offset_;
    inline static int &offset() { return offset_; }
    inline static hdr_homa *access(const Packet *p)
    {
        return (hdr_homa *)p->access(offset_);
    }

    /**
     * returns the header size of this packet.
     */
    uint32_t headerSize();
    uint32_t getDataBytes();
    uint32_t getFirstByte() const;
    static uint32_t getBytesOnWire(uint32_t numDataBytes, PktType homaPktType);

public:
    friend bool operator>(const hdr_homa &lhs, const hdr_homa &rhs);
    class HomaPktSorter
    {
    public:
        HomaPktSorter() {}

        /**
         * Predicate functor operator () for comparison.
         *
         * \param pkt1
         *      first pkt for priority comparison
         * \param pkt2
         *      second pkt for priority comparison
         * \return
         *      true if pkt1 compared greater than pkt2.
         */
        bool operator()(const hdr_homa *pkt1, const hdr_homa *pkt2)
        {
            return *pkt1 > *pkt2;
        }
    };

    // =============== GETTERS(/SETTERS) (for compat with existing code) =================
public:
    uint32_t getPriority() const;
    int32_t getSrcAddr() const;
    uint64_t getMsgId() const;
    int getPktType() const;
    SchedDataFields &getSchedDataFields();
    GrantFields &getGrantFields();
    UnschedFields &getUnschedFields();
    const SchedDataFields &getSchedDataFields() const { return const_cast<hdr_homa *>(this)->getSchedDataFields(); }
    const GrantFields &getGrantFields() const { return const_cast<hdr_homa *>(this)->getGrantFields(); }
    const UnschedFields &getUnschedFields() const { return const_cast<hdr_homa *>(this)->getUnschedFields(); }

    // --------------- OMNET++ functions in cMessage ----------------
    double getArrivalTime() const;
    /**
     * @brief To be set upon arrival at the host
     *
     * @param arrival_time
     * @return double
     */
    void setArrivalTime(const double &arrival_time);
    double getCreationTime() const;
    void setCreationTime(const double &creation_time);
    uint32_t getByteLength() const;
    void setByteLength(uint32_t byteLength);

    // =============== Homa compatibility variables =================
private:
    double arrivalTime_;
    uint32_t byteLength_var_; // the byteLength_var variable in OMNET's IPv6Datagram_m.cc
    double creation_time_;

public:
    static uint32_t maxEthFrameSize()
    {
        return ETHERNET_PREAMBLE_SIZE + ETHERNET_HEADER_SIZE +
               MAX_ETHERNET_PAYLOAD + INTER_PKT_GAP_SIZE;
    }
};
#endif