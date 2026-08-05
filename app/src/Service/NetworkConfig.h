// Applies to the wintun adapter what NEPacketTunnelNetworkSettings applies on
// macOS: the tunnel local address, MTU, split-default routes, and DNS. Also
// discovers the physical egress interface (best non-tun default route) for the
// R1 socket self-exclusion.
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

struct TunnelNetworkSettings {
  std::string local_address_v4;      // e.g. "169.254.2.1" (DeviceLocal.tunnelLocalAddress)
  uint8_t prefix_v4 = 24;
  uint32_t mtu = 1440;
  std::vector<std::string> dns_servers;  // resolvers to set on the tun interface
  std::string dns_search;                // optional search domain
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

  // Remove the routes/addresses/DNS added by Apply(), restoring prior state.
  void Revert();

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
  // device node), and to make the situation visible when it does.

  // Delete every route Apply() installs from tunLuid. Allocation-free and
  // lock-free; safe from an unhandled-exception filter. Returns how many
  // entries were actually removed.
  static int DeleteTunnelRoutes(NET_LUID tunLuid);

  // Clear the DNS servers set on tunLuid. Allocation-free (no server list to
  // marshal), so it too is safe from a crash path.
  static void ClearTunnelDns(NET_LUID tunLuid);

  // Publish/withdraw the LUID that CrashRevert() should clean. Armed by Apply()
  // and withdrawn by Revert(), so the crash path never touches an interface we
  // are not currently responsible for.
  static void ArmCrashRevert(NET_LUID tunLuid);
  static void DisarmCrashRevert();

  // Last-chance revert for abnormal termination. Idempotent, does nothing when
  // nothing is armed, and takes no lock and no allocation — it runs from the
  // unhandled-exception filter and the console control handler, where the
  // process may already be in an undefined state. NOT the mechanism we rely on:
  // see the note above.
  static void CrashRevert();

  // Startup sweep. If an interface carrying the pinned tun GUID — or, in case
  // wintun had to fall back to a different GUID, our adapter alias — still
  // exists when the service starts, a previous run died without unwinding (or
  // the adapter outlived it). Delete our route set and DNS from each and say so
  // loudly. Returns how many orphaned interfaces were found.
  static int SweepOrphanedTunnel(const GUID& tunGuid, const wchar_t* adapterName);

  // Find the best default-route interface indices that are NOT the tun. Used to
  // set the SDK egress binding. Recomputed on every network change.
  static EgressInterfaces DiscoverEgress(NET_LUID tunLuid);

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
  TunnelNetworkSettings settings_;
};

}  // namespace urnw
