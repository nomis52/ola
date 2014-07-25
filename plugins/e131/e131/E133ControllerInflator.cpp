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
 * E133ControllerInflator.cpp
 * The Inflator for the E1.33 Controller messages.
 * Copyright (C) 2014 Simon Newton
 */

#include <string>
#include <algorithm>
#include "ola/e133/E133Enums.h"
#include "plugins/e131/e131/E133ControllerInflator.h"

namespace ola {
namespace plugin {
namespace e131 {

using std::string;

/**
 * Create a new E1.33 status inflator
 */
E133ControllerInflator::E133ControllerInflator()
    : BaseInflator(PDU::TWO_BYTES) {
}


/*
 * Handle a E1.33 Controller PDU.
 */
bool E133ControllerInflator::HandlePDUData(uint32_t vector,
                                           const HeaderSet &headers,
                                           const uint8_t *data,
                                           unsigned int pdu_len) {
  string raw_data(reinterpret_cast<const char*>(data), pdu_len);

  m_handler->Run(&headers.GetTransportHeader(),
                 static_cast<uint16_t>(vector),
                 raw_data);
  return true;
}
}  // namespace e131
}  // namespace plugin
}  // namespace ola
