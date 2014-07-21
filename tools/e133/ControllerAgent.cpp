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
 * ControllerAgent.cpp
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
#include "tools/e133/ControllerAgent.h"
#include "tools/e133/E133HealthCheckedConnection.h"
#include "tools/e133/TCPConnectionStats.h"

using ola::NewCallback;
using ola::NewSingleCallback;
using ola::TimeInterval;
using ola::io::IOStack;
using ola::network::HealthCheckedConnection;
using ola::network::IPV4Address;
using ola::network::IPV4SocketAddress;
using ola::plugin::e131::TransportHeader;
using ola::rdm::RDMResponse;
using std::auto_ptr;
using std::string;
using std::vector;


// The max number of un-ack'ed messages we'll allow.
const unsigned int ControllerAgent::MAX_QUEUE_SIZE = 10;

const unsigned int ControllerAgent::TCP_CONNECT_TIMEOUT_SECONDS = 5;
const int16_t ControllerAgent::CONNECT_FAILURE_PENALTY = 200;

// Track the un-ack'ed messages.
class OutstandingMessage {
 public:
    OutstandingMessage(uint16_t endpoint, const RDMResponse *rdm_response)
      : m_endpoint(endpoint),
        m_message_sent(false),
        m_rdm_response(rdm_response) {
    }

    bool was_sent() const { return m_message_sent; }
    void set_was_sent(bool was_sent) { m_message_sent = was_sent; }

    const RDMResponse* rdm_response() const { return m_rdm_response.get(); }
    uint16_t endpoint() const { return m_endpoint; }

 private:
    uint16_t m_endpoint;
    bool m_message_sent;
    auto_ptr<const RDMResponse> m_rdm_response;

    OutstandingMessage(const OutstandingMessage&);
    OutstandingMessage& operator=(const OutstandingMessage&);
};


/**
 * Create a new ControllerAgent.
 * This listens for connections from the controllers, and will ensure that if
 * any controllers try to connect, at least one will be picked as the
 * designated controller.
 */
ControllerAgent::ControllerAgent(
    RefreshControllersCallback *refresh_controllers_cb,
    ola::io::SelectServerInterface *ss,
    ola::e133::MessageBuilder *message_builder,
    TCPConnectionStats *tcp_stats,
    unsigned int max_queue_size)
    : m_controllers_cb(refresh_controllers_cb),
      m_max_queue_size(max_queue_size),
      m_ss(ss),
      m_message_builder(message_builder),
      m_tcp_stats(tcp_stats),
      m_discovery_timeout(ola::thread::INVALID_TIMEOUT),
      m_tcp_connector(m_ss),
      m_connection_id(NULL),
      m_tcp_socket(NULL),
      m_health_checked_connection(NULL),
      m_message_queue(NULL),
      m_incoming_tcp_transport(NULL),
      m_root_inflator(
          NewCallback(this, &ControllerAgent::RLPDataReceived)),
      m_unsent_messages(false) {
  m_root_inflator.AddInflator(&m_e133_inflator);
  m_e133_inflator.AddInflator(&m_e133_status_inflator);

  m_e133_status_inflator.SetStatusHandler(
      NewCallback(this, &ControllerAgent::HandleStatusMessage));
}


ControllerAgent::~ControllerAgent() {
  if (!m_unacked_messages.empty())
    OLA_WARN << m_unacked_messages.size()
             << " RDM commands remain un-ack'ed and will not be delivered";
  ola::STLDeleteValues(&m_unacked_messages);

  TCPConnectionClosed();

  if (m_discovery_timeout != ola::thread::INVALID_TIMEOUT) {
    m_ss->RemoveTimeout(m_discovery_timeout);
  }

  if (m_connection_id) {
    if (!m_tcp_connector.Cancel(m_connection_id)) {
      OLA_WARN << "Failed to cancel connection " << m_connection_id;
    }
  }

  if (m_controllers_cb) {
    delete m_controllers_cb;
  }
}

bool ControllerAgent::Start() {
  if (!m_controllers_cb) {
    return false;
  }

  AttemptConnection();
  return true;
}

bool ControllerAgent::IsConnected() {
  return m_tcp_socket != NULL;
}

bool ControllerAgent::SendStatusMessage(
    uint16_t endpoint,
    const RDMResponse *raw_response) {
  auto_ptr<const RDMResponse> response(raw_response);

  if (m_unacked_messages.size() == m_max_queue_size) {
    OLA_WARN << "MessageQueue limit reached, no further messages will be held";
    return false;
  }

  unsigned int our_sequence_number = m_sequence_number.Next();
  if (ola::STLContains(m_unacked_messages, our_sequence_number)) {
    // TODO(simon): think about what we want to do here
    OLA_WARN << "Sequence number collision!";
    return false;
  }

  OutstandingMessage *message = new OutstandingMessage(
      endpoint, response.release());
  ola::STLInsertIfNotPresent(&m_unacked_messages, our_sequence_number, message);

  if (m_message_queue) {
    message->set_was_sent(
      SendRDMCommand(our_sequence_number, endpoint, message->rdm_response()));
  }
  return true;
}

bool ControllerAgent::CloseTCPConnection() {
  if (!m_tcp_socket)
    return false;

  ola::io::ConnectedDescriptor::OnCloseCallback *callback =
    m_tcp_socket->TransferOnClose();
  callback->Run();
  return true;
}

void ControllerAgent::AttemptConnection() {
  IPV4SocketAddress controller;
  if (!PickController(&controller)) {
    m_discovery_timeout = m_ss->RegisterSingleTimeout(
        TimeInterval(2, 0),
        NewSingleCallback(this, &ControllerAgent::AttemptConnection));
    return;
  }

  m_connection_id = m_tcp_connector.Connect(
      controller,
      TimeInterval(TCP_CONNECT_TIMEOUT_SECONDS, 0),
      NewSingleCallback(this, &ControllerAgent::ConnectionResult, controller));
}

bool ControllerAgent::PickController(IPV4SocketAddress *controller) {
  vector<E133ControllerInfo> controllers;
  m_controllers_cb->Run(&controllers);
  OLA_INFO << "I know about " << controllers.size() << " controllers";

  // The expected number of controllers is small, so we take the naive approach.
  bool all_bad = true;
  ControllerList::iterator iter = m_known_controllers.begin();
  for (; iter != m_known_controllers.end(); ++iter) {
    iter->seen = false;
    all_bad &= iter->priority < 0;
  }

  // loop through the new controller list, if
  vector<E133ControllerInfo>::iterator new_iter = controllers.begin();
  for (; new_iter != controllers.end(); ++new_iter) {
    ControllerList::iterator iter = m_known_controllers.begin();
    for (; iter != m_known_controllers.end(); ++iter) {
      if (new_iter->address == iter->address) {
        iter->seen = true;
        break;
      }
    }
    if (iter == m_known_controllers.end()) {
      OLA_INFO << "Added " << new_iter->address
               << " to the list of known controllers";
      ControllerInfo info;
      info.address = new_iter->address;
      info.priority = new_iter->priority;
      info.seen = true;
      m_known_controllers.push_back(info);
      all_bad = false;
    }
  }

  if (all_bad) {
    OLA_INFO << "All known controllers are bad, resetting priorities";
  }

  // Remove the old controllers and pick the best one.
  int16_t best_priority = -1;
  iter = m_known_controllers.begin();
  while (iter != m_known_controllers.end()) {
    if (!iter->seen) {
      OLA_INFO << "Removed " << iter->address;
      iter = m_known_controllers.erase(iter);
      continue;
    }

    if (iter->priority >= best_priority) {
      *controller = iter->address;
      best_priority = iter->priority;
    } else if (all_bad) {
      iter->priority += CONNECT_FAILURE_PENALTY;
    }
    iter++;
  }
  if (best_priority == -1) {
    return false;
  }

  OLA_INFO << "Selected " << *controller << " with priority " << best_priority;
  return true;
}

void ControllerAgent::ConnectionResult(IPV4SocketAddress controller_address,
                                       int fd, int error) {
  m_connection_id = NULL;
  if (fd == -1) {
    // connection failed, penalize this controller and pick the next best.
    OLA_INFO << "Failed to connect to " << controller_address << ": "
             << strerror(error);
    ControllerList::iterator iter = m_known_controllers.begin();
    for (; iter != m_known_controllers.end(); ++iter) {
      if (iter->address == controller_address) {
        iter->priority -= CONNECT_FAILURE_PENALTY;
      }
    }
    AttemptConnection();
    return;
  }

  OLA_INFO << "TCP Connection established to " << controller_address;
  m_tcp_stats->connection_events++;
  // TODO(simon): include the port here as well.
  m_tcp_stats->ip_address = controller_address.Host();
  NewTCPConnection(new ola::network::TCPSocket(fd));
}

void ControllerAgent::NewTCPConnection(
    ola::network::TCPSocket *socket_ptr) {
  auto_ptr<ola::network::TCPSocket> socket(socket_ptr);

  if (m_tcp_socket) {
    OLA_WARN << "Already got a TCP connection open, closing the new one";
    socket->Close();
    return;
  }

  m_tcp_socket = socket.release();
  if (m_message_queue)
    OLA_WARN << "Already have a MessageQueue";
  m_message_queue = new MessageQueue(m_tcp_socket, m_ss,
                                     m_message_builder->pool());

  if (m_health_checked_connection) {
    OLA_WARN << "Already have a E133HealthCheckedConnection";
  }

  m_health_checked_connection = new E133HealthCheckedConnection(
    m_message_builder,
    m_message_queue,
    ola::NewSingleCallback(
      this, &ControllerAgent::TCPConnectionUnhealthy),
    m_ss);

  // this sends a heartbeat message to indicate this is the live connection
  if (!m_health_checked_connection->Setup()) {
    OLA_WARN << "Failed to setup HealthCheckedConnection, closing TCP socket";
    delete m_health_checked_connection;
    m_health_checked_connection = NULL;
    delete m_message_queue;
    m_message_queue = NULL;
    m_tcp_socket->Close();
    delete m_tcp_socket;
    m_tcp_socket = NULL;
    return;
  }

  // TODO(simon): Send the first PDU here that contains our IP:Port:UID info.

  OLA_INFO << "New connection, sending any un-acked messages";
  bool sent_all = true;
  PendingMessageMap::iterator iter = m_unacked_messages.begin();
  for (; iter != m_unacked_messages.end(); iter++) {
    OutstandingMessage *message = iter->second;
    bool was_sent = SendRDMCommand(iter->first, message->endpoint(),
                                   message->rdm_response());
    sent_all &= was_sent;
    message->set_was_sent(was_sent);
  }
  m_unsent_messages = !sent_all;

  if (m_incoming_tcp_transport) {
    OLA_WARN << "Already have an IncomingTCPTransport";
  }
  m_incoming_tcp_transport = new ola::plugin::e131::IncomingTCPTransport(
      &m_root_inflator, m_tcp_socket);

  m_tcp_socket->SetOnData(
      NewCallback(this, &ControllerAgent::ReceiveTCPData));
  m_tcp_socket->SetOnClose(ola::NewSingleCallback(
      this, &ControllerAgent::TCPConnectionClosed));
  m_ss->AddReadDescriptor(m_tcp_socket);
}


/**
 * Called when there is new TCP data available
 */
void ControllerAgent::ReceiveTCPData() {
  if (m_incoming_tcp_transport) {
    if (!m_incoming_tcp_transport->Receive()) {
      OLA_WARN << "TCP STREAM IS BAD!!!";
      CloseTCPConnection();
    }
  }
}


/**
 * Called when the TCP connection goes unhealthy.
 */
void ControllerAgent::TCPConnectionUnhealthy() {
  OLA_INFO << "TCP connection went unhealthy, closing";
  m_tcp_stats->unhealthy_events++;

  CloseTCPConnection();
}


/**
 * Close and cleanup the TCP connection. This can be triggered one of three
 * ways:
 *  - remote end closes the connection
 *  - the local end decides to close the connection
 *  - the heartbeats time out
 */
void ControllerAgent::TCPConnectionClosed() {
  OLA_INFO << "TCP conection closed";

  // zero out the master's IP
  m_tcp_stats->ip_address = IPV4Address();
  m_ss->RemoveReadDescriptor(m_tcp_socket);

  // shutdown the tx side
  delete m_health_checked_connection;
  m_health_checked_connection = NULL;

  delete m_message_queue;
  m_message_queue = NULL;

  // shutdown the rx side
  delete m_incoming_tcp_transport;
  m_incoming_tcp_transport = NULL;

  // finally delete the socket
  m_tcp_socket->Close();
  delete m_tcp_socket;
  m_tcp_socket = NULL;
}


/**
 * Called when we receive a valid Root Layer PDU.
 */
void ControllerAgent::RLPDataReceived(const TransportHeader&) {
  if (m_health_checked_connection) {
    m_health_checked_connection->HeartbeatReceived();
  }
}


bool ControllerAgent::SendRDMCommand(
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


/**
 * Handle a E1.33 Status PDU on the TCP connection.
 */
void ControllerAgent::HandleStatusMessage(
    const TransportHeader *transport_header,
    const ola::plugin::e131::E133Header *e133_header,
    uint16_t status_code,
    const string &description) {
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
}
