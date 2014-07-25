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
 * E133ControllerPDU.h
 * Interface for the E133ControllerPDU class
 * Copyright (C) 2012 Simon Newton
 */

#ifndef PLUGINS_E131_E131_E133CONTROLLERPDU_H_
#define PLUGINS_E131_E131_E133CONTROLLERPDU_H_

#include <ola/io/IOStack.h>
#include <ola/rdm/RDMCommand.h>
#include <memory>

#include "plugins/e131/e131/PDU.h"
#include "plugins/e131/e131/RDMInflator.h"

namespace ola {
namespace plugin {
namespace e131 {

/**
 * An RDM PDU carries a RDMCommand.
 */
class E133ControllerPDU: public PDU {
 public:
  static void PrependPDU(ola::acn::E133ControllerVector vector,
                         ola::io::IOStack *stack);
};
}  // namespace e131
}  // namespace plugin
}  // namespace ola
#endif  // PLUGINS_E131_E131_E133CONTROLLERPDU_H_
