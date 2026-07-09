//
// Copyright (C) OpenSim Ltd.
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
// along with this program; if not, see http://www.gnu.org/licenses/.
//

#include "inet/common/ModuleAccess.h"
#include "inet/common/Simsignals.h"
#include "inet/queueing/function/PacketComparatorFunction.h"
#include "inet/queueing/function/PacketDropperFunction.h"
#include "inet/queueing/queue/PacketQueue.h"

#define MSGKIND_QLENSAMPLE 10

namespace inet {
namespace queueing {


simsignal_t PacketQueue::queueLenBytesSampled = registerSignal("queueLenBytesSampled");
simsignal_t PacketQueue::byteDepartures = registerSignal("byteDepartures");

Define_Module(PacketQueue);

void PacketQueue::initialize(int stage)
{
    PacketQueueBase::initialize(stage);
    if (stage == INITSTAGE_LOCAL) {
        inputGate = gate("in");
        producer = findConnectedModule<IActivePacketSource>(inputGate);
        outputGate = gate("out");
        collector = findConnectedModule<IActivePacketSink>(outputGate);
        packetCapacity = par("packetCapacity");
        dataCapacity = b(par("dataCapacity"));
        buffer = getModuleFromPar<IPacketBuffer>(par("bufferModule"), this, false);
        const char *comparatorClass = par("comparatorClass");
        customQueueLengthSignal = registerSignal("customQueueLength");
        customQueueLengthSignalPacketBytes = registerSignal("customQueueLengthPacketBytes");
        if (*comparatorClass != '\0')
            packetComparatorFunction = check_and_cast<IPacketComparatorFunction *>(createOne(comparatorClass));
        if (packetComparatorFunction != nullptr)
            queue.setup(packetComparatorFunction);
        const char *dropperClass = par("dropperClass");
        if (*dropperClass != '\0')
            packetDropperFunction = check_and_cast<IPacketDropperFunction *>(createOne(dropperClass));
    }
    else if (stage == INITSTAGE_QUEUEING) {
        if (producer != nullptr) {
            checkPushPacketSupport(inputGate);
            producer->handleCanPushPacket(inputGate);
        }
        if (collector != nullptr)
            checkPopPacketSupport(outputGate);
    }
    else if (stage == INITSTAGE_LAST)
        updateDisplayString();


    q_sample_interval = par("queue_sample_interval_s");
    warmup_period = par("warmup_period");
    if (q_sample_interval < 0.0 || warmup_period < 0.0)
    {
        throw cRuntimeError("You must explicitly define the sampling frequency. Warning: Lots of data");
    }

    // using a specific stage to create one timer per interface (and not $num_stages)
    // I could have just used the boolean.. didn't notice "stage".. I don't understand stages currently.
    if (stage == 12 && !sampling_started)
    {
        cMessage *qlen_sample_timer = new cMessage("qlensample");
        qlen_sample_timer->setKind(MSGKIND_QLENSAMPLE);
        scheduleAt(simTime() + warmup_period, qlen_sample_timer);
        sampling_started = true;
    }
}

void PacketQueue::handleMessage(cMessage *message)
{

    if (message->isSelfMessage())
    {
        switch (message->getKind())
        {
        case MSGKIND_QLENSAMPLE:
        {
            void *a = this;
            cSimpleModule::emit(queueLenBytesSampled, getTotalLength().get() / 8);
            cSimpleModule::emit(byteDepartures, bytes_departed);
            delete message;
            cMessage *qlen_sample_timer = new cMessage("qlensample");
            qlen_sample_timer->setKind(MSGKIND_QLENSAMPLE);
            scheduleAt(simTime() + q_sample_interval, qlen_sample_timer);
            break;
        }
        default:
        {
            throw cRuntimeError("Invalid timer msg: kind=%d", message->getKind());
        }
        }
        return;
    }

    EV << "Handle message called in PacketQueue" << endl;
    auto packet = check_and_cast<Packet *>(message);
    pushPacket(packet, packet->getArrivalGate());
}

bool PacketQueue::isOverloaded()
{
    return (packetCapacity != -1 && getNumPackets() > packetCapacity) ||
           (dataCapacity != b(-1) && getTotalLength() > dataCapacity);
}

int PacketQueue::getNumPackets()
{
    return queue.getLength();
}

Packet *PacketQueue::getPacket(int index)
{
    if (index < 0 || index >= queue.getLength())
        throw cRuntimeError("index %i out of range", index);
    return check_and_cast<Packet *>(queue.get(index));
}

void PacketQueue::pushPacket(Packet *packet, cGate *gate)
{
    Enter_Method("pushPacket");
    emit(packetPushedSignal, packet);
    EV_INFO << "Pushing packet " << packet->getName() << " into the queue." << endl;
    packet->setArrivalTime(simTime());
    queue.insert(packet);
    EV_INFO << "SEPEHR: A packet is inserted into the queue. Queue length: "
            << getNumPackets() << " & packetCapacity: " << packetCapacity <<
            ", Queue data occupancy is " << getTotalLength() <<
            " and dataCapacity is " << dataCapacity << endl;
    if (buffer != nullptr)
        buffer->addPacket(packet);
    else if (isOverloaded()) {
        if (packetDropperFunction != nullptr)
            packetDropperFunction->dropPackets(this);
        else
            throw cRuntimeError("Queue is overloaded but packet dropper function is not specified");
    }
    updateDisplayString();
    if (packetCapacity != -1)
        cSimpleModule::emit(customQueueLengthSignal, getNumPackets());
    else
        cSimpleModule::emit(customQueueLengthSignalPacketBytes, getTotalLength().get());
    if (collector != nullptr && getNumPackets() != 0){
        EV << "SEPEHR: Handling can pop packet." << endl;
        collector->handleCanPopPacket(outputGate);
    }
}

void PacketQueue::pushPacketAfter(Packet *where, Packet *packet){
    queue.insertAfter(where, packet);
}

Packet *PacketQueue::popPacket(cGate *gate)
{
    Enter_Method("popPacket");
    auto packet = check_and_cast<Packet *>(queue.front());
    bytes_departed += packet->getTotalLength().get() / 8;
    EV_INFO << "Popping packet " << packet->getName() << " from the queue." << endl;
    EV << "Sepehr: Time is " << simTime() << " and packet arrival time is " << packet->getArrivalTime() << " so queueing time is: " << (simTime() - packet->getArrivalTime()) << endl;
    if (buffer != nullptr)
        buffer->removePacket(packet);
    else
        queue.pop();
    emit(packetPoppedSignal, packet);
    updateDisplayString();
    animateSend(packet, outputGate);
    if (packetCapacity != -1)
        cSimpleModule::emit(customQueueLengthSignal, getNumPackets());
    else
        cSimpleModule::emit(customQueueLengthSignalPacketBytes, getTotalLength().get());
    return packet;
}

void PacketQueue::removePacket(Packet *packet)
{
    Enter_Method("removePacket");
    EV_INFO << "Removing packet " << packet->getName() << " from the queue." << endl;
    if (buffer != nullptr)
        buffer->removePacket(packet);
    else {
        queue.remove(packet);
        emit(packetRemovedSignal, packet);
        updateDisplayString();
        if (packetCapacity != -1)
            cSimpleModule::emit(customQueueLengthSignal, getNumPackets());
        else
            cSimpleModule::emit(customQueueLengthSignalPacketBytes, getTotalLength().get());
    }
}



bool PacketQueue::canPushSomePacket(cGate *gate)
{
    if (packetDropperFunction)
        return true;
    if (getMaxNumPackets() != -1 && getNumPackets() >= getMaxNumPackets())
        return false;
    if (getMaxTotalLength() != b(-1) && getTotalLength() >= getMaxTotalLength())
        return false;
    return true;
}

bool PacketQueue::canPushPacket(Packet *packet, cGate *gate)
{
    if (packetDropperFunction)
        return true;
    if (getMaxNumPackets() != -1 && getNumPackets() >= getMaxNumPackets())
        return false;
    if (getMaxTotalLength() != b(-1) && getMaxTotalLength() - getTotalLength() < packet->getDataLength())
        return false;
    return true;
}

void PacketQueue::handlePacketRemoved(Packet *packet)
{
    Enter_Method("handlePacketRemoved");
    queue.remove(packet);
    emit(packetRemovedSignal, packet);
    updateDisplayString();

    if (packetCapacity != -1)
        cSimpleModule::emit(customQueueLengthSignal, getNumPackets());
    else
        cSimpleModule::emit(customQueueLengthSignalPacketBytes, getTotalLength().get());
}

} // namespace queueing
} // namespace inet

