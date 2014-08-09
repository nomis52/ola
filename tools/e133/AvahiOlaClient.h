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
 * AvahiOlaClient.h
 * Wraps the AvahiClient struct in a C++ object.
 * Copyright (C) 2014 Simon Newton
 */

#ifndef TOOLS_E133_AVAHIOLACLIENT_H_
#define TOOLS_E133_AVAHIOLACLIENT_H_

#include <avahi-client/client.h>
#include <avahi-client/lookup.h>
#include <avahi-client/publish.h>

#include <ola/base/Macro.h>
#include <ola/util/Backoff.h>

#include <string>
#include <set>

#include "tools/e133/AvahiOlaPoll.h"

/**
 * @brief A listener that is notified when the AvahiClient state changes.
 */
class ClientStateChangeListener {
 public:
  virtual ~ClientStateChangeListener() {}

  /**
   * @brief Called when the state changes.
   */
  virtual void ClientStateChanged(AvahiClientState state) = 0;
};

/**
 * @brief An object wrapper around AvahiClient.
 */
class AvahiOlaClient {
 public:
  explicit AvahiOlaClient(AvahiOlaPoll *poller);
  ~AvahiOlaClient();

  /**
   * @brief Start the client.
   */
  void Start();

  /**
   * @brief Stop the client.
   */
  void Stop();

  /**
   * @brief Return the connection state of the client.
   */
  AvahiClientState GetState() const { return m_state; }

  /**
   * @brief Add a ClientStateChangeListener to be called when the state changes
   */
  void AddStateChangeListener(ClientStateChangeListener *listener);

  /**
   * @brief Remove a previously added ClientStateChangeListener.
   */
  void RemoveStateChangeListener(ClientStateChangeListener *listener);

  /**
   * @brief Create a new AvahiEntryGroup from this client.
   */
  AvahiEntryGroup* CreateEntryGroup(AvahiEntryGroupCallback callback,
                                    void *userdata);

  /**
   * @brief Create a new AvahiServiceBrowser from this client.
   */
  AvahiServiceBrowser* CreateServiceBrowser(
      AvahiIfIndex interface,
      AvahiProtocol protocol,
      const std::string &type,
      const char *domain,
      AvahiLookupFlags flags,
      AvahiServiceBrowserCallback callback,
      void *userdata);

  /**
   * @brief Create a new AvahiServiceResolver from this client.
   */
  AvahiServiceResolver* CreateServiceResolver(
      AvahiIfIndex interface,
      AvahiProtocol protocol,
      const std::string &name,
      const std::string &type,
      const std::string &domain,
      AvahiProtocol aprotocol,
      AvahiLookupFlags flags,
      AvahiServiceResolverCallback callback,
      void *userdata);

  /**
   * @brief Return the last error as a string.
   */
  std::string GetLastError();

  /**
   * @brief Called by the avahi callbacks when the client state changes.
   */
  void ClientStateChanged(AvahiClientState state,
                          AvahiClient *client);

  void ReconnectTimeout();

 private:
  typedef std::set<ClientStateChangeListener*> StateChangeListeners;

  AvahiOlaPoll *m_poller;
  AvahiClient *m_client;
  AvahiClientState m_state;
  AvahiTimeout *m_reconnect_timeout;
  ola::BackoffGenerator m_backoff;

  StateChangeListeners m_state_change_listeners;

  void CreateNewClient();
  void SetUpReconnectTimeout();

  DISALLOW_COPY_AND_ASSIGN(AvahiOlaClient);
};
#endif  // TOOLS_E133_AVAHIOLACLIENT_H_
