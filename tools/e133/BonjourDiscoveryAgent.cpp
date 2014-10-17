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
#include <utility>

#include "tools/e133/BonjourIOAdapter.h"
#include "tools/e133/BonjourRegistration.h"
#include "tools/e133/BonjourResolver.h"

using ola::network::IPV4SocketAddress;
using ola::thread::MutexLocker;
using std::auto_ptr;
using std::string;

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

  agent->BrowseResult(service, flags, interface_index, service_name, regtype,
                      reply_domain);
}

// BonjourE133DiscoveryAgent
// ----------------------------------------------------------------------------
BonjourE133DiscoveryAgent::BonjourE133DiscoveryAgent(
    const E133DiscoveryAgentInterface::Options &options)
    : m_io_adapter(new BonjourIOAdapter(&m_ss)),
      m_find_controllers(options.include_controllers),
      m_find_distributors(options.include_distributors),
      m_controller_service_ref(NULL),
      m_distributor_service_ref(NULL),
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
  MutexLocker lock(&m_mutex);
  if (m_scope == scope) {
    return;
  }

  m_orphaned_controllers.reserve(
      m_orphaned_controllers.size() + m_controllers.size());
  copy(m_controllers.begin(), m_controllers.end(),
       back_inserter(m_orphaned_controllers));
  m_controllers.clear();

  m_orphaned_distributors.reserve(
      m_orphaned_distributors.size() + m_distributors.size());
  copy(m_distributors.begin(), m_distributors.end(),
       back_inserter(m_orphaned_distributors));
  m_distributors.clear();

  m_scope = scope;
  m_changing_scope = true;

  m_ss.Execute(ola::NewSingleCallback(
      this,
      &BonjourE133DiscoveryAgent::TriggerScopeChange,
      reinterpret_cast<ola::thread::Future<bool>*>(NULL)));
}

void BonjourE133DiscoveryAgent::FindControllers(
    ControllerEntryList *controllers) {
  MutexLocker lock(&m_mutex);

  ControllerResolverList::iterator iter = m_controllers.begin();
  for (; iter != m_controllers.end(); ++iter) {
    E133ControllerEntry controller_entry;
    if ((*iter)->GetControllerEntry(&controller_entry)) {
      controllers->push_back(controller_entry);
    }
  }
}

void BonjourE133DiscoveryAgent::RegisterController(
    const E133ControllerEntry &controller) {
  m_ss.Execute(ola::NewSingleCallback(
      this,
      &BonjourE133DiscoveryAgent::InternalRegisterController, controller));
}

void BonjourE133DiscoveryAgent::DeRegisterController(
      const ola::network::IPV4SocketAddress &controller_address) {
  m_ss.Execute(ola::NewSingleCallback(
      this, &BonjourE133DiscoveryAgent::InternalDeRegisterController,
      controller_address));
}

void BonjourE133DiscoveryAgent::FindDistributors(
    DistributorEntryList *distributors) {
  MutexLocker lock(&m_mutex);

  DistributorResolverList::iterator iter = m_distributors.begin();
  for (; iter != m_distributors.end(); ++iter) {
    E133DistributorEntry distributor_entry;
    if ((*iter)->GetDistributorEntry(&distributor_entry)) {
      distributors->push_back(distributor_entry);
    }
  }
}

void BonjourE133DiscoveryAgent::RegisterDistributor(
    const E133DistributorEntry &distributor) {
  m_ss.Execute(ola::NewSingleCallback(
      this,
      &BonjourE133DiscoveryAgent::InternalRegisterDistributor, distributor));
}

void BonjourE133DiscoveryAgent::DeRegisterDistributor(
    const IPV4SocketAddress &distributor_address) {
  m_ss.Execute(ola::NewSingleCallback(
      this, &BonjourE133DiscoveryAgent::InternalDeRegisterDistributor,
      distributor_address));
}

void BonjourE133DiscoveryAgent::BrowseResult(DNSServiceRef service_ref,
                                             DNSServiceFlags flags,
                                             uint32_t interface_index,
                                             const string &service_name,
                                             const string &regtype,
                                             const string &reply_domain) {
  MutexLocker lock(&m_mutex);
  if (m_changing_scope) {
    // We're in the middle of changing scopes so don't change m_controllers.
    return;
  }

  if (service_ref == m_controller_service_ref) {
    UpdateController(flags, interface_index, service_name, regtype,
                     reply_domain);
  } else if (service_ref == m_distributor_service_ref) {
    UpdateDistributor(flags, interface_index, service_name, regtype,
                      reply_domain);
  } else {
    OLA_WARN << "Unknown DNSServiceRef " << service_ref;
  }

}

void BonjourE133DiscoveryAgent::RunThread() {
  m_ss.Run();

  {
    MutexLocker lock(&m_mutex);
    StopResolution();
  }
}

void BonjourE133DiscoveryAgent::TriggerScopeChange(
    ola::thread::Future<bool> *f) {
  MutexLocker lock(&m_mutex);
  StopResolution();

  m_changing_scope = false;

  bool ret = true;

  if (m_find_controllers) {
    const string service_type = GenerateE133SubType(
        m_scope, E133_CONTROLLER_SERVICE);
    OLA_INFO << "Starting browse op " << service_type;
    DNSServiceErrorType error = DNSServiceBrowse(
        &m_controller_service_ref,
        0,
        kDNSServiceInterfaceIndexAny,
        service_type.c_str(),
        NULL,  // domain
        &BrowseServiceCallback,
        reinterpret_cast<void*>(this));

    if (error == kDNSServiceErr_NoError) {
      m_io_adapter->AddDescriptor(m_controller_service_ref);
    } else {
      OLA_WARN << "DNSServiceBrowse returned " << error;
      ret = false;
    }
  }

  if (m_find_distributors) {
    const string service_type = GenerateE133SubType(
        m_scope, E133_DISTRIBUTOR_SERVICE);
    OLA_INFO << "Starting browse op " << service_type;
    DNSServiceErrorType error = DNSServiceBrowse(
        &m_distributor_service_ref,
        0,
        kDNSServiceInterfaceIndexAny,
        service_type.c_str(),
        NULL,  // domain
        &BrowseServiceCallback,
        reinterpret_cast<void*>(this));

    if (error == kDNSServiceErr_NoError) {
      m_io_adapter->AddDescriptor(m_distributor_service_ref);
    } else {
      OLA_WARN << "DNSServiceBrowse returned " << error;
      ret = false;
    }
  }

  if (f) {
    f->Set(ret);
  }
}

void BonjourE133DiscoveryAgent::StopResolution() {
  // Tear down the existing resolution
  ola::STLDeleteElements(&m_controllers);
  ola::STLDeleteElements(&m_orphaned_controllers);
  ola::STLDeleteElements(&m_distributors);
  ola::STLDeleteElements(&m_orphaned_distributors);

  if (m_controller_service_ref) {
    m_io_adapter->RemoveDescriptor(m_controller_service_ref);
    DNSServiceRefDeallocate(m_controller_service_ref);
    m_controller_service_ref = NULL;
  }

  if (m_distributor_service_ref) {
    m_io_adapter->RemoveDescriptor(m_distributor_service_ref);
    DNSServiceRefDeallocate(m_distributor_service_ref);
    m_distributor_service_ref = NULL;
  }
}

void BonjourE133DiscoveryAgent::InternalRegisterController(
    E133ControllerEntry controller) {
  std::pair<ControllerRegistrationList::iterator, bool> p =
      m_controller_registrations.insert(
          ControllerRegistrationList::value_type(controller.address, NULL));

  if (p.first->second == NULL) {
    p.first->second = new ControllerRegistration(m_io_adapter.get());
  }
  ControllerRegistration *registration = p.first->second;
  registration->RegisterOrUpdate(controller);
}

void BonjourE133DiscoveryAgent::InternalDeRegisterController(
      ola::network::IPV4SocketAddress controller_address) {
  ola::STLRemoveAndDelete(&m_controller_registrations, controller_address);
}

void BonjourE133DiscoveryAgent::InternalRegisterDistributor(
    E133DistributorEntry distributor) {
  std::pair<DistributorRegistrationList::iterator, bool> p =
      m_distributor_registrations.insert(
          DistributorRegistrationList::value_type(distributor.address, NULL));

  if (p.first->second == NULL) {
    p.first->second = new DistributorRegistration(m_io_adapter.get());
  }
  DistributorRegistration *registration = p.first->second;
  registration->RegisterOrUpdate(distributor);
}

void BonjourE133DiscoveryAgent::InternalDeRegisterDistributor(
      ola::network::IPV4SocketAddress distributor_address) {
  ola::STLRemoveAndDelete(&m_distributor_registrations, distributor_address);
}

void BonjourE133DiscoveryAgent::UpdateController(
    DNSServiceFlags flags,
    uint32_t interface_index,
    const std::string &service_name,
    const std::string &regtype,
    const std::string &reply_domain) {
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

void BonjourE133DiscoveryAgent::UpdateDistributor(
    DNSServiceFlags flags,
    uint32_t interface_index,
    const std::string &service_name,
    const std::string &regtype,
    const std::string &reply_domain) {
  if (flags & kDNSServiceFlagsAdd) {
    DistributorResolver *distributor = new DistributorResolver(
        m_io_adapter.get(), interface_index, service_name, regtype,
        reply_domain);

    DNSServiceErrorType error = distributor->StartResolution();
    OLA_INFO << "Starting resolution for " << *distributor << ", ret was "
             << error;

    if (error == kDNSServiceErr_NoError) {
      m_distributors.push_back(distributor);
      OLA_INFO << "Added " << *distributor << " at " << m_distributors.back();
    } else {
      OLA_WARN << "Failed to start resolution for " << *distributor;
      delete distributor;
    }
  } else {
    DistributorResolver distributor(m_io_adapter.get(), interface_index,
                                  service_name, regtype, reply_domain);
    DistributorResolverList::iterator iter = m_distributors.begin();
    for (; iter != m_distributors.end(); ++iter) {
      if (**iter == distributor) {
        // Cancel DNSServiceRef.
        OLA_INFO << "Removed " << distributor << " at " << *iter;
        delete *iter;
        m_distributors.erase(iter);
        return;
      }
    }
    OLA_INFO << "Failed to find " << distributor;
  }
}
