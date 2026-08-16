// SPDX-License-Identifier: MPL-2.0
#include "NetworkConfig.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <netioapi.h>

#include <atomic>
#include <cwchar>

#include "Log.h"
#include "NetPolicy.h"
#include "Strings.h"

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")

namespace urnw {
namespace {

bool ParseV4(const std::string& s, IN_ADDR& out) {
  return ::inet_pton(AF_INET, s.c_str(), &out) == 1;
}

// Windows defaults IPv6 interfaces to LinkLocalAlwaysOn, which can synthesize
// a fe80:: address even when the app never supplies an IPv6 address. Keep the
// Wintun interface genuinely IPv4-only without touching IPv6 on any physical
// interface. Router discovery/default routes are disabled as a second guard,
// and an address created before this policy landed is removed explicitly.
bool EnforceIpv4OnlyTunnelInterface(NET_LUID tun) {
  MIB_IPINTERFACE_ROW row;
  ::InitializeIpInterfaceEntry(&row);
  row.Family = AF_INET6;
  row.InterfaceLuid = tun;
  DWORD err = ::GetIpInterfaceEntry(&row);
  if (err == ERROR_NOT_FOUND || err == ERROR_FILE_NOT_FOUND ||
      err == ERROR_NOT_SUPPORTED) {
    LogInfo("netcfg: tunnel has no IPv6 interface row; IPv4-only policy already "
            "satisfied");
    return true;
  }
  if (err != NO_ERROR) {
    LogError("netcfg: cannot inspect tunnel IPv6 interface policy: {}", err);
    return false;
  }

  row.LinkLocalAddressBehavior = LinkLocalAlwaysOff;
  row.RouterDiscoveryBehavior = RouterDiscoveryDisabled;
  row.AdvertisingEnabled = FALSE;
  row.AdvertiseDefaultRoute = FALSE;
  row.DisableDefaultRoutes = TRUE;
  err = ::SetIpInterfaceEntry(&row);
  if (err != NO_ERROR) {
    LogError("netcfg: cannot suppress IPv6 on Wintun: {}", err);
    return false;
  }

  PMIB_UNICASTIPADDRESS_TABLE addresses = nullptr;
  err = ::GetUnicastIpAddressTable(AF_INET6, &addresses);
  if (err != NO_ERROR) {
    LogError("netcfg: cannot verify Wintun has no IPv6 addresses: {}", err);
    return false;
  }
  bool clean = true;
  size_t removed = 0;
  for (ULONG i = 0; i < addresses->NumEntries; ++i) {
    const auto& address = addresses->Table[i];
    if (address.InterfaceLuid.Value != tun.Value) continue;
    const DWORD deleteErr = ::DeleteUnicastIpAddressEntry(&address);
    if (deleteErr == NO_ERROR || deleteErr == ERROR_NOT_FOUND) {
      ++removed;
    } else {
      clean = false;
      LogError("netcfg: cannot remove an automatically generated Wintun IPv6 "
               "address: {}",
               deleteErr);
    }
  }
  ::FreeMibTable(addresses);
  if (clean) {
    LogInfo("netcfg: Wintun IPv6 disabled (link-local addresses removed={}); "
            "physical IPv6 unchanged",
            removed);
  }
  return clean;
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

// The tun's capture set — the whole IPv4 space EXCEPT the ranges that bypass
// the tunnel. It is NOT written here any more: it is derived at compile time
// from net::kLocalBypassV4, which is also what the WFP LAN permit is built
// from, so the route table and the firewall cannot disagree about which
// addresses are local. See NetPolicy.h for why that coupling matters and for
// the 169.254/16 + 224.0.0.0/3 decisions.
constexpr const auto& kIncludedV4Routes = net::kTunCaptureV4;

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

// DnsFlushResolverCache, resolved by NAME.
//
// It is exported by dnsapi.dll (ordinal 85 on 26100) and declared in NO SDK
// header — windns.h has DnsFlushResolverCacheEntry_* and not this — so an
// import-library reference would be a build-time bet on an undocumented symbol.
// GetProcAddress makes its absence a logged no-op at runtime instead of a link
// error, which is the right shape for something whose failure must not fail a
// connect.
//
// The handle is deliberately never freed: dnsapi.dll is a system DLL that is
// already resident in this process, and a FreeLibrary race with a flush in
// flight buys nothing.
using DnsFlushFn = BOOL(WINAPI*)(void);

DnsFlushFn ResolveDnsFlush() {
  static const DnsFlushFn fn = [] () -> DnsFlushFn {
    HMODULE mod = ::LoadLibraryExW(L"dnsapi.dll", nullptr,
                                   LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!mod) return nullptr;
    return reinterpret_cast<DnsFlushFn>(reinterpret_cast<void*>(
        ::GetProcAddress(mod, "DnsFlushResolverCache")));
  }();
  return fn;
}

}  // namespace

bool NetworkConfig::ResolverCacheFlushAvailable() {
  return ResolveDnsFlush() != nullptr;
}

bool NetworkConfig::FlushResolverCache() {
  const DnsFlushFn flush = ResolveDnsFlush();
  if (!flush) {
    LogWarn("dns: this system does not export DnsFlushResolverCache, so the "
            "machine-wide resolver cache was NOT flushed. Names resolved before "
            "this transition keep being served from it for the rest of their "
            "TTL, to every process on the box.");
    return false;
  }
  if (!flush()) {
    LogWarn("dns: DnsFlushResolverCache failed ({}); the machine-wide resolver "
            "cache still holds answers from before this transition. Not fatal — "
            "a stale cache is not worth failing a working tunnel over.",
            ::GetLastError());
    return false;
  }
  LogInfo("dns: flushed the machine-wide resolver cache, so no answer from "
          "before this transition survives it");
  return true;
}

bool NetworkConfig::IsIpv4OnlyTunnelSettings(
    const TunnelNetworkSettings& settings) {
  IN_ADDR address{};
  if (!ParseV4(settings.local_address_v4, address) || settings.prefix_v4 == 0 ||
      settings.prefix_v4 > 32) {
    return false;
  }
  for (const auto& server : settings.dns_servers_v4) {
    IN_ADDR dns{};
    if (!ParseV4(server, dns)) return false;
  }
  return true;
}

bool NetworkConfig::Apply(const TunnelNetworkSettings& settings) {
  if (!IsIpv4OnlyTunnelSettings(settings)) {
    LogError("netcfg: refusing non-IPv4 tunnel configuration (addr={}/{} "
             "dns-count={})",
             settings.local_address_v4, settings.prefix_v4,
             settings.dns_servers_v4.size());
    return false;
  }
  settings_ = settings;

  // Arm the crash path before the FIRST mutation, not after the last one: a
  // crash halfway through the route loop must still be cleanable.
  ArmCrashRevert(tunLuid_);

  // Do this before installing the IPv4 address or routes. A newly-created
  // Wintun can otherwise gain an automatic IPv6 link-local address merely by
  // coming up, despite receiving no IPv6 settings from the SDK.
  if (!EnforceIpv4OnlyTunnelInterface(tunLuid_)) {
    DisarmCrashRevert();
    return false;
  }

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

  // --- DNS ------------------------------------------------------------------
  // NOT fatal, deliberately: tearing a working tunnel down because its
  // resolvers did not take would trade a DNS problem for a connectivity one.
  // But it is not silent either, and that WAS the bug. Before dns_applied_
  // existed, a failure here left applied_ = true, Apply returning true, the
  // status reporting state=up + routes_installed=true, and a single LogWarn as
  // the only trace — so the UI said Connected while every query went out in the
  // clear. The flag is the honest half of "the tunnel is up but not all of it
  // is": TunnelStatus::dns_applied carries it to the app, which renders a
  // degraded state instead of a clean one.
  //
  // With the WFP layer armed the consequence changes shape but not severity:
  // WfpPolicy hard-blocks port 53 to everything except the tun's resolvers, so
  // a failure here means no adapter points at a permitted resolver and DNS
  // stops entirely rather than leaking. Fail-closed is the better of the two,
  // and it is still a state the user must be told about.
  dns_applied_ = false;
  if (!settings.dns_servers_v4.empty()) {
    dns_applied_ =
        SetTunDns(tunLuid_, settings.dns_servers_v4, settings.dns_search);
    if (!dns_applied_)
      LogError("netcfg: tun DNS NOT SET — the tunnel is up but name resolution "
               "is not tunnelled (R6). Without the firewall layer queries go to "
               "the physical adapter's resolver in the clear; with it they are "
               "blocked and DNS fails. Reported to the app as dns_applied=false.");
  } else {
    LogError("netcfg: NO tun DNS servers supplied — same consequence as a failed "
             "set; reported as dns_applied=false");
  }

  // Cross-check the resolvers against the ONE table (NetPolicy.h). A resolver
  // inside the bypass set is routed out the physical NIC, not the tun, so the
  // firewall's tun-scoped DNS permit will not match it and the port-53 block
  // will kill resolution outright. This can only happen if the SDK's resolver
  // address and our bypass table are changed independently — which is exactly
  // the class of drift NetPolicy.h exists to make impossible, so say so loudly
  // rather than debug it later from a "DNS is broken" report.
  for (const auto& server : settings.dns_servers_v4) {
    IN_ADDR s{};
    if (!ParseV4(server, s)) continue;
    if (net::IsLocalBypassV4(ntohl(s.S_un.S_addr))) {
      LogError("netcfg: tunnel resolver {} falls inside a range that BYPASSES "
               "the tunnel (NetPolicy.h kLocalBypassV4). It is not reachable "
               "through the tun and the firewall's port-53 block will drop it. "
               "This is a table/SDK disagreement, not a network fault.",
               server);
    }
  }

  applied_ = true;
  LogInfo("netcfg: applied addr={}/{} mtu={} dns={} dns_applied={}",
          settings.local_address_v4, settings.prefix_v4, settings.mtu,
          settings.dns_servers_v4.size(), dns_applied_ ? "yes" : "NO");
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
  dns_applied_ = false;
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
                                       const wchar_t* adapterName, bool remove) {
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

  // ConvertInterfaceGuidToLuid RESOLVES A PERSISTENT MAPPING, NOT THE LIVE
  // INTERFACE TABLE. This line used to carry a comment claiming the opposite —
  // "a success here means an interface with our pinned GUID exists right now" —
  // and that claim is false. Measured on this machine, unelevated, with no
  // URnetwork adapter present anywhere:
  //
  //   ConvertInterfaceGuidToLuid({C4E5F6A7-...}) -> NO_ERROR, luid 0x35008000000000
  //   ConvertInterfaceLuidToIndex                -> NO_ERROR, ifIndex 9
  //   ConvertInterfaceLuidToAlias                -> NO_ERROR, "URnetwork"
  //   GetIfEntry2(luid)                          -> NO_ERROR, "URnetwork"
  //
  // ...while Get-NetAdapter -IncludeHidden, `netsh interface ipv4 show
  // interfaces`, Get-NetIPInterface and Get-NetRoute ALL agree there is no
  // interface 9 and no route on it.
  //
  // Windows keeps the GUID <-> NetLuidIndex binding, and an interface record
  // behind it, after the miniport is gone. So once this machine has EVER created
  // the tun, all of those lookups keep succeeding forever. What that produced
  // was a PERMANENT FALSE ORPHAN: every start warned "a previous run did not
  // revert", and `urnetworkd revert` reported orphaned_interfaces=1 on a machine
  // with nothing wrong with it — including at 05:33 on 2026-08-09, while the
  // owner was using it to recover from the shutdown hang.
  //
  // That is not a cosmetic bug. This warning and the active marker are, by the
  // design's own words, "the only way the owner learns that a crash cost them
  // their network". A warning that is always on is a warning that is never read.
  //
  // The candidate is therefore a LEAD, confirmed against the live IP stack below.
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

  int confirmed = 0;
  for (int i = 0; i < candidateCount; ++i) {
    NET_LUID luid{};
    luid.Value = candidates[i];

    // THE EVIDENCE TEST. A candidate only counts if the IP STACK still has this
    // interface — not merely if a name-to-LUID lookup resolves it (see the note
    // on ConvertInterfaceGuidToLuid above; GetIfEntry2 is no better, it answers
    // for the phantom too).
    //
    // GetIpInterfaceEntry is the right question because it queries the very
    // table that holds routes and per-interface DNS: the two things this sweep
    // exists to take back. An interface absent from it CANNOT be holding either.
    //
    // AND THIS CANNOT PRODUCE A FALSE NEGATIVE FOR THE CASE THAT MATTERS. A real
    // orphan stranding the machine is, by definition, one with our routes still
    // pointed at it — and a route cannot exist on an interface the IP stack does
    // not have. So every orphan capable of doing harm still passes this gate.
    // The only thing filtered out is the harmless residue, which is exactly what
    // the file's own "prefer the miss" rule asks for.
    //
    // Both families are tried: we only ever install v4 routes and v4 DNS, but
    // accepting either is the direction that errs towards sweeping.
    bool live = false;
    for (const ADDRESS_FAMILY family : {AF_INET, AF_INET6}) {
      MIB_IPINTERFACE_ROW ip{};
      ip.Family = family;
      ip.InterfaceLuid = luid;
      if (::GetIpInterfaceEntry(&ip) == NO_ERROR) {
        live = true;
        break;
      }
    }

    MIB_IF_ROW2 row{};
    row.InterfaceLuid = luid;
    std::string alias = (::GetIfEntry2(&row) == NO_ERROR) ? Narrow(row.Alias)
                                                          : std::string("<gone>");
    if (!live) {
      // Debug, not warn: on a machine that has ever run the tunnel this is the
      // NORMAL steady state, and it is the noise that used to drown the real
      // signal. Kept rather than dropped so the residue is still explicable to
      // anyone reading the log after a crash.
      LogDebug("netcfg: the name \"{}\" (luid {:#x}) still resolves, but the ip "
               "stack has no such interface — stale guid/alias binding from an "
               "adapter that is already gone, holding no route and no dns. Not "
               "an orphan; not swept.",
               alias, luid.Value);
      continue;
    }
    ++confirmed;
    if (!remove) {
      LogWarn("netcfg: ORPHANED tun interface \"{}\" (luid {:#x}) present at "
              "startup — a previous run did not revert. NOT cleaning it: this "
              "process is observe-only (rpc-only mode) and removing routes "
              "needs elevation. To take the routes back: STOP THIS PROCESS "
              "FIRST, then run `urnetworkd revert` from an elevated prompt — "
              "revert refuses while a urnetworkd holds the control pipe.",
              alias, luid.Value);
      continue;
    }
    int removed = DeleteTunnelRoutes(luid);
    ClearTunnelDns(luid);
    LogWarn(
        "netcfg: ORPHANED tun interface \"{}\" (luid {:#x}) present at startup — "
        "a previous run did not revert; removed {} stale routes and cleared its "
        "DNS",
        alias, luid.Value, removed);
  }
  // CONFIRMED orphans, not candidates. This return value is what
  // ReportAndClearPriorState turns into "found N tun interface(s) from an
  // earlier run" and what `urnetworkd revert` prints as orphaned_interfaces —
  // i.e. it is the number the owner reads when deciding whether a crash cost
  // them their network. Returning the unconfirmed candidate count made every
  // clean start on this machine report a crash that never happened.
  return confirmed;
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

std::vector<std::string> NetworkConfig::HostResolversV4(NET_LUID excludeLuid) {
  std::vector<std::string> out;

  // The buffer requirement can grow between the sizing call and the real one
  // (an adapter arriving), so retry rather than assume.
  ULONG size = 16 * 1024;
  std::vector<uint8_t> buf;
  ULONG err = ERROR_BUFFER_OVERFLOW;
  for (int attempt = 0; attempt < 4 && err == ERROR_BUFFER_OVERFLOW; ++attempt) {
    buf.assign(size, 0);
    err = ::GetAdaptersAddresses(
        AF_UNSPEC,
        GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
            GAA_FLAG_SKIP_FRIENDLY_NAME,
        nullptr, reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buf.data()), &size);
  }
  if (err != NO_ERROR) {
    LogWarn("netcfg: GetAdaptersAddresses failed while reading the host's "
            "resolvers: {}. The firewall policy will treat this machine as "
            "having no resolver, which stands the port-53 block down rather "
            "than blocking DNS with no path back.",
            err);
    return out;
  }

  for (auto* a = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buf.data()); a;
       a = a->Next) {
    if (a->OperStatus != IfOperStatusUp) continue;
    if (excludeLuid.Value != 0 && a->Luid.Value == excludeLuid.Value) continue;
    for (auto* d = a->FirstDnsServerAddress; d; d = d->Next) {
      const SOCKADDR* sa = d->Address.lpSockaddr;
      if (!sa || sa->sa_family != AF_INET) continue;
      const auto* sin = reinterpret_cast<const sockaddr_in*>(sa);
      const uint32_t host = ::ntohl(sin->sin_addr.S_un.S_addr);
      if (host == 0) continue;                            // "none" placeholder
      if ((host & 0xFF000000u) == 0x7F000000u) continue;  // 127/8 — see header
      char text[INET_ADDRSTRLEN] = {};
      if (!::inet_ntop(AF_INET, &sin->sin_addr, text, sizeof(text))) continue;
      std::string s(text);
      bool seen = false;
      for (const auto& have : out)
        if (have == s) seen = true;
      if (!seen) out.push_back(std::move(s));
    }
  }
  return out;
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
