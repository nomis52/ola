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
 * ControllerAgent.h
 * Manages the connection between a E1.33 device and an E1.33 Controller.
 * Copyright (C) 2014 Simon Newton
 */

#ifndef TOOLS_E133_CONTROLLERAGENT_H_
#define TOOLS_E133_CONTROLLERAGENT_H_

#include <stdint.h>

#include <map>
#include <string>
#include <vector>

#include "ola/Callback.h"
#include "ola/base/Macro.h"
#include "ola/e133/MessageBuilder.h"
#include "ola/io/SelectServerInterface.h"
#include "ola/network/TCPConnector.h"
#include "ola/network/TCPSocket.h"
#include "ola/rdm/RDMCommand.h"
#include "plugins/e131/e131/E133Inflator.h"
#include "plugins/e131/e131/E133StatusInflator.h"
#include "plugins/e131/e131/RootInflator.h"
#include "tools/e133/E133HealthCheckedConnection.h"
#include "tools/e133/MessageQueue.h"
#include "tools/e133/TCPConnectionStats.h"


#include "ola/network/Socket.h"
#include "ola/network/SocketAddress.h"
#include "ola/util/SequenceNumber.h"
#include "plugins/e131/e131/TCPTransport.h"


/**
 * @brief Manages the connection between a E1.33 device and an E1.33 Controller.
 *
 * The controller(s) are located by the refesh_controllers callback. This is
 * usually called when a connection fails, or when we're trying to locate a
 * controller.
 *
 * This does not handle the controller to controller communication.
 */
class ControllerAgent {
 public:
  struct E133ControllerInfo {
    ola::network::IPV4SocketAddress address;
    uint8_t priority;
  };

  /**
   * @brief The callback run as a result of FindControllers.
   *
   * The callback populates the first argument with the list of known
   * controllers.
   */
  typedef ola::Callback1<void, std::vector<E133ControllerInfo>*>
      RefreshControllersCallback;

  /**
   * @brief Create a new ControllerAgent.
   * @param refresh_controllers_cb The callback used to refresh the controller
   *   list. Ownership is transferred.
   */
  ControllerAgent(RefreshControllersCallback *refresh_controllers_cb,
                  ola::io::SelectServerInterface *ss,
                  ola::e133::MessageBuilder *message_builder,
                  TCPConnectionStats *tcp_stats,
                  unsigned int max_queue_size = MAX_QUEUE_SIZE);

  /**
   * @Clean up
   */
  ~ControllerAgent();

  /**
   * @brief Start trying to connect to an E1.33 controller.
   */
  bool Start();

  /**
   * @brief Check if we have a TCP connection to a controller
   */
  bool IsConnected();

  /**
   * @brief Send a RDMResponse to the controller
   * @param endpoint The endpoint this RDMResponse originated from.
   * @param response The RDMResponse to send
   * @returns True if the message was accepted for sending. False if there is
   *   insufficient space left in the buffer for this message.
   *
   * If there is no controller connection when this is called, the message us
   * buffered and will be send when a connection becomes available.
   */
  bool SendStatusMessage(
    uint16_t endpoint,
    const ola::rdm::RDMResponse *response);

  /**
   * @brief Close the controller connection, and start the discovery cycle
   * again.
   * @return, true if there was a connection to close, false otherwise.
   */
  bool CloseTCPConnection();

 private:
  struct ControllerInfo {
    ola::network::IPV4SocketAddress address;
    int16_t priority;
    bool seen;  // used to remove old controllers
  };

  typedef std::vector<ControllerInfo> ControllerList;

  RefreshControllersCallback *m_controllers_cb;
  const unsigned int m_max_queue_size;
  ola::io::SelectServerInterface *m_ss;
  ola::e133::MessageBuilder *m_message_builder;
  TCPConnectionStats *m_tcp_stats;

  // Connection members
  ControllerList m_known_controllers;
  ola::thread::timeout_id m_discovery_timeout;
  ola::network::TCPConnector m_tcp_connector;
  ola::network::TCPConnector::TCPConnectionID m_connection_id;

  // TCP connection classes
  ola::network::TCPSocket *m_tcp_socket;
  E133HealthCheckedConnection *m_health_checked_connection;
  MessageQueue *m_message_queue;
  ola::plugin::e131::IncomingTCPTransport *m_incoming_tcp_transport;

  // Inflators
  ola::plugin::e131::RootInflator m_root_inflator;
  ola::plugin::e131::E133Inflator m_e133_inflator;
  ola::plugin::e131::E133StatusInflator m_e133_status_inflator;

  // The message state.
  // Indicates if we have messages that haven't been sent on the MessageQueue
  // yet.
  typedef std::map<unsigned int, class OutstandingMessage*> PendingMessageMap;
  bool m_unsent_messages;
  PendingMessageMap m_unacked_messages;
  ola::SequenceNumber<unsigned int> m_sequence_number;

  void AttemptConnection();
  bool PickController(ola::network::IPV4SocketAddress *controller);
  void ConnectionResult(ola::network::IPV4SocketAddress controller_address,
                        int fd, int error);



  void NewTCPConnection(ola::network::TCPSocket *socket);
  void ReceiveTCPData();
  void TCPConnectionUnhealthy();
  void TCPConnectionClosed();
  void RLPDataReceived(const ola::plugin::e131::TransportHeader &header);

  bool SendRDMCommand(unsigned int sequence_number, uint16_t endpoint,
                      const ola::rdm::RDMResponse *rdm_response);

  void HandleStatusMessage(
      const ola::plugin::e131::TransportHeader *transport_header,
      const ola::plugin::e131::E133Header *e133_header,
      uint16_t status_code,
      const std::string &description);

  static const unsigned int MAX_QUEUE_SIZE;
  static const unsigned int TCP_CONNECT_TIMEOUT_SECONDS;
  static const int16_t CONNECT_FAILURE_PENALTY;

  DISALLOW_COPY_AND_ASSIGN(ControllerAgent);
};
#endif  // TOOLS_E133_CONTROLLERAGENT_H_
