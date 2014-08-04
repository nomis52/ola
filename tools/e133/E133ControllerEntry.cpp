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
 * E133ControllerEntry.cpp
 * Information about an E1.33 controller.
 * Copyright (C) 2014 Simon Newton
 */

#include "tools/e133/E133ControllerEntry.h"

#include <stdint.h>
#include <ola/network/SocketAddress.h>
#include <ola/rdm/UID.h>
#include <string>
#include <iostream>

std::string E133ControllerEntry::ServiceName() const {
  if (!service_name.empty()) {
    return service_name;
  }

  std::ostringstream str;
  str << "OLA Controller " << address.Port();
  return str.str();
}

std::string E133ControllerEntry::ToString() const {
  std::ostringstream out;
  out << "Controller: '" << service_name << "' @ " << address << ", priority "
      << static_cast<int>(priority) << ", scope " << scope << ", UID " << uid
      << ", E1.33 Ver " << static_cast<int>(e133_version) << ", Model '"
      << model << "', Manufacturer '" << manufacturer << "'";
  return out.str();
}
