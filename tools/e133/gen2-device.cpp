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
 * gen2-device.cpp
 * Copyright (C) 2014 Simon Newton
 *
 * A Generation II device which opens a TCP connection to a controller.
 * I'm using this for scale testing.
 */

#include <ola/Logging.h>
#include <ola/base/Flags.h>
#include <ola/base/Init.h>
#include <ola/base/SysExits.h>
#include <ola/rdm/UID.h>
#include <signal.h>

#include <memory>

#include "tools/e133/Gen2E133Device.h"

DEFINE_string(controller_address, "",
              "The IP:Port of the controller, if set this bypasses discovery");
DEFINE_string(uid, "7a70:00000001", "The UID of the responder.");
DEFINE_uint16(uid_offset, 0, "An offset to apply to the UID.");

DEFINE_uint16(udp_port, 0, "The port to listen on");

using ola::network::IPV4SocketAddress;
using ola::rdm::UID;

Gen2Device *device = NULL;

/**
 * Interupt handler
 */
static void InteruptSignal(int unused) {
  if (device)
    device->Stop();
  (void) unused;
}

int main(int argc, char *argv[]) {
  ola::SetHelpString("[options]", "Simple E1.33 Device.");
  ola::ParseFlags(&argc, argv);
  ola::InitLoggingFromFlags();

  std::auto_ptr<UID> uid(UID::FromString(FLAGS_uid));
  if (!uid.get()) {
    OLA_WARN << "Invalid UID: " << FLAGS_uid;
    ola::DisplayUsage();
    exit(ola::EXIT_USAGE);
  }

  UID actual_uid(uid->ManufacturerId(), uid->DeviceId() + FLAGS_uid_offset);

  Gen2Device::Options options(actual_uid);
  options.port = FLAGS_udp_port;

  if (!FLAGS_controller_address.str().empty()) {
    if (!IPV4SocketAddress::FromString(FLAGS_controller_address.str(),
                                       &options.controller)) {
      OLA_WARN << "Invalid --controller-address";
      exit(ola::EXIT_USAGE);
    }
  }

  device = new Gen2Device(options);

  ola::InstallSignal(SIGINT, InteruptSignal);
  if (!device->Run()) {
    OLA_WARN << "Failed to start device";
  }
  delete device;
  device = NULL;
}
