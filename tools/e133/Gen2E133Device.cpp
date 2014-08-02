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
 * Gen2E133Device.cpp
 * Copyright (C) 2014 Simon Newton
 *
 * A Generation II device which opens a TCP connection to a controller.
 * I'm using this for scale testing.
 */

#include "tools/e133/Gen2E133Device.h"

#include <ola/BaseTypes.h>
#include <ola/Callback.h>
#include <ola/Clock.h>
#include <ola/Logging.h>
#include <ola/acn/CID.h>
#include <ola/base/Flags.h>
#include <ola/e133/MessageBuilder.h>
#include <ola/io/SelectServer.h>
#include <ola/network/AdvancedTCPConnector.h>
#include <ola/network/TCPSocketFactory.h>
#include <ola/network/InterfacePicker.h>
#include <ola/rdm/RDMCommandSerializer.h>
#include <ola/rdm/RDMHelper.h>

#include <memory>
#include <string>
#include <vector>

#include "plugins/e131/e131/RDMPDU.h"
#include "plugins/e131/e131/TCPTransport.h"
#include "tools/e133/ControllerAgent.h"
#include "tools/e133/E133DiscoveryAgent.h"
#include "tools/e133/E133HealthCheckedConnection.h"
#include "tools/e133/MessageQueue.h"
#include "tools/e133/TCPConnectionStats.h"

DEFINE_uint16(discovery_startup_delay, 2000,
              "The time in ms to let DNS-SD run before selecting a controller");
DEFINE_uint16(terminate_after, 0, "The number of ms to wait before exiting");

using ola::NewCallback;
using ola::NewSingleCallback;
using ola::TimeInterval;
using ola::io::IOStack;
using ola::network::IPV4Address;
using ola::network::IPV4SocketAddress;
using ola::network::TCPSocket;
using ola::plugin::e131::IncomingTCPTransport;
using ola::plugin::e131::RDMPDU;
using std::auto_ptr;
using std::string;
using std::vector;

Gen2Device::Gen2Device(const Options &options)
    : m_options(options),
      m_message_builder(ola::acn::CID::Generate(), "E1.33 Device"),
      m_management_endpoint(NULL, E133Endpoint::EndpointProperties(),
                            options.uid, &m_endpoint_manager,
                            &m_tcp_stats),
      m_incoming_udp_transport(&m_udp_socket, &m_root_inflator),
      m_controller_agent(NewCallback(this, &Gen2Device::ControllerList),
                         &m_ss,
                         &m_message_builder,
                         &m_tcp_stats,
                         options.uid) {
  m_root_inflator.AddInflator(&m_e133_inflator);
  m_e133_inflator.AddInflator(&m_rdm_inflator);

  m_rdm_inflator.SetRDMHandler(
      NewCallback(this, &Gen2Device::EndpointRequest));

  if (m_options.controller.Host().IsWildcard()) {
    E133DiscoveryAgentFactory discovery_agent_factory;
    m_discovery_agent.reset(discovery_agent_factory.New());
  }
}

Gen2Device::~Gen2Device() {
  if (m_discovery_agent.get()) {
    m_discovery_agent->Stop();
  }
}

bool Gen2Device::Run() {
  if (!m_discovery_agent->Start()) {
    return false;
  }

  // Setup the UDP socket
  if (!m_udp_socket.Init()) {
    return false;
  }

  if (!m_udp_socket.Bind(IPV4SocketAddress(IPV4Address::WildCard(),
          m_options.port))) {
    return false;
  }


  IPV4SocketAddress our_addr;
  if (!m_udp_socket.GetSocketAddress(&our_addr)) {
    OLA_INFO << "Failed to get local address";
    return false;
  }

  OLA_INFO << "E1.33 device listening at " << our_addr << ", UID "
           << m_options.uid;

  m_udp_socket.SetOnData(
        NewCallback(&m_incoming_udp_transport,
                    &ola::plugin::e131::IncomingUDPTransport::Receive));

  m_ss.AddReadDescriptor(&m_udp_socket);

  // The socket is bound to 0.0.0.0 so we need to 'guess' the local ip here.
  auto_ptr<ola::network::InterfacePicker> picker(
      ola::network::InterfacePicker::NewPicker());
  ola::network::Interface iface;
  if (!picker->ChooseInterface(&iface, "")) {
    OLA_WARN << "Failed to lookup local ip";
  }

  m_controller_agent.SetLocalSocketAddress(
      IPV4SocketAddress(iface.ip_address, our_addr.Port()));

  // Now figure out which controller we're going to connect to.
  if (m_options.controller.Host().IsWildcard()) {
    m_ss.RegisterSingleTimeout(
        FLAGS_discovery_startup_delay,
        NewSingleCallback(this, &Gen2Device::ConnectToController));
  } else {
    // If it was static we can connect immediately
    ConnectToController();
  }

  if (FLAGS_terminate_after) {
    m_ss.RegisterSingleTimeout(
        FLAGS_terminate_after,
        NewSingleCallback(&m_ss, &ola::io::SelectServer::Terminate));
  }
  m_ss.Run();

  // Clean up
  m_ss.RemoveReadDescriptor(&m_udp_socket);
  return true;
}

void Gen2Device::AddEndpoint(uint16_t endpoint_id,
                                 E133Endpoint *endpoint) {
  m_endpoint_manager.RegisterEndpoint(endpoint_id, endpoint);
}

void Gen2Device::RemoveEndpoint(uint16_t endpoint_id) {
  m_endpoint_manager.UnRegisterEndpoint(endpoint_id);
}

/**
 * Start the ControllerAgent which will attempt to connect to a controller
 */
void Gen2Device::ConnectToController() {
  m_controller_agent.Start();
}

/**
 * Get the list if controllers, either from the options passed to the
 * constructor or from the E133DiscoveryAgent.
 */
void Gen2Device::ControllerList(ControllerEntryList *controllers) {
  if (m_options.controller.Host().IsWildcard()) {
    m_discovery_agent->FindControllers(controllers);
  } else {
    E133ControllerEntry controller_entry;
    controller_entry.address = m_options.controller;
    controller_entry.priority = 100;
    controllers->push_back(controller_entry);
  }
}

/**
 * Handle requests to an endpoint.
 */
void Gen2Device::EndpointRequest(
    const ola::plugin::e131::TransportHeader *transport_header,
    const ola::plugin::e131::E133Header *e133_header,
    const std::string &raw_request) {
  IPV4SocketAddress target = transport_header->Source();
  uint16_t endpoint_id = e133_header->Endpoint();
  OLA_INFO << "Got request for to endpoint " << endpoint_id
           << " from " << target;

  E133EndpointInterface *endpoint = NULL;
  if (endpoint_id)
    endpoint = m_endpoint_manager.GetEndpoint(endpoint_id);
  else
    endpoint = &m_management_endpoint;

  if (!endpoint) {
    OLA_INFO << "Request to non-existent endpoint " << endpoint_id;
    SendStatusMessage(target, e133_header->Sequence(), endpoint_id,
                      ola::e133::SC_E133_NONEXISTANT_ENDPOINT,
                      "No such endpoint");
    return;
  }

  // attempt to unpack as a request
  const ola::rdm::RDMRequest *request = ola::rdm::RDMRequest::InflateFromData(
    reinterpret_cast<const uint8_t*>(raw_request.data()),
    raw_request.size());

  if (!request) {
    OLA_WARN << "Failed to unpack E1.33 RDM message, ignoring request.";
    // There is no way to return 'invalid request' so pretend this is a timeout
    // but give a descriptive error msg.
    SendStatusMessage(target, e133_header->Sequence(), endpoint_id,
                      ola::e133::SC_E133_RDM_TIMEOUT,
                     "Invalid RDM request");
    return;
  }

  endpoint->SendRDMRequest(
      request,
      ola::NewSingleCallback(this,
                             &Gen2Device::EndpointRequestComplete,
                             target,
                             e133_header->Sequence(),
                             endpoint_id));
}


/**
 * Handle a completed RDM request.
 */
void Gen2Device::EndpointRequestComplete(
    ola::network::IPV4SocketAddress target,
    uint32_t sequence_number,
    uint16_t endpoint_id,
    ola::rdm::rdm_response_code response_code,
    const ola::rdm::RDMResponse *response_ptr,
    const std::vector<std::string>&) {
  auto_ptr<const ola::rdm::RDMResponse> response(response_ptr);

  if (response_code != ola::rdm::RDM_COMPLETED_OK) {
    ola::e133::E133StatusCode status_code =
      ola::e133::SC_E133_RDM_INVALID_RESPONSE;
    string description = ola::rdm::ResponseCodeToString(response_code);
    switch (response_code) {
      case ola::rdm::RDM_COMPLETED_OK:
        break;
      case ola::rdm::RDM_WAS_BROADCAST:
        status_code = ola::e133::SC_E133_BROADCAST_COMPLETE;
        break;
      case ola::rdm::RDM_FAILED_TO_SEND:
      case ola::rdm::RDM_TIMEOUT:
        status_code = ola::e133::SC_E133_RDM_TIMEOUT;
        break;
      case ola::rdm::RDM_UNKNOWN_UID:
        status_code = ola::e133::SC_E133_UNKNOWN_UID;
        break;
      case ola::rdm::RDM_INVALID_RESPONSE:
      case ola::rdm::RDM_CHECKSUM_INCORRECT:
      case ola::rdm::RDM_TRANSACTION_MISMATCH:
      case ola::rdm::RDM_SUB_DEVICE_MISMATCH:
      case ola::rdm::RDM_SRC_UID_MISMATCH:
      case ola::rdm::RDM_DEST_UID_MISMATCH:
      case ola::rdm::RDM_WRONG_SUB_START_CODE:
      case ola::rdm::RDM_PACKET_TOO_SHORT:
      case ola::rdm::RDM_PACKET_LENGTH_MISMATCH:
      case ola::rdm::RDM_PARAM_LENGTH_MISMATCH:
      case ola::rdm::RDM_INVALID_COMMAND_CLASS:
      case ola::rdm::RDM_COMMAND_CLASS_MISMATCH:
      case ola::rdm::RDM_INVALID_RESPONSE_TYPE:
      case ola::rdm::RDM_PLUGIN_DISCOVERY_NOT_SUPPORTED:
      case ola::rdm::RDM_DUB_RESPONSE:
        status_code = ola::e133::SC_E133_RDM_INVALID_RESPONSE;
        break;
    }
    SendStatusMessage(target, sequence_number, endpoint_id,
                      status_code, description);
    return;
  }

  IOStack packet(m_message_builder.pool());
  ola::rdm::RDMCommandSerializer::Write(*response.get(), &packet);
  RDMPDU::PrependPDU(&packet);
  m_message_builder.BuildUDPRootE133(
      &packet, ola::acn::VECTOR_FRAMING_RDMNET, sequence_number,
      endpoint_id);

  if (!m_udp_socket.SendTo(&packet, target)) {
    OLA_WARN << "Failed to send E1.33 response to " << target;
  }
}

void Gen2Device::SendStatusMessage(
    const ola::network::IPV4SocketAddress target,
    uint32_t sequence_number,
    uint16_t endpoint_id,
    ola::e133::E133StatusCode status_code,
    const string &description) {
  IOStack packet(m_message_builder.pool());
  m_message_builder.BuildUDPE133StatusPDU(
      &packet, sequence_number, endpoint_id,
      status_code, description);
  if (!m_udp_socket.SendTo(&packet, target)) {
    OLA_WARN << "Failed to send E1.33 response to " << target;
  }
}
