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

DECLARE_uint8(controller_priority);


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
  ControllerResolver(const ControllerResolver &other);
  ~ControllerResolver();

  bool operator==(const ControllerResolver &other) const;

  std::string ToString() const;

  friend std::ostream& operator<<(std::ostream &out,
                                  const ControllerResolver &info) {
    return out << info.ToString();
  }

  DNSServiceErrorType StartResolution();

  bool GetControllerResolver(E133ControllerEntry *controller_entry);

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
                                   uint16_t txtLen,
                                   const unsigned char *txtRecord,
                                   void *context) {
  ControllerResolver *controller_resolver =
      reinterpret_cast<ControllerResolver*>(context);
  controller_resolver->ResolveHandler(
      errorCode, hosttarget, NetworkToHost(port), txtLen, txtRecord);
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
  if (error_code != kDNSServiceErr_NoError) {
    OLA_WARN << "DNSServiceRegister for " << name << "." << type << domain
             << " returned error " << error_code;
  } else {
    OLA_INFO << "Registered: " << name << "." << type << domain;
  }
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


// IOAdapter
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
    E133ControllerEntry *controller_entry) {
  if (m_resolved_address.Host().IsWildcard()) {
    return false;
  }

  controller_entry->priority = m_priority;
  controller_entry->address = m_resolved_address;
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

// BonjourE133DiscoveryAgent
// ----------------------------------------------------------------------------
BonjourE133DiscoveryAgent::BonjourE133DiscoveryAgent()
    : m_io_adapter(new IOAdapter(&m_ss)),
      m_registration_ref(NULL) {
}

BonjourE133DiscoveryAgent::~BonjourE133DiscoveryAgent() {
  Stop();
}

bool BonjourE133DiscoveryAgent::Init() {
  // Probably want to pass a future to the thread here.
  m_thread.reset(new ola::thread::CallbackThread(ola::NewSingleCallback(
      this, &BonjourE133DiscoveryAgent::RunThread)));
  m_thread->Start();
  return true;
}

bool BonjourE133DiscoveryAgent::Stop() {
  if (m_thread.get() && m_thread->IsRunning()) {
    m_ss.Terminate();
    m_thread->Join();
    m_thread.reset();
  }

  if (m_registration_ref) {
    m_io_adapter->RemoveDescriptor(m_registration_ref);
    DNSServiceRefDeallocate(m_registration_ref);
    m_registration_ref = NULL;
  }

  /*
  ServiceRefs::iterator iter = m_refs.begin();
  for (; iter != m_refs.end(); ++iter) {
    m_ss.RemoveReadDescriptor(iter->descriptor);
    delete iter->descriptor;
    DNSServiceRefDeallocate(iter->service_ref);
  }
  */

  /*
  Future<bool> f;
  m_ss.Execute(NewSingleCallback(
      this,
      &BonjourE133DiscoveryAgent::InternalBrowseForService, service_type, callback,
      &f));
  return f.Get();
  */
  return true;
}

bool BonjourE133DiscoveryAgent::FindControllers(BrowseCallback *callback) {
  if (!callback) {
    return false;
  }

  MutexLocker lock(&m_controllers_mu);
  ControllerEntryList controllers;

  ControllerResolverList::iterator iter = m_controllers.begin();
  for (; iter != m_controllers.end(); ++iter) {
    E133ControllerEntry controller_entry;
    if ((*iter)->GetControllerResolver(&controller_entry)) {
      controllers.push_back(controller_entry);
    }
  }
  callback->Run(controllers);
  return true;
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
    const IPV4SocketAddress &controller) {
  m_ss.Execute(ola::NewSingleCallback(
      this,
      &BonjourE133DiscoveryAgent::InternalRegisterService, controller));
}

void BonjourE133DiscoveryAgent::RunThread() {
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

void BonjourE133DiscoveryAgent::BrowseResult(DNSServiceFlags flags,
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

void BonjourE133DiscoveryAgent::InternalRegisterService(
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
