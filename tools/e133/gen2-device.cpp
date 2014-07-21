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
 * gen2-device.cpp
 * Copyright (C) 2014 Simon Newton
 *
 * A Generation II device which opens a TCP connection to a controller.
 * I'm using this for scale testing.
 */

#include <ola/BaseTypes.h>
#include <ola/Callback.h>
#include <ola/Clock.h>
#include <ola/Logging.h>
#include <ola/acn/CID.h>
#include <ola/base/Flags.h>
#include <ola/base/Init.h>
#include <ola/base/SysExits.h>
#include <ola/e133/MessageBuilder.h>
#include <ola/io/SelectServer.h>
#include <ola/network/AdvancedTCPConnector.h>
#include <ola/network/TCPSocketFactory.h>
#include <signal.h>

#include <memory>
#include <string>
#include <vector>

#include "plugins/e131/e131/RootInflator.h"
#include "plugins/e131/e131/TCPTransport.h"
#include "tools/e133/ControllerAgent.h"
#include "tools/e133/E133DiscoveryAgent.h"
#include "tools/e133/E133HealthCheckedConnection.h"
#include "tools/e133/MessageQueue.h"
#include "tools/e133/TCPConnectionStats.h"

DEFINE_string(controller_address, "",
              "The IP:Port of the controller, if set this bypasses discovery");
DEFINE_uint16(discovery_startup_delay, 2000,
              "The time in ms to let DNS-SD run before selecting a controller");

using ola::NewCallback;
using ola::NewSingleCallback;
using ola::TimeInterval;
using ola::network::IPV4Address;
using ola::network::IPV4SocketAddress;
using ola::network::TCPSocket;
using ola::plugin::e131::IncomingTCPTransport;
using std::auto_ptr;
using std::string;
using std::vector;

/**
 * A very simple E1.33 Device that uses the reverse-connection model.
 */
class Gen2Device {
 public:
  struct Options {
    // If provided, this overides DNS-SD and specifes controller to connect to.
    IPV4SocketAddress controller;
  };

  explicit Gen2Device(const Options &options);
  ~Gen2Device();

  void Run();
  void Stop() { m_ss.Terminate(); }

 private:
  const IPV4SocketAddress m_static_controller;

  ola::io::SelectServer m_ss;
  ola::e133::MessageBuilder m_message_builder;
  TCPConnectionStats m_tcp_stats;
  ControllerAgent m_controller_agent;

  auto_ptr<E133DiscoveryAgentInterface> m_discovery_agent;

  void ConnectToController();

  void ControllerList(
      vector<ControllerAgent::E133ControllerInfo> *controllers);

  DISALLOW_COPY_AND_ASSIGN(Gen2Device);
};


Gen2Device::Gen2Device(const Options &options)
    : m_static_controller(options.controller),
      m_message_builder(ola::acn::CID::Generate(), "E1.33 Device"),
      m_controller_agent(NewCallback(this, &Gen2Device::ControllerList),
                         &m_ss,
                         &m_message_builder,
                         &m_tcp_stats) {
  if (m_static_controller.Host().IsWildcard()) {
    E133DiscoveryAgentFactory discovery_agent_factory;
    m_discovery_agent.reset(discovery_agent_factory.New());
    m_discovery_agent->Init();
  }
}

Gen2Device::~Gen2Device() {
  if (m_discovery_agent.get()) {
    m_discovery_agent->Stop();
  }
}

void Gen2Device::Run() {
  if (m_static_controller.Host().IsWildcard()) {
    m_ss.RegisterSingleTimeout(
        FLAGS_discovery_startup_delay,
        NewSingleCallback(this, &Gen2Device::ConnectToController));
  } else {
    ConnectToController();
  }

  m_ss.RegisterSingleTimeout(
      5000,
      NewSingleCallback(&m_ss, &ola::io::SelectServer::Terminate));
  m_ss.Run();
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
void Gen2Device::ControllerList(
      vector<ControllerAgent::E133ControllerInfo> *controllers) {
  if (m_static_controller.Host().IsWildcard()) {
    // TODO(simon): make this struct common somewhere
    vector<E133DiscoveryAgentInterface::E133ControllerInfo> e133_controllers;
    m_discovery_agent->FindControllers(&e133_controllers);

    vector<E133DiscoveryAgentInterface::E133ControllerInfo>::iterator iter =
      e133_controllers.begin();
    for (; iter != e133_controllers.end(); ++iter) {
      ControllerAgent::E133ControllerInfo info;
      info.address = iter->address;
      info.priority = iter->priority;
      controllers->push_back(info);
    }
  } else {
    ControllerAgent::E133ControllerInfo info;
    info.address = m_static_controller;
    info.priority = 100;
    controllers->push_back(info);
  }
}

Gen2Device *device = NULL;

/**
 * Interupt handler
 */
static void InteruptSignal(int unused) {
  if (device)
    device->Stop();
  (void) unused;
}

int main(int argc, char *argv[]) {
  ola::SetHelpString("[options]", "Simple E1.33 Device.");
  ola::ParseFlags(&argc, argv);
  ola::InitLoggingFromFlags();

  Gen2Device::Options options;
  if (!FLAGS_controller_address.str().empty()) {
    if (!IPV4SocketAddress::FromString(FLAGS_controller_address.str(),
                                       &options.controller)) {
      OLA_WARN << "Invalid --controller-address";
      exit(ola::EXIT_USAGE);
    }
  }

  device = new Gen2Device(options);

  ola::InstallSignal(SIGINT, InteruptSignal);
  device->Run();
  delete device;
  device = NULL;
}
