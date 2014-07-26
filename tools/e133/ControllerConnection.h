/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Library General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * ControllerConnection.h
 * Manages the connection between E1.33 controllers.
 * Copyright (C) 2014 Simon Newton
 */

#ifndef TOOLS_E133_CONTROLLERCONNECTION_H_
#define TOOLS_E133_CONTROLLERCONNECTION_H_

#include <stdint.h>

#include <memory>

#include "ola/Callback.h"
#include "ola/base/Macro.h"
#include "ola/io/IOStack.h"
#include "ola/e133/MessageBuilder.h"
#include "ola/io/SelectServerInterface.h"
#include "ola/network/SocketAddress.h"
#include "ola/network/TCPSocket.h"
#include "plugins/e131/e131/E133Inflator.h"
#include "plugins/e131/e131/RootInflator.h"
#include "plugins/e131/e131/TCPTransport.h"
#include "tools/e133/E133HealthCheckedConnection.h"
#include "tools/e133/MessageQueue.h"

/**
 * @brief Handles the health checked connection to a controller.
 *
 */
class ControllerConnection {
  // TODO(simonn): This should be merged with the E133HealthCheckedConnection
  // since it really represents the fundermental application layer connection
  // between components.
 public:
  typedef ola::Callback1<void, const ola::network::IPV4SocketAddress &>
      CloseCallback;

  ControllerConnection(const ola::network::IPV4SocketAddress &address,
                       ola::io::SelectServerInterface *ss,
                       CloseCallback *close_callback,
                       ola::plugin::e131::E133Inflator *m_e133_inflator);

  ~ControllerConnection();

  const ola::network::IPV4SocketAddress& Address() const { return m_address; }

  bool IsConnected() const { return m_tcp_socket.get() != NULL; }

  /**
   * @param socket the new TCPSocket, ownership is transferred.
   */
  bool SetupConnection(ola::network::TCPSocket *socket_ptr,
                       ola::e133::MessageBuilder *message_builder);

  bool SendMessage(ola::io::IOStack *stack);

 private:
  ola::network::IPV4SocketAddress m_address;
  ola::io::SelectServerInterface *m_ss;
  CloseCallback *m_close_callback;
  ola::plugin::e131::RootInflator m_root_inflator;

  std::auto_ptr<ola::network::TCPSocket> m_tcp_socket;
  std::auto_ptr<E133HealthCheckedConnection> m_health_checked_connection;
  std::auto_ptr<MessageQueue> m_message_queue;
  std::auto_ptr<ola::plugin::e131::IncomingTCPTransport>
      m_incoming_tcp_transport;

  void CloseConnection();
  void ConnectionUnhealthy();
  void ReceiveTCPData();
  void RLPDataReceived(const ola::plugin::e131::TransportHeader &header);

  DISALLOW_COPY_AND_ASSIGN(ControllerConnection);
};

#endif  // TOOLS_E133_CONTROLLERCONNECTION_H_
