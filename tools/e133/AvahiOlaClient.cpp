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

#include "tools/e133/AvahiOlaClient.h"

#include <avahi-common/error.h>

#include <ola/Callback.h>
#include <ola/Logging.h>
#include <ola/stl/STLUtils.h>
#include <ola/io/Descriptor.h>
#include <stdint.h>

#include <string>
#include <utility>

#include "tools/e133/AvahiHelper.h"
#include "tools/e133/AvahiOlaPoll.h"

using ola::NewCallback;
using ola::NewSingleCallback;
using ola::TimeInterval;
using std::string;

// static callback functions
// ----------------------------------------------------------------------------

namespace {

/*
 * Called when the client state changes. This is called once from
 * the thread that calls avahi_client_new, and then from the poll thread.
 */
static void client_callback(AvahiClient *client,
                            AvahiClientState state,
                            void *data) {
  AvahiOlaClient *ola_client = reinterpret_cast<AvahiOlaClient*>(data);
  ola_client->ClientStateChanged(state, client);
}

static void reconnect_callback(AvahiTimeout*, void *data) {
  AvahiOlaClient *client = reinterpret_cast<AvahiOlaClient*>(data);
  client->ReconnectTimeout();
}

}  // namespace


// AvahiOlaClient
// ----------------------------------------------------------------------------


AvahiOlaClient::AvahiOlaClient(AvahiOlaPoll *poller)
    : m_poller(poller),
      m_client(NULL),
      m_state(AVAHI_CLIENT_CONNECTING),
      m_reconnect_timeout(NULL),
      m_backoff(new ola::ExponentialBackoffPolicy(TimeInterval(1, 0),
                                                  TimeInterval(60, 0))) {
}

AvahiOlaClient::~AvahiOlaClient() {
  Stop();
  m_state = AVAHI_CLIENT_CONNECTING;
}

void AvahiOlaClient::Start() {
  CreateNewClient();
}

void AvahiOlaClient::Stop() {
  if (m_client) {
    avahi_client_free(m_client);
    m_client = NULL;
  }
}

void AvahiOlaClient::AddStateChangeListener(
    ClientStateChangeListener *listener) {
  m_state_change_listeners.insert(listener);
}

void AvahiOlaClient::RemoveStateChangeListener(
    ClientStateChangeListener *listener) {
  ola::STLRemove(&m_state_change_listeners, listener);
}

AvahiEntryGroup* AvahiOlaClient::CreateEntryGroup(
    AvahiEntryGroupCallback callback, void *userdata) {
  return m_client ? avahi_entry_group_new(m_client, callback, userdata) : NULL;
}

AvahiServiceBrowser* AvahiOlaClient::CreateServiceBrowser(
    AvahiIfIndex interface,
    AvahiProtocol protocol,
    const string &type,
    const char *domain,
    AvahiLookupFlags flags,
    AvahiServiceBrowserCallback callback,
    void *userdata) {
  if (!m_client) {
    return NULL;
  }

  return avahi_service_browser_new(m_client, interface, protocol, type.c_str(),
                                   domain, flags, callback, userdata);
}

string AvahiOlaClient::GetLastError() {
  if (m_client) {
    return avahi_strerror(avahi_client_errno(m_client));
  } else {
    return "Client not connected";
  }
}

void AvahiOlaClient::ClientStateChanged(AvahiClientState state,
                                        AvahiClient *client) {
  // The first time this is called is from the avahi_client_new context. In
  // that case m_client is still null so we set it here.
  if (!m_client) {
    m_client = client;
  }

  if (m_state == state) {
    return;
  }

  m_state = state;
  OLA_INFO << "Avahi client state changed to " << ClientStateToString(state);

  StateChangeListeners::iterator iter = m_state_change_listeners.begin();
  for (; iter != m_state_change_listeners.end(); ++iter) {
    (*iter)->ClientStateChanged(state);
  }

  switch (state) {
    case AVAHI_CLIENT_FAILURE:
      SetUpReconnectTimeout();
      break;
    default:
      {}
  }
}

void AvahiOlaClient::ReconnectTimeout() {
  if (m_client) {
    avahi_client_free(m_client);
    m_client = NULL;
  }
  CreateNewClient();
}

void AvahiOlaClient::CreateNewClient() {
  if (m_client) {
    OLA_WARN << "CreateNewClient called but m_client is not NULL";
    return;
  }

  if (m_poller) {
    int error;
    // In the successful case, m_client is set in the ClientStateChanged method
    avahi_client_new(m_poller->GetPoll(),
                     AVAHI_CLIENT_NO_FAIL, client_callback, this,
                     &error);
    if (m_client) {
      m_backoff.Reset();
    } else {
      OLA_WARN << "Failed to create Avahi client " << avahi_strerror(error);
      SetUpReconnectTimeout();
    }
  }
}

void AvahiOlaClient::SetUpReconnectTimeout() {
  // We don't strictly need an ExponentialBackoffPolicy here because the client
  // goes into the AVAHI_CLIENT_CONNECTING state if the server isn't running.
  // Still, it's a useful defense against spinning rapidly if something goes
  // wrong.
  TimeInterval delay = m_backoff.Next();
  OLA_INFO << "Re-creating avahi client in " << delay << "s";
  struct timeval tv;
  delay.AsTimeval(&tv);

  const AvahiPoll *poll = m_poller->GetPoll();
  if (m_reconnect_timeout) {
    poll->timeout_update(m_reconnect_timeout, &tv);
  } else {
    m_reconnect_timeout = poll->timeout_new(
        poll, &tv, reconnect_callback, this);
  }
}
