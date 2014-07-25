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
 * E133ControllerPDU.cpp
 * The E133ControllerPDU
 * Copyright (C) 2012 Simon Newton
 */


#include <string.h>
#include <ola/network/NetworkUtils.h>
#include "plugins/e131/e131/E133ControllerPDU.h"

namespace ola {
namespace plugin {
namespace e131 {

using ola::network::HostToNetwork;

void E133ControllerPDU::PrependPDU(ola::acn::E133ControllerVector vector_host,
                                   ola::io::IOStack *stack) {
  uint16_t vector = HostToNetwork(static_cast<uint16_t>(vector_host));
  stack->Write(reinterpret_cast<uint8_t*>(&vector), sizeof(vector));
  PrependFlagsAndLength(stack);
}
}  // namespace e131
}  // namespace plugin
}  // namespace ola
