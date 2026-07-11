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

  // Find the best default-route interface indices that are NOT the tun. Used to
  // set the SDK egress binding. Recomputed on every network change.
  static EgressInterfaces DiscoverEgress(NET_LUID tunLuid);

  // Preferred unicast source address of an interface (network byte order),
  // for the split-tunnel driver to rebind excluded sockets to. addr must hold
  // 4 bytes (family AF_INET) or 16 (AF_INET6). Returns false if none found.
  static bool InterfaceSourceAddress(uint32_t ifIndex, int family, uint8_t* addr);

 private:
  NET_LUID tunLuid_;
  bool applied_ = false;
  TunnelNetworkSettings settings_;
};

}  // namespace urnw
