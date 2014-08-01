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
 * E133DiscoveryAgent.cpp
 * The Interface for DNS-SD Discovery of E1.33 Controllers.
 * Copyright (C) 2013 Simon Newton
 */
#include "tools/e133/E133DiscoveryAgent.h"

#if HAVE_CONFIG_H
#include <config.h>
#endif

#include <ola/base/Flags.h>

#ifdef HAVE_DNSSD
#include "tools/e133/BonjourDiscoveryAgent.h"
#endif

#ifdef HAVE_AVAHI
#include "tools/e133/AvahiDiscoveryAgent.h"
#endif

const char E133DiscoveryAgentInterface::E133_CONTROLLER_SERVICE[] =
    "_rdmnet-ctrl._tcp";

DEFINE_uint8(controller_priority, 50,
             "The priority to use to register our service with");

E133DiscoveryAgentInterface* E133DiscoveryAgentFactory::New() {
#ifdef HAVE_DNSSD
  return new BonjourE133DiscoveryAgent();
#endif
#ifdef HAVE_AVAHI
  return new AvahiE133DiscoveryAgent();
#endif
  return NULL;
}
