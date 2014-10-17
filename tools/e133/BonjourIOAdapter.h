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
 * BonjourE133DiscoveryAgent.h
 * The Bonjour implementation of DiscoveryAgentInterface.
 * Copyright (C) 2013 Simon Newton
 */

#ifndef TOOLS_E133_BONJOURIOADAPTER_H_
#define TOOLS_E133_BONJOURIOADAPTER_H_

#include <dns_sd.h>

#include <ola/base/Macro.h>
#include <ola/io/Descriptor.h>
#include <map>

#include "tools/e133/E133DiscoveryAgent.h"

namespace ola {
namespace io {
class SelectServerInterface;
}
}

// DNSSDDescriptor
// ----------------------------------------------------------------------------
class DNSSDDescriptor : public ola::io::ReadFileDescriptor {
 public:
  explicit DNSSDDescriptor(DNSServiceRef service_ref)
      : m_service_ref(service_ref),
        m_ref_count(0) {
  }

  int ReadDescriptor() const {
    return DNSServiceRefSockFD(m_service_ref);
  }

  void Ref() {
    m_ref_count++;
  }

  /**
   * @brief Deref this descriptor.
   * @returns true if the descriptor is still in use, false if it's no longer
   *   used.
   */
  bool DeRef() {
    if (m_ref_count) {
      m_ref_count--;
    }
    return m_ref_count != 0;
  }

  void PerformRead();

 private:
  DNSServiceRef m_service_ref;
  unsigned int m_ref_count;
};

/**
 * @brief The adapter between the Bonjour library and SelectServerInterface.
 */
class BonjourIOAdapter {
 public:
  explicit BonjourIOAdapter(ola::io::SelectServerInterface *ss)
      : m_ss(ss) {
  }

  void AddDescriptor(DNSServiceRef service_ref);
  void RemoveDescriptor(DNSServiceRef service_ref);

 private:
  typedef std::map<int, class DNSSDDescriptor*> DescriptorMap;

  DescriptorMap m_descriptors;
  ola::io::SelectServerInterface *m_ss;

  DISALLOW_COPY_AND_ASSIGN(BonjourIOAdapter);
};

#endif  // TOOLS_E133_BONJOURIOADAPTER_H_
