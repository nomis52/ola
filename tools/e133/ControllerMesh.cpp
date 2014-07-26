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
#include <utility>
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
#include "plugins/e131/e131/E133ControllerPDU.h"
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
    AddDeviceCallback *add_device_callback,
    ControllerDisconnectCallback *disconnect_cb,
    ola::io::SelectServerInterface *ss,
    ola::e133::MessageBuilder *message_builder,
    uint16_t our_port,
    unsigned int max_queue_size)
    : m_controllers_cb(refresh_controllers_cb),
      m_add_device_callback(add_device_callback),
      m_disconnect_cb(disconnect_cb),
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
  m_e133_inflator.AddInflator(&m_e133_controller_inflator);

  m_e133_status_inflator.SetStatusHandler(
      NewCallback(this, &ControllerMesh::HandleStatusMessage));

  m_e133_controller_inflator.SetControllerHandler(
            NewCallback(this, &ControllerMesh::ControllerMessage));
}

ControllerMesh::~ControllerMesh() {
  if (m_discovery_timeout != ola::thread::INVALID_TIMEOUT) {
    m_ss->RemoveTimeout(m_discovery_timeout);
  }

  ControllerList::iterator iter = m_known_controllers.begin();
  for (; iter != m_known_controllers.end(); ++iter) {
    delete iter->connection;
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

unsigned int ControllerMesh::ConnectedControllerCount() {
  unsigned int count = 0;
  ControllerList::iterator iter = m_known_controllers.begin();
  for (; iter != m_known_controllers.end(); ++iter) {
    if (iter->connection->IsConnected()) {
      count++;
    }
  }
  return count;
}

void ControllerMesh::InformControllerOfDevices(
    const IPV4SocketAddress &controller_address,
    const std::vector<std::pair<
      ola::rdm::UID,
      ola::network::IPV4SocketAddress> > &devices) {
  ControllerConnection *connection =
    FindControllerConnection(controller_address);
  if (!connection) {
    OLA_WARN << "Can't find controller " << controller_address;
    return;
  }

  (void) devices;
}

void ControllerMesh::InformControllersOfAcquiredDevice(
    const ola::rdm::UID &uid,
    const ola::network::IPV4SocketAddress &udp_address) {
  struct DeviceAcquireMessage {
    uint32_t ip;
    uint16_t port;
    uint8_t uid[ola::rdm::UID::LENGTH];
  } __attribute__((packed));

  DeviceAcquireMessage message;
  message.ip = udp_address.Host().AsInt();
  message.port = ola::network::HostToNetwork(udp_address.Port());
  uid.Pack(reinterpret_cast<uint8_t*>(&message.uid), ola::rdm::UID::LENGTH);

  ControllerList::iterator iter = m_known_controllers.begin();
  // TODO(simon): this creates one copy for each controller. At some point we
  // should ref count the memory blocks.
  for (; iter != m_known_controllers.end(); ++iter) {
    if (!iter->connection->IsConnected()) {
      continue;
    }

    IOStack packet(m_message_builder->pool());
    packet.Write(reinterpret_cast<const uint8_t*>(&message), sizeof(message));
    ola::plugin::e131::E133ControllerPDU::PrependPDU(
        ola::acn::VECTOR_CONTROLLER_DEVICE_ACQUIRED, &packet);
    m_message_builder->BuildTCPRootE133(
        &packet, ola::acn::VECTOR_FRAMING_CONTROLLER, 0, 0);

    if (!iter->connection->SendMessage(&packet)) {
      OLA_WARN << "Failed to send release device to "
               << iter->connection->Address();
    }
  }
}

void ControllerMesh::InformControllersOfReleasedDevice(
    const ola::rdm::UID &uid) {
  struct DeviceReleaseMessage {
    uint8_t uid[ola::rdm::UID::LENGTH];
  } __attribute__((packed));

  DeviceReleaseMessage message;
  uid.Pack(reinterpret_cast<uint8_t*>(&message.uid), ola::rdm::UID::LENGTH);

  ControllerList::iterator iter = m_known_controllers.begin();
  // TODO(simon): this creates one copy for each controller. At some point we
  // should ref count the memory blocks.
  for (; iter != m_known_controllers.end(); ++iter) {
    if (!iter->connection->IsConnected()) {
      continue;
    }

    IOStack packet(m_message_builder->pool());
    packet.Write(reinterpret_cast<const uint8_t*>(&message), sizeof(message));
    ola::plugin::e131::E133ControllerPDU::PrependPDU(
        ola::acn::VECTOR_CONTROLLER_DEVICE_RELEASED, &packet);
    m_message_builder->BuildTCPRootE133(
        &packet, ola::acn::VECTOR_FRAMING_CONTROLLER, 0, 0);

    if (!iter->connection->SendMessage(&packet)) {
      OLA_WARN << "Failed to send release device to "
               << iter->connection->Address();
    }
  }
}

void ControllerMesh::PrintStats() {
  ControllerList::iterator iter = m_known_controllers.begin();
  std::cout << "------------------" << std::endl;
  for (; iter != m_known_controllers.end(); ++iter) {
    std::cout << iter->connection->Address() << " : "
              << (iter->connection->IsConnected() ?
                  "connected" : "disconnected")
              << std::endl;
  }
  std::cout << "------------------" << std::endl;
}

bool ControllerMesh::CheckForNewControllers() {
  vector<IPV4SocketAddress> controllers;
  m_controllers_cb->Run(&controllers);
  // OLA_INFO << "I know about " << controllers.size() << " controllers";

  ControllerList::iterator iter = m_known_controllers.begin();
  for (; iter != m_known_controllers.end(); ++iter) {
    iter->seen = false;
  }

  vector<IPV4SocketAddress>::const_iterator c_iter = controllers.begin();
  for (; c_iter != controllers.end(); ++c_iter) {
    if (c_iter->Host() == IPV4Address::Loopback() &&
        c_iter->Port() == m_our_port) {
      // OLA_INFO << "Skipping " << *c_iter << " since it's ourself";
      continue;
    }

    iter = m_known_controllers.begin();
    for (; iter != m_known_controllers.end(); ++iter) {
      if (iter->connection->Address() == *c_iter) {
        break;
      }
    }

    if (iter == m_known_controllers.end()) {
      ControllerConnection *connection = new ControllerConnection(
        *c_iter,
        m_ss,
        m_message_builder,
        NewCallback(this, &ControllerMesh::TCPConnectionClosed),
        &m_e133_inflator);
      ControllerInfo info;
      info.connection = connection;
      info.seen = true;
      m_known_controllers.push_back(info);
      m_tcp_connector.AddEndpoint(*c_iter, &m_backoff_policy);
    } else {
      iter->seen = true;
    }
  }

  // Remove any controllers that no longer exist.
  for (iter = m_known_controllers.begin(); iter != m_known_controllers.end();) {
    if (!iter->seen) {
      // TODO(simon): handle the case where the connection to the controller is
      // still open.
      OLA_INFO << "Removed " << iter->connection->Address();
      m_tcp_connector.RemoveEndpoint(iter->connection->Address());
      delete iter->connection;
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
  if (info) {
    info->SetupConnection(socket.release(), m_message_builder);
  } else {
    OLA_WARN << "Can't find controller for " << peer;
  }
}

void ControllerMesh::TCPConnectionClosed(const IPV4SocketAddress &peer_addr) {
  if (m_disconnect_cb) {
    m_disconnect_cb->Run(peer_addr);
  }
  m_tcp_connector.Disconnect(peer_addr);
}

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
    if (iter->connection->Address() == peer_addr) {
      return iter->connection;
    }
  }
  return NULL;
}


void ControllerMesh::ControllerMessage(
    const ola::plugin::e131::TransportHeader *transport_header,
    uint16_t vector,
    const string &raw_data) {
  OLA_INFO << "Got controller message with vector " << vector << " ,size  "
           << raw_data.size();
  if (transport_header->Transport() !=
      ola::plugin::e131::TransportHeader::TCP) {
    OLA_WARN << "Controller message via UDP!";
    return;
  }

  switch (vector) {
    case ola::acn::VECTOR_CONTROLLER_DEVICE_LIST:
      DeviceList(transport_header->Source(),
                 reinterpret_cast<const uint8_t*>(raw_data.data()),
                 raw_data.size());
      break;
    default:
      break;
  }
}

void ControllerMesh::DeviceList(
    const ola::network::IPV4SocketAddress &controller_address,
    const uint8_t *data, unsigned int size) {

  struct DeviceEntry {
    uint32_t ip;
    uint16_t port;
    uint8_t uid[ola::rdm::UID::LENGTH];
  } __attribute__((packed));

  if (size % sizeof(DeviceEntry) != 0) {
    OLA_WARN << "Invalid multiple of " << sizeof(DeviceEntry)
             << " in VECTOR_CONTROLLER_DEVICE_LIST message";
    return;
  }

  if (!m_add_device_callback) {
    return;
  }

  const uint8_t *ptr = data;
  while (ptr < data + size) {
    DeviceEntry device;
    memcpy(reinterpret_cast<uint8_t*>(&device), data, size);
    IPV4SocketAddress remote_device(
        IPV4Address(device.ip),
        ola::network::NetworkToHost(device.port));
      ola::rdm::UID uid(device.uid);

    m_add_device_callback->Run(remote_device, controller_address, uid);
    ptr += sizeof(DeviceEntry);
  }
}
