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

#include "tools/e133/AvahiDiscoveryAgent.h"

#include <avahi-client/lookup.h>
#include <avahi-client/publish.h>
#include <avahi-common/error.h>
#include <avahi-common/strlst.h>

#include <netinet/in.h>
#include <ola/Callback.h>
#include <ola/Logging.h>
#include <ola/network/NetworkUtils.h>
#include <ola/stl/STLUtils.h>
#include <ola/io/Descriptor.h>
#include <stdint.h>

#include <map>
#include <string>
#include <vector>
#include <utility>

#include "tools/e133/AvahiHelper.h"
#include "tools/e133/AvahiOlaPoll.h"

using ola::io::SelectServerInterface;
using ola::io::UnmanagedFileDescriptor;
using ola::network::HostToNetwork;
using ola::network::IPV4Address;
using ola::network::IPV4SocketAddress;
using ola::network::NetworkToHost;
using ola::NewCallback;
using ola::NewSingleCallback;
using ola::thread::MutexLocker;
using ola::TimeInterval;
using std::auto_ptr;
using std::ostringstream;
using std::string;
using std::vector;

// ControllerResolver
// ----------------------------------------------------------------------------
class ControllerResolver {
 public:
  ControllerResolver(AvahiIfIndex interface_index,
                     AvahiProtocol protocol,
                     const std::string &service_name,
                     const std::string &type,
                     const std::string &domain);

  ControllerResolver(const ControllerResolver &other);
  ~ControllerResolver();

  bool operator==(const ControllerResolver &other) const;

  std::string ToString() const;

  friend std::ostream& operator<<(std::ostream &out,
                                  const ControllerResolver &info) {
    return out << info.ToString();
  }

  // DNSServiceErrorType StartResolution();

  bool GetControllerEntry(E133ControllerEntry *entry);

  /*
  void ResolveHandler(
      DNSServiceErrorType errorCode,
      const string &host_target,
      uint16_t port,
      uint16_t txtLen,
      const unsigned char *txtRecord);

  void UpdateAddress(const IPV4Address &v4_address);
  */

 private:
  const AvahiIfIndex m_interface_index;
  const AvahiProtocol m_protocol;
  const std::string m_service_name;
  const std::string m_type;
  const std::string m_domain;

  uint8_t m_priority;
  ola::network::IPV4SocketAddress m_resolved_address;

  static const uint8_t DEFAULT_PRIORITY;
};

const uint8_t ControllerResolver::DEFAULT_PRIORITY = 100;

// ControllerRegistration
// ----------------------------------------------------------------------------
class ControllerRegistration {
 public:
  explicit ControllerRegistration(AvahiClient *client, AvahiClientState state)
      : m_client(client),
        m_state(state),
        m_entry_group(NULL) {
  }

  ~ControllerRegistration();

  void ChangeState(AvahiClientState state);

  void RegisterOrUpdate(const E133ControllerEntry &controller);

  void GroupEvent(AvahiEntryGroupState state);

 private:
  AvahiClient *m_client;
  AvahiClientState m_state;
  E133ControllerEntry m_controller_entry;
  AvahiEntryGroup *m_entry_group;

  void PerformRegistration(const E133ControllerEntry &controller);
  void UpdateRegistration(const E133ControllerEntry &controller);
  void CancelRegistration();

  AvahiStringList *BuildTxtRecord(const E133ControllerEntry &controller);

  DISALLOW_COPY_AND_ASSIGN(ControllerRegistration);
};

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

static void browse_callback(AvahiServiceBrowser *b,
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

  OLA_INFO << "In browse_callback: " << BrowseEventToString(event);
  agent->BrowseEvent(interface, protocol, event, name, type, domain, flags);

  (void) b;
}

static void entry_group_callback(AvahiEntryGroup *group,
                                 AvahiEntryGroupState state,
                                 void *data) {
  ControllerRegistration *controller_registration =
      reinterpret_cast<ControllerRegistration*>(data);
  controller_registration->GroupEvent(state);
  (void) group;
}
}  // namespace

// ControllerResolver
// ----------------------------------------------------------------------------
ControllerResolver::ControllerResolver(AvahiIfIndex interface_index,
                                       AvahiProtocol protocol,
                                       const std::string &service_name,
                                       const std::string &type,
                                       const std::string &domain)
    : m_interface_index(interface_index),
      m_protocol(protocol),
      m_service_name(service_name),
      m_type(type),
      m_domain(domain) {
}

ControllerResolver::ControllerResolver(
    const ControllerResolver &other)
    : m_interface_index(other.m_interface_index),
      m_protocol(other.m_protocol),
      m_service_name(other.m_service_name),
      m_type(other.m_type),
      m_domain(other.m_domain) {
}

ControllerResolver::~ControllerResolver() {
  /*
  if (resolve_in_progress) {
    m_io_adapter->RemoveDescriptor(m_resolve_ref);
    DNSServiceRefDeallocate(m_resolve_ref);
  }

  if (to_addr_in_progress) {
    m_io_adapter->RemoveDescriptor(m_to_addr_ref);
    DNSServiceRefDeallocate(m_to_addr_ref);
  }
  */
}

bool ControllerResolver::operator==(const ControllerResolver &other) const {
  return (m_interface_index == other.m_interface_index &&
          m_protocol == other.m_protocol &&
          m_service_name == other.m_service_name &&
          m_type == other.m_type &&
          m_domain == other.m_domain);
}

string ControllerResolver::ToString() const {
  std::ostringstream str;
  str << m_service_name << "." << m_type << m_domain << " on iface "
      << m_interface_index;
  return str.str();
}

/*
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
*/

bool ControllerResolver::GetControllerEntry(E133ControllerEntry *entry) {
  /*
  if (m_resolved_address.Host().IsWildcard()) {
    return false;
  }

  */
  entry->priority = m_priority;
  // info->address = m_resolved_address;
  return true;
}

/*
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

// ControllerRegistration
// ----------------------------------------------------------------------------

ControllerRegistration::~ControllerRegistration() {
  CancelRegistration();
}

void ControllerRegistration::ChangeState(AvahiClientState state) {
  if (state == AVAHI_CLIENT_S_RUNNING && state != m_state) {
    m_state = state;
    PerformRegistration(m_controller_entry);
  } else {
    CancelRegistration();
    m_state = state;
  }
}

void ControllerRegistration::RegisterOrUpdate(
    const E133ControllerEntry &controller) {
  if (m_controller_entry == controller) {
    // No change.
    return;
  }

  if (m_state != AVAHI_CLIENT_S_RUNNING) {
    // Store the controller info until we change to running.
    m_controller_entry = controller;
    return;
  }

  if (m_entry_group) {
    OLA_INFO << "Updating controller registration for " << controller.address;
    UpdateRegistration(controller);
  } else {
    PerformRegistration(controller);
  }
  m_controller_entry = controller;
}

void ControllerRegistration::GroupEvent(AvahiEntryGroupState state) {
  OLA_INFO << "Group state changed to " << GroupStateToString(state);
}

void ControllerRegistration::PerformRegistration(
    const E133ControllerEntry &controller) {
  if (m_entry_group) {
    OLA_WARN << "Already got an AvahiEntryGroup!";
  }

  m_entry_group = avahi_entry_group_new(m_client, entry_group_callback, this);
  if (!m_entry_group) {
    OLA_WARN << "avahi_entry_group_new() failed: "
             << avahi_strerror(avahi_client_errno(m_client));
    return;
  }

  AvahiStringList *txt_str_list = BuildTxtRecord(controller);;

  // Change to  avahi_entry_group_add_service_strlst
  int ret = avahi_entry_group_add_service_strlst(
      m_entry_group, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC,
      static_cast<AvahiPublishFlags>(0),
      controller.ServiceName().c_str(),
      E133DiscoveryAgentInterface::E133_CONTROLLER_SERVICE,
      NULL, NULL, controller.address.Port(), txt_str_list);

  avahi_string_list_free(txt_str_list);

  if (ret < 0) {
    OLA_WARN << "Failed to add " << controller << " : " << avahi_strerror(ret);

    if (ret == AVAHI_ERR_COLLISION) {
      OLA_WARN << "AVAHI_ERR_COLLISION";
    }
    return;
  }

  // Add subtypes here

  ret = avahi_entry_group_commit(m_entry_group);
  if (ret < 0) {
    OLA_WARN << "Failed to commit controller " << controller << " : "
             << avahi_strerror(ret);
  }
}

void ControllerRegistration::UpdateRegistration(
    const E133ControllerEntry &controller) {
  // Check for scope change?

  // TODO(simon): implement me
  (void) controller;
}

void ControllerRegistration::CancelRegistration() {
  if (!m_entry_group) {
    return;
  }
  avahi_entry_group_free(m_entry_group);
  m_entry_group = NULL;
}

AvahiStringList *ControllerRegistration::BuildTxtRecord(
    const E133ControllerEntry &controller) {
  AvahiStringList *txt_str_list = NULL;
  txt_str_list = avahi_string_list_add_printf(
      txt_str_list, "%s=%d",
      E133DiscoveryAgentInterface::TXT_VERSION_KEY,
      E133DiscoveryAgentInterface::TXT_VERSION);

  txt_str_list = avahi_string_list_add_printf(
      txt_str_list, "%s=%d",
      E133DiscoveryAgentInterface::PRIORITY_KEY,
      controller.priority);

  txt_str_list = avahi_string_list_add_printf(
      txt_str_list, "%s=%s",
      E133DiscoveryAgentInterface::SCOPE_KEY,
      controller.scope.c_str());

  txt_str_list = avahi_string_list_add_printf(
      txt_str_list, "%s=%d",
      E133DiscoveryAgentInterface::E133_VERSION_KEY,
      controller.e133_version);

  if (controller.uid.ManufacturerId() != 0 && controller.uid.DeviceId() != 0) {
    txt_str_list = avahi_string_list_add_printf(
        txt_str_list, "%s=%s",
        E133DiscoveryAgentInterface::UID_KEY,
        controller.uid.ToString().c_str());
  }

  if (!controller.model.empty()) {
    txt_str_list = avahi_string_list_add_printf(
        txt_str_list, "%s=%s",
        E133DiscoveryAgentInterface::MODEL_KEY,
        controller.model.c_str());
  }

  if (!controller.manufacturer.empty()) {
    txt_str_list = avahi_string_list_add_printf(
        txt_str_list, "%s=%s",
        E133DiscoveryAgentInterface::MANUFACTURER_KEY,
        controller.manufacturer.c_str());
  }

  return txt_str_list;
}

// AvahiE133DiscoveryAgent
// ----------------------------------------------------------------------------
AvahiE133DiscoveryAgent::AvahiE133DiscoveryAgent()
    : m_avahi_poll(NULL),
      m_client(NULL),
      m_state(AVAHI_CLIENT_CONNECTING),
      m_reconnect_timeout(NULL),
      m_backoff(new ola::ExponentialBackoffPolicy(TimeInterval(1, 0),
                                                  TimeInterval(60, 0))),
      m_controller_browser(NULL) {
}

AvahiE133DiscoveryAgent::~AvahiE133DiscoveryAgent() {
  Stop();
}

bool AvahiE133DiscoveryAgent::Start() {
  m_thread.reset(new ola::thread::CallbackThread(ola::NewSingleCallback(
      this, &AvahiE133DiscoveryAgent::RunThread)));
  m_thread->Start();
  return true;
}

bool AvahiE133DiscoveryAgent::Stop() {
  if (m_thread.get() && m_thread->IsRunning()) {
    m_ss.Terminate();
    m_thread->Join();
    m_thread.reset();
  }

  return true;

  /*
  if (m_registration_ref) {
    m_io_adapter->RemoveDescriptor(m_registration_ref);
    DNSServiceRefDeallocate(m_registration_ref);
    m_registration_ref = NULL;
  }

  */
  return true;
}

void AvahiE133DiscoveryAgent::SetScope(const std::string &scope) {

  (void) scope;
}

void AvahiE133DiscoveryAgent::FindControllers(
    ControllerEntryList *controllers) {
  MutexLocker lock(&m_controllers_mu);

  ControllerResolverList::iterator iter = m_controllers.begin();
  for (; iter != m_controllers.end(); ++iter) {
    E133ControllerEntry entry;
    if ((*iter)->GetControllerEntry(&entry)) {
      controllers->push_back(entry);
    }
  }
}

void AvahiE133DiscoveryAgent::RegisterController(
    const E133ControllerEntry &controller) {
  m_ss.Execute(ola::NewSingleCallback(
      this,
      &AvahiE133DiscoveryAgent::InternalRegisterService, controller));
}

void AvahiE133DiscoveryAgent::DeRegisterController(
      const ola::network::IPV4SocketAddress &controller_address) {
  m_ss.Execute(ola::NewSingleCallback(
      this, &AvahiE133DiscoveryAgent::InternalDeRegisterService,
      controller_address));
}

void AvahiE133DiscoveryAgent::RunThread() {
  m_avahi_poll = new AvahiOlaPoll(&m_ss);

  CreateNewClient();
  m_ss.Run();

  if (m_controller_browser) {
    avahi_service_browser_free(m_controller_browser);
  }

  if (m_client) {
    avahi_client_free(m_client);
    m_client = NULL;
  }

  delete m_avahi_poll;
  m_avahi_poll = NULL;

  /*
  {
    MutexLocker lock(&m_controllers_mu);
    StopResolution();
  }
  */
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

  m_state = state;
  OLA_INFO << "Avahi client state changed to " << ClientStateToString(state);

  ControllerRegistrationList::iterator iter = m_registrations.begin();
  for (; iter != m_registrations.end(); ++iter) {
    iter->second->ChangeState(state);
  }

  switch (state) {
    case AVAHI_CLIENT_S_RUNNING:
      // The server has startup successfully and registered its host
      // name on the network, so it's time to create our services.
      // register_stuff

      LocateControllerServices();

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

void AvahiE133DiscoveryAgent::BrowseEvent(AvahiIfIndex interface,
                                          AvahiProtocol protocol,
                                          AvahiBrowserEvent event,
                                          const char *name,
                                          const char *type,
                                          const char *domain,
                                          AvahiLookupResultFlags flags) {
  switch (event) {
    case AVAHI_BROWSER_FAILURE:
      OLA_WARN << "(Browser) " << avahi_strerror(avahi_client_errno(
            avahi_service_browser_get_client(m_controller_browser)));
      return;
    case AVAHI_BROWSER_NEW:
      OLA_INFO << "(Browser) NEW: service " << name << " of type " << type
               << " in domain " << domain;

      AddController(interface, protocol, name, type, domain);
      break;

    case AVAHI_BROWSER_REMOVE:
      RemoveController(interface, protocol, name, type, domain);
      break;

    case AVAHI_BROWSER_ALL_FOR_NOW:
    case AVAHI_BROWSER_CACHE_EXHAUSTED:
      OLA_WARN << "(Browser) "
               << (event == AVAHI_BROWSER_CACHE_EXHAUSTED ? "CACHE_EXHAUSTED" :
                    "ALL_FOR_NOW");
      break;
  }
  (void) flags;
}

/*

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

  if (m_avahi_poll) {
    int error;
    // In the successful case, m_client is set in the ClientStateChanged method
    avahi_client_new(m_avahi_poll->GetPoll(),
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

  const AvahiPoll *poll = m_avahi_poll->GetPoll();
  if (m_reconnect_timeout) {
    poll->timeout_update(m_reconnect_timeout, &tv);
  } else {
    m_reconnect_timeout = poll->timeout_new(
        poll, &tv, reconnect_callback, this);
  }
}

void AvahiE133DiscoveryAgent::LocateControllerServices() {
  m_controller_browser = avahi_service_browser_new(
      m_client, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC,
      E133_CONTROLLER_SERVICE, NULL,
      static_cast<AvahiLookupFlags>(0), browse_callback, this);
  if (!m_controller_browser) {
    OLA_WARN << "Failed to start browsing for " << E133_CONTROLLER_SERVICE
             << ": " << avahi_strerror(avahi_client_errno(m_client));
  }
}

void AvahiE133DiscoveryAgent::AddController(AvahiIfIndex interface,
                                            AvahiProtocol protocol,
                                            const std::string &name,
                                            const std::string &type,
                                            const std::string &domain) {
  ControllerResolver *controller = new ControllerResolver(
      interface, protocol, name, type, domain);

  /*
  DNSServiceErrorType error = controller->StartResolution();
  */
  OLA_INFO << "Starting resolution for " << *controller;

  /*
  if (error == kDNSServiceErr_NoError) {
    m_controllers.push_back(controller);
    OLA_INFO << "Added " << *controller << " at " << m_controllers.back();
  } else {
    OLA_WARN << "Failed to start resolution for " << *controller;
    delete controller;
  }

       We ignore the returned resolver object. In the callback
         function we free it. If the server is terminated before
         the callback function is called the server will free
         the resolver for us. 

      if (!(avahi_service_resolver_new(c, interface, protocol, name, type, domain, AVAHI_PROTO_UNSPEC, 0, resolve_callback, c)))
          fprintf(stderr, "Failed to resolve service '%s': %s\n", name, avahi_strerror(avahi_client_errno(c)));

      (void) interface;
      (void) flags;
      (void) protocol;
  */
}

void AvahiE133DiscoveryAgent::RemoveController(AvahiIfIndex interface,
                                               AvahiProtocol protocol,
                                               const std::string &name,
                                               const std::string &type,
                                               const std::string &domain) {
  OLA_WARN << "(Browser) REMOVE: service " << name << " of type " << type
           << " in domain " << domain;

  (void) interface;
  (void) protocol;
}

void AvahiE133DiscoveryAgent::InternalRegisterService(
    E133ControllerEntry controller) {
  std::pair<ControllerRegistrationList::iterator, bool> p =
      m_registrations.insert(
          ControllerRegistrationList::value_type(controller.address, NULL));

  if (p.first->second == NULL) {
    p.first->second = new ControllerRegistration(m_client, m_state);
  }
  ControllerRegistration *registration = p.first->second;
  registration->RegisterOrUpdate(controller);
}

void AvahiE133DiscoveryAgent::InternalDeRegisterService(
      ola::network::IPV4SocketAddress controller_address) {
  ola::STLRemoveAndDelete(&m_registrations, controller_address);
}
