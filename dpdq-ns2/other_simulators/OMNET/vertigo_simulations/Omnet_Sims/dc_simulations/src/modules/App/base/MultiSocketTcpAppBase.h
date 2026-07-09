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

#ifndef __INET_MULTISOCKETTCPAPPBASE_H
#define __INET_MULTISOCKETTCPAPPBASE_H

#include "inet/common/INETDefs.h"
#include "inet/applications/base/ApplicationBase.h"
#include "inet/transportlayer/contract/tcp/TcpSocket.h"
#include "inet/common/socket/SocketMap.h"
#include "map"

using namespace inet;

/**
 * Base class for clients app for TCP-based request-reply protocols or apps.
 * Handles a single session (and TCP connection) at a time.
 *
 * It needs the following NED parameters: localAddress, localPort, connectAddress, connectPort.
 */
class INET_API MultiSocketTcpAppBase : public ApplicationBase, public TcpSocket::ICallback
{
protected:
  std::map<std::string, SocketMap *> socket_maps; // maps server destinations IP addresses to a socket pool (SocketMap)
  SocketMap socket_map;                           // duplicate structure to avoid work.. Both must be in sync

  // statistics
  int numSessions;
  int numBroken;
  int packetsSent;
  int packetsRcvd;
  int bytesSent;
  int bytesRcvd;
  static int requestsSent; // global

  // statistics:
  static simsignal_t connectSignal;

  /*
   * Keeps the mapping between socket objects and the query IDs assigned to them
   */
  std::map<TcpSocket *, unsigned long> socket_query_mapper; // maps a socket to a query id

protected:
  virtual ~MultiSocketTcpAppBase();
  virtual void initialize(int stage) override;
  virtual int numInitStages() const override { return NUM_INIT_STAGES; }
  virtual void handleMessageWhenUp(cMessage *msg) override;
  virtual void finish() override;
  virtual void refreshDisplay() const override;

  /* Utility functions */
  virtual void connect(int local_port, int dest_server_idx, int connect_port, simtime_t startTime, bool is_bursty = false,
                       unsigned long query_id = 0);
  /* Returns nullptr if there are no sockets left */
  TcpSocket *removeSocketFromPool(std::string remoteAddr);
  void returnSocketToPool(std::string remoteAddr, TcpSocket *socket);
  void idxToL3Addr(int server_idx, L3Address &destination);

  virtual void close(int socket_id);
  virtual void sendPacket(Packet *pkt);

  virtual void handleTimer(cMessage *msg) = 0;

  /* TcpSocket::ICallback callback methods */
  virtual void socketDataArrived(TcpSocket *socket, Packet *msg, bool urgent) override;
  virtual void socketAvailable(TcpSocket *socket, TcpAvailableInfo *availableInfo) override { socket->accept(availableInfo->getNewSocketId()); }

  /*
   * returns the query id for a msg related to a socket
   */
  // unsigned long get_query_id_for_socket(Packet *msg);
  virtual void socketEstablished(TcpSocket *socket) override;
  virtual void socketPeerClosed(TcpSocket *socket) override;
  virtual void socketClosed(TcpSocket *socket) override;
  virtual void socketFailure(TcpSocket *socket, int code) override;
  virtual void socketStatusArrived(TcpSocket *socket, TcpStatusInfo *status) override {}
  virtual void socketDeleted(TcpSocket *socket) override {}
};

#endif // ifndef __INET_MULTISOCKETTCPAPPBASE_H
