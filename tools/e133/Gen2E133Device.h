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

#include <memory>
#include <vector>

#include "tools/e133/ControllerAgent.h"
#include "tools/e133/E133DiscoveryAgent.h"
#include "tools/e133/E133HealthCheckedConnection.h"
#include "tools/e133/TCPConnectionStats.h"

/**
 * A very simple E1.33 Device that uses the reverse-connection model.
 */
class Gen2Device {
 public:
  struct Options {
    // If provided, this overides DNS-SD and specifes controller to connect to.
    ola::network::IPV4SocketAddress controller;
  };

  explicit Gen2Device(const Options &options);
  ~Gen2Device();

  void Run();
  void Stop() { m_ss.Terminate(); }

 private:
  const ola::network::IPV4SocketAddress m_static_controller;

  ola::io::SelectServer m_ss;
  ola::e133::MessageBuilder m_message_builder;
  TCPConnectionStats m_tcp_stats;
  ControllerAgent m_controller_agent;

  std::auto_ptr<E133DiscoveryAgentInterface> m_discovery_agent;

  void ConnectToController();

  void ControllerList(
      std::vector<ControllerAgent::E133ControllerInfo> *controllers);

  DISALLOW_COPY_AND_ASSIGN(Gen2Device);
};

#endif  // TOOLS_E133_GEN2E133DEVICE_H_
