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
// #include <ola/thread/Future.h>
#include <ola/thread/Mutex.h>
#include <ola/thread/CallbackThread.h>
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
  BonjourE133DiscoveryAgent();
  ~BonjourE133DiscoveryAgent();

  bool Init();

  bool Stop();

  bool FindControllers(BrowseCallback *callback);

  void FindControllers(std::vector<E133ControllerInfo> *controllers);

  void RunThread();

  void BrowseResult(DNSServiceFlags flags,
                    uint32_t interface_index,
                    const std::string &service_name,
                    const std::string &regtype,
                    const std::string &reply_domain);

 private:
  typedef std::vector<class ControllerResolver*> ControllerResolverList;

  ola::io::SelectServer m_ss;
  std::auto_ptr<ola::thread::CallbackThread> m_thread;
  std::auto_ptr<class IOAdapter> m_io_adapter;
  DNSServiceRef m_discovery_service_ref;

  ControllerResolverList m_controllers;
  ola::thread::Mutex m_controllers_mu;

  DISALLOW_COPY_AND_ASSIGN(BonjourE133DiscoveryAgent);
};
#endif  // TOOLS_E133_BONJOURDISCOVERYAGENT_H_
