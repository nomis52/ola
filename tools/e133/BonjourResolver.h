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
 * BonjourResolver.h
 * Resolve service names using Bonjour.
 * Copyright (C) 2014 Simon Newton
 */

#ifndef TOOLS_E133_BONJOURRESOLVER_H_
#define TOOLS_E133_BONJOURRESOLVER_H_

#include <dns_sd.h>

#include <ola/base/Macro.h>
#include <ola/network/IPV4Address.h>
#include <ola/network/SocketAddress.h>
#include <ola/rdm/UID.h>
#include <string>

class BonjourIOAdapter;

class BonjourResolver {
 public:
  BonjourResolver(
      BonjourIOAdapter *io_adapter,
      uint32_t interface_index,
      const std::string &service_name,
      const std::string &regtype,
      const std::string &reply_domain);
  virtual ~BonjourResolver();

  bool operator==(const BonjourResolver &other) const {
    return (interface_index == other.interface_index &&
            service_name == other.service_name &&
            regtype == other.regtype &&
            reply_domain == other.reply_domain);
  }

  std::string ToString() const {
    std::ostringstream str;
    str << service_name << "." << regtype << reply_domain << " on iface "
        << interface_index;
    return str.str();
  }

  friend std::ostream& operator<<(std::ostream &out,
                                  const BonjourResolver &info) {
    return out << info.ToString();
  }

  DNSServiceErrorType StartResolution();

  void ResolveHandler(
      DNSServiceErrorType errorCode,
      const std::string &host_target,
      uint16_t port,
      uint16_t txt_length,
      const unsigned char *txt_data);

  void UpdateAddress(const ola::network::IPV4Address &v4_address) {
    m_resolved_address.Host(v4_address);
  }

  std::string ServiceName() const { return service_name; }
  std::string Scope() const { return m_scope; }
  std::string Model() const { return m_model; }
  std::string Manufacturer() const { return m_manufacturer; }
  ola::network::IPV4SocketAddress ResolvedAddress() const {
    return m_resolved_address;
  }

 protected:
  virtual bool ProcessTxtData(uint16_t txt_length,
                              const unsigned char *txt_data);

  bool ExtractString(uint16_t txt_length,
                     const unsigned char *txt_data,
                     const std::string &key,
                     std::string *dest);
  bool ExtractInt(uint16_t txt_length,
                  const unsigned char *txt_data,
                  const std::string &key, unsigned int *dest);

 private:
  BonjourIOAdapter *m_io_adapter;
  bool m_resolve_in_progress;
  DNSServiceRef m_resolve_ref;

  bool to_addr_in_progress;
  DNSServiceRef m_to_addr_ref;

  uint32_t interface_index;
  const std::string service_name;
  const std::string regtype;
  const std::string reply_domain;
  std::string m_host_target;

  std::string m_scope;
  std::string m_model;
  std::string m_manufacturer;

  ola::network::IPV4SocketAddress m_resolved_address;

  bool CheckVersionMatches(
    uint16_t txt_length,
    const unsigned char *txt_data,
    const std::string &key,
    unsigned int version);

  DISALLOW_COPY_AND_ASSIGN(BonjourResolver);
};

/**
 * @brief A subclass of BonjourResolver that resolves E1.33 Controllers
 */
class ControllerResolver : public BonjourResolver {
 public:
  ControllerResolver(
      BonjourIOAdapter *io_adapter,
      uint32_t interface_index,
      const std::string &service_name,
      const std::string &regtype,
      const std::string &reply_domain);
  ~ControllerResolver() {}

  bool GetControllerEntry(class E133ControllerEntry *controller_entry) const;

 protected:
  bool ProcessTxtData(uint16_t txt_length, const unsigned char *txt_data);

 private:
  uint8_t m_priority;
  ola::rdm::UID m_uid;

  static const uint8_t DEFAULT_PRIORITY;

  DISALLOW_COPY_AND_ASSIGN(ControllerResolver);
};


/**
 * @brief A subclass of BonjourResolver that resolves E1.33 Distributors.
 */
class DistributorResolver : public BonjourResolver {
 public:
  DistributorResolver(
      BonjourIOAdapter *io_adapter,
      uint32_t interface_index,
      const std::string &service_name,
      const std::string &regtype,
      const std::string &reply_domain);
  ~DistributorResolver() {}

  bool GetDistributorEntry(class E133DistributorEntry *distributor_entry) const;

 private:
  DISALLOW_COPY_AND_ASSIGN(DistributorResolver);
};
#endif  // TOOLS_E133_BONJOURRESOLVER_H_
