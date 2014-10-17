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
 * BonjourE133DiscoveryAgent.cpp
 * The Bonjour implementation of DiscoveryAgentInterface.
 * Copyright (C) 2013 Simon Newton
 */

#define __STDC_LIMIT_MACROS  // for UINT8_MAX & friends

#include "tools/e133/BonjourIOAdapter.h"

#include <dns_sd.h>
#include <ola/Logging.h>
#include <ola/io/SelectServer.h>
#include <ola/stl/STLUtils.h>

#include <map>
#include <utility>

void DNSSDDescriptor::PerformRead() {
  DNSServiceErrorType error = DNSServiceProcessResult(m_service_ref);
  if (error != kDNSServiceErr_NoError) {
    // TODO(simon): Consider de-registering from the ss here?
    OLA_FATAL << "DNSServiceProcessResult returned " << error;
  }
}

void BonjourIOAdapter::AddDescriptor(DNSServiceRef service_ref) {
  int fd = DNSServiceRefSockFD(service_ref);

  std::pair<DescriptorMap::iterator, bool> p = m_descriptors.insert(
      DescriptorMap::value_type(fd, NULL));

  if (p.first->second) {
    // Descriptor exists, increment the ref count.
    p.first->second->Ref();
    return;
  }

  p.first->second = new DNSSDDescriptor(service_ref);
  p.first->second->Ref();
  m_ss->AddReadDescriptor(p.first->second);
}

void BonjourIOAdapter::RemoveDescriptor(DNSServiceRef service_ref) {
  int fd = DNSServiceRefSockFD(service_ref);
  DescriptorMap::iterator iter = m_descriptors.find(fd);
  if (iter == m_descriptors.end()) {
    OLA_FATAL << "Missing FD " << fd << " in descriptor map";
    return;
  }

  if (iter->second->DeRef()) {
    return;
  }
  // RefCount is 0
  m_ss->RemoveReadDescriptor(iter->second);
  delete iter->second;
  m_descriptors.erase(iter);
}
