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

// TODO(simon): change to _rdmnet-ctrl._tcp once finalized
const char E133DiscoveryAgentInterface::E133_CONTROLLER_SERVICE[] =
    " _draft-e133-cntrl._tcp";

const char E133DiscoveryAgentInterface::DEFAULT_SCOPE[] = "default";

const char E133DiscoveryAgentInterface::E133_VERSION_KEY[] = "e133vers";
const char E133DiscoveryAgentInterface::MANUFACTURER_KEY[] = "manuf";
const char E133DiscoveryAgentInterface::MODEL_KEY[] = "model";
const char E133DiscoveryAgentInterface::PRIORITY_KEY[] = "priority";
const char E133DiscoveryAgentInterface::SCOPE_KEY[] = "confScope";
const char E133DiscoveryAgentInterface::TXT_VERSION_KEY[] = "txtvers";
const char E133DiscoveryAgentInterface::UID_KEY[] = "uid";

E133DiscoveryAgentInterface* E133DiscoveryAgentFactory::New() {
#ifdef HAVE_DNSSD
  return new BonjourE133DiscoveryAgent();
#endif
#ifdef HAVE_AVAHI
  return new AvahiE133DiscoveryAgent();
#endif
  return NULL;
}
