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
#include <signal.h>

#include "tools/e133/Gen2E133Device.h"

DEFINE_string(controller_address, "",
              "The IP:Port of the controller, if set this bypasses discovery");

using ola::network::IPV4SocketAddress;

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

  Gen2Device::Options options;
  if (!FLAGS_controller_address.str().empty()) {
    if (!IPV4SocketAddress::FromString(FLAGS_controller_address.str(),
                                       &options.controller)) {
      OLA_WARN << "Invalid --controller-address";
      exit(ola::EXIT_USAGE);
    }
  }

  device = new Gen2Device(options);

  ola::InstallSignal(SIGINT, InteruptSignal);
  device->Run();
  delete device;
  device = NULL;
}
