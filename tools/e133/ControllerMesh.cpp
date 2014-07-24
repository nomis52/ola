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
 * ControllerMesh.cpp
 * Copyright (C) 2014 Simon Newton
 */

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "ola/Callback.h"
#include "ola/Logging.h"
#include "ola/acn/ACNVectors.h"
#include "ola/io/SelectServerInterface.h"
#include "ola/network/HealthCheckedConnection.h"
#include "ola/stl/STLUtils.h"
#include "ola/network/IPV4Address.h"
#include "ola/network/SocketAddress.h"
#include "ola/rdm/RDMCommand.h"
#include "ola/rdm/RDMCommandSerializer.h"
#include "plugins/e131/e131/E133Header.h"
#include "plugins/e131/e131/E133StatusInflator.h"
#include "plugins/e131/e131/RDMPDU.h"
#include "tools/e133/ControllerMesh.h"
#include "tools/e133/ControllerConnection.h"
#include "tools/e133/E133HealthCheckedConnection.h"
#include "tools/e133/TCPConnectionStats.h"

using ola::NewCallback;
using ola::NewSingleCallback;
using ola::TimeInterval;
using ola::io::IOStack;
using ola::network::HealthCheckedConnection;
using ola::network::IPV4Address;
using ola::network::IPV4SocketAddress;
using ola::network::TCPSocket;
using ola::plugin::e131::TransportHeader;
using std::auto_ptr;
using std::string;
using std::vector;


// The max number of un-ack'ed messages we'll allow.
const unsigned int ControllerMesh::MAX_QUEUE_SIZE = 10;

const unsigned int ControllerMesh::TCP_CONNECT_TIMEOUT_SECONDS = 5;
const int16_t ControllerMesh::CONNECT_FAILURE_PENALTY = 200;

// retry TCP connects after 5 seconds
const TimeInterval ControllerMesh::INITIAL_TCP_RETRY_DELAY(5, 0);
// we grow the retry interval to a max of 30 seconds
const TimeInterval ControllerMesh::MAX_TCP_RETRY_DELAY(30, 0);

/**
 * Create a new ControllerMesh.
 * This listens for connections from the controllers, and will ensure that if
 * any controllers try to connect, at least one will be picked as the
 * designated controller.
 */
ControllerMesh::ControllerMesh(
    RefreshControllersCallback *refresh_controllers_cb,
    ola::io::SelectServerInterface *ss,
    ola::e133::MessageBuilder *message_builder,
    uint16_t our_port,
    unsigned int max_queue_size)
    : m_controllers_cb(refresh_controllers_cb),
      m_our_port(our_port),
      m_max_queue_size(max_queue_size),
      m_ss(ss),
      m_message_builder(message_builder),
      m_discovery_timeout(ola::thread::INVALID_TIMEOUT),
      m_tcp_socket_factory(NewCallback(this, &ControllerMesh::OnTCPConnect)),
      m_tcp_connector(
          m_ss, &m_tcp_socket_factory,
          TimeInterval(TCP_CONNECT_TIMEOUT_SECONDS, 0)),
      m_backoff_policy(INITIAL_TCP_RETRY_DELAY, MAX_TCP_RETRY_DELAY) {
  m_e133_inflator.AddInflator(&m_e133_status_inflator);

  m_e133_status_inflator.SetStatusHandler(
      NewCallback(this, &ControllerMesh::HandleStatusMessage));
}

ControllerMesh::~ControllerMesh() {
  if (m_discovery_timeout != ola::thread::INVALID_TIMEOUT) {
    m_ss->RemoveTimeout(m_discovery_timeout);
  }

  ControllerList::iterator iter = m_known_controllers.begin();
  for (; iter != m_known_controllers.end(); ++iter) {
    delete *iter;
  }

  if (m_controllers_cb) {
    delete m_controllers_cb;
  }
}

bool ControllerMesh::Start() {
  if (!m_controllers_cb) {
    return false;
  }

  CheckForNewControllers();

  m_discovery_timeout = m_ss->RegisterRepeatingTimeout(
      TimeInterval(2, 0),
      NewCallback(this, &ControllerMesh::CheckForNewControllers));
  return true;
}

bool ControllerMesh::CheckForNewControllers() {
  vector<IPV4SocketAddress> controllers;
  m_controllers_cb->Run(&controllers);
  // OLA_INFO << "I know about " << controllers.size() << " controllers";

  ControllerList::iterator iter = m_known_controllers.begin();
  for (; iter != m_known_controllers.end(); ++iter) {
    (*iter)->seen = false;
  }

  vector<IPV4SocketAddress>::const_iterator c_iter = controllers.begin();
  for (; c_iter != controllers.end(); ++c_iter) {
    if (c_iter->Host() == IPV4Address::Loopback() &&
        c_iter->Port() == m_our_port) {
      // OLA_INFO << "Skipping " << *c_iter << " since it's ourself";
      continue;
    }

    ControllerConnection *info = FindControllerConnection(*c_iter);
    if (!info) {
      info = new ControllerConnection(
        *c_iter,
        m_ss,
        NewCallback(this, &ControllerMesh::TCPConnectionClosed),
        &m_e133_inflator);
      m_known_controllers.push_back(info);
      m_tcp_connector.AddEndpoint(*c_iter, &m_backoff_policy);
    }
    info->seen = true;
  }

  if (!m_known_controllers.empty()) {
    OLA_INFO << m_known_controllers.size() << " known controllers";
  }

  // Remove any controllers that no longer exist.
  for (iter = m_known_controllers.begin(); iter != m_known_controllers.end();) {
    if (!(*iter)->seen) {
      // TODO(simon): handle the case where the connection to the controller is
      // still open.
      OLA_INFO << "Removed " << (*iter)->Address();
      m_tcp_connector.RemoveEndpoint((*iter)->Address());
      delete *iter;
      iter = m_known_controllers.erase(iter);
      continue;
    }
    iter++;
  }
  return true;
}

void ControllerMesh::OnTCPConnect(TCPSocket *socket_ptr) {
  auto_ptr<TCPSocket> socket(socket_ptr);

  ola::network::GenericSocketAddress generic_peer = socket->GetPeerAddress();
  if (generic_peer.Family() != AF_INET) {
    OLA_WARN << "Unknown family " << generic_peer.Family();
    return;
  }
  IPV4SocketAddress peer = generic_peer.V4Addr();

  OLA_INFO << "Connected to controller at " << peer;

  ControllerConnection *info = FindControllerConnection(peer);

  if (!info) {
    OLA_WARN << "Can't find controller for " << peer;
    return;
  }

  info->SetupConnection(socket.release(), m_message_builder);
  return;

  /*
  if (info->m_tcp_socket) {
    OLA_WARN << "Already got a TCP connection open, closing the new one";
    socket->Close();
    return;
  }

  info->m_tcp_socket = socket.release();
  if (info->m_message_queue)
    OLA_WARN << "Already have a MessageQueue";
  info->m_message_queue = new MessageQueue(info->m_tcp_socket, m_ss,
                                           m_message_builder->pool());

  if (info->m_health_checked_connection) {
    OLA_WARN << "Already have a E133HealthCheckedConnection";
  }

  info->m_health_checked_connection = new E133HealthCheckedConnection(
    m_message_builder,
    info->m_message_queue,
    ola::NewSingleCallback(
      this, &ControllerMesh::TCPConnectionUnhealthy, peer),
    m_ss);

  // this sends a heartbeat message to indicate this is the live connection
  if (!info->m_health_checked_connection->Setup()) {
    OLA_WARN << "Failed to setup HealthCheckedConnection, closing TCP socket";
    delete info->m_health_checked_connection;
    info->m_health_checked_connection = NULL;
    delete info->m_message_queue;
    info->m_message_queue = NULL;
    info->m_tcp_socket->Close();
    delete info->m_tcp_socket;
    info->m_tcp_socket = NULL;
    return;
  }

  // TODO(simon): Send the first PDU here that contains our IP:Port:UID info.

  if (info->m_incoming_tcp_transport) {
    OLA_WARN << "Already have an IncomingTCPTransport";
  }
  info->m_incoming_tcp_transport = new ola::plugin::e131::IncomingTCPTransport(
      &m_root_inflator, info->m_tcp_socket);

  info->m_tcp_socket->SetOnData(
      NewCallback(this, &ControllerMesh::ReceiveTCPData,
      info->m_incoming_tcp_transport));
  info->m_tcp_socket->SetOnClose(ola::NewSingleCallback(
      this, &ControllerMesh::TCPConnectionClosed, peer));
  m_ss->AddReadDescriptor(info->m_tcp_socket);
  */
}


/**
 * Called when there is new TCP data available
void ControllerMesh::ReceiveTCPData(
    ola::plugin::e131::IncomingTCPTransport *incoming_tcp_transport) {
  if (!incoming_tcp_transport->Receive()) {
    OLA_WARN << "TCP STREAM IS BAD!!!";
    //CloseTCPConnection();
  }
}

 * Called when the TCP connection goes unhealthy.
void ControllerMesh::TCPConnectionUnhealthy(IPV4SocketAddress peer_addr) {
  OLA_INFO << "TCP connection to " << peer_addr << " went unhealthy, closing";
  ControllerConnection *info = FindControllerConnection(peer_addr);

  (void) info;
  //CloseTCPConnection();
}
*/

void ControllerMesh::TCPConnectionClosed(const IPV4SocketAddress &peer_addr) {
  m_tcp_connector.Disconnect(peer_addr);
}

  /*
  m_ss->RemoveReadDescriptor(info->m_tcp_socket);

  // shutdown the tx side
  delete info->m_health_checked_connection;
  info->m_health_checked_connection = NULL;

  delete info->m_message_queue;
  info->m_message_queue = NULL;

  // shutdown the rx side
  delete info->m_incoming_tcp_transport;
  info->m_incoming_tcp_transport = NULL;

  // finally delete the socket
  info->m_tcp_socket->Close();
  delete info->m_tcp_socket;
  info->m_tcp_socket = NULL;
  */

/**
 * Called when we receive a valid Root Layer PDU.
void ControllerMesh::RLPDataReceived(const TransportHeader&) {
  if (m_health_checked_connection) {
    m_health_checked_connection->HeartbeatReceived();
  }
}
  */

/*
bool ControllerMesh::SendRDMCommand(
    unsigned int sequence_number,
    uint16_t endpoint,
    const RDMResponse *rdm_response) {
  if (m_message_queue->LimitReached())
    return false;

  IOStack packet(m_message_builder->pool());
  ola::rdm::RDMCommandSerializer::Write(*rdm_response, &packet);
  ola::plugin::e131::RDMPDU::PrependPDU(&packet);
  m_message_builder->BuildTCPRootE133(
      &packet, ola::acn::VECTOR_FRAMING_RDMNET, sequence_number,
      endpoint);

  return m_message_queue->SendMessage(&packet);
}
*/


/**
 * Handle a E1.33 Status PDU on the TCP connection.
 */
void ControllerMesh::HandleStatusMessage(
    const TransportHeader *transport_header,
    const ola::plugin::e131::E133Header *e133_header,
    uint16_t status_code,
    const string &description) {
  (void) transport_header;
  (void) e133_header;
  (void) status_code;
  (void) description;
/*
  if (status_code != ola::e133::SC_E133_ACK) {
    OLA_INFO << "Received a non-ack status code from "
             << transport_header->Source() << ": " << status_code << " : "
             << description;
  }
  OLA_INFO << "Controller has ack'ed " << e133_header->Sequence();

  ola::STLRemoveAndDelete(&m_unacked_messages, e133_header->Sequence());
  if (m_unsent_messages && !m_message_queue->LimitReached()) {
    bool sent_all = true;
    PendingMessageMap::iterator iter = m_unacked_messages.begin();
    for (; iter != m_unacked_messages.end(); iter++) {
      OutstandingMessage *message = iter->second;
      if (message->was_sent())
        continue;
      bool was_sent = SendRDMCommand(iter->first, message->endpoint(),
                                     message->rdm_response());
      sent_all &= was_sent;
      message->set_was_sent(was_sent);
    }
    m_unsent_messages = !sent_all;
  }
*/
}

ControllerConnection *ControllerMesh::FindControllerConnection(
    const IPV4SocketAddress &peer_addr) {
  ControllerList::iterator iter = m_known_controllers.begin();
  for (; iter != m_known_controllers.end(); ++iter) {
    if ((*iter)->Address() == peer_addr) {
      return *iter;
    }
  }
  return NULL;
}
