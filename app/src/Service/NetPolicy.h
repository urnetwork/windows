// THE table. One list of IPv4 prefixes that must bypass the tunnel, and one
// derived list of the prefixes the tun therefore captures. Everything that has
// an opinion about "is this address local" reads this file:
//
//   * NetworkConfig::Apply installs kTunCaptureV4 as the tun's route set.
//   * WfpPolicy's LAN permit is built from kLocalBypassV4.
//
// That coupling is the whole point. The two lists are the SAME decision seen
// from opposite sides — the route table says "these go out the physical NIC",
// the firewall says "these may go out the physical NIC" — and the moment they
// are written from two tables they can disagree. A firewall that permits less
// than the routes send is a broken LAN; a firewall that permits MORE than the
// routes send is a silent leak, and it is silent precisely because the route
// table looks right. So the capture set is not a table at all here: it is
// COMPUTED from the bypass set at compile time, and cannot drift from it.
//
// (research: docs/superpowers/research/2026-08-08-windows-leak-prevention-wfp.md
//  §7.2, "the LAN exemption must match kIncludedV4Routes")
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace urnw::net {

// {network in HOST byte order, prefix length}
struct V4Prefix {
  uint32_t network;
  uint8_t prefix;

  constexpr bool operator==(const V4Prefix& o) const {
    return network == o.network && prefix == o.prefix;
  }
};

// ---------------------------------------------------------------------------
// THE TABLE — the only place this decision is written down.
//
// These bypass the tunnel and are permitted by the firewall, matching Android
// (MainService excludeRoute), iOS (NEIPv4Settings.excludedRoutes) and Linux.
//
// Two ranges are DELIBERATELY ABSENT, against Mullvad's g_ipv4LanNets, and the
// reasoning is recorded here because the research note (§7.2) asks for it to be
// a decision rather than an inheritance:
//
//   169.254.0.0/16 — link-local/APIPA. Mullvad exempts it; we do not, because
//     OUR OWN TUN ADDRESS LIVES THERE (169.254.2.1/24, NetworkConfig.cpp). The
//     tun's on-link /24 is installed by the address assignment and already wins
//     on longest prefix, so carving the whole /16 out to the physical NIC would
//     buy nothing our own address does not already have, while handing every
//     link-local destination — including 169.254.169.254, the cloud metadata
//     address — to the physical adapter while connected. Link-local is not
//     routable off the link, so keeping it captured costs nothing.
//
//   224.0.0.0/3 — multicast + reserved + 255.255.255.255. Mullvad exempts
//     224.0.0.0/24, 239.0.0.0/8 and 255.255.255.255/32 as "LAN multicast", and
//     the research's filter C6 would permit them. WE DO NOT, and permitting
//     them would CONTRADICT §5.6 of the same note: 224.0.0.252 is LLMNR and
//     224.0.0.251 is mDNS, i.e. the two name-resolution channels §5.6 requires
//     us to block, and they live inside the 224.0.0.0/24 that C6 permits. In
//     Mullvad's design the port-53/5353/5355 blocks sit in a SEPARATE sublayer
//     where block beats permit, so C6 is survivable there; ours are in a
//     separate sublayer too (WfpPolicy), but relying on that to undo a permit
//     we did not need to write is one refactor away from a leak. Multicast
//     stays captured into the tun, where the SDK drops it.
//     DHCP's 255.255.255.255 broadcast is unaffected: the DHCP client sends it
//     on a bound interface, not through a route lookup, and the firewall
//     permits it by PORT (see WfpPolicy's DHCP filters), not by address range.
//
// Changing this table changes the machine's routing table. p7-gates.ps1's
// $P7RouteCnt and the static_assert below both have to move with it, on
// purpose.
// ---------------------------------------------------------------------------
inline constexpr V4Prefix kLocalBypassV4[] = {
    {0x0A000000u, 8},   // 10.0.0.0/8
    {0xAC100000u, 12},  // 172.16.0.0/12
    {0xC0A80000u, 16},  // 192.168.0.0/16
};

namespace detail {

constexpr uint32_t PrefixMask(uint8_t prefix) {
  return prefix == 0 ? 0u : (0xFFFFFFFFu << (32 - prefix));
}

// Is `inner` entirely inside `outer`?
constexpr bool Contains(const V4Prefix& outer, const V4Prefix& inner) {
  if (outer.prefix > inner.prefix) return false;
  return (inner.network & PrefixMask(outer.prefix)) == outer.network;
}

// The aligned complement of kLocalBypassV4 within 0.0.0.0/0, emitted low to
// high. Classic recursive prefix subtraction, done with an explicit stack so it
// is constexpr-evaluable: take a candidate; if a bypass prefix covers it, drop
// it; if it covers no bypass prefix, keep it; otherwise split in half and
// recurse. The result is minimal — no two emitted prefixes are adjacent-and-
// mergeable — because a node is only split when it genuinely straddles.
//
// `out == nullptr` counts without writing, which is how the array size below is
// derived from the same code that fills it.
constexpr std::size_t ComplementV4(V4Prefix* out) {
  V4Prefix stack[64]{};
  int sp = 0;
  stack[sp++] = V4Prefix{0u, 0};
  std::size_t n = 0;
  while (sp > 0) {
    const V4Prefix cur = stack[--sp];
    bool covered = false;
    bool straddles = false;
    for (const V4Prefix& b : kLocalBypassV4) {
      if (Contains(b, cur)) {
        covered = true;
        break;
      }
      if (Contains(cur, b)) straddles = true;
    }
    if (covered) continue;
    if (!straddles) {
      if (out) out[n] = cur;
      ++n;
      continue;
    }
    // A straddling node must be splittable: it strictly contains some bypass
    // prefix, so its own length is strictly less than 32.
    const uint8_t half = static_cast<uint8_t>(cur.prefix + 1);
    const uint32_t step = 1u << (32 - half);
    // Push high first so the low half pops first and the output stays sorted.
    stack[sp++] = V4Prefix{cur.network + step, half};
    stack[sp++] = V4Prefix{cur.network, half};
  }
  return n;
}

}  // namespace detail

// How many prefixes the tun captures. DERIVED, not typed in.
inline constexpr std::size_t kTunCaptureV4Count = detail::ComplementV4(nullptr);

// The tun's route set: all of 0.0.0.0/0 except kLocalBypassV4. Like the old
// 0.0.0.0/1 + 128.0.0.0/1 capture these sort above the physical default route
// without deleting it; the bypassed ranges fall through to the physical and
// connected routes.
inline constexpr std::array<V4Prefix, kTunCaptureV4Count> kTunCaptureV4 = [] {
  std::array<V4Prefix, kTunCaptureV4Count> a{};
  detail::ComplementV4(a.data());
  return a;
}();

// The datapath this number describes was validated arithmetically and is what
// p7-gates.ps1 asserts against ($P7RouteCnt). A change here is a change to the
// machine's routing table, so it fails the build rather than surprising a gate.
static_assert(kTunCaptureV4Count == 31,
              "the tun capture set changed size; update p7-gates.ps1's "
              "$P7RouteCnt and re-run gate D before shipping this");

// Would this address be sent out the PHYSICAL adapter while the tunnel is up?
// Used to sanity-check the tunnel's own resolvers: a resolver inside the bypass
// set is unreachable through the tun, and with the WFP port-53 block in force
// that is not a leak but a total DNS outage.
constexpr bool IsLocalBypassV4(uint32_t hostOrderAddr) {
  for (const V4Prefix& b : kLocalBypassV4) {
    if ((hostOrderAddr & detail::PrefixMask(b.prefix)) == b.network) return true;
  }
  return false;
}

}  // namespace urnw::net
