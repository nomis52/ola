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
 * DiscoveryAgent.h
 * The Interface for DNS-SD Discovery.
 * Copyright (C) 2013 Simon Newton
 */

#ifndef TOOLS_E133_E133DISCOVERYAGENT_H_
#define TOOLS_E133_E133DISCOVERYAGENT_H_

#include <stdint.h>
#include <ola/base/Macro.h>
#include <ola/Callback.h>
#include <ola/network/SocketAddress.h>
#include <vector>

/**
 * @brief The interface to DNS-SD operations like register, browse etc.
 */
class E133DiscoveryAgentInterface {
 public:
  struct E133ControllerInfo {
    ola::network::IPV4SocketAddress address;
    uint8_t priority;
  };

  /**
   * @brief The callback run as a result of FindControllers.
   */
  typedef ola::SingleUseCallback1<void, const std::vector<E133ControllerInfo>&>
      BrowseCallback;

  virtual ~E133DiscoveryAgentInterface() {}

  /**
   * @brief Initialize the DiscoveryAgent.
   */
  virtual bool Init() = 0;

  /**
   * @brief Once stop is called, the callback will no longer be executed.
   */
  virtual bool Stop() = 0;

  /**
   * @brief Find E1.33 controllers.
   * @param callback the callback to run when controllers are found.
   *   Ownership of the callback is transferred.
   */
  virtual bool FindControllers(BrowseCallback *callback) = 0;

  /**
   * @brief A non-callback version of FindControllers
   */
  virtual void FindControllers(
      std::vector<E133ControllerInfo> *controllers) = 0;

  static const char E133_CONTROLLER_SERVICE[];
};

/**
 * @brief A Factory which produces implementations of DiscoveryAgentInterface.
 * The exact type of object returns depends on what implementation of DNS-SD was
 * available at build time.
 */
class E133DiscoveryAgentFactory {
 public:
  E133DiscoveryAgentFactory() {}

  /**
   * @brief Create a new DiscoveryAgent.
   * This returns a DiscoveryAgent appropriate for the platform. It can
   * either be a BonjourDiscoveryAgent or a AvahiDiscoveryAgent.
   */
  E133DiscoveryAgentInterface* New();

 private:
  DISALLOW_COPY_AND_ASSIGN(E133DiscoveryAgentFactory);
};
#endif  // TOOLS_E133_E133DISCOVERYAGENT_H_
