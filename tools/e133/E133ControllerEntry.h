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
 * E133ControllerEntry.h
 * Information about an E1.33 controller.
 * Copyright (C) 2013 Simon Newton
 */

#ifndef TOOLS_E133_E133CONTROLLERENTRY_H_
#define TOOLS_E133_E133CONTROLLERENTRY_H_

#include <stdint.h>
#include <ola/network/SocketAddress.h>
#include <vector>

/**
 * @brief Represents a controller discovered using DNS-SD.
 *
 * The information in this struct is from the A and TXT records in DNS-SD.
 */
struct E133ControllerEntry {
  /** @brief The address of the controller */
  ola::network::IPV4SocketAddress address;
  /** @brief The controller's priority */
  uint8_t priority;

  // TODO(simon): Add the other fields we agreed upon here.
};

typedef std::vector<E133ControllerEntry> ControllerEntryList;

#endif  // TOOLS_E133_E133CONTROLLERENTRY_H_
