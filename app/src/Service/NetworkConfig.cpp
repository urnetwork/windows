// SPDX-License-Identifier: MPL-2.0
#include "NetworkConfig.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <netioapi.h>

#include "Log.h"
#include "Strings.h"

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")

namespace urnw {
namespace {

bool ParseV4(const std::string& s, IN_ADDR& out) {
  return ::inet_pton(AF_INET, s.c_str(), &out) == 1;
}

// Add a route through the tun for one half of the default range (0.0.0.0/1 and
// 128.0.0.0/1 together cover all of 0.0.0.0/0 while sorting above the physical
// default route, so the tun captures everything without deleting the existing
// default — the macOS "split default" trick).
bool AddTunRoute(NET_LUID tun, uint32_t network, uint8_t prefix) {
  MIB_IPFORWARD_ROW2 row;
  ::InitializeIpForwardEntry(&row);
  row.InterfaceLuid = tun;
  row.DestinationPrefix.Prefix.si_family = AF_INET;
  row.DestinationPrefix.Prefix.Ipv4.sin_family = AF_INET;
  row.DestinationPrefix.Prefix.Ipv4.sin_addr.S_un.S_addr = htonl(network);
  row.DestinationPrefix.PrefixLength = prefix;
  row.NextHop.si_family = AF_INET;
  row.NextHop.Ipv4.sin_family = AF_INET;  // 0.0.0.0 next hop => on-link
  row.Metric = 0;
  row.Protocol = MIB_IPPROTO_NETMGMT;
  DWORD err = ::CreateIpForwardEntry2(&row);
  if (err != NO_ERROR && err != ERROR_OBJECT_ALREADY_EXISTS) {
    LogError("route: add {:#x}/{} via tun failed: {}", network, prefix, err);
    return false;
  }
  return true;
}

void DeleteTunRoute(NET_LUID tun, uint32_t network, uint8_t prefix) {
  MIB_IPFORWARD_ROW2 row;
  ::InitializeIpForwardEntry(&row);
  row.InterfaceLuid = tun;
  row.DestinationPrefix.Prefix.si_family = AF_INET;
  row.DestinationPrefix.Prefix.Ipv4.sin_family = AF_INET;
  row.DestinationPrefix.Prefix.Ipv4.sin_addr.S_un.S_addr = htonl(network);
  row.DestinationPrefix.PrefixLength = prefix;
  ::DeleteIpForwardEntry2(&row);
}

bool SetTunDns(NET_LUID tun, const std::vector<std::string>& servers,
               const std::string& search) {
  GUID guid{};
  if (::ConvertInterfaceLuidToGuid(&tun, &guid) != NO_ERROR) return false;

  std::wstring joined;
  for (const auto& s : servers) {
    if (!joined.empty()) joined += L",";
    joined += Widen(s);
  }

  DNS_INTERFACE_SETTINGS settings{};
  settings.Version = DNS_INTERFACE_SETTINGS_VERSION1;
  settings.Flags = DNS_SETTING_NAMESERVER;
  settings.NameServer = joined.empty() ? nullptr : joined.data();
  std::wstring wsearch = Widen(search);
  if (!wsearch.empty()) {
    settings.Flags |= DNS_SETTING_SEARCHLIST;
    settings.SearchList = wsearch.data();
  }
  DWORD err = ::SetInterfaceDnsSettings(guid, &settings);
  if (err != NO_ERROR) {
    // Win10 pre-2004 lacks SetInterfaceDnsSettings; caller should fall back to
    // the netsh/registry path (plan R6). Surface for diagnostics.
    LogWarn("dns: SetInterfaceDnsSettings failed: {} (pre-2004? use fallback)", err);
    return false;
  }
  return true;
}

}  // namespace

bool NetworkConfig::Apply(const TunnelNetworkSettings& settings) {
  settings_ = settings;

  // --- tun local address ---
  IN_ADDR addr{};
  if (!ParseV4(settings.local_address_v4, addr)) {
    LogError("netcfg: bad local address {}", settings.local_address_v4);
    return false;
  }
  MIB_UNICASTIPADDRESS_ROW ipRow;
  ::InitializeUnicastIpAddressEntry(&ipRow);
  ipRow.InterfaceLuid = tunLuid_;
  ipRow.Address.Ipv4.sin_family = AF_INET;
  ipRow.Address.Ipv4.sin_addr = addr;
  ipRow.OnLinkPrefixLength = settings.prefix_v4;
  ipRow.DadState = IpDadStatePreferred;
  DWORD err = ::CreateUnicastIpAddressEntry(&ipRow);
  if (err != NO_ERROR && err != ERROR_OBJECT_ALREADY_EXISTS) {
    LogError("netcfg: set tun address failed: {}", err);
    return false;
  }

  // --- MTU + low metric so the tun default sorts first ---
  MIB_IPINTERFACE_ROW ifRow{};
  ifRow.Family = AF_INET;
  ifRow.InterfaceLuid = tunLuid_;
  if (::GetIpInterfaceEntry(&ifRow) == NO_ERROR) {
    ifRow.NlMtu = settings.mtu;
    ifRow.UseAutomaticMetric = FALSE;
    ifRow.Metric = 1;
    // SitePrefixLength must be cleared before SetIpInterfaceEntry per docs.
    ifRow.SitePrefixLength = 0;
    err = ::SetIpInterfaceEntry(&ifRow);
    if (err != NO_ERROR) LogWarn("netcfg: set MTU/metric failed: {}", err);
  }

  // --- split-default routes through the tun ---
  bool routesOk = AddTunRoute(tunLuid_, 0x00000000u, 1) &&   // 0.0.0.0/1
                  AddTunRoute(tunLuid_, 0x80000000u, 1);      // 128.0.0.0/1
  if (!routesOk) {
    Revert();
    return false;
  }

  // --- DNS (R6: also needs a leak guard against other adapters' resolvers) ---
  if (!settings.dns_servers.empty()) {
    SetTunDns(tunLuid_, settings.dns_servers, settings.dns_search);
  }

  applied_ = true;
  LogInfo("netcfg: applied addr={}/{} mtu={} dns={}", settings.local_address_v4,
          settings.prefix_v4, settings.mtu, settings.dns_servers.size());
  return true;
}

void NetworkConfig::Revert() {
  if (!applied_ && settings_.local_address_v4.empty()) return;
  DeleteTunRoute(tunLuid_, 0x00000000u, 1);
  DeleteTunRoute(tunLuid_, 0x80000000u, 1);
  // Address and DNS go away with the adapter on session end; clearing DNS
  // explicitly avoids a stale resolver if the adapter lingers.
  SetTunDns(tunLuid_, {}, {});
  applied_ = false;
  LogInfo("netcfg: reverted");
}

EgressInterfaces NetworkConfig::DiscoverEgress(NET_LUID tunLuid) {
  EgressInterfaces result;
  for (ADDRESS_FAMILY family : {AF_INET, AF_INET6}) {
    PMIB_IPFORWARD_TABLE2 table = nullptr;
    if (::GetIpForwardTable2(family, &table) != NO_ERROR || !table) continue;

    ULONG bestMetric = ~0u;
    uint32_t bestIndex = 0;
    for (ULONG i = 0; i < table->NumEntries; ++i) {
      const MIB_IPFORWARD_ROW2& row = table->Table[i];
      if (row.DestinationPrefix.PrefixLength != 0) continue;  // default only
      if (row.InterfaceLuid.Value == tunLuid.Value) continue;  // skip our tun
      // combined route+interface metric; lower wins (same ordering Windows uses)
      MIB_IPINTERFACE_ROW ifRow{};
      ifRow.Family = family;
      ifRow.InterfaceLuid = row.InterfaceLuid;
      ULONG ifMetric = 0;
      if (::GetIpInterfaceEntry(&ifRow) == NO_ERROR) ifMetric = ifRow.Metric;
      ULONG metric = row.Metric + ifMetric;
      if (metric < bestMetric) {
        bestMetric = metric;
        bestIndex = row.InterfaceIndex;
      }
    }
    ::FreeMibTable(table);
    if (family == AF_INET)
      result.index4 = bestIndex;
    else
      result.index6 = bestIndex;
  }
  return result;
}

bool NetworkConfig::InterfaceSourceAddress(uint32_t ifIndex, int family,
                                           uint8_t* addr) {
  PMIB_UNICASTIPADDRESS_TABLE table = nullptr;
  if (::GetUnicastIpAddressTable(static_cast<ADDRESS_FAMILY>(family), &table) !=
          NO_ERROR ||
      !table) {
    return false;
  }
  bool found = false;
  for (ULONG i = 0; i < table->NumEntries && !found; ++i) {
    const MIB_UNICASTIPADDRESS_ROW& row = table->Table[i];
    if (row.InterfaceIndex != ifIndex) continue;
    if (row.DadState != IpDadStatePreferred) continue;
    if (family == AF_INET) {
      const IN_ADDR& a = row.Address.Ipv4.sin_addr;
      // skip link-local (169.254/16) — not a usable egress source
      if ((ntohl(a.S_un.S_addr) & 0xFFFF0000u) == 0xA9FE0000u) continue;
      std::memcpy(addr, &a, sizeof(IN_ADDR));
      found = true;
    } else if (family == AF_INET6) {
      const IN6_ADDR& a = row.Address.Ipv6.sin6_addr;
      // skip link-local (fe80::/10)
      if (a.u.Byte[0] == 0xFE && (a.u.Byte[1] & 0xC0) == 0x80) continue;
      std::memcpy(addr, &a, sizeof(IN6_ADDR));
      found = true;
    }
  }
  ::FreeMibTable(table);
  return found;
}

}  // namespace urnw
