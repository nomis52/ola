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
 * ControllerConnection.cpp
 * Copyright (C) 2014 Simon Newton
 */

#include <memory>

#include "ola/Callback.h"
#include "ola/Logging.h"
#include "ola/io/SelectServerInterface.h"
#include "ola/network/HealthCheckedConnection.h"
#include "ola/network/SocketAddress.h"
#include "tools/e133/ControllerConnection.h"
#include "tools/e133/E133HealthCheckedConnection.h"

using ola::NewCallback;
using ola::NewSingleCallback;
using ola::network::HealthCheckedConnection;
using ola::network::IPV4SocketAddress;
using ola::network::TCPSocket;
using ola::io::IOStack;
using ola::plugin::e131::TransportHeader;
using std::auto_ptr;

ControllerConnection::ControllerConnection(
    const ola::network::IPV4SocketAddress &address,
    ola::io::SelectServerInterface *ss,
    CloseCallback *close_callback,
    ola::plugin::e131::E133Inflator *m_e133_inflator)
      : m_address(address),
        m_ss(ss),
        m_close_callback(close_callback),
        m_root_inflator(
            NewCallback(this, &ControllerConnection::RLPDataReceived)) {
    m_root_inflator.AddInflator(m_e133_inflator);
  }

ControllerConnection::~ControllerConnection() {
  if (m_tcp_socket.get()) {
    CloseConnection();
  }
}

bool ControllerConnection::SetupConnection(
    TCPSocket *socket_ptr,
    ola::e133::MessageBuilder *message_builder) {
  if (m_tcp_socket.get()) {
    OLA_WARN << "Already got a TCP connection open, closing the new one";
    delete socket_ptr;
    return false;
  }

  m_tcp_socket.reset(socket_ptr);

  if (m_message_queue.get())
    OLA_WARN << "Already have a MessageQueue";
  m_message_queue.reset(new MessageQueue(m_tcp_socket.get(), m_ss,
                                         message_builder->pool()));

  if (m_health_checked_connection.get()) {
    OLA_WARN << "Already have a E133HealthCheckedConnection";
  }
  m_health_checked_connection.reset(new E133HealthCheckedConnection(
    message_builder,
    m_message_queue.get(),
    ola::NewSingleCallback(this, &ControllerConnection::ConnectionUnhealthy),
    m_ss));

  // this sends a heartbeat message to indicate this is the live connection
  if (!m_health_checked_connection->Setup()) {
    OLA_WARN << "Failed to setup HealthCheckedConnection, closing TCP socket";
    m_health_checked_connection.reset();
    m_message_queue.reset();
    m_tcp_socket.reset();
    return false;
  }

  // TODO(simon): Send the first PDU here that contains our IP:Port:UID info.

  if (m_incoming_tcp_transport.get()) {
    OLA_WARN << "Already have an IncomingTCPTransport";
  }
  m_incoming_tcp_transport.reset(new ola::plugin::e131::IncomingTCPTransport(
      &m_root_inflator, m_tcp_socket.get()));

  m_tcp_socket->SetOnData(
    NewCallback(this, &ControllerConnection::ReceiveTCPData));
  m_tcp_socket->SetOnClose(
    NewSingleCallback(this, &ControllerConnection::CloseConnection));
  m_ss->AddReadDescriptor(m_tcp_socket.get());
  return true;
}

bool ControllerConnection::SendMessage(IOStack *stack) {
  return m_message_queue->SendMessage(stack);
}

void ControllerConnection::CloseConnection() {
  OLA_INFO << "Closing TCP conection to " << m_address;
  m_ss->RemoveReadDescriptor(m_tcp_socket.get());

  // shutdown the tx side
  m_health_checked_connection.reset();
  m_message_queue.reset();
  m_incoming_tcp_transport.reset();

  // finally delete the socket
  m_tcp_socket.reset();

  m_close_callback->Run(m_address);
}

void ControllerConnection::ConnectionUnhealthy() {
  OLA_INFO << "Connection to " << m_address << " went unhealthy.";
  CloseConnection();
}

void ControllerConnection::ReceiveTCPData() {
  if (!m_incoming_tcp_transport->Receive()) {
    OLA_WARN << "TCP stream to " << m_address << " is bad";
    CloseConnection();
  }
}

void ControllerConnection::RLPDataReceived(const TransportHeader&) {
  if (m_health_checked_connection.get()) {
    m_health_checked_connection->HeartbeatReceived();
  }
}
