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
 * E133DistributorEntry.cpp
 * Information about an E1.33 distributor.
 * Copyright (C) 2014 Simon Newton
 */

#include "tools/e133/E133DistributorEntry.h"

#include <stdint.h>
#include <ola/network/SocketAddress.h>
#include <string>
#include <iostream>

using std::string;

E133DistributorEntry::E133DistributorEntry()
    : e133_version(E133_VERSION),
      manufacturer("Open Lighting") {
  if (service_name.empty()) {
    std::ostringstream str;
    str << "OLA Distributor";
    m_actual_service_name = str.str();
  } else {
    m_actual_service_name = service_name;
  }
}

void E133DistributorEntry::UpdateFrom(const E133DistributorEntry &other) {
  service_name = other.service_name;
  address = other.address;
  scope = other.scope;
  e133_version = other.e133_version;
  model = other.model;
  manufacturer = other.manufacturer;
}

void E133DistributorEntry::SetServiceName(const std::string &service_name) {
  m_actual_service_name = service_name;
}

string E133DistributorEntry::ToString() const {
  std::ostringstream out;
  out << "Distributor: '" << service_name << "' @ " << address << ", scope "
      << scope << ", E1.33 Ver " << static_cast<int>(e133_version)
      << ", Model '" << model << "', Manufacturer '" << manufacturer << "'";
  return out.str();
}
