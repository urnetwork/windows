// SPDX-License-Identifier: MPL-2.0
#include "SelfTest.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <iterator>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "ConnectionHealth.h"
#include "ConsoleArgs.h"
#include "InstallVerb.h"
#include "NetPolicy.h"
#include "NetworkConfig.h"
#include "Protocol.h"
#include "RpcSessionBlob.h"
#include "Sdk.h"
#include "StopBudget.h"
#include "UpdateFormats.h"
#include "Version.h"
#include "VersionGrammar.h"
#include "WfpPolicy.h"
#include "WindowTrace.h"

namespace urnw {
namespace {

int g_pass = 0;
int g_fail = 0;

void Check(bool ok, const std::string& what, const std::string& detail = {}) {
  if (ok) {
    ++g_pass;
    std::printf("  PASS  %s\n", what.c_str());
  } else {
    ++g_fail;
    std::printf("  FAIL  %s%s%s\n", what.c_str(), detail.empty() ? "" : " — ",
                detail.c_str());
  }
}

void Section(const char* name) { std::printf("\n== %s ==\n", name); }

// The route set exactly as it was written by hand before NetPolicy.h derived
// it. Kept ONLY as a test fixture: it is the arithmetically-validated datapath
// the P7 gates were written against, so if the derivation ever stops producing
// it, that is a change to the machine's routing table and it must be a
// deliberate one.
constexpr net::V4Prefix kHistoricalCaptureSet[] = {
    {0x00000000u, 5},  {0x08000000u, 7},  {0x0B000000u, 8},  {0x0C000000u, 6},
    {0x10000000u, 4},  {0x20000000u, 3},  {0x40000000u, 2},  {0x80000000u, 3},
    {0xA0000000u, 5},  {0xA8000000u, 6},  {0xAC000000u, 12}, {0xAC200000u, 11},
    {0xAC400000u, 10}, {0xAC800000u, 9},  {0xAD000000u, 8},  {0xAE000000u, 7},
    {0xB0000000u, 4},  {0xC0000000u, 9},  {0xC0800000u, 11}, {0xC0A00000u, 13},
    {0xC0A90000u, 16}, {0xC0AA0000u, 15}, {0xC0AC0000u, 14}, {0xC0B00000u, 12},
    {0xC0C00000u, 10}, {0xC1000000u, 8},  {0xC2000000u, 7},  {0xC4000000u, 6},
    {0xC8000000u, 5},  {0xD0000000u, 4},  {0xE0000000u, 3},
};

std::string PrefixText(const net::V4Prefix& p) {
  return std::format("{}.{}.{}.{}/{}", (p.network >> 24) & 0xFF,
                     (p.network >> 16) & 0xFF, (p.network >> 8) & 0xFF,
                     p.network & 0xFF, p.prefix);
}

// --- part 1: the single source of truth ------------------------------------

void TestNetPolicyTable() {
  Section("NetPolicy — one table for the route set and the firewall's LAN permit");

  Check(net::kTunCaptureV4Count == 31,
        "the derived capture set has 31 prefixes",
        std::format("got {}", net::kTunCaptureV4Count));

  bool same = net::kTunCaptureV4Count == std::size(kHistoricalCaptureSet);
  std::string firstDiff;
  if (same) {
    for (size_t i = 0; i < net::kTunCaptureV4Count; ++i) {
      if (!(net::kTunCaptureV4[i] == kHistoricalCaptureSet[i])) {
        same = false;
        firstDiff = std::format("index {}: derived {} vs validated {}", i,
                                PrefixText(net::kTunCaptureV4[i]),
                                PrefixText(kHistoricalCaptureSet[i]));
        break;
      }
    }
  }
  Check(same,
        "the derived capture set is byte-identical to the validated datapath",
        firstDiff);

  // Aligned: every prefix's host bits are zero. A misaligned prefix is accepted
  // by CreateIpForwardEntry2 with the host bits silently masked off, so it
  // produces a route that is not the one that was written.
  bool aligned = true;
  for (const auto& p : net::kTunCaptureV4) {
    const uint32_t mask = net::detail::PrefixMask(p.prefix);
    if ((p.network & mask) != p.network) aligned = false;
  }
  Check(aligned, "every capture prefix is aligned to its own length");

  // Sorted ascending — not cosmetic: the log and the gate transcript are read
  // against this order.
  bool sorted = true;
  for (size_t i = 1; i < net::kTunCaptureV4Count; ++i) {
    if (net::kTunCaptureV4[i].network <= net::kTunCaptureV4[i - 1].network)
      sorted = false;
  }
  Check(sorted, "the capture set is sorted ascending");

  // Exact partition: capture + bypass covers all 2^32 addresses with no overlap.
  // Summed as a 64-bit count of addresses, which catches both a gap (a range
  // that reaches neither the tunnel nor the LAN) and an overlap.
  uint64_t total = 0;
  for (const auto& p : net::kTunCaptureV4) total += 1ull << (32 - p.prefix);
  for (const auto& p : net::kLocalBypassV4) total += 1ull << (32 - p.prefix);
  Check(total == (1ull << 32),
        "capture ∪ bypass covers 0.0.0.0/0 exactly, with no gap and no overlap",
        std::format("covered {} of {} addresses", total, 1ull << 32));

  // No capture prefix may intersect a bypass prefix. This is THE property the
  // whole file exists for: it is what makes "the firewall permits the bypass
  // set" and "the routes send everything else to the tun" the same statement.
  bool disjoint = true;
  std::string overlap;
  for (const auto& c : net::kTunCaptureV4) {
    for (const auto& b : net::kLocalBypassV4) {
      if (net::detail::Contains(b, c) || net::detail::Contains(c, b)) {
        disjoint = false;
        overlap = std::format("{} intersects {}", PrefixText(c), PrefixText(b));
      }
    }
  }
  Check(disjoint, "no captured prefix intersects a bypassed prefix", overlap);

  struct { uint32_t addr; bool bypass; const char* label; } probes[] = {
      {0x0A000001u, true, "10.0.0.1"},
      {0xAC100001u, true, "172.16.0.1"},
      {0xC0A80001u, true, "192.168.0.1"},
      {0xAC0FFFFFu, false, "172.15.255.255 (just below 172.16/12)"},
      {0xAC200000u, false, "172.32.0.0 (just above 172.16/12)"},
      {0x08080808u, false, "8.8.8.8"},
      {0xA9FE0201u, false, "169.254.2.1 (our own tun address — CAPTURED)"},
      {0xE00000FBu, false, "224.0.0.251 (mDNS — CAPTURED, see NetPolicy.h)"},
  };
  bool probesOk = true;
  std::string probeDetail;
  for (const auto& p : probes) {
    if (net::IsLocalBypassV4(p.addr) != p.bypass) {
      probesOk = false;
      probeDetail = p.label;
    }
  }
  Check(probesOk, "IsLocalBypassV4 agrees with the table on 8 probe addresses",
        probeDetail);
}

// --- part 2: the filter set -------------------------------------------------

using Specs = std::vector<WfpFilterSpec>;

WfpConfig SampleConfig(uint64_t luid) {
  WfpConfig cfg;
  cfg.tun_luid = luid;
  cfg.tunnel_resolvers_v4 = {"169.254.2.53"};
  // What TunnelController reads off the machine's own adapters for every
  // non-Connected state. Ignored while Connected, where the tunnel's resolvers
  // are the path.
  cfg.host_resolvers_v4 = {"192.168.1.1"};
  cfg.service_image_path = L"C:\\Program Files\\URnetwork\\urnetworkd.exe";
  // SET IN EVERY SAMPLE, INCLUDING THE NO-TUNNEL ONE. The whole assertion about
  // this field is that populating it widens exactly one state, so a fixture that
  // left it empty for Armed and Connecting would make that test vacuous — it
  // would be checking that an absent path produces no filter, which is the
  // uninteresting half.
  cfg.app_image_path = L"C:\\Program Files\\URnetwork\\URnetwork.exe";
  return cfg;
}

// The config TunnelController builds when there is NO TUN — which is both Armed
// and Connecting: no tun luid, no tunnel resolver, and the host's own resolvers
// read fresh off the adapters.
//
// The same config is deliberately used for both states, because the whole point
// of the split is that the two states are different POLICIES over the SAME
// facts. Armed ignores host_resolvers_v4 entirely (asserted below); Connecting
// is the only state that turns it into a filter.
WfpConfig NoTunnelConfig(const WfpConfig& base) {
  WfpConfig cfg;
  cfg.host_resolvers_v4 = base.host_resolvers_v4;
  cfg.service_image_path = base.service_image_path;
  cfg.app_image_path = base.app_image_path;
  return cfg;
}

// A DNS-sublayer PERMIT on remote port 53 that a query from ANOTHER PROCESS can
// match — i.e. matched by remote ADDRESS, not by app id and not by the loopback
// flag. This is the shape the service's own name resolution needs, because that
// resolution is performed by the DNS Client service inside svchost.exe and not
// by urnetworkd.exe. See WfpPolicy.cpp filters 9 and 9b.
bool IsServiceDnsPath(const WfpFilterSpec& f) {
  if (f.block || f.sublayer != WfpSublayer::Dns) return false;
  bool port53 = false, byAddress = false;
  for (const auto& c : f.conditions) {
    if (c.field == WfpField::RemotePort && c.number == 53) port53 = true;
    if (c.field == WfpField::RemoteAddrV4 || c.field == WfpField::RemoteAddrV6)
      byAddress = true;
    if (c.field == WfpField::AppId) return false;
    if (c.field == WfpField::Flags) return false;  // the loopback permit
  }
  return port53 && byAddress;
}

bool HasServiceDnsPath(const Specs& s) {
  for (const auto& f : s)
    if (IsServiceDnsPath(f)) return true;
  return false;
}

bool HasName(const Specs& s, const std::string& name);

// Can a name be resolved at all in this state, by EITHER route the policy has:
// a permit our own resolution can actually match, or the port-53 hard block
// having stood down (which permits plaintext DNS to any server via filter 7's
// lift). This is the predicate the reachability guard below is written in: a
// state a connection attempt runs in must be resolvable one way or the other,
// and Armed must be resolvable NEITHER way.
bool CanResolve(const Specs& s) {
  return HasServiceDnsPath(s) || !HasName(s, "urnetwork-block-dns-v4");
}

// The multiset of filter names, so two policies can be compared as sets without
// caring about emission order.
std::multiset<std::string> Names(const Specs& s) {
  std::multiset<std::string> out;
  for (const auto& f : s) out.insert(f.name);
  return out;
}

bool HasName(const Specs& s, const std::string& name) {
  for (const auto& f : s)
    if (f.name == name) return true;
  return false;
}

int CountName(const Specs& s, const std::string& name) {
  int n = 0;
  for (const auto& f : s)
    if (f.name == name) ++n;
  return n;
}

const WfpFilterSpec* Find(const Specs& s, const std::string& name) {
  for (const auto& f : s)
    if (f.name == name) return &f;
  return nullptr;
}

bool HasCondition(const WfpFilterSpec& f, WfpField field) {
  for (const auto& c : f.conditions)
    if (c.field == field) return true;
  return false;
}

void TestFilterSet() {
  Section("WfpPolicy — filter-set construction (no BFE contacted)");

  const WfpConfig cfg = SampleConfig(0x1234'5678'9ABC'DEF0ull);
  const Specs off = BuildFilterSet(WfpState::Off, cfg);
  const Specs armed = BuildFilterSet(WfpState::Armed, NoTunnelConfig(cfg));
  const Specs connecting = BuildFilterSet(WfpState::Connecting, NoTunnelConfig(cfg));
  const Specs connected = BuildFilterSet(WfpState::Connected, cfg);

  Check(off.empty(), "state Off installs nothing at all",
        std::format("got {} filters", off.size()));

  Check(!armed.empty() && !connecting.empty() && !connected.empty(),
        "Armed, Connecting and Connected all produce a policy",
        std::format("armed={} connecting={} connected={}", armed.size(),
                    connecting.size(), connected.size()));

  // --- the floor -----------------------------------------------------------
  for (const auto& [label, set] : {std::pair<const char*, const Specs&>{"armed", armed},
                                   {"connecting", connecting},
                                   {"connected", connected}}) {
    Check(HasName(set, "urnetwork-block-all-v4-out") &&
              HasName(set, "urnetwork-block-all-v4-in"),
          std::format("{}: blocks all IPv4 at both ALE layers", label));
    Check(HasName(set, "urnetwork-block-all-v6-out") &&
              HasName(set, "urnetwork-block-all-v6-in"),
          std::format("{}: blocks all IPv6 at both ALE layers (R7)", label));
    Check(HasName(set, "urnetwork-block-dns-v4") &&
              HasName(set, "urnetwork-block-dns-v6"),
          std::format("{}: hard-blocks remote port 53 in the DNS sublayer (R6)",
                      label));
    Check(HasName(set, "urnetwork-lift-dns-v4"),
          std::format("{}: lifts port 53 out of the baseline so the LAN permit "
                      "cannot leak a query to the router's resolver",
                      label));
    Check(CountName(set, "urnetwork-permit-service-v4") == 2 &&
              CountName(set, "urnetwork-permit-service-v6") == 2,
          std::format("{}: permits urnetworkd at all four ALE layers", label));
    Check(HasName(set, "urnetwork-permit-service-dns-v4"),
          std::format("{}: repeats the urnetworkd exemption in the DNS "
                      "sublayer (block beats permit ACROSS sublayers)",
                      label));
    Check(CountName(set, "urnetwork-permit-loopback-v4") == 2 &&
              CountName(set, "urnetwork-permit-loopback-v6") == 2,
          std::format("{}: permits loopback at all four ALE layers", label));
    Check(HasName(set, "urnetwork-permit-dhcpv4-out") &&
              HasName(set, "urnetwork-permit-dhcpv4-in"),
          std::format("{}: permits DHCPv4 in both directions", label));
    Check(HasName(set, "urnetwork-permit-dhcpv6-out") &&
              HasName(set, "urnetwork-permit-ndp-rs-out") &&
              HasName(set, "urnetwork-permit-ndp-ra-in"),
          std::format("{}: permits DHCPv6 and NDP even while v6 is blocked",
                      label));
    Check(HasName(set, "urnetwork-permit-lan-out") &&
              HasName(set, "urnetwork-permit-lan-in"),
          std::format("{}: permits the LAN", label));
  }

  // --- the states differ ONLY where they should ----------------------------
  const std::string kHostResolverPermit = "urnetwork-permit-dns-host-resolver";

  Check(!HasName(armed, "urnetwork-permit-tun-v4") &&
            !HasName(armed, "urnetwork-permit-dns-tunnel-resolver") &&
            !HasName(connecting, "urnetwork-permit-tun-v4") &&
            !HasName(connecting, "urnetwork-permit-dns-tunnel-resolver"),
        "neither Armed nor Connecting has a tun permit or a tunnel-resolver DNS "
        "permit (there is no tun in either)");
  Check(CountName(connected, "urnetwork-permit-tun-v4") == 2 &&
            CountName(connected, "urnetwork-permit-tun-v6") == 2,
        "Connected permits the tun LUID at all four ALE layers");
  Check(CountName(connected, "urnetwork-permit-dns-tunnel-resolver") == 1,
        "Connected permits exactly one tunnel resolver (one was configured)");

  // --- THE UI PROCESS: CONNECTED ONLY --------------------------------------
  //
  // The app runs its own SDK instance, so once the tunnel is up its platform
  // traffic follows the route table into the tun and the UI cannot reach the
  // platform when the tunnel has no working exit — the moment the user is most
  // likely to be looking at it. The fix is a pair: the app binds its own sdk
  // egress to the physical NIC (TunnelStatus::egress_index4) and this permit
  // stops the weight-0 baseline floor blocking the result.
  //
  // THESE FOUR CHECKS ARE THE GUARD ON THE OTHER HALF OF THAT TRADE. The owner's
  // standing ruling is that ARMED permits urnetworkd and nothing else, so a kill
  // switch cannot put user traffic on the physical NIC in the clear. Every
  // fixture above now carries app_image_path, so "Armed does not have it" is a
  // statement about the POLICY and not about the config.
  Check(CountName(connected, "urnetwork-permit-app-v4") == 2 &&
            CountName(connected, "urnetwork-permit-app-v6") == 2,
        "Connected permits URnetwork.exe at all four ALE layers — the app's own "
        "sdk instance can reach the platform off-tunnel while connected");
  Check(!HasName(armed, "urnetwork-permit-app-v4") &&
            !HasName(armed, "urnetwork-permit-app-v6"),
        "ARMED DOES NOT PERMIT THE APP, even with app_image_path set. The armed "
        "state permits urnetworkd and nothing else, so a kill switch cannot put "
        "user traffic on the physical NIC in the clear");
  Check(!HasName(connecting, "urnetwork-permit-app-v4") &&
            !HasName(connecting, "urnetwork-permit-app-v6"),
        "Connecting does not permit the app either — it is Armed plus filter 9b "
        "and nothing else, which is what keeps the Armed -> Connecting "
        "transition one-directional");
  {
    // Structural, like the Armed-independent-of-resolvers check below: with no
    // app path there must be NO app filter in any state, so a machine where
    // URnetwork.exe is missing gets today's policy exactly.
    WfpConfig noApp = cfg;
    noApp.app_image_path.clear();
    bool none = true;
    for (WfpState s : {WfpState::Armed, WfpState::Connecting, WfpState::Connected}) {
      const Specs set = BuildFilterSet(
          s, s == WfpState::Connected ? noApp : NoTunnelConfig(noApp));
      if (HasName(set, "urnetwork-permit-app-v4") ||
          HasName(set, "urnetwork-permit-app-v6"))
        none = false;
    }
    Check(none,
          "with no app image path, NO state emits an app permit — a machine "
          "where URnetwork.exe cannot be found gets exactly the policy it got "
          "before this filter existed");
  }
  {
    // It must be an APP-ID permit and nothing else. A permit that quietly grew
    // an address or interface condition would be a different filter wearing the
    // same name, and the disclosure logic keys off structure.
    const WfpFilterSpec* app = Find(connected, "urnetwork-permit-app-v4");
    Check(app != nullptr && app->conditions.size() == 1 &&
              app->conditions[0].field == WfpField::AppId &&
              app->sublayer == WfpSublayer::Baseline && !app->block,
          "the app permit is a single ALE_APP_ID condition in the BASELINE "
          "sublayer — not an address permit, and not in the DNS sublayer (the "
          "app resolves through svchost, so an app-id DNS permit would match "
          "nothing)");
    Check(!HasName(connected, "urnetwork-permit-app-dns-v4") &&
              !HasName(connected, "urnetwork-permit-app-dns-v6"),
          "no app permit is repeated in the DNS sublayer — the app's name "
          "resolution still goes to the tunnel's resolvers over the tun, and "
          "this policy does not pretend otherwise");
  }

  // --- CONNECTING IS EXACTLY ARMED PLUS ONE FILTER -------------------------
  //
  // This is the assertion the whole Armed/Connecting split rests on. The two
  // states were merged precisely so that the transition between them could not
  // be a window where the policy is briefly weaker; splitting them keeps that
  // property only if the difference is ONE-DIRECTIONAL. Asserted as a multiset
  // difference in both directions, with the one permitted name spelled out, so
  // a second name silently joining it fails here rather than in the field.
  {
    const std::multiset<std::string> a = Names(armed);
    const std::multiset<std::string> c = Names(connecting);
    std::multiset<std::string> onlyInArmed, onlyInConnecting;
    std::set_difference(a.begin(), a.end(), c.begin(), c.end(),
                        std::inserter(onlyInArmed, onlyInArmed.end()));
    std::set_difference(c.begin(), c.end(), a.begin(), a.end(),
                        std::inserter(onlyInConnecting, onlyInConnecting.end()));
    Check(onlyInArmed.empty(),
          "Connecting is a SUPERSET of Armed — the Armed -> Connecting "
          "transition only ever widens, so nothing permitted while armed is "
          "interrupted by a connection attempt starting",
          onlyInArmed.empty() ? "" : *onlyInArmed.begin());
    Check(onlyInConnecting.size() == 1 &&
              *onlyInConnecting.begin() == kHostResolverPermit,
          std::format("the ONLY difference between Armed and Connecting is {} — "
                      "the machine-wide plaintext-DNS permit, which is the "
                      "entire cost of opening the window",
                      kHostResolverPermit),
          std::format("{} extra filter(s): {}", onlyInConnecting.size(),
                      onlyInConnecting.empty() ? std::string("none")
                                               : *onlyInConnecting.begin()));
  }

  // Everything Armed permits, Connected must also permit. There is no exception
  // any more: Armed no longer carries the host-resolver permit, so the narrowing
  // that used to need spelling out has moved to the Connecting -> Connected
  // edge below. Any name present in Armed but absent from Connected would be a
  // transition window where the policy is briefly weaker in the direction that
  // breaks the machine.
  std::set<std::string> connectedNames;
  for (const auto& f : connected) connectedNames.insert(f.name);
  bool superset = true;
  std::string missing;
  for (const auto& f : armed) {
    if (!connectedNames.count(f.name)) {
      superset = false;
      missing = f.name;
    }
  }
  Check(superset,
        "Connected is a superset of Armed with NO exceptions (no transition "
        "window)",
        missing);

  // The Connecting -> Connected edge is where the deliberate narrowing lives.
  // Connected does not merely drop the host-resolver permit, it REPLACES it with
  // a strictly narrower one (the tunnel's own resolvers, over the tun), and
  // carrying it forward would permit the physical adapter's resolvers while
  // connected — which IS the R6 leak.
  bool connectedSupersetOfConnecting = true;
  std::string missingOnConnect;
  for (const auto& f : connecting) {
    if (f.name == kHostResolverPermit) continue;
    if (!connectedNames.count(f.name)) {
      connectedSupersetOfConnecting = false;
      missingOnConnect = f.name;
    }
  }
  Check(connectedSupersetOfConnecting,
        "Connected is a superset of Connecting apart from the one deliberately "
        "narrowed DNS permit",
        missingOnConnect);
  Check(HasName(connecting, kHostResolverPermit) &&
            !HasName(armed, kHostResolverPermit) &&
            !HasName(connected, kHostResolverPermit),
        "the host-resolver DNS permit exists in CONNECTING ONLY — not while "
        "idle (it is machine-wide, and idle is where a kill switch lives) and "
        "not while connected (that would permit the physical adapter's "
        "resolvers, which is R6)");

  // --- Armed does not depend on the host's resolvers at all ----------------
  //
  // Structural, not incidental. TunnelController's connecting watchdog rebuilds
  // the armed policy from a thread that holds no lock and reads no session
  // state; that is only sound if the armed filter set is a function of nothing
  // the session knows.
  {
    WfpConfig noResolvers = NoTunnelConfig(cfg);
    noResolvers.host_resolvers_v4.clear();
    WfpConfig manyResolvers = NoTunnelConfig(cfg);
    manyResolvers.host_resolvers_v4 = {"192.168.1.1", "1.1.1.1", "9.9.9.9"};
    const bool independent =
        Names(BuildFilterSet(WfpState::Armed, noResolvers)) == Names(armed) &&
        Names(BuildFilterSet(WfpState::Armed, manyResolvers)) == Names(armed);
    Check(independent,
          "the Armed policy is identical with 0, 1 and 3 host resolvers — it is "
          "a function of no session state, which is what lets the connecting "
          "watchdog rebuild it off-thread");
  }

  // --- the LAN permit is built from THE table ------------------------------
  const WfpFilterSpec* lan = Find(connected, "urnetwork-permit-lan-out");
  bool lanMatches = lan != nullptr &&
                    lan->conditions.size() == std::size(net::kLocalBypassV4);
  if (lanMatches) {
    for (size_t i = 0; i < lan->conditions.size(); ++i) {
      const auto& c = lan->conditions[i];
      const auto& p = net::kLocalBypassV4[i];
      if (c.field != WfpField::RemoteAddrV4 || c.v4_addr != p.network ||
          c.v4_prefix != p.prefix) {
        lanMatches = false;
      }
    }
  }
  Check(lanMatches,
        "the LAN permit's conditions ARE net::kLocalBypassV4, prefix for "
        "prefix — the firewall and the route table cannot disagree");

  // --- weights --------------------------------------------------------------
  bool weightsValid = true;
  for (const auto& f : connected)
    if (f.weight > 15) weightsValid = false;
  Check(weightsValid, "every weight is a valid libwfp class (0..15)");

  // WireGuard encodes this as an assertion in rules.go for the same reason: if
  // the DNS block outweighed the tunnel-resolver permit inside the DNS
  // sublayer, DNS would fail closed everywhere including through the tunnel.
  const WfpFilterSpec* dnsPermit =
      Find(connected, "urnetwork-permit-dns-tunnel-resolver");
  const WfpFilterSpec* dnsBlock = Find(connected, "urnetwork-block-dns-v4");
  Check(dnsPermit && dnsBlock && dnsPermit->weight > dnsBlock->weight,
        "the tunnel-resolver DNS permit outweighs the port-53 block inside the "
        "DNS sublayer");

  const WfpFilterSpec* blockAll = Find(connected, "urnetwork-block-all-v4-out");
  const WfpFilterSpec* svc = Find(connected, "urnetwork-permit-service-v4");
  Check(blockAll && blockAll->weight == 0 && blockAll->conditions.empty(),
        "the block-all floor is unconditional and at the minimum weight");
  Check(svc && blockAll && svc->weight > blockAll->weight,
        "our own service outweighs the floor — without this the machine is "
        "armed, blocked and unable to reconnect");

  // --- byte order -----------------------------------------------------------
  // The single easiest way to ship a leak that reads clean: FWP_UINT16 is HOST
  // byte order, so htons(53) makes a filter for port 13568.
  bool hostOrderPorts = true;
  std::string wrongPort;
  for (const auto& f : connected) {
    for (const auto& c : f.conditions) {
      if (c.field != WfpField::RemotePort && c.field != WfpField::LocalPort)
        continue;
      // Every port and ICMPv6 type this policy uses is < 6000; a byte-swapped
      // one lands above 13000.
      if (c.number > 6000) {
        hostOrderPorts = false;
        wrongPort = std::format("{} has port/type {}", f.name, c.number);
      }
    }
  }
  Check(hostOrderPorts,
        "every port and ICMPv6 type is in HOST byte order (no accidental "
        "htons)",
        wrongPort);

  const WfpFilterSpec* dnsBlockSpec = Find(connected, "urnetwork-block-dns-v4");
  bool dnsIs53 = false;
  if (dnsBlockSpec) {
    for (const auto& c : dnsBlockSpec->conditions)
      if (c.field == WfpField::RemotePort && c.number == 53) dnsIs53 = true;
  }
  Check(dnsIs53, "the DNS block really is remote port 53, not 13568");

  // --- protocol OR groups stay adjacent ------------------------------------
  // Consecutive same-field conditions are OR'd by WFP; a non-adjacent pair is
  // not a documented OR. Every "UDP or TCP" filter must therefore have its two
  // protocol conditions next to each other.
  bool adjacency = true;
  std::string adjDetail;
  for (const auto& f : connected) {
    int lastProto = -2;
    int protoCount = 0;
    for (int i = 0; i < static_cast<int>(f.conditions.size()); ++i) {
      if (f.conditions[i].field != WfpField::Protocol) continue;
      ++protoCount;
      if (protoCount > 1 && i != lastProto + 1) {
        adjacency = false;
        adjDetail = f.name;
      }
      lastProto = i;
    }
  }
  Check(adjacency, "same-field (UDP|TCP) conditions are adjacent, so WFP OR's "
                   "them", adjDetail);

  // --- ICMPv6 filters keep their protocol condition ------------------------
  // FWPM_CONDITION_ICMP_TYPE and FWPM_CONDITION_IP_LOCAL_PORT are the SAME
  // GUID. An NDP filter that lost its IP_PROTOCOL == 58 condition silently
  // becomes "permit local port 133", which is a hole.
  bool ndpGuarded = true;
  std::string ndpDetail;
  for (const auto& f : connected) {
    if (f.name.rfind("urnetwork-permit-ndp", 0) != 0) continue;
    bool proto58 = false;
    for (const auto& c : f.conditions)
      if (c.field == WfpField::Protocol && c.number == 58) proto58 = true;
    if (!proto58) {
      ndpGuarded = false;
      ndpDetail = f.name;
    }
  }
  Check(ndpGuarded,
        "every NDP filter AND's IP_PROTOCOL == 58, so the ICMP-type/local-port "
        "GUID aliasing cannot open a port",
        ndpDetail);

  // --- the tun permit is a LUID, never an interface index ------------------
  const WfpFilterSpec* tun = Find(connected, "urnetwork-permit-tun-v4");
  Check(tun && HasCondition(*tun, WfpField::LocalInterface) &&
            tun->conditions[0].number == cfg.tun_luid,
        "the tun permit matches on the interface LUID (indices are recycled; "
        "an index-based permit can end up permitting another adapter)");

  // --- config knobs actually do something ----------------------------------
  WfpConfig noV6 = cfg;
  noV6.block_ipv6 = false;
  const Specs v6Open = BuildFilterSet(WfpState::Connected, noV6);
  Check(!HasName(v6Open, "urnetwork-block-all-v6-out"),
        "block_ipv6=false removes the v6 floor (and reopens R7 — it exists for "
        "the day the tunnel carries v6, not as a preference)");

  WfpConfig noNameBlock = cfg;
  noNameBlock.block_local_name_resolution = false;
  const Specs nameOpen = BuildFilterSet(WfpState::Connected, noNameBlock);
  Check(HasName(connected, "urnetwork-block-llmnr-v4") &&
            HasName(connected, "urnetwork-block-mdns-v4") &&
            HasName(connected, "urnetwork-block-netbios-v4"),
        "LLMNR, mDNS and NetBIOS are blocked by default (LLMNR fires on every "
        "DNS failure, which is the state this policy creates)");
  Check(!HasName(nameOpen, "urnetwork-block-llmnr-v4"),
        "block_local_name_resolution=false removes those three");

  WfpConfig noLan = cfg;
  noLan.allow_lan = false;
  Check(!HasName(BuildFilterSet(WfpState::Connected, noLan),
                 "urnetwork-permit-lan-out"),
        "allow_lan=false removes the LAN permit");

  WfpConfig noService = cfg;
  noService.service_image_path.clear();
  Check(!HasName(BuildFilterSet(WfpState::Connected, noService),
                 "urnetwork-permit-service-v4"),
        "an empty service path emits no app-id filter rather than a filter "
        "matching everything");

  // The numbers p7-gates.ps1 cross-checks against the machine. Connected is
  // printed with its resolver count because filter 10 emits ONE FILTER PER
  // TUNNEL RESOLVER — the count is not a constant, and a gate that hardcodes it
  // fails on a two-resolver session for no real reason.
  std::printf("  note  armed = %zu filters, connecting = %zu, connected = %zu "
              "(with %zu tunnel resolver(s); filter 10 emits one PER resolver)\n",
              armed.size(), connecting.size(), connected.size(),
              cfg.tunnel_resolvers_v4.size());
}

// --- part 2b: the service can always resolve WHEN IT IS TRYING TO CONNECT ----
//
// THE INVARIANT, AS RESTATED BY THE Armed/Connecting SPLIT (WfpPolicy.h,
// AttemptsConnection):
//
//   EVERY STATE FROM WHICH A CONNECTION ATTEMPT IS MADE MUST HAVE A DNS PATH.
//
// It used to read "there is no state in which the policy is installed and the
// service cannot resolve", and that form is now WRONG in a way that would undo
// the change if it were left here: Armed is deliberately a state in which the
// policy is installed and nothing can resolve, including us. Read literally, the
// old invariant would stand the port-53 block down in Armed and reopen plaintext
// DNS machine-wide on an idle machine — the exact thing the split removed.
//
// What the old form was really protecting is intact and is asserted below: the
// unrecoverable state — no resolution -> no connect -> still armed -> no
// resolution, whose only exits are `sc stop urnetworkd` or an elevated revert —
// must be unreachable. It is unreachable because the state a connect RUNS IN is
// Connecting, not Armed, and Connecting always has a path (or stands the block
// down). The transition is TunnelController's responsibility; that it exists at
// all is asserted by the reachability guard at the end of this section.
//
// The trap these assertions exist to catch: the DNS-sublayer permit for our own
// service is scoped on the app id of urnetworkd.exe, and OUR NAME RESOLUTION
// DOES NOT COME OUT OF urnetworkd.exe. The SDK is Go; Go on Windows resolves
// through the OS resolver (net/conf.go returns the fallback order
// unconditionally for GOOS=windows, i.e. net/lookup_windows.go's GetAddrInfoW),
// which is an RPC into the DNS Client service — so the wire query is issued by
// svchost.exe. An app-id permit for urnetworkd never matches it. That is
// already documented one filter further down, where it was used to scope the
// tunnel-resolver permit on address rather than identity; it was not applied to
// the service's own permit.

void TestServiceDnsPath() {
  Section("every state a connection attempt runs in can resolve — and ARMED "
          "deliberately cannot");

  const WfpConfig cfg = SampleConfig(0x1234'5678'9ABC'DEF0ull);
  const Specs armed = BuildFilterSet(WfpState::Armed, NoTunnelConfig(cfg));
  const Specs connecting = BuildFilterSet(WfpState::Connecting, NoTunnelConfig(cfg));
  const Specs connected = BuildFilterSet(WfpState::Connected, cfg);

  // Armed is NOT the connecting state any more. This Check replaces the one that
  // asserted the opposite ("there are exactly three policy states, and Armed is
  // also the CONNECTING state"), and it is kept rather than deleted because the
  // assumption it guards has only reversed, not gone away: everything below
  // reads differently depending on which is true.
  Check(AttemptsConnection(WfpState::Connecting) &&
            AttemptsConnection(WfpState::Connected) &&
            !AttemptsConnection(WfpState::Armed) &&
            !AttemptsConnection(WfpState::Off),
        "ARMED IS NOT THE CONNECTING STATE. A connection attempt is made from "
        "Connecting and Connected only, and that is the set the DNS-path "
        "invariant quantifies over");

  // --- ARMED: no permit, block present -------------------------------------
  // The product decision, asserted as policy rather than described in prose.
  Check(!HasServiceDnsPath(armed),
        "ARMED has NO DNS path — no port-53 permit that a query from another "
        "process could match",
        "the permit is address-scoped and therefore machine-wide; keeping it "
        "while idle lets every process on the box resolve in plaintext for as "
        "long as the kill switch is on");
  Check(!HasName(armed, "urnetwork-permit-dns-host-resolver"),
        "ARMED does not carry the host-resolver permit");
  Check(HasName(armed, "urnetwork-block-dns-v4") &&
            HasName(armed, "urnetwork-block-dns-v6"),
        "ARMED carries the port-53 hard block at BOTH ALE connect layers, "
        "unconditionally — armed-with-a-block-and-no-path is the design, not "
        "the unrecoverable state");
  Check(!CanResolve(armed),
        "nothing resolves while ARMED, by either route: no matchable permit AND "
        "no stood-down block. That IS the kill switch");

  // --- CONNECTING and CONNECTED: a path, always ----------------------------
  Check(HasServiceDnsPath(connecting),
        "CONNECTING has at least one DNS path the service can actually use (a "
        "port-53 permit matched by ADDRESS, not by app id)",
        "without this a connect attempt cannot resolve, so an armed machine can "
        "never come back and cannot be recovered from inside the product");
  Check(HasServiceDnsPath(connected),
        "CONNECTED has at least one DNS path the service can actually use");

  // The app-id permit must NOT be mistaken for that path. If this ever passes,
  // the whole section above has stopped meaning anything.
  const WfpFilterSpec* appIdPermit =
      Find(connecting, "urnetwork-permit-service-dns-v4");
  Check(appIdPermit && !IsServiceDnsPath(*appIdPermit),
        "the app-id DNS permit is NOT counted as a service DNS path — the "
        "query is issued by svchost.exe (GetAddrInfoW), so it never matches");

  // Same for the loopback permit: it gets the query to a local stub, but the
  // stub's own upstream is a separate port-53 socket the block still catches.
  const WfpFilterSpec* loopbackPermit =
      Find(connecting, "urnetwork-permit-dns-loopback-v4");
  Check(loopbackPermit && !IsServiceDnsPath(*loopbackPermit),
        "the loopback DNS permit is NOT counted as a service DNS path — it "
        "reaches a local stub whose own upstream is still blocked");

  // --- THE INVARIANT, over a matrix of configs -----------------------------
  // block installed  =>  a usable path exists. Checked for every state against
  // every shape of config the service can hand us, including the degenerate
  // ones, because the unrecoverable state must be unreachable by construction
  // and not merely absent from the happy path.
  struct Case { const char* label; WfpConfig cfg; };
  WfpConfig noHost = NoTunnelConfig(cfg);
  noHost.host_resolvers_v4.clear();
  WfpConfig loopbackOnly = NoTunnelConfig(cfg);
  loopbackOnly.host_resolvers_v4 = {};  // HostResolversV4 filters 127/8 out
  WfpConfig junkHost = NoTunnelConfig(cfg);
  junkHost.host_resolvers_v4 = {"not-an-address", ""};
  WfpConfig noTunnelResolver = cfg;
  noTunnelResolver.tunnel_resolvers_v4.clear();
  WfpConfig noLuid = cfg;
  noLuid.tun_luid = 0;
  WfpConfig junkTunnelResolver = cfg;
  junkTunnelResolver.tunnel_resolvers_v4 = {"169.254.2.999"};
  WfpConfig noService = NoTunnelConfig(cfg);
  noService.service_image_path.clear();

  const Case cases[] = {
      {"shipped, no tun", NoTunnelConfig(cfg)},
      {"shipped connected", cfg},
      {"no host resolver", noHost},
      {"loopback-only resolver", loopbackOnly},
      {"unparseable host resolver", junkHost},
      {"connected, no tunnel resolver", noTunnelResolver},
      {"connected, no tun luid", noLuid},
      {"connected, unparseable tunnel resolver", junkTunnelResolver},
      {"no service image path", noService},
  };

  // DIRECTION 1 — block => path, but ONLY where a connection is attempted.
  // This is the half that keeps the unrecoverable state unreachable. Armed is
  // excluded by AttemptsConnection, not by an exception list, so the two places
  // that decide "is this a connecting state" cannot drift apart.
  bool invariant = true;
  std::string broke;
  for (const auto& c : cases) {
    for (WfpState s : {WfpState::Armed, WfpState::Connecting, WfpState::Connected}) {
      if (!AttemptsConnection(s)) continue;
      const Specs set = BuildFilterSet(s, c.cfg);
      const bool blocks = HasName(set, "urnetwork-block-dns-v4") ||
                          HasName(set, "urnetwork-block-dns-v6");
      if (blocks && !HasServiceDnsPath(set)) {
        invariant = false;
        broke = std::format("{} / {}", c.label, ToString(s));
      }
    }
  }
  Check(invariant,
        "NO state a connection is attempted from hard-blocks port 53 without a "
        "DNS path the service can use — the unrecoverable state is unreachable "
        "by construction",
        broke);

  // DIRECTION 2 — path => block, in the connecting states. Standing down is the
  // escape hatch, not the default.
  bool blocksWhenItCan = true;
  std::string slack;
  for (const auto& c : cases) {
    for (WfpState s : {WfpState::Armed, WfpState::Connecting, WfpState::Connected}) {
      if (!AttemptsConnection(s)) continue;
      const Specs set = BuildFilterSet(s, c.cfg);
      if (HasServiceDnsPath(set) && !HasName(set, "urnetwork-block-dns-v4")) {
        blocksWhenItCan = false;
        slack = std::format("{} / {}", c.label, ToString(s));
      }
    }
  }
  Check(blocksWhenItCan,
        "whenever a usable DNS path exists the port-53 hard block IS installed "
        "— standing down is the escape hatch, never the default",
        slack);

  // DIRECTION 3 — the half the split ADDS. Armed blocks port 53 for EVERY shape
  // of config, including the degenerate ones the old invariant used to stand the
  // block down for. Without this, a host with no resolver would silently get an
  // idle kill switch that permits plaintext DNS machine-wide — the defect this
  // change exists to remove, reintroduced by the config rather than by the code.
  bool armedAlwaysBlocks = true;
  std::string armedSlack;
  for (const auto& c : cases) {
    const Specs set = BuildFilterSet(WfpState::Armed, c.cfg);
    if (!HasName(set, "urnetwork-block-dns-v4") ||
        !HasName(set, "urnetwork-block-dns-v6") || CanResolve(set)) {
      armedAlwaysBlocks = false;
      armedSlack = c.label;
    }
  }
  Check(armedAlwaysBlocks,
        "ARMED hard-blocks port 53 for EVERY config shape, including a host with "
        "no resolver at all — the fail-safe stand-down belongs to the connecting "
        "states and must never fire while idle",
        armedSlack);

  // THE REACHABILITY GUARD — the split cannot silently produce an
  // unreconnectable machine.
  //
  // Armed is a dead end by design: nothing resolves there (direction 3). That is
  // only safe because a connect attempt does not run in Armed — it runs in
  // Connecting, which TunnelController enters BEFORE step 3/8, where the first
  // name is resolved. So the guard is: for every config shape, the state
  // reachable from Armed by starting a connection can resolve, by one route or
  // the other. If this ever fails, arming has become a one-way door again and no
  // amount of retrying gets out of it.
  bool reachable = true;
  std::string dead;
  for (const auto& c : cases) {
    if (!CanResolve(BuildFilterSet(WfpState::Connecting, c.cfg))) {
      reachable = false;
      dead = c.label;
    }
  }
  Check(reachable,
        "from ARMED, the state a connection attempt enters (CONNECTING) can "
        "always resolve — by a matchable permit, or by the block standing down. "
        "Arming is never a one-way door",
        dead);

  // What standing down actually opens, asserted rather than asserted-in-prose:
  // everything except port 53 stays blocked, and the local name-resolution
  // protocols stay hard-blocked, so a failed lookup cannot fall back to
  // broadcasting the hostname to the LAN. Now asserted in CONNECTING, the only
  // state that can stand the block down without a tunnel.
  const Specs standDown = BuildFilterSet(WfpState::Connecting, noHost);
  Check(!HasName(standDown, "urnetwork-block-dns-v4") &&
            HasName(standDown, "urnetwork-block-all-v4-out") &&
            HasName(standDown, "urnetwork-block-all-v6-out") &&
            HasName(standDown, "urnetwork-block-llmnr-v4") &&
            HasName(standDown, "urnetwork-block-mdns-v4") &&
            HasName(standDown, "urnetwork-block-netbios-v4"),
        "with the port-53 block stood down, the floor and the LLMNR/mDNS/"
        "NetBIOS blocks are still in force — the residual leak is plaintext "
        "DNS only, and it cannot widen into LLMNR name broadcast");

  // --- the host-resolver permit's shape ------------------------------------
  const WfpFilterSpec* hostPermit =
      Find(connecting, "urnetwork-permit-dns-host-resolver");
  const WfpFilterSpec* dnsBlock = Find(connecting, "urnetwork-block-dns-v4");
  Check(hostPermit && dnsBlock && hostPermit->weight > dnsBlock->weight,
        "the host-resolver permit outweighs the port-53 block inside the DNS "
        "sublayer");

  bool addressesMatch = hostPermit != nullptr;
  if (addressesMatch) {
    // 192.168.1.1, and nothing else. One filter, addresses OR'd as consecutive
    // same-field conditions — the LAN permit's shape, so the filter count stays
    // deterministic however many resolvers the machine has.
    int addrCount = 0;
    for (const auto& c : hostPermit->conditions)
      if (c.field == WfpField::RemoteAddrV4) {
        ++addrCount;
        if (c.v4_addr != 0xC0A80101u || c.v4_prefix != 32) addressesMatch = false;
      }
    if (addrCount != 1) addressesMatch = false;
  }
  Check(addressesMatch,
        "the host-resolver permit carries exactly the configured resolver "
        "addresses as /32s, in one filter");

  WfpConfig twoResolvers = NoTunnelConfig(cfg);
  twoResolvers.host_resolvers_v4 = {"192.168.1.1", "1.1.1.1"};
  Check(CountName(BuildFilterSet(WfpState::Connecting, twoResolvers),
                  "urnetwork-permit-dns-host-resolver") == 1,
        "two HOST resolvers still produce ONE filter (consecutive same-field "
        "conditions are OR'd), so the connecting filter count is stable");

  // The tunnel-resolver permit is the OTHER shape, and it is the one that is
  // NOT stable: filter 10 emits one filter PER resolver. Asserted here because
  // p7-gates.ps1 used to hardcode the connected count at 43 on the assumption of
  // exactly one, which fails gate H2 on a two-resolver session for no real
  // reason. The gate now derives the number; this is where the derivation is
  // pinned to the code.
  WfpConfig twoTunnelResolvers = cfg;
  twoTunnelResolvers.tunnel_resolvers_v4 = {"169.254.2.53", "169.254.2.54"};
  const Specs connectedTwo = BuildFilterSet(WfpState::Connected, twoTunnelResolvers);
  Check(CountName(connectedTwo, "urnetwork-permit-dns-tunnel-resolver") == 2 &&
            connectedTwo.size() == connected.size() + 1,
        "two TUNNEL resolvers produce TWO filters, one more than one resolver — "
        "the connected filter count is a FUNCTION of the resolver count, not a "
        "constant",
        std::format("one={} two={}", connected.size(), connectedTwo.size()));

  // --- what THIS machine would get -----------------------------------------
  // Deliberately a note and not a Check: it reads the live adapter table, so
  // making it pass/fail would make the unit test depend on how the box happens
  // to be configured. It is printed because it is the number the elevated gate
  // cross-checks — the addresses inside urnetwork-permit-dns-host-resolver must
  // be exactly these, and Get-DnsClientServerAddress must agree with both. It
  // is also the only part of this file that runs the discovery code at all;
  // everything above tests the policy, not the reading of the machine.
  const std::vector<std::string> live =
      NetworkConfig::HostResolversV4(NET_LUID{});
  std::string liveText;
  for (const auto& s : live) {
    if (!liveText.empty()) liveText += ", ";
    liveText += s;
  }
  std::printf("  note  this machine's IPv4 resolvers (what the armed policy "
              "would permit): [%s]\n",
              live.empty() ? "none — the port-53 block would stand down"
                           : liveText.c_str());
}

// --- part 2c: the DNS-window disclosure is keyed on STRUCTURE ----------------
//
// WfpPolicy::Apply logs two facts at every transition: whether the machine-wide
// plaintext-DNS permit is installed, and whether the port-53 hard block is in
// force. Those two log lines are the ONLY evidence outside the filter engine
// that the DNS window was open, and they used to be decided by comparing
// spec.name against the literals "urnetwork-permit-dns-host-resolver" and
// "urnetwork-block-dns-v4".
//
// Comparing the NAME reports on the name. Rename filter 9b and the disclosure
// goes quiet with nothing failing; add a SECOND filter of the same shape under
// any other name and the window opens with the log still saying it is closed.
// Both are silent failures of the one mechanism that exists to make a silent
// failure impossible.
//
// So the predicates ask what the filter DOES (WfpPolicy.h), and this section
// pins them: first against every filter whose shape is nearby and must NOT be
// confused with them, then — the assertion that makes the change provably
// behaviour-preserving — as an EQUIVALENCE with the names they replaced, over
// every state and every config shape.

void TestDnsDisclosureShape() {
  Section("the DNS-window disclosure keys off filter STRUCTURE, not filter names");

  const WfpConfig cfg = SampleConfig(0x1234'5678'9ABC'DEF0ull);
  const Specs connecting = BuildFilterSet(WfpState::Connecting, NoTunnelConfig(cfg));
  const Specs connected = BuildFilterSet(WfpState::Connected, cfg);

  const WfpFilterSpec* hostPermit =
      Find(connecting, "urnetwork-permit-dns-host-resolver");
  Check(hostPermit && IsMachineWideDnsPermit(*hostPermit),
        "filter 9b IS a machine-wide DNS permit: port 53, matched by ADDRESS, "
        "narrowed by no app id, no loopback flag and no interface — which is "
        "exactly why it cannot be scoped to us");

  // The near miss that matters most. Filter 10 is the SAME shape plus an
  // interface condition, and calling it machine-wide would make Apply warn
  // "DNS WINDOW OPEN" on every healthy connected session — crying leak about the
  // state that has none, which is how a real disclosure stops being read.
  const WfpFilterSpec* tunnelPermit =
      Find(connected, "urnetwork-permit-dns-tunnel-resolver");
  Check(tunnelPermit && !IsMachineWideDnsPermit(*tunnelPermit),
        "filter 10 is NOT machine-wide — same port and address shape, but pinned "
        "to the tun's LUID, so it permits nothing off the tunnel");

  const WfpFilterSpec* appIdPermit =
      Find(connecting, "urnetwork-permit-service-dns-v4");
  Check(appIdPermit && !IsMachineWideDnsPermit(*appIdPermit),
        "filter 9 is NOT machine-wide — it is scoped to one app id");

  const WfpFilterSpec* loopbackPermit =
      Find(connecting, "urnetwork-permit-dns-loopback-v4");
  Check(loopbackPermit && !IsMachineWideDnsPermit(*loopbackPermit),
        "filter 11 is NOT machine-wide — it is scoped to the loopback flag");

  const WfpFilterSpec* block = Find(connecting, "urnetwork-block-dns-v4");
  const WfpFilterSpec* blockV6 = Find(connecting, "urnetwork-block-dns-v6");
  Check(block && !IsMachineWideDnsPermit(*block),
        "a BLOCK is never a permit, however its conditions read");
  Check(block && blockV6 && IsDnsPort53Block(*block) && IsDnsPort53Block(*blockV6),
        "filter 12 is recognised as the port-53 hard block at BOTH ALE connect "
        "layers");

  // Same sublayer, same action, different ports. Counting these as the port-53
  // block would report the block in force in a state that stood it down — the
  // disclosure inverted, which is worse than absent.
  bool nameBlocksExcluded = true;
  std::string nameBlockDetail;
  for (const char* n : {"urnetwork-block-llmnr-v4", "urnetwork-block-mdns-v4",
                        "urnetwork-block-netbios-v4"}) {
    const WfpFilterSpec* f = Find(connecting, n);
    if (!f || IsDnsPort53Block(*f)) {
      nameBlocksExcluded = false;
      nameBlockDetail = n;
    }
  }
  Check(nameBlocksExcluded,
        "the LLMNR / mDNS / NetBIOS blocks are NOT counted as the port-53 block "
        "— same sublayer and same action, different ports",
        nameBlockDetail);

  // The baseline lift is a port-53 permit too, and it is emitted in EVERY state.
  // Counting it would make Apply report the DNS window permanently open.
  const WfpFilterSpec* lift = Find(connecting, "urnetwork-lift-dns-v4");
  Check(lift && !IsMachineWideDnsPermit(*lift) && !IsDnsPort53Block(*lift),
        "filter 7's baseline lift is neither — it is a port-53 permit in the "
        "BASELINE sublayer, and both predicates are scoped to the DNS sublayer "
        "where the decision actually lives");

  // --- THE EQUIVALENCE -----------------------------------------------------
  // Over every state and every config shape the service can produce: structure
  // and the replaced names answer identically, and at most one machine-wide
  // permit is ever emitted (filter 9b is ONE filter with the addresses OR'd, so
  // "how many" is not a function of how many resolvers the host has).
  WfpConfig noHost = NoTunnelConfig(cfg);
  noHost.host_resolvers_v4.clear();
  WfpConfig junkHost = NoTunnelConfig(cfg);
  junkHost.host_resolvers_v4 = {"not-an-address", ""};
  WfpConfig manyHost = NoTunnelConfig(cfg);
  manyHost.host_resolvers_v4 = {"192.168.1.1", "1.1.1.1", "9.9.9.9"};
  WfpConfig noTunnelResolver = cfg;
  noTunnelResolver.tunnel_resolvers_v4.clear();
  WfpConfig noLuid = cfg;
  noLuid.tun_luid = 0;
  WfpConfig noService = NoTunnelConfig(cfg);
  noService.service_image_path.clear();

  struct Case { const char* label; WfpConfig cfg; };
  const Case cases[] = {
      {"shipped, no tun", NoTunnelConfig(cfg)},
      {"shipped connected", cfg},
      {"no host resolver", noHost},
      {"unparseable host resolver", junkHost},
      {"three host resolvers", manyHost},
      {"connected, no tunnel resolver", noTunnelResolver},
      {"connected, no tun luid", noLuid},
      {"no service image path", noService},
  };

  bool agrees = true;
  bool atMostOne = true;
  bool onlyConnecting = true;
  std::string disagreement, tooMany, wrongState;
  for (const auto& c : cases) {
    for (WfpState s : {WfpState::Off, WfpState::Armed, WfpState::Connecting,
                       WfpState::Connected}) {
      const Specs set = BuildFilterSet(s, c.cfg);
      int permits = 0, blocks = 0;
      for (const auto& f : set) {
        if (IsMachineWideDnsPermit(f)) ++permits;
        if (IsDnsPort53Block(f)) ++blocks;
      }
      if ((permits > 0) != HasName(set, "urnetwork-permit-dns-host-resolver") ||
          (blocks > 0) != HasName(set, "urnetwork-block-dns-v4")) {
        agrees = false;
        disagreement = std::format("{} / {}", c.label, ToString(s));
      }
      if (permits > 1) {
        atMostOne = false;
        tooMany = std::format("{} / {}: {}", c.label, ToString(s), permits);
      }
      if (permits > 0 && s != WfpState::Connecting) {
        onlyConnecting = false;
        wrongState = std::format("{} / {}", c.label, ToString(s));
      }
    }
  }
  Check(agrees,
        "structure and the filter NAMES it replaced answer identically for every "
        "state and every config shape — the disclosure Apply() logs did not "
        "change meaning, it only stopped depending on a string",
        disagreement);
  Check(atMostOne,
        "AT MOST ONE machine-wide DNS permit is ever emitted, however many "
        "resolvers the host has (9b OR's the addresses inside one filter)",
        tooMany);
  Check(onlyConnecting,
        "a machine-wide DNS permit appears in CONNECTING and nowhere else — if "
        "one ever appears in Armed or Connected, the log line that discloses it "
        "now fires there too instead of naming one filter that moved",
        wrongState);
}

// --- part 2d: the resolver-cache flush is reachable on THIS machine ----------

void TestResolverCacheFlush() {
  Section("the OS resolver-cache flush at the Connected / disconnected edges");

  // RESOLVES the entry point; does NOT flush. DnsFlushResolverCache is exported
  // by dnsapi.dll and declared in no SDK header, so nothing at build time can
  // tell us it is there — "it links" proves nothing, because it is not linked.
  // Without it, every name the machine resolved during the connecting window
  // (through the HOST's resolvers, in the clear — filter 9b) keeps being served
  // from the machine-wide cache for the rest of its TTL after the tunnel is up,
  // and every name resolved THROUGH the tunnel keeps being served after it is
  // gone. TunnelController flushes at both edges; this is the check that the
  // flush can actually happen here rather than logging a warning forever.
  Check(NetworkConfig::ResolverCacheFlushAvailable(),
        "dnsapi.dll exports DnsFlushResolverCache, so the flush at both tunnel "
        "edges is a real operation on this machine and not a logged no-op",
        "without it the machine-wide DNS cache outlives every transition: host "
        "answers survive into the connected session, tunnel answers survive out "
        "of it");
}

// --- part 3: the persistent path is gated off -------------------------------

void TestPersistentIsGatedOff() {
  Section("armed-across-reboot — present, reviewable, and NOT installed");

  const Specs persistent = BuildPersistentFilterSet();
  Check(persistent.size() == 8,
        "the persistent set is 8 filters (block-all + loopback permit at each "
        "of the four ALE layers)",
        std::format("got {}", persistent.size()));

  bool allPersistentSublayer = true;
  for (const auto& f : persistent)
    if (f.sublayer != WfpSublayer::Persistent) allPersistentSublayer = false;
  Check(allPersistentSublayer,
        "every persistent filter is in its OWN sublayer (a persistent object "
        "may only reference persistent objects owned by the same provider)");

  bool hasLoopbackPermit = false;
  for (const auto& f : persistent)
    if (!f.block) hasLoopbackPermit = true;
  Check(hasLoopbackPermit,
        "the persistent set permits loopback — Mullvad's does not, and "
        "blocking loopback between boot and the daemon starting breaks local "
        "databases and IPC in ways that look like our bug");

  // The gate itself: no live policy may ever emit a Persistent-sublayer filter,
  // so no code path from Apply() can install one.
  const WfpConfig cfg = SampleConfig(1);
  bool leaked = false;
  for (WfpState s : {WfpState::Off, WfpState::Armed, WfpState::Connecting,
                     WfpState::Connected}) {
    for (const auto& f : BuildFilterSet(s, cfg))
      if (f.sublayer == WfpSublayer::Persistent) leaked = true;
  }
  Check(!leaked,
        "no live state emits a persistent filter — the boot-time serviceName "
        "gate is UNVERIFIED, and a persistent block orphaned by a crash is "
        "worse than the leak it prevents");
}

// --- part 3b: the console flags, and what a typo must NOT become -------------
//
// `--stop-after=<N>` is the staged bring-up flag, and it is the only reason a
// wintun adapter can be created without the sequence carrying on into step 6/8
// — the first call that rewrites this machine's routes and DNS. Two properties
// are asserted here because both fail SILENTLY if they break:
//
//   1. A malformed or out-of-range value is REFUSED. The failure mode this
//      prevents is not a confusing error message, it is a full tunnel bring-up:
//      an operator who types --stop-after=O (letter O) or --stop-after=0 and
//      gets "no stop" runs all eight steps on a machine they were staging.
//      Every rejection below is therefore asserted as ok=false, and the matrix
//      at the end asserts that NO rejected input can produce a run at all.
//
//   2. It cannot widen --rpc-only. That clamp is one-way and holds for the life
//      of the process; a debug flag that could raise its ceiling would re-enable
//      the destructive half of a process launched specifically so it could not
//      reach it. EffectiveStopStep is where the two compose, and it composes
//      them as a MINIMUM.
//
// Everything here is a pure function over an argument vector. Nothing is
// started, and the flag it parses cannot be exercised any other way without
// running the service it configures.

void TestConsoleArgs() {
  Section("console options — --stop-after parsing, clamping, and --rpc-only");

  // --- absent: byte-identical to every invocation that works today ---------
  {
    const ConsoleArgs none = ParseConsoleArgs({});
    Check(none.ok && !none.rpc_only && none.stop_after == 0,
          "`console` with no options parses as a normal full run — the flag is "
          "inert unless it is explicitly passed");
    const ConsoleArgs rpc = ParseConsoleArgs({L"--rpc-only"});
    Check(rpc.ok && rpc.rpc_only && rpc.stop_after == 0,
          "`console --rpc-only` is unchanged: rpc-only, no stop point");
    const ConsoleArgs rpcTwice = ParseConsoleArgs({L"--rpc-only", L"--rpc-only"});
    Check(rpcTwice.ok && rpcTwice.rpc_only,
          "a repeated --rpc-only is still accepted — it is accepted today, and "
          "this change must not break an invocation that works");
    const ConsoleArgs bad = ParseConsoleArgs({L"--xyz"});
    const ConsoleArgs badAfterRpc = ParseConsoleArgs({L"--rpc-only", L"--xyz"});
    Check(!bad.ok && !badAfterRpc.ok,
          "an unknown option is still fatal in EVERY position, including after "
          "--rpc-only (the 'check every extra argument' rule)");
  }

  // --- valid 1..8 -----------------------------------------------------------
  {
    bool allParse = true;
    std::string firstBad;
    for (int n = kStopAfterMinStep; n <= kStopAfterMaxStep; ++n) {
      const ConsoleArgs a =
          ParseConsoleArgs({L"--stop-after=" + std::to_wstring(n)});
      if (!a.ok || a.stop_after != n || a.rpc_only) {
        allParse = false;
        if (firstBad.empty()) firstBad = std::format("N={}", n);
      }
    }
    Check(allParse,
          "every step 1..8 parses to exactly that step, and none of them turns "
          "rpc-only on (the flag narrows the sequence; it does not change mode)",
          firstBad);
  }

  // --- rejected: out of range, malformed, empty, repeated ------------------
  {
    struct Case { const wchar_t* arg; const char* label; };
    const Case rejected[] = {
        {L"--stop-after=0", "0 — there is no step 0, and it must not read as "
                            "'no stop'"},
        {L"--stop-after=9", "9 — the sequence has eight steps"},
        {L"--stop-after=99", "99 — two digits, still out of range"},
        {L"--stop-after=100", "100 — longer than any step number"},
        {L"--stop-after=-1", "-1 — a sign is not a digit"},
        {L"--stop-after=+1", "+1 — a sign is not a digit"},
        {L"--stop-after=1x", "1x — a trailing character is not ignored"},
        {L"--stop-after=x", "x — not a number at all"},
        {L"--stop-after=O", "O — the letter, the typo that would otherwise "
                            "become a full bring-up"},
        {L"--stop-after= 1", "' 1' — leading whitespace"},
        {L"--stop-after=1.0", "1.0 — not an integer"},
        {L"--stop-after=01", "01 — no quiet normalising"},
        {L"--stop-after=", "an empty value"},
        {L"--stop-after", "the flag with no value at all"},
        {L"--stop-after-1", "--stop-after-1 — a hyphen where the = should be"},
    };
    bool allRejected = true;
    std::string leaked;
    for (const auto& c : rejected) {
      const ConsoleArgs a = ParseConsoleArgs({c.arg});
      if (a.ok || a.stop_after != 0 || a.error.empty()) {
        allRejected = false;
        leaked = c.label;
      }
    }
    Check(allRejected,
          std::format("all {} malformed / out-of-range spellings are REFUSED "
                      "with a reason, and none of them yields a parsed run",
                      std::size(rejected)),
          leaked);

    const ConsoleArgs twice =
        ParseConsoleArgs({L"--stop-after=1", L"--stop-after=6"});
    Check(!twice.ok,
          "--stop-after given twice is REFUSED — it carries a value, so a "
          "repeat has two readings and one of them rewrites the route table");
    const ConsoleArgs twiceSame =
        ParseConsoleArgs({L"--stop-after=1", L"--stop-after=1"});
    Check(!twiceSame.ok,
          "even a repeat with the SAME value is refused; 'they agreed this "
          "time' is not a rule anyone can rely on");

    // The property, not the enumeration: a refused parse can never be read as
    // an instruction to run. Whatever else is wrong with it, ok=false is what
    // wmain checks, and stop_after stays 0 only because nothing may act on a
    // rejected result at all.
    bool refusalIsTotal = true;
    for (const auto& c : rejected) {
      const ConsoleArgs a = ParseConsoleArgs({L"--rpc-only", c.arg});
      if (a.ok) refusalIsTotal = false;
    }
    Check(refusalIsTotal,
          "a rejected --stop-after is still rejected when it follows a VALID "
          "option — a good flag next to a bad one does not rescue the command "
          "line");
  }

  // --- combined with --rpc-only, in both orders ----------------------------
  {
    const ConsoleArgs a = ParseConsoleArgs({L"--rpc-only", L"--stop-after=3"});
    const ConsoleArgs b = ParseConsoleArgs({L"--stop-after=3", L"--rpc-only"});
    Check(a.ok && a.rpc_only && a.stop_after == 3 && b.ok && b.rpc_only &&
              b.stop_after == 3,
          "--rpc-only and --stop-after combine in either order, and neither "
          "silently drops the other");
  }

  // --- THE CLAMP: out of range never means 'run everything' ----------------
  {
    Check(ClampStopAfterStep(0) == kStopAfterMinStep &&
              ClampStopAfterStep(-1) == kStopAfterMinStep &&
              ClampStopAfterStep(-1000) == kStopAfterMinStep,
          "a step below the range clamps to 1 — the EARLIEST stop. The one "
          "unacceptable reading of a nonsense value is 'run all eight steps'");
    Check(ClampStopAfterStep(9) == kStopAfterMaxStep &&
              ClampStopAfterStep(1000) == kStopAfterMaxStep,
          "a step above the range clamps to 8, not to 'no stop'");
    bool identity = true;
    for (int n = kStopAfterMinStep; n <= kStopAfterMaxStep; ++n)
      if (ClampStopAfterStep(n) != n) identity = false;
    Check(identity, "the clamp is the identity on 1..8 — it corrects nonsense "
                    "and touches nothing else");
  }

  // --- THE INVARIANT: --stop-after CANNOT WIDEN --rpc-only -----------------
  //
  // The one that matters. rpc-only ends the sequence at step 5 by returning at
  // the fence; the flag composes with that as a MINIMUM. If this ever fails, a
  // process launched precisely so that it could not reach step 6 has been handed
  // a command-line switch that lets it.
  {
    bool neverWidens = true;
    std::string widened;
    for (int n = -3; n <= 12; ++n) {
      if (EffectiveStopStep(n, /*rpcOnly=*/true) > kRpcOnlyCeilingStep) {
        neverWidens = false;
        widened = std::format("--stop-after={} reached step {}", n,
                              EffectiveStopStep(n, true));
      }
    }
    Check(neverWidens,
          std::format("under --rpc-only, NO value of --stop-after (including "
                      "out-of-range ones) lets the sequence past step {}/8 — "
                      "the clamp wins where they overlap",
                      kRpcOnlyCeilingStep),
          widened);

    Check(EffectiveStopStep(7, true) == 5 && EffectiveStopStep(8, true) == 5,
          "--rpc-only --stop-after=7 stops at 5, not 7: the flag asked for more "
          "than the clamp allows and got the clamp");
    Check(EffectiveStopStep(3, true) == 3 && EffectiveStopStep(3, false) == 3,
          "below the clamp's ceiling the flag still narrows, in both modes — "
          "rpc-only winning the overlap does not mean it wins everywhere");

    // Never widens the NORMAL run either: the flag can only ever move the last
    // step the sequence reaches earlier than it would otherwise have gone.
    bool monotone = true;
    std::string grew;
    for (int n = -3; n <= 12; ++n) {
      for (bool rpcOnly : {false, true}) {
        const int with = EffectiveStopStep(n, rpcOnly);
        const int without = EffectiveStopStep(0, rpcOnly);
        if (with > without) {
          monotone = false;
          grew = std::format("--stop-after={} rpc_only={} -> {} > {}", n,
                             rpcOnly, with, without);
        }
      }
    }
    Check(monotone,
          "for EVERY value and both modes, passing --stop-after reaches a step "
          "no later than not passing it — the flag is monotonically narrowing, "
          "which is the whole safety claim",
          grew);
  }
}

// --- part 4: the SDK's resolver against THE table ---------------------------

void TestSdkResolverAgainstTable() {
  Section("cross-check — the SDK's default tunnel resolver reaches the tun");

  std::string addr;
  try {
    addr = urnet::getDefaultTunnelDnsAddressIpv4();
  } catch (const std::exception& e) {
    std::printf("  note  could not read the SDK's default tunnel resolver: %s\n",
                e.what());
    return;
  }
  if (addr.empty()) {
    std::printf("  note  the SDK reported no default tunnel resolver\n");
    return;
  }

  unsigned a = 0, b = 0, c = 0, d = 0;
  if (::sscanf_s(addr.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) != 4) {
    std::printf("  note  default tunnel resolver '%s' is not dotted-quad\n",
                addr.c_str());
    return;
  }
  const uint32_t host = (a << 24) | (b << 16) | (c << 8) | d;
  Check(!net::IsLocalBypassV4(host),
        std::format("the SDK's default tunnel resolver {} is CAPTURED by the "
                    "tun, not bypassed to the physical NIC",
                    addr),
        "a resolver inside kLocalBypassV4 is unreachable through the tun, and "
        "with the port-53 block in force that is a total DNS outage, not a leak");
}

// --- the shutdown budget ---------------------------------------------------
//
// The bug these exist for cannot be reproduced on this machine: reaching the UP
// state needs elevation, a wintun adapter and a live session, and even then the
// hang needs the SDK to be genuinely wedged. So the WEDGE IS CONSTRUCTED HERE
// instead — a teardown that really does not return — and the properties that
// matter are proved against the real RunBounded and the real escalation ladder,
// not against a copy of them.
//
// What this can prove: that a stop with an unreturning teardown still returns,
// within budget; that abandoning it does not free what the wedged thread is
// still using; that a second Ctrl+C escalates rather than queueing; and that an
// escalation shortens a wait ALREADY IN PROGRESS. What it cannot prove is that
// DeviceLocal::close() is the thing that wedges in the field — see the live
// procedure in the report.

// Stands in for the objects an abandoned teardown keeps hold of (the
// DeviceLocal, the wintun adapter, the pump). Its destructor is the observable:
// if it runs while a wedged thread is still executing, that is the
// use-after-free this design exists to prevent.
struct WedgeState {
  std::atomic<bool> entered{false};
  std::atomic<bool> release{false};
  std::atomic<bool> ownedDestroyed{false};
};

struct OwnedByTeardown {
  std::shared_ptr<WedgeState> state;
  OwnedByTeardown(std::shared_ptr<WedgeState> s) : state(std::move(s)) {}
  OwnedByTeardown(OwnedByTeardown&&) = default;
  OwnedByTeardown& operator=(OwnedByTeardown&&) = default;
  ~OwnedByTeardown() {
    if (state) state->ownedDestroyed.store(true);
  }
};

int64_t MillisSince(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now() - start)
      .count();
}

void TestStopBudget() {
  Section("the shutdown budget — a teardown that never returns must not be able "
          "to hold the machine");

  ResetStopBudgetForTest();

  // --- the budgets are internally consistent -------------------------------
  //
  // These are the relationships the numbers have to satisfy to mean anything.
  // A forced budget that is not smaller than the normal one makes escalation a
  // no-op; a force grace shorter than the forced budget makes escalation a kill
  // that never lets the collapsed teardown report; and the whole stop has to fit
  // inside the 5000ms wait hint this service gives the SCM.
  Check(kForcedTeardownBudget < kSdkTeardownBudget,
        "escalating actually shortens the sdk teardown budget",
        std::format("forced {}ms vs normal {}ms", kForcedTeardownBudget.count(),
                    kSdkTeardownBudget.count()));
  Check(kConsoleForceGrace > kForcedTeardownBudget,
        "the console's post-escalation grace outlasts the collapsed budget, so a "
        "forced teardown can finish and report instead of always being killed",
        std::format("grace {}ms vs forced budget {}ms",
                    kConsoleForceGrace.count(), kForcedTeardownBudget.count()));
  Check(kStopLockBudget + kSdkTeardownBudget < std::chrono::milliseconds{5000},
        "worst-case Stop() (lock budget + sdk budget) fits inside the 5000ms "
        "STOP_PENDING wait hint given to the SCM",
        std::format("{}ms + {}ms", kStopLockBudget.count(),
                    kSdkTeardownBudget.count()));

  // --- the escalation ladder ------------------------------------------------
  Check(DecideConsoleStop(1) == ConsoleStopAction::Graceful,
        "first Ctrl+C runs the ORDINARY teardown");
  Check(DecideConsoleStop(2) == ConsoleStopAction::Force,
        "second Ctrl+C ESCALATES — it does not queue a second graceful attempt");
  Check(DecideConsoleStop(3) == ConsoleStopAction::Terminate &&
            DecideConsoleStop(16) == ConsoleStopAction::Terminate,
        "third and every later Ctrl+C terminates; sixteen presses cannot all "
        "mean the same thing again");
  Check(DecideConsoleStop(0) == ConsoleStopAction::Graceful &&
            DecideConsoleStop(-1) == ConsoleStopAction::Graceful,
        "an impossible press count resolves to the one action that is never "
        "destructive, not to a kill");

  // --- THE WEDGE: a teardown that will not return ---------------------------
  //
  // This is the live failure in miniature: work that has entered and will not
  // come back until something outside it says so, exactly like a DeviceLocal
  // close retrying transports that are all down.
  {
    constexpr std::chrono::milliseconds kTestBudget{150};
    auto state = std::make_shared<WedgeState>();
    const auto start = std::chrono::steady_clock::now();
    const bool finished =
        RunBounded(kTestBudget, [s = state, owned = OwnedByTeardown(state)]() mutable {
          s->entered.store(true);
          while (!s->release.load())
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        });
    const int64_t elapsed = MillisSince(start);

    Check(!finished,
          "a teardown that never returns is reported as UNFINISHED rather than "
          "waited on");
    Check(elapsed >= kTestBudget.count() && elapsed < kTestBudget.count() + 1000,
          "the bounded stop RETURNED, and did so at its budget",
          std::format("{}ms against a {}ms budget", elapsed,
                      kTestBudget.count()));
    Check(TeardownAbandoned(),
          "abandoning latches TeardownAbandoned(), which is what makes the "
          "process refuse a restart and exit by TerminateProcess");
    Check(state->entered.load() && !state->ownedDestroyed.load(),
          "THE SAFETY PROPERTY: the abandoned worker is still running and what "
          "it owns is STILL ALIVE — nothing was freed under a thread that is "
          "still using it");

    // Let the wedge go and prove the worker cleans up after itself. This is why
    // abandonment is a leak-at-worst rather than a crash: the objects belong to
    // the worker, so they go when it does.
    state->release.store(true);
    for (int i = 0; i < 2000 && !state->ownedDestroyed.load(); ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
    Check(state->ownedDestroyed.load(),
          "once unwedged, the abandoned worker destroys what it owned — the "
          "objects belong to it, so nothing else has to clean up");
    ResetStopBudgetForTest();
  }

  // --- a teardown that DOES finish is not penalised -------------------------
  {
    auto state = std::make_shared<WedgeState>();
    const auto start = std::chrono::steady_clock::now();
    const bool finished = RunBounded(
        std::chrono::milliseconds{5000},
        [s = state, owned = OwnedByTeardown(state)]() mutable {
          s->entered.store(true);
        });
    const int64_t elapsed = MillisSince(start);
    Check(finished && elapsed < 1000,
          "a teardown that completes returns immediately, not at the budget",
          std::format("finished={} in {}ms", finished, elapsed));
    Check(state->ownedDestroyed.load(),
          "when it reports finished, everything it owned is ALREADY destroyed — "
          "'done' means done, not 'about to be'");
    Check(!TeardownAbandoned(),
          "a teardown that completed does not latch the abandoned flag");
    ResetStopBudgetForTest();
  }

  // --- escalation shortens a wait that is ALREADY RUNNING -------------------
  //
  // The case that matters, because the second Ctrl+C by definition arrives while
  // the first stop is still stuck. A budget that could only be chosen up front
  // would make the second press cosmetic — which is precisely what the old
  // handler was.
  {
    auto state = std::make_shared<WedgeState>();
    const auto start = std::chrono::steady_clock::now();
    std::thread escalate([] {
      std::this_thread::sleep_for(std::chrono::milliseconds{50});
      RequestForcedStop();  // the operator's second Ctrl+C
    });
    const bool finished =
        RunBounded(std::chrono::milliseconds{10000},
                   [s = state, owned = OwnedByTeardown(state)]() mutable {
                     s->entered.store(true);
                     while (!s->release.load())
                       std::this_thread::sleep_for(std::chrono::milliseconds{1});
                   });
    const int64_t elapsed = MillisSince(start);
    escalate.join();
    Check(!finished && elapsed < 3000,
          "a second Ctrl+C COLLAPSES a ten-second wait that had already begun, "
          "instead of leaving the operator to serve out the original budget",
          std::format("returned in {}ms with a 10000ms budget already running",
                      elapsed));
    state->release.store(true);
    for (int i = 0; i < 2000 && !state->ownedDestroyed.load(); ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
    ResetStopBudgetForTest();
  }
}

// --- the egress index on the wire -------------------------------------------
//
// TunnelStatus::egress_index4 is what lets the APP perform R1 self-exclusion on
// its own SDK instance, and the whole fix is dead if the field does not survive
// the JSON round trip — silently, with no symptom other than the UI still losing
// the platform whenever the tunnel is up. That is exactly the class of failure a
// cheap round-trip test exists for: from_json uses find/get_to and tolerates an
// absent key, so a typo in the to_json name would produce a status that parses
// cleanly and always reads 0.
void TestStatusEgressWire() {
  Section("TunnelStatus — the egress index survives the wire (app R1)");

  proto::TunnelStatus s;
  s.state = proto::TunnelState::Up;
  s.mode = proto::StartMode::Tunnel;
  s.routes_installed = true;
  s.egress_index4 = 17;
  s.egress_index6 = 23;

  const nlohmann::json j = s;
  Check(j.contains("egress_index4") && j.contains("egress_index6"),
        "the status carries both egress index fields on the wire");

  proto::TunnelStatus back = j.get<proto::TunnelStatus>();
  Check(back.egress_index4 == 17 && back.egress_index6 == 23,
        "both survive the round trip",
        std::format("got v4={} v6={}", back.egress_index4, back.egress_index6));

  // A peer too old to send them. `from_json` must leave the defaults rather than
  // throw, and the default must be 0 — which the app reads as "do not bind",
  // i.e. exactly the behaviour that existed before this field. Absent means
  // unchanged, and unchanged is the safe direction.
  nlohmann::json old = j;
  old.erase("egress_index4");
  old.erase("egress_index6");
  proto::TunnelStatus legacy = old.get<proto::TunnelStatus>();
  Check(legacy.egress_index4 == 0 && legacy.egress_index6 == 0 &&
            legacy.routes_installed,
        "a status from a peer that does not send them parses fine and reads 0 — "
        "no protocol bump was needed, and the app treats 0 as 'do not bind'");
}

// --- URNETWORK_SDK_TRACE ----------------------------------------------------
//
// Tested here for ParseConsoleArgs' reason: this flag decides whether a live
// session gets sampled every N milliseconds and whether provider client ids are
// written into the service log, and both of those have to be checkable without
// starting a session.
//
// The direction that matters is the one a diagnostic flag gets wrong: a value
// nobody understood must end up OFF, never on-with-a-guess. Every rejection
// below is asserted to be off AND to carry an error, because "off and silent"
// would leave an operator watching for a trace that is never coming.
void TestWindowTraceFlag() {
  Section("URNETWORK_SDK_TRACE — the window trace flag (no device started)");

  for (const char* off : {"", " ", "0", "off", "OFF", "false", "No", "\tno\r\n"}) {
    const WindowTraceConfig c = ParseWindowTrace(off);
    Check(!c.enabled && c.error.empty(),
          std::format("'{}' turns the trace off, and that is not an error", off));
  }

  for (const char* on : {"1", "on", "ON", "true", "Yes", " true "}) {
    const WindowTraceConfig c = ParseWindowTrace(on);
    Check(c.enabled && c.error.empty() &&
              c.interval == std::chrono::milliseconds(kWindowTraceDefaultIntervalMs),
          std::format("'{}' turns it on at the default {}ms period", on,
                      kWindowTraceDefaultIntervalMs));
  }

  {
    const WindowTraceConfig c = ParseWindowTrace("250");
    Check(c.enabled && c.error.empty() &&
              c.interval == std::chrono::milliseconds(250),
          "a plain number is taken as the sampling period in milliseconds");
  }
  {
    const WindowTraceConfig c = ParseWindowTrace(
        std::to_string(kWindowTraceMinIntervalMs));
    Check(c.enabled && c.interval == std::chrono::milliseconds(kWindowTraceMinIntervalMs),
          "the minimum period is accepted (the bound is inclusive)");
  }
  {
    const WindowTraceConfig c = ParseWindowTrace(
        std::to_string(kWindowTraceMaxIntervalMs));
    Check(c.enabled && c.interval == std::chrono::milliseconds(kWindowTraceMaxIntervalMs),
          "the maximum period is accepted (the bound is inclusive)");
  }

  // Below the floor this becomes a load generator on the session it is
  // measuring; above the ceiling a window can form and collapse between two
  // samples. Both are refusals, not clamps: a clamp would silently trace at a
  // period the operator did not ask for and then be blamed for what it missed.
  for (const char* bad : {"1", "19", "10001", "999999", "0.5", "100ms", "fast",
                          "-100", "+100", " 1 0 0 "}) {
    const std::string v = bad;
    if (v == "1") continue;  // "1" is the switch form, asserted above
    const WindowTraceConfig c = ParseWindowTrace(v);
    Check(!c.enabled && !c.error.empty(),
          std::format("'{}' is REFUSED and the trace stays off, with a reason",
                      v));
  }

  // The one that would be worst to get wrong: a five-digit value that overflows
  // into range if the length is not checked first.
  {
    const WindowTraceConfig c = ParseWindowTrace("100000");
    Check(!c.enabled && !c.error.empty(),
          "a six-digit period is refused rather than truncated or wrapped into "
          "the accepted range");
  }
}

// --- the release tag grammar ------------------------------------------------

void TestVersionGrammar() {
  Section("VersionGrammar — release tags rank by code, or not at all");

  // The shapes CI actually mints: v<YYYY.M.D>-<code>[-beta]. The update
  // checker hands the parser the WHOLE tag — nothing is stripped first, so
  // the parser is the single opinion on what counts as a release.
  Check(version::ParseReleaseCode("v2026.8.9-101076420-beta") == 101076420ull,
        "a full beta tag yields its code");
  Check(version::ParseReleaseCode("v2026.8.9-101076420") == 101076420ull,
        "the -beta suffix is optional");
  Check(version::ParseReleaseCode("2026.8.9-101076420-beta") == 101076420ull,
        "the leading v is optional — Version.h stamps the v-less form");
  Check(version::ParseReleaseCode("v2026.12.31-1-beta") == 1ull,
        "two-digit month and day parse");
  Check(version::ParseReleaseCode("v2026.8.9-1010764200-beta") == 1010764200ull,
        "a ten-digit code — the real magnitude in 2026 — parses exactly");

  // 0 is the single no-match answer, and it is also what a dev build stamps
  // as its own code: "not a release tag" and "never newer than anything"
  // coincide by design.
  for (const char* bad :
       {"", "v", "0.0.0-dev", "v2026.8.9", "v2026.8.9-beta",
        "v2026.8.9-101076420-rc1", "v2026.8.9-101076420-beta2",
        "v2026.8.9-101076420-beta-beta", "v2026.8.9-101076420beta",
        "v2026.8-101076420", "v2026.8.9.1-101076420", "v26.8.9-101076420",
        "v2026.13.9-101076420", "v2026.8.32-101076420", "v2026.0.9-101076420",
        "v2026.8.0-101076420", "V2026.8.9-101076420", "v2026.8.9--101076420",
        "v2026.8.9-101076420 ", " v2026.8.9-101076420",
        "urnetwork-v2026.8.9-101076420"}) {
    Check(version::ParseReleaseCode(bad) == 0,
          std::format("'{}' is no release: code 0", bad));
  }

  // Overflow is a digit-COUNT gate, not arithmetic that happens to survive:
  // eighteen digits always fit uint64_t, a nineteenth is refused outright.
  // A wrapped code would be a plausible number that outranks every real
  // release forever — the worst possible failure for an update checker.
  Check(version::ParseReleaseCode("v2026.8.9-999999999999999999") ==
            999999999999999999ull,
        "an eighteen-digit code is the accepted maximum");
  Check(version::ParseReleaseCode("v2026.8.9-9999999999999999999") == 0,
        "a nineteen-digit code is refused, not wrapped");

  // The stamp this very binary carries must round-trip through the same
  // parser: a stamped build's kString parses back to kCode, and the dev
  // stamp parses to 0 — exactly why dev builds never self-update.
  Check(version::ParseReleaseCode(version::kString) == version::kCode,
        "the binary's own stamp round-trips: ParseReleaseCode(kString) == kCode",
        std::format("kString='{}' kCode={}", version::kString,
                    static_cast<unsigned long long>(version::kCode)));
}

// --- the update checker's parsers (Common/UpdateFormats.h) ------------------

void TestUpdateFormats() {
  Section("UpdateFormats — asset digest parsing and the swap allowlist");

  // The digest exactly as the releases API mints it on every asset:
  // `sha256:<64 lowercase hex>`. The parser's empty return means "this
  // release cannot be verified, skip it", so everything below the well-formed
  // case is a refusal shape, never a best-effort read.
  Check(update::DigestHexFromAssetDigest("sha256:" + std::string(64, '1')) ==
            std::string(64, '1'),
        "the minted shape parses to its bare hex");
  Check(update::DigestHexFromAssetDigest(
            "sha256:"
            "ABCDEF1111111111111111111111111111111111111111111111111111111111") ==
            "abcdef1111111111111111111111111111111111111111111111111111111111",
        "uppercase hex verifies, and comes back canonical lowercase");
  Check(update::DigestHexFromAssetDigest("sha512:" + std::string(64, '1'))
            .empty(),
        "another algorithm is not a SHA-256 — refused, not truncated to fit");
  Check(update::DigestHexFromAssetDigest("SHA256:" + std::string(64, '1'))
            .empty(),
        "the prefix is matched as minted — an uppercase prefix is refused");
  Check(update::DigestHexFromAssetDigest(std::string(64, '1')).empty(),
        "bare hex without the algorithm prefix is refused");
  Check(update::DigestHexFromAssetDigest("sha256:" + std::string(63, '1'))
            .empty(),
        "63 hex chars are not a SHA-256");
  Check(update::DigestHexFromAssetDigest("sha256:" + std::string(65, '1'))
            .empty(),
        "65 hex chars are not a SHA-256 either — length is exact");
  Check(update::DigestHexFromAssetDigest("sha256:" + std::string(64, 'z'))
            .empty(),
        "64 non-hex characters are not a hash");
  Check(update::DigestHexFromAssetDigest("").empty(),
        "an empty digest field yields empty, which equals no hash");

  // The allowlist: what a swap may touch is a bare top-level exe/dll/pri name
  // and NOTHING else. Archive entry paths are never trusted (zip-slip), so a
  // separator or a traversal in a name is an outright refusal.
  for (const char* good : {"URnetwork.exe", "urnetworkd.exe", "URnetworkSdk.dll",
                           "wintun.dll", "resources.pri", "URNETWORK.EXE"}) {
    Check(update::IsAllowedPayloadName(good),
          std::format("'{}' is swap payload", good));
  }
  for (const char* bad :
       {"", ".", "..", "README.txt", "THIRD-PARTY-NOTICES.txt", "x.pdb",
        ".exe", "Assets", "..\\evil.exe", "../evil.exe", "a/b.dll",
        "C:\\windows\\system32\\evil.dll", "URnetwork.exe.old"}) {
    Check(!update::IsAllowedPayloadName(bad),
          std::format("'{}' is NOT swap payload", bad));
  }

  // The stale-leftover matcher gates a DeleteFile in the user's own install
  // folder, so the shapes the swap MINTS match and everything else — however
  // close — does not. `report.old-2024.xlsx` is the reviewer-found data-loss
  // case: ".old" followed by '-' mid-name, in a folder the user unzipped
  // themselves.
  for (const char* stale :
       {"URnetwork.exe.old", "urnetworkd.exe.old", "URnetworkSdk.dll.old",
        "URnetwork.exe.old-101076420", "wintun.dll.old-1"}) {
    Check(update::IsStaleRenamedName(stale),
          std::format("'{}' is a swap leftover", stale));
  }
  for (const char* keep :
       {"", ".old", "report.old-2024.xlsx", "URnetwork.exe.old-backup",
        "notes.older", "x.old-", "URnetwork.exe", "gold", "a.oldx"}) {
    Check(!update::IsStaleRenamedName(keep),
          std::format("'{}' is NOT a swap leftover — never deleted", keep));
  }
}

void TestInstallVerb() {
  Section("install verb — binPath quoting and the start/stop verdicts (no SCM)");

  // These tests exist because the verb's real caller cannot read its output:
  // the app fires `urnetworkd install` through a UAC prompt, sees only the
  // exit code, and polls the SCM itself. The SCM calls need elevation and are
  // therefore out of selftest's reach forever — but the decisions (what to
  // store as binPath, what each observed outcome means, which exit code it
  // earns) are pure, and InstallVerb.h keeps them that way.

  // Quoting. The unquoted-service-path failure this prevents: an unquoted
  // `C:\Program Files\...` binPath makes the SCM try `C:\Program.exe` first.
  Check(install::QuoteServiceBinPath(L"C:\\Program Files\\URnetwork\\urnetworkd.exe") ==
            L"\"C:\\Program Files\\URnetwork\\urnetworkd.exe\"",
        "a path with spaces is wrapped in quotes");
  Check(install::QuoteServiceBinPath(L"C:\\u\\urnetworkd.exe") ==
            L"\"C:\\u\\urnetworkd.exe\"",
        "a path without spaces is quoted too — one output shape, no rule to "
        "remember");
  const std::wstring once =
      install::QuoteServiceBinPath(L"C:\\Program Files\\URnetwork\\urnetworkd.exe");
  Check(install::QuoteServiceBinPath(once) == once,
        "quoting is idempotent: an already-quoted path is not double-quoted");
  Check(install::QuoteServiceBinPath(L"").empty(),
        "an empty path stays empty — the caller refuses it before the SCM");

  // The start verdict, row by row. Exit 0 must mean RUNNING and nothing else.
  Check(install::JudgeStartWait(install::kStateRunning, false).exit_code == 0 &&
            install::JudgeStartWait(install::kStateRunning, false).error.empty(),
        "RUNNING is exit 0 with no error text");
  Check(install::JudgeStartWait(install::kStateRunning, true).exit_code == 0,
        "RUNNING ignores the pipe flag — the running service is what holds it");

  const install::StartVerdict console =
      install::JudgeStartWait(install::kStateStopped, /*pipeBusy=*/true);
  Check(console.exit_code != 0 &&
            console.error.find(L"console") != std::wstring::npos,
        "STOPPED with the pipe busy blames the console-mode urnetworkd by name");
  const install::StartVerdict stopped =
      install::JudgeStartWait(install::kStateStopped, /*pipeBusy=*/false);
  Check(stopped.exit_code != 0 &&
            stopped.error.find(L"log") != std::wstring::npos,
        "STOPPED with the pipe free points at the log, not at a console");
  Check(stopped.error.find(L"console") == std::wstring::npos,
        "…and does NOT mention a console it has no evidence for");

  const install::StartVerdict pending =
      install::JudgeStartWait(install::kStateStartPending, false);
  Check(pending.exit_code != 0 &&
            pending.error.find(L"did not reach RUNNING") != std::wstring::npos,
        "a start still pending at budget end is a failure that says so");
  Check(install::JudgeStartWait(install::kStateQueryFailed, false).exit_code != 0,
        "a failed status query is a failure, never folded into success");
  // A state number this code has never heard of must not fall through to
  // exit 0 — the verdict is total, and unknowns land in the timeout row.
  Check(install::JudgeStartWait(99, false).exit_code != 0,
        "an unknown state is refused, not defaulted to success");

  // The stop half of the idempotent path.
  Check(install::StopFailureText(install::kStateStopPending)
                .find(L"did not stop") != std::wstring::npos,
        "a stop that never lands says the service did not stop");
  Check(install::StopFailureText(install::kStateQueryFailed)
                .find(L"could not query") != std::wstring::npos,
        "a failed query during stop is reported as a query failure");
  // The uninstall verb shares the wait-for-STOPPED (DeleteService on an
  // unstopped service only marks it delete-pending); its flavor of the text
  // must tell the user to re-run UNINSTALL, not to install what they were
  // removing.
  Check(install::StopFailureText(install::kStateStopPending, L"uninstall")
                .find(L"`urnetworkd uninstall` again") != std::wstring::npos,
        "the uninstall flavor sends the user back to uninstall");
  Check(install::StopFailureText(install::kStateStopPending, L"uninstall")
                .starts_with(L"uninstall:"),
        "the uninstall flavor is prefixed with its own verb");

  // The budgets are the bound on a UAC elevation the user already approved:
  // they must exist (nonzero), be pollable (interval strictly inside), and
  // stay near the spec's ~10s so a miss means "wrong", not "slow".
  Check(install::kScmPollIntervalMs > 0 &&
            install::kScmPollIntervalMs < install::kStartWaitBudgetMs &&
            install::kScmPollIntervalMs < install::kStopWaitBudgetMs,
        "the poll interval fits inside both wait budgets");
  Check(install::kStartWaitBudgetMs >= 5000 &&
            install::kStartWaitBudgetMs <= 30000 &&
            install::kStopWaitBudgetMs >= 5000 &&
            install::kStopWaitBudgetMs <= 30000,
        "both budgets are bounded near the spec's ~10s — finite, not infinite");
}

// --- ConnectionHealth: the aggregate the status line / strip / tray render --
//
// Pure logic in Common (ConnectionHealth.h), pinned here for the VersionGrammar
// reason: the words the app says for a given set of facts are a contract with
// the user, and the two failure modes this table exists to kill — "Connected
// but nothing works" and "yellow forever" — were both silences of the OLD
// derivation. Every row below is a claim about what the UI says.
void TestConnectionHealth() {
  Section("ConnectionHealth — status derivation + degrade hysteresis");
  using health::Activity;
  using health::ActivityFromStatus;
  using health::CellOccupiesWindow;
  using health::CellProven;
  using health::Signals;
  using health::State;
  using health::ToString;
  using health::Tracker;

  constexpr int64_t kHold = Tracker::kDegradeHoldMillis;
  Check(5000 <= kHold && kHold <= 10000,
        "the degrade hold sits inside the 5-10s anti-flap band",
        std::format("got {}ms", kHold));

  auto sig = [](bool svc, Activity a, bool grid, int64_t window, int64_t proven) {
    Signals s;
    s.serviceConnected = svc;
    s.activity = a;
    s.gridKnown = grid;
    s.windowSize = window;
    s.provenCount = proven;
    return s;
  };

  // The SDK status fold: the four live values, this app's two clamp sentinels,
  // and garbage. An unknown status must never claim a connection.
  Check(ActivityFromStatus("CONNECTED") == Activity::Active,
        "CONNECTED folds to Active");
  Check(ActivityFromStatus("connected") == Activity::Active,
        "…case-insensitively, like android's ConnectStatus.fromString");
  Check(ActivityFromStatus("CONNECTING") == Activity::Connecting &&
            ActivityFromStatus("DESTINATION_SET") == Activity::Connecting,
        "CONNECTING and DESTINATION_SET are one transitional state");
  Check(ActivityFromStatus("DISCONNECTED") == Activity::Inactive,
        "DISCONNECTED is Inactive");
  Check(ActivityFromStatus("RPC_ONLY") == Activity::Inactive &&
            ActivityFromStatus("SERVICE_DOWN") == Activity::Inactive,
        "both ReadStats clamp sentinels read as Inactive");
  Check(ActivityFromStatus("") == Activity::Inactive &&
            ActivityFromStatus("BANANA") == Activity::Inactive,
        "empty/unknown statuses are Inactive, never a claimed connection");

  // Grid cell classification: "Added" is the app-side stand-in for the
  // reliability heartbeat's proven count; Removed cells claim nothing.
  Check(CellProven("Added") && !CellProven("InEvaluation") &&
            !CellProven("EvaluationFailed") && !CellProven("NotAdded") &&
            !CellProven("Removed"),
        "only Added counts as proven");
  Check(CellOccupiesWindow("Added") && CellOccupiesWindow("InEvaluation") &&
            CellOccupiesWindow("EvaluationFailed") && CellOccupiesWindow("NotAdded"),
        "live cells occupy the window whatever their verdict");
  Check(!CellOccupiesWindow("Removed") && !CellOccupiesWindow(""),
        "Removed/empty cells do not");

  // ---- the transition table, as one connect lifetime ----
  Tracker t;
  Check(t.Update(sig(false, Activity::Inactive, false, 0, 0), 0) == State::NoService,
        "pipe down is NoService — the signed-out launch can never render Connected");
  Check(t.Update(sig(true, Activity::Inactive, false, 0, 0), 1000) ==
            State::Disconnected,
        "service up, nothing driving: Disconnected (rpc-only lands here too)");
  Check(t.Update(sig(true, Activity::Connecting, true, 0, 0), 2000) ==
            State::Connecting,
        "a connect begins: Connecting");
  Check(t.Update(sig(true, Activity::Active, true, 0, 0), 3000) == State::Connecting,
        "CONNECTED with an empty window is still Connecting, not a green lie");
  Check(t.Update(sig(true, Activity::Active, true, 4, 0), 4000) == State::Evaluating,
        "window populated, nothing proven: Evaluating — the honest all-yellow");
  Check(t.Update(sig(true, Activity::Connecting, true, 4, 0), 4500) ==
            State::Evaluating,
        "…and a transitional SDK status does not un-say it");
  Check(t.Update(sig(true, Activity::Active, true, 4, 1), 5000) == State::Connected,
        "one proven provider is Connected");
  Check(t.ReevalAtMillis() == 0, "a healthy Connected schedules no re-eval");

  // A blip: proven drops and returns inside the hold. The status must not flap.
  Check(t.Update(sig(true, Activity::Active, true, 4, 0), 6000) == State::Connected,
        "proven drops to 0: Connected HOLDS through the blip window");
  Check(t.ReevalAtMillis() == 6000 + kHold,
        "…and the hold names the deadline the clock must re-ask at",
        std::format("got {}", t.ReevalAtMillis()));
  Check(t.Update(sig(true, Activity::Active, true, 4, 1), 6000 + kHold / 2) ==
            State::Connected,
        "proven back inside the hold: still Connected — no flap");
  Check(t.ReevalAtMillis() == 0, "recovery clears the pending deadline");

  // A real loss: the hold expires.
  Check(t.Update(sig(true, Activity::Active, true, 4, 0), 10000) == State::Connected,
        "loss again: the hold restarts from the new loss instant");
  Check(t.Update(sig(true, Activity::Active, true, 4, 0), 10000 + kHold - 1) ==
            State::Connected,
        "one millisecond inside the hold is still Connected");
  Check(t.Update(sig(true, Activity::Active, true, 4, 0), 10000 + kHold) ==
            State::Degraded,
        "the hold's own deadline is Degraded — the boundary the re-eval fires at");
  Check(t.Update(sig(true, Activity::Active, true, 0, 0), 20000) == State::Degraded,
        "…even if the whole window collapsed afterwards: was-connected wins over "
        "looks-like-connecting");
  Check(t.Update(sig(true, Activity::Active, true, 4, 2), 30000) == State::Connected,
        "recovery is IMMEDIATE — hysteresis only guards the bad direction");

  // A deliberate location change must read as a fresh attempt, not a loss
  // (the session worker calls NoteNewAttempt when it applies the intent).
  t.NoteNewAttempt();
  Check(t.Update(sig(true, Activity::Active, true, 4, 0), 31000) == State::Evaluating,
        "after NoteNewAttempt the rebuilding window is Evaluating, never Degraded");

  // Disconnect resets the attempt; a fresh connect starts clean.
  Check(t.Update(sig(true, Activity::Inactive, true, 0, 0), 32000) ==
            State::Disconnected,
        "disconnect lands in Disconnected");
  Check(t.Update(sig(true, Activity::Active, true, 3, 0), 33000) == State::Evaluating,
        "…and the next connect evaluates: the old attempt's proof is gone");

  // The service dying mid-session must not freeze a green light.
  Tracker t2;
  t2.Update(sig(true, Activity::Active, true, 4, 1), 0);
  Check(t2.Update(sig(false, Activity::Inactive, false, 0, 0), 1000) ==
            State::NoService,
        "pipe drop mid-Connected is NoService, not a frozen Connected");
  Check(t2.Update(sig(true, Activity::Active, true, 4, 0), 2000) == State::Evaluating,
        "…and the session after it starts unproven (attempt was reset)");

  // The hidden-window rules: no grid feed means no fresh evidence, so an
  // evidence-based claim HOLDS rather than sharpening from an empty snapshot.
  Tracker t3;
  t3.Update(sig(true, Activity::Active, true, 4, 0), 0);  // Evaluating
  Check(t3.Update(sig(true, Activity::Connecting, false, 0, 0), 1000) ==
            State::Evaluating,
        "window hidden (no grid feed): the last evidence-based claim holds");

  // ...and with no evidence at ALL, the tunnel's own claim stands (the tray
  // over a reattached session whose window was never opened), but it inherits
  // the grace hold: once the feed opens, an empty grid has kHold to prove
  // itself before the claim is withdrawn as Degraded.
  Tracker t4;
  Check(t4.Update(sig(true, Activity::Active, false, 0, 0), 0) == State::Connected,
        "no evidence at all + Active session: the unverified Connected claim");
  Check(t4.Update(sig(true, Activity::Active, true, 3, 0), 1000) == State::Connected,
        "the feed opening onto an unproven window holds (grace), no flap");
  Check(t4.ReevalAtMillis() == 1000 + kHold,
        "…with the grace deadline scheduled like any other hold");
  Check(t4.Update(sig(true, Activity::Active, true, 3, 0), 1000 + kHold) ==
            State::Degraded,
        "an unverified claim that never proves out becomes Degraded, honestly");

  // ToString is total (log/test plumbing, never user-facing).
  Check(std::string(ToString(State::NoService)) == "no_service" &&
            std::string(ToString(State::Disconnected)) == "disconnected" &&
            std::string(ToString(State::Connecting)) == "connecting" &&
            std::string(ToString(State::Evaluating)) == "evaluating" &&
            std::string(ToString(State::Connected)) == "connected" &&
            std::string(ToString(State::Degraded)) == "degraded",
        "ToString names every state");
}

// --- the app's reattach blob (task #40) --------------------------------------
//
// WHY A SERVICE SELFTEST COVERS AN APP FILE. The blob is how the app decides
// whether it may adopt a tunnel THIS service is already running, and the whole
// failure it encodes only exists on a machine with an elevated service, a live
// tunnel and an app restart in between — which is to say it cannot be exercised
// anywhere a test can run. So the decision was moved out of SdkHost.cpp into a
// pure header (Common/RpcSessionBlob.h) and pinned here, exactly as
// VersionGrammar.h and ConnectionHealth.h already are. What this CANNOT prove
// is the live reattach itself; that still needs the owner.
void TestRpcSessionBlob() {
  Section("RpcSessionBlob — the reattach blob, its migration and its refusals");
  using namespace urnw::rpcsession;

  // The shape the SDK actually emits: canonical, dashed, lowercase.
  const std::string kId = "019fe9cc-3e1a-7a4b-9c2d-0a1b2c3d4e5f";

  // ---- the round trip -------------------------------------------------------
  //
  // The whole contract with the file on disk: what the fresh-start path writes
  // at start_tunnel is what the next launch reads back and pairs with. A field
  // that survives Serialize but not Parse is precisely the class of bug this
  // section exists to catch, so every field is compared, not just the new one.
  {
    Blob out;
    out.client_pem = "-----BEGIN CERTIFICATE-----\nclient\n-----END CERTIFICATE-----";
    out.server_cert_pem = "-----BEGIN CERTIFICATE-----\nserver\n-----END CERTIFICATE-----";
    out.host_port = "127.0.0.1:12035";
    out.instance_id = kId;
    const auto back = Parse(Serialize(out));
    Check(back.has_value(), "a serialized blob parses back");
    if (back) {
      Check(back->client_pem == out.client_pem, "…client_pem survives the round trip");
      Check(back->server_cert_pem == out.server_cert_pem,
            "…server_cert_pem survives the round trip");
      Check(back->host_port == out.host_port, "…host_port survives the round trip");
      Check(back->instance_id == out.instance_id,
            "…AND the instance id — the field the pairing fix turns on");
    }
  }

  // ---- MIGRATION: blobs written before the field existed ---------------------
  //
  // The exact bytes the shipped build writes. It must parse (there IS a session
  // recorded here) and it must come back UNPAIRABLE, because there is no id to
  // pair with and no safe substitute for one: an empty instance id becomes a nil
  // *Id at the cgo boundary and yields a device handle that silently answers
  // nothing, and the nil UUID would SKIP the service's pairing check rather than
  // pass it. Unpairable is what makes the caller start a fresh session instead.
  {
    const auto legacy = Parse(
        R"({"client_pem":"c","server_cert_pem":"s","host_port":"127.0.0.1:12035"})");
    Check(legacy.has_value(), "a blob from before instance_id existed still parses");
    if (legacy) {
      Check(legacy->host_port == "127.0.0.1:12035" && legacy->client_pem == "c" &&
                legacy->server_cert_pem == "s",
            "…with every field it DID carry intact");
      Check(legacy->instance_id.empty(),
            "…and an empty instance id, which the caller reads as "
            "not-reattachable");
    }
  }

  // ---- refusals: there is no session here ------------------------------------
  Check(!Parse("").has_value(), "an empty file is not a session");
  Check(!Parse("{").has_value(), "a truncated write is not a session (and no throw)");
  Check(!Parse("not json at all").has_value(), "garbage is not a session");
  Check(!Parse(R"(["client_pem","host_port"])").has_value(),
        "a json ARRAY is not a session");
  Check(!Parse("null").has_value(), "json null is not a session");
  Check(!Parse(R"({"client_pem":"c","server_cert_pem":"s"})").has_value(),
        "no host_port means nothing to reattach TO");
  Check(!Parse(R"({"host_port":""})").has_value(), "an empty host_port likewise");

  // A field of the wrong TYPE must not throw out of a parse that sits between
  // the filesystem and a bootstrap. Unreadable reads as absent, everywhere.
  {
    const auto odd = Parse(
        R"({"client_pem":42,"server_cert_pem":null,"host_port":"127.0.0.1:1",)"
        R"("instance_id":{"a":1}})");
    Check(odd.has_value(), "wrong-typed fields do not throw or discard the blob");
    if (odd) {
      Check(odd->client_pem.empty() && odd->server_cert_pem.empty(),
            "…they read as absent");
      Check(odd->instance_id.empty(),
            "…and a non-string instance id is unpairable, not garbage");
    }
  }

  // ---- what counts as an id we may hand to the SDK ---------------------------
  Check(IsPairableInstanceId(kId), "the canonical dashed uuid is pairable");
  Check(IsPairableInstanceId("019FE9CC-3E1A-7A4B-9C2D-0A1B2C3D4E5F"),
        "uppercase hex too — the sdk's decoder accepts both cases");
  Check(!IsPairableInstanceId(""), "empty is not an id");
  Check(!IsPairableInstanceId("00000000-0000-0000-0000-000000000000"),
        "the NIL uuid is REFUSED — a zero id makes the service SKIP the pairing "
        "check, so accepting one out of a file would disable the very check "
        "this change exists to make work");
  Check(!IsPairableInstanceId("019fe9cc3e1a7a4b9c2d0a1b2c3d4e5f"),
        "the 32-char dashless form is refused — the sdk never emits it");
  Check(!IsPairableInstanceId("019fe9cc-3e1a-7a4b-9c2d-0a1b2c3d4e5"),
        "one char short is refused");
  Check(!IsPairableInstanceId("019fe9cc-3e1a-7a4b-9c2d-0a1b2c3d4e5ff"),
        "one char long is refused");
  Check(!IsPairableInstanceId("019fe9cc-3e1a-7a4b-9c2d-0a1b2c3d4e5g"),
        "a non-hex digit is refused");
  Check(!IsPairableInstanceId("019fe9cc:3e1a:7a4b:9c2d:0a1b2c3d4e5f"),
        "separators must be dashes — the sdk slices at fixed offsets without "
        "checking them, so a 36-char string with the wrong separators reaches "
        "its hex decoder as garbage");
  Check(!IsPairableInstanceId("019fe9cc-3e1a-7a4b-9c2d0-a1b2c3d4e5f"),
        "…and dashes in the wrong PLACES are refused for the same reason");

  // The blob-level consequence of those two rules, since the blob is where the
  // caller actually reads them: an unusable id is downgraded to "unpairable",
  // never passed through.
  {
    const auto nil = Parse(
        R"({"host_port":"127.0.0.1:1",)"
        R"("instance_id":"00000000-0000-0000-0000-000000000000"})");
    Check(nil.has_value() && nil->instance_id.empty(),
          "a blob carrying the nil uuid parses, but not as a pairable id");
  }
  {
    const auto ok =
        Parse(R"({"host_port":"127.0.0.1:1","instance_id":")" + kId + R"("})");
    Check(ok.has_value() && ok->instance_id == kId,
          "…while a real id comes through untouched");
  }
}

}  // namespace

int RunSelfTest() {
  g_pass = 0;
  g_fail = 0;
  std::printf(
      "urnetworkd selftest — pure logic only. Nothing here opens the filter\n"
      "engine, creates an adapter, writes a route or flushes a cache. Two\n"
      "checks READ the machine (its resolver list, and whether dnsapi exports\n"
      "the flush entry point); neither changes it. It cannot prove that a\n"
      "filter blocks anything; that needs an elevated session and the\n"
      "leak-validation gates in p7-baseline/p7-gates.ps1. It also cannot prove\n"
      "that --stop-after actually stops at a step — every step past 5 needs\n"
      "elevation — only that the flag parses, clamps, and cannot widen\n"
      "--rpc-only. And it cannot register, start or stop the real service —\n"
      "the SCM half of `urnetworkd install` needs elevation — only that the\n"
      "verb's binPath quoting and its exit-code verdicts are right.\n");

  TestNetPolicyTable();
  TestFilterSet();
  TestServiceDnsPath();
  TestDnsDisclosureShape();
  TestResolverCacheFlush();
  TestPersistentIsGatedOff();
  TestConsoleArgs();
  TestStatusEgressWire();
  TestWindowTraceFlag();
  TestStopBudget();
  TestSdkResolverAgainstTable();
  TestVersionGrammar();
  TestUpdateFormats();
  TestInstallVerb();
  TestConnectionHealth();
  TestRpcSessionBlob();

  Section("WFP object identities (for `netsh wfp show filters` diffs)");
  for (const auto& g : WfpPolicy::ObjectGuidsText())
    std::printf("  %s\n", g.c_str());

  std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}

}  // namespace urnw
