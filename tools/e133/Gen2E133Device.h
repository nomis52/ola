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
 * Gen2E133Device.h
 * Copyright (C) 2014 Simon Newton
 *
 * A Generation II device which opens a TCP connection to a controller.
 */

#ifndef TOOLS_E133_GEN2E133DEVICE_H_
#define TOOLS_E133_GEN2E133DEVICE_H_

#include <ola/e133/MessageBuilder.h>
#include <ola/io/SelectServer.h>
#include <ola/network/Socket.h>
#include <ola/rdm/UID.h>

#include <memory>
#include <string>
#include <vector>

#include "plugins/e131/e131/UDPTransport.h"
#include "plugins/e131/e131/E133Inflator.h"
#include "plugins/e131/e131/RDMInflator.h"
#include "plugins/e131/e131/RootInflator.h"
#include "tools/e133/ControllerAgent.h"
#include "tools/e133/E133DiscoveryAgent.h"
#include "tools/e133/E133HealthCheckedConnection.h"
#include "tools/e133/EndpointManager.h"
#include "tools/e133/ManagementEndpoint.h"
#include "tools/e133/TCPConnectionStats.h"

/**
 * A very simple E1.33 Device that uses the reverse-connection model.
 */
class Gen2Device {
 public:
  struct Options {
    // If provided, this overides DNS-SD and specifes controller to connect to.
    ola::network::IPV4SocketAddress controller;
    ola::rdm::UID uid;
    uint16_t port;

    explicit Options(const ola::rdm::UID &uid) : uid(uid), port(0) {}
  };

  explicit Gen2Device(const Options &options);
  ~Gen2Device();

  bool Run();
  void Stop() { m_ss.Terminate(); }

  // Ownership not passed.
  void AddEndpoint(uint16_t endpoint_id, E133Endpoint *endpoint);
  void RemoveEndpoint(uint16_t endpoint_id);

 private:
  const Options m_options;

  ola::io::SelectServer m_ss;
  ola::e133::MessageBuilder m_message_builder;
  TCPConnectionStats m_tcp_stats;
  EndpointManager m_endpoint_manager;
  ManagementEndpoint m_management_endpoint;

  // Network members
  ola::network::UDPSocket m_udp_socket;
  ola::plugin::e131::IncomingUDPTransport m_incoming_udp_transport;

  // inflators
  ola::plugin::e131::RootInflator m_root_inflator;
  ola::plugin::e131::E133Inflator m_e133_inflator;
  ola::plugin::e131::RDMInflator m_rdm_inflator;

  // Discovery & Controller connections
  std::auto_ptr<E133DiscoveryAgentInterface> m_discovery_agent;
  ControllerAgent m_controller_agent;

  void ConnectToController();

  void ControllerList(ControllerEntryList *controllers);

  void EndpointRequest(
      const ola::plugin::e131::TransportHeader *transport_header,
      const ola::plugin::e131::E133Header *e133_header,
      const string &raw_request);

  void EndpointRequestComplete(ola::network::IPV4SocketAddress target,
                               uint32_t sequence_number,
                               uint16_t endpoint_id,
                               ola::rdm::rdm_response_code response_code,
                               const ola::rdm::RDMResponse *response,
                               const std::vector<string> &packets);

  void SendStatusMessage(const ola::network::IPV4SocketAddress target,
                         uint32_t sequence_number,
                         uint16_t endpoint_id,
                         ola::e133::E133StatusCode status_code,
                         const string &description);

  DISALLOW_COPY_AND_ASSIGN(Gen2Device);
};

#endif  // TOOLS_E133_GEN2E133DEVICE_H_
