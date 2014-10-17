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
 * BonjourE133DiscoveryAgent.h
 * The Bonjour implementation of DiscoveryAgentInterface.
 * Copyright (C) 2013 Simon Newton
 */

#ifndef TOOLS_E133_BONJOURDISCOVERYAGENT_H_
#define TOOLS_E133_BONJOURDISCOVERYAGENT_H_

#include <dns_sd.h>

#include <ola/base/Macro.h>
#include <ola/io/Descriptor.h>
#include <ola/io/SelectServer.h>
#include <ola/thread/CallbackThread.h>
#include <ola/thread/Future.h>
#include <ola/thread/Mutex.h>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "tools/e133/E133DiscoveryAgent.h"

/**
 * @brief An implementation of E133DiscoveryAgentInterface that uses the Apple
 * dns_sd.h library.
 */
class BonjourE133DiscoveryAgent : public E133DiscoveryAgentInterface {
 public:
  BonjourE133DiscoveryAgent(
      const E133DiscoveryAgentInterface::Options &options);
  ~BonjourE133DiscoveryAgent();

  bool Start();

  bool Stop();

  void SetScope(const std::string &scope);

  void FindControllers(ControllerEntryList *controllers);

  void RegisterController(const E133ControllerEntry &controller);

  void DeRegisterController(
      const ola::network::IPV4SocketAddress &controller_address);

  void FindDistributors(DistributorEntryList *distributors);

  void RegisterDistributor(const E133DistributorEntry &distributor);

  void DeRegisterDistributor(
      const ola::network::IPV4SocketAddress &distributor_address);

  /**
   * @brief Called by our static callback function when a new controller is
   * found.
   */
  void BrowseResult(DNSServiceRef service_ref,
                    DNSServiceFlags flags,
                    uint32_t interface_index,
                    const std::string &service_name,
                    const std::string &regtype,
                    const std::string &reply_domain);

 private:
  typedef std::vector<class ControllerResolver*> ControllerResolverList;
  typedef std::map<ola::network::IPV4SocketAddress,
                   class ControllerRegistration*> ControllerRegistrationList;

  typedef std::vector<class DistributorResolver*> DistributorResolverList;
  typedef std::map<ola::network::IPV4SocketAddress,
                   class DistributorRegistration*> DistributorRegistrationList;

  ola::io::SelectServer m_ss;
  std::auto_ptr<ola::thread::CallbackThread> m_thread;
  std::auto_ptr<class BonjourIOAdapter> m_io_adapter;
  const bool m_find_controllers;
  const bool m_find_distributors;

  // Controllers
  DNSServiceRef m_controller_service_ref;
  DNSServiceRef m_distributor_service_ref;

  // These are all protected by m_mutex
  ControllerResolverList m_controllers;
  ControllerResolverList m_orphaned_controllers;
  DistributorResolverList m_distributors;
  DistributorResolverList m_orphaned_distributors;

  std::string m_scope;
  bool m_changing_scope;
  // End protected by m_mutex

  ola::thread::Mutex m_mutex;

  ControllerRegistrationList m_controller_registrations;
  DistributorRegistrationList m_distributor_registrations;

  void RunThread();
  void TriggerScopeChange(ola::thread::Future<bool> *f);
  void StopResolution();

  void InternalRegisterController(E133ControllerEntry controller_entry);
  void InternalDeRegisterController(
      ola::network::IPV4SocketAddress controller_address);
  void InternalRegisterDistributor(E133DistributorEntry distributor_entry);
  void InternalDeRegisterDistributor(
      ola::network::IPV4SocketAddress distributor_address);
  void UpdateController(DNSServiceFlags flags,
                        uint32_t interface_index,
                        const std::string &service_name,
                        const std::string &regtype,
                        const std::string &reply_domain);
  void UpdateDistributor(DNSServiceFlags flags,
                         uint32_t interface_index,
                         const std::string &service_name,
                         const std::string &regtype,
                         const std::string &reply_domain);

  DISALLOW_COPY_AND_ASSIGN(BonjourE133DiscoveryAgent);
};
#endif  // TOOLS_E133_BONJOURDISCOVERYAGENT_H_
