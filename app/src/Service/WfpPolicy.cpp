// SPDX-License-Identifier: MPL-2.0
#include "WfpPolicy.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <fwpmu.h>

#include <cstring>

#include "Log.h"
#include "NetPolicy.h"
#include "Strings.h"

#pragma comment(lib, "fwpuclnt.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "ws2_32.lib")

namespace urnw {
namespace {

// ---------------------------------------------------------------------------
// Our object identities. Fixed forever: the startup purge finds leftovers by
// provider GUID, so changing one of these orphans anything an older build left
// behind, permanently and invisibly.
// ---------------------------------------------------------------------------

// {6D2B1C40-9E77-4B58-8F5A-1C0E9D3A7B10}
constexpr GUID kProviderGuid = {
    0x6d2b1c40, 0x9e77, 0x4b58, {0x8f, 0x5a, 0x1c, 0x0e, 0x9d, 0x3a, 0x7b, 0x10}};
// {6D2B1C41-9E77-4B58-8F5A-1C0E9D3A7B10} — the persistent provider. NOTHING in
// this build ever adds it; the purge knows it so a future build's leftovers, or
// a hand-installed lockdown, can still be cleaned by an older binary.
constexpr GUID kPersistentProviderGuid = {
    0x6d2b1c41, 0x9e77, 0x4b58, {0x8f, 0x5a, 0x1c, 0x0e, 0x9d, 0x3a, 0x7b, 0x10}};
// {6D2B1C42-...} baseline sublayer, {..43} dns sublayer, {..44} persistent.
constexpr GUID kSublayerBaselineGuid = {
    0x6d2b1c42, 0x9e77, 0x4b58, {0x8f, 0x5a, 0x1c, 0x0e, 0x9d, 0x3a, 0x7b, 0x10}};
constexpr GUID kSublayerDnsGuid = {
    0x6d2b1c43, 0x9e77, 0x4b58, {0x8f, 0x5a, 0x1c, 0x0e, 0x9d, 0x3a, 0x7b, 0x10}};
constexpr GUID kSublayerPersistentGuid = {
    0x6d2b1c44, 0x9e77, 0x4b58, {0x8f, 0x5a, 0x1c, 0x0e, 0x9d, 0x3a, 0x7b, 0x10}};

std::string GuidText(const GUID& g) {
  return std::format(
      "{{{:08X}-{:04X}-{:04X}-{:02X}{:02X}-{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}}}",
      g.Data1, g.Data2, g.Data3, g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
      g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
}

bool IsOurProvider(const GUID* key) {
  if (!key) return false;
  return ::IsEqualGUID(*key, kProviderGuid) ||
         ::IsEqualGUID(*key, kPersistentProviderGuid);
}

// --- protocol numbers, in the one place they are written -------------------
constexpr uint8_t kProtoTcp = 6;
constexpr uint8_t kProtoUdp = 17;
constexpr uint8_t kProtoIcmpV6 = 58;

// Ports. HOST byte order everywhere — FWP_UINT16 is not network order.
constexpr uint16_t kPortDns = 53;
constexpr uint16_t kPortDhcpServer = 67;
constexpr uint16_t kPortDhcpClient = 68;
constexpr uint16_t kPortNetbiosNs = 137;
constexpr uint16_t kPortNetbiosDgm = 138;
constexpr uint16_t kPortNetbiosSsn = 139;
constexpr uint16_t kPortDhcpV6Client = 546;
constexpr uint16_t kPortDhcpV6Server = 547;
constexpr uint16_t kPortMdns = 5353;
constexpr uint16_t kPortLlmnr = 5355;

// ICMPv6 neighbour-discovery types.
constexpr uint16_t kIcmpV6RouterSolicit = 133;
constexpr uint16_t kIcmpV6RouterAdvert = 134;
constexpr uint16_t kIcmpV6NeighborSolicit = 135;
constexpr uint16_t kIcmpV6NeighborAdvert = 136;

// libwfp weight classes.
constexpr uint8_t kWeightMax = 15;
constexpr uint8_t kWeightLoopback = 13;
constexpr uint8_t kWeightExempt = 12;
constexpr uint8_t kWeightMedium = 7;
constexpr uint8_t kWeightMin = 0;

// --- condition constructors -------------------------------------------------

WfpCondition CondAppId(const std::wstring& path) {
  WfpCondition c;
  c.field = WfpField::AppId;
  c.app_path = path;
  return c;
}

// Match on the FLAG, never on 127.0.0.0/8 or ::1: local-to-local traffic
// between two of the host's own REAL addresses also carries the loopback flag,
// and an address-based rule misses it.
WfpCondition CondLoopback() {
  WfpCondition c;
  c.field = WfpField::Flags;
  c.match = WfpMatch::FlagsAllSet;
  c.number = FWP_CONDITION_FLAG_IS_LOOPBACK;
  return c;
}

WfpCondition CondLocalInterface(uint64_t luid) {
  WfpCondition c;
  c.field = WfpField::LocalInterface;
  c.number = luid;
  return c;
}

WfpCondition CondProtocol(uint8_t proto) {
  WfpCondition c;
  c.field = WfpField::Protocol;
  c.number = proto;
  return c;
}

WfpCondition CondLocalPort(uint16_t port) {
  WfpCondition c;
  c.field = WfpField::LocalPort;
  c.number = port;
  return c;
}

WfpCondition CondRemotePort(uint16_t port) {
  WfpCondition c;
  c.field = WfpField::RemotePort;
  c.number = port;
  return c;
}

WfpCondition CondRemoteV4(uint32_t hostOrderAddr, uint8_t prefix) {
  WfpCondition c;
  c.field = WfpField::RemoteAddrV4;
  c.v4_addr = hostOrderAddr;
  c.v4_prefix = prefix;
  return c;
}

WfpCondition CondV6(WfpField field, const uint8_t (&addr)[16], uint8_t prefix) {
  WfpCondition c;
  c.field = field;
  std::memcpy(c.v6_addr, addr, 16);
  c.v6_prefix = prefix;
  return c;
}

// Link-local unicast, and the link-scope multicast range that carries every
// NDP/DHCPv6 group we care about (ff02::1, ff02::2, ff02::1:2, and the whole
// solicited-node block ff02::1:ff00:0/104). Neither can be routed off the link,
// so permitting them is not a v6 leak even while all other v6 is blocked.
constexpr uint8_t kV6LinkLocal[16] = {0xfe, 0x80};
constexpr uint8_t kV6LinkLocalMcast[16] = {0xff, 0x02};
constexpr uint8_t kV6DhcpRelayAgentsAndServers[16] = {
    0xff, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01, 0, 0x02};  // ff02::1:2
constexpr uint8_t kV6DhcpAllServers[16] = {
    0xff, 0x05, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01, 0, 0x03};  // ff05::1:3
constexpr uint8_t kV6AllRouters[16] = {
    0xff, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x02};  // ff02::2

// --- the two "UDP or TCP" conditions, kept adjacent -------------------------
// Consecutive conditions on the SAME field are OR'd by WFP; different fields
// are AND'd. That adjacency is the only way to express "UDP or TCP" in one
// filter, so it is a correctness property, not formatting.
void PushUdpTcp(std::vector<WfpCondition>& out) {
  out.push_back(CondProtocol(kProtoUdp));
  out.push_back(CondProtocol(kProtoTcp));
}

WfpFilterSpec Spec(std::string name, WfpLayer layer, WfpSublayer sublayer,
                   bool block, uint8_t weight,
                   std::vector<WfpCondition> conditions = {}) {
  WfpFilterSpec s;
  s.name = std::move(name);
  s.layer = layer;
  s.sublayer = sublayer;
  s.block = block;
  s.weight = weight;
  s.conditions = std::move(conditions);
  return s;
}

}  // namespace

const char* ToString(WfpState s) {
  switch (s) {
    case WfpState::Off: return "off";
    case WfpState::Armed: return "armed";
    case WfpState::Connecting: return "connecting";
    case WfpState::Connected: return "connected";
  }
  return "?";
}

// ---------------------------------------------------------------------------
// BuildFilterSet — pure, no Windows API, no BFE. This is the unit under test.
// ---------------------------------------------------------------------------
std::vector<WfpFilterSpec> BuildFilterSet(WfpState state, const WfpConfig& cfg) {
  std::vector<WfpFilterSpec> f;
  if (state == WfpState::Off) return f;

  const bool connected = state == WfpState::Connected;
  // The ONE state that carries filter 9b. Everything else about Connecting is
  // Armed, byte for byte — which is what makes the superset relation checkable.
  const bool connecting = state == WfpState::Connecting;
  const bool v6 = cfg.block_ipv6;

  // === BASELINE SUBLAYER ===================================================

  // 1. OUR OWN SERVICE. Rank-1 exemption: without it the machine is armed,
  //    blocked, and structurally unable to reconnect — no network and no way
  //    back except an elevated `urnetworkd revert`. This is R1 expressed in
  //    WFP, and it mirrors WireGuard's permitWireGuardService.
  //
  //    Deliberately NOT the app (URnetwork.exe). The app does its own platform
  //    HTTP for account/auth screens, so while ARMED those calls fail — which
  //    is what a kill switch means, and permitting the app would put real user
  //    traffic on the physical NIC in the clear. Connecting is unaffected: the
  //    tunnel is built by THIS service, and the app reaches it over loopback.
  if (!cfg.service_image_path.empty()) {
    for (WfpLayer l : {WfpLayer::ConnectV4, WfpLayer::RecvAcceptV4}) {
      f.push_back(Spec("urnetwork-permit-service-v4", l, WfpSublayer::Baseline,
                       false, kWeightMax, {CondAppId(cfg.service_image_path)}));
    }
    for (WfpLayer l : {WfpLayer::ConnectV6, WfpLayer::RecvAcceptV6}) {
      f.push_back(Spec("urnetwork-permit-service-v6", l, WfpSublayer::Baseline,
                       false, kWeightMax, {CondAppId(cfg.service_image_path)}));
    }
  }

  // 1b. THE UI PROCESS — CONNECTED ONLY.
  //
  //     The same R1 self-exclusion filter 1 gives the service, for the OTHER
  //     process that runs an SDK instance. Read the long comment on
  //     WfpConfig::app_image_path before changing anything here; the short
  //     version is:
  //
  //       * it pairs with the app binding its own SDK egress to the physical
  //         NIC (TunnelStatus::egress_index4). The bind is what moves the app's
  //         sockets out of the tun; this is what stops the baseline floor
  //         blocking them once they are out. Neither half works alone.
  //       * `connected` is the WHOLE gate, and it is written against the state
  //         and not against the config so that populating app_image_path can
  //         never widen Armed or Connecting by accident. Filter 1's comment
  //         says "deliberately NOT the app" — that ruling is about ARMED, and it
  //         is still in force: the state whose promise is "nothing leaves"
  //         permits urnetworkd and nothing else.
  //       * Connecting is excluded for a second, independent reason: the
  //         selftest pins filter 9b as the SINGLE difference between Armed and
  //         Connecting, and a second name there would break the
  //         one-directional-widening property that split rests on.
  if (connected && !cfg.app_image_path.empty()) {
    for (WfpLayer l : {WfpLayer::ConnectV4, WfpLayer::RecvAcceptV4}) {
      f.push_back(Spec("urnetwork-permit-app-v4", l, WfpSublayer::Baseline,
                       false, kWeightMax, {CondAppId(cfg.app_image_path)}));
    }
    for (WfpLayer l : {WfpLayer::ConnectV6, WfpLayer::RecvAcceptV6}) {
      f.push_back(Spec("urnetwork-permit-app-v6", l, WfpSublayer::Baseline,
                       false, kWeightMax, {CondAppId(cfg.app_image_path)}));
    }
  }

  // 2. LOOPBACK. Rank-2: breaks local databases, dev servers, RPC-over-loopback
  //    and our OWN mTLS device-RPC channel, which the app dials on 127.0.0.1.
  for (WfpLayer l : {WfpLayer::ConnectV4, WfpLayer::RecvAcceptV4}) {
    f.push_back(Spec("urnetwork-permit-loopback-v4", l, WfpSublayer::Baseline,
                     false, kWeightLoopback, {CondLoopback()}));
  }
  for (WfpLayer l : {WfpLayer::ConnectV6, WfpLayer::RecvAcceptV6}) {
    f.push_back(Spec("urnetwork-permit-loopback-v6", l, WfpSublayer::Baseline,
                     false, kWeightLoopback, {CondLoopback()}));
  }

  // 3. THE TUN. Everything on it is tunnelled by definition. Connected only —
  //    in Armed there is no tun and a LUID of 0 would match nothing anyway.
  if (connected && cfg.tun_luid != 0) {
    for (WfpLayer l : {WfpLayer::ConnectV4, WfpLayer::RecvAcceptV4}) {
      f.push_back(Spec("urnetwork-permit-tun-v4", l, WfpSublayer::Baseline,
                       false, kWeightExempt,
                       {CondLocalInterface(cfg.tun_luid)}));
    }
    for (WfpLayer l : {WfpLayer::ConnectV6, WfpLayer::RecvAcceptV6}) {
      f.push_back(Spec("urnetwork-permit-tun-v6", l, WfpSublayer::Baseline,
                       false, kWeightExempt,
                       {CondLocalInterface(cfg.tun_luid)}));
    }
  }

  // 4. LAN. Built from net::kLocalBypassV4 — THE SAME TABLE the tun's route set
  //    is derived from — so the firewall permits exactly what the routing table
  //    sends out the physical NIC, no more and no less. Writing this list a
  //    second time is the bug NetPolicy.h exists to prevent.
  //
  //    No v6 LAN permit (fc00::/7): we block v6 entirely, and fe80::/10 is
  //    already covered by the NDP and DHCPv6 filters below.
  if (cfg.allow_lan) {
    std::vector<WfpCondition> lan;
    for (const auto& p : net::kLocalBypassV4)
      lan.push_back(CondRemoteV4(p.network, p.prefix));
    f.push_back(Spec("urnetwork-permit-lan-out", WfpLayer::ConnectV4,
                     WfpSublayer::Baseline, false, kWeightExempt, lan));
    f.push_back(Spec("urnetwork-permit-lan-in", WfpLayer::RecvAcceptV4,
                     WfpSublayer::Baseline, false, kWeightExempt, lan));
  }

  // 5. DHCPv4. Rank-3: without it everything works for hours and then the lease
  //    expires and the machine loses its address — a delayed failure nobody
  //    attributes to the VPN. Outbound is scoped to the broadcast address
  //    (Mullvad's shape); a UNICAST renew to a server inside the LAN ranges is
  //    covered by the LAN permit above, which is another place the two tables
  //    have to agree.
  f.push_back(Spec("urnetwork-permit-dhcpv4-out", WfpLayer::ConnectV4,
                   WfpSublayer::Baseline, false, kWeightExempt,
                   {CondProtocol(kProtoUdp), CondLocalPort(kPortDhcpClient),
                    CondRemotePort(kPortDhcpServer),
                    CondRemoteV4(0xFFFFFFFFu, 32)}));
  f.push_back(Spec("urnetwork-permit-dhcpv4-in", WfpLayer::RecvAcceptV4,
                   WfpSublayer::Baseline, false, kWeightExempt,
                   {CondProtocol(kProtoUdp), CondLocalPort(kPortDhcpClient),
                    CondRemotePort(kPortDhcpServer)}));

  // 6. DHCPv6 + NDP. Kept even though all other v6 is blocked: rank-4 says the
  //    v6 link never comes up cleanly without NDP, DAD fails, and on some
  //    drivers that stalls interface init and slows the WHOLE stack, v4
  //    included. Every address here is link-scoped and cannot leave the link.
  //
  //    ICMPv6 type is expressed on the ALE layers through IP_LOCAL_PORT
  //    (fwpmu.h literally #defines FWPM_CONDITION_ICMP_TYPE to it). It is only
  //    unambiguous because IP_PROTOCOL == 58 is AND'd with it — do not drop the
  //    protocol condition to "simplify".
  //
  //    ICMPv6 REDIRECT (type 137) is deliberately absent. Mullvad and WireGuard
  //    both permit it; we are blocking v6, so there is nothing to redirect.
  f.push_back(Spec("urnetwork-permit-dhcpv6-out", WfpLayer::ConnectV6,
                   WfpSublayer::Baseline, false, kWeightExempt,
                   {CondProtocol(kProtoUdp),
                    CondV6(WfpField::LocalAddrV6, kV6LinkLocal, 10),
                    CondLocalPort(kPortDhcpV6Client),
                    CondRemotePort(kPortDhcpV6Server),
                    // both groups: omitting ff05::1:3 breaks relayed
                    // enterprise DHCPv6
                    CondV6(WfpField::RemoteAddrV6, kV6DhcpRelayAgentsAndServers, 128),
                    CondV6(WfpField::RemoteAddrV6, kV6DhcpAllServers, 128)}));
  f.push_back(Spec("urnetwork-permit-dhcpv6-in", WfpLayer::RecvAcceptV6,
                   WfpSublayer::Baseline, false, kWeightExempt,
                   {CondProtocol(kProtoUdp),
                    CondV6(WfpField::LocalAddrV6, kV6LinkLocal, 10),
                    CondLocalPort(kPortDhcpV6Client),
                    CondV6(WfpField::RemoteAddrV6, kV6LinkLocal, 10),
                    CondRemotePort(kPortDhcpV6Server)}));

  f.push_back(Spec("urnetwork-permit-ndp-rs-out", WfpLayer::ConnectV6,
                   WfpSublayer::Baseline, false, kWeightExempt,
                   {CondProtocol(kProtoIcmpV6),
                    CondLocalPort(kIcmpV6RouterSolicit),
                    CondV6(WfpField::RemoteAddrV6, kV6AllRouters, 128)}));
  f.push_back(Spec("urnetwork-permit-ndp-ra-in", WfpLayer::RecvAcceptV6,
                   WfpSublayer::Baseline, false, kWeightExempt,
                   {CondProtocol(kProtoIcmpV6),
                    CondLocalPort(kIcmpV6RouterAdvert),
                    CondV6(WfpField::RemoteAddrV6, kV6LinkLocal, 10)}));
  for (uint16_t type : {kIcmpV6NeighborSolicit, kIcmpV6NeighborAdvert}) {
    const char* name = type == kIcmpV6NeighborSolicit
                           ? "urnetwork-permit-ndp-ns"
                           : "urnetwork-permit-ndp-na";
    for (WfpLayer l : {WfpLayer::ConnectV6, WfpLayer::RecvAcceptV6}) {
      f.push_back(Spec(name, l, WfpSublayer::Baseline, false, kWeightExempt,
                       {CondProtocol(kProtoIcmpV6), CondLocalPort(type),
                        CondV6(WfpField::RemoteAddrV6, kV6LinkLocal, 10),
                        CondV6(WfpField::RemoteAddrV6, kV6LinkLocalMcast, 16)}));
    }
  }

  // 7. THE DNS LIFTING RULE. Rank-5, and the subtle one: it is not a breakage
  //    when missing, it is a SILENT LEAK. Without it the LAN permit above (a
  //    permit at weight 12) terminates evaluation in this sublayer for a query
  //    to 192.168.1.1:53 — the router's resolver — and the query leaves. This
  //    soft permit lifts port 53 out of the baseline decision so the DNS
  //    sublayer, which hard-blocks it, gets to decide instead.
  //
  //    CORRECTION to the reasoning above, kept next to it because the filter is
  //    right and the reason given for it is not. A permit terminates evaluation
  //    only WITHIN its own sublayer; every sublayer is still evaluated, and
  //    block beats permit across them — which is exactly what the header and
  //    filter 9's comment both say. So the DNS sublayer's block would defeat the
  //    LAN permit with or without this filter. What this filter actually does is
  //    the converse: it makes the DNS sublayer the SOLE authority on port 53, by
  //    lifting a query that the weight-0 floor would otherwise have blocked in
  //    the baseline.
  //
  //    That is why it must stay. Filter 9b permits the host's own resolvers in
  //    the DNS sublayer, and those are frequently NOT in the LAN bypass ranges
  //    (1.1.1.1, an ISP resolver). Without this lift the baseline floor blocks
  //    them and 9b permits into a sublayer whose verdict is overruled — the
  //    service still cannot resolve, and the armed state is unrecoverable
  //    again. Deleting this filter on the strength of its old rationale would
  //    silently reintroduce the defect 9b exists to fix.
  //
  //    The cost of it being a permit rather than a no-op is real and should be
  //    understood: off-tunnel DNS is defended by ONE layer (the DNS sublayer),
  //    not two. Filter 12 is therefore not optional depth, it is the whole
  //    defence, which is why 12 is coupled to a usable path rather than to a
  //    config flag.
  {
    std::vector<WfpCondition> lift;
    PushUdpTcp(lift);
    lift.push_back(CondRemotePort(kPortDns));
    f.push_back(Spec("urnetwork-lift-dns-v4", WfpLayer::ConnectV4,
                     WfpSublayer::Baseline, false, kWeightMedium, lift));
    f.push_back(Spec("urnetwork-lift-dns-v6", WfpLayer::ConnectV6,
                     WfpSublayer::Baseline, false, kWeightMedium, lift));
  }

  // 8. THE FLOOR. A filter's block is a HARD block by default, so this beats
  //    Windows Firewall's permits and every other VPN's permits in every
  //    sublayer. We can only ever be more restrictive than the rest of the
  //    system, never less — which is the right way round for a fail-closed
  //    policy.
  f.push_back(Spec("urnetwork-block-all-v4-out", WfpLayer::ConnectV4,
                   WfpSublayer::Baseline, true, kWeightMin));
  f.push_back(Spec("urnetwork-block-all-v4-in", WfpLayer::RecvAcceptV4,
                   WfpSublayer::Baseline, true, kWeightMin));
  if (v6) {
    // R7. This is the whole IPv6 fix: two unconditional ALE blocks. It catches
    // TCP connect, the first UDP packet per tuple and raw sockets, and adding
    // it triggers ALE REAUTHORIZATION so flows established before we armed die
    // on their next packet instead of continuing to leak.
    f.push_back(Spec("urnetwork-block-all-v6-out", WfpLayer::ConnectV6,
                     WfpSublayer::Baseline, true, kWeightMin));
    f.push_back(Spec("urnetwork-block-all-v6-in", WfpLayer::RecvAcceptV6,
                     WfpSublayer::Baseline, true, kWeightMin));
  }

  // === DNS SUBLAYER ========================================================
  // Enumerate what to ALLOW, never what to block.

  // Whether THIS state has a path by which the SERVICE's own name resolution
  // can reach a resolver. Set by the two address-scoped permits below (9b and
  // 10) and read by filter 12.
  //
  // It is NOT read as "install the block only if this is true" any more — that
  // form of the invariant died with the Armed/Connecting split, because Armed is
  // deliberately a blocked state with no path. It is read as the restated one:
  // every state that ATTEMPTS A CONNECTION must have a path, so in those states
  // (and only those) a missing path stands the block down rather than stranding
  // an attempt that can then never succeed — no resolution -> no connect -> still
  // armed -> no resolution. See AttemptsConnection in WfpPolicy.h and filter 12.
  //
  // Deliberately NOT set by filter 9 (app id) or filter 11 (loopback). See why
  // at each of them.
  bool serviceDnsPath = false;

  // 9. OUR OWN SERVICE, AGAIN — and this one is a correction to the research
  //    note, not a copy of it. §4.1 permits urnetworkd in the BASELINE sublayer
  //    only (filter C1). Block beats permit ACROSS sublayers, so the port-53
  //    hard block below would also block urnetworkd's own name resolution, and
  //    a service that cannot resolve the platform host cannot reconnect — the
  //    rank-1 unrecoverable state, reintroduced through the sublayer it was
  //    exempted from. The exemption has to be repeated here.
  //
  //    IT IS NOT SUFFICIENT, AND ON ITS OWN IT MATCHES NOTHING WE NEED. The
  //    SDK is Go, and Go on Windows resolves through the OS resolver:
  //    net/conf.go returns the fallback order unconditionally for GOOS=windows,
  //    which is net/lookup_windows.go's GetAddrInfoW. Nothing in this repo sets
  //    ConnectSettings.Resolver (there is no API on the vendored SDK to set it)
  //    and nothing sets GODEBUG=netdns=go, so the wire query is issued by the
  //    DNS Client service inside svchost.exe — a different process, whose
  //    app id is not ours. This filter covers only sockets urnetworkd opens to
  //    port 53 ITSELF, which today is none; it is kept so that the day the SDK
  //    gains an in-process resolver the permit is already correct. The filter
  //    that actually keeps the service resolving is 9b.
  //
  //    This is the same fact filter 10 below is already scoped around. It was
  //    applied there and not here.
  if (!cfg.service_image_path.empty()) {
    f.push_back(Spec("urnetwork-permit-service-dns-v4", WfpLayer::ConnectV4,
                     WfpSublayer::Dns, false, kWeightMax,
                     {CondAppId(cfg.service_image_path)}));
    f.push_back(Spec("urnetwork-permit-service-dns-v6", WfpLayer::ConnectV6,
                     WfpSublayer::Dns, false, kWeightMax,
                     {CondAppId(cfg.service_image_path)}));
  }

  // 9b. THE HOST'S OWN RESOLVERS, and ONLY while a connection attempt is
  //     ACTUALLY IN FLIGHT.
  //
  //     This is the filter that lets a kill-switched machine come back: the
  //     service resolves the platform host through the OS resolver, whose query
  //     leaves svchost.exe for one of these addresses. Without it in the state a
  //     connect runs in, arming is a one-way door — the attempt cannot resolve,
  //     so the tunnel never returns, so the policy never widens.
  //
  //     NOT IN Armed, AND THAT IS THE POINT OF THE Armed/Connecting SPLIT. The
  //     permit is address-scoped and therefore machine-wide (it cannot be scoped
  //     to us — the query is issued by Dnscache in svchost.exe by the time it
  //     reaches the filter engine), so while it is installed EVERY process on the
  //     machine can resolve in plaintext. Idle is where a kill switch spends its
  //     life; paying that for an attempt that is not happening is the wrong
  //     trade, and the owner's call was to open the hole per attempt instead.
  //
  //     Whoever attempts a connection is therefore responsible for entering
  //     Connecting BEFORE any resolution and returning to Armed after. There is
  //     exactly one such path (ControlServer -> TunnelController::StartLocked)
  //     and it is bounded by a watchdog; see TunnelController.h.
  //
  //     Scoped on the resolver ADDRESSES, exactly like filter 10, and for
  //     exactly filter 10's reason: app identity is the wrong axis when the
  //     query comes from a shared svchost. Permitting svchost.exe wholesale
  //     would permit dozens of unrelated services; hardcoding bootstrap IPs
  //     breaks when they change. The addresses are re-read on every policy
  //     application, and every reconnect attempt re-applies the armed policy
  //     (StartLocked -> StopLocked -> Apply(Armed)), so a roam is picked up on
  //     the next retry rather than needing its own notification path.
  //
  //     NOT emitted while Connected: there the tunnel's resolvers are the path
  //     (filter 10), and permitting the physical adapter's resolvers on top of
  //     them would be the R6 leak this whole sublayer exists to close.
  //
  //     ONE filter with the addresses as consecutive conditions on the same
  //     field, which WFP OR's — same shape as the LAN permit, and it keeps the
  //     filter count deterministic no matter how many resolvers the machine has.
  //
  //     DEPENDS ON FILTER 7. A resolver outside the LAN bypass ranges (1.1.1.1,
  //     an ISP resolver) is blocked by the weight-0 floor in the BASELINE
  //     sublayer, and a permit here cannot overrule a block there. Filter 7's
  //     lift is what keeps the baseline out of the port-53 decision. Read its
  //     comment before touching either.
  //
  //     Loopback resolvers are excluded upstream (NetworkConfig::HostResolversV4)
  //     rather than permitted here: a local stub at 127.0.0.1 is already covered
  //     by filter 11, but its OWN upstream is still blocked by filter 12, so
  //     counting it as a service DNS path would claim a route that does not
  //     resolve. Excluded, it correctly reports "no path" and filter 12 stands
  //     down instead.
  if (connecting && !cfg.host_resolvers_v4.empty()) {
    std::vector<WfpCondition> c;
    PushUdpTcp(c);
    c.push_back(CondRemotePort(kPortDns));
    size_t addrs = 0;
    for (const auto& server : cfg.host_resolvers_v4) {
      IN_ADDR a{};
      if (::inet_pton(AF_INET, server.c_str(), &a) != 1) continue;
      c.push_back(CondRemoteV4(::ntohl(a.S_un.S_addr), 32));
      ++addrs;
    }
    if (addrs > 0) {
      f.push_back(Spec("urnetwork-permit-dns-host-resolver", WfpLayer::ConnectV4,
                       WfpSublayer::Dns, false, kWeightMedium, std::move(c)));
      serviceDnsPath = true;
    }
  }

  // 10. The tunnel's own resolvers, and ONLY over the tun. Scoped on port +
  //     protocol + remote address + LOCAL INTERFACE, never on app identity:
  //     dnscache runs inside svchost.exe, so "permit 53 only from svchost"
  //     permits dozens of unrelated services and "deny 53 except svchost" is
  //     exactly the bypass an app with its own resolver uses.
  if (connected && cfg.tun_luid != 0) {
    for (const auto& server : cfg.tunnel_resolvers_v4) {
      IN_ADDR a{};
      if (::inet_pton(AF_INET, server.c_str(), &a) != 1) continue;
      std::vector<WfpCondition> c;
      PushUdpTcp(c);
      c.push_back(CondRemotePort(kPortDns));
      c.push_back(CondRemoteV4(::ntohl(a.S_un.S_addr), 32));
      c.push_back(CondLocalInterface(cfg.tun_luid));
      f.push_back(Spec("urnetwork-permit-dns-tunnel-resolver",
                       WfpLayer::ConnectV4, WfpSublayer::Dns, false,
                       kWeightMedium, std::move(c)));
      serviceDnsPath = true;
    }
  }

  // 11. Local stub resolvers: dnscrypt-proxy, Docker's embedded DNS, WSL.
  //
  //     Does NOT count as a service DNS path. It permits the query to reach the
  //     stub; the stub's own upstream is a separate socket to a real resolver on
  //     port 53, which filter 12 still blocks. Treating this as a path would
  //     install the hard block on the strength of a hop that dead-ends.
  {
    std::vector<WfpCondition> c;
    PushUdpTcp(c);
    c.push_back(CondRemotePort(kPortDns));
    c.push_back(CondLoopback());
    f.push_back(Spec("urnetwork-permit-dns-loopback-v4", WfpLayer::ConnectV4,
                     WfpSublayer::Dns, false, kWeightMedium, c));
    f.push_back(Spec("urnetwork-permit-dns-loopback-v6", WfpLayer::ConnectV6,
                     WfpSublayer::Dns, false, kWeightMedium, c));
  }

  // 12. THE R6 FIX. Deny broadly with no interface condition — deliberately NOT
  //     "local interface != tun", which needs negation/enumeration and
  //     reintroduces the adapter-arrival race the whole approach exists to
  //     avoid. A dock, a phone hotspot or a Hyper-V vSwitch appearing
  //     mid-session is covered the instant it appears, because we match on the
  //     remote port and not on a list of interfaces we have to keep current.
  //
  //     ALWAYS INSTALLED IN Armed. INSTALLED IN Connecting/Connected ONLY WHEN
  //     THE SERVICE HAS A DNS PATH. That asymmetry IS the restated invariant
  //     (AttemptsConnection, WfpPolicy.h), expressed as a dependency rather than
  //     as a comment.
  //
  //     Armed first, because it is the half that changed. Armed is idle: nothing
  //     is trying to resolve, so there is nothing to strand and no reconnect to
  //     break. Blocking port 53 with no permit lifted through it is what "the
  //     kill switch is on and nothing leaves" MEANS, and standing the block down
  //     there — which the old, path-shaped invariant would have done on a host
  //     with no resolver — would reopen plaintext DNS machine-wide on exactly
  //     the machine that has no way to use it.
  //
  //     Connecting and Connected keep the fail-safe, because they are the states
  //     that must be able to resolve: without a permit the OS resolver's query
  //     can actually match, this block makes an ATTEMPT unable to resolve, so
  //     the tunnel never returns, so the policy never widens. That state has no
  //     way out from inside the product — only `sc stop urnetworkd` or an
  //     elevated revert — so it must not be reachable.
  //
  //     What is open when it stands down, named exactly: filter 7 lifts port 53
  //     to a weight-7 permit in the BASELINE sublayer, so with no block here
  //     plaintext DNS to ANY server is permitted, from any process, for as long
  //     as that state lasts. Everything that is not port 53 stays blocked by the
  //     floor, and LLMNR/mDNS/NetBIOS stay hard-blocked below. That is a
  //     deliberately narrower failure than an attempt that can never succeed. It
  //     is only reachable when the host has no usable IPv4 resolver at all — no
  //     adapter DNS, or loopback-only — and the log line in Apply() names it so
  //     it is never silent. It now also ends with the attempt: the next return to
  //     Armed reinstates the block unconditionally.
  if (!AttemptsConnection(state) || serviceDnsPath) {
    std::vector<WfpCondition> c;
    PushUdpTcp(c);
    c.push_back(CondRemotePort(kPortDns));
    f.push_back(Spec("urnetwork-block-dns-v4", WfpLayer::ConnectV4,
                     WfpSublayer::Dns, true, kWeightMin, c));
    f.push_back(Spec("urnetwork-block-dns-v6", WfpLayer::ConnectV6,
                     WfpSublayer::Dns, true, kWeightMin, c));
  }

  // 13. The other name-resolution channels. Port 53 is not the only way a name
  //     becomes an address, and LLMNR fires on ANY DNS failure — which is the
  //     state this policy creates, so leaving it open turns every blocked query
  //     into a broadcast of the requested hostname to every device on the LAN.
  //     Mullvad's published design does not cover these; that is a gap, not a
  //     precedent.
  //
  //     NetBIOS is one filter over three ports rather than three: the OR groups
  //     are per-field, so (UDP|TCP) x (137|138|139) also covers TCP/137 and
  //     UDP/139, which are not real services. Blocking strictly more here is
  //     free.
  if (cfg.block_local_name_resolution) {
    struct { const char* name; uint16_t ports[3]; int count; } sets[] = {
        {"urnetwork-block-llmnr", {kPortLlmnr}, 1},
        {"urnetwork-block-mdns", {kPortMdns}, 1},
        {"urnetwork-block-netbios",
         {kPortNetbiosNs, kPortNetbiosDgm, kPortNetbiosSsn}, 3},
    };
    for (const auto& s : sets) {
      std::vector<WfpCondition> c;
      PushUdpTcp(c);
      for (int i = 0; i < s.count; ++i) c.push_back(CondRemotePort(s.ports[i]));
      f.push_back(Spec(std::string(s.name) + "-v4", WfpLayer::ConnectV4,
                       WfpSublayer::Dns, true, kWeightMin, c));
      f.push_back(Spec(std::string(s.name) + "-v6", WfpLayer::ConnectV6,
                       WfpSublayer::Dns, true, kWeightMin, c));
    }
  }

  return f;
}

// ---------------------------------------------------------------------------
// The two structural questions Apply() asks about a spec. See WfpPolicy.h for
// why they are structural and not name comparisons.
// ---------------------------------------------------------------------------

bool IsMachineWideDnsPermit(const WfpFilterSpec& f) {
  if (f.block || f.sublayer != WfpSublayer::Dns) return false;
  bool port53 = false;
  bool byAddress = false;
  for (const auto& c : f.conditions) {
    switch (c.field) {
      case WfpField::RemotePort:
        if (c.number == kPortDns) port53 = true;
        break;
      case WfpField::RemoteAddrV4:
      case WfpField::RemoteAddrV6:
        byAddress = true;
        break;
      // Each of these NARROWS the permit to something smaller than "every
      // process on this machine", so any one of them disqualifies it:
      //   AppId          — scoped to one executable (filter 9)
      //   Flags          — the loopback flag, i.e. a local stub (filter 11)
      //   LocalInterface — pinned to the tun (filter 10)
      case WfpField::AppId:
      case WfpField::Flags:
      case WfpField::LocalInterface:
        return false;
      default:
        break;
    }
  }
  return port53 && byAddress;
}

bool IsDnsPort53Block(const WfpFilterSpec& f) {
  if (!f.block || f.sublayer != WfpSublayer::Dns) return false;
  for (const auto& c : f.conditions) {
    if (c.field == WfpField::RemotePort && c.number == kPortDns) return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
std::vector<WfpFilterSpec> BuildPersistentFilterSet() {
  // Four boot-time + four persistent unconditional blocks, plus a loopback
  // permit Mullvad's equivalent set does not have (blocking loopback between
  // boot and the daemon starting breaks local databases, dev servers and IPC in
  // ways that look like our bug, and the exposure window is seconds).
  //
  // Returned for review and for the purge to recognise. NOT INSTALLED — see the
  // header. WfpPolicy has no method that adds these.
  std::vector<WfpFilterSpec> f;
  for (WfpLayer l : {WfpLayer::ConnectV4, WfpLayer::ConnectV6,
                     WfpLayer::RecvAcceptV4, WfpLayer::RecvAcceptV6}) {
    f.push_back(Spec("urnetwork-persistent-permit-loopback", l,
                     WfpSublayer::Persistent, false, kWeightMax,
                     {CondLoopback()}));
    f.push_back(Spec("urnetwork-persistent-block-all", l,
                     WfpSublayer::Persistent, true, kWeightMax));
  }
  return f;
}

// ---------------------------------------------------------------------------
// Turning specs into filters. Everything below here talks to BFE.
// ---------------------------------------------------------------------------
namespace {

const GUID& LayerGuid(WfpLayer l) {
  switch (l) {
    case WfpLayer::ConnectV4: return FWPM_LAYER_ALE_AUTH_CONNECT_V4;
    case WfpLayer::ConnectV6: return FWPM_LAYER_ALE_AUTH_CONNECT_V6;
    case WfpLayer::RecvAcceptV4: return FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V4;
    case WfpLayer::RecvAcceptV6: return FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V6;
  }
  return FWPM_LAYER_ALE_AUTH_CONNECT_V4;
}

const GUID& SublayerGuid(WfpSublayer s) {
  switch (s) {
    case WfpSublayer::Baseline: return kSublayerBaselineGuid;
    case WfpSublayer::Dns: return kSublayerDnsGuid;
    case WfpSublayer::Persistent: return kSublayerPersistentGuid;
  }
  return kSublayerBaselineGuid;
}

const GUID& FieldGuid(WfpField f) {
  switch (f) {
    case WfpField::AppId: return FWPM_CONDITION_ALE_APP_ID;
    case WfpField::Flags: return FWPM_CONDITION_FLAGS;
    case WfpField::LocalInterface: return FWPM_CONDITION_IP_LOCAL_INTERFACE;
    case WfpField::Protocol: return FWPM_CONDITION_IP_PROTOCOL;
    // NOTE: FWPM_CONDITION_ICMP_TYPE *is* FWPM_CONDITION_IP_LOCAL_PORT and
    // FWPM_CONDITION_ICMP_CODE *is* FWPM_CONDITION_IP_REMOTE_PORT. That is not
    // a shortcut here — fwpmu.h defines them as the same GUID.
    case WfpField::LocalPort: return FWPM_CONDITION_IP_LOCAL_PORT;
    case WfpField::RemotePort: return FWPM_CONDITION_IP_REMOTE_PORT;
    case WfpField::LocalAddrV4:
    case WfpField::LocalAddrV6: return FWPM_CONDITION_IP_LOCAL_ADDRESS;
    case WfpField::RemoteAddrV4:
    case WfpField::RemoteAddrV6: return FWPM_CONDITION_IP_REMOTE_ADDRESS;
  }
  return FWPM_CONDITION_FLAGS;
}

// Per-filter backing store for the values FWP_CONDITION_VALUE0 holds BY
// POINTER. These must outlive FwpmFilterAdd0, and the vectors are reserved up
// front so a push_back can never reallocate under a pointer we already handed
// to WFP.
struct CondStore {
  std::vector<UINT64> u64;
  std::vector<FWP_V4_ADDR_AND_MASK> v4;
  std::vector<FWP_V6_ADDR_AND_MASK> v6;
  std::vector<FWP_BYTE_BLOB*> blobs;

  void Reserve(size_t n) {
    u64.reserve(n);
    v4.reserve(n);
    v6.reserve(n);
    blobs.reserve(n);
  }
  ~CondStore() {
    for (auto* b : blobs) {
      if (b) ::FwpmFreeMemory0(reinterpret_cast<void**>(&b));
    }
  }
};

uint32_t V4Mask(uint8_t prefix) {
  return prefix == 0 ? 0u : (0xFFFFFFFFu << (32 - prefix));
}

bool BuildConditions(const WfpFilterSpec& spec, CondStore& store,
                     std::vector<FWPM_FILTER_CONDITION0>& out,
                     std::string& error) {
  store.Reserve(spec.conditions.size());
  out.reserve(spec.conditions.size());
  for (const auto& c : spec.conditions) {
    FWPM_FILTER_CONDITION0 fc{};
    fc.fieldKey = FieldGuid(c.field);
    fc.matchType = c.match == WfpMatch::FlagsAllSet ? FWP_MATCH_FLAGS_ALL_SET
                                                    : FWP_MATCH_EQUAL;
    switch (c.field) {
      case WfpField::AppId: {
        FWP_BYTE_BLOB* blob = nullptr;
        DWORD err = ::FwpmGetAppIdFromFileName0(c.app_path.c_str(), &blob);
        if (err != ERROR_SUCCESS || !blob) {
          error = std::format("FwpmGetAppIdFromFileName0({}) failed: {:#x}",
                              Narrow(c.app_path), err);
          return false;
        }
        store.blobs.push_back(blob);
        fc.conditionValue.type = FWP_BYTE_BLOB_TYPE;
        fc.conditionValue.byteBlob = blob;
        break;
      }
      case WfpField::Flags:
        fc.conditionValue.type = FWP_UINT32;
        fc.conditionValue.uint32 = static_cast<UINT32>(c.number);
        break;
      case WfpField::LocalInterface:
        store.u64.push_back(static_cast<UINT64>(c.number));
        fc.conditionValue.type = FWP_UINT64;
        fc.conditionValue.uint64 = &store.u64.back();
        break;
      case WfpField::Protocol:
        fc.conditionValue.type = FWP_UINT8;
        fc.conditionValue.uint8 = static_cast<UINT8>(c.number);
        break;
      case WfpField::LocalPort:
      case WfpField::RemotePort:
        // HOST byte order. htons() here yields a filter on the wrong port and a
        // leak that reads clean.
        fc.conditionValue.type = FWP_UINT16;
        fc.conditionValue.uint16 = static_cast<UINT16>(c.number);
        break;
      case WfpField::LocalAddrV4:
      case WfpField::RemoteAddrV4: {
        FWP_V4_ADDR_AND_MASK m{};
        m.addr = c.v4_addr;  // host order, as WFP expects
        m.mask = V4Mask(c.v4_prefix);
        store.v4.push_back(m);
        fc.conditionValue.type = FWP_V4_ADDR_MASK;
        fc.conditionValue.v4AddrMask = &store.v4.back();
        break;
      }
      case WfpField::LocalAddrV6:
      case WfpField::RemoteAddrV6: {
        FWP_V6_ADDR_AND_MASK m{};
        std::memcpy(m.addr, c.v6_addr, 16);
        m.prefixLength = c.v6_prefix;
        store.v6.push_back(m);
        fc.conditionValue.type = FWP_V6_ADDR_MASK;
        fc.conditionValue.v6AddrMask = &store.v6.back();
        break;
      }
    }
    out.push_back(fc);
  }
  return true;
}

}  // namespace

// ---------------------------------------------------------------------------

std::vector<std::string> WfpPolicy::ObjectGuidsText() {
  return {"provider " + GuidText(kProviderGuid),
          "provider(persistent, never installed) " +
              GuidText(kPersistentProviderGuid),
          "sublayer(baseline) " + GuidText(kSublayerBaselineGuid),
          "sublayer(dns) " + GuidText(kSublayerDnsGuid),
          "sublayer(persistent, never installed) " +
              GuidText(kSublayerPersistentGuid)};
}

WfpPolicy::~WfpPolicy() {
  std::scoped_lock lock(mutex_);
  CloseEngine();
}

bool WfpPolicy::OpenEngine() {
  if (engine_) return true;

  // THE DYNAMIC SESSION. This one flag is the entire crash-safety story: BFE
  // destroys the provider, both sublayers and every filter when this process
  // dies, however it dies. Nothing else in this file has to be crash-safe.
  FWPM_SESSION0 session{};
  session.flags = FWPM_SESSION_FLAG_DYNAMIC;
  std::wstring sessionName = L"URnetwork tunnel policy";
  session.displayData.name = sessionName.data();

  HANDLE engine = nullptr;
  DWORD err = ::FwpmEngineOpen0(nullptr, RPC_C_AUTHN_DEFAULT, nullptr, &session,
                                &engine);
  if (err != ERROR_SUCCESS) {
    lastError_ = std::format("FwpmEngineOpen0 failed: {:#x}", err);
    LogError("wfp: {} — the leak-prevention layer is NOT in force. This needs "
             "LocalSystem or an elevated administrator; unelevated it cannot "
             "run, exactly like step 1/8's wintun adapter.",
             lastError_);
    return false;
  }

  err = ::FwpmTransactionBegin0(engine, 0);
  if (err != ERROR_SUCCESS) {
    lastError_ = std::format("FwpmTransactionBegin0 failed: {:#x}", err);
    ::FwpmEngineClose0(engine);
    return false;
  }

  // The provider carries NO serviceName. It is tempting to set
  // FWPM_PROVIDER0.serviceName = L"urnetworkd" — the research argues for it,
  // and it is genuinely the right field for the PERSISTENT provider, where BFE
  // refuses to enable objects at boot whose named service is absent or not
  // auto-start. But that gate is documented for objects BFE adds at boot, and
  // what it does to a DYNAMIC provider added by a running process — especially
  // by `urnetworkd console`, where no such service is installed at all — is not
  // documented. If BFE were to mark the provider FWPM_PROVIDER_FLAG_DISABLED,
  // every filter under it would be disabled and we would believe we were
  // protected while leaking. That failure is silent, so it is not a risk to
  // take on an unverified reading. The field belongs on the persistent
  // provider, and the persistent path is gated off (see the header).
  FWPM_PROVIDER0 provider{};
  provider.providerKey = kProviderGuid;
  std::wstring providerName = L"URnetwork";
  std::wstring providerDesc = L"URnetwork VPN leak prevention";
  provider.displayData.name = providerName.data();
  provider.displayData.description = providerDesc.data();
  err = ::FwpmProviderAdd0(engine, &provider, nullptr);
  if (err != ERROR_SUCCESS && err != FWP_E_ALREADY_EXISTS) {
    lastError_ = std::format("FwpmProviderAdd0 failed: {:#x}", err);
    ::FwpmTransactionAbort0(engine);
    ::FwpmEngineClose0(engine);
    return false;
  }

  struct { const GUID* key; const wchar_t* name; UINT16 weight; } subs[] = {
      {&kSublayerBaselineGuid, L"URnetwork baseline", 0xFFFF},
      {&kSublayerDnsGuid, L"URnetwork DNS", 0xFFFE},
  };
  for (const auto& s : subs) {
    FWPM_SUBLAYER0 sub{};
    sub.subLayerKey = *s.key;
    std::wstring n = s.name;
    sub.displayData.name = n.data();
    sub.providerKey = const_cast<GUID*>(&kProviderGuid);
    sub.weight = s.weight;
    err = ::FwpmSubLayerAdd0(engine, &sub, nullptr);
    if (err != ERROR_SUCCESS && err != FWP_E_ALREADY_EXISTS) {
      lastError_ = std::format("FwpmSubLayerAdd0({}) failed: {:#x}",
                               Narrow(s.name), err);
      ::FwpmTransactionAbort0(engine);
      ::FwpmEngineClose0(engine);
      return false;
    }
  }

  err = ::FwpmTransactionCommit0(engine);
  if (err != ERROR_SUCCESS) {
    lastError_ = std::format("FwpmTransactionCommit0(objects) failed: {:#x}", err);
    ::FwpmEngineClose0(engine);
    return false;
  }

  // Turn the unverified assumption above into a runtime check rather than a
  // belief. If BFE ever DOES disable our provider, every filter beneath it is
  // inert and we would otherwise report success while protecting nothing.
  FWPM_PROVIDER0* readback = nullptr;
  if (::FwpmProviderGetByKey0(engine, &kProviderGuid, &readback) ==
          ERROR_SUCCESS && readback) {
    if (readback->flags & FWPM_PROVIDER_FLAG_DISABLED) {
      LogError("wfp: BFE marked our provider DISABLED (flags {:#x}). Every "
               "filter under it is inert — the leak-prevention layer is NOT in "
               "force despite being installed. Refusing to report success.",
               readback->flags);
      ::FwpmFreeMemory0(reinterpret_cast<void**>(&readback));
      lastError_ = "provider disabled by BFE";
      ::FwpmEngineClose0(engine);
      return false;
    }
    ::FwpmFreeMemory0(reinterpret_cast<void**>(&readback));
  }

  engine_ = engine;
  LogInfo("wfp: dynamic session open (provider {}); BFE removes everything "
          "below it when this process dies",
          GuidText(kProviderGuid));
  return true;
}

void WfpPolicy::CloseEngine() {
  if (!engine_) return;
  // Closing a DYNAMIC session is the teardown: BFE deletes the provider, both
  // sublayers and every filter. This is why WireGuard's DisableFirewall() is
  // literally just an engine close.
  ::FwpmEngineClose0(engine_);
  engine_ = nullptr;
  filterIds_.clear();
  state_ = WfpState::Off;
}

WfpState WfpPolicy::State() const {
  std::scoped_lock lock(mutex_);
  return state_;
}

std::string WfpPolicy::LastError() const {
  std::scoped_lock lock(mutex_);
  return lastError_;
}

size_t WfpPolicy::FilterCount() const {
  std::scoped_lock lock(mutex_);
  return filterIds_.size();
}

bool WfpPolicy::Apply(WfpState state, const WfpConfig& cfg) {
  std::scoped_lock lock(mutex_);
  return ApplyLocked(state, cfg);
}

bool WfpPolicy::ApplyLocked(WfpState state, const WfpConfig& cfg) {
  lastError_.clear();
  if (state == WfpState::Off) {
    RevertLocked();
    return true;
  }
  if (!OpenEngine()) return false;

  const std::vector<WfpFilterSpec> specs = BuildFilterSet(state, cfg);
  HANDLE engine = engine_;

  DWORD err = ::FwpmTransactionBegin0(engine, 0);
  if (err != ERROR_SUCCESS) {
    lastError_ = std::format("FwpmTransactionBegin0 failed: {:#x}", err);
    LogError("wfp: {}", lastError_);
    return false;
  }

  // ONE transaction for the whole swap. Adding a permit and a deny
  // non-atomically means a window in which either everything is blocked or
  // everything leaks.
  //
  // AND THE DELETES ARE CHECKED. Ignoring the return here was silent in the
  // worst possible direction: a filter that failed to delete stays installed
  // alongside the new set for the rest of the session, and BLOCK BEATS PERMIT
  // ACROSS SUBLAYERS — so a leftover block from a narrower policy quietly
  // overrules the wider one we just committed (the tun permit stops working, or
  // the port-53 permit does), while a leftover permit from a wider policy is a
  // leak the new policy believes it closed. Neither shows up anywhere: the
  // filter count reported to the log and to the selftest is `ids`, which does
  // not know about the orphan.
  //
  // FWP_E_FILTER_NOT_FOUND is the one benign outcome — the filter is already
  // gone, which is the state the delete was asking for — so it is tolerated.
  // Anything else aborts, which leaves the PREVIOUS state in force. That is the
  // contract this method already documents, and it is the fail-safe direction:
  // one coherent policy that is out of date beats two policies at once.
  for (uint64_t id : filterIds_) {
    DWORD del = ::FwpmFilterDeleteById0(engine, id);
    if (del != ERROR_SUCCESS && del != FWP_E_FILTER_NOT_FOUND) {
      lastError_ = std::format(
          "FwpmFilterDeleteById0({}) failed: {:#x} — the previous filter set "
          "cannot be removed, so installing the new one would leave BOTH in "
          "force",
          id, del);
      LogError("wfp: {}. Aborting the swap; the {} policy stays in force.",
               lastError_, ToString(state_));
      ::FwpmTransactionAbort0(engine);
      return false;
    }
  }

  std::vector<uint64_t> ids;
  ids.reserve(specs.size());
  for (const auto& spec : specs) {
    CondStore store;
    std::vector<FWPM_FILTER_CONDITION0> conds;
    if (!BuildConditions(spec, store, conds, lastError_)) {
      LogError("wfp: building '{}': {}", spec.name, lastError_);
      ::FwpmTransactionAbort0(engine);
      return false;
    }

    std::wstring name = Widen(spec.name);
    FWPM_FILTER0 filter{};
    filter.displayData.name = name.data();
    filter.layerKey = LayerGuid(spec.layer);
    filter.subLayerKey = SublayerGuid(spec.sublayer);
    filter.providerKey = const_cast<GUID*>(&kProviderGuid);
    filter.weight.type = FWP_UINT8;
    filter.weight.uint8 = spec.weight;
    filter.action.type = spec.block ? FWP_ACTION_BLOCK : FWP_ACTION_PERMIT;
    filter.numFilterConditions = static_cast<UINT32>(conds.size());
    filter.filterCondition = conds.empty() ? nullptr : conds.data();

    UINT64 id = 0;
    err = ::FwpmFilterAdd0(engine, &filter, nullptr, &id);
    if (err != ERROR_SUCCESS) {
      lastError_ = std::format("FwpmFilterAdd0('{}') failed: {:#x}", spec.name, err);
      LogError("wfp: {}", lastError_);
      ::FwpmTransactionAbort0(engine);
      return false;
    }
    ids.push_back(id);
  }

  err = ::FwpmTransactionCommit0(engine);
  if (err != ERROR_SUCCESS) {
    lastError_ = std::format("FwpmTransactionCommit0 failed: {:#x}", err);
    LogError("wfp: {} — nothing was changed", lastError_);
    return false;
  }

  filterIds_ = std::move(ids);
  const WfpState prior = state_;
  state_ = state;
  LogInfo("wfp: policy {} -> {} ({} filters; tun_luid={:#x} tunnel_resolvers={} "
          "host_resolvers={} block_ipv6={} lan={})",
          ToString(prior), ToString(state), filterIds_.size(), cfg.tun_luid,
          cfg.tunnel_resolvers_v4.size(), cfg.host_resolvers_v4.size(),
          cfg.block_ipv6 ? "yes" : "no",
          cfg.allow_lan ? "permitted" : "blocked");

  // THE DNS HOLE, AT BOTH EDGES. The owner has to be able to read off the log
  // when machine-wide plaintext DNS was open and when it closed again, because
  // that window is the entire cost of the Armed/Connecting split and it is
  // otherwise invisible from outside the filter engine.
  //
  // ASKED OF THE FILTERS' STRUCTURE, NOT OF THEIR NAMES. Both facts used to be
  // decided by comparing spec.name against a literal, which reports on the name
  // rather than on the policy: a rename silences the disclosure, and a new
  // filter of the same shape under a different name opens the window with the
  // log still saying it is closed. See IsMachineWideDnsPermit / IsDnsPort53Block
  // in WfpPolicy.h.
  bool hostResolverPermit = false;
  bool dnsBlocked = false;
  for (const auto& spec : specs) {
    if (IsMachineWideDnsPermit(spec)) hostResolverPermit = true;
    if (IsDnsPort53Block(spec)) dnsBlocked = true;
  }
  if (hostResolverPermit) {
    LogWarn("wfp: DNS WINDOW OPEN ({} -> {}). While a connection attempt is in "
            "flight the host's own resolvers [{}] are permitted on port 53 — and "
            "that permit is ADDRESS-scoped, so it is machine-wide: every process "
            "on this box can resolve in plaintext until the attempt ends. It "
            "closes on the next transition, and at the latest when the "
            "connecting watchdog fires.",
            ToString(prior), ToString(state), cfg.host_resolvers_v4.size());
  } else if (state == WfpState::Armed) {
    LogInfo("wfp: DNS WINDOW CLOSED ({} -> armed). No DNS permit is installed and "
            "the port-53 hard block is in force: nothing on this machine resolves "
            "in plaintext, including us. A connection attempt reopens it for the "
            "length of the attempt.",
            ToString(prior));
  }

  // The states where the port-53 hard block stands down. It is never silent:
  // this is a real, named leak, and the alternative it was chosen over is an
  // attempt that cannot resolve, so a machine that cannot connect and cannot be
  // recovered from inside the product.
  //
  // Armed is excluded BY THE INVARIANT, not by omission: armed-with-a-block-and-
  // no-path is the design, so warning there would cry leak about the one state
  // that has none.
  if (!dnsBlocked && AttemptsConnection(state)) {
    LogWarn("wfp: the port-53 hard block is NOT in force in state {} — this host "
            "has no usable IPv4 resolver for the service to reach ({} tunnel, {} "
            "host), and blocking DNS with no path back would leave the attempt "
            "unable to resolve and the machine unable to connect. PLAINTEXT DNS "
            "TO ANY SERVER IS PERMITTED while this holds; everything else is "
            "still blocked. It clears as soon as an adapter has a resolver, and "
            "at the latest on the return to armed.",
            ToString(state), cfg.tunnel_resolvers_v4.size(),
            cfg.host_resolvers_v4.size());
  }
  return true;
}

void WfpPolicy::Revert() {
  std::scoped_lock lock(mutex_);
  RevertLocked();
}

void WfpPolicy::RevertLocked() {
  if (!engine_) {
    state_ = WfpState::Off;
    return;
  }
  const size_t n = filterIds_.size();
  CloseEngine();
  LogInfo("wfp: policy off ({} filters removed with the session)", n);
}

int WfpPolicy::SweepOrphanedObjects(bool remove) {
  // A STANDARD (non-dynamic) session: we are deleting objects that belong to a
  // process that is gone, not registering any of our own.
  HANDLE engine = nullptr;
  DWORD err = ::FwpmEngineOpen0(nullptr, RPC_C_AUTHN_DEFAULT, nullptr, nullptr,
                                &engine);
  if (err != ERROR_SUCCESS) {
    // Unelevated this is the normal outcome, and rpc-only mode runs here. Not
    // an error: it is the same "no privilege, and none claimed" story the rest
    // of that mode tells.
    LogInfo("wfp: cannot open the filter engine to check for leftovers ({:#x}); "
            "skipping the sweep (this needs elevation)",
            err);
    return 0;
  }

  std::vector<UINT64> filterIds;
  std::vector<GUID> sublayerKeys;
  std::vector<GUID> providerKeys;

  // Collect keys first and drop every enumerator before deleting anything:
  // objects cannot be deleted while the enumeration that produced them is in
  // progress, and a sublayer still holding filters cannot be deleted at all.
  HANDLE en = nullptr;
  if (::FwpmFilterCreateEnumHandle0(engine, nullptr, &en) == ERROR_SUCCESS) {
    for (;;) {
      FWPM_FILTER0** entries = nullptr;
      UINT32 count = 0;
      if (::FwpmFilterEnum0(engine, en, 256, &entries, &count) != ERROR_SUCCESS)
        break;
      if (count == 0) {
        if (entries) ::FwpmFreeMemory0(reinterpret_cast<void**>(&entries));
        break;
      }
      for (UINT32 i = 0; i < count; ++i) {
        if (entries[i] && IsOurProvider(entries[i]->providerKey))
          filterIds.push_back(entries[i]->filterId);
      }
      ::FwpmFreeMemory0(reinterpret_cast<void**>(&entries));
      if (count < 256) break;
    }
    ::FwpmFilterDestroyEnumHandle0(engine, en);
  }

  en = nullptr;
  if (::FwpmSubLayerCreateEnumHandle0(engine, nullptr, &en) == ERROR_SUCCESS) {
    for (;;) {
      FWPM_SUBLAYER0** entries = nullptr;
      UINT32 count = 0;
      if (::FwpmSubLayerEnum0(engine, en, 256, &entries, &count) != ERROR_SUCCESS)
        break;
      if (count == 0) {
        if (entries) ::FwpmFreeMemory0(reinterpret_cast<void**>(&entries));
        break;
      }
      for (UINT32 i = 0; i < count; ++i) {
        if (entries[i] && IsOurProvider(entries[i]->providerKey))
          sublayerKeys.push_back(entries[i]->subLayerKey);
      }
      ::FwpmFreeMemory0(reinterpret_cast<void**>(&entries));
      if (count < 256) break;
    }
    ::FwpmSubLayerDestroyEnumHandle0(engine, en);
  }

  en = nullptr;
  if (::FwpmProviderCreateEnumHandle0(engine, nullptr, &en) == ERROR_SUCCESS) {
    for (;;) {
      FWPM_PROVIDER0** entries = nullptr;
      UINT32 count = 0;
      if (::FwpmProviderEnum0(engine, en, 256, &entries, &count) != ERROR_SUCCESS)
        break;
      if (count == 0) {
        if (entries) ::FwpmFreeMemory0(reinterpret_cast<void**>(&entries));
        break;
      }
      for (UINT32 i = 0; i < count; ++i) {
        if (entries[i] && IsOurProvider(&entries[i]->providerKey))
          providerKeys.push_back(entries[i]->providerKey);
      }
      ::FwpmFreeMemory0(reinterpret_cast<void**>(&entries));
      if (count < 256) break;
    }
    ::FwpmProviderDestroyEnumHandle0(engine, en);
  }

  const int found = static_cast<int>(filterIds.size() + sublayerKeys.size() +
                                     providerKeys.size());
  if (found == 0) {
    ::FwpmEngineClose0(engine);
    return 0;
  }

  if (!remove) {
    // Same discipline as SweepOrphanedTunnel: report, do not mutate.
    LogWarn("wfp: {} LEFTOVER filter-engine object(s) from a previous run "
            "({} filters, {} sublayers, {} providers). NOT cleaning them: this "
            "process is OBSERVE-ONLY (rpc-only). To take them back: STOP THIS "
            "PROCESS FIRST, then run `urnetworkd revert` from an elevated "
            "prompt.",
            found, filterIds.size(), sublayerKeys.size(), providerKeys.size());
    ::FwpmEngineClose0(engine);
    return found;
  }

  // With a dynamic session there should never be anything here. Finding
  // something means an older build installed static or persistent objects, or
  // somebody hand-installed a lockdown — the case this path exists for.
  LogWarn("wfp: {} leftover filter-engine object(s) from a previous run "
          "({} filters, {} sublayers, {} providers) — a dynamic session should "
          "leave none, so this was static or persistent state. Purging.",
          found, filterIds.size(), sublayerKeys.size(), providerKeys.size());

  if (::FwpmTransactionBegin0(engine, 0) == ERROR_SUCCESS) {
    // Filters first, then sublayers, then providers: a sublayer holding filters
    // cannot be deleted, and a provider owning either cannot be either.
    for (UINT64 id : filterIds) ::FwpmFilterDeleteById0(engine, id);
    for (const GUID& key : sublayerKeys) ::FwpmSubLayerDeleteByKey0(engine, &key);
    for (const GUID& key : providerKeys) ::FwpmProviderDeleteByKey0(engine, &key);
    DWORD commit = ::FwpmTransactionCommit0(engine);
    if (commit != ERROR_SUCCESS) {
      LogError("wfp: purge commit failed: {:#x} — the leftovers are still in "
               "place. `net stop bfe` / `net start bfe` from an elevated prompt "
               "clears every non-persistent filter on the machine if you are "
               "stuck.",
               commit);
    } else {
      LogWarn("wfp: purged {} leftover object(s)", found);
    }
  }
  ::FwpmEngineClose0(engine);
  return found;
}

}  // namespace urnw
