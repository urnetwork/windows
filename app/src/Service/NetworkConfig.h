// Applies to the wintun adapter what NEPacketTunnelNetworkSettings applies on
// macOS: the tunnel local address, MTU, split-default routes, and DNS. The
// Wintun interface is also prevented from synthesizing an IPv6 link-local
// address; physical-interface IPv6 is untouched. Also discovers the physical
// egress interface (best non-tun default route) for the R1 socket self-exclusion.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ifdef.h>  // NET_LUID

namespace urnw {

// Mirrors sdk.GetDefaultTunnelMtu/connect.DefaultMtu. One full encrypted
// tunnel packet plus the UR envelope fits one initial H3 QUIC DATAGRAM.
inline constexpr uint32_t kTunnelMtu = 1100;

struct TunnelNetworkSettings {
  std::string local_address_v4;      // e.g. "169.254.2.1" (DeviceLocal.tunnelLocalAddress)
  uint8_t prefix_v4 = 24;
  uint32_t mtu = kTunnelMtu;
  std::vector<std::string> dns_servers_v4;  // IPv4 resolvers set on the tun interface
  std::string dns_search;                   // optional search domain
};

// Physical egress selection for R1.
struct EgressInterfaces {
  uint32_t index4 = 0;  // interface index of the best IPv4 default route (non-tun)
  uint32_t index6 = 0;  // interface index of the best IPv6 default route (non-tun)
};

class NetworkConfig {
 public:
  explicit NetworkConfig(NET_LUID tunLuid) : tunLuid_(tunLuid) {}

  // Apply address + MTU + routes + DNS to the tun interface. Idempotent-ish:
  // Revert() undoes what Apply() added.
  bool Apply(const TunnelNetworkSettings& settings);

  // Pure preflight used by Apply and the self-test. Remote providers currently
  // forward IPv4 only, so no IPv6 address or DNS transport may reach Wintun.
  static bool IsIpv4OnlyTunnelSettings(const TunnelNetworkSettings& settings);

  // Remove the routes/addresses/DNS added by Apply(), restoring prior state.
  void Revert();

  // True only when the tun's resolvers were actually accepted by the stack.
  // Apply() deliberately SUCCEEDS when DNS fails — a working tunnel is not
  // worth tearing down over its resolvers — so this is the field that separates
  // "connected" from "connected and resolving through the tunnel". It reaches
  // the app as TunnelStatus::dns_applied. Without it a DNS failure was visible
  // only as one warning in the service log while every surface said Connected.
  bool DnsApplied() const { return applied_ && dns_applied_; }

  // --- crash safety (the worst failure this service can have) ---------------
  //
  // Apply() only ever writes state that hangs off the TUN interface: its
  // unicast address, its MTU/metric, routes whose InterfaceLuid is the tun, and
  // that interface's DNS servers. Nothing on the physical adapter is touched
  // and the physical default route is never deleted — the tun captures traffic
  // by adding higher-sorting prefixes above it. So the primary restore path is
  // the OS's own: when this process dies for ANY reason the wintun adapter it
  // created goes away with it (wintun creates the adapter as a software device
  // owned by this process), and the stack drops every route and address bound
  // to that interface. The machine heals without us running a line of code.
  //
  // The three below are defence in depth for the cases where that does not
  // happen (an adapter that outlives the process, a driver that leaks the
  // device node), and to make the situation visible when it does. Note how
  // little the FIRST of them actually covers — the SDK embeds the Go runtime,
  // whose vectored handler preempts a top-level exception filter, a Go panic
  // raises no SEH at all, and TerminateProcess runs nothing. The sweep is the
  // one that runs regardless of how the last process died.

  // Delete every route Apply() installs from tunLuid. Allocates nothing, takes
  // no lock, and calls only into the tcpip stack; safe from an
  // unhandled-exception filter. Returns how many entries were actually removed.
  static int DeleteTunnelRoutes(NET_LUID tunLuid);

  // Clear the DNS servers and search list set on tunLuid.
  // NOT crash-path safe: SetInterfaceDnsSettings is an RPC to the dnscache
  // service and can block, and blocking inside an exception filter leaves the
  // process wedged rather than dead. Orderly paths only.
  static void ClearTunnelDns(NET_LUID tunLuid);

  // --- the machine-wide resolver cache --------------------------------------
  //
  // Setting the tun's resolvers changes where the NEXT query goes. It does not
  // touch the ANSWERS the machine already has, and those live in one
  // machine-wide cache (the DNS Client service, inside svchost.exe) that every
  // process on the box is served from. So without this, everything resolved
  // while the tunnel was coming up — through the HOST's resolvers, over the
  // physical NIC, in the clear, which is exactly what WfpState::Connecting's
  // filter 9b permits — keeps being handed out for the rest of its TTL AFTER the
  // tunnel is up. A cache-shaped hole straight through "DNS is pinned to the
  // tunnel's resolvers while connected": the queries stop leaking at the
  // Connected edge, the answers do not.
  //
  // It matters at BOTH edges and for opposite reasons. Coming up, host answers
  // must not outlive the host path. Going down, TUNNEL answers must not outlive
  // the tunnel — a name the exit resolved (a split-horizon or geo-steered
  // record, or simply an address only reachable through the tunnel) would
  // otherwise be served to every process on the machine long after the tun is
  // gone. WireGuard and Mullvad both flush; this does it at both transitions.
  //
  // Returns false when the flush did not happen. EVERY CALLER IS EXPECTED TO
  // IGNORE THAT: a stale cache is a privacy and correctness wart, not a reason
  // to fail a connect that otherwise worked. The failure is logged, never
  // thrown.
  //
  // NOT crash-path safe, for precisely ClearTunnelDns's reason: this is an RPC
  // into the DNS Client service and it can block. Orderly paths only — never
  // from CrashRevert(), which is why CrashRevert still does routes and nothing
  // else.
  static bool FlushResolverCache();

  // True when this OS exports the flush entry point at all. RESOLVES the symbol;
  // does NOT flush. It exists so the selftest can prove the flush path is
  // reachable on the machine it runs on without touching that machine's DNS
  // cache — the entry point is undocumented and declared in no SDK header, so
  // "it links" proves nothing about it.
  static bool ResolverCacheFlushAvailable();

  // Publish/withdraw the LUID that CrashRevert() should clean. Armed by Apply()
  // and withdrawn by Revert(), so the crash path never touches an interface we
  // are not currently responsible for.
  static void ArmCrashRevert(NET_LUID tunLuid);
  static void DisarmCrashRevert();

  // Last-chance ROUTE revert for abnormal termination. Idempotent, does nothing
  // when nothing is armed, takes no lock and allocates nothing — it runs from
  // the unhandled-exception filter and the console control handler, where the
  // process may already be in an undefined state and where blocking is worse
  // than doing less. Routes only, deliberately: see ClearTunnelDns above. NOT
  // the mechanism we rely on — see the note at the top of this block.
  //
  // AND IT DELIBERATELY HAS NO WFP PATH. Do not "fix" the omission.
  // WfpPolicy's filters are registered on an engine opened with
  // FWPM_SESSION_FLAG_DYNAMIC, so BFE destroys every one of them when this
  // process dies for ANY reason, including the reasons that run no user code at
  // all (TerminateProcess, a bugcheck). There is nothing left for a crash
  // handler to undo. Adding one would be actively harmful: FwpmEngineClose0 is
  // an RPC to the Base Filtering Engine, exactly the class of call
  // ClearTunnelDns is excluded from above, and an RPC that blocks inside an
  // exception filter wedges the process instead of letting it die — leaving a
  // hung LocalSystem service still holding the routes.
  static void CrashRevert();

  // Startup sweep. If an interface carrying the pinned tun GUID — or, in case
  // wintun had to fall back to a different GUID, our adapter alias — still
  // exists when the service starts, a previous run died without unwinding (or
  // the adapter outlived it). Delete our route set and DNS from each and say so
  // loudly. Returns how many orphaned interfaces were found.
  //
  // remove=false makes it OBSERVE ONLY: it still finds and reports orphans but
  // deletes no route and clears no DNS. That is what the unelevated rpc-only
  // mode uses — the removal needs privilege it does not have, and a mode whose
  // entire promise is "this will not touch your network" must not open by
  // rewriting the route table. Reporting an orphan it cannot clean is still
  // worth doing: it tells the owner to run `urnetworkd revert` elevated.
  static int SweepOrphanedTunnel(const GUID& tunGuid, const wchar_t* adapterName,
                                 bool remove = true);

  // Find the best default-route interface indices that are NOT the tun. Used to
  // set the SDK egress binding. Recomputed on every network change.
  static EgressInterfaces DiscoverEgress(NET_LUID tunLuid);

  // Every IPv4 resolver the HOST is currently configured with, on any
  // operational adapter except excludeLuid (pass the tun's), deduplicated and
  // in adapter order. Read-only: GetAdaptersAddresses, no elevation, no state.
  //
  // This is what the leak-prevention policy has to permit on port 53 while
  // there is no tunnel, because the service's own name resolution does not come
  // out of urnetworkd.exe — Go on Windows resolves through GetAddrInfoW, whose
  // wire query is issued by the DNS Client service in svchost.exe. See filter
  // 9b in WfpPolicy.cpp.
  //
  // Loopback resolvers (127.0.0.0/8) are deliberately EXCLUDED. A local stub
  // is already reachable via the loopback permit, but the stub's own upstream
  // is a separate port-53 socket that the policy still blocks, so counting it
  // would claim a path that dead-ends. Excluding it makes the caller correctly
  // conclude "no path" and stand the port-53 block down instead.
  static std::vector<std::string> HostResolversV4(NET_LUID excludeLuid);

  // Preferred unicast source address of an interface (network byte order),
  // for the split-tunnel driver to rebind excluded sockets to. addr must hold
  // 4 bytes (family AF_INET) or 16 (AF_INET6). Returns false if none found.
  static bool InterfaceSourceAddress(uint32_t ifIndex, int family, uint8_t* addr);

  // Human-readable "12 \"Wi-Fi\" (Intel AX211)" for logs. Empty index or an
  // interface that no longer exists yields a placeholder rather than failing —
  // this only ever feeds a log line.
  static std::string DescribeInterface(uint32_t ifIndex);

 private:
  NET_LUID tunLuid_;
  bool applied_ = false;
  bool dns_applied_ = false;
  TunnelNetworkSettings settings_;
};

}  // namespace urnw
