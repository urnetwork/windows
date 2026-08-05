// SPDX-License-Identifier: MPL-2.0
#include "NetworkConfig.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <netioapi.h>

#include <atomic>
#include <cwchar>

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

// Returns true when a route was actually removed (ERROR_NOT_FOUND just means
// it was never there, which is the normal case on a second revert).
bool DeleteTunRoute(NET_LUID tun, uint32_t network, uint8_t prefix) {
  MIB_IPFORWARD_ROW2 row;
  ::InitializeIpForwardEntry(&row);
  row.InterfaceLuid = tun;
  row.DestinationPrefix.Prefix.si_family = AF_INET;
  row.DestinationPrefix.Prefix.Ipv4.sin_family = AF_INET;
  row.DestinationPrefix.Prefix.Ipv4.sin_addr.S_un.S_addr = htonl(network);
  row.DestinationPrefix.PrefixLength = prefix;
  return ::DeleteIpForwardEntry2(&row) == NO_ERROR;
}

// The interface whose routes CrashRevert() must remove, or 0 when the tunnel is
// down. A plain atomic so the crash path reads it without taking a lock that
// the crashing thread might already hold.
std::atomic<uint64_t> g_armedTunLuid{0};

// The whole ipv4 space EXCEPT the private ranges (10.0.0.0/8, 172.16.0.0/12,
// 192.168.0.0/16), captured through the tun so LAN traffic bypasses the tunnel —
// matching Android (MainService excludeRoute), iOS (NEIPv4Settings.excludedRoutes)
// and Linux. These are the complement prefixes of those ranges within 0.0.0.0/0
// (the same set Android adds on its no-excludeRoute path). Like the old 0.0.0.0/1 +
// 128.0.0.0/1 capture they sort above the physical default without deleting it; the
// excluded ranges fall through to the physical/connected routes. {network (host
// byte order), prefix length}.
struct TunPrefix {
  uint32_t network;
  uint8_t prefix;
};
constexpr TunPrefix kIncludedV4Routes[] = {
    {0x00000000u, 5},  {0x08000000u, 7},  {0x0B000000u, 8},  {0x0C000000u, 6},
    {0x10000000u, 4},  {0x20000000u, 3},  {0x40000000u, 2},  {0x80000000u, 3},
    {0xA0000000u, 5},  {0xA8000000u, 6},  {0xAC000000u, 12}, {0xAC200000u, 11},
    {0xAC400000u, 10}, {0xAC800000u, 9},  {0xAD000000u, 8},  {0xAE000000u, 7},
    {0xB0000000u, 4},  {0xC0000000u, 9},  {0xC0800000u, 11}, {0xC0A00000u, 13},
    {0xC0A90000u, 16}, {0xC0AA0000u, 15}, {0xC0AC0000u, 14}, {0xC0B00000u, 12},
    {0xC0C00000u, 10}, {0xC1000000u, 8},  {0xC2000000u, 7},  {0xC4000000u, 6},
    {0xC8000000u, 5},  {0xD0000000u, 4},  {0xE0000000u, 3},
};

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

  // Arm the crash path before the FIRST mutation, not after the last one: a
  // crash halfway through the route loop must still be cleanable.
  ArmCrashRevert(tunLuid_);

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

  // --- split-default routes through the tun, EXCLUDING the local network ---
  // From here on the host's traffic is being redirected, so every exit from
  // this function must either leave a complete route set or none at all.
  bool routesOk = true;
  for (const auto& r : kIncludedV4Routes) {
    routesOk = routesOk && AddTunRoute(tunLuid_, r.network, r.prefix);
  }
  if (!routesOk) {
    LogError("netcfg: route install incomplete, reverting the partial set");
    Revert();
    return false;
  }
  LogInfo("netcfg: installed {} tun routes (private ranges excluded)",
          std::size(kIncludedV4Routes));

  // --- DNS (R6: also needs a leak guard against other adapters' resolvers) ---
  if (!settings.dns_servers.empty()) {
    bool dnsOk = SetTunDns(tunLuid_, settings.dns_servers, settings.dns_search);
    // Not fatal: the tunnel still carries traffic, DNS just leaks to the
    // physical adapter's resolver. Loud, because that IS the R6 leak.
    if (!dnsOk) LogWarn("netcfg: tun DNS not set — queries will use the physical resolver (R6)");
  } else {
    LogWarn("netcfg: no tun DNS servers supplied");
  }

  applied_ = true;
  LogInfo("netcfg: applied addr={}/{} mtu={} dns={}", settings.local_address_v4,
          settings.prefix_v4, settings.mtu, settings.dns_servers.size());
  return true;
}

void NetworkConfig::Revert() {
  if (!applied_ && settings_.local_address_v4.empty()) {
    DisarmCrashRevert();
    return;
  }
  int removed = DeleteTunnelRoutes(tunLuid_);
  // Address and DNS go away with the adapter on session end; clearing DNS
  // explicitly avoids a stale resolver if the adapter lingers.
  ClearTunnelDns(tunLuid_);
  // Drop the address too rather than trusting the adapter teardown to do it.
  // ERROR_NOT_FOUND here is fine — it means the adapter already went away.
  IN_ADDR addr{};
  if (ParseV4(settings_.local_address_v4, addr)) {
    MIB_UNICASTIPADDRESS_ROW ipRow;
    ::InitializeUnicastIpAddressEntry(&ipRow);
    ipRow.InterfaceLuid = tunLuid_;
    ipRow.Address.Ipv4.sin_family = AF_INET;
    ipRow.Address.Ipv4.sin_addr = addr;
    ::DeleteUnicastIpAddressEntry(&ipRow);
  }
  applied_ = false;
  DisarmCrashRevert();
  LogInfo("netcfg: reverted ({} of {} routes removed, dns cleared)", removed,
          std::size(kIncludedV4Routes));
}

// --- crash safety ----------------------------------------------------------

int NetworkConfig::DeleteTunnelRoutes(NET_LUID tunLuid) {
  int removed = 0;
  for (const auto& r : kIncludedV4Routes) {
    if (DeleteTunRoute(tunLuid, r.network, r.prefix)) ++removed;
  }
  return removed;
}

void NetworkConfig::ClearTunnelDns(NET_LUID tunLuid) {
  GUID guid{};
  if (::ConvertInterfaceLuidToGuid(&tunLuid, &guid) != NO_ERROR) return;
  DNS_INTERFACE_SETTINGS settings{};
  settings.Version = DNS_INTERFACE_SETTINGS_VERSION1;
  // Clear BOTH of the things Apply can set. Apply adds DNS_SETTING_SEARCHLIST
  // whenever a search domain is supplied, so clearing only NAMESERVER left the
  // search list in force on the interface.
  settings.Flags = DNS_SETTING_NAMESERVER | DNS_SETTING_SEARCHLIST;
  settings.NameServer = nullptr;
  settings.SearchList = nullptr;
  ::SetInterfaceDnsSettings(guid, &settings);
}

void NetworkConfig::ArmCrashRevert(NET_LUID tunLuid) {
  g_armedTunLuid.store(tunLuid.Value);
}

void NetworkConfig::DisarmCrashRevert() { g_armedTunLuid.store(0); }

void NetworkConfig::CrashRevert() {
  // exchange, not load+store: two termination paths can fire at once (an
  // unhandled exception on one thread while the control handler runs on
  // another) and only one of them should do the work.
  uint64_t value = g_armedTunLuid.exchange(0);
  if (value == 0) return;
  NET_LUID luid{};
  luid.Value = value;
  // Routes ONLY. They are what strands the machine, and DeleteIpForwardEntry2
  // is an ioctl into the stack — bounded, no service to talk to.
  //
  // The DNS clear is deliberately NOT here. SetInterfaceDnsSettings is an RPC
  // to the dnscache service, and this function runs from an exception filter:
  // if that RPC blocks, the filter never returns and the process wedges instead
  // of dying — a hung LocalSystem service holding the routes is worse than the
  // crash we were handling. Stale resolvers on an interface that is about to
  // disappear harm nothing; the orderly Revert() and the startup sweep both
  // clear DNS, and neither runs under that constraint.
  DeleteTunnelRoutes(luid);
}

int NetworkConfig::SweepOrphanedTunnel(const GUID& tunGuid,
                                       const wchar_t* adapterName) {
  // Collect candidate LUIDs first, then sweep, so a rename or a GUID fallback
  // cannot make us miss the interface that is holding the machine's traffic.
  uint64_t candidates[8] = {0};
  int candidateCount = 0;
  auto add = [&](NET_LUID luid) {
    if (luid.Value == 0) return;
    for (int i = 0; i < candidateCount; ++i)
      if (candidates[i] == luid.Value) return;
    if (candidateCount < 8) candidates[candidateCount++] = luid.Value;
  };

  // ConvertInterfaceGuidToLuid resolves against the live interface table, so a
  // success here means an interface with our pinned GUID exists right now.
  NET_LUID byGuid{};
  GUID guid = tunGuid;
  if (::ConvertInterfaceGuidToLuid(&guid, &byGuid) == NO_ERROR) add(byGuid);

  // WintunCreateAdapter treats the GUID as a REQUEST; if it was taken, the
  // adapter exists under another one, so fall back to the alias.
  //
  // The alias alone is NOT sufficient evidence. "URnetwork" is a name any
  // adapter could carry — a user can rename a NIC to anything — and sweeping a
  // wrongly-matched interface would delete routes and DNS from a real adapter,
  // which is the failure this whole file exists to prevent, caused by the code
  // meant to prevent it. So require the alias match AND a wintun-shaped device.
  //
  // Deliberately asymmetric: a false negative means we fail to clean an orphan
  // that the GUID path and the adapter teardown were already covering; a false
  // positive means we break the machine's real network. Prefer the miss.
  if (adapterName && *adapterName) {
    PMIB_IF_TABLE2 table = nullptr;
    if (::GetIfTable2(&table) == NO_ERROR && table) {
      for (ULONG i = 0; i < table->NumEntries; ++i) {
        const MIB_IF_ROW2& row = table->Table[i];
        if (::wcscmp(row.Alias, adapterName) != 0) continue;
        const bool virtualDevice = row.Type == IF_TYPE_PROP_VIRTUAL ||
                                   row.Type == IF_TYPE_TUNNEL;
        const bool wintunDevice = ::wcsstr(row.Description, L"Wintun") != nullptr;
        if (!virtualDevice && !wintunDevice) {
          LogWarn("netcfg: interface \"{}\" matches our adapter name but is not a "
                  "tunnel device ({}); leaving it alone",
                  Narrow(row.Alias), Narrow(row.Description));
          continue;
        }
        add(row.InterfaceLuid);
      }
      ::FreeMibTable(table);
    }
  }

  for (int i = 0; i < candidateCount; ++i) {
    NET_LUID luid{};
    luid.Value = candidates[i];
    MIB_IF_ROW2 row{};
    row.InterfaceLuid = luid;
    std::string alias = (::GetIfEntry2(&row) == NO_ERROR) ? Narrow(row.Alias)
                                                          : std::string("<gone>");
    int removed = DeleteTunnelRoutes(luid);
    ClearTunnelDns(luid);
    LogWarn(
        "netcfg: ORPHANED tun interface \"{}\" (luid {:#x}) present at startup — "
        "a previous run did not revert; removed {} stale routes and cleared its "
        "DNS",
        alias, luid.Value, removed);
  }
  return candidateCount;
}

std::string NetworkConfig::DescribeInterface(uint32_t ifIndex) {
  if (ifIndex == 0) return "none";
  MIB_IF_ROW2 row{};
  row.InterfaceIndex = ifIndex;
  if (::GetIfEntry2(&row) != NO_ERROR) return std::format("{} <unknown>", ifIndex);
  return std::format("{} \"{}\" ({}, type={}, {})", ifIndex, Narrow(row.Alias),
                     Narrow(row.Description), row.Type,
                     row.MediaConnectState == MediaConnectStateConnected
                         ? "connected"
                         : "disconnected");
}

EgressInterfaces NetworkConfig::DiscoverEgress(NET_LUID tunLuid) {
  EgressInterfaces result;
  for (ADDRESS_FAMILY family : {AF_INET, AF_INET6}) {
    PMIB_IPFORWARD_TABLE2 table = nullptr;
    if (::GetIpForwardTable2(family, &table) != NO_ERROR || !table) continue;

    // Two elections at once. A default route can outlive the link that owns it
    // (unplugged ethernet keeps its route for a while, and a low metric is
    // exactly what a wired adapter has), so a disconnected NIC can win on
    // metric and we would pin every socket to a link with no carrier. Prefer a
    // CONNECTED interface always, and fall back to the best disconnected one
    // only when nothing connected has a default route — the fallback is still
    // better than 0, which means "follow the route table" and, with the tunnel
    // up, means the tun.
    ULONG bestMetric = ~0u;
    uint32_t bestIndex = 0;
    ULONG bestLinkDownMetric = ~0u;
    uint32_t bestLinkDownIndex = 0;
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

      MIB_IF_ROW2 ifEntry{};
      ifEntry.InterfaceLuid = row.InterfaceLuid;
      // Unknown state counts as connected: never let a lookup failure demote a
      // working adapter.
      bool connected = true;
      if (::GetIfEntry2(&ifEntry) == NO_ERROR)
        connected = ifEntry.MediaConnectState != MediaConnectStateDisconnected;

      if (connected) {
        if (metric < bestMetric) {
          bestMetric = metric;
          bestIndex = row.InterfaceIndex;
        }
      } else if (metric < bestLinkDownMetric) {
        bestLinkDownMetric = metric;
        bestLinkDownIndex = row.InterfaceIndex;
      }
    }
    ::FreeMibTable(table);
    if (bestIndex == 0 && bestLinkDownIndex != 0) {
      LogWarn("egress: no CONNECTED default route for family {}; falling back to "
              "interface {}, whose link is down",
              family == AF_INET ? "ipv4" : "ipv6", bestLinkDownIndex);
      bestIndex = bestLinkDownIndex;
    }
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
