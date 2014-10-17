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
 * E133DistributorEntry.h
 * Information about an E1.33 distributor.
 * Copyright (C) 2013 Simon Newton
 */

#ifndef TOOLS_E133_E133DISTRIBUTORENTRY_H_
#define TOOLS_E133_E133DISTRIBUTORENTRY_H_

#include <stdint.h>
#include <ola/network/SocketAddress.h>
#include <string>
#include <vector>

/**
 * @brief Represents a distributor discovered using DNS-SD.
 *
 * The information in this struct is from the A and TXT records in DNS-SD.
 */
class E133DistributorEntry {
 public:
  /** @brief The service name of the controller */
  std::string service_name;

  /** @brief The address of the controller */
  ola::network::IPV4SocketAddress address;

  /** @brief The controller's scope */
  std::string scope;

  /** @brief The version of E1.33 this controller is using */
  uint8_t e133_version;

  /** @brief The controller's model. */
  std::string model;

  /** @brief The controller's manufacturer. */
  std::string manufacturer;

  E133DistributorEntry();

  bool operator==(const E133DistributorEntry &other) const {
    return (service_name == other.service_name &&
            address == other.address &&
            scope == other.scope &&
            e133_version == other.e133_version &&
            model == other.model &&
            manufacturer == other.manufacturer);
  }

  std::string ServiceName() const {
    return m_actual_service_name;
  }

  void UpdateFrom(const E133DistributorEntry &other);

  void SetServiceName(const std::string &service_name);

  std::string ToString() const;

  friend std::ostream& operator<<(std::ostream &out,
                                  const E133DistributorEntry &entry) {
    return out << entry.ToString();
  }

  static const uint8_t E133_VERSION = 1;

 private:
  std::string m_actual_service_name;
};


typedef std::vector<E133DistributorEntry> DistributorEntryList;

#endif  // TOOLS_E133_E133DISTRIBUTORENTRY_H_
