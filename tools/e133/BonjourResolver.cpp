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
 * BonjourResolver.cpp
 * The Bonjour implementation of DiscoveryAgentInterface.
 * Copyright (C) 2013 Simon Newton
 */

#include "tools/e133/BonjourResolver.h"

#include <dns_sd.h>
#include <netinet/in.h>
#include <stdint.h>
#include <ola/Logging.h>
#include <ola/network/NetworkUtils.h>
#include <ola/StringUtils.h>

#include <string>

#include "tools/e133/BonjourIOAdapter.h"

using ola::network::IPV4Address;
using ola::network::IPV4SocketAddress;
using ola::network::NetworkToHost;
using std::auto_ptr;
using std::string;

const uint8_t ControllerResolver::DEFAULT_PRIORITY = 100;

// Static callbacks
static void ResolveServiceCallback(OLA_UNUSED DNSServiceRef sdRef,
                                   OLA_UNUSED DNSServiceFlags flags,
                                   OLA_UNUSED uint32_t interface_index,
                                   DNSServiceErrorType errorCode,
                                   OLA_UNUSED const char *fullname,
                                   const char *hosttarget,
                                   uint16_t port, /* In network byte order */
                                   uint16_t txt_length,
                                   const unsigned char *txt_data,
                                   void *context) {
  BonjourResolver *resolver = reinterpret_cast<BonjourResolver*>(context);
  resolver->ResolveHandler(
      errorCode, hosttarget, NetworkToHost(port), txt_length, txt_data);
}

static void ResolveAddressCallback(OLA_UNUSED DNSServiceRef sdRef,
                                   OLA_UNUSED DNSServiceFlags flags,
                                   OLA_UNUSED uint32_t interface_index,
                                   OLA_UNUSED DNSServiceErrorType errorCode,
                                   const char *hostname,
                                   OLA_UNUSED const struct sockaddr *address,
                                   OLA_UNUSED uint32_t ttl,
                                   void *context) {
  BonjourResolver *resolver = reinterpret_cast<BonjourResolver*>(context);

  if (address->sa_family != AF_INET) {
    OLA_WARN << "Got wrong address family for " << hostname << ", was "
             << address->sa_family;
    return;
  }

  const struct sockaddr_in *v4_addr =
      reinterpret_cast<const struct sockaddr_in*>(address);
  resolver->UpdateAddress(IPV4Address(v4_addr->sin_addr.s_addr));
}

// ControllerResolver
// ----------------------------------------------------------------------------
BonjourResolver::BonjourResolver(
    BonjourIOAdapter *io_adapter,
    uint32_t interface_index,
    const string &service_name,
    const string &regtype,
    const string &reply_domain)
    : m_io_adapter(io_adapter),
      m_resolve_in_progress(false),
      to_addr_in_progress(false),
      interface_index(interface_index),
      service_name(service_name),
      regtype(regtype),
      reply_domain(reply_domain) {
}

BonjourResolver::~BonjourResolver() {
  if (m_resolve_in_progress) {
    m_io_adapter->RemoveDescriptor(m_resolve_ref);
    DNSServiceRefDeallocate(m_resolve_ref);
  }

  if (to_addr_in_progress) {
    m_io_adapter->RemoveDescriptor(m_to_addr_ref);
    DNSServiceRefDeallocate(m_to_addr_ref);
  }
}

DNSServiceErrorType BonjourResolver::StartResolution() {
  if (m_resolve_in_progress) {
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
    m_resolve_in_progress = true;
    m_io_adapter->AddDescriptor(m_resolve_ref);
  }
  return error;
}

void BonjourResolver::ResolveHandler(
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

  if (!ExtractString(txt_length, txt_data,
                     E133DiscoveryAgentInterface::SCOPE_KEY, &m_scope)) {
    return;
  }

  if (!ProcessTxtData(txt_length, txt_data)) {
    return;
  }

  // These are optional?
  ExtractString(txt_length, txt_data, E133DiscoveryAgentInterface::MODEL_KEY,
                &m_model);
  ExtractString(txt_length, txt_data,
                E133DiscoveryAgentInterface::MANUFACTURER_KEY, &m_manufacturer);

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

bool BonjourResolver::ProcessTxtData(OLA_UNUSED uint16_t txt_length,
                                     OLA_UNUSED const unsigned char *txt_data) {
  return true;
}

bool BonjourResolver::ExtractString(
    uint16_t txt_length,
    const unsigned char *txt_data,
    const string &key, string *dest) {
  if (!TXTRecordContainsKey(txt_length, txt_data, key.c_str())) {
    OLA_WARN << ServiceName() << " is missing " << key
             << " from the TXT record";
    return false;
  }

  uint8_t value_length = 0;
  const void *value = TXTRecordGetValuePtr(
      txt_length, txt_data, key.c_str(), &value_length);
  if (value == NULL) {
    OLA_WARN << ServiceName() << " is missing a value for " << key
             << " from the TXT record";
  }
  dest->assign(reinterpret_cast<const char*>(value), value_length);
  return true;
}

bool BonjourResolver::ExtractInt(
    uint16_t txt_length,
    const unsigned char *txt_data,
    const string &key, unsigned int *dest) {
  string value;
  if (!ExtractString(txt_length, txt_data, key, &value))
    return false;

  if (!ola::StringToInt(value, dest)) {
    OLA_WARN << ServiceName() << " has an invalid value of " << value
             << " for " << key;
    return false;
  }
  return true;
}

bool BonjourResolver::CheckVersionMatches(
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

ControllerResolver::ControllerResolver(
    class BonjourIOAdapter *io_adapter,
    uint32_t interface_index,
    const std::string &service_name,
    const std::string &regtype,
    const std::string &reply_domain)
    : BonjourResolver(io_adapter, interface_index, service_name,
                      regtype, reply_domain),
      m_uid(0, 0) {
}

bool ControllerResolver::GetControllerEntry(
    E133ControllerEntry *controller_entry) const {
  IPV4SocketAddress resolved_address = ResolvedAddress();

  if (resolved_address.Host().IsWildcard()) {
    return false;
  }

  controller_entry->service_name = ServiceName();
  controller_entry->priority = m_priority;
  controller_entry->scope = Scope();
  controller_entry->uid = m_uid;
  controller_entry->model = Model();
  controller_entry->manufacturer = Manufacturer();
  controller_entry->address = resolved_address;
  return true;
}

bool ControllerResolver::ProcessTxtData(uint16_t txt_length,
                                        const unsigned char *txt_data) {
  unsigned int priority;
  if (!ExtractInt(txt_length, txt_data,
                  E133DiscoveryAgentInterface::PRIORITY_KEY, &priority)) {
    return false;
  }

  // These is optional?
  string uid_str;
  if (ExtractString(txt_length, txt_data, E133DiscoveryAgentInterface::UID_KEY,
                    &uid_str)) {
    auto_ptr<ola::rdm::UID> uid(ola::rdm::UID::FromString(uid_str));
    if (uid.get()) {
      m_uid = *uid;
    }
  }

  m_priority = static_cast<uint8_t>(priority);
  return true;
}

DistributorResolver::DistributorResolver(
    class BonjourIOAdapter *io_adapter,
    uint32_t interface_index,
    const std::string &service_name,
    const std::string &regtype,
    const std::string &reply_domain)
    : BonjourResolver(io_adapter, interface_index, service_name,
                      regtype, reply_domain) {
}

bool DistributorResolver::GetDistributorEntry(
    E133DistributorEntry *distributor_entry) const {
  IPV4SocketAddress resolved_address = ResolvedAddress();
  if (resolved_address.Host().IsWildcard()) {
    return false;
  }

  distributor_entry->service_name = ServiceName();
  distributor_entry->scope = Scope();
  distributor_entry->model = Model();
  distributor_entry->manufacturer = Manufacturer();
  distributor_entry->address = resolved_address;
  return true;
}
