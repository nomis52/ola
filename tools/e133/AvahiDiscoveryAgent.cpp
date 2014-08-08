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
#include <avahi-common/alternative.h>
#include <avahi-common/error.h>
#include <avahi-common/malloc.h>
#include <avahi-common/strlst.h>

#include <netinet/in.h>
#include <ola/Callback.h>
#include <ola/io/Descriptor.h>
#include <ola/Logging.h>
#include <ola/network/NetworkUtils.h>
#include <ola/stl/STLUtils.h>
#include <ola/thread/Future.h>
#include <stdint.h>

#include <memory>
#include <string>
#include <utility>

#include "tools/e133/AvahiHelper.h"
#include "tools/e133/AvahiOlaPoll.h"

using ola::io::SelectServerInterface;
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
class ControllerRegistration : public ClientStateChangeListener {
 public:
  explicit ControllerRegistration(AvahiOlaClient *client);
  ~ControllerRegistration();

  void ClientStateChanged(AvahiClientState state);

  void RegisterOrUpdate(const E133ControllerEntry &controller);

  void GroupEvent(AvahiEntryGroupState state);

 private:
  AvahiOlaClient *m_client;
  E133ControllerEntry m_controller_entry;
  AvahiEntryGroup *m_entry_group;

  void PerformRegistration();
  bool AddGroupEntry(AvahiEntryGroup *group);
  void UpdateRegistration(const E133ControllerEntry &new_controller);
  void CancelRegistration();
  void ChooseAlternateServiceName();

  AvahiStringList *BuildTxtRecord(const E133ControllerEntry &controller);

  DISALLOW_COPY_AND_ASSIGN(ControllerRegistration);
};

// static callback functions
// ----------------------------------------------------------------------------

namespace {

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
ControllerRegistration::ControllerRegistration(AvahiOlaClient *client)
    : m_client(client),
      m_entry_group(NULL) {
  m_client->AddStateChangeListener(this);
}

ControllerRegistration::~ControllerRegistration() {
  CancelRegistration();
  m_client->RemoveStateChangeListener(this);
}

void ControllerRegistration::ClientStateChanged(AvahiClientState state) {
  switch (state) {
    case AVAHI_CLIENT_S_RUNNING:
      PerformRegistration();
      break;
    default:
      CancelRegistration();
  }
}

void ControllerRegistration::RegisterOrUpdate(
    const E133ControllerEntry &controller) {
  if (m_controller_entry == controller) {
    // No change.
    return;
  }

  if (m_client->GetState() != AVAHI_CLIENT_S_RUNNING) {
    // Store the controller info until we change to running.
    m_controller_entry = controller;
    return;
  }

  if (m_entry_group) {
    OLA_INFO << "Updating controller registration for " << controller.address;
    UpdateRegistration(controller);
  } else {
    m_controller_entry = controller;
    PerformRegistration();
  }
}

void ControllerRegistration::GroupEvent(AvahiEntryGroupState state) {
  OLA_INFO << "Group state changed to " << GroupStateToString(state);
  if (state == AVAHI_ENTRY_GROUP_COLLISION) {
    ChooseAlternateServiceName();
    PerformRegistration();
  }
}

void ControllerRegistration::PerformRegistration() {
  AvahiEntryGroup *group = NULL;
  if (m_entry_group) {
    group = m_entry_group;
    m_entry_group = NULL;
  } else {
    group = m_client->CreateEntryGroup(entry_group_callback, this);
    if (!group) {
      OLA_WARN << "avahi_entry_group_new() failed: "
               << m_client->GetLastError();
      return;
    }
  }

  if (!AddGroupEntry(group)) {
    avahi_entry_group_free(group);
  } else {
    m_entry_group = group;
  }
}

bool ControllerRegistration::AddGroupEntry(AvahiEntryGroup *group) {
  AvahiStringList *txt_str_list = BuildTxtRecord(m_controller_entry);

  int ret = avahi_entry_group_add_service_strlst(
      group, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC,
      static_cast<AvahiPublishFlags>(0),
      m_controller_entry.ServiceName().c_str(),
      E133DiscoveryAgentInterface::E133_CONTROLLER_SERVICE,
      NULL, NULL, m_controller_entry.address.Port(), txt_str_list);

  avahi_string_list_free(txt_str_list);

  if (ret < 0) {
    if (ret == AVAHI_ERR_COLLISION) {
      ChooseAlternateServiceName();
      PerformRegistration();
    } else {
      OLA_WARN << "Failed to add " << m_controller_entry << " : "
               << avahi_strerror(ret);
    }
    return false;
  }

  if (!m_controller_entry.scope.empty()) {
    ostringstream sub_type;
    sub_type << "_" << m_controller_entry.scope << "._sub."
             << E133DiscoveryAgentInterface::E133_CONTROLLER_SERVICE;

    ret = avahi_entry_group_add_service_subtype(
        group, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC,
        static_cast<AvahiPublishFlags>(0),
        m_controller_entry.ServiceName().c_str(),
        E133DiscoveryAgentInterface::E133_CONTROLLER_SERVICE,
        NULL, sub_type.str().c_str());

    if (ret < 0) {
      OLA_WARN << "Failed to add subtype for " << m_controller_entry << " : "
               << avahi_strerror(ret);
      return false;
    }
  }

  ret = avahi_entry_group_commit(group);
  if (ret < 0) {
    OLA_WARN << "Failed to commit controller " << m_controller_entry << " : "
             << avahi_strerror(ret);
  }
  OLA_INFO << "committed " << group << " : "
           << m_controller_entry.ServiceName();
  return ret == 0;
}

void ControllerRegistration::UpdateRegistration(
    const E133ControllerEntry &new_controller) {
  if (new_controller == m_controller_entry) {
    return;
  }

  if (new_controller.scope != m_controller_entry.scope) {
    // We require a full reset.
    avahi_entry_group_reset(m_entry_group);
    m_controller_entry.UpdateFrom(new_controller);
    PerformRegistration();
    return;
  }

  m_controller_entry.UpdateFrom(new_controller);

  AvahiStringList *txt_str_list = BuildTxtRecord(m_controller_entry);

  OLA_INFO << "updating  " << m_entry_group << " : " <<
    m_controller_entry.ServiceName();
  int ret = avahi_entry_group_update_service_txt_strlst(
      m_entry_group, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC,
      static_cast<AvahiPublishFlags>(0),
      m_controller_entry.ServiceName().c_str(),
      E133DiscoveryAgentInterface::E133_CONTROLLER_SERVICE,
      NULL, txt_str_list);

  avahi_string_list_free(txt_str_list);

  if (ret < 0) {
    OLA_WARN << "Failed to update controller " << m_controller_entry << ": "
             << avahi_strerror(ret);
  }
}

void ControllerRegistration::CancelRegistration() {
  if (!m_entry_group) {
    return;
  }
  avahi_entry_group_free(m_entry_group);
  m_entry_group = NULL;
}

void ControllerRegistration::ChooseAlternateServiceName() {
  char *new_name = avahi_alternative_service_name(
    m_controller_entry.ServiceName().c_str());
  OLA_INFO << "Renamed " << m_controller_entry.ServiceName() << " to "
           << new_name;
  m_controller_entry.SetServiceName(new_name);
  avahi_free(new_name);
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
    : m_controller_browser(NULL) {
}

AvahiE133DiscoveryAgent::~AvahiE133DiscoveryAgent() {
  Stop();
}

bool AvahiE133DiscoveryAgent::Start() {
  ola::thread::Future<void> f;
  m_thread.reset(new ola::thread::CallbackThread(ola::NewSingleCallback(
      this, &AvahiE133DiscoveryAgent::RunThread, &f)));
  m_thread->Start();
  f.Get();
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

void AvahiE133DiscoveryAgent::ClientStateChanged(AvahiClientState state) {
  switch (state) {
    case AVAHI_CLIENT_S_RUNNING:
      // The server has started successfully and registered its host
      // name on the network, so we can start locating the controllers.
      LocateControllerServices();
      break;
    default:
      // MutexLocker lock(&m_controllers_mu);
      // StopResolution();
      {}
  }
}

void AvahiE133DiscoveryAgent::RunThread(ola::thread::Future<void> *future) {
  m_avahi_poll.reset(new AvahiOlaPoll(&m_ss));
  m_client.reset(new AvahiOlaClient(m_avahi_poll.get()));

  m_ss.Execute(NewSingleCallback(future, &ola::thread::Future<void>::Set));
  m_ss.Execute(NewSingleCallback(m_client.get(), &AvahiOlaClient::Start));
  m_ss.Run();

  if (m_controller_browser) {
    avahi_service_browser_free(m_controller_browser);
  }

  ola::STLDeleteValues(&m_registrations);

  m_client->Stop();
  m_client.reset();

  m_avahi_poll.reset();

  /*
  {
    MutexLocker lock(&m_controllers_mu);
    StopResolution();
  }
  */
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


void AvahiE133DiscoveryAgent::LocateControllerServices() {
  m_controller_browser = m_client->CreateServiceBrowser(
      AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC,
      E133_CONTROLLER_SERVICE, NULL,
      static_cast<AvahiLookupFlags>(0), browse_callback, this);
  if (!m_controller_browser) {
    OLA_WARN << "Failed to start browsing for " << E133_CONTROLLER_SERVICE
             << ": " << m_client->GetLastError();
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
    p.first->second = new ControllerRegistration(m_client.get());
  }
  ControllerRegistration *registration = p.first->second;
  registration->RegisterOrUpdate(controller);
}

void AvahiE133DiscoveryAgent::InternalDeRegisterService(
      ola::network::IPV4SocketAddress controller_address) {
  ola::STLRemoveAndDelete(&m_registrations, controller_address);
}
