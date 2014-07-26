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
 * ControllerMesh.h
 * Manages the connection between E1.33 controllers.
 * Copyright (C) 2014 Simon Newton
 */

#ifndef TOOLS_E133_CONTROLLERMESH_H_
#define TOOLS_E133_CONTROLLERMESH_H_

#include <stdint.h>

#include <map>
#include <string>
#include <vector>
#include <utility>

#include "ola/Callback.h"
#include "ola/base/Macro.h"
#include "ola/e133/MessageBuilder.h"
#include "ola/io/SelectServerInterface.h"
#include "ola/network/AdvancedTCPConnector.h"
#include "ola/network/TCPSocket.h"
#include "ola/rdm/RDMCommand.h"
#include "plugins/e131/e131/E133Inflator.h"
#include "plugins/e131/e131/E133StatusInflator.h"
#include "plugins/e131/e131/E133ControllerInflator.h"
#include "plugins/e131/e131/RootInflator.h"
#include "tools/e133/E133HealthCheckedConnection.h"
#include "tools/e133/MessageQueue.h"


#include "ola/network/Socket.h"
#include "ola/network/SocketAddress.h"
#include "ola/util/SequenceNumber.h"
#include "plugins/e131/e131/TCPTransport.h"


/**
 * @brief Manages the connection between E1.33 controllers.
 *
 * The controller(s) are located by the refesh_controllers callback. This is
 * usually called when a connection fails, or when periodically as we're trying
 * to locate controllers.
 *
 * This doesn't implement the two-way connection resolution, so for now we have
 * two TCP connections to each controller (one initiated by each end).
 */
class ControllerMesh {
 public:
  /**
   * @brief The callback run as a result of FindControllers.
   *
   * The callback populates the first argument with the list of known
   * controllers.
   */
  typedef ola::Callback1<void, std::vector<ola::network::IPV4SocketAddress>*>
      RefreshControllersCallback;

  /**
   * @brief The callback run as a result of FindControllers.
   *
   * The callback populates the first argument with the list of known
   * controllers.
   */
  typedef ola::Callback3<void,
      const ola::network::IPV4SocketAddress&,
      const ola::network::IPV4SocketAddress&,
      const ola::rdm::UID&> AddDeviceCallback;

  typedef ola::Callback1<void,
      const ola::network::IPV4SocketAddress&> ControllerDisconnectCallback;

  /**
   * @brief Create a new ControllerMesh.
   * @param refresh_controllers_cb The callback used to refresh the controller
   *   list. Ownership is transferred.
   */
  ControllerMesh(RefreshControllersCallback *refresh_controllers_cb,
                 AddDeviceCallback *add_device_callback,
                 ControllerDisconnectCallback *disconnect_cb,
                 ola::io::SelectServerInterface *ss,
                 ola::e133::MessageBuilder *message_builder,
                 uint16_t our_port,
                 unsigned int max_queue_size = MAX_QUEUE_SIZE);

  /**
   * @Clean up
   */
  ~ControllerMesh();

  /**
   * @brief Start trying to connect to all E1.33 controllers.
   */
  bool Start();

  unsigned int ConnectedControllerCount();

  void InformControllerOfDevices(
      const ola::network::IPV4SocketAddress &controller_address,
      const std::vector<std::pair<
        ola::rdm::UID,
        ola::network::IPV4SocketAddress> > &devices);

  void InformControllersOfAcquiredDevice(
      const ola::rdm::UID &uid,
      const ola::network::IPV4SocketAddress &udp_address);

  void InformControllersOfReleasedDevice(
      const ola::rdm::UID &uid);

  void PrintStats();

 private:
  struct ControllerInfo {
    class ControllerConnection *connection;
    bool seen;
  };

  typedef std::vector<ControllerInfo> ControllerList;

  RefreshControllersCallback *m_controllers_cb;
  AddDeviceCallback *m_add_device_callback;
  ControllerDisconnectCallback *m_disconnect_cb;
  const uint16_t m_our_port;
  const unsigned int m_max_queue_size;
  ola::io::SelectServerInterface *m_ss;
  ola::e133::MessageBuilder *m_message_builder;

  // Connection members
  ControllerList m_known_controllers;
  ola::thread::timeout_id m_discovery_timeout;
  ola::network::TCPSocketFactory m_tcp_socket_factory;
  ola::network::AdvancedTCPConnector m_tcp_connector;
  ola::LinearBackoffPolicy m_backoff_policy;

  // Inflators
  ola::plugin::e131::E133Inflator m_e133_inflator;
  ola::plugin::e131::E133StatusInflator m_e133_status_inflator;
  ola::plugin::e131::E133ControllerInflator m_e133_controller_inflator;

  bool CheckForNewControllers();
  void OnTCPConnect(ola::network::TCPSocket *socket);
  void TCPConnectionClosed(const ola::network::IPV4SocketAddress &peer_addr);

  void HandleStatusMessage(
      const ola::plugin::e131::TransportHeader *transport_header,
      const ola::plugin::e131::E133Header *e133_header,
      uint16_t status_code,
      const std::string &description);

  class ControllerConnection *FindControllerConnection(
      const ola::network::IPV4SocketAddress &peer_addr);

  void ControllerMessage(
      const ola::plugin::e131::TransportHeader *transport_header,
      uint16_t vector,
      const std::string &raw_data);

  void DeviceList(
      const ola::network::IPV4SocketAddress &controller_address,
      const uint8_t *data, unsigned int size);

  static const unsigned int MAX_QUEUE_SIZE;
  static const unsigned int TCP_CONNECT_TIMEOUT_SECONDS;
  static const int16_t CONNECT_FAILURE_PENALTY;

  static const ola::TimeInterval INITIAL_TCP_RETRY_DELAY;
  static const ola::TimeInterval MAX_TCP_RETRY_DELAY;

  DISALLOW_COPY_AND_ASSIGN(ControllerMesh);
};
#endif  // TOOLS_E133_CONTROLLERMESH_H_
