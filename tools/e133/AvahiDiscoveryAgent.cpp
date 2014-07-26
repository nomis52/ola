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
 * AvahiE133DiscoveryAgent.cpp
 * The Avahi implementation of DiscoveryAgentInterface.
 * Copyright (C) 2013 Simon Newton
 */

#define __STDC_LIMIT_MACROS  // for UINT8_MAX & friends

#include "tools/e133/AvahiDiscoveryAgent.h"

#include <avahi-common/error.h>
#include <netinet/in.h>
#include <ola/base/Flags.h>
#include <ola/Callback.h>
#include <ola/Logging.h>
#include <ola/network/NetworkUtils.h>
#include <ola/stl/STLUtils.h>
#include <ola/thread/CallbackThread.h>
#include <stdint.h>

#include <map>
#include <string>
#include <vector>
#include <utility>

using ola::network::HostToNetwork;
using ola::network::NetworkToHost;
using ola::network::IPV4Address;
using ola::network::IPV4SocketAddress;
using ola::io::SelectServerInterface;
using ola::thread::MutexLocker;
using ola::TimeInterval;
using std::auto_ptr;
using std::string;
using std::vector;
using std::ostringstream;

DECLARE_uint8(controller_priority);


// ControllerResolver
// ----------------------------------------------------------------------------
/*
class ControllerResolver {
 public:
  ControllerResolver(
      class IOAdapter *io_adapter,
      uint32_t interface_index,
      const std::string &service_name,
      const std::string &regtype,
      const std::string &reply_domain);
  ControllerResolver(const ControllerResolver &other);
  ~ControllerResolver();

  bool operator==(const ControllerResolver &other) const;

  std::string ToString() const;

  friend std::ostream& operator<<(std::ostream &out,
                                  const ControllerResolver &info) {
    return out << info.ToString();
  }

  DNSServiceErrorType StartResolution();

  bool GetControllerResolver(
      E133DiscoveryAgentInterface::E133ControllerInfo *info);

  void ResolveHandler(
      DNSServiceErrorType errorCode,
      const string &host_target,
      uint16_t port,
      uint16_t txtLen,
      const unsigned char *txtRecord);

  void UpdateAddress(const IPV4Address &v4_address);

 private:
  class IOAdapter *m_io_adapter;
  bool resolve_in_progress;
  DNSServiceRef m_resolve_ref;

  bool to_addr_in_progress;
  DNSServiceRef m_to_addr_ref;

  uint32_t interface_index;
  const std::string service_name;
  const std::string regtype;
  const std::string reply_domain;
  std::string m_host_target;

  uint8_t m_priority;
  ola::network::IPV4SocketAddress m_resolved_address;

  static const uint8_t DEFAULT_PRIORITY;
  static const char PRIORITY_KEY[];
};

const uint8_t ControllerResolver::DEFAULT_PRIORITY = 100;
const char ControllerResolver::PRIORITY_KEY[] = "priority";
*/

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
  AvahiE133DiscoveryAgent *agent =
      reinterpret_cast<AvahiE133DiscoveryAgent*>(data);
  agent->ClientStateChanged(state, client);
}

static void reconnect_callback(AvahiTimeout*, void *data) {
  AvahiE133DiscoveryAgent *agent =
      reinterpret_cast<AvahiE133DiscoveryAgent*>(data);
  agent->ReconnectTimeout();
}

/*
static void browse_callback(
    AvahiServiceBrowser *b,
    AvahiIfIndex interface,
    AvahiProtocol protocol,
    AvahiBrowserEvent event,
    const char *name,
    const char *type,
    const char *domain,
    AvahiLookupResultFlags flags,
    void* data) {

  AvahiE133DiscoveryAgent *agent =
      reinterpret_cast<AvahiE133DiscoveryAgent*>(data);
  (void) agent;

  (void) b;
  (void) interface;
  (void) protocol;
  (void) event;
  (void) name;
  (void) type;
  (void) domain;
  (void) flags;

   Called whenever a new services becomes available on the LAN or is removed from the LAN 

  switch (event) {
    case AVAHI_BROWSER_FAILURE:
      fprintf(stderr, "(Browser) %s\n", avahi_strerror(avahi_client_errno(avahi_service_browser_get_client(b))));
      avahi_simple_poll_quit(simple_poll);
      return;

    case AVAHI_BROWSER_NEW:
      fprintf(stderr, "(Browser) NEW: service '%s' of type '%s' in domain '%s'\n", name, type, domain);

       We ignore the returned resolver object. In the callback
         function we free it. If the server is terminated before
         the callback function is called the server will free
         the resolver for us. 

      if (!(avahi_service_resolver_new(c, interface, protocol, name, type, domain, AVAHI_PROTO_UNSPEC, 0, resolve_callback, c)))
          fprintf(stderr, "Failed to resolve service '%s': %s\n", name, avahi_strerror(avahi_client_errno(c)));

      break;

    case AVAHI_BROWSER_REMOVE:
      fprintf(stderr, "(Browser) REMOVE: service '%s' of type '%s' in domain '%s'\n", name, type, domain);
      break;

    case AVAHI_BROWSER_ALL_FOR_NOW:
    case AVAHI_BROWSER_CACHE_EXHAUSTED:
      fprintf(stderr, "(Browser) %s\n", event == AVAHI_BROWSER_CACHE_EXHAUSTED ? "CACHE_EXHAUSTED" : "ALL_FOR_NOW");
      break;
  }
}
  */

}  // namespace

// ControllerResolver
// ----------------------------------------------------------------------------
/*
ControllerResolver::ControllerResolver(
    IOAdapter *io_adapter,
    uint32_t interface_index,
    const string &service_name,
    const string &regtype,
    const string &reply_domain)
    : m_io_adapter(io_adapter),
      resolve_in_progress(false),
      to_addr_in_progress(false),
      interface_index(interface_index),
      service_name(service_name),
      regtype(regtype),
      reply_domain(reply_domain) {
}

ControllerResolver::ControllerResolver(
    const ControllerResolver &other)
    : m_io_adapter(other.m_io_adapter),
      resolve_in_progress(false),
      to_addr_in_progress(false),
      interface_index(other.interface_index),
      service_name(other.service_name),
      regtype(other.regtype),
      reply_domain(other.reply_domain),
      m_priority(0) {
}

ControllerResolver::~ControllerResolver() {
  if (resolve_in_progress) {
    m_io_adapter->RemoveDescriptor(m_resolve_ref);
    DNSServiceRefDeallocate(m_resolve_ref);
  }

  if (to_addr_in_progress) {
    m_io_adapter->RemoveDescriptor(m_to_addr_ref);
    DNSServiceRefDeallocate(m_to_addr_ref);
  }
}

bool ControllerResolver::operator==(const ControllerResolver &other) const {
  return (interface_index == other.interface_index &&
          service_name == other.service_name &&
          regtype == other.regtype &&
          reply_domain == other.reply_domain);
}

string ControllerResolver::ToString() const {
  std::ostringstream str;
  str << service_name << "." << regtype << reply_domain << " on iface "
      << interface_index;
  return str.str();
}


DNSServiceErrorType ControllerResolver::StartResolution() {
  if (resolve_in_progress) {
    return kDNSServiceErr_NoError;
  }

  DNSServiceErrorType error = DNSServiceResolve(
      &m_resolve_ref,
      0,
      interface_index,
      service_name.c_str(),
      regtype.c_str(),
      reply_domain.c_str(),
      &ResolveServiceCallback,
      reinterpret_cast<void*>(this));
  if (error == kDNSServiceErr_NoError) {
    resolve_in_progress = true;
    m_io_adapter->AddDescriptor(m_resolve_ref);
  }
  return error;
}

bool ControllerResolver::GetControllerResolver(
    E133DiscoveryAgentInterface::E133ControllerInfo *info) {
  if (m_resolved_address.Host().IsWildcard()) {
    return false;
  }

  info->priority = m_priority;
  info->address = m_resolved_address;
  return true;
}

void ControllerResolver::ResolveHandler(
    DNSServiceErrorType errorCode,
    const string &host_target,
    uint16_t port,
    uint16_t txtLen,
    const unsigned char *txtRecord) {
  // Do we need to remove here or let this continue running?
  // m_io_adapter->RemoveDescriptor(m_);

  if (errorCode != kDNSServiceErr_NoError) {
    OLA_WARN << "Failed to resolve " << this->ToString();
    return;
  }

  OLA_INFO << "Got resolv response " << host_target << ":"
           << port;

  uint8_t priority = DEFAULT_PRIORITY;
  if (TXTRecordContainsKey(txtLen, txtRecord, PRIORITY_KEY)) {
    uint8_t value_length = 0;
    const void *value = TXTRecordGetValuePtr(txtLen, txtRecord, PRIORITY_KEY,
                                             &value_length);
    if (value == NULL) {
      OLA_WARN << "Missing " << PRIORITY_KEY << " from " << host_target;
    } else {
      const string value_str(
          reinterpret_cast<const char*>(value), value_length);
      if (!ola::StringToInt(value_str, &priority)) {
        OLA_WARN << "Invalid value for " << PRIORITY_KEY << " in "
                 << host_target;
      }
    }
  } else {
    OLA_WARN << host_target << " is missing key " << PRIORITY_KEY
             << " in the TXT record";
  }

  m_priority = priority;
  m_resolved_address.Port(port);

  if (host_target == m_host_target) {
    return;
  }
  m_host_target = host_target;

  // Otherwise start a new resolution
  if (to_addr_in_progress) {
    m_io_adapter->RemoveDescriptor(m_to_addr_ref);
    DNSServiceRefDeallocate(m_to_addr_ref);
  }

  OLA_INFO << "Calling DNSServiceGetAddrInfo for " << m_host_target;
  DNSServiceErrorType error = DNSServiceGetAddrInfo(
      &m_to_addr_ref,
      0,
      interface_index,
      kDNSServiceProtocol_IPv4,
      m_host_target.c_str(),
      &ResolveAddressCallback,
      reinterpret_cast<void*>(this));

  if (error == kDNSServiceErr_NoError) {
    m_io_adapter->AddDescriptor(m_to_addr_ref);
  } else {
    OLA_WARN << "DNSServiceGetAddrInfo for " << m_host_target
             << " failed with " << error;
  }
}

void ControllerResolver::UpdateAddress(const IPV4Address &v4_address) {
  m_resolved_address.Host(v4_address);
  OLA_INFO << "New controller at " << m_resolved_address;
}
*/

// AvahiE133DiscoveryAgent
// ----------------------------------------------------------------------------
AvahiE133DiscoveryAgent::AvahiE133DiscoveryAgent()
    : m_threaded_poll(avahi_threaded_poll_new()),
      m_client(NULL),
      m_reconnect_timeout(NULL),
      m_backoff(new ola::ExponentialBackoffPolicy(TimeInterval(1, 0),
                                                  TimeInterval(60, 0))),
      m_controller_browser(NULL) {
}

AvahiE133DiscoveryAgent::~AvahiE133DiscoveryAgent() {
  Stop();
}

bool AvahiE133DiscoveryAgent::Init() {
  CreateNewClient();

  if (m_threaded_poll) {
    avahi_threaded_poll_start(m_threaded_poll);
    return true;
  } else {
    return false;
  }
}

bool AvahiE133DiscoveryAgent::Stop() {
  if (m_threaded_poll) {
    avahi_threaded_poll_stop(m_threaded_poll);
  }

  if (m_client) {
    avahi_client_free(m_client);
    m_client = NULL;
  }

  if (m_threaded_poll) {
    avahi_threaded_poll_free(m_threaded_poll);
    m_threaded_poll = NULL;
  }

  /*
  if (m_registration_ref) {
    m_io_adapter->RemoveDescriptor(m_registration_ref);
    DNSServiceRefDeallocate(m_registration_ref);
    m_registration_ref = NULL;
  }

  */
  return true;
}

bool AvahiE133DiscoveryAgent::FindControllers(BrowseCallback *callback) {
  if (!callback) {
    return false;
  }

  MutexLocker lock(&m_controllers_mu);
  vector<E133ControllerInfo> controllers;

  /*
  ControllerResolverList::iterator iter = m_controllers.begin();
  for (; iter != m_controllers.end(); ++iter) {
    E133ControllerInfo info;
    if ((*iter)->GetControllerResolver(&info)) {
      controllers.push_back(info);
    }
  }
  */
  callback->Run(controllers);
  return true;
}

void AvahiE133DiscoveryAgent::FindControllers(
    vector<E133ControllerInfo> *controllers) {

  MutexLocker lock(&m_controllers_mu);

  (void) controllers;

  /*
  ControllerResolverList::iterator iter = m_controllers.begin();
  for (; iter != m_controllers.end(); ++iter) {
    E133ControllerInfo info;
    if ((*iter)->GetControllerResolver(&info)) {
      controllers->push_back(info);
    }
  }
  */
}

void AvahiE133DiscoveryAgent::RegisterController(
    const IPV4SocketAddress &controller) {

  (void) controller;
}

/*
 * This is a bit tricky because it can be called from either the main thread on
 * startup or from the poll thread.
 */
void AvahiE133DiscoveryAgent::ClientStateChanged(AvahiClientState state,
                                                 AvahiClient *client) {
  // The first time this is called is from the avahi_client_new context. In
  // that case m_client is still null so we set it here.
  if (!m_client) {
    m_client = client;
  }

  OLA_INFO << "Avahi client state changed to " << ClientStateToString(state);

  switch (state) {
    case AVAHI_CLIENT_S_RUNNING:
      // The server has startup successfully and registered its host
      // name on the network, so it's time to create our services.
      // register_stuff

      // UpdateServices();
      break;
    case AVAHI_CLIENT_FAILURE:
      // DeregisterAllServices();

      SetUpReconnectTimeout();
      break;
    case AVAHI_CLIENT_S_COLLISION:
      // There was a hostname collision on the network.
      // Let's drop our registered services. When the server is back
      // in AVAHI_SERVER_RUNNING state we will register them again with the
      // new host name.

      // DeregisterAllServices();

      break;
    case AVAHI_CLIENT_S_REGISTERING:
      // The server records are now being established. This
      // might be caused by a host name change. We need to wait
      // for our own records to register until the host name is
      // properly established.

      // DeregisterAllServices();
      break;
    case AVAHI_CLIENT_CONNECTING:
      break;
  }
}

void AvahiE133DiscoveryAgent::ReconnectTimeout() {
  if (m_client) {
    avahi_client_free(m_client);
    m_client = NULL;
  }
  CreateNewClient();
}

/*
void AvahiE133DiscoveryAgent::RunThread() {
  OLA_INFO << "Starting Discovery thread";

  DNSServiceErrorType error = DNSServiceBrowse(
      &m_discovery_service_ref,
      0,
      kDNSServiceInterfaceIndexAny,
      E133_CONTROLLER_SERVICE,
      NULL,  // domain
      &BrowseServiceCallback,
      reinterpret_cast<void*>(this));

  if (error != kDNSServiceErr_NoError) {
    OLA_WARN << "DNSServiceBrowse returned " << error;
    // f->Set(false);
    return;
  }

  // f->Set(true);
  m_io_adapter->AddDescriptor(m_discovery_service_ref);
  m_ss.Run();

  m_io_adapter->RemoveDescriptor(m_discovery_service_ref);
  DNSServiceRefDeallocate(m_discovery_service_ref);
  OLA_INFO << "Done with discovery thread";
}

void AvahiE133DiscoveryAgent::BrowseResult(DNSServiceFlags flags,
                                             uint32_t interface_index,
                                             const string &service_name,
                                             const string &regtype,
                                             const string &reply_domain) {
  if (flags & kDNSServiceFlagsAdd) {
    ControllerResolver *controller = new ControllerResolver(
        m_io_adapter.get(), interface_index, service_name, regtype,
        reply_domain);

    DNSServiceErrorType error = controller->StartResolution();
    OLA_INFO << "Starting resolution for " << *controller << ", ret was "
             << error;

    if (error == kDNSServiceErr_NoError) {
      m_controllers.push_back(controller);
      OLA_INFO << "Added " << *controller << " at " << m_controllers.back();
    } else {
      OLA_WARN << "Failed to start resolution for " << *controller;
      delete controller;
    }
  } else {
    ControllerResolver controller(m_io_adapter.get(), interface_index,
                                  service_name, regtype, reply_domain);
    ControllerResolverList::iterator iter = m_controllers.begin();
    for (; iter != m_controllers.end(); ++iter) {
      if (**iter == controller) {
        // Cancel DNSServiceRef.
        OLA_INFO << "Removed " << controller << " at " << *iter;
        delete *iter;
        m_controllers.erase(iter);
        return;
      }
    }
    OLA_INFO << "Failed to find " << controller;
  }
}

void AvahiE133DiscoveryAgent::InternalRegisterService(
    IPV4SocketAddress controller_address) {
  ostringstream str;
  str << "controller-" << controller_address.Port();
  const string service = str.str();
  str.str("");

  str << "priority=" << static_cast<int>(FLAGS_controller_priority);

  OLA_INFO << "Adding " << service << " : " << E133_CONTROLLER_SERVICE << ":"
           << controller_address.Port() << ", txt: " << str.str();

  string txt_data;
  txt_data.append(1, static_cast<char>(str.str().size()));
  txt_data.append(str.str());

  DNSServiceErrorType error = DNSServiceRegister(
      &m_registration_ref,
      0, 0, service.c_str(), E133_CONTROLLER_SERVICE,
      NULL,  // default domain
      NULL,  // use default host name
      HostToNetwork(controller_address.Port()),
      txt_data.size(), txt_data.c_str(),
      &RegisterCallback,  // call back function
      NULL);  // no context

  if (error != kDNSServiceErr_NoError) {
    OLA_WARN << "DNSServiceRegister returned " << error;
    return;
  }

  // TODO(simon): allow this to be called more than once.
  m_io_adapter->AddDescriptor(m_registration_ref);
}
*/


void AvahiE133DiscoveryAgent::CreateNewClient() {
  if (m_client) {
    OLA_WARN << "CreateNewClient called but m_client is not NULL";
    return;
  }

  if (m_threaded_poll) {
    int error;
    // In the successful case, m_client is set in the ClientStateChanged method
    m_client = avahi_client_new(avahi_threaded_poll_get(m_threaded_poll),
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

void AvahiE133DiscoveryAgent::SetUpReconnectTimeout() {
  // We don't strictly need an ExponentialBackoffPolicy here because the client
  // goes into the AVAHI_CLIENT_CONNECTING state if the server isn't running.
  // Still, it's a useful defense against spinning rapidly if something goes
  // wrong.
  TimeInterval delay = m_backoff.Next();
  OLA_INFO << "Re-creating avahi client in " << delay << "s";
  struct timeval tv;
  delay.AsTimeval(&tv);

  const AvahiPoll *poll = avahi_threaded_poll_get(m_threaded_poll);
  if (m_reconnect_timeout) {
    poll->timeout_update(m_reconnect_timeout, &tv);
  } else {
    m_reconnect_timeout = poll->timeout_new(
        avahi_threaded_poll_get(m_threaded_poll),
        &tv,
        reconnect_callback,
        this);
  }
}

string AvahiE133DiscoveryAgent::ClientStateToString(AvahiClientState state) {
  switch (state) {
    case AVAHI_CLIENT_S_REGISTERING:
      return "AVAHI_CLIENT_S_REGISTERING";
    case AVAHI_CLIENT_S_RUNNING:
      return "AVAHI_CLIENT_S_RUNNING";
    case AVAHI_CLIENT_S_COLLISION:
      return "AVAHI_CLIENT_S_COLLISION";
    case AVAHI_CLIENT_FAILURE:
      return "AVAHI_CLIENT_FAILURE";
    case AVAHI_CLIENT_CONNECTING:
      return "AVAHI_CLIENT_CONNECTING";
    default:
      return "Unknown state";
  }
}

string AvahiE133DiscoveryAgent::GroupStateToString(AvahiEntryGroupState state) {
  switch (state) {
    case AVAHI_ENTRY_GROUP_UNCOMMITED:
      return "AVAHI_ENTRY_GROUP_UNCOMMITED";
    case AVAHI_ENTRY_GROUP_REGISTERING:
      return "AVAHI_ENTRY_GROUP_REGISTERING";
    case AVAHI_ENTRY_GROUP_ESTABLISHED:
      return "AVAHI_ENTRY_GROUP_ESTABLISHED";
    case AVAHI_ENTRY_GROUP_COLLISION:
      return "AVAHI_ENTRY_GROUP_COLLISION";
    case AVAHI_ENTRY_GROUP_FAILURE:
      return "AVAHI_ENTRY_GROUP_FAILURE";
    default:
      return "Unknown state";
  }
}
