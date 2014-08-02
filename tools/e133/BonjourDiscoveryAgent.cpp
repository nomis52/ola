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
 * BonjourE133DiscoveryAgent.cpp
 * The Bonjour implementation of DiscoveryAgentInterface.
 * Copyright (C) 2013 Simon Newton
 */

#define __STDC_LIMIT_MACROS  // for UINT8_MAX & friends

#include "tools/e133/BonjourDiscoveryAgent.h"

#include <dns_sd.h>
#include <netinet/in.h>
#include <stdint.h>
#include <ola/Callback.h>
#include <ola/base/Flags.h>
#include <ola/Logging.h>
#include <ola/network/NetworkUtils.h>
#include <ola/stl/STLUtils.h>
#include <ola/thread/CallbackThread.h>

#include <algorithm>
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
using std::auto_ptr;
using std::string;
using std::vector;
using std::ostringstream;

std::string GenerateE133SubType(const std::string &scope) {
  string service_type(E133DiscoveryAgentInterface::E133_CONTROLLER_SERVICE);
  if (!scope.empty()) {
    service_type.append(",_");
    service_type.append(scope);
  }
  return service_type;
}

// ControllerResolver
// ----------------------------------------------------------------------------
class ControllerResolver {
 public:
  ControllerResolver(
      class IOAdapter *io_adapter,
      uint32_t interface_index,
      const std::string &service_name,
      const std::string &regtype,
      const std::string &reply_domain);
  ~ControllerResolver();

  bool operator==(const ControllerResolver &other) const {
    return (interface_index == other.interface_index &&
            service_name == other.service_name &&
            regtype == other.regtype &&
            reply_domain == other.reply_domain);
  }

  std::string ToString() const {
    std::ostringstream str;
    str << service_name << "." << regtype << reply_domain << " on iface "
        << interface_index;
    return str.str();
  }

  friend std::ostream& operator<<(std::ostream &out,
                                  const ControllerResolver &info) {
    return out << info.ToString();
  }

  DNSServiceErrorType StartResolution();

  bool GetControllerResolver(E133ControllerEntry *controller_entry) const;

  void ResolveHandler(
      DNSServiceErrorType errorCode,
      const string &host_target,
      uint16_t port,
      uint16_t txt_length,
      const unsigned char *txt_data);

  void UpdateAddress(const IPV4Address &v4_address) {
    m_resolved_address.Host(v4_address);
  }

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
  std::string m_scope;
  ola::rdm::UID m_uid;
  std::string m_model;
  std::string m_manufacturer;

  ola::network::IPV4SocketAddress m_resolved_address;

  bool ExtractString(uint16_t txt_length,
                     const unsigned char *txt_data,
                     const std::string &key,
                     std::string *dest);
  bool ExtractInt(uint16_t txt_length,
                  const unsigned char *txt_data,
                  const std::string &key, unsigned int *dest);
  bool CheckVersionMatches(
    uint16_t txt_length,
    const unsigned char *txt_data,
    const string &key, unsigned int version);

  static const uint8_t DEFAULT_PRIORITY;

  DISALLOW_COPY_AND_ASSIGN(ControllerResolver);
};

const uint8_t ControllerResolver::DEFAULT_PRIORITY = 100;

// ControllerRegistration
// ----------------------------------------------------------------------------
class ControllerRegistration {
 public:
  explicit ControllerRegistration(class IOAdapter *io_adapter)
      : m_io_adapter(io_adapter),
        m_registration_ref(NULL) {
  }

  ~ControllerRegistration();

  bool RegisterOrUpdate(const E133ControllerEntry &controller);

  void RegisterEvent(DNSServiceErrorType error_code, const std::string &name,
                     const std::string &type, const std::string &domain);

 private:
  class IOAdapter *m_io_adapter;
  string m_scope;
  string m_last_txt_data;
  DNSServiceRef m_registration_ref;

  void CancelRegistration();
  bool UpdateRecord(const std::string &txt_data);

  // TODO(simon): Depend on what the Avahi API is we may want to move this to a
  // common function.
  std::string BuildTxtRecord(const E133ControllerEntry &controller);

  DISALLOW_COPY_AND_ASSIGN(ControllerRegistration);
};

// static callback functions
// ----------------------------------------------------------------------------
static void BrowseServiceCallback(DNSServiceRef service,
                                  DNSServiceFlags flags,
                                  uint32_t interface_index,
                                  DNSServiceErrorType error_code,
                                  const char *service_name,
                                  const char *regtype,
                                  const char *reply_domain,
                                  void *context) {
  BonjourE133DiscoveryAgent *agent =
      reinterpret_cast<BonjourE133DiscoveryAgent*>(context);

  if (error_code != kDNSServiceErr_NoError) {
    OLA_WARN << "DNSServiceBrowse returned error " << error_code;
    return;
  }

  agent->BrowseResult(flags, interface_index, service_name, regtype,
                      reply_domain);
  (void) service;
}

static void ResolveServiceCallback(DNSServiceRef sdRef,
                                   DNSServiceFlags flags,
                                   uint32_t interface_index,
                                   DNSServiceErrorType errorCode,
                                   const char *fullname,
                                   const char *hosttarget,
                                   uint16_t port, /* In network byte order */
                                   uint16_t txt_length,
                                   const unsigned char *txt_data,
                                   void *context) {
  ControllerResolver *controller_resolver =
      reinterpret_cast<ControllerResolver*>(context);
  controller_resolver->ResolveHandler(
      errorCode, hosttarget, NetworkToHost(port), txt_length, txt_data);
  (void) sdRef;
  (void) flags;
  (void) fullname;
  (void) interface_index;
}

static void ResolveAddressCallback(DNSServiceRef sdRef,
                                   DNSServiceFlags flags,
                                   uint32_t interface_index,
                                   DNSServiceErrorType errorCode,
                                   const char *hostname,
                                   const struct sockaddr *address,
                                   uint32_t ttl,
                                   void *context) {
  ControllerResolver *controller_resolver =
      reinterpret_cast<ControllerResolver*>(context);

  if (address->sa_family != AF_INET) {
    OLA_WARN << "Got wrong address family for " << hostname << ", was "
             << address->sa_family;
    return;
  }

  const struct sockaddr_in *v4_addr =
      reinterpret_cast<const struct sockaddr_in*>(address);
  controller_resolver->UpdateAddress(IPV4Address(v4_addr->sin_addr.s_addr));

  (void) errorCode;
  (void) sdRef;
  (void) flags;
  (void) interface_index;
  (void) address;
  (void) ttl;
}

static void RegisterCallback(DNSServiceRef service,
                             DNSServiceFlags flags,
                             DNSServiceErrorType error_code,
                             const char *name,
                             const char *type,
                             const char *domain,
                             void *context) {
  ControllerRegistration *controller_registration =
      reinterpret_cast<ControllerRegistration*>(context);
  controller_registration->RegisterEvent(error_code, name, type, domain);
  (void) service;
  (void) flags;
  (void) context;
}

// DNSSDDescriptor
// ----------------------------------------------------------------------------
class DNSSDDescriptor : public ola::io::ReadFileDescriptor {
 public:
  explicit DNSSDDescriptor(DNSServiceRef service_ref)
      : m_service_ref(service_ref),
        m_ref_count(0) {
  }

  int ReadDescriptor() const {
    return DNSServiceRefSockFD(m_service_ref);
  }

  void Ref() {
    m_ref_count++;
  }

  /**
   * @brief Deref this descriptor.
   * @returns true if the descriptor is still in use, false if it's no longer
   *   used.
   */
  bool DeRef() {
    if (m_ref_count) {
      m_ref_count--;
    }
    return m_ref_count != 0;
  }

  void PerformRead();

 private:
  DNSServiceRef m_service_ref;
  unsigned int m_ref_count;
};

void DNSSDDescriptor::PerformRead() {
  DNSServiceErrorType error = DNSServiceProcessResult(m_service_ref);
  if (error != kDNSServiceErr_NoError) {
    // TODO(simon): Consider de-registering from the ss here?
    OLA_FATAL << "DNSServiceProcessResult returned " << error;
  }
}

// IOAdapter
// ----------------------------------------------------------------------------
class IOAdapter {
 public:
  explicit IOAdapter(SelectServerInterface *ss)
      : m_ss(ss) {
  }

  void AddDescriptor(DNSServiceRef service_ref);
  void RemoveDescriptor(DNSServiceRef service_ref);

 private:
  typedef std::map<int, class DNSSDDescriptor*> DescriptorMap;

  DescriptorMap m_descriptors;
  SelectServerInterface *m_ss;

  DISALLOW_COPY_AND_ASSIGN(IOAdapter);
};

void IOAdapter::AddDescriptor(DNSServiceRef service_ref) {
  int fd = DNSServiceRefSockFD(service_ref);

  std::pair<DescriptorMap::iterator, bool> p = m_descriptors.insert(
      DescriptorMap::value_type(fd, NULL));

  if (p.first->second) {
    // Descriptor exists, increment the ref count.
    p.first->second->Ref();
    return;
  }

  p.first->second = new DNSSDDescriptor(service_ref);
  p.first->second->Ref();
  m_ss->AddReadDescriptor(p.first->second);
}

void IOAdapter::RemoveDescriptor(DNSServiceRef service_ref) {
  int fd = DNSServiceRefSockFD(service_ref);
  DescriptorMap::iterator iter = m_descriptors.find(fd);
  if (iter == m_descriptors.end()) {
    OLA_FATAL << "Missing FD " << fd << " in descriptor map";
    return;
  }

  if (iter->second->DeRef()) {
    return;
  }
  // RefCount is 0
  m_ss->RemoveReadDescriptor(iter->second);
  delete iter->second;
  m_descriptors.erase(iter);
}

// ControllerResolver
// ----------------------------------------------------------------------------
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
      reply_domain(reply_domain),
      m_uid(0, 0) {
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
    E133ControllerEntry *controller_entry) const {
  if (m_resolved_address.Host().IsWildcard()) {
    return false;
  }

  controller_entry->service_name = service_name;
  controller_entry->priority = m_priority;
  controller_entry->scope = m_scope;
  controller_entry->uid = m_uid;
  controller_entry->model = m_model;
  controller_entry->manufacturer = m_manufacturer;
  controller_entry->address = m_resolved_address;
  return true;
}

void ControllerResolver::ResolveHandler(
    DNSServiceErrorType errorCode,
    const string &host_target,
    uint16_t port,
    uint16_t txt_length,
    const unsigned char *txt_data) {
  if (errorCode != kDNSServiceErr_NoError) {
    OLA_WARN << "Failed to resolve " << this->ToString();
    return;
  }

  OLA_INFO << "Got resolv response " << host_target << ":" << port;

  if (!CheckVersionMatches(txt_length, txt_data,
                           E133DiscoveryAgentInterface::TXT_VERSION_KEY,
                           E133DiscoveryAgentInterface::TXT_VERSION)) {
    return;
  }

  if (!CheckVersionMatches(txt_length, txt_data,
                           E133DiscoveryAgentInterface::E133_VERSION_KEY,
                           E133DiscoveryAgentInterface::E133_VERSION)) {
    return;
  }

  unsigned int priority;
  if (!ExtractInt(txt_length, txt_data,
                  E133DiscoveryAgentInterface::PRIORITY_KEY, &priority)) {
    return;
  }

  if (!ExtractString(txt_length, txt_data,
                     E133DiscoveryAgentInterface::SCOPE_KEY, &m_scope)) {
    return;
  }

  // These are optional?
  string uid_str;
  if (ExtractString(txt_length, txt_data, E133DiscoveryAgentInterface::UID_KEY,
                    &uid_str)) {
    auto_ptr<ola::rdm::UID> uid(ola::rdm::UID::FromString(uid_str));
    if (uid.get()) {
      m_uid = *uid;
    }
  }

  ExtractString(txt_length, txt_data, E133DiscoveryAgentInterface::MODEL_KEY,
                &m_model);
  ExtractString(txt_length, txt_data,
                E133DiscoveryAgentInterface::MANUFACTURER_KEY, &m_manufacturer);

  m_priority = static_cast<uint8_t>(priority);
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

bool ControllerResolver::ExtractString(
    uint16_t txt_length,
    const unsigned char *txt_data,
    const string &key, string *dest) {
  if (!TXTRecordContainsKey(txt_length, txt_data, key.c_str())) {
    OLA_WARN << service_name << " is missing " << key << " from the TXT record";
    return false;
  }

  uint8_t value_length = 0;
  const void *value = TXTRecordGetValuePtr(
      txt_length, txt_data, key.c_str(), &value_length);
  if (value == NULL) {
    OLA_WARN << service_name << " is missing a value for " << key
             << " from the TXT record";
  }
  dest->assign(reinterpret_cast<const char*>(value), value_length);
  return true;
}

bool ControllerResolver::ExtractInt(
    uint16_t txt_length,
    const unsigned char *txt_data,
    const string &key, unsigned int *dest) {
  string value;
  if (!ExtractString(txt_length, txt_data, key, &value))
    return false;

  if (!ola::StringToInt(value, dest)) {
    OLA_WARN << service_name << " has an invalid value of " << value
             << " for " << key;
    return false;
  }
  return true;
}

bool ControllerResolver::CheckVersionMatches(
    uint16_t txt_length,
    const unsigned char *txt_data,
    const string &key, unsigned int expected_version) {
  unsigned int version;
  if (!ExtractInt(txt_length, txt_data, key, &version)) {
    return false;
  }

  if (version != expected_version) {
    OLA_WARN << "Unknown version for " << key << " : " << version << " for "
             << service_name;
    return false;
  }
  return true;
}

// ControllerRegistration
// ----------------------------------------------------------------------------

ControllerRegistration::~ControllerRegistration() {
  CancelRegistration();
}

bool ControllerRegistration::RegisterOrUpdate(
    const E133ControllerEntry &controller) {
  if (m_registration_ref) {
    // This is an update.
    const string txt_data = BuildTxtRecord(controller);
    if (m_last_txt_data == txt_data) {
      return true;
    }

    OLA_INFO << "Updating controller registration for " << controller.address;
    // If the scope isn't changing, this is just an update.
    if (controller.scope == m_scope) {
      return UpdateRecord(txt_data);
    }

    // Otherwise we need to cancel this registration and continue with the new
    // one.
    CancelRegistration();
  }

  string service_name;
  if (controller.service_name.empty()) {
    ostringstream str;
    str << "OLA Controller " << controller.address.Port();
    service_name = str.str();
    str.str("");
  } else {
    service_name = controller.service_name;
  }

  const string txt_data = BuildTxtRecord(controller);

  string service_type = GenerateE133SubType(controller.scope);

  OLA_INFO << "Adding " << service_name << " : "
           << service_type << ":" << controller.address.Port();
  DNSServiceErrorType error = DNSServiceRegister(
      &m_registration_ref,
      0, 0, service_name.c_str(),
      service_type.c_str(),
      NULL,  // default domain
      NULL,  // use default host name
      HostToNetwork(controller.address.Port()),
      txt_data.size(), txt_data.c_str(),
      &RegisterCallback,  // call back function
      this);  // no context

  if (error != kDNSServiceErr_NoError) {
    OLA_WARN << "DNSServiceRegister returned " << error;
    return false;
  }

  m_last_txt_data = txt_data;
  m_scope = controller.scope;
  m_io_adapter->AddDescriptor(m_registration_ref);
  return true;
}

void ControllerRegistration::RegisterEvent(
    DNSServiceErrorType error_code, const std::string &name,
    const std::string &type, const std::string &domain) {
  if (error_code != kDNSServiceErr_NoError) {
    OLA_WARN << "DNSServiceRegister for " << name << "." << type << domain
             << " returned error " << error_code;
  } else {
    OLA_INFO << "Registered: " << name << "." << type << domain;
  }
}

void ControllerRegistration::CancelRegistration() {
  if (m_registration_ref) {
    m_io_adapter->RemoveDescriptor(m_registration_ref);
    DNSServiceRefDeallocate(m_registration_ref);
    m_registration_ref = NULL;
  }
}

bool ControllerRegistration::UpdateRecord(const string &txt_data) {
  // Update required
  DNSServiceErrorType error = DNSServiceUpdateRecord(
      m_registration_ref, NULL,
      0, txt_data.size(), txt_data.c_str(), 0);
  if (error != kDNSServiceErr_NoError) {
    OLA_WARN << "DNSServiceUpdateRecord returned " << error;
    return false;
  }
  m_last_txt_data = txt_data;
  return true;
}

string ControllerRegistration::BuildTxtRecord(
    const E133ControllerEntry &controller) {
  ostringstream str;
  vector<string> records;

  str << E133DiscoveryAgentInterface::TXT_VERSION_KEY << "="
      << static_cast<int>(E133DiscoveryAgentInterface::TXT_VERSION);
  records.push_back(str.str());
  str.str("");

  str << E133DiscoveryAgentInterface::PRIORITY_KEY << "="
      << static_cast<int>(controller.priority);
  records.push_back(str.str());
  str.str("");

  str << E133DiscoveryAgentInterface::SCOPE_KEY << "="
      << controller.scope;
  records.push_back(str.str());
  str.str("");

  str << E133DiscoveryAgentInterface::E133_VERSION_KEY << "="
      << static_cast<int>(controller.e133_version);
  records.push_back(str.str());
  str.str("");

  if (controller.uid.ManufacturerId() != 0 && controller.uid.DeviceId() != 0) {
    str << E133DiscoveryAgentInterface::UID_KEY << "=" << controller.uid;
    records.push_back(str.str());
    str.str("");
  }

  if (!controller.model.empty()) {
    str << E133DiscoveryAgentInterface::MODEL_KEY << "=" << controller.model;
    records.push_back(str.str());
    str.str("");
  }

  if (!controller.manufacturer.empty()) {
    str << E133DiscoveryAgentInterface::MANUFACTURER_KEY << "="
        << controller.manufacturer;
    records.push_back(str.str());
    str.str("");
  }

  string txt_data;
  vector<string>::const_iterator iter = records.begin();
  for (; iter != records.end(); ++iter) {
    txt_data.append(1, static_cast<char>(iter->size()));
    txt_data.append(*iter);
  }
  return txt_data;
}

// BonjourE133DiscoveryAgent
// ----------------------------------------------------------------------------
BonjourE133DiscoveryAgent::BonjourE133DiscoveryAgent()
    : m_io_adapter(new IOAdapter(&m_ss)),
      m_discovery_service_ref(NULL),
      m_scope(DEFAULT_SCOPE),
      m_changing_scope(false) {
}

BonjourE133DiscoveryAgent::~BonjourE133DiscoveryAgent() {
  Stop();
}

bool BonjourE133DiscoveryAgent::Start() {
  ola::thread::Future<bool> f;

  m_ss.Execute(ola::NewSingleCallback(
      this,
      &BonjourE133DiscoveryAgent::TriggerScopeChange, &f));

  m_thread.reset(new ola::thread::CallbackThread(ola::NewSingleCallback(
      this, &BonjourE133DiscoveryAgent::RunThread)));
  m_thread->Start();

  bool ok = f.Get();
  if (!ok) {
    Stop();
  }
  return ok;
}

bool BonjourE133DiscoveryAgent::Stop() {
  if (m_thread.get() && m_thread->IsRunning()) {
    m_ss.Terminate();
    m_thread->Join();
    m_thread.reset();
  }
  return true;
}

void BonjourE133DiscoveryAgent::SetScope(const std::string &scope) {
  // We need to ensure that FindControllers only returns controllers in the new
  // scope. So we empty the list here and trigger a scope change in the DNS-SD
  // thread.
  MutexLocker lock(&m_controllers_mu);
  m_orphaned_controllers.reserve(
      m_orphaned_controllers.size() + m_controllers.size());
  copy(m_controllers.begin(), m_controllers.end(),
       back_inserter(m_orphaned_controllers));
  m_controllers.clear();
  m_scope = scope;
  m_changing_scope = true;

  m_ss.Execute(ola::NewSingleCallback(
      this,
      &BonjourE133DiscoveryAgent::TriggerScopeChange,
      reinterpret_cast<ola::thread::Future<bool>*>(NULL)));
}

void BonjourE133DiscoveryAgent::FindControllers(
    ControllerEntryList *controllers) {
  MutexLocker lock(&m_controllers_mu);

  ControllerResolverList::iterator iter = m_controllers.begin();
  for (; iter != m_controllers.end(); ++iter) {
    E133ControllerEntry controller_entry;
    if ((*iter)->GetControllerResolver(&controller_entry)) {
      controllers->push_back(controller_entry);
    }
  }
}

void BonjourE133DiscoveryAgent::RegisterController(
    const E133ControllerEntry &controller) {
  m_ss.Execute(ola::NewSingleCallback(
      this,
      &BonjourE133DiscoveryAgent::InternalRegisterService, controller));
}

void BonjourE133DiscoveryAgent::DeRegisterController(
      const ola::network::IPV4SocketAddress &controller_address) {
  m_ss.Execute(ola::NewSingleCallback(
      this, &BonjourE133DiscoveryAgent::InternalDeRegisterService,
      controller_address));
}

void BonjourE133DiscoveryAgent::BrowseResult(DNSServiceFlags flags,
                                             uint32_t interface_index,
                                             const string &service_name,
                                             const string &regtype,
                                             const string &reply_domain) {
  MutexLocker lock(&m_controllers_mu);
  if (m_changing_scope) {
    // We're in the middle of changing scopes so don't change m_controllers.
    return;
  }

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

void BonjourE133DiscoveryAgent::RunThread() {
  OLA_INFO << "Starting Discovery thread";
  m_ss.Run();

  {
    MutexLocker lock(&m_controllers_mu);
    StopResolution();
  }
  OLA_INFO << "Done with discovery thread";
}

void BonjourE133DiscoveryAgent::TriggerScopeChange(
    ola::thread::Future<bool> *f) {
  MutexLocker lock(&m_controllers_mu);
  StopResolution();

  m_changing_scope = false;

  string service_type = GenerateE133SubType(m_scope);
  OLA_INFO << "Starting browse op " << service_type;
  DNSServiceErrorType error = DNSServiceBrowse(
      &m_discovery_service_ref,
      0,
      kDNSServiceInterfaceIndexAny,
      service_type.c_str(),
      NULL,  // domain
      &BrowseServiceCallback,
      reinterpret_cast<void*>(this));

  if (error != kDNSServiceErr_NoError) {
    OLA_WARN << "DNSServiceBrowse returned " << error;
    if (f) {
      f->Set(false);
    }
    return;
  }

  if (f) {
    f->Set(true);
  }

  m_io_adapter->AddDescriptor(m_discovery_service_ref);
}

void BonjourE133DiscoveryAgent::StopResolution() {
  // Tear down the existing resolution
  ola::STLDeleteElements(&m_controllers);
  ola::STLDeleteElements(&m_orphaned_controllers);

  if (m_discovery_service_ref) {
    m_io_adapter->RemoveDescriptor(m_discovery_service_ref);
    DNSServiceRefDeallocate(m_discovery_service_ref);
    m_discovery_service_ref = NULL;
  }
}

void BonjourE133DiscoveryAgent::InternalRegisterService(
    E133ControllerEntry controller) {
  std::pair<ControllerRegistrationList::iterator, bool> p =
      m_registrations.insert(
          ControllerRegistrationList::value_type(controller.address, NULL));

  if (p.first->second == NULL) {
    p.first->second = new ControllerRegistration(m_io_adapter.get());
  }
  ControllerRegistration *registration = p.first->second;
  registration->RegisterOrUpdate(controller);
}

void BonjourE133DiscoveryAgent::InternalDeRegisterService(
      ola::network::IPV4SocketAddress controller_address) {
  ola::STLRemoveAndDelete(&m_registrations, controller_address);
}
