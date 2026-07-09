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

#include "MultiSocketTcpAppBase.h"

#include "inet/networklayer/common/L3AddressResolver.h"
#include "inet/transportlayer/contract/tcp/TcpSocket.h"
#include "inet/applications/common/SocketTag_m.h"

using namespace inet;
using namespace std;

simsignal_t MultiSocketTcpAppBase::connectSignal = registerSignal("connect");

MultiSocketTcpAppBase::~MultiSocketTcpAppBase()
{
    for (auto socket_pair = socket_map.getMap().begin(); socket_pair != socket_map.getMap().end(); socket_pair++)
    {
        delete socket_pair->second;
    }
    for (auto pool_it = socket_maps.begin(); pool_it != socket_maps.end(); pool_it++)
    {
        for (auto socket_pair = pool_it->second->getMap().begin(); socket_pair != pool_it->second->getMap().end(); socket_pair++)
            delete socket_pair->second;
    }
    for (auto pool_it = socket_maps.begin(); pool_it != socket_maps.end(); pool_it++)
    {
        delete pool_it->second;
    }
}

int MultiSocketTcpAppBase::requestsSent = 0;

void MultiSocketTcpAppBase::initialize(int stage)
{
    ApplicationBase::initialize(stage);

    if (stage == INITSTAGE_LOCAL)
    {
        numSessions = numBroken = packetsSent = packetsRcvd = bytesSent = bytesRcvd = 0;

        WATCH(numSessions);
        WATCH(numBroken);
        WATCH(packetsSent);
        WATCH(packetsRcvd);
        WATCH(bytesSent);
        WATCH(bytesRcvd);
    }
}

void MultiSocketTcpAppBase::handleMessageWhenUp(cMessage *msg)
{
    // EV << "SEPEHR: Handling Message wMultiSocketTcpAppBaseTcpAppBase! msg is: " << msg->str() << endl;
    if (msg->isSelfMessage())
    {
        // EV << "SEPEHR: Calling handleTimer" << endl;
        handleTimer(msg);
    }

    else
    {
        // EV << "SEPEHR: Calling socket.processMessage" << endl;

        TcpSocket *socket = check_and_cast_nullable<TcpSocket *>(socket_map.findSocketFor(msg));
        if (socket)
        {
            socket->processMessage(msg);
            return;
        }

        throw cRuntimeError("message %s(%s) arrived for unknown socket 1\n", msg->getFullName(), msg->getClassName());
        delete msg;
    }
}
void MultiSocketTcpAppBase::idxToL3Addr(int server_idx, L3Address &destination)
{
    std::string connect_address_str = "server[" + std::to_string(server_idx) + "]";
    const char *connect_address = connect_address_str.c_str();
    L3AddressResolver().tryResolve(connect_address, destination);
    // std::cout << "converted index " << connect_address_str << " to " << destination.str() << std::endl;
    if (destination.isUnspecified())
    {
        throw cRuntimeError("Connecting to %a: cannot resolve destination address\n", connect_address);
    }
}

void MultiSocketTcpAppBase::connect(int local_port, int dest_server_idx, int connect_port, simtime_t startTime, bool is_bursty,
                                    unsigned long query_id)
{
    TcpSocket *socket;
    // the workload has not started. Should establish connections and add them to the pool
    if (simTime() < startTime)
    {
        socket = new TcpSocket();
        // std::cout << simTime() << " creating new tcp socket" << std::endl;
        const char *localAddress = par("localAddress");
        // std::cout << "localAddress: " << localAddress << std::endl;
        socket->bind(*localAddress ? L3AddressResolver().resolve(localAddress) : L3Address(), local_port);
        socket->setCallback(this);
        socket->setOutputGate(gate("socketOut"));

        // std::string connect_address_str = "server[" + std::to_string(dest_server_idx) + "]";
        // const char *connect_address = connect_address_str.c_str();
        // // std::cout << "connect_address: " << connect_address << std::endl;
        int timeToLive = par("timeToLive");
        if (timeToLive != -1)
            socket->setTimeToLive(timeToLive);

        int dscp = par("dscp");
        if (dscp != -1)
            socket->setDscp(dscp);

        // connect
        L3Address destination;
        // std::cout << "::connect, asking for conversion of " << dest_server_idx << std::endl;
        idxToL3Addr(dest_server_idx, destination);
        // std::cout << "resolved address of type: " << destination.getType() << std::endl;
        // std::cout << "Connecting to " << dest_server_idx << "(" << destination << ") port=" << connect_port << endl;
        socket->connect(destination, connect_port);
        numSessions++;
        //        emit(connectSignal, 1L);
        // }
        // std::cout << "SEPEHR: adding socket with id: " << socket->getSocketId() << " to the list for server " << dest_server_idx << ". Before adding the size of socket_map is " << socket_map.size();
        // std::cout << "The dst addr in str format is: " << destination.str() << endl;
        socket_map.addSocket(socket);

        // see if there is a pool for destination and add socket to pool
        auto pool_itr = socket_maps.find(destination.str());
        SocketMap *socketMap = nullptr;
        if (pool_itr == socket_maps.end())
        {
            // create pool for specific destination
            socketMap = new SocketMap();
            socket_maps[destination.str()] = socketMap;
        }
        else
        {
            socketMap = pool_itr->second;
        }
        socketMap->addSocket(socket);
        // std::cout << "added socket to pool with key " << dest_server_idx << ". Size is " << socketMap->size() << std::endl;
    }
    else
    {
        throw cRuntimeError("Connect should not be called after the beginning of the workload");
    }
}

TcpSocket *MultiSocketTcpAppBase::removeSocketFromPool(std::string remoteAddr)
{
    TcpSocket *socket = nullptr;
    bool found = false;
    // find pool
    SocketMap *pool = nullptr;
    auto pool_itr = socket_maps.find(remoteAddr);
    if (pool_itr == socket_maps.end())
    {
        throw cRuntimeError("removeSocketFromPool: Could not find pool for destination IP address %s. It should exist.", remoteAddr.c_str());
    }
    pool = pool_itr->second;
    std::map<int, ISocket *> &pool_map = pool->getMap();
    // std::cout << " begin == end: " << (pool_map.begin() == pool_map.end()) << " begin != end: " << (pool_map.begin() != pool_map.end()) << " pool_map.empty() " << pool_map.empty() << std::endl;
    for (std::map<int, ISocket *>::iterator socket_pair = pool_map.begin(); socket_pair != pool_map.end(); socket_pair++)
    {
        // std::cout << "Loopin eq " << (socket_pair == pool_map.end()) << " neq " << (socket_pair != pool_map.end()) << " size: " << pool_map.size() << " empty? " << pool_map.empty() << " begin == end: " << (pool_map.begin() == pool_map.end()) << std::endl;
        socket = check_and_cast_nullable<TcpSocket *>(socket_pair->second);
        found = true;
        break; // just take one
    }

    // Remove socket from pool
    if (found)
    {
        if (pool->removeSocket(socket) == nullptr)
            throw cRuntimeError("Failed while removing socket from pool");
        if (socket->getState() != TcpSocket::CONNECTED)
            throw cRuntimeError("Attempted to use socket that was not in CONNECTED state");
        // std::cout << "Pool size after removing socket " << pool->size() << std::endl;
    }

    return socket;
}

void MultiSocketTcpAppBase::returnSocketToPool(std::string remoteAddr, TcpSocket *socket)
{
    // std::cout << "Returning socket for addr " << remoteAddr << std::endl;
    // find pool
    SocketMap *pool = nullptr;
    auto pool_itr = socket_maps.find(remoteAddr);
    if (pool_itr == socket_maps.end())
    {
        throw cRuntimeError("returnSocketToPool: Could not find pool for destination IP address %s. It should exist.", remoteAddr.c_str());
    }
    pool = pool_itr->second;
    // std::cout << "Pool size b4 adding socket " << pool->size() << std::endl;
    pool->addSocket(socket);
    // std::cout << "Pool size after adding socket " << pool->size() << std::endl;
}

void MultiSocketTcpAppBase::socketEstablished(TcpSocket *)
{
    // *redefine* to perform or schedule first sending
    EV_INFO << "connected\n";
}

void MultiSocketTcpAppBase::close(int socket_id)
{
    EV_INFO << "issuing CLOSE command\n";
    // std::cout << "issuing CLOSE command" << std::endl;

    TcpSocket *socket = check_and_cast_nullable<TcpSocket *>(socket_map.getSocketById(socket_id));
    if (socket)
    {
        socket->close();
        return;
    }

    throw cRuntimeError("No socket was found for socket id %d", socket_id);
}

void MultiSocketTcpAppBase::sendPacket(Packet *msg)
{
    TcpSocket *socket = check_and_cast_nullable<TcpSocket *>(socket_map.findSocketFor(msg));
    // std::cout << getFullPath() << " sendPacket. sock state " << socket->getState() << std::endl;
    if (socket)
    {
        delete msg->removeTagIfPresent<SocketInd>();
        // EV << "SEPEHR: the packet is related to a socket with id " << socket->getSocketId() << endl;
        int numBytes = msg->getByteLength();
        emit(packetSentSignal, msg);
        socket->send(msg);

        packetsSent++;
        bytesSent += numBytes;
        return;
    }

    throw cRuntimeError("message %s(%s) arrived for unknown socket 2\n", msg->getFullName(), msg->getClassName());
    delete msg;
}

void MultiSocketTcpAppBase::refreshDisplay() const
{
    ApplicationBase::refreshDisplay();
    //    getDisplayString().setTagArg("t", 0, TcpSocket::stateName(socket.getState()));
}

void MultiSocketTcpAppBase::socketDataArrived(TcpSocket *, Packet *msg, bool)
{
    // *redefine* to perform or schedule next sending
    packetsRcvd++;
    bytesRcvd += msg->getByteLength();
    // EV << "Emitting packetReceivedSignal with msg: " << msg << endl;
    emit(packetReceivedSignal, msg);
    delete msg;
}

void MultiSocketTcpAppBase::socketPeerClosed(TcpSocket *socket_)
{
    //    ASSERT(socket_ == &socket);
    // close the connection (if not already closed)
    //    if (socket.getState() == TcpSocket::PEER_CLOSED) {
    //        EV_INFO << "remote TCP closed, closing here as well\n";
    //        close();
    //    }
}

void MultiSocketTcpAppBase::socketClosed(TcpSocket *)
{
    // *redefine* to start another session etc.
    EV_INFO << "connection closed\n";
}

void MultiSocketTcpAppBase::socketFailure(TcpSocket *, int code)
{
    // subclasses may override this function, and add code try to reconnect after a delay.
    EV_WARN << "connection broken\n";
    numBroken++;
}

void MultiSocketTcpAppBase::finish()
{
    std::string modulePath = getFullPath();

    EV_INFO << modulePath << ": opened " << numSessions << " sessions\n";
    EV_INFO << modulePath << ": sent " << bytesSent << " bytes in " << packetsSent << " packets\n";
    EV_INFO << modulePath << ": received " << bytesRcvd << " bytes in " << packetsRcvd << " packets\n";
}
