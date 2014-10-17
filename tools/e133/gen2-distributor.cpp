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
 * gen2-distributorcpp
 * Copyright (C) 2014 Simon Newton
 *
 * A Generation II distributor which reflects E1.33 messages over TCP
 * connections.
 */

#include <ola/BaseTypes.h>
#include <ola/Callback.h>
#include <ola/Clock.h>
#include <ola/ExportMap.h>
#include <ola/Logging.h>
#include <ola/acn/CID.h>
#include <ola/base/Flags.h>
#include <ola/base/Init.h>
#include <ola/base/SysExits.h>
#include <ola/e133/MessageBuilder.h>
#include <ola/io/SelectServer.h>
#include <ola/io/StdinHandler.h>
#include <ola/network/TCPSocketFactory.h>
#include <ola/rdm/UID.h>
#include <ola/stl/STLUtils.h>
#include <signal.h>

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "plugins/e131/e131/E133ControllerInflator.h"
#include "plugins/e131/e131/E133ControllerPDU.h"
#include "plugins/e131/e131/E133Inflator.h"
#include "plugins/e131/e131/RootInflator.h"
#include "plugins/e131/e131/TCPTransport.h"
#include "tools/e133/E133ControllerEntry.h"
#include "tools/e133/E133DiscoveryAgent.h"
#include "tools/e133/E133HealthCheckedConnection.h"
#include "tools/e133/MessageQueue.h"

DEFINE_string(listen_ip, "", "The IP Address to listen on");
DEFINE_uint16(listen_port, 5569, "The port to listen on");
DEFINE_uint16(listen_backlog, 100,
              "The backlog for the listen() call. Often limited to 128");
DEFINE_string(e133_scope, "default", "The E1.33 scope to use.");

using ola::NewCallback;
using ola::NewSingleCallback;
using ola::STLFindOrNull;
using ola::TimeInterval;
using ola::TimeStamp;
using ola::network::GenericSocketAddress;
using ola::network::IPV4Address;
using ola::network::IPV4SocketAddress;
using ola::network::TCPSocket;
using ola::plugin::e131::IncomingTCPTransport;
using ola::rdm::UID;
using std::auto_ptr;
using std::cout;
using std::endl;
using std::string;
using std::vector;

/**
 * Holds the state for each device
 */
class ControllerState {
 public:
  ControllerState()
    : socket(NULL),
      message_queue(NULL),
      health_checked_connection(NULL),
      in_transport(NULL) {
  }

  // The following may be NULL.
  // The socket connected to the E1.33 device
  auto_ptr<TCPSocket> socket;
  auto_ptr<MessageQueue> message_queue;
  // The Health Checked connection
  auto_ptr<E133HealthCheckedConnection> health_checked_connection;
  auto_ptr<IncomingTCPTransport> in_transport;

 private:
  DISALLOW_COPY_AND_ASSIGN(ControllerState);
};

/**
 * An E1.33 Distributor.
 */
class Gen2Distributor {
 public:
  struct Options {
    IPV4SocketAddress distributor_ip;
  };

  explicit Gen2Distributor(const Options &options);
  ~Gen2Distributor();

  bool Start();
  void Stop();

 private:
  struct RemoteDevice {
    // This is the remote address of the controller we learnt the device from.
    IPV4SocketAddress controller_addr;
    IPV4SocketAddress device_addr;
  };

  typedef std::map<IPV4SocketAddress, ControllerState*> ControllerMap;
  typedef std::map<ola::rdm::UID, RemoteDevice> UIDMap;

  ControllerMap m_controller_map;
  UIDMap m_uid_map;

  const IPV4SocketAddress m_listen_address;
  ola::ExportMap m_export_map;
  ola::io::SelectServer m_ss;
  ola::network::TCPSocketFactory m_tcp_socket_factory;
  ola::network::TCPAcceptingSocket m_listen_socket;

  ola::e133::MessageBuilder m_message_builder;
  ola::plugin::e131::RootInflator m_root_inflator;
  ola::plugin::e131::E133Inflator m_e133_inflator;
  ola::plugin::e131::E133ControllerInflator m_e133_controller_inflator;

  auto_ptr<E133DiscoveryAgentInterface> m_discovery_agent;

  ola::io::StdinHandler m_stdin_handler;

  void ShowHelp();
  void ShowControllers();
  void ShowSummary();
  void ShowUIDMap();

  void Input(char c);

  void OnTCPConnect(TCPSocket *socket);
  void ReceiveTCPData(IPV4SocketAddress peer,
                      IncomingTCPTransport *transport);
  void RLPDataReceived(const ola::plugin::e131::TransportHeader &header);

  void SocketUnhealthy(IPV4SocketAddress peer);

  void SocketClosed(IPV4SocketAddress peer);

  void ControllerMessage(
      const ola::plugin::e131::TransportHeader *transport_header,
      uint16_t vector,
      const string &raw_data);

  void SendDeviceList(
    const ola::plugin::e131::TransportHeader *transport_header,
    unsigned int size);

  void LearnDevice(
    const ola::plugin::e131::TransportHeader *transport_header,
    const uint8_t *data, unsigned int size);

  void ForgetDevice(
    const ola::plugin::e131::TransportHeader *transport_header,
    const uint8_t *data, unsigned int size);

  void RemoveDevicesForController(
    const ola::network::IPV4SocketAddress &controller_address);

  DISALLOW_COPY_AND_ASSIGN(Gen2Distributor);
};

Gen2Distributor::Gen2Distributor(const Options &options)
    : m_listen_address(options.distributor_ip),
      m_ss(&m_export_map),
      m_tcp_socket_factory(
          NewCallback(this, &Gen2Distributor::OnTCPConnect)),
      m_listen_socket(&m_tcp_socket_factory),
      m_message_builder(ola::acn::CID::Generate(), "E1.33 Controller"),
      m_root_inflator(
          NewCallback(this, &Gen2Distributor::RLPDataReceived)),
      m_stdin_handler(&m_ss,
                      ola::NewCallback(this, &Gen2Distributor::Input)) {
  E133DiscoveryAgentInterface::Options agent_options;
  agent_options.include_distributors = false;
  agent_options.include_controllers = false;
  E133DiscoveryAgentFactory discovery_agent_factory;
  m_discovery_agent.reset(discovery_agent_factory.New(agent_options));
  m_discovery_agent->SetScope(FLAGS_e133_scope);

  m_root_inflator.AddInflator(&m_e133_inflator);
  m_e133_inflator.AddInflator(&m_e133_controller_inflator);
  m_e133_controller_inflator.SetControllerHandler(
      NewCallback(this, &Gen2Distributor::ControllerMessage));
}

Gen2Distributor::~Gen2Distributor() {
  if (m_discovery_agent.get()) {
    m_discovery_agent->Stop();
  }
}

bool Gen2Distributor::Start() {
  if (!m_listen_socket.Listen(m_listen_address, FLAGS_listen_backlog)) {
    return false;
  }
  OLA_INFO << "Listening on " << m_listen_address;

  E133DistributorEntry distributor;
  distributor.address = m_listen_address;
  distributor.scope = FLAGS_e133_scope.str();
  distributor.model = "Test Distributor";

  if (m_discovery_agent.get()) {
    m_discovery_agent->RegisterDistributor(distributor);
  }

  if (!m_discovery_agent->Start()) {
    OLA_INFO << "Failed to start E133DiscoveryAgent, trying again";
    return false;
  }

  m_ss.AddReadDescriptor(&m_listen_socket);
  ShowHelp();
  m_ss.Run();
  m_ss.RemoveReadDescriptor(&m_listen_socket);
  return true;
}

void Gen2Distributor::Stop() {
  m_ss.Terminate();
}

void Gen2Distributor::ShowHelp() {
  cout << "------------------" << endl;
  cout << "c   : Show connected controllers." << endl;
  cout << "h   : Show this message." << endl;
  cout << "s   : Show summary." << endl;
  cout << "u   : Show UID map." << endl;
  cout << "q - Quit." << endl;
  cout << "------------------" << endl;
}

void Gen2Distributor::ShowControllers() {
  cout << "------------------" << endl;
  ControllerMap::const_iterator iter = m_controller_map.begin();
  for (; iter != m_controller_map.end(); ++iter) {
    cout << iter->first << endl;
  }
  cout << "------------------" << endl;
}

void Gen2Distributor::ShowSummary() {
  cout << "------------------" << endl;
  cout << m_controller_map.size() << " controllers connected" << endl;
  cout << m_uid_map.size() << " known UIDs" << endl;
  cout << "------------------" << endl;
}

void Gen2Distributor::Input(char c) {
  switch (c) {
    case 'c':
      ShowControllers();
      break;
    case 'h':
      ShowHelp();
      break;
    case 'q':
      m_ss.Terminate();
      break;
    case 's':
      ShowSummary();
      break;
    default:
      break;
  }
}

void Gen2Distributor::OnTCPConnect(TCPSocket *socket_ptr) {
  auto_ptr<TCPSocket> socket(socket_ptr);

  GenericSocketAddress generic_peer = socket->GetPeerAddress();
  if (generic_peer.Family() != AF_INET) {
    OLA_WARN << "Unknown family " << generic_peer.Family();
    return;
  }
  IPV4SocketAddress peer = generic_peer.V4Addr();

  OLA_INFO << "Received new TCP connection from: " << peer;

  auto_ptr<ControllerState> device_state(new ControllerState());
  device_state->in_transport.reset(
      new IncomingTCPTransport(&m_root_inflator, socket.get()));

  socket->SetOnData(
      NewCallback(this, &Gen2Distributor::ReceiveTCPData, peer,
                  device_state->in_transport.get()));
  socket->SetOnClose(
      NewSingleCallback(this, &Gen2Distributor::SocketClosed, peer));

  device_state->message_queue.reset(
      new MessageQueue(socket.get(), &m_ss, m_message_builder.pool()));

  auto_ptr<E133HealthCheckedConnection> health_checked_connection(
      new E133HealthCheckedConnection(
          &m_message_builder,
          device_state->message_queue.get(),
          NewSingleCallback(this, &Gen2Distributor::SocketUnhealthy, peer),
          &m_ss));

  if (!health_checked_connection->Setup()) {
    OLA_WARN << "Failed to setup heartbeat controller for " << peer;
    return;
  }

  device_state->health_checked_connection.reset(
    health_checked_connection.release());
  device_state->socket.reset(socket.release());

  m_ss.AddReadDescriptor(socket_ptr);

  std::pair<ControllerMap::iterator, bool> p = m_controller_map.insert(
      std::pair<IPV4SocketAddress, ControllerState*>(peer, NULL));
  if (!p.second) {
    OLA_WARN << "Peer " << peer << " is already connected! This is a bug";
    delete p.first->second;
  }
  p.first->second = device_state.release();
}

void Gen2Distributor::ReceiveTCPData(IPV4SocketAddress peer,
                                    IncomingTCPTransport *transport) {
  if (!transport->Receive()) {
    OLA_WARN << "TCP STREAM IS BAD!!!";
    SocketClosed(peer);
  }
}

void Gen2Distributor::RLPDataReceived(
    const ola::plugin::e131::TransportHeader &header) {
  if (header.Transport() != ola::plugin::e131::TransportHeader::TCP)
    return;

  ControllerState *device_state = STLFindOrNull(m_controller_map,
                                                header.Source());

  if (!device_state) {
    OLA_FATAL << "Received data but unable to lookup socket for "
              << header.Source();
    return;
  }

  device_state->health_checked_connection->HeartbeatReceived();
}

void Gen2Distributor::SocketUnhealthy(IPV4SocketAddress peer) {
  OLA_INFO << "connection to " << peer << " went unhealthy";
  SocketClosed(peer);
}

void Gen2Distributor::SocketClosed(IPV4SocketAddress peer) {
  OLA_INFO << "Connection to " << peer << " was closed";

  auto_ptr<ControllerState> device(ola::STLLookupAndRemovePtr(
      &m_controller_map, peer));

  if (!device.get()) {
    OLA_WARN << "Can't find device entry";
    return;
  }

  m_ss.RemoveReadDescriptor(device->socket.get());

  RemoveDevicesForController(peer);
}

void Gen2Distributor::ControllerMessage(
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
    case ola::acn::VECTOR_CONTROLLER_FETCH_DEVICES:
      SendDeviceList(transport_header, raw_data.size());
      break;
    case ola::acn::VECTOR_CONTROLLER_DEVICE_ACQUIRED:
      LearnDevice(transport_header,
                  reinterpret_cast<const uint8_t*>(raw_data.data()),
                  raw_data.size());
      break;
    case ola::acn::VECTOR_CONTROLLER_DEVICE_RELEASED:
      ForgetDevice(transport_header,
                  reinterpret_cast<const uint8_t*>(raw_data.data()),
                  raw_data.size());
      break;
    default:
      break;
  }
}

void Gen2Distributor::SendDeviceList(
    const ola::plugin::e131::TransportHeader *transport_header,
    unsigned int size) {
  if (size != 0) {
    OLA_WARN << "FetchDeviceList message of incorrect size " << size;
    return;
  }

  ControllerState *state = STLFindOrNull(m_controller_map,
                                         transport_header->Source());
  if (!state) {
    OLA_WARN << "Can't find state for " << transport_header->Source();
    return;
  }

  if (!state->message_queue.get()) {
    return;
  }

  struct DeviceEntry {
    uint32_t ip;
    uint16_t port;
    uint8_t uid[ola::rdm::UID::LENGTH];
  } __attribute__((packed));

  ola::io::IOStack packet(m_message_builder.pool());
  unsigned int device_count = 0;
  UIDMap::const_iterator iter = m_uid_map.begin();
  for (; iter != m_uid_map.end(); ++iter) {
    DeviceEntry entry;
    entry.ip = iter->second.device_addr.Host().AsInt();
    entry.port = ola::network::HostToNetwork(iter->second.device_addr.Port());
    iter->first.Pack(reinterpret_cast<uint8_t*>(&entry.uid),
                     ola::rdm::UID::LENGTH);
    packet.Write(reinterpret_cast<const uint8_t*>(&entry), sizeof(entry));
    device_count++;
  }

  ola::plugin::e131::E133ControllerPDU::PrependPDU(
      ola::acn::VECTOR_CONTROLLER_DEVICE_LIST, &packet);
  m_message_builder.BuildTCPRootE133(
      &packet, ola::acn::VECTOR_FRAMING_CONTROLLER, 0, 0);

  OLA_INFO << "Sending VECTOR_CONTROLLER_DEVICE_LIST message with "
           << device_count << " devices to " << transport_header->Source();
  state->message_queue->SendMessage(&packet);
}

void Gen2Distributor::LearnDevice(
    const ola::plugin::e131::TransportHeader *transport_header,
    const uint8_t *data, unsigned int size) {
  struct DeviceAcquireMessage {
    uint32_t ip;
    uint16_t port;
    uint8_t uid[ola::rdm::UID::LENGTH];
  } __attribute__((packed));

  DeviceAcquireMessage message;
  if (size != sizeof(message)) {
    OLA_WARN << "DeviceAcquireMessage of incorrect size "
             << size << " != " << sizeof(message);
    return;
  }

  memcpy(reinterpret_cast<uint8_t*>(&message), data, sizeof(message));
  IPV4SocketAddress remote_device(IPV4Address(message.ip),
                                  ola::network::NetworkToHost(message.port));
  OLA_INFO << "Informed about device at " << remote_device;
  ola::rdm::UID uid(message.uid);

  RemoteDevice device;
  device.controller_addr = transport_header->Source(),
  device.device_addr = remote_device;
  ola::STLReplace(&m_uid_map, uid, device);

  // TODO(simon): Notify all connected controllers here.
}

void Gen2Distributor::ForgetDevice(
    const ola::plugin::e131::TransportHeader *transport_header,
    const uint8_t *data, unsigned int size) {
  struct DeviceReleaseMessage {
    uint8_t uid[ola::rdm::UID::LENGTH];
  } __attribute__((packed));

  DeviceReleaseMessage message;
  if (size != sizeof(message)) {
    OLA_WARN << "DeviceReleaseMessage of incorrect size "
             << size << " != " << sizeof(message);
    return;
  }

  memcpy(reinterpret_cast<uint8_t*>(&message), data, sizeof(message));
  ola::rdm::UID uid(message.uid);
  OLA_INFO << "Informed to forget about " << uid;

  UIDMap::iterator iter = m_uid_map.find(uid);
  if (iter == m_uid_map.end()) {
    OLA_WARN << "UID " << uid << " not found in map, inconsistent state!";
    return;
  }

  if (iter->second.controller_addr != transport_header->Source()) {
    OLA_WARN << "Release for " << uid << ", owner mismatch "
             << iter->second.controller_addr << " != "
             << transport_header->Source();
  }

  m_uid_map.erase(iter);

  // TODO(simon): Notify all connected controllers here.
}

void Gen2Distributor::RemoveDevicesForController(
    const ola::network::IPV4SocketAddress &controller_address) {
  UIDMap::iterator iter = m_uid_map.begin();
  while (iter != m_uid_map.end()) {
    if (controller_address == iter->second.controller_addr) {
      OLA_INFO << "Removed UID " << iter->first;
      m_uid_map.erase(iter++);
    } else {
      ++iter;
    }
  }

  // TODO(simon): Notify all connected controllers here.
}

class Gen2Distributor *distributor = NULL;

/**
 * Interupt handler
 */
static void InteruptSignal(OLA_UNUSED int unused) {
  if (distributor)
    distributor->Stop();
}

int main(int argc, char *argv[]) {
  ola::SetHelpString("[options]", "E1.33 Distributor.");
  ola::ParseFlags(&argc, argv);
  ola::InitLoggingFromFlags();

  // Convert the distributor's IP address
  IPV4Address distributor_ip;
  if (!FLAGS_listen_ip.str().empty() &&
      !IPV4Address::FromString(FLAGS_listen_ip, &distributor_ip)) {
    ola::DisplayUsage();
    exit(ola::EXIT_USAGE);
  }

  ola::InstallSignal(SIGINT, InteruptSignal);
  Gen2Distributor::Options options;
  options.distributor_ip = IPV4SocketAddress(distributor_ip, FLAGS_listen_port);
  distributor = new Gen2Distributor(options);
  distributor->Start();
  delete distributor;
  distributor = NULL;
}

