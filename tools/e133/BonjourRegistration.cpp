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
 * BonjourRegistration.cpp
 * Handles DNS-SD registration.
 * Copyright (C) 2014 Simon Newton
 */

#include "tools/e133/BonjourRegistration.h"

#include <dns_sd.h>
#include <stdint.h>
#include <ola/Logging.h>
#include <ola/network/NetworkUtils.h>

#include <string>
#include <vector>

#include "tools/e133/BonjourIOAdapter.h"

using ola::network::HostToNetwork;
using ola::network::IPV4SocketAddress;
using std::auto_ptr;
using std::string;
using std::vector;
using std::ostringstream;

string GenerateE133SubType(const string &scope,
                           const string &service) {
  string service_type(service);
  if (!scope.empty()) {
    service_type.append(",_");
    service_type.append(scope);
  }
  return service_type;
}

// static callback functions
// ----------------------------------------------------------------------------
static void RegisterCallback(OLA_UNUSED DNSServiceRef service,
                             OLA_UNUSED DNSServiceFlags flags,
                             DNSServiceErrorType error_code,
                             const char *name,
                             const char *type,
                             const char *domain,
                             OLA_UNUSED void *context) {
  ControllerRegistration *controller_registration =
      reinterpret_cast<ControllerRegistration*>(context);
  controller_registration->RegisterEvent(error_code, name, type, domain);
}

// ControllerRegistration
// ----------------------------------------------------------------------------

BonjourRegistration::~BonjourRegistration() {
  CancelRegistration();
}

bool BonjourRegistration::RegisterOrUpdateInternal(
    const string &service_type,
    const string &scope,
    const string &service_name,
    const IPV4SocketAddress &address,
    const string &txt_data) {
  if (m_registration_ref) {
    // This is an update.
    if (m_last_txt_data == txt_data) {
      return true;
    }

    OLA_INFO << "Updating controller registration for " << address;
    // If the scope isn't changing, this is just an update.
    if (scope == m_scope) {
      return UpdateRecord(txt_data);
    }

    // Otherwise we need to cancel this registration and continue with the new
    // one.
    CancelRegistration();
  }

  const string sub_service_type = GenerateE133SubType(scope, service_type);

  OLA_INFO << "Adding " << service_name << " : '"
           << sub_service_type << "' :" << address.Port();
  DNSServiceErrorType error = DNSServiceRegister(
      &m_registration_ref,
      0, 0,
      service_name.c_str(),
      sub_service_type.c_str(),
      NULL,  // default domain
      NULL,  // use default host name
      HostToNetwork(address.Port()),
      txt_data.size(), txt_data.c_str(),
      &RegisterCallback,  // call back function
      this);  // no context

  if (error != kDNSServiceErr_NoError) {
    OLA_WARN << "DNSServiceRegister returned " << error;
    return false;
  }

  m_last_txt_data = txt_data;
  m_scope = scope;
  m_io_adapter->AddDescriptor(m_registration_ref);
  return true;
}

void BonjourRegistration::RegisterEvent(
    DNSServiceErrorType error_code, const std::string &name,
    const std::string &type, const std::string &domain) {
  if (error_code != kDNSServiceErr_NoError) {
    OLA_WARN << "DNSServiceRegister for " << name << "." << type << domain
             << " returned error " << error_code;
  } else {
    OLA_INFO << "Registered: " << name << "." << type << domain;
  }
}

void BonjourRegistration::CancelRegistration() {
  if (m_registration_ref) {
    m_io_adapter->RemoveDescriptor(m_registration_ref);
    DNSServiceRefDeallocate(m_registration_ref);
    m_registration_ref = NULL;
  }
}

bool BonjourRegistration::UpdateRecord(const string &txt_data) {
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

string BonjourRegistration::BuildTxtString(const vector<string> &records) {
  string txt_data;
  vector<string>::const_iterator iter = records.begin();
  for (; iter != records.end(); ++iter) {
    txt_data.append(1, static_cast<char>(iter->size()));
    txt_data.append(*iter);
  }
  return txt_data;
}

bool ControllerRegistration::RegisterOrUpdate(
    const E133ControllerEntry &controller) {
  OLA_INFO << "Controller name is " << controller.ServiceName();
  return RegisterOrUpdateInternal(
      E133DiscoveryAgentInterface::E133_CONTROLLER_SERVICE,
      controller.scope,
      controller.ServiceName(),
      controller.address,
      BuildTxtRecord(controller));
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
  return BuildTxtString(records);
}

bool DistributorRegistration::RegisterOrUpdate(
    const E133DistributorEntry &distributor) {
  OLA_INFO << "Distributor name is " << distributor.ServiceName();

  return RegisterOrUpdateInternal(
      E133DiscoveryAgentInterface::E133_DISTRIBUTOR_SERVICE,
      distributor.scope,
      distributor.ServiceName(),
      distributor.address,
      BuildTxtRecord(distributor));
}

string DistributorRegistration::BuildTxtRecord(
    const E133DistributorEntry &distributor) {
  ostringstream str;
  vector<string> records;

  str << E133DiscoveryAgentInterface::TXT_VERSION_KEY << "="
      << static_cast<int>(E133DiscoveryAgentInterface::TXT_VERSION);
  records.push_back(str.str());
  str.str("");

  str << E133DiscoveryAgentInterface::SCOPE_KEY << "=" << distributor.scope;
  records.push_back(str.str());
  str.str("");

  str << E133DiscoveryAgentInterface::E133_VERSION_KEY << "="
      << static_cast<int>(distributor.e133_version);
  records.push_back(str.str());
  str.str("");

  if (!distributor.model.empty()) {
    str << E133DiscoveryAgentInterface::MODEL_KEY << "=" << distributor.model;
    records.push_back(str.str());
    str.str("");
  }

  if (!distributor.manufacturer.empty()) {
    str << E133DiscoveryAgentInterface::MANUFACTURER_KEY << "="
        << distributor.manufacturer;
    records.push_back(str.str());
    str.str("");
  }
  return BuildTxtString(records);
}
