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
 * AvahiE133DiscoveryAgent.h
 * The Avahi implementation of DiscoveryAgentInterface.
 * Copyright (C) 2013 Simon Newton
 */

#ifndef TOOLS_E133_AVAHIDISCOVERYAGENT_H_
#define TOOLS_E133_AVAHIDISCOVERYAGENT_H_

#include <avahi-client/client.h>
#include <avahi-common/thread-watch.h>
#include <avahi-client/publish.h>
#include <avahi-client/lookup.h>

#include <ola/base/Macro.h>
#include <ola/io/Descriptor.h>
#include <ola/io/SelectServer.h>
#include <ola/thread/CallbackThread.h>
#include <ola/thread/Mutex.h>
#include <ola/util/Backoff.h>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "tools/e133/E133DiscoveryAgent.h"

/**
 * @brief An implementation of E133DiscoveryAgentInterface that uses the Avahi.
 */
class AvahiE133DiscoveryAgent : public E133DiscoveryAgentInterface {
 public:
  AvahiE133DiscoveryAgent();
  ~AvahiE133DiscoveryAgent();

  bool Init();

  bool Stop();

  bool FindControllers(BrowseCallback *callback);

  void FindControllers(std::vector<E133ControllerInfo> *controllers);

  void RegisterController(
      const ola::network::IPV4SocketAddress &controller_address);

  // Called from the static callback functions.

  /**
   * @brief Called when the Avahi client state changes
   */
  void ClientStateChanged(AvahiClientState state, AvahiClient *client);

  /**
   * @brief Called when the reconnect timeout expires.
   */
  void ReconnectTimeout();

  void BrowseEvent(AvahiIfIndex interface,
                   AvahiProtocol protocol,
                   AvahiBrowserEvent event,
                   const char *name,
                   const char *type,
                   const char *domain,
                   AvahiLookupResultFlags flags);

  static std::string BrowseEventToString(AvahiBrowserEvent state);

 private:
  typedef std::vector<class ControllerResolver*> ControllerResolverList;

  AvahiThreadedPoll *m_threaded_poll;
  AvahiClient *m_client;
  AvahiTimeout *m_reconnect_timeout;
  ola::BackoffGenerator m_backoff;

  AvahiServiceBrowser *m_controller_browser;

  ControllerResolverList m_controllers;
  ola::thread::Mutex m_controllers_mu;

  void CreateNewClient();
  void SetUpReconnectTimeout();

  void LocateControllerServices();
  void AddController(AvahiIfIndex interface,
                     AvahiProtocol protocol,
                     const std::string &name,
                     const std::string &type,
                     const std::string &domain);

  void RemoveController(AvahiIfIndex interface,
                        AvahiProtocol protocol,
                        const std::string &name,
                        const std::string &type,
                        const std::string &domain);

  static std::string ClientStateToString(AvahiClientState state);
  static std::string GroupStateToString(AvahiEntryGroupState state);

  DISALLOW_COPY_AND_ASSIGN(AvahiE133DiscoveryAgent);
};
#endif  // TOOLS_E133_AVAHIDISCOVERYAGENT_H_
