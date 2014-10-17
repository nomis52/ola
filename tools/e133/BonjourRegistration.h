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
 * BonjourRegistration.h
 * Handles DNS-SD registration.
 * Copyright (C) 2014 Simon Newton
 */

#ifndef TOOLS_E133_BONJOURREGISTRATION_H_
#define TOOLS_E133_BONJOURREGISTRATION_H_

#include <dns_sd.h>
#include <ola/base/Macro.h>
#include <ola/network/SocketAddress.h>
#include <string>
#include <vector>

#include "tools/e133/E133ControllerEntry.h"
#include "tools/e133/E133DistributorEntry.h"

class BonjourIOAdapter;

std::string GenerateE133SubType(const std::string &scope,
                                const std::string &service);

class BonjourRegistration {
 public:
  explicit BonjourRegistration(class BonjourIOAdapter *io_adapter)
      : m_io_adapter(io_adapter),
        m_registration_ref(NULL) {
  }
  virtual ~BonjourRegistration();

  void RegisterEvent(DNSServiceErrorType error_code,
                     const std::string &name,
                     const std::string &type,
                     const std::string &domain);

 protected:
  bool RegisterOrUpdateInternal(const std::string &service_type,
                                const std::string &scope,
                                const std::string &service_name,
                                const ola::network::IPV4SocketAddress &address,
                                const std::string &txt_record);

  std::string BuildTxtString(const std::vector<std::string> &records);

 private:
  class BonjourIOAdapter *m_io_adapter;
  std::string m_scope;
  std::string m_last_txt_data;
  DNSServiceRef m_registration_ref;

  void CancelRegistration();
  bool UpdateRecord(const std::string &txt_data);

  DISALLOW_COPY_AND_ASSIGN(BonjourRegistration);
};

class ControllerRegistration : public BonjourRegistration {
 public:
  explicit ControllerRegistration(class BonjourIOAdapter *io_adapter)
      : BonjourRegistration(io_adapter) {
  }
  ~ControllerRegistration() {}

  bool RegisterOrUpdate(const E133ControllerEntry &controller);

 private:
  std::string BuildTxtRecord(const E133ControllerEntry &controller);

  DISALLOW_COPY_AND_ASSIGN(ControllerRegistration);
};

class DistributorRegistration : public BonjourRegistration {
 public:
  explicit DistributorRegistration(class BonjourIOAdapter *io_adapter)
      : BonjourRegistration(io_adapter) {
  }
  ~DistributorRegistration() {}

  bool RegisterOrUpdate(const E133DistributorEntry &distributor);

 private:
  std::string BuildTxtRecord(const E133DistributorEntry &distributor);

  DISALLOW_COPY_AND_ASSIGN(DistributorRegistration);
};


#endif  // TOOLS_E133_BONJOURREGISTRATION_H_
