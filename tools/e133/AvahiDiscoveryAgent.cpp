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

#include <algorithm>
#include <memory>
#include <string>
#include <utility>

#include "tools/e133/AvahiHelper.h"
#include "tools/e133/AvahiOlaPoll.h"

using ola::network::IPV4Address;
using ola::network::IPV4SocketAddress;
using ola::NewCallback;
using ola::NewSingleCallback;
using ola::thread::MutexLocker;
using std::auto_ptr;
using std::ostringstream;
using std::string;

// ControllerResolver
// ----------------------------------------------------------------------------
class ControllerResolver {
 public:
  ControllerResolver(AvahiOlaClient *client,
                     AvahiIfIndex interface_index,
                     AvahiProtocol protocol,
                     const std::string &service_name,
                     const std::string &type,
                     const std::string &domain);

  ~ControllerResolver();

  bool operator==(const ControllerResolver &other) const;

  std::string ToString() const;

  friend std::ostream& operator<<(std::ostream &out,
                                  const ControllerResolver &info) {
    return out << info.ToString();
  }

  bool StartResolution();

  bool GetControllerEntry(E133ControllerEntry *entry);

  void ResolveEvent(AvahiResolverEvent event,
                    const AvahiAddress *a,
                    uint16_t port,
                    AvahiStringList *txt);

 private:
  AvahiOlaClient *m_client;
  AvahiServiceResolver *m_resolver;

  const AvahiIfIndex m_interface_index;
  const AvahiProtocol m_protocol;
  const std::string m_service_name;
  const std::string m_type;
  const std::string m_domain;

  uint8_t m_priority;
  ola::network::IPV4SocketAddress m_resolved_address;
  std::string m_scope;
  ola::rdm::UID m_uid;
  std::string m_model;
  std::string m_manufacturer;

  bool ExtractString(AvahiStringList *txt_list,
                     const std::string &key,
                     std::string *dest);
  bool ExtractInt(AvahiStringList *txt_list,
                  const std::string &key, unsigned int *dest);
  bool CheckVersionMatches(
      AvahiStringList *txt_list,
      const string &key, unsigned int version);

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

  agent->BrowseEvent(interface, protocol, event, name, type, domain, flags);
  (void) b;
}

static void resolve_callback(AvahiServiceResolver *r,
                             AvahiIfIndex interface,
                             AvahiProtocol protocol,
                             AvahiResolverEvent event,
                             const char *name,
                             const char *type,
                             const char *domain,
                             const char *host_name,
                             const AvahiAddress *a,
                             uint16_t port,
                             AvahiStringList *txt,
                             AvahiLookupResultFlags flags,
                             void *userdata) {
  ControllerResolver *resolver =
    reinterpret_cast<ControllerResolver*>(userdata);
  resolver->ResolveEvent(event, a, port, txt);

  (void) r;
  (void) interface;
  (void) protocol;
  (void) name;
  (void) type;
  (void) domain;
  (void) host_name;
  (void) flags;
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
ControllerResolver::ControllerResolver(AvahiOlaClient *client,
                                       AvahiIfIndex interface_index,
                                       AvahiProtocol protocol,
                                       const std::string &service_name,
                                       const std::string &type,
                                       const std::string &domain)
    : m_client(client),
      m_resolver(NULL),
      m_interface_index(interface_index),
      m_protocol(protocol),
      m_service_name(service_name),
      m_type(type),
      m_domain(domain),
      m_uid(0, 0) {
}


ControllerResolver::~ControllerResolver() {
  if (m_resolver) {
    avahi_service_resolver_free(m_resolver);
    m_resolver = NULL;
  }
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

bool ControllerResolver::StartResolution() {
  if (m_resolver) {
    return true;
  }

  m_resolver = m_client->CreateServiceResolver(
      m_interface_index, m_protocol, m_service_name, m_type, m_domain,
        AVAHI_PROTO_INET, static_cast<AvahiLookupFlags>(0), resolve_callback,
      this);
  if (!m_resolver) {
    OLA_WARN << "Failed to start resolution for " << m_service_name << "."
             << m_type << ": " << m_client->GetLastError();
    return false;
  }
  return true;
}

bool ControllerResolver::GetControllerEntry(E133ControllerEntry *entry) {
  if (m_resolved_address.Host().IsWildcard()) {
    return false;
  }

  entry->service_name = m_service_name;
  entry->priority = m_priority;
  entry->scope = m_scope;
  entry->uid = m_uid;
  entry->model = m_model;
  entry->manufacturer = m_manufacturer;
  entry->address = m_resolved_address;
  return true;
}

void ControllerResolver::ResolveEvent(AvahiResolverEvent event,
                                      const AvahiAddress *address,
                                      uint16_t port,
                                      AvahiStringList *txt) {
  if (event == AVAHI_RESOLVER_FAILURE) {
    OLA_WARN << "Failed to resolve " << m_service_name << "." << m_type
             << ", proto: " << ProtoToString(m_protocol);
    return;
  }

  if (address->proto != AVAHI_PROTO_INET) {
    return;
  }

  if (!CheckVersionMatches(txt,
                           E133DiscoveryAgentInterface::TXT_VERSION_KEY,
                           E133DiscoveryAgentInterface::TXT_VERSION)) {
    return;
  }

  if (!CheckVersionMatches(txt,
                           E133DiscoveryAgentInterface::E133_VERSION_KEY,
                           E133DiscoveryAgentInterface::E133_VERSION)) {
    return;
  }

  unsigned int priority;
  if (!ExtractInt(txt, E133DiscoveryAgentInterface::PRIORITY_KEY, &priority)) {
    return;
  }

  if (!ExtractString(txt, E133DiscoveryAgentInterface::SCOPE_KEY, &m_scope)) {
    return;
  }

  // These are optional?
  string uid_str;
  if (ExtractString(txt, E133DiscoveryAgentInterface::UID_KEY, &uid_str)) {
    auto_ptr<ola::rdm::UID> uid(ola::rdm::UID::FromString(uid_str));
    if (uid.get()) {
      m_uid = *uid;
    }
  }

  ExtractString(txt, E133DiscoveryAgentInterface::MODEL_KEY, &m_model);
  ExtractString(txt, E133DiscoveryAgentInterface::MANUFACTURER_KEY,
                &m_manufacturer);

  m_priority = static_cast<uint8_t>(priority);
  m_resolved_address = IPV4SocketAddress(
      IPV4Address(address->data.ipv4.address), port);
}

bool ControllerResolver::ExtractString(AvahiStringList *txt_list,
                                           const std::string &key,
                                           std::string *dest) {
  AvahiStringList *entry = avahi_string_list_find(txt_list, key.c_str());
  if (!entry) {
    return false;
  }
  char *key_result = NULL;
  char *value = NULL;
  size_t length = 0;

  if (avahi_string_list_get_pair(entry, &key_result, &value, &length)) {
    OLA_WARN << "avahi_string_list_get_pair for " << key << " failed";
    return false;
  }

  if (key != string(key_result)) {
    OLA_WARN << "Mismatched key, " << key << " != " << string(key_result);
    avahi_free(key_result);
    avahi_free(value);
    return false;
  }

  *dest = string(value, length);
  avahi_free(key_result);
  avahi_free(value);
  return true;
}


bool ControllerResolver::ExtractInt(AvahiStringList *txt_list,
                                        const std::string &key,
                                        unsigned int *dest) {
  string value;
  if (!ExtractString(txt_list, key, &value))
    return false;

  if (!ola::StringToInt(value, dest)) {
    OLA_WARN << m_service_name << " has an invalid value of " << value
             << " for " << key;
    return false;
  }
  return true;
}

bool ControllerResolver::CheckVersionMatches(
    AvahiStringList *txt_list,
    const string &key, unsigned int expected_version) {
  unsigned int version;
  if (!ExtractInt(txt_list, key, &version)) {
    return false;
  }

  if (version != expected_version) {
    OLA_WARN << "Unknown version for " << key << " : " << version << " for "
             << m_service_name;
    return false;
  }
  return true;
}

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
    : m_controller_browser(NULL),
      m_scope(DEFAULT_SCOPE),
      m_changing_scope(false) {
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
}

void AvahiE133DiscoveryAgent::SetScope(const std::string &scope) {
  // We need to ensure that FindControllers only returns controllers in the new
  // scope. So we empty the list here and trigger a scope change in the DNS-SD
  // thread.
  MutexLocker lock(&m_controllers_mu);
  if (m_scope == scope) {
    return;
  }

  m_orphaned_controllers.reserve(
      m_orphaned_controllers.size() + m_controllers.size());
  copy(m_controllers.begin(), m_controllers.end(),
       back_inserter(m_orphaned_controllers));
  m_controllers.clear();
  m_scope = scope;
  m_changing_scope = true;

  m_ss.Execute(ola::NewSingleCallback(
      this,
      &AvahiE133DiscoveryAgent::TriggerScopeChange));
}

void AvahiE133DiscoveryAgent::FindControllers(
    ControllerEntryList *controllers) {
  MutexLocker lock(&m_controllers_mu);

  ControllerResolverList::iterator iter = m_controllers.begin();
  for (; iter != m_controllers.end(); ++iter) {
    E133ControllerEntry entry;
    if ((*iter)->GetControllerEntry(&entry)) {
      if (entry.scope != m_scope) {
        OLA_WARN << "Mismatched scope for " << entry;
      } else {
        controllers->push_back(entry);
      }
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
  if (state == AVAHI_CLIENT_S_RUNNING) {
    // The server has started successfully and registered its host
    // name on the network, so we can start locating the controllers.
    StartServiceBrowser();
    return;
  }

  MutexLocker lock(&m_controllers_mu);
  StopResolution();
}

void AvahiE133DiscoveryAgent::RunThread(ola::thread::Future<void> *future) {
  m_avahi_poll.reset(new AvahiOlaPoll(&m_ss));
  m_client.reset(new AvahiOlaClient(m_avahi_poll.get()));
  m_client->AddStateChangeListener(this);

  m_ss.Execute(NewSingleCallback(future, &ola::thread::Future<void>::Set));
  m_ss.Execute(NewSingleCallback(m_client.get(), &AvahiOlaClient::Start));
  m_ss.Run();

  m_client->RemoveStateChangeListener(this);

  {
    MutexLocker lock(&m_controllers_mu);
    StopResolution();
  }

  ola::STLDeleteValues(&m_registrations);

  m_client->Stop();
  m_client.reset();
  m_avahi_poll.reset();
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
      OLA_WARN << "(Browser) " << m_client->GetLastError();
      return;
    case AVAHI_BROWSER_NEW:
      AddController(interface, protocol, name, type, domain);
      break;
    case AVAHI_BROWSER_REMOVE:
      RemoveController(interface, protocol, name, type, domain);
      break;
    default:
      {}
  }
  (void) flags;
}

void AvahiE133DiscoveryAgent::StartServiceBrowser() {
  ostringstream service;
  {
    MutexLocker lock(&m_controllers_mu);
    service << "_" << m_scope;
  }
  service << "._sub." << E133_CONTROLLER_SERVICE;

  m_controller_browser = m_client->CreateServiceBrowser(
      AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC,
      service.str().c_str(), NULL,
      static_cast<AvahiLookupFlags>(0), browse_callback, this);
  if (!m_controller_browser) {
    OLA_WARN << "Failed to start browsing for " << E133_CONTROLLER_SERVICE
             << ": " << m_client->GetLastError();
  }
  OLA_INFO << "Started browsing for " << service.str();
}

void AvahiE133DiscoveryAgent::StopResolution() {
  // Tear down the existing resolution
  ola::STLDeleteElements(&m_controllers);
  ola::STLDeleteElements(&m_orphaned_controllers);

  if (m_controller_browser) {
    avahi_service_browser_free(m_controller_browser);
    m_controller_browser = NULL;
  }
}

void AvahiE133DiscoveryAgent::TriggerScopeChange() {
  {
    MutexLocker lock(&m_controllers_mu);
    StopResolution();
    m_changing_scope = false;
  }
  StartServiceBrowser();
}

void AvahiE133DiscoveryAgent::AddController(AvahiIfIndex interface,
                                            AvahiProtocol protocol,
                                            const std::string &name,
                                            const std::string &type,
                                            const std::string &domain) {
  OLA_INFO << "(Browser) NEW: service " << name << " of type " << type
           << " in domain " << domain << ", iface" << interface
           << ", proto " << protocol;

  MutexLocker lock(&m_controllers_mu);
  if (m_changing_scope) {
    // We're in the middle of changing scopes so don't change m_controllers.
    return;
  }

  auto_ptr<ControllerResolver> controller(new ControllerResolver(
      m_client.get(), interface, protocol, name, type, domain));

  // We get the callback multiple times for the same
  ControllerResolverList::iterator iter = m_controllers.begin();
  for (; iter != m_controllers.end(); ++iter) {
    if ((**iter) == *controller) {
      return;
    }
  }
  if (controller->StartResolution()) {
    m_controllers.push_back(controller.release());
  }
}

void AvahiE133DiscoveryAgent::RemoveController(AvahiIfIndex interface,
                                               AvahiProtocol protocol,
                                               const std::string &name,
                                               const std::string &type,
                                               const std::string &domain) {
  ControllerResolver controller(m_client.get(), interface, protocol, name,
                                type, domain);
  OLA_WARN << "Removing: " << controller;

  MutexLocker lock(&m_controllers_mu);

  if (m_changing_scope) {
    // We're in the middle of changing scopes so don't change m_controllers.
    return;
  }

  ControllerResolverList::iterator iter = m_controllers.begin();
  for (; iter != m_controllers.end(); ++iter) {
    if (**iter == controller) {
      delete *iter;
      m_controllers.erase(iter);
      return;
    }
  }
  OLA_INFO << "Failed to find " << controller;
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
