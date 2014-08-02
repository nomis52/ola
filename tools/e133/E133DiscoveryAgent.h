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
 * E133DiscoveryAgent.h
 * The Interface for E1.33 DNS-SD Discovery & Registration.
 * Copyright (C) 2013 Simon Newton
 */

#ifndef TOOLS_E133_E133DISCOVERYAGENT_H_
#define TOOLS_E133_E133DISCOVERYAGENT_H_

#include <stdint.h>
#include <ola/base/Macro.h>
#include <ola/Callback.h>
#include <ola/network/SocketAddress.h>
#include <string>
#include <vector>

#include "tools/e133/E133ControllerEntry.h"

/**
 * @brief The interface to E1.33 DNS-SD operations like register, browse etc.
 *
 * The E133DiscoveryAgentInterface encapsulates the DNS-SD operations of
 * registering and browsing for controllers.
 *
 * Two implementations exists: Bonjour (Apple) and Avahi.
 *
 * Since the implementation of this interface depends on which DNS-SD library
 * is available on the platform, the E133DiscoveryAgentFactory::New() should be
 * used to create instances of E133DiscoveryAgentInterface.
 */
class E133DiscoveryAgentInterface {
 public:
  virtual ~E133DiscoveryAgentInterface() {}

  /**
   * @brief Start the DiscoveryAgent.
   *
   * In both the Avahi and Bonjour implementations this starts the DNS-SD
   * thread.
   */
  virtual bool Start() = 0;

  /**
   * @brief Stop the DiscoveryAgent.
   *
   * Once this returns any threads will have been terminated.
   */
  virtual bool Stop() = 0;

  /**
   * @brief Change the scope for discovery.
   *
   * The scope corresponds to the sub_type in DNS-SD. If the scope is the empty
   * string, all controllers will be discovered.
   *
   * Once this method returns, FindControllers() will only return controllers
   * in the current scope.
   */
  virtual void SetScope(const std::string &scope) = 0;

  /**
   * @brief Get a list of the known controllers.
   * @param[out] controllers A vector to be populated with the known
   *   controllers.
   */
  virtual void FindControllers(ControllerEntryList *controllers) = 0;

  /**
   * @brief Register the SocketAddress as an E1.33 controller.
   * @param controller The controller entry to register in DNS-SD.
   *
   * If this is called twice with a controller with the same IPV4SocketAddress
   * the TXT field will be updated with the newer values.
   *
   * Registration may be performed in a separate thread.
   */
  virtual void RegisterController(const E133ControllerEntry &controller) = 0;

  /**
   * @brief De-Register the SocketAddress as an E1.33 controller.
   * @param controller_address The SocketAddress to de-register. This should be
   * the same as what was in the E133ControllerEntry that was passed to
   * RegisterController().
   *
   * DeRegistration may be performed in a separate thread.
   */
  virtual void DeRegisterController(
      const ola::network::IPV4SocketAddress &controller_address) = 0;

  static const char E133_CONTROLLER_SERVICE[];
  static const char DEFAULT_SCOPE[];

  static const char E133_VERSION_KEY[];
  static const char MANUFACTURER_KEY[];
  static const char MODEL_KEY[];
  static const char PRIORITY_KEY[];
  static const char SCOPE_KEY[];
  static const char TXT_VERSION_KEY[];
  static const char UID_KEY[];

  static const uint8_t TXT_VERSION = 1;
  static const uint8_t E133_VERSION = 1;
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
