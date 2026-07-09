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

#ifndef ns_homa_transport_h
#define ns_homa_transport_h

#include "r2p2-generic.h"
#include "traced-class.h"
#include "r2p2-app.h"
#include "homa-config-depot.h"
#include "homa-hdr.h"
#include "unsched-byte-allocator.h"
#include "app-message.h"
#include "scheduler.h"
#include "homa-timers.h"
#include "workload-estimator.h"
#include <queue>
#include <vector>
#include <list>
#include <unordered_map>
#include <unordered_set>
#include <set>
#include <cfloat>

#define MAXTIME DBL_MAX
#define SIMTIME_ZERO 0.0

class HomaConfigDepot;
class PriorityResolver;
class HomaAgent;
class WorkloadEstimator;

/**
 * @brief This code is informed by the Homa simulation code for OMNET++
 * https://github.com/PlatformLab/HomaSimulation/tree/omnet_simulations/RpcTransportDesign/OMNeT%2B%2BSimulation
 * Similar to class "R2p2" in terms of functionality
 */
class HomaTransport : public R2p2Transport, public SimpleTracedClass
{
public:
    class SendController;
    class ReceiveScheduler;
    /**
     * Design based on/copied from https://github.com/PlatformLab/HomaSimulation/tree/omnet_simulations/RpcTransportDesign/OMNeT%2B%2BSimulation
     * Represents and handles transmiting a message from the senders side.
     * For each message represented by this class, this class exposes the api
     * for sending the req. pkt and some number of unscheduled pkts following
     * the request. This class also allows scheduled data to transmitted.
     */
    class OutboundMessage
    {
    public:
        explicit OutboundMessage();
        explicit OutboundMessage(AppMessage *outMsg,
                                 SendController *sxController,
                                 uint64_t msgId,
                                 std::vector<uint32_t> reqUnschedDataVec);
        explicit OutboundMessage(const OutboundMessage &outboundMsg);
        ~OutboundMessage();
        OutboundMessage &operator=(const OutboundMessage &other);
        void prepareRequestAndUnsched();
        uint32_t prepareSchedPkt(uint32_t offset, uint32_t numBytes,
                                 uint32_t schedPrio);

    public:
        /**
         * A utility predicate for creating PriorityQueues of hdr_homa instances.
         * Determines in what order pkts of this msg will be sent out to the
         * network.
         */
        class OutbndPktSorter
        {
        public:
            OutbndPktSorter() {}
            bool operator()(const hdr_homa *pkt1, const hdr_homa *pkt2);
        };

        typedef std::priority_queue<
            hdr_homa *,
            std::vector<hdr_homa *>,
            OutbndPktSorter>
            OutbndPktQueue;

        // getters functions
        const uint32_t &getMsgSize() { return msgSize; }
        const uint64_t &getMsgId() { return msgId; }
        const OutbndPktQueue &getTxPktQueue() { return txPkts; }
        const std::unordered_set<hdr_homa *> &getTxSchedPkts()
        {
            return txSchedPkts;
        }
        const uint32_t getBytesLeft() { return bytesLeft; }
        const double getMsgCreationTime() { return msgCreationTime; }
        bool getTransmitReadyPkt(hdr_homa **outPkt);

    protected:
        // Whether the message is a request or a reply
        HomaMsgType msgType;
        // For compatibility with R2p2App, these need to be carried around
        request_id reqId;
        long appLevelId;
        int clientThreadId;
        int serverThreadId;

        // Unique identification number assigned by in the construction time for
        // the purpose of easy external access to this message.
        uint64_t msgId;

        // Total byte size of the message received from application
        uint32_t msgSize;

        // Total num bytes that need grants from the receiver for transmission
        // in this mesg.
        uint32_t bytesToSched;

        // Total num bytes left to be transmitted for this messge, including
        // bytes that are packetized and queued in the transport, but not yet
        // sent to the network interface for transmission.
        uint32_t bytesLeft;

        // Total unsched bytes left to be transmitted for this messge.
        uint32_t unschedBytesLeft;

        // Next time this message expects an unsched packet must be sent. This
        // is equal to time at which last bit of last unsched pkt was serialized
        // out to the network.
        double nextExpectedUnschedTxTime;

        // This vector is length of total unsched pkts to be sent for
        // this message. Element i in this vector is number of data bytes to be
        // sent by the i'th unsched packet for this message. Note that first
        // unsched pkt at position zero of this vector is always the request
        // packet and sum of the elements is the total unsched bytes sent for
        // this mesage. For every message, there will at least one unsched
        // packet that is request packet, so this vector is at least size 1.
        std::vector<uint32_t> reqUnschedDataVec;

        // IpAddress of destination host for this outbound msg.
        int32_t destAddr;

        // IpAddress of sender host (local host).
        int32_t srcAddr;

        // Simulation global time at which this message was originally created
        // in the application.
        double msgCreationTime;

        // Priority Queue containing all sched/unsched pkts are to be sent out
        // for this message and are waiting for transmission.
        OutbndPktQueue txPkts;

        // Set of all sched pkts ready for transmission
        std::unordered_set<hdr_homa *> txSchedPkts;

        // The SendController that manages the transmission of this msg.
        SendController *sxController;

        // The object that keeps all transport configuration parameters
        HomaConfigDepot *homaConfig;

    private:
        void copy(const OutboundMessage &other);
        std::vector<uint32_t> getUnschedPerPrio(
            std::vector<uint32_t> &unschedPrioVec);
        friend class SendController;
        friend class PriorityResolver;
    };

    /**
     * Design based on/copied from https://github.com/PlatformLab/HomaSimulation/tree/omnet_simulations/RpcTransportDesign/OMNeT%2B%2BSimulation
     * Manages the transmission of all OutboundMessages from this transport and
     * keeps the state necessary for transmisssion of the messages. For every
     * new message that arrives from the applications, this class is responsible
     * for sending the request packet, unscheduled packets, and scheduled packet
     * (when grants are received).
     */
    class SendController
    {
    public:
        typedef std::unordered_map<uint64_t, OutboundMessage> OutboundMsgMap;
        SendController(HomaTransport *transport);
        ~SendController();
        void initSendController(HomaConfigDepot *homaConfig,
                                PriorityResolver *prioResolver);
        void processSendMsgFromApp(AppMessage *sendMsg);
        void processReceivedGrant(hdr_homa *rxPkt);
        OutboundMsgMap *getOutboundMsgMap() { return &outboundMsgMap; }
        void sendOrQueue(Event *msg = NULL);
        void handlePktTransmitEnd();

    public:
        /**
         * A predicate func object for sorting OutboundMessages based on the
         * senderScheme used for transport. Detemines in what order
         * OutboundMessages will send their sched/unsched outbound packets (ie.
         * pkts queues in OutboundMessage::txPkts).
         */
        class OutbndMsgSorter
        {
        public:
            OutbndMsgSorter() {}
            bool operator()(OutboundMessage *msg1, OutboundMessage *msg2);
        };
        typedef std::set<OutboundMessage *, OutbndMsgSorter> SortedOutboundMsg;

    protected:
        void sendPktAndScheduleNext(hdr_homa *sxPkt);
        void msgTransmitComplete(OutboundMessage *msg);

    protected:
        // For the purpose of statistics recording, this variable tracks the
        // total number bytes left to be sent for all outstanding messages.
        // (including unsched/sched bytes ready to be sent but not yet
        // transmitted).
        uint64_t bytesLeftToSend;

        // Sum of all sched bytes in all outbound messages that are yet to be
        // scheduled by their corresponding reveivers;
        uint64_t bytesNeedGrant;

        // The identification number for the next outstanding message.
        uint64_t msgId;

        // Keeps a copy of last packet transmitted by this module
        hdr_homa sentPkt;

        // Serialization time of sentPkt at nic link speed
        double sentPktDuration;

        // The hash map from the msgId to outstanding messages.
        OutboundMsgMap outboundMsgMap;

        // Each entry (ie. map value) is a sorted set of all outbound
        // messages for a specific receiver mapped to the ip address of that
        // receiver (ie. map key).
        std::unordered_map<int32_t, std::unordered_set<OutboundMessage *>>
            rxAddrMsgMap;

        // For each distinct receiver, allocates the number of request and
        // unsched bytes for various sizes of message.
        UnschedByteAllocator *unschedByteAllocator;

        // Determine priority of packets that are to be sent
        PriorityResolver *prioResolver;

        // Sorted collection of all OutboundMessages that have unsched/sched
        // pkts queued up and ready to be sent out into the network.
        SortedOutboundMsg outbndMsgSet;

        // Queue for keeping grants that receiver side of this host machine
        // wants to send out.
        std::priority_queue<hdr_homa *,
                            std::vector<hdr_homa *>,
                            hdr_homa::HomaPktSorter>
            outGrantQueue;

        // Transport that owns this SendController.
        HomaTransport *transport;

        // The object that keeps the configuration parameters for the transport
        HomaConfigDepot *homaConfig;

        // Tracks the begining of time periods during which outstanding bytes is
        // nonzero.
        double activePeriodStart;

        // Tracks the bytes received during each active period.
        uint64_t sentBytesPerActivePeriod;

        // Tracks sum of grant pkts received between two respective received
        // data pkts.  SendController cumulates grant packets to this variable
        // and ReceiveScheduler zeros it out whenever a new data packet has
        // arrived.
        uint64_t sumGrantsInGap;

        friend class OutboundMessage;
        friend class HomaTransport;
    }; // End SendController

    /**
     * Handles reception of an incoming message by concatanations of data
     * fragments in received packets and keeping track of reception progress.
     */
    class InboundMessage
    {
    public:
        explicit InboundMessage();
        explicit InboundMessage(const InboundMessage &other);
        explicit InboundMessage(hdr_homa *rxPkt, ReceiveScheduler *rxScheduler,
                                HomaConfigDepot *homaConfig);
        ~InboundMessage();

    public:
        typedef std::list<std::tuple<uint32_t, uint32_t, double>> GrantList;

        /**
         * A predicate functor that compares the remaining required grants
         * to be sent for two inbound message.
         */
        class CompareBytesToGrant
        {
        public:
            CompareBytesToGrant()
            {
            }

            /**
             * Predicate functor operator () for comparison.
             *
             * \param msg1
             *      inbound message 1 in the comparison
             * \param msg2
             *      inbound message 2 in the comparison
             * \return
             *      a bool from the result of the comparison
             */
            bool operator()(const InboundMessage *msg1,
                            const InboundMessage *msg2)
            {
                return (msg1->bytesToGrant < msg2->bytesToGrant) ||
                       (msg1->bytesToGrant == msg2->bytesToGrant &&
                        msg1->msgSize < msg2->msgSize) ||
                       (msg1->bytesToGrant == msg2->bytesToGrant &&
                        msg1->msgSize == msg2->msgSize &&
                        msg1->msgCreationTime < msg2->msgCreationTime) ||
                       (msg1->bytesToGrant == msg2->bytesToGrant &&
                        msg1->msgSize == msg2->msgSize &&
                        msg1->msgCreationTime == msg2->msgCreationTime &&
                        msg1->msgIdAtSender < msg2->msgIdAtSender);
            }
        };
        const uint32_t &getMsgSize() { return msgSize; }

    protected:
        // Whether the message is a request or a reply
        HomaMsgType msgType;
        // For compatibility with R2p2App, these need to be carried around
        request_id reqId;
        long appLevelId;
        int clientThreadId;
        int serverThreadId;

        // The ReceiveScheduler that manages the reception of this message.
        ReceiveScheduler *rxScheduler;

        // The object that keeps all transport configuration parameters
        HomaConfigDepot *homaConfig;

        // Address of the sender of this message.
        int32_t srcAddr;

        // Address of the receiver (ie. this host) of this message. Used to
        // specify the sources address when grant packets are being sent.
        int32_t destAddr;

        // The id of this message at the sender host. Used in the grant packets
        // to help the sender identify which outbound message a received grant
        // belongs to.
        uint64_t msgIdAtSender;

        // Tracks the total number of grant bytes that the rxScheduler should
        // send for this message.
        uint32_t bytesToGrant;

        // Tracks the next for this messaage that is to be scheduled for
        // transmission by next grant packet. This value is monotonically
        // increasing and together with next grantSize, uinquely identifies the
        // nest chunck of data bytes in the message will be tranmitted by the
        // sender.
        uint32_t offset;

        // Tracks the total number of data bytes scheduled (granted) for this
        // messages but has not yet been received.
        uint32_t bytesGrantedInFlight;

        // Total number of bytes inflight for this message including all
        // different header bytes and ethernet overhead bytes.
        uint32_t totalBytesInFlight;

        // Tracks the total number of bytes that has not yet been received for
        // this message. The message is complete when this value reaches zero
        // and therefore it can be handed over to the application.
        uint32_t bytesToReceive;

        // The total size of the message as indicated in the req. packet.
        uint32_t msgSize;

        // Tracks the number of scheduled data bytes plus header bytes, as the
        // receiver sends grants for this message.
        uint32_t schedBytesOnWire;

        // Total bytes transmitted on wire for this message
        uint32_t totalBytesOnWire;

        // All unscheduled bytes that come in req. pkt and the following
        // unsched packets for this message.
        uint32_t totalUnschedBytes;

        // simulation time at which this message was created in the sender side.
        // Used to calculate the end to end latency of this message.
        double msgCreationTime;

        // simulation time at which the first packet (req. pkt) of this inbound
        // message arrived at receiver. Used only for statistics recording
        // purpose.
        double reqArrivalTime;

        //****************************************************************//
        //*****Below variables are for statistic collection purpose.******//
        //****************************************************************//
        // When the last grant for this message was scheduled. Initialized in
        // the constructor and must only be updated by the prepareGrant()
        // method.
        double lastGrantTime;

        // List to keep track of the outstanding grant pkts. Each tuple in the
        // list has the offset byte scheduled by a grant, and the size of the
        // grant and time at which the receiver scheduled that grant. The
        // unscheduled bytes are also added to this list as hypothetical grant
        // pkts that receiver sent one RTT before the first packet arrived and
        // for zero offset byte.
        GrantList inflightGrants;

        //***************************************************************//
        //****Below variables are snapshots, first after construction****//
        //****and then at grant times, of the corresponding variables****//
        //****defined in ReceiveScheduler.                           ****//
        //***************************************************************//
        std::vector<uint64_t> bytesRecvdPerPrio;
        std::vector<uint64_t> scheduledBytesPerPrio;
        std::vector<uint64_t> unschedToRecvPerPrio;

        //***************************************************************//
        //****Below variables are snapshots, first after construction****//
        //****and then at grant times, of the corresponding variables****//
        //****defined in TrafficPacer.                               ****//
        //***************************************************************//
        std::vector<uint32_t> sumInflightUnschedPerPrio;
        std::vector<uint32_t> sumInflightSchedPerPrio;

        friend class CompareBytesToGrant;
        friend class ReceiveScheduler;
        friend class TrafficPacer; // definition or declaration nowhere to be found neither in
                                   // homa code nor OMNET code.
        friend class PriorityResolver;

    protected:
        void copy(const InboundMessage &other);
        void fillinRxBytes(uint32_t byteStart, uint32_t byteEnd,
                           PktType pktType);
        uint32_t schedBytesInFlight();
        uint32_t unschedBytesInFlight();
        hdr_homa *prepareGrant(uint32_t grantSize, uint32_t schedPrio);
        AppMessage *prepareRxMsgForApp();
        void updatePerPrioStats();
    };

    /**
     * Manages reception of messages that are being sent to this host through
     * this transport. Keeps a list of all incomplete rx messages and sends
     * grants for them based on SRPT policy. At the completion of each message,
     * it will be handed off to the application.
     */
    class ReceiveScheduler
    {
    public:
        ReceiveScheduler(HomaTransport *transport);
        ~ReceiveScheduler();
        InboundMessage *lookupInboundMesg(hdr_homa *rxPkt) const;

        class UnschedRateComputer
        {
        public:
            UnschedRateComputer(HomaConfigDepot *homaConfig,
                                bool computeAvgUnschRate = false, double minAvgTimeWindow = .1);
            double getAvgUnschRate(double currentTime);
            void updateUnschRate(double arrivalTime, uint32_t bytesRecvd);

        public:
            bool computeAvgUnschRate;
            std::vector<std::pair<uint32_t, double>> bytesRecvTime;
            uint64_t sumBytes;
            double minAvgTimeWindow; // in seconds
            HomaConfigDepot *homaConfig;
        };

        /**
         * Per sender object to manage the reception of messages from each
         * sender. It also contains all per sender information that are needed
         * to schedule messages of the corresponding sender.
         */
        class SenderState
        {
        public:
            SenderState(int32_t srcAddr,
                        ReceiveScheduler *rxScheduler, HomaGrantTimer *grantTimer,
                        HomaConfigDepot *homaConfig);
            ~SenderState() {}
            const int32_t &getSenderAddr()
            {
                return senderAddr;
            }
            double getNextGrantTime(uint32_t grantSize);
            int sendAndScheduleGrant(uint32_t grantPrio);
            std::pair<bool, int> handleInboundPkt(hdr_homa *rxPkt);

        protected:
            HomaConfigDepot *homaConfig;
            ReceiveScheduler *rxScheduler;
            int32_t senderAddr;

            // Timer object for sending grants for this sender. Will be used
            // to send timer paced grants for this sender if totalBytesInFlight
            // for the top mesg of this sender is less than RTT.
            HomaGrantTimer *grantTimer;

            // A sorted list of sched messages that need grants from the sender.
            std::set<InboundMessage *, InboundMessage::CompareBytesToGrant>
                mesgsToGrant;

            // Map of all incomplete inboundMsgs from the sender hashed by msgId
            std::unordered_map<uint64_t, InboundMessage *> incompleteMesgs;

            // Priority of last sent grant for this sender
            uint32_t lastGrantPrio;

            // Index of this sender in SchedSenders as of the last time its been
            // removed from SchedSender.
            uint32_t lastIdx;

            friend class HomaTransport::ReceiveScheduler;
        };

        /**
         * Collection of all scheduled senders (ie. senders with at least one
         * message that needs grants). This object also contains all information
         * requiered for scheduling messages and implementing the scheduler's
         * logic and the sched prio assignment.
         */
        class SchedSenders
        {
        public:
            SchedSenders(HomaConfigDepot *homaConfig, HomaTransport *transport,
                         ReceiveScheduler *rxScheduler);
            ~SchedSenders() {}
            class CompSched
            {
            public:
                CompSched() {}
                bool operator()(const SenderState *lhs, const SenderState *rhs)
                {
                    if (!lhs && !rhs)
                        return false;
                    if (!lhs && rhs)
                        return true;
                    if (lhs && !rhs)
                        return false;

                    InboundMessage::CompareBytesToGrant cbg;
                    return cbg(*lhs->mesgsToGrant.begin(),
                               *rhs->mesgsToGrant.begin());
                }
            };

            /**
             * This class is a container to keep a copy of scheduled state of
             * the receiver scheduler. This is used to track state changes after
             * each new event arrives at the ReceiveScheduler.
             */
            class SchedState
            {
            public:
                // Total number of senders we allow to be scheduled and granted
                // at the same time (ie. overcommittment level).
                int numToGrant;

                // Index of the highest priority sender in the list. This could
                // be non zero which means no sender will be located at indexes
                // 0 to headIdx-1 of the senders list.
                int headIdx;

                // Total number of senders in the list.
                int numSenders;

                // SenderState for which we have handled one of the events
                SenderState *s;

                // Index of s in the senders list
                int sInd;

            public:
                void setVar(uint32_t numToGrant, uint32_t headIdx,
                            uint32_t numSenders, SenderState *s, int sInd)
                {
                    this->numToGrant = numToGrant;
                    this->headIdx = headIdx;
                    this->numSenders = numSenders;
                    this->s = s;
                    this->sInd = sInd;
                }

                friend std::ostream &operator<<(std::ostream &os,
                                                const SchedState &ss)
                {
                    os << "Sender: " << ss.s->getSenderAddr() << ", senderInd: " << ss.sInd << ", headInd: " << ss.headIdx << ", numToGrant: " << ss.numToGrant << ", numSenders: " << ss.numSenders;
                    return os;
                }
            }; // end SchedState

            std::tuple<int, int, int> insPoint(SenderState *s);
            void insert(SenderState *s);
            int remove(SenderState *s);
            SenderState *removeAt(uint32_t rmInd);
            uint32_t numActiveSenders();

            // void handleGrantRequest(SenderState* s, int sInd, int headInd);
            uint32_t getPrioForMesg(SchedState &cur);
            void handleBwUtilTimerEvent(HomaSchedBwUtilTimer *timer);
            void handlePktArrivalEvent(SchedState &old, SchedState &cur);
            void handleGrantSentEvent(SchedState &old, SchedState &cur);
            void handleMesgRecvCompletionEvent(
                const std::pair<bool, int> &msgCompHandle,
                SchedState &old, SchedState &cur);
            void handleGrantTimerEvent(SenderState *s);

        protected:
            // Back pointer to the transport module.
            HomaTransport *transport;

            // Back pointer to the ReceiveScheduler managing this container
            ReceiveScheduler *rxScheduler;

            // Sorted list of all scheduled senders.
            std::vector<SenderState *> senders;

            // Total number available priorities for the scheduled packets.
            uint32_t schedPrios;

            // Total number of senders we allow to be scheduled and granted at
            // the same time (ie. overcommittment level).
            uint32_t numToGrant;

            // Index of the highest priority sender in the list. This could be
            // non zero which means no sender will be located at indexes 0 to
            // headIdx-1 of the senders list.
            uint32_t headIdx;

            // Total number of senders in the list.
            uint32_t numSenders;

            // Collection of user provided config parameters for the transport.
            HomaConfigDepot *homaConfig;

            friend class HomaTransport::ReceiveScheduler;
        }; // end SchedSenders

    protected:
        // Back pointer to the transport instance that owns this scheduler.
        HomaTransport *transport;

        // Collection of user specified config parameters.
        HomaConfigDepot *homaConfig;

        // Object for computing the average received rate of unscheduled bytes.
        UnschedRateComputer *unschRateComp;

        // Hash container for accessing each sender's state collection using
        // that sender's IP.
        std::unordered_map<int32_t, SenderState *> ipSendersMap;

        // Hash container for accessing each sender's state collection using
        // from the grant timer object (port: the timer_id of the object) of the sender.
        std::unordered_map<HomaGrantTimer *, SenderState *, HomaGrantTimer::HomaGrantTimerHash> grantTimersMap;

        // Collection of all senders that have at least one message that is not
        // fully granted.
        SchedSenders *schedSenders;

        // Timer for detecting if the receiver's scheduled bandwidth is being
        // wasted. ie. the senders are delaying sending the grants back.
        HomaSchedBwUtilTimer *schedBwUtilTimer;

        // The lenght of time interval during which if we don't receive a
        // packet, receiver inbound bw is considered wasted.
        double bwCheckInterval;

        //*******************************************************//
        //*****Below variables are for statistic collection******//
        //*******************************************************//

        // These variables track outstanding bytes (ie. not yet arrived bytes)
        // that the receiver knows they must arrive in the future.
        std::vector<uint32_t> inflightUnschedPerPrio;
        std::vector<uint32_t> inflightSchedPerPrio;
        uint64_t inflightSchedBytes;
        uint64_t inflightUnschedBytes;

        // The vector below is of size allPrio and each element of the vector is
        // a monotoically increasing number that tracks total number of bytes
        // received on that priority through out the simulation.  Used for
        // statistics collection.
        std::vector<uint64_t> bytesRecvdPerPrio;

        // The vector below is of size allPrio and each element of the vector is
        // a monotoically increasing number that tracks total number of bytes
        // granted on that priority through out the simulation.  Used for
        // statistics collection.
        std::vector<uint64_t> scheduledBytesPerPrio;

        // The vector below is of size allPrio and each element of the vector is
        // a monotoically increasing number that tracks total number of unsched
        // bytes that are expected to be received on that priority through out
        // the simulation. Used for statistics collection.
        std::vector<uint64_t> unschedToRecvPerPrio;

        // A monotonically increasing number that tracks total number of bytes
        // received throughout the simulation. Used for statistics collection.
        uint64_t allBytesRecvd;

        // A monotonically increasing number that tracks total number of unsched
        // bytes to be received throughout the simulation. Used for statistics
        // collection.
        uint64_t unschedBytesToRecv;

        // Tracks the begining of time periods during which outstanding bytes is
        // nonzero. Such periods are defined as active periods.
        double activePeriodStart;

        // Tracks the bytes received during each active period.
        uint64_t rcvdBytesPerActivePeriod;

        // Tracks the begining and end of time periods during which total number
        // of senders with scheduled messages is larger than redundancy factor.
        // This period is called an over subscription period.
        double oversubPeriodStart, oversubPeriodStop;

        // Only true when an over subscription period has started not yet ended.
        bool inOversubPeriod;

        // Tracks the bytes received during each over subscription period.
        uint64_t rcvdBytesPerOversubPeriod;

        // In Homa, number of scheduled senders that receiver is granting
        // at any point of time changes depending on the number of senders and
        // numToGrant and the receiver's bandwidth that is being wasted. This
        // varible tracks the current number sched senders that  receiver is
        // actively granting. The value of this variable is obtained through
        // calling SchedSenders::numActiveSenders() method.
        uint32_t numActiveScheds;

        // The id of the last GrantTimer created (to be incremented for every timer)
        uint64_t grantTimerId;

        // Tracks the last simulation time at which numActiveScheds has changed.
        double schedChangeTime;

        // Tracks the time at which we received the last data packet
        double lastRecvTime;

    protected:
        void initialize(HomaConfigDepot *homaConfig,
                        PriorityResolver *prioResolver);
        void processReceivedPkt(hdr_homa *rxPkt);
        void processGrantTimers(HomaGrantTimer *grantTimer);

        inline uint64_t getInflightBytes()
        {
            return inflightUnschedBytes + inflightSchedBytes;
        }

        inline const std::vector<uint32_t> &getInflightUnschedPerPrio()
        {
            return inflightUnschedPerPrio;
        }

        inline const std::vector<uint32_t> &getInflightSchedPerPrio()
        {
            return inflightSchedPerPrio;
        }

        void addArrivedBytes(PktType pktType, uint32_t prio,
                             uint32_t dataBytes);
        void addPendingGrantedBytes(uint32_t prio, uint32_t grantedBytes);
        void addPendingUnschedBytes(PktType pktType, uint32_t prio,
                                    uint32_t bytesToArrive);
        void pendingBytesArrived(PktType pktType, uint32_t prio,
                                 uint32_t dataBytesInPkt);
        void tryRecordActiveMesgStats(double timeNow);
        friend class HomaTransport;
        friend class SchedSenders;
        friend class InboundMessage;
        friend class HomaGrantTimer;
        friend class HomaSchedBwUtilTimer;
    }; // end ReceiveScheduler

    /**
     * Keeps record of the reported rtt samples for each sender and provides max
     * value of RTT between all senders, ie. RTT on the longest path in the
     * network.
     */
    class TrackRTTs
    {
    public:
        TrackRTTs(HomaTransport *transport);
        ~TrackRTTs() {}
        void updateRTTSample(int32_t senderIP, double rttVal);

    public:
        // For each sender, keeps track of the max of reported observed RTT for
        // that sender.
        std::unordered_map<int32_t, double>
            sendersRTT;

        // This is the largest of minimum RTT value observed from senders so
        // far. It will track
        std::pair<int32_t, double> maxRTT;

        // The transport module that owns the instanace of this class.
        HomaTransport *transport;
    };

public:
    HomaTransport();
    virtual ~HomaTransport();
    int command(int argc, const char *const *argv) override;
    // API - same as r2p2.h to reuse r2p2-app
    void r2p2_send_req(int payload, const RequestIdTuple &request_id_tuple) override;
    void r2p2_send_response(int payload, const RequestIdTuple &request_id_tuple, int new_n) override;
    void recv(Packet *pkt, Handler *h) override;

    void attach_r2p2_application(R2p2Application *r2p2_application);
    int32_t get_local_addr() override;
    void attach_homa_agent(HomaAgent *homaAgent);
    inline int get_debug()
    {
        return debug_;
    }

    // Handles the transmission of outbound messages based on the logic of
    // HomaProtocol.
    SendController sxController;

    // Manages the reception of all inbound messages.
    ReceiveScheduler rxScheduler;

    // Keeps records of the smallest rtt observed from different senders and the
    // max of those observations as the RTT of the network.
    TrackRTTs trackRTTs;

protected:
    virtual void init();
    virtual void handleMessage();
    std::unordered_map<int, R2p2Application *> thread_id_to_app_;
    // The object that keeps the configuration parameters for this transport
    HomaConfigDepot *homaConfig;

    // Determine priority of packets that are to be sent
    PriorityResolver *prioResolver;

    // Keeps track of the message size distribution that this transport is
    // seeing.
    WorkloadEstimator *distEstimator;

    // Timer object for send side packet pacing. At every packet transmission at
    // sender this will be used to schedule next send after the current send is
    // completed.
    HomaSendTimer *sendTimer;

    // Map from destination address to the pre-connected UDP agent that can send to it.
    std::unordered_map<int32_t, HomaAgent *> homa_agents_;

    // Tracks the total outstanding grant bytes which will be used for stats
    // collection and recording.
    int outstandingGrantBytes;

    friend class ReceiveScheduler;
    friend class SendController;

    // -------------------------- Homa config depot parameters -----------------------------
    //  This is super ugly but it is done
    // to pass these parameters via TCL. No time to do it properly

    // NIC link speed (in Gb/s) connected to this host.
    int nicLinkSpeed;

    // RTT in bytes for the topology, given as a configuration parameter.
    uint32_t rttBytes;

    // This parameter is read from the omnetpp.ini config file and provides an
    // upper bound on the total allowed outstanding bytes. It is necessary for
    // the rxScheduler to check that the total outstanding bytes is smaller than
    // this value every time a new grant is to be sent.
    uint32_t maxOutstandingRecvBytes;

    // udp ports assigned to this transprt
    // int localPort;
    // int destPort;

    // Maximum possible data bytes allowed in grant (????)
    uint32_t grantMaxBytes;

    // Total number of available priorities
    uint32_t allPrio;

    // Total priority levels available for adaptive scheduling when adaptive
    // scheduling is enabled. (?????)
    uint32_t adaptiveSchedPrioLevels;

    // Number of senders that receiver tries to keeps granted cuncurrently to
    // avoid its bandwidth wasted in one or more of the these senders stops
    // sending. This number is at most as large as adaptiveSchedPrioLevels
    uint32_t numSendersToKeepGranted;

    // If true, network traffic bytes from grant packets will be accounted in
    // when computing CBF and unscheduled priority cutoff sizes.
    int accountForGrantTraffic;

    // Total number of priorities that PrioResolver would use to resolve
    // priorities.
    uint32_t prioResolverPrioLevels;

    // Specifies which priority resolution mode should be used for unscheduled
    // packets. Resolution modes are defined in PrioResolver class.
    const char *unschedPrioResolutionMode;

    // Specifies for the unsched prio level, how many times prio p_i will be
    // used comparing to p_i-1 (p_i is higher prio). Cutoff weight are used
    // in PrioResolver class.
    double unschedPrioUsageWeight;

    // If unschedPrioResolutionMode is set to EXPLICIT, then this string defines
    // the priority cutoff points of unsched bytes for the remaining message
    // sizes. Example would be "100 1500 9000"
    std::vector<uint32_t> explicitUnschedPrioCutoff;

    // Defines the type of logic sender uses for transmitting messages and pkts
    const char *senderScheme;

    // Specifies the scheduler type. True, means round robin scheduler and false
    // is for SRBF scheduler.
    int isRoundRobinScheduler;

    // If receiver inbound link is idle for longer than (link speed X
    // bwCheckInterval), while there are senders waiting for grants, we consider
    // receiver bw is wasted. -1 means don't check for the bw-waste.
    int linkCheckBytes;

    // Specifies that only first cbfCapMsgSize bytes of a message must be used
    // in computing the cbf function. (?????)
    uint32_t cbfCapMsgSize;

    // This value is in bytes and determines that this number of last scheduled
    // bytes of the message will be send at priority equal to unscheduled
    // priorites. The default is 0 bytes.
    uint32_t boostTailBytesPrio;

    // Number data bytes to be packed in request packet.
    uint32_t defaultReqBytes;

    // Maximum number of unscheduled data bytes that will be sent in unsched.
    // packets except the data bytes in request packet. (?????)
    uint32_t defaultUnschedBytes;

    // True means that scheduler would use
    // (1-avgUnscheduledRate)*maxOutstandingRecvBytes as the cap for number of
    // scheduled bytes allowed outstanding.
    int useUnschRateInScheduler;

    // workload
    int workloadType;
    const char *map_workload(int workload)
    {
        switch (workload)
        {
        case 1:
            throw std::runtime_error("Must do some more work to integrate workload " + std::to_string(workload) + " (part of it is not in the cdf)");
            return "FACEBOOK_KEY_VALUE";
            break;
        case 2:
            return "GOOGLE_SEARCH_RPC";
            break;
        case 3:
            return "GOOGLE_ALL_RPC";
            break;
        case 4:
            return "FACEBOOK_HADOOP_ALL";
            break;
        case 5:
            return "DCTCP";
            break;
        default:
            throw std::runtime_error("Unknown workload: " + std::to_string(workload));
            break;
        }
    }
    // ------------------------------------------------------------------------------------------
}; // End HomaTransport

/**
 * To create cumulative-time-percent statistics of active scheduled
 * senders, we send activeSchedsSignal to GlobalSignalListener which
 * dumps that stats. Thise signal carries a pointer to object of type
 * ActiveScheds class and this object contais the last value of
 * numActiveSenders and time duration that the scheduler was keeping
 * that many active scheduled senders.
 */
class ActiveScheds //: public cObject, noncopyable // TODO: did not consider the implications of not inheriting
{
public:
    ActiveScheds() {}

public:
    uint32_t numActiveSenders;
    double duration;
}; // end ActiveScheds

#endif
