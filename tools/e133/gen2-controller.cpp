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
 * gen2-controller.cpp
 * Copyright (C) 2014 Simon Newton
 *
 * A Generation II controller which listens for new TCP connections from
 * devices.
 * I'm using this for scale testing.
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
#include "tools/e133/ControllerMesh.h"
#include "tools/e133/E133ControllerEntry.h"
#include "tools/e133/E133DiscoveryAgent.h"
#include "tools/e133/E133HealthCheckedConnection.h"
#include "tools/e133/MessageQueue.h"

DEFINE_string(listen_ip, "", "The IP Address to listen on");
DEFINE_uint16(listen_port, 5569, "The port to listen on");
DEFINE_uint16(listen_backlog, 100,
              "The backlog for the listen() call. Often limited to 128");
DEFINE_uint32(expected_devices, 1,
              "Time how long it takes until this many devices connect.");
DEFINE_bool(stop_after_all_devices, false,
            "Exit once all devices connect");

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
class DeviceState {
 public:
  DeviceState()
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
  DISALLOW_COPY_AND_ASSIGN(DeviceState);
};

/**
 * A very simple E1.33 Controller that uses the reverse-connection model.
 */
class Gen2Controller {
 public:
  struct Options {
    // The controller to connect to.
    IPV4SocketAddress controller;

    explicit Options(const IPV4SocketAddress &controller)
        : controller(controller) {
    }
  };

  explicit Gen2Controller(const Options &options);
  ~Gen2Controller();

  bool Start();
  void Stop();

 private:
  struct RemoteDevice {
    // This is either the remote address of the device or the remote address of
    // the controller we learnt the device from.
    IPV4SocketAddress tcp_socket;
    IPV4SocketAddress udp_dest;
    bool local;
  };

  typedef std::map<IPV4SocketAddress, DeviceState*> DeviceMap;
  typedef std::map<ola::rdm::UID, RemoteDevice> UIDMap;

  TimeStamp m_start_time;
  DeviceMap m_device_map;
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

  auto_ptr<ControllerMesh> m_controller_mesh;

  ola::io::StdinHandler m_stdin_handler;

  void ShowHelp();
  void ShowDevices();
  void ShowSummary();
  void ShowUIDMap();

  void Input(char c);

  void GetControllerList(vector<IPV4SocketAddress> *controllers);

  bool PrintStats();

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

  void RegisterDevice(
    const ola::plugin::e131::TransportHeader *transport_header,
    const uint8_t *data, unsigned int size);

  void AddDevice(
    const ola::network::IPV4SocketAddress &device_address,
    const ola::network::IPV4SocketAddress &learnt_via,
    const ola::rdm::UID &uid);

  void RemoveDevicesForController(
    bool is_local,
    const ola::network::IPV4SocketAddress &controller_address);

  DISALLOW_COPY_AND_ASSIGN(Gen2Controller);
};

Gen2Controller::Gen2Controller(const Options &options)
    : m_listen_address(options.controller),
      m_ss(&m_export_map),
      m_tcp_socket_factory(
          NewCallback(this, &Gen2Controller::OnTCPConnect)),
      m_listen_socket(&m_tcp_socket_factory),
      m_message_builder(ola::acn::CID::Generate(), "E1.33 Controller"),
      m_root_inflator(
          NewCallback(this, &Gen2Controller::RLPDataReceived)),
      m_stdin_handler(&m_ss,
                      ola::NewCallback(this, &Gen2Controller::Input)) {
  E133DiscoveryAgentFactory discovery_agent_factory;
  m_discovery_agent.reset(discovery_agent_factory.New());
  m_discovery_agent->Init();

  m_root_inflator.AddInflator(&m_e133_inflator);
  m_e133_inflator.AddInflator(&m_e133_controller_inflator);
  m_e133_controller_inflator.SetControllerHandler(
      NewCallback(this, &Gen2Controller::ControllerMessage));
}

Gen2Controller::~Gen2Controller() {
  if (m_discovery_agent.get()) {
    m_discovery_agent->Stop();
  }
}

bool Gen2Controller::Start() {
  ola::Clock clock;
  clock.CurrentTime(&m_start_time);

  if (!m_listen_socket.Listen(m_listen_address, FLAGS_listen_backlog)) {
    return false;
  }
  OLA_INFO << "Listening on " << m_listen_address;
  if (m_discovery_agent.get()) {
    m_discovery_agent->RegisterController(m_listen_address);
  }

  m_controller_mesh.reset(new ControllerMesh(
        NewCallback(this, &Gen2Controller::GetControllerList),
        NewCallback(this, &Gen2Controller::AddDevice),
        NewCallback(this, &Gen2Controller::RemoveDevicesForController, false),
        &m_ss, &m_message_builder, m_listen_address.Port()));
  m_controller_mesh->Start();

  m_ss.AddReadDescriptor(&m_listen_socket);
  m_ss.RegisterRepeatingTimeout(
      TimeInterval(0, 500000),
      NewCallback(this, &Gen2Controller::PrintStats));
    ShowHelp();
  m_ss.Run();
  m_ss.RemoveReadDescriptor(&m_listen_socket);
  return true;
}

void Gen2Controller::Stop() {
  m_ss.Terminate();
}

void Gen2Controller::GetControllerList(
    vector<IPV4SocketAddress> *controllers) {
  ControllerEntryList e133_controllers;
  m_discovery_agent->FindControllers(&e133_controllers);

  ControllerEntryList::iterator iter = e133_controllers.begin();
  for (; iter != e133_controllers.end(); ++iter) {
    controllers->push_back(iter->address);
  }
}

void Gen2Controller::ShowHelp() {
  cout << "------------------" << endl;
  cout << "c - Show peer controllers." << endl;
  cout << "d - Show connected devices." << endl;
  cout << "h - Show this message." << endl;
  cout << "s - Show summary." << endl;
  cout << "u - Show UID map." << endl;
  cout << "q - Quit." << endl;
  cout << "------------------" << endl;
}

void Gen2Controller::ShowDevices() {
  cout << "------------------" << endl;
  DeviceMap::const_iterator iter = m_device_map.begin();
  for (; iter != m_device_map.end(); ++iter) {
    cout << iter->first << endl;
  }
  cout << "------------------" << endl;
}

void Gen2Controller::ShowSummary() {
  cout << "------------------" << endl;
  cout << m_controller_mesh->ConnectedControllerCount()
       << " controllers connected" << endl;
  cout << m_device_map.size() << " devices connected" << endl;
  cout << m_uid_map.size() << " known UIDs" << endl;
  cout << "------------------" << endl;
}

void Gen2Controller::ShowUIDMap() {
  cout << "------------------" << endl;
  UIDMap::const_iterator iter = m_uid_map.begin();
  for (; iter != m_uid_map.end(); ++iter) {
    cout << iter->first << " -> " << iter->second.udp_dest << ", via "
         << iter->second.tcp_socket
         << (iter->second.local ? " (local)" : "") << endl;
  }
  cout << "------------------" << endl;
}

void Gen2Controller::Input(char c) {
  switch (c) {
    case 'c':
      m_controller_mesh->PrintStats();
      break;
    case 'd':
      ShowDevices();
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
    case 'u':
      ShowUIDMap();
      break;
    default:
      break;
  }
}

bool Gen2Controller::PrintStats() {
  /*
  const TimeStamp *now = m_ss.WakeUpTime();
  const TimeInterval delay = *now - m_start_time;
  ola::CounterVariable *ss_iterations = m_export_map.GetCounterVar(
      "ss-loop-count");
  OLA_INFO << delay << "," << m_device_map.size() << ","
      << ss_iterations->Value();
  */
  return true;
}

void Gen2Controller::OnTCPConnect(TCPSocket *socket_ptr) {
  auto_ptr<TCPSocket> socket(socket_ptr);

  GenericSocketAddress generic_peer = socket->GetPeerAddress();
  if (generic_peer.Family() != AF_INET) {
    OLA_WARN << "Unknown family " << generic_peer.Family();
    return;
  }
  IPV4SocketAddress peer = generic_peer.V4Addr();

  // OLA_INFO << "Received new TCP connection from: " << peer;

  auto_ptr<DeviceState> device_state(new DeviceState());
  device_state->in_transport.reset(
      new IncomingTCPTransport(&m_root_inflator, socket.get()));

  socket->SetOnData(
      NewCallback(this, &Gen2Controller::ReceiveTCPData, peer,
                  device_state->in_transport.get()));
  socket->SetOnClose(
      NewSingleCallback(this, &Gen2Controller::SocketClosed, peer));

  device_state->message_queue.reset(
      new MessageQueue(socket.get(), &m_ss, m_message_builder.pool()));

  auto_ptr<E133HealthCheckedConnection> health_checked_connection(
      new E133HealthCheckedConnection(
          &m_message_builder,
          device_state->message_queue.get(),
          NewSingleCallback(this, &Gen2Controller::SocketUnhealthy, peer),
          &m_ss));

  if (!health_checked_connection->Setup()) {
    OLA_WARN << "Failed to setup heartbeat controller for " << peer;
    return;
  }

  device_state->health_checked_connection.reset(
    health_checked_connection.release());
  device_state->socket.reset(socket.release());

  m_ss.AddReadDescriptor(socket_ptr);

  std::pair<DeviceMap::iterator, bool> p = m_device_map.insert(
      std::pair<IPV4SocketAddress, DeviceState*>(peer, NULL));
  if (!p.second) {
    OLA_WARN << "Peer " << peer << " is already connected! This is a bug";
    delete p.first->second;
  }
  p.first->second = device_state.release();

  if (m_device_map.size() == FLAGS_expected_devices) {
    ola::Clock clock;
    TimeStamp now;
    clock.CurrentTime(&now);
    OLA_INFO << FLAGS_expected_devices << " connected in "
             << (now - m_start_time);
    if (FLAGS_stop_after_all_devices) {
      m_ss.Terminate();
    }
  }
}

void Gen2Controller::ReceiveTCPData(IPV4SocketAddress peer,
                                    IncomingTCPTransport *transport) {
  if (!transport->Receive()) {
    OLA_WARN << "TCP STREAM IS BAD!!!";
    SocketClosed(peer);
  }
}

void Gen2Controller::RLPDataReceived(
    const ola::plugin::e131::TransportHeader &header) {
  if (header.Transport() != ola::plugin::e131::TransportHeader::TCP)
    return;

  DeviceState *device_state = STLFindOrNull(m_device_map, header.Source());

  if (!device_state) {
    OLA_FATAL << "Received data but unable to lookup socket for "
        << header.Source();
    return;
  }

  device_state->health_checked_connection->HeartbeatReceived();
}

void Gen2Controller::SocketUnhealthy(IPV4SocketAddress peer) {
  OLA_INFO << "connection to " << peer << " went unhealthy";
  SocketClosed(peer);
}

void Gen2Controller::SocketClosed(IPV4SocketAddress peer) {
  OLA_INFO << "Connection to " << peer << " was closed";

  auto_ptr<DeviceState> device(ola::STLLookupAndRemovePtr(&m_device_map, peer));

  if (!device.get()) {
    OLA_WARN << "Can't find device entry";
    return;
  }

  m_ss.RemoveReadDescriptor(device->socket.get());

  RemoveDevicesForController(true, peer);
}

void Gen2Controller::ControllerMessage(
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
    case ola::acn::VECTOR_CONTROLLER_DEVICE_REG:
      RegisterDevice(transport_header,
                     reinterpret_cast<const uint8_t*>(raw_data.data()),
                     raw_data.size());
      break;
    default:
      break;
  }
}

void Gen2Controller::SendDeviceList(
    const ola::plugin::e131::TransportHeader *transport_header,
    unsigned int size) {
  if (size != 0) {
    OLA_WARN << "FetchDeviceList message of incorrect size " << size;
    return;
  }

  DeviceState *state = STLFindOrNull(m_device_map, transport_header->Source());
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
    if (!iter->second.local) {
      continue;
    }

    DeviceEntry entry;
    entry.ip = iter->second.udp_dest.Host().AsInt();
    entry.port = ola::network::HostToNetwork(iter->second.udp_dest.Port());
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

void Gen2Controller::LearnDevice(
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

  AddDevice(remote_device, transport_header->Source(), uid);
}

void Gen2Controller::ForgetDevice(
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

  if (iter->second.tcp_socket != transport_header->Source()) {
    OLA_WARN << "Release for " << uid << ", owner mismatch "
             << iter->second.tcp_socket << " != " << transport_header->Source();
  }

  m_uid_map.erase(iter);
}

void Gen2Controller::RegisterDevice(
    const ola::plugin::e131::TransportHeader *transport_header,
    const uint8_t *data, unsigned int size) {
  // Assumes transport_header.Transport() is TCP
  struct DeviceRegistrationMessage {
    uint32_t ip;
    uint16_t port;
    uint8_t uid[ola::rdm::UID::LENGTH];
  } __attribute__((packed));

  DeviceRegistrationMessage message;
  if (size != sizeof(message)) {
    OLA_WARN << "DeviceRegistrationMessage of incorrect size "
             << size << " != " << sizeof(message);
    return;
  }

  memcpy(reinterpret_cast<uint8_t*>(&message), data, sizeof(message));
  IPV4SocketAddress remote_device(IPV4Address(message.ip),
                                  ola::network::NetworkToHost(message.port));
  OLA_INFO << "Found device at " << remote_device;
  ola::rdm::UID uid(message.uid);

  RemoteDevice device;
  device.tcp_socket = transport_header->Source();
  device.udp_dest = remote_device;
  device.local = true;
  ola::STLReplace(&m_uid_map, uid, device);
  m_controller_mesh->InformControllersOfAcquiredDevice(uid, remote_device);
}

void Gen2Controller::AddDevice(
    const IPV4SocketAddress &device_address,
    const IPV4SocketAddress &learnt_via,
    const UID &uid) {

  RemoteDevice device;
  device.tcp_socket = learnt_via,
  device.udp_dest = device_address;
  device.local = false;
  ola::STLReplace(&m_uid_map, uid, device);
}

void Gen2Controller::RemoveDevicesForController(
    bool is_local,
    const ola::network::IPV4SocketAddress &controller_address) {
  UIDMap::iterator iter = m_uid_map.begin();
  while (iter != m_uid_map.end()) {
    if (controller_address == iter->second.tcp_socket) {
      OLA_INFO << "Removed UID " << iter->first;
      if (is_local) {
        m_controller_mesh->InformControllersOfReleasedDevice(iter->first);
      }
      m_uid_map.erase(iter++);
    } else {
      ++iter;
    }
  }
}

class Gen2Controller *controller = NULL;

/**
 * Interupt handler
 */
static void InteruptSignal(int unused) {
  if (controller)
    controller->Stop();
  (void) unused;
}

int main(int argc, char *argv[]) {
  ola::SetHelpString("[options]", "Simple E1.33 Controller.");
  ola::ParseFlags(&argc, argv);
  ola::InitLoggingFromFlags();

  // Convert the controller's IP address
  IPV4Address controller_ip;
  if (!FLAGS_listen_ip.str().empty() &&
      !IPV4Address::FromString(FLAGS_listen_ip, &controller_ip)) {
    ola::DisplayUsage();
    exit(ola::EXIT_USAGE);
  }

  ola::InstallSignal(SIGINT, InteruptSignal);
  controller = new Gen2Controller(
      Gen2Controller::Options(
          IPV4SocketAddress(controller_ip, FLAGS_listen_port)));
  controller->Start();
  delete controller;
  controller = NULL;
}
