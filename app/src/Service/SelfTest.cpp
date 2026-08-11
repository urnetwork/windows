// SPDX-License-Identifier: MPL-2.0
#include "SelfTest.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <iterator>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "ConnectAction.h"
#include "ConnectionHealth.h"
#include "ConsoleArgs.h"
#include "CrashDumps.h"
#include "Heartbeat.h"
#include "InstallVerb.h"
#include "NetPolicy.h"
#include "NetworkConfig.h"
#include "Protocol.h"
#include "RpcSessionBlob.h"
#include "Sdk.h"
#include "StopBudget.h"
#include "ThreadGuard.h"
#include "TunnelWatchdog.h"
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

  // ...and as a MULTISET, which is the stronger claim the dead-tunnel failsafe
  // rests on (task #41).
  //
  // With the kill switch ON a failsafe teardown takes Connected -> Armed while
  // the machine is still routed at the tun, on a path the user did not ask for.
  // The promise made about that path is "nothing is leaking" — and that promise
  // is only true if the transition is a PURE NARROWING. The set-based check
  // above would pass if Armed carried two copies of a permit Connected carries
  // once; a multiset difference cannot.
  {
    const std::multiset<std::string> a = Names(armed);
    const std::multiset<std::string> c = Names(connected);
    std::multiset<std::string> onlyInArmed;
    std::set_difference(a.begin(), a.end(), c.begin(), c.end(),
                        std::inserter(onlyInArmed, onlyInArmed.end()));
    Check(onlyInArmed.empty(),
          "Connected -> Armed is a PURE NARROWING, counted rather than merely "
          "named — so a failsafe teardown with the kill switch on can only ever "
          "permit LESS than the session it ended, which is what makes 'nothing "
          "is leaking' a fact about the filter set rather than a hope",
          onlyInArmed.empty() ? "" : *onlyInArmed.begin());
    Check(c.size() > a.size(),
          "…and strictly less: Armed drops the tun permit, the UI app permit "
          "and the tunnel-resolver DNS permit that Connected carries",
          std::format("armed={} connected={}", a.size(), c.size()));
  }

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

// Defined below, and called from the end of TestStopBudget so the two run as one
// story: the budget mechanism first, then the question StartLocked asks of it.
void TestHeldDeviceSweep();

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
  // The watchdog's two bounded joins sit at the TOP of StopLocked, ahead of the
  // route revert, so they are part of every stop's worst case and belong in the
  // same ledger. Counted rather than assumed negligible: the whole reason
  // StopBudget.h exists is that an unbounded wait on the stop path is a
  // correct-looking way to write "this machine is now stuck".
  Check(kStopLockBudget + kSdkTeardownBudget +
                std::chrono::milliseconds{2 * kWatchdogJoinBudgetMillis} <
            std::chrono::milliseconds{kServiceStopWaitHintMillis},
        "worst-case Stop() (lock budget + sdk budget + both watchdog joins) "
        "fits inside the STOP_PENDING wait hint given to the SCM — the hint is "
        "derived from these budgets, and this is what stops the two drifting "
        "apart the next time one of them is raised",
        std::format("{}ms + {}ms + 2x{}ms against a {}ms hint",
                    kStopLockBudget.count(), kSdkTeardownBudget.count(),
                    kWatchdogJoinBudgetMillis, kServiceStopWaitHintMillis));
  // THE MEASUREMENT THAT CAUSED THIS RAISE. A tester's machine finished the SDK
  // teardown in 2013 ms against the old 2000 ms budget and was abandoned for it,
  // then completed 392 ms later. A budget a real healthy machine can overrun by
  // 0.65% is not discriminating "wedged" from "slow"; it is measuring hardware.
  Check(kSdkTeardownBudget >= std::chrono::milliseconds{4000},
        "the sdk teardown budget leaves real room over the 2013ms a real "
        "machine actually took — the old 2000ms lost to it by 13ms and bricked "
        "connect until the service was restarted by hand",
        std::format("{}ms", kSdkTeardownBudget.count()));

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
          "process exit by TerminateProcess instead of unwinding through a "
          "thread that is still inside the sdk");
    // THE WATCHDOG-SAMPLER DECISION, PINNED AS MECHANISM. This RunBounded took
    // the DEFAULT hazard, i.e. a worker that owns nothing — the detached sdk
    // sampler's case (TunnelWatchdog.cpp), and the lock-free escapes in Stop()
    // and FailsafeStop(). It must commit the process to TerminateProcess, as
    // checked above, and it must NOT be able to refuse a later start: those
    // callers hold no device, no adapter and no pump, so they cannot make wintun
    // issue a second adapter on the pinned guid, which is the entire hazard the
    // refusal exists for. One bool used to answer both questions, and that is
    // how a merely-slow sampler bricked Connect for the life of the process.
    Check(SweepAbandonedTeardowns().outstanding == 0,
          "an abandoned worker that owns NOTHING does not hold a device, so it "
          "cannot refuse a start — only the exit path cares about it");
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

  TestHeldDeviceSweep();
}

// --- "is the device STILL held", which used to be "was it ever" --------------
//
// The bug (2026-08-11, both testers, build v2026.8.11-1016372210-beta): a
// teardown ran 2013 ms against a 2000 ms budget and was abandoned by THIRTEEN
// MILLISECONDS; the abandoned worker finished 392 ms later and released
// everything; 2.2 seconds after that, and for the rest of the process's life,
// every Connect was refused with "a previous teardown could not be completed and
// its device is still held". Nothing was held. StartLocked was reading a
// process-global one-way bool with no clear function.
//
// These checks are the honest question in miniature, against the REAL registry
// and the REAL RunBounded — a wedge that is genuinely outstanding must refuse, a
// wedge that has since finished must not, and the difference has to be visible
// rather than silent.
void TestHeldDeviceSweep() {
  Section("the abandoned-teardown gate — a refusal that can be withdrawn when "
          "the worker it was about finishes");

  ResetStopBudgetForTest();

  Check(SweepAbandonedTeardowns().outstanding == 0 &&
            SweepAbandonedTeardowns().completed_late == 0,
        "a process that has abandoned nothing holds nothing and reports nothing "
        "— the sweep's resting state is silence, not a latch waiting to fire");

  // --- abandoned and STILL RUNNING refuses; then finishes, and clears --------
  {
    constexpr std::chrono::milliseconds kTestBudget{150};
    auto state = std::make_shared<WedgeState>();
    const bool finished = RunBounded(
        kTestBudget,
        [s = state, owned = OwnedByTeardown(state)]() mutable {
          s->entered.store(true);
          while (!s->release.load())
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        },
        AbandonHazard::HoldsSessionDevice);
    Check(!finished, "the wedged teardown was abandoned, as the setup requires");

    const AbandonedTeardownSweep held = SweepAbandonedTeardowns();
    Check(held.outstanding == 1 && held.completed_late == 0,
          "WHILE THE WORKER IS GENUINELY OUTSTANDING the device counts as held "
          "and a start must be refused — this is the case the refusal exists "
          "for and it is not weakened",
          std::format("outstanding={} completed_late={}", held.outstanding,
                      held.completed_late));
    Check(SweepAbandonedTeardowns().outstanding == 1,
          "and asking twice does not lose it: a sweep retires only workers that "
          "have actually finished");

    // Now let it go, exactly as the tester's worker did 392 ms after it was
    // written off, and prove the refusal is WITHDRAWN.
    state->release.store(true);
    std::size_t lateSeen = 0;
    AbandonedTeardownSweep sweep{};
    for (int i = 0; i < 4000; ++i) {
      sweep = SweepAbandonedTeardowns();
      lateSeen += sweep.completed_late;
      if (sweep.outstanding == 0) break;
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    Check(sweep.outstanding == 0,
          "THE FIX: once the abandoned worker finishes, the device is no longer "
          "held and the next start proceeds — the tester's machine was refused "
          "2.2 seconds after this moment");
    Check(lateSeen == 1,
          "and it is REPORTED, exactly once, so the log says 'it finished late' "
          "instead of silently starting to work again",
          std::format("completed_late totalled {}", lateSeen));
    Check(state->ownedDestroyed.load(),
          "clearing means RELEASED, not 'nearly done': the gate publishes only "
          "after the worker's owned objects are destroyed, so a cleared sweep is "
          "a statement that the device, adapter and pump are actually gone");
    Check(SweepAbandonedTeardowns().completed_late == 0,
          "a retired worker is not re-reported on every later sweep — the good "
          "news is news once");
    Check(TeardownAbandoned(),
          "and the EXIT question keeps its answer regardless: a thread was "
          "abandoned in this process, so unwinding through static destructors is "
          "permanently off the table even though the start refusal is over");
    ResetStopBudgetForTest();
  }

  // --- two sequential abandons, finishing OUT OF ORDER ----------------------
  //
  // A process can abandon more than one teardown in its life, so the answer
  // cannot be a bool OR a single retained gate — and there is no rule that says
  // they finish in the order they were abandoned. Both are released in reverse
  // here for exactly that reason.
  {
    constexpr std::chrono::milliseconds kTestBudget{100};
    auto first = std::make_shared<WedgeState>();
    auto second = std::make_shared<WedgeState>();
    auto wedge = [](std::shared_ptr<WedgeState> s) {
      return [s, owned = OwnedByTeardown(s)]() mutable {
        s->entered.store(true);
        while (!s->release.load())
          std::this_thread::sleep_for(std::chrono::milliseconds{1});
      };
    };
    RunBounded(kTestBudget, wedge(first), AbandonHazard::HoldsSessionDevice);
    RunBounded(kTestBudget, wedge(second), AbandonHazard::HoldsSessionDevice);

    Check(SweepAbandonedTeardowns().outstanding == 2,
          "TWO abandoned teardowns are two held devices, not one flag set twice "
          "— a set of gates is the only representation that can count them");

    // Release the SECOND one first. A design that retained "the" abandoned
    // teardown, or that assumed FIFO completion, gets this wrong.
    second->release.store(true);
    AbandonedTeardownSweep sweep{};
    std::size_t lateSeen = 0;
    for (int i = 0; i < 4000; ++i) {
      sweep = SweepAbandonedTeardowns();
      lateSeen += sweep.completed_late;
      if (sweep.outstanding <= 1) break;
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    Check(sweep.outstanding == 1 && lateSeen == 1,
          "one of two finishing — the LATER one — retires only itself; the "
          "start stays refused because the other really is still holding a "
          "device",
          std::format("outstanding={} late={}", sweep.outstanding, lateSeen));

    first->release.store(true);
    for (int i = 0; i < 4000; ++i) {
      sweep = SweepAbandonedTeardowns();
      lateSeen += sweep.completed_late;
      if (sweep.outstanding == 0) break;
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    Check(sweep.outstanding == 0 && lateSeen == 2,
          "and only when BOTH have finished is the condition clear — each "
          "abandonment is accounted for exactly once");
    Check(first->ownedDestroyed.load() && second->ownedDestroyed.load(),
          "both workers destroyed what they owned; abandoning is a leak at "
          "worst, and not even that once they return");
    ResetStopBudgetForTest();
  }

  // --- the watchdog's detached sampler, and the two lock-free escapes --------
  //
  // NoteTeardownAbandoned() is reached from four places with two different
  // meanings, and this pins the split. The sampler (TunnelWatchdog.cpp) reads
  // through a raw DeviceLocal* that has already been cleared and owns no
  // adapter, no pump and no device; Stop() and FailsafeStop() call it on their
  // lock-free escapes, where no worker exists at all and the session is still
  // owned by the controller. None of the three can make wintun issue a second
  // adapter on the pinned guid, so none of them may refuse a start — while all
  // three still commit the process to TerminateProcess, because a thread really
  // is parked inside the sdk.
  {
    NoteTeardownAbandoned();  // what all three of those call sites do, verbatim
    Check(TeardownAbandoned(),
          "a bare abandonment note still answers the EXIT question: this "
          "process may no longer unwind through a thread it cannot locate");
    Check(SweepAbandonedTeardowns().outstanding == 0,
          "...and answers the START question with NO. A detached sampler "
          "holding nothing, or a lock-free escape that handed no worker "
          "anything, must not brick tunnel starts for the life of the process");
    ResetStopBudgetForTest();
    Check(!TeardownAbandoned() && SweepAbandonedTeardowns().outstanding == 0,
          "the test-only reset clears BOTH the flag and the registry, so these "
          "checks cannot leak into the ones after them");
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

  // ---- the failsafe's two fields (task #41) ---------------------------------
  //
  // Same argument, same failure mode: from_json tolerates an absent key, so a
  // typo in the to_json name produces a status that parses cleanly and always
  // reads "user disconnected you" over an automatic teardown.
  proto::TunnelStatus dead;
  dead.state = proto::TunnelState::Stopped;
  dead.stop_reason = kStopReasonNoInbound;
  dead.failsafe_armed = true;
  const nlohmann::json dj = dead;
  Check(dj.contains("stop_reason") && dj.contains("failsafe_armed"),
        "the status carries the teardown reason and the countdown flag on the "
        "wire");
  proto::TunnelStatus deadBack = dj.get<proto::TunnelStatus>();
  Check(deadBack.stop_reason == kStopReasonNoInbound && deadBack.failsafe_armed,
        "…and both survive the round trip",
        std::format("got '{}' armed={}", deadBack.stop_reason,
                    deadBack.failsafe_armed));
  Check(proto::IsFailsafeStop(deadBack.stop_reason),
        "…and the reason still reads as a failsafe stop on the far side, which "
        "is what switches the app's copy from 'you did this' to 'we did this "
        "to keep you online'");

  nlohmann::json noFailsafe = dj;
  noFailsafe.erase("stop_reason");
  noFailsafe.erase("failsafe_armed");
  proto::TunnelStatus older = noFailsafe.get<proto::TunnelStatus>();
  Check(older.stop_reason.empty() && !older.failsafe_armed &&
            !proto::IsFailsafeStop(older.stop_reason),
        "a peer too old to send them parses fine and claims neither a reason "
        "nor a countdown — absent means 'today's behaviour', which is why no "
        "protocol bump was needed here either");
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

// --- the Go runtime crash capture (task #39) ---------------------------------
//
// This is the one test here that touches the filesystem, and it is worth the
// exception. Everything the capture promises is a promise about a moment nobody
// will be present for — a Go fatal in a LocalSystem service, minutes into a
// tunnel, with the process about to call ExitProcess and take the evidence with
// it. There is no second chance to notice it did not work, so the mechanism is
// exercised end to end rather than described.
//
// It runs entirely inside a temp directory of its own and puts this process's
// stderr back where it found it. It opens no adapter, writes no route and does
// not go near the filter engine.

// Read a file that ANOTHER handle currently holds open for writing. This is not
// incidental to the test: the capture is only useful if the owner can read the
// crash file off a running service, so the share modes are part of the contract.
std::string ReadFileWhileOpen(const std::filesystem::path& p) {
  HANDLE h = ::CreateFileW(p.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return {};
  std::string out;
  char buf[4096];
  DWORD got = 0;
  while (::ReadFile(h, buf, sizeof(buf), &got, nullptr) && got > 0)
    out.append(buf, got);
  ::CloseHandle(h);
  return out;
}

void TestGoCrashCapture() {
  Section("go crash capture — where a Go runtime fatal goes (task #39)");

  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() /
      ("urnetworkd-selftest-gocrash-" + std::to_string(::GetCurrentProcessId()));
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);

  // Every arm below repoints this process's STD_ERROR_HANDLE and leaves the
  // capture handle open, because that is what the real thing does — it is meant
  // to outlive everything and be reclaimed by process exit. A test cannot exit
  // the process, so it plays the part of process exit itself: take the handle
  // back out of STD_ERROR_HANDLE, restore what was there, and close it.
  const HANDLE savedStderr = ::GetStdHandle(STD_ERROR_HANDLE);
  const auto releaseCapture = [&] {
    const HANDLE held = ::GetStdHandle(STD_ERROR_HANDLE);
    ::SetStdHandle(STD_ERROR_HANDLE, savedStderr);
    if (held && held != INVALID_HANDLE_VALUE && held != savedStderr)
      ::CloseHandle(held);
  };

  // --- arm it on a clean directory ------------------------------------------
  const GoCrashCapture first = RedirectGoCrashOutput(dir);
  Check(first.armed, "arming the capture on a fresh dir succeeds", first.error);
  Check(first.path == dir / L"go-crash.log", "it names go-crash.log");
  Check(std::filesystem::exists(first.path), "the file exists immediately");
  Check(std::filesystem::file_size(first.path, ec) == 0 && !ec,
        "…and starts empty, which is the healthy steady state");
  Check(first.carried_over.empty() && first.carried_over_bytes == 0,
        "a first-ever start carries nothing over");

  // --- write the way runtime.write1 writes ----------------------------------
  //
  // THE assertion this whole section exists for. The Go runtime does not write
  // through the CRT and cannot be asked to write anywhere: it calls
  // GetStdHandle(STD_ERROR_HANDLE) for itself and WriteFile()s to the result.
  // So the test does exactly that, with no CRT in the path, and then looks in
  // the file. If this does not land, nothing else here means anything.
  const HANDLE armed = ::GetStdHandle(STD_ERROR_HANDLE);
  Check(armed != savedStderr && armed != INVALID_HANDLE_VALUE,
        "STD_ERROR_HANDLE — the one thing the Go runtime consults — was moved");

  static constexpr char kMarker[] =
      "fatal error: urnetworkd selftest marker, not a real crash\n";
  static constexpr char kSecond[] = "goroutine 1 [running]:\n";
  const DWORD markerLen = static_cast<DWORD>(sizeof(kMarker) - 1);
  const DWORD secondLen = static_cast<DWORD>(sizeof(kSecond) - 1);
  DWORD wrote = 0;
  const BOOL ok = ::WriteFile(armed, kMarker, markerLen, &wrote, nullptr);
  Check(ok && wrote == markerLen,
        "a raw WriteFile to STD_ERROR_HANDLE lands, as the Go runtime's would");

  const std::string live = ReadFileWhileOpen(first.path);
  const DWORD liveErr = ::GetLastError();
  Check(live == kMarker,
        "…and is readable from another handle WHILE the capture holds it open — "
        "an unreadable crash file would be no crash file at all",
        std::format("read {} bytes, last error {}, on-disk size {}", live.size(),
                    liveErr, std::filesystem::file_size(first.path, ec)));

  // A traceback is many writes, not one, and the handle is append-only so that
  // none of them can land on top of another. Proving it takes one more write.
  DWORD wrote2 = 0;
  ::WriteFile(armed, kSecond, secondLen, &wrote2, nullptr);
  Check(ReadFileWhileOpen(first.path) == std::string(kMarker) + kSecond,
        "successive writes APPEND — a multi-line traceback accumulates rather "
        "than overwriting itself");

  const DWORD capturedLen = markerLen + secondLen;
  releaseCapture();

  // --- a crash, then a restart: the evidence has to survive ------------------
  // This is the hazard the rotation exists for. The service is configured to
  // auto-restart on failure, so the run that follows a Go fatal is five seconds
  // behind it and is the most likely thing to destroy the only copy.
  const GoCrashCapture second = RedirectGoCrashOutput(dir);
  Check(second.armed, "re-arming after a crashed run succeeds", second.error);
  Check(second.carried_over == dir / L"go-crash.prev.log" &&
            second.carried_over_bytes == capturedLen,
        "the crashed run's output is rotated aside, with its size reported so "
        "the restart can shout about it");
  Check(ReadFileWhileOpen(second.carried_over) == std::string(kMarker) + kSecond,
        "…intact, byte for byte");
  Check(std::filesystem::file_size(second.path, ec) == 0 && !ec,
        "…and the new run starts on an empty file again");

  releaseCapture();

  // --- and a HEALTHY restart must not quietly eat it ------------------------
  // The rotation is conditional on the file being non-empty for exactly this
  // reason. Rotate unconditionally and one clean restart — an owner rebooting,
  // an update re-pointing the service — silently replaces the traceback with
  // nothing, days before anyone thinks to look for it.
  const GoCrashCapture third = RedirectGoCrashOutput(dir);
  Check(third.armed, "arming over an empty file succeeds", third.error);
  Check(third.carried_over.empty(),
        "a run that wrote nothing rotates nothing");
  Check(ReadFileWhileOpen(dir / L"go-crash.prev.log") ==
            std::string(kMarker) + kSecond,
        "…so the kept traceback survives healthy restarts, not just one");

  releaseCapture();

  // Nothing may be left holding the file. An EXCLUSIVE open (share mode 0) is
  // the strict form of that question — it succeeds only when this process holds
  // no other handle at all — and it is the right one to ask, because the capture
  // shares for delete, so merely deleting the file would prove nothing.
  const HANDLE exclusive =
      ::CreateFileW(third.path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL, nullptr);
  Check(exclusive != INVALID_HANDLE_VALUE,
        "releasing the capture really releases the file (an exclusive open of "
        "it succeeds)",
        std::format("CreateFileW err={}", ::GetLastError()));
  if (exclusive != INVALID_HANDLE_VALUE) ::CloseHandle(exclusive);

  std::filesystem::remove_all(dir, ec);
  Check(!ec && !std::filesystem::exists(dir), "the temp dir cleans up",
        ec.message());
}

// --- does the native-fault channel actually produce anything? (task #39) -----
//
// Clearing SEM_NOGPFAULTERRORBOX restores this process's ABILITY to reach WER.
// What WER then writes is machine configuration, and on a box with no
// LocalDumps key the answer is "an Application Error 1000 and no stack". The
// verdicts below are what stops that from being discovered the hard way — by
// looking in an empty CrashDumps folder after the next death and concluding
// there was no fault.
//
// The probe itself is also RUN here, against this real machine, and its verdict
// printed: a check that only exercised synthetic structs would prove the
// formatter and tell the operator nothing about the box they are about to test
// on.
void TestCrashDumpChannel() {
  Section("crash-dump channel — what a native fault will actually leave behind");

  CrashDumpChannel none;  // defaults: WER on, no LocalDumps anywhere
  const std::string noneText = DescribeCrashDumpChannel(none, "urnetworkd.exe");
  Check(noneText.find("NOT KNOWN TO WRITE A DUMP FILE") != std::string::npos,
        "with LocalDumps absent the verdict says plainly that no dump file is "
        "expected — the failure mode being prevented is an investigator reading "
        "an empty folder as 'it did not crash'");
  Check(noneText.find("does not exist at all") != std::string::npos,
        "…and says which of the two ways it is unconfigured");
  Check(noneText.find("Application Error 1000") != std::string::npos,
        "…while still naming what WILL exist, so the channel is not written off "
        "entirely");
  Check(noneText.find("reg add") != std::string::npos &&
            noneText.find("DumpType") != std::string::npos &&
            noneText.find("urnetworkd.exe") != std::string::npos,
        "…and it carries the exact elevated command that opens the channel, "
        "for this executable by name");

  // THE SHAPE THIS MACHINE IS ACTUALLY IN, and the one a naive probe gets
  // wrong. LocalDumps exists — because Razer, NVIDIA and BlueStacks each
  // planted a per-executable subkey under it — but it carries no values of its
  // own and there is no urnetworkd.exe subkey. "The key exists" must NOT read
  // as "dumps are configured", or this instrumentation manufactures exactly the
  // false confidence it was written to destroy.
  CrashDumpChannel container;
  container.localDumpsGlobal = true;         // present…
  container.localDumpsGlobalHasValues = false;  // …but an empty container
  const std::string containerText =
      DescribeCrashDumpChannel(container, "urnetworkd.exe");
  Check(containerText.find("NOT KNOWN TO WRITE A DUMP FILE") != std::string::npos,
        "a LocalDumps key that exists ONLY as a container for other programs' "
        "per-executable subkeys is NOT reported as a working channel — the key "
        "existing is not the question, its values are");
  Check(containerText.find("no values of its own") != std::string::npos,
        "…and the reason is spelled out, so nobody re-checks the registry, sees "
        "the key, and overrules the log");
  Check(containerText.find("reg add") != std::string::npos,
        "…and it still hands over the command that fixes it");

  CrashDumpChannel off;
  off.werDisabled = true;
  const std::string offText = DescribeCrashDumpChannel(off, "urnetworkd.exe");
  Check(offText.find("OFF AT THE MACHINE LEVEL") != std::string::npos,
        "WER disabled machine-wide is reported as its own verdict");
  Check(offText.find("Application Error 1000") == std::string::npos ||
            offText.find("no Application Error 1000") != std::string::npos,
        "…and never promises an event-log entry that will not be written");

  CrashDumpChannel mini;
  mini.localDumpsForThisExe = true;
  mini.dumpType = 1;
  const std::string miniText = DescribeCrashDumpChannel(mini, "urnetworkd.exe");
  Check(miniText.find("OPEN") != std::string::npos,
        "a configured channel reports OPEN");
  Check(miniText.find("systemprofile") != std::string::npos,
        "…and with no DumpFolder set it names the WINDOWS DEFAULT location for "
        "a LocalSystem service, which is not the operator's profile and is not "
        "where anyone looks first");
  Check(miniText.find("MINI") != std::string::npos &&
            miniText.find("reg add") != std::string::npos,
        "…and a mini dump is called out as probably insufficient for a process "
        "that is half Go, with the command that upgrades it to a full dump");

  CrashDumpChannel full;
  full.localDumpsForThisExe = true;
  full.dumpType = 2;
  full.dumpFolder = L"C:\\ProgramData\\URnetwork\\service\\crashdumps";
  const std::string fullText = DescribeCrashDumpChannel(full, "urnetworkd.exe");
  Check(fullText.find("C:\\ProgramData\\URnetwork\\service\\crashdumps") !=
            std::string::npos,
        "a configured DumpFolder is reported verbatim — the operator should not "
        "have to guess where the file went",
        fullText.substr(0, 120));
  Check(fullText.find("MINI") == std::string::npos,
        "…and a full dump carries no mini-dump caveat");

  // The real machine, printed rather than asserted: what it says depends on the
  // box, and the point is that the operator reads it before the next run.
  const CrashDumpChannel live = ProbeCrashDumpChannel(L"urnetworkd.exe");
  Check(true,
        "probed THIS machine's WER configuration (read-only; nothing written)",
        std::format("wer_disabled={} local_dumps_key_exists={} "
                    "…and_has_values_of_its_own={} for_urnetworkd={} "
                    "dump_type={}",
                    live.werDisabled, live.localDumpsGlobal,
                    live.localDumpsGlobalHasValues, live.localDumpsForThisExe,
                    live.dumpType));
  std::printf("  ----  on THIS machine, after a native fault:\n        %s\n",
              DescribeCrashDumpChannel(live, "urnetworkd.exe").c_str());
}

// --- the per-thread terminate belt (task #39) --------------------------------
//
// WHAT THIS CAN AND CANNOT PROVE. It cannot prove the guard reverts routes or
// writes a log line on the way down: that path ends in std::abort(), and a test
// that reaches it takes the test process with it. What it CAN pin — and what
// the whole design rests on — is the platform fact underneath: whether
// std::set_terminate is per-thread on this toolchain. If it were process-wide,
// the handler wmain arms would already cover every worker and ThreadGuard would
// be dead weight; because it is per-thread, a worker that does not arm its own
// is structurally unable to log or revert, which is the gap that let four
// deaths pass without evidence.
void TestThreadGuard() {
  Section("thread guard — the per-thread terminate belt (task #39)");

  // wmain has already armed OnTerminate on this thread, so `outer` is a real
  // handler and not a default.
  const std::terminate_handler outer = std::get_terminate();
  Check(outer != nullptr,
        "the wmain thread has a terminate handler armed (main.cpp does it "
        "before any verb runs)");

  std::terminate_handler bare = nullptr;
  std::thread([&] { bare = std::get_terminate(); }).join();
  Check(bare != outer,
        "std::set_terminate is PER-THREAD here — a fresh thread does NOT "
        "inherit the wmain thread's handler, which is the entire reason "
        "ThreadGuard exists",
        std::format("wmain armed={} fresh-thread armed={} identical={}",
                    outer != nullptr, bare != nullptr, outer == bare));

  std::terminate_handler insideGuard = nullptr;
  std::string nameInside;
  std::thread([&] {
    RunGuarded("selftest-guarded", [&] {
      insideGuard = std::get_terminate();
      nameInside = ThreadGuardName();
    });
  }).join();
  Check(insideGuard != nullptr && insideGuard != bare,
        "a guarded body runs with a handler of OURS installed on its thread");
  Check(nameInside == "selftest-guarded",
        "…and the thread is named, so whatever it logs is attributable to a "
        "thread rather than to 'the service'",
        nameInside);
  Check(std::get_terminate() == outer,
        "arming a worker thread leaves the wmain thread's handler untouched — "
        "per-thread in both directions");

  std::string nameStarted;
  StartGuardedThread("selftest-started", [&] {
    nameStarted = ThreadGuardName();
  }).join();
  Check(nameStarted == "selftest-started",
        "StartGuardedThread arms and names the thread it creates, so a thread "
        "added later inherits the instrumentation instead of opting out of it",
        nameStarted);

  // THE REASON ServiceMain HAD TO BE ARMED BY HAND, pinned rather than argued.
  //
  // An unarmed thread does not get nothing — it gets the CRT's DEFAULT handler,
  // which measurably is NOT null on this toolchain (an earlier version of this
  // check asserted null and failed, which is why the check now states what the
  // machine actually does). What matters is that the default is the SAME on
  // every fresh thread and is not ours: it runs no log line and no
  // NetworkConfig::CrashRevert(). StartServiceCtrlDispatcher calls ServiceMain
  // on a thread IT creates, so on the SCM path the thread running SdkInit, the
  // control server and the entire teardown sat in exactly this state until
  // main.cpp started arming it by hand.
  std::terminate_handler bare2 = nullptr;
  std::thread([&] { bare2 = std::get_terminate(); }).join();
  Check(bare2 == bare && bare != outer,
        "every unarmed thread gets the same CRT DEFAULT handler, and it is not "
        "ours — so a thread we never armed cannot log or revert, which is why "
        "the SCM's ServiceMain thread had to arm its own",
        std::format("two fresh threads agree={} and differ from wmain's={}",
                    bare2 == bare, bare != outer));

  bool ranPastCatch = false;
  StartGuardedThread("selftest-inner-catch", [&] {
    try {
      throw std::runtime_error("handled inside the body");
    } catch (const std::exception&) {
    }
    ranPastCatch = true;
  }).join();
  Check(ranPastCatch,
        "an exception CAUGHT inside the body is none of the guard's business — "
        "it only ever sees what escapes");
}

// --- the heartbeat (task #39) ------------------------------------------------
//
// The death window in the four recorded deaths was as wide as the gap between
// two event-driven log lines: 9 to 28 seconds. This closes it to about one
// second. The parts that can be proved without a death are the record's shape
// (fixed width is what makes an in-place rewrite safe) and the file behaviour
// (one record, rewritten, readable while the service holds it).
// Counts the ticker's per-tick work. A file-scope atomic because HeartbeatTick
// is a bare function pointer — the service passes a glog flush through it, and
// a signature that admitted captures would admit state the ticker does not own.
std::atomic<int> g_heartbeatTicks{0};

void TestHeartbeat() {
  Section("heartbeat — narrowing the death window to about one second");

  Check(FormatUptime(0) == "00:00:00", "uptime formats from zero",
        FormatUptime(0));
  Check(FormatUptime(3723) == "01:02:03", "…as hh:mm:ss", FormatUptime(3723));
  Check(FormatUptime(360000) == "100:00:00",
        "…and hours are NOT wrapped at 24: a service up for four days must not "
        "read as one that just restarted",
        FormatUptime(360000));

  // The mirror starts where a process with no tunnel actually is. Checked
  // before anything below publishes over it.
  Check(std::string(PublishedTunnelState()) == "stopped",
        "the lock-free tunnel-state mirror starts at 'stopped'",
        PublishedTunnelState());

  const std::string a =
      FormatHeartbeatRecord(4321, 3723, "up", "2026-08-10 11:22:33");
  Check(a.size() == kHeartbeatRecordBytes,
        "every record is exactly the fixed width",
        std::format("got {} want {}", a.size(), kHeartbeatRecordBytes));
  Check(a.compare(0, 3, "\xEF\xBB\xBF") == 0,
        "…opens with a UTF-8 BOM, because Windows PowerShell 5.1 reads a "
        "BOM-less file in the ANSI code page (same reason Log.cpp writes one)");
  Check(a.ends_with("\r\n"), "…and ends with CRLF");
  Check(a.find("pid=4321") != std::string::npos &&
            a.find("uptime=01:02:03") != std::string::npos &&
            a.find("tunnel=up") != std::string::npos &&
            a.find("at=2026-08-10 11:22:33") != std::string::npos,
        "it carries the wall clock, the pid, the uptime and the tunnel state — "
        "which together say WHEN it died and WHAT it was doing", a);

  // Printed, not just asserted: the whole point of this file is that a human
  // finds it after a death and understands it without reading any code, so what
  // they will actually see belongs in the transcript. Trailing padding trimmed
  // for legibility only — on disk it is there, and it is what makes the
  // in-place rewrite safe.
  {
    std::string shown = a.substr(3);  // past the BOM
    while (!shown.empty() && (shown.back() == ' ' || shown.back() == '\r' ||
                              shown.back() == '\n'))
      shown.pop_back();
    std::printf("  ----  a heartbeat record reads:\n        %s\n",
                shown.c_str());
  }

  const std::string b =
      FormatHeartbeatRecord(1, 0, "rpc_only", "2026-08-10 11:22:34");
  Check(b.size() == a.size(),
        "a shorter tick still produces the same number of bytes — this is what "
        "makes an in-place rewrite safe, because a short record can never leave "
        "the tail of a long one on disk",
        std::format("{} vs {}", b.size(), a.size()));

  const std::string over = FormatHeartbeatRecord(
      4294967295UL, 999999999, std::string(400, 'x').c_str(),
      std::string(400, 'y'));
  Check(over.size() == kHeartbeatRecordBytes,
        "absurd input is truncated to the width rather than allowed to change "
        "it",
        std::format("got {}", over.size()));
  // Valid UTF-8 even after truncation, proved with the same encoder the wire
  // uses: nlohmann's strict dump() IS a UTF-8 validator.
  bool utf8Ok = true;
  try {
    (void)nlohmann::json(over).dump();
  } catch (const std::exception&) {
    utf8Ok = false;
  }
  Check(utf8Ok,
        "…and what survives is still valid UTF-8: truncation stops at a "
        "character boundary instead of cutting a multi-byte sequence in half");

  // ---- the file -------------------------------------------------------------
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() /
      ("urnetworkd-selftest-heartbeat-" + std::to_string(::GetCurrentProcessId()));
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);

  const std::filesystem::path file = dir / L"heartbeat.txt";
  {
    HeartbeatFile beat;
    Check(beat.Open(file), "the heartbeat file opens (creating its directory)",
          beat.LastError());

    PublishTunnelState("up");
    beat.Beat(3723, PublishedTunnelState());
    const std::string first = ReadFileWhileOpen(file);
    Check(first.size() == kHeartbeatRecordBytes,
          "one tick writes exactly one record, readable from another handle "
          "WHILE the writer holds it open — an unreadable heartbeat would be no "
          "heartbeat at all",
          std::format("read {} bytes", first.size()));
    Check(first.find("tunnel=up") != std::string::npos,
          "…carrying the published tunnel state, obtained without taking the "
          "tunnel lock (a wedged connect holds that lock forever, and that is "
          "exactly the state worth recording)");

    PublishTunnelState("stopped");
    beat.Beat(4, PublishedTunnelState());
    const std::string second = ReadFileWhileOpen(file);
    Check(second.size() == kHeartbeatRecordBytes,
          "a second tick REWRITES IN PLACE — the file does not grow, so a "
          "service running for weeks costs 256 bytes",
          std::format("size after two ticks: {}", second.size()));
    Check(second.find("tunnel=stopped") != std::string::npos &&
              second.find("tunnel=up") == std::string::npos,
          "…and nothing of the previous tick survives it");
  }
  std::filesystem::remove_all(dir, ec);
  Check(!ec && !std::filesystem::exists(dir),
        "the file is released and the temp dir cleans up", ec.message());

  // ---- the ticker, end to end ----------------------------------------------
  //
  // The half that cannot be proved by inspection: that the detached thread
  // really ticks at about 1 Hz, that its per-tick work runs (the service passes
  // a glog flush there), and that StopHeartbeat both stops it and returns
  // inside its bounded budget. That last one matters most — an unbounded
  // quiesce would have made the instrumentation a new way to hang shutdown,
  // which is the one thing this repo has already been burned by.
  //
  // StartHeartbeat latches after its first call, so this consumes the latch for
  // THIS process. Safe exactly here: the selftest verb never becomes a service
  // and never reaches Run()/RunConsole(), which are the only callers.
  const std::filesystem::path liveDir =
      std::filesystem::temp_directory_path() /
      ("urnetworkd-selftest-heartbeat-live-" +
       std::to_string(::GetCurrentProcessId()));
  std::filesystem::remove_all(liveDir, ec);
  const std::filesystem::path liveFile = liveDir / L"heartbeat.txt";

  g_heartbeatTicks.store(0);
  StartHeartbeat(liveFile, [] { g_heartbeatTicks.fetch_add(1); });
  std::this_thread::sleep_for(std::chrono::milliseconds(1200));

  const std::string live = ReadFileWhileOpen(liveFile);
  Check(live.size() == kHeartbeatRecordBytes,
        "the detached ticker really writes: one record on disk while the "
        "process runs",
        std::format("read {} bytes", live.size()));
  Check(g_heartbeatTicks.load() >= 2,
        "…and its per-tick work ran at about 1 Hz over 1.2s (this is where the "
        "service flushes the sdk's glog)",
        std::format("{} ticks", g_heartbeatTicks.load()));

  const auto stopStart = std::chrono::steady_clock::now();
  StopHeartbeat();
  const auto stopMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - stopStart)
                          .count();
  Check(stopMs <= 400,
        "StopHeartbeat returns inside its bounded quiesce budget — the ticker "
        "may never become a reason a shutdown hangs",
        std::format("took {}ms, budget 250ms", stopMs));

  const int settled = g_heartbeatTicks.load();
  std::this_thread::sleep_for(std::chrono::milliseconds(1400));
  Check(g_heartbeatTicks.load() == settled,
        "…and it really stopped: no further ticks after StopHeartbeat returned",
        std::format("{} -> {}", settled, g_heartbeatTicks.load()));

  std::filesystem::remove_all(liveDir, ec);
  Check(!ec && !std::filesystem::exists(liveDir),
        "the ticker released its file too", ec.message());
}

// --- the reply that used to kill the service (task #39) ----------------------
//
// PipeServer built `reply.dump()` OUTSIDE the try/catch that wrapped the
// handler. dump() throws type_error.316 on invalid UTF-8, and the reply carries
// SDK-supplied strings verbatim (ControlServer: reply.error = st.error), so one
// bad byte from a remote peer, a resolver or a Go library was enough to throw
// out of the pipe's worker thread — which, with no per-thread terminate handler
// armed, ended a LocalSystem service holding the machine's default routes with
// no log line and no route revert.
void TestWireSerialization() {
  Section("wire serialization — the reply dump() that could kill the service");

  proto::Reply bad;
  bad.ok = false;
  bad.in_reply_to = proto::msg::kStartTunnel;
  // The exact shape: a diagnostic string with bytes that are not valid UTF-8.
  // 0xFF and 0xFE can never appear in a UTF-8 sequence.
  bad.error = std::string("dial failed to ") + '\xFF' + '\xFE' + " (host)";
  const nlohmann::json j = bad;

  bool strictThrew = false;
  std::string strictWhat;
  try {
    (void)j.dump();
  } catch (const std::exception& e) {
    strictThrew = true;
    strictWhat = e.what();
  }
  Check(strictThrew,
        "a plain dump() STILL throws on that reply — the hazard is current, not "
        "historical",
        strictWhat);
  Check(strictWhat.find("316") != std::string::npos,
        "…and it is type_error.316, invalid UTF-8, which is the throw that sat "
        "outside PipeServer's try/catch",
        strictWhat);

  std::string wire;
  bool wireThrew = false;
  try {
    wire = proto::DumpForWire(j);
  } catch (const std::exception& e) {
    wireThrew = true;
    strictWhat = e.what();
  }
  Check(!wireThrew, "DumpForWire does not throw on it", strictWhat);

  bool parsed = false;
  proto::Reply round;
  try {
    round = nlohmann::json::parse(wire).get<proto::Reply>();
    parsed = true;
  } catch (const std::exception&) {
  }
  Check(parsed, "…it produces a message the peer can parse");
  Check(!round.ok && round.in_reply_to == proto::msg::kStartTunnel,
        "…and the fields the app ACTS on are untouched: only the unencodable "
        "bytes degrade");
  Check(round.error.starts_with("dial failed to ") &&
            round.error.find("(host)") != std::string::npos,
        "the diagnostic text either side of the bad bytes is preserved",
        round.error);
  Check(round.error.find("\xEF\xBF\xBD") != std::string::npos,
        "the invalid bytes became U+FFFD, which reads as 'a byte here was not "
        "valid UTF-8' — true, and itself the diagnosis");

  // The last-resort reply has to be serializable by construction, or it fails
  // in exactly the situation it exists for.
  bool fallbackOk = false;
  nlohmann::json fb;
  try {
    fb = nlohmann::json::parse(proto::kUnserializableReplyJson);
    fallbackOk = true;
  } catch (const std::exception&) {
  }
  Check(fallbackOk && proto::TypeOf(fb) == proto::msg::kReply &&
            fb.value("ok", true) == false,
        "the last-resort reply is a valid, failed reply — built from a literal, "
        "never from the data that just failed to serialize");
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

// --- the dead-tunnel failsafe (task #41) -------------------------------------
//
// WHY THE DECISION IS TESTED HERE AND THE I/O IS NOT. Reproducing the bug needs
// an elevated service, a wintun adapter, 31 routes, 47 filters and a network
// somebody can physically unplug. What CAN be pinned without any of that is the
// only part that must never be wrong: a false positive tears down a working VPN,
// and a false negative leaves the owner with no internet and no explanation —
// which is the report this whole area exists to answer.
//
// So Evaluate() is a pure function over a struct and a clock, exactly as
// ConnectionHealth::Tracker and DecideConsoleStop already are, and the table
// below is its specification.
void TestDeadTunnelVerdict() {
  Section("TunnelWatchdog — the dead-tunnel verdict (pure; no device, no thread)");

  // The session came up at t=0 and every case below reads at some later `now`.
  // Baseline: a healthy tunnel, sampled a moment ago, with a proven exit.
  auto healthy = [] {
    DeadTunnelSignals s;
    s.tunnelUp = true;
    s.routesInstalled = true;
    s.upSinceMillis = 1000;
    s.provenCount = 2;
    s.lastProvenMillis = 100000;
    s.lastSampleMillis = 100000;
    s.lastInboundMillis = 100000;
    s.outboundSinceInbound = 0;
    return s;
  };

  // ---- the states that are NOT this failsafe's business ---------------------
  {
    DeadTunnelSignals s = healthy();
    s.tunnelUp = false;
    s.provenCount = 0;
    s.lastProvenMillis = -1;
    s.lastInboundMillis = -1;
    Check(Evaluate(s, 10'000'000).reason == DeadTunnelReason::None,
          "a tunnel that is not up is never torn down by the failsafe — there "
          "is nothing holding this machine's routes to tear down");
    s = healthy();
    s.routesInstalled = false;
    s.provenCount = 0;
    s.lastProvenMillis = -1;
    Check(Evaluate(s, 10'000'000).reason == DeadTunnelReason::None,
          "…nor is a session with no routes installed (the rpc-only case, which "
          "carries nothing and blocks nothing)");
    s = healthy();
    s.upSinceMillis = 0;
    s.provenCount = 0;
    Check(Evaluate(s, 10'000'000).reason == DeadTunnelReason::None,
          "…nor is a session with no start instant, which is the shape of "
          "'there is no session'");
  }

  // ---- traffic is flowing: NOTHING may fire, ever --------------------------
  //
  // The load-bearing direction. Every threshold below is a licence to tear down
  // a VPN the user asked for, so the first thing pinned is that a working one
  // never reaches them.
  {
    bool everFired = false;
    for (int64_t t = 1000; t <= 600000; t += 500) {
      DeadTunnelSignals s = healthy();
      // proven, sampled and receiving continuously, while sending hard
      s.lastProvenMillis = t;
      s.lastSampleMillis = t;
      s.lastInboundMillis = t;
      s.outboundSinceInbound = 0;
      if (Evaluate(s, t).reason != DeadTunnelReason::None) everFired = true;
    }
    Check(!everFired,
          "over ten minutes of a tunnel that is proven, sampled and carrying "
          "packets in both directions, the failsafe NEVER fires");
  }

  // ---- an IDLE machine is not a dead one -----------------------------------
  {
    DeadTunnelSignals s = healthy();
    s.provenCount = 1;      // an exit is there
    s.lastProvenMillis = 1000;
    s.lastSampleMillis = 300000;   // still being sampled
    s.lastInboundMillis = 1000;    // nothing has come back since Up
    s.outboundSinceInbound = 0;    // ...because nothing was sent
    Check(Evaluate(s, 300000).reason == DeadTunnelReason::None,
          "an IDLE machine with a proven exit is left alone: no outbound "
          "commitment means no evidence, and a teardown with no evidence is "
          "the false positive that costs a working VPN");
  }
  {
    DeadTunnelSignals s = healthy();
    s.provenCount = 0;
    s.lastProvenMillis = -1;
    s.lastSampleMillis = 300000;
    s.lastInboundMillis = -1;
    s.outboundSinceInbound = kDeadFastOutboundPackets - 1;  // one short
    Check(Evaluate(s, 1000 + kDeadFastMillis + 1).reason !=
              DeadTunnelReason::NoInbound,
          std::format("…and {} outbound packets is one short of the fast "
                      "window's evidence bar, so DEAD_FAST does not fire on it",
                      kDeadFastOutboundPackets - 1));
  }

  // ---- DEAD_FAST: sending hard, nothing coming back ------------------------
  {
    DeadTunnelSignals s = healthy();
    s.provenCount = 0;
    s.lastProvenMillis = -1;
    s.lastSampleMillis = 500000;  // the sdk is answering; it just has no exit
    s.lastInboundMillis = -1;     // nothing has EVER come back
    s.outboundSinceInbound = kDeadFastOutboundPackets;
    Check(Evaluate(s, 1000 + kDeadFastMillis - 1).reason == DeadTunnelReason::None,
          std::format("one millisecond before {}s of committed sending with "
                      "zero inbound, nothing fires",
                      kDeadFastMillis / 1000));
    Check(Evaluate(s, 1000 + kDeadFastMillis).reason == DeadTunnelReason::NoInbound,
          std::format("…and at exactly {}s it does — reported as {}",
                      kDeadFastMillis / 1000, kStopReasonNoInbound));
  }

  // ---- ONE inbound packet resets everything --------------------------------
  //
  // Recovery is one-sided and immediate, mirroring ConnectionHealth::Tracker.
  // With the connected policy blocking every other path, a working tunnel's
  // host stack retransmits — so a single TCP ACK, DNS answer or keepalive is
  // proof, and it must count as proof instantly rather than after a hold.
  {
    const int64_t now = 1000 + kDeadFastMillis + 5000;
    DeadTunnelSignals s = healthy();
    s.provenCount = 0;
    s.lastProvenMillis = -1;
    s.lastSampleMillis = now;
    s.lastInboundMillis = -1;
    s.outboundSinceInbound = 4000;
    Check(Evaluate(s, now).reason == DeadTunnelReason::NoInbound,
          "a machine well past the fast window with thousands of packets sent "
          "and none received is Dead");
    // ...and now ONE packet arrives.
    s.lastInboundMillis = now;
    s.outboundSinceInbound = 0;
    Check(Evaluate(s, now).reason == DeadTunnelReason::None,
          "ONE inbound packet clears it outright — no hold, no hysteresis, no "
          "second sample needed");
  }

  // ---- DEAD_SLOW: no proven exit at all ------------------------------------
  {
    DeadTunnelSignals s = healthy();
    s.provenCount = 0;
    s.lastProvenMillis = -1;
    s.lastInboundMillis = -1;
    s.outboundSinceInbound = 0;  // idle: only the slow rule can apply
    s.lastSampleMillis = 1000 + kDeadSlowMillis;  // the sdk keeps answering
    Check(Evaluate(s, 1000 + kDeadSlowMillis - 1).reason == DeadTunnelReason::None,
          std::format("one millisecond before {}s with no proven exit, nothing "
                      "fires — a cold attempt after a roam costs ~35s and must "
                      "survive its second try",
                      kDeadSlowMillis / 1000));
    Check(Evaluate(s, 1000 + kDeadSlowMillis).reason == DeadTunnelReason::NoExit,
          std::format("…and at {}s it does, reported as {}, on a machine with "
                      "nothing coming back to veto it: a tunnel with nothing to "
                      "carry to cannot carry",
                      kDeadSlowMillis / 1000, kStopReasonNoExit));
    // One proven exit at any point restarts the whole clock.
    s.lastProvenMillis = 1000 + kDeadSlowMillis - 1;
    Check(Evaluate(s, 1000 + kDeadSlowMillis).reason == DeadTunnelReason::None,
          "…and a single proven exit restarts that clock from zero");
  }
  {
    // The one absence the slow rule refuses to read as evidence.
    DeadTunnelSignals s = healthy();
    s.provenCount = 0;
    s.lastProvenMillis = -1;
    s.lastSampleMillis = -1;  // the sampler has NEVER completed
    s.lastInboundMillis = -1;
    Check(Evaluate(s, 1000 + kSdkUnresponsiveMillis).reason ==
              DeadTunnelReason::SdkUnresponsive,
          "'the sdk never answered' is reported as UNRESPONSIVE, not as 'no "
          "exit' — the second would be a verdict on evidence that was never "
          "collected");
  }

  // ---- UNRESPONSIVE outranks the rest --------------------------------------
  {
    DeadTunnelSignals s = healthy();
    s.provenCount = 0;       // stale: it is whatever the last answer said
    s.lastProvenMillis = -1;
    s.lastInboundMillis = -1;
    s.outboundSinceInbound = 10000;
    s.lastSampleMillis = 1000;  // one sample at Up, then silence
    const int64_t now = 1000 + kDeadSlowMillis + 1;
    Check(Evaluate(s, now).reason == DeadTunnelReason::SdkUnresponsive,
          "when the sdk has stopped answering, the verdict is UNRESPONSIVE "
          "even though the other two rules also hold — they would be judging a "
          "reading that has not been refreshed");
    s.lastSampleMillis = now;  // it is answering again
    Check(Evaluate(s, now).reason == DeadTunnelReason::NoInbound,
          "…and with a fresh sample the verdict underneath surfaces — as "
          "NO_INBOUND rather than NO_EXIT, because with committed traffic the "
          "rule that MEASURED the hole outranks the one that inferred it, and "
          "it is also the better sentence to show a user");
    s.outboundSinceInbound = 0;  // the same machine, idle
    Check(Evaluate(s, now).reason == DeadTunnelReason::NoExit,
          "…and with no traffic to measure, the inference is all there is, so "
          "NO_EXIT is what is reported");
  }
  {
    DeadTunnelSignals s = healthy();
    s.lastSampleMillis = 1000;
    // Nothing has come back through the tun this session — without that the
    // carrying veto below would (correctly) refuse to call anything dead.
    s.lastInboundMillis = -1;
    Check(Evaluate(s, 1000 + kSdkUnresponsiveMillis - 1).reason ==
              DeadTunnelReason::None,
          std::format("{}s of sdk silence is not yet unresponsive",
                      (kSdkUnresponsiveMillis - 1) / 1000));
    Check(Evaluate(s, 1000 + kSdkUnresponsiveMillis).reason ==
              DeadTunnelReason::SdkUnresponsive,
          std::format("…{}s of it is, and this is the ONLY rule that can fire "
                      "while the sdk is wedged — it needs no cooperation from "
                      "it whatsoever",
                      kSdkUnresponsiveMillis / 1000));
  }

  // ---- THE CARRYING VETO: a measurement outranks every claim ---------------
  //
  // The load-bearing direction restated as a rule rather than as a coincidence.
  // Both remaining rules judge the SDK — one its answer, one its answering —
  // and both were reachable while the DATA PATH, which is a different set of
  // goroutines entirely, was still handing this machine packets. A failsafe
  // that tears down a tunnel the user is actively using is worse than the bug
  // it exists for.
  {
    DeadTunnelSignals s = healthy();
    s.provenCount = 0;
    s.lastProvenMillis = -1;
    s.lastSampleMillis = -1;  // the sdk has NEVER answered: a wedge by any reading
    s.outboundSinceInbound = 50000;
    const int64_t now = 1000 + 10 * kSdkUnresponsiveMillis;
    s.lastInboundMillis = now - (kDeadFastMillis - 1);  // ...but packets ARE arriving
    Check(Evaluate(s, now).reason == DeadTunnelReason::None,
          "an sdk that has not answered in five minutes does NOT tear down a "
          "tunnel that is still handing this machine packets — getExits() can "
          "block on a state lock the data path never takes, and the watchdog's "
          "own networkChanged() runs on that same thread");
    Check(!Evaluate(s, now).armed,
          "…and no countdown is shown either: there is nothing to warn about "
          "while the tunnel is demonstrably carrying");
    s.lastSampleMillis = now;  // the sdk answers again, still with no exit
    s.lastProvenMillis = -1;
    Check(Evaluate(s, now).reason == DeadTunnelReason::None,
          "…and neither does 'no proven exit', however long it has been: "
          "Exit.Proven is a probe result (task #15/S2 exists because exit state "
          "is incomplete) and a probe cannot outrank a packet");
    // ...and the veto lapses the moment the packets stop.
    s.lastSampleMillis = -1;
    s.lastInboundMillis = now - kDeadFastMillis;
    Check(Evaluate(s, now).reason == DeadTunnelReason::SdkUnresponsive,
          std::format("…and the veto is exactly {}s wide: one millisecond older "
                      "than the fast window and the wedge underneath surfaces, "
                      "so a real wedge is still caught within {}s of the last "
                      "packet",
                      kDeadFastMillis / 1000, kDeadFastMillis / 1000));
  }

  // ---- A FREEZE IS NOT A FAILURE ------------------------------------------
  //
  // Every window here is a duration on a steady clock, and Modern Standby keeps
  // that clock running while it freezes every thread. Without this the first
  // tick after a lid opens fires UNRESPONSIVE — against an sdk that was never
  // asked, before the resume's own network events can reach it, and with the
  // kill switch on that is a laptop that resumes blocked.
  {
    Check(!EvaluatorFroze(kEvaluateIntervalMillis),
          "a tick that arrives on time is not a freeze");
    Check(!EvaluatorFroze(kEvaluatorFrozenMillis - 1),
          "…nor is ordinary scheduling jitter short of the bar");
    Check(EvaluatorFroze(kEvaluatorFrozenMillis),
          std::format("…{}ms late, the process was not running, and every age "
                      "measured across it is the length of a sleep rather than "
                      "the length of a failure",
                      kEvaluatorFrozenMillis));
    Check(EvaluatorFroze(8 * 3600 * 1000),
          "…and an overnight modern-standby resume certainly is");
    Check(kEvaluatorFrozenMillis > kEvaluateIntervalMillis &&
              kEvaluatorFrozenMillis < kDeadFastMillis,
          "the freeze bar sits above the tick it protects and below the "
          "shortest window it protects, so it can neither be tripped by jitter "
          "nor swallow a real verdict");

    Check(StampInSession(500, 1000) == -1,
          "a reading taken before the session began is folded to 'never in this "
          "session' — after a rebase that is exactly what a sample completed "
          "before the machine went to sleep is");
    Check(StampInSession(1000, 1000) == 1000 && StampInSession(1500, 1000) == 1500,
          "…and a reading from inside the session is kept as it is");

    // End to end: the same signals, before and after the rebase the evaluator
    // performs. This is the case that fires today.
    const int64_t sleptFor = 8 * 3600 * 1000;
    const int64_t resumeAt = 1000 + sleptFor;
    DeadTunnelSignals s = healthy();
    s.provenCount = 0;
    s.lastProvenMillis = 900;
    s.lastSampleMillis = 900;   // the last sample completed before the sleep
    s.lastInboundMillis = 900;
    s.outboundSinceInbound = 0;
    Check(Evaluate(s, resumeAt).reason == DeadTunnelReason::SdkUnresponsive,
          "WITHOUT the rebase, the first tick after an eight-hour sleep tears "
          "the tunnel down and blames the sdk — this is the bug the rebase "
          "removes, pinned so the removal cannot be undone silently");
    // What RunEvaluator does instead: session start moves to now, and every
    // stamp that predates it becomes 'never in this session'.
    s.upSinceMillis = resumeAt;
    s.lastProvenMillis = StampInSession(s.lastProvenMillis, resumeAt);
    s.lastSampleMillis = StampInSession(s.lastSampleMillis, resumeAt);
    s.lastInboundMillis = -1;  // TrafficTracker::Reset
    Check(Evaluate(s, resumeAt).reason == DeadTunnelReason::None &&
              !Evaluate(s, resumeAt).armed,
          "…and WITH it the resumed session is judged on nothing but the time "
          "since it woke: no verdict, no countdown, a full fresh grace period "
          "for the sdk to re-dial on the network events the resume raises");
    Check(Evaluate(s, resumeAt + kSdkUnresponsiveMillis).reason ==
              DeadTunnelReason::SdkUnresponsive,
          std::format("…and a tunnel that is still dead {}s after the resume is "
                      "still torn down — the rebase delays the verdict, it does "
                      "not cancel it",
                      kSdkUnresponsiveMillis / 1000));
  }

  // ---- the countdown the UI renders ---------------------------------------
  //
  // The teardown must never be a surprise. `armed` is what lets the connect
  // page say "if nothing gets through in the next N seconds" BEFORE it happens,
  // so it has to be true only while a countdown is genuinely running and close.
  {
    DeadTunnelSignals s = healthy();
    Check(!Evaluate(s, 100000).armed,
          "a healthy tunnel never reports a countdown");

    s.provenCount = 0;
    s.lastProvenMillis = -1;
    s.lastInboundMillis = -1;
    s.lastSampleMillis = 200000;
    const int64_t early = 1000 + kDeadSlowMillis - kFailsafeNoticeMillis - 1000;
    Check(!Evaluate(s, early).armed,
          std::format("…and neither does one whose slow deadline is still more "
                      "than {}s away: a warning 90s early is noise that teaches "
                      "the owner to ignore the one that matters",
                      kFailsafeNoticeMillis / 1000));
    const int64_t late = 1000 + kDeadSlowMillis - 5000;
    const DeadTunnelVerdict v = Evaluate(s, late);
    Check(v.armed && v.reason == DeadTunnelReason::None,
          "…but 5s out it reports a countdown WITHOUT having fired, which is "
          "the whole point of the field");
    Check(v.millisToFailsafe == 5000,
          "…carrying the real remaining time, so the copy can say how long",
          std::format("got {}ms", v.millisToFailsafe));
  }
  {
    // The unresponsive countdown must not be permanently 'armed' just because a
    // sample lands every 2s and its deadline is therefore always ~28s out.
    DeadTunnelSignals s = healthy();
    s.lastSampleMillis = 200000;
    Check(!Evaluate(s, 200000 + kSdkUnresponsiveArmMillis - 1).armed,
          "a sample that arrived within the normal cadence never arms the "
          "unresponsive countdown, which would otherwise be armed forever");
    Check(Evaluate(s, 200000 + kSdkUnresponsiveArmMillis).armed,
          std::format("…{}s overdue, it does", kSdkUnresponsiveArmMillis / 1000));
  }

  // ---- the thresholds are what this file claims they are -------------------
  //
  // Pinned as VALUES, not just as behaviour: each one is justified in
  // TunnelWatchdog.h against a measured cost (the Windows DNS query schedule, a
  // black-holed TCP connect, the sampler's own cadence), and a silent edit to
  // any of them changes when a user's VPN is torn down.
  Check(kDeadFastMillis == 20000 && kDeadSlowMillis == 90000 &&
            kSdkUnresponsiveMillis == 30000 && kDeadFastOutboundPackets == 8,
        "the four thresholds are 20s fast / 90s slow / 30s unresponsive / 8 "
        "packets of committed evidence");
  Check(kDeadFastMillis > 13000,
        "the fast window exceeds the ~12-13s Windows DNS query schedule, so an "
        "honest machine-wide resolution stall cannot look like a dead tunnel");
  Check(kDeadSlowMillis > 2 * 35000,
        "the slow window is more than twice a cold attempt (DNS ~13s + TCP "
        "~21s + handshake), so a roam that recovers on its second try survives");
  Check(kSdkUnresponsiveMillis >= 15 * kSdkSampleIntervalMillis,
        "the wedge threshold is at least 15 consecutive missed samples — slow "
        "is not wedged");
  Check(kNetworkNotifyDebounceMillis < kEvaluateIntervalMillis &&
            kEvaluateIntervalMillis < kDeadFastMillis,
        "debounce < evaluate interval < the shortest verdict window, so the "
        "verdict is never decided by the sampling rate");
}

// The one-line function that decides whether the failsafe hands the machine
// back its internet or leaves it blocked. Pinned on its own because inverting
// it is a single character and the consequence is the opposite product.
void TestFailsafeDisarmChoice() {
  Section("TunnelWatchdog — what a failsafe teardown does to the firewall");

  Check(FailsafeFinalDisarm(/*killSwitchOn=*/false),
        "kill switch OFF -> finalDisarm TRUE: the policy is lifted and this "
        "machine gets its internet back. With the switch off, being left "
        "blocked by a tunnel that cannot carry is the one outcome the service "
        "must never produce");
  Check(!FailsafeFinalDisarm(/*killSwitchOn=*/true),
        "kill switch ON -> finalDisarm FALSE: the policy narrows to ARMED and "
        "this machine stays blocked. Silently restoring the internet would "
        "break the promise that setting makes — the state is surfaced instead");

  // The reason strings are a wire contract with the app, which switches copy on
  // them. A typo here is a user reading "you disconnected" after an automatic
  // teardown.
  Check(std::string(StopReasonOf(DeadTunnelReason::None)) == "" &&
            std::string(StopReasonOf(DeadTunnelReason::NoExit)) ==
                "failsafe_no_exit" &&
            std::string(StopReasonOf(DeadTunnelReason::NoInbound)) ==
                "failsafe_no_inbound" &&
            std::string(StopReasonOf(DeadTunnelReason::SdkUnresponsive)) ==
                "failsafe_sdk_unresponsive",
        "every reason has its exact wire spelling");
  Check(proto::IsFailsafeStop(StopReasonOf(DeadTunnelReason::NoExit)) &&
            proto::IsFailsafeStop(StopReasonOf(DeadTunnelReason::NoInbound)) &&
            proto::IsFailsafeStop(StopReasonOf(DeadTunnelReason::SdkUnresponsive)),
        "…and every one of them is recognised as a failsafe stop by the shared "
        "predicate the tray, the connect page and the status strip all use");
  Check(!proto::IsFailsafeStop(kStopReasonUser) &&
            !proto::IsFailsafeStop(kStopReasonNone),
        "…while a user disconnect and an unset reason are not, which is the "
        "distinction the whole surface hangs on");
}

// The coalescer and the traffic tracker: the two small stateful pieces that
// stand between an OS notification storm and a cgo call, and between two raw
// counters and the verdict's edge timestamps.
void TestEgressCoalescer() {
  Section("TunnelWatchdog — network-change coalescing and the packet tracker");

  // ---- a roam is one notification, not thirty -----------------------------
  {
    NotifyCoalescer c;
    Check(!c.pending() && !c.TakeDue(0),
          "an untouched coalescer has nothing to notify");

    // Thirty events inside 300ms — the shape of a Wi-Fi roam or a DHCP renew.
    for (int i = 0; i < 30; ++i) c.Observe(1000 + i * 10);
    int fired = 0;
    for (int64_t t = 1000; t <= 1000 + kNetworkNotifyDebounceMillis + 500; t += 10)
      if (c.TakeDue(t)) ++fired;
    Check(fired == 1,
          std::format("30 events inside 300ms produce EXACTLY ONE sdk "
                      "notification (each one would otherwise kick every "
                      "transport in the process)"),
          std::format("fired {} times", fired));
    Check(c.lastBurstSize() == 30,
          "…and it reports how many it folded, so the log reads as one kick "
          "over N events rather than a suspiciously quiet single event");
  }

  // ---- the window does not re-extend --------------------------------------
  //
  // A re-extending debounce can be starved indefinitely by a link that keeps
  // flapping, which is precisely the condition in which the SDK most needs to
  // be told the network moved.
  {
    NotifyCoalescer c;
    c.Observe(0);
    bool fired = false;
    for (int64_t t = 0; t <= 5000; t += 100) {
      c.Observe(t);  // never stops flapping
      if (c.TakeDue(t)) { fired = true; break; }
    }
    Check(fired,
          "a link that flaps continuously still gets a notification: the "
          "deadline is set by the first event of a burst and never pushed out");
  }

  // ---- a second burst is a second notification ----------------------------
  {
    NotifyCoalescer c;
    c.Observe(0);
    Check(c.TakeDue(kNetworkNotifyDebounceMillis), "the first burst fires");
    Check(!c.TakeDue(kNetworkNotifyDebounceMillis + 10000),
          "…and does not fire twice, however long the caller waits");
    c.Observe(20000);
    Check(!c.TakeDue(20000), "a new burst starts a new window");
    Check(c.TakeDue(20000 + kNetworkNotifyDebounceMillis),
          "…which fires on its own deadline");
  }

  // ---- the packet tracker --------------------------------------------------
  {
    TrafficTracker t;
    t.Reset(/*outbound=*/100, /*inbound=*/50);
    Check(t.lastInboundMillis() == -1 && t.outboundSinceInbound() == 0,
          "a fresh session inherits no evidence from the last one — the "
          "counters are monotonic across sessions, the verdict must not be");

    t.Observe(140, 50, 1000);
    Check(t.lastInboundMillis() == -1 && t.outboundSinceInbound() == 40,
          "40 packets out and none in: the commitment accumulates and there is "
          "still no inbound instant",
          std::format("got {}", t.outboundSinceInbound()));

    t.Observe(180, 51, 2000);
    Check(t.lastInboundMillis() == 2000 && t.outboundSinceInbound() == 0,
          "ONE packet in stamps the instant and resets the commitment — both "
          "halves of the fast window's precondition clear together");

    t.Observe(190, 51, 3000);
    Check(t.lastInboundMillis() == 2000 && t.outboundSinceInbound() == 10,
          "…and the commitment starts again from that packet, not from Up");

    // A relaxed read can straddle an increment and hand back an outbound total
    // older than the one recorded at the last inbound packet.
    t.Observe(5, 51, 4000);
    Check(t.outboundSinceInbound() == 10,
          "an out-of-order counter read cannot produce a negative (and "
          "therefore enormous, unsigned) commitment");
  }
}

// Every row here is a bug that has already shipped, or is one refactor away
// from shipping. The two the owner hit by name:
//
//   A. Disconnect drove only the SDK, so the routes, the DNS and the firewall
//      policy stayed installed and the machine had no internet — reported, from
//      the app's own tooltip, as a kill switch that was not on.
//   B. Connect drove only the SDK whenever device_ was non-null, so after the
//      tray's tunnel stop it re-issued connectBestAvailable() into a destroyed
//      listener and never sent start_tunnel.
//
// The table is evaluated at COMPILE TIME wherever it can be, which is the point
// of Decide being constexpr: a row that stops holding is a build error in the
// static_asserts and a FAIL line here.
void TestConnectGesture() {
  Section("ConnectAction — what each gesture does to the session and the tunnel");
  using gesture::ActionIsDisconnect;
  using gesture::AppFacts;
  using gesture::Decide;
  using gesture::Gesture;
  using gesture::KillSwitchIsLiftable;
  using gesture::MachineIsCaptured;
  using gesture::Plan;
  using gesture::ServiceFacts;
  using health::State;

  // A service with a real tunnel up: routes in, policy connected.
  auto tunnelUp = [] {
    ServiceFacts f;
    f.pipeUp = true;
    f.known = true;
    f.state = proto::TunnelState::Up;
    f.mode = proto::StartMode::Tunnel;
    f.routesInstalled = true;
    f.wfpState = "connected";
    return f;
  };
  // A service with nothing running and nothing in force.
  auto idle = [] {
    ServiceFacts f;
    f.pipeUp = true;
    f.known = true;
    return f;
  };
  auto app = [](bool haveDevice) {
    AppFacts a;
    a.haveDevice = haveDevice;
    return a;
  };

  // ---- bug A: Disconnect must reach the SERVICE ----
  {
    const Plan p = Decide(Gesture::Disconnect, tunnelUp(), app(true));
    Check(p.stopTunnel,
          "DISCONNECT over a live tunnel sends stop_tunnel — bug A: it used to "
          "drive only the SDK and leave 31 capture routes on the machine");
    Check(p.tearDownDevice,
          "…and drops the DeviceRemote, because the stop destroys the "
          "service-side listener it points at");
    Check(p.sdkDisconnect, "…and tells the connect controller to stop");
    Check(!p.startTunnel && !p.liftKillSwitch,
          "…and starts nothing and flips no preference");
    Check(p.why[0] != '\0', "…and says why, on a channel the log and the UI read");
  }
  {
    // The kill switch must not change the answer. Whether the policy is lifted
    // is the SERVICE's call (RevertMachineStateLocked's finalDisarm branch) and
    // it has always lifted on a deliberate stop; the plan carries no arming
    // intent in either direction.
    AppFacts a = app(true);
    a.killSwitch = true;
    const Plan on = Decide(Gesture::Disconnect, tunnelUp(), a);
    a.killSwitch = false;
    const Plan off = Decide(Gesture::Disconnect, tunnelUp(), a);
    Check(on.stopTunnel && off.stopTunnel,
          "DISCONNECT stops the tunnel with the kill switch ON exactly as with "
          "it off — a kill switch blocks an unexpected drop, not a deliberate "
          "one (that would be lockdown, which is not this product)");
    Check(on.stopTunnel == off.stopTunnel &&
              on.tearDownDevice == off.tearDownDevice &&
              on.liftKillSwitch == off.liftKillSwitch,
          "…and the plan is byte-identical: no arming decision lives on this "
          "side of the pipe");
  }
  {
    // The app restarted over a tunnel it did not start. There is no device to
    // tear down and the machine is still captured.
    const Plan p = Decide(Gesture::Disconnect, tunnelUp(), app(false));
    Check(p.stopTunnel && !p.tearDownDevice && !p.sdkDisconnect,
          "DISCONNECT with no device over a live tunnel still stops the tunnel "
          "— the machine is captured whether or not this process has a session");
  }
  {
    // Nothing installed, nothing in force: only the SDK has anything to do.
    const Plan p = Decide(Gesture::Disconnect, idle(), app(true));
    Check(!p.stopTunnel, "DISCONNECT with nothing installed sends no stop_tunnel");
    Check(p.tearDownDevice,
          "…but still drops a DeviceRemote the service has no session for — "
          "this is the row the tray escape hatch's follow-up lands on, and "
          "without it Connect stays broken after a force-stop (bug B)");
  }
  {
    // A firewall policy left armed with no tunnel: still a blocked machine, and
    // stop_tunnel is what lifts it (StopLocked(finalDisarm=true)).
    ServiceFacts f = idle();
    f.wfpState = "armed";
    const Plan p = Decide(Gesture::Disconnect, f, app(false));
    Check(p.stopTunnel,
          "DISCONNECT with the firewall armed and no tunnel sends stop_tunnel — "
          "the idempotent stop is what lifts a policy holding the machine");
  }

  // ---- bug B: Connect must ask the SERVICE, not device_ ----
  {
    ServiceFacts f = idle();  // the state a tray force-stop leaves behind
    const Plan p = Decide(Gesture::Connect, f, app(true));
    Check(p.startTunnel,
          "CONNECT with a device but NO tunnel starts one — bug B: `if "
          "(device_) { ok = true; }` sent nothing at all here");
    Check(p.tearDownDevice,
          "…after dropping the stale DeviceRemote, whose service-side listener "
          "the stop destroyed");
    Check(!p.stopTunnel,
          "…and does NOT send a deliberate stop first: start_tunnel is itself "
          "the reconciler, and its internal stop holds the firewall Armed "
          "across the gap a kill switch exists for");
    Check(p.sdkConnect, "…and then drives the connect controller");
  }
  {
    const Plan p = Decide(Gesture::Connect, idle(), app(false));
    Check(p.startTunnel && !p.tearDownDevice && p.sdkConnect,
          "CONNECT cold: bootstrap, then connect");
  }
  {
    const Plan p = Decide(Gesture::Connect, tunnelUp(), app(true));
    Check(!p.startTunnel && !p.tearDownDevice && !p.stopTunnel && p.sdkConnect,
          "CONNECT with session and tunnel both live touches neither — it just "
          "drives the controller");
  }
  {
    // App restarted over a live tunnel: no device, but a session to reattach to.
    // #40's reattach-by-instance-id runs inside the bootstrap, against a session
    // that is genuinely live — this row is what routes it there.
    const Plan p = Decide(Gesture::Connect, tunnelUp(), app(false));
    Check(p.startTunnel && !p.tearDownDevice && !p.stopTunnel,
          "CONNECT with a live tunnel and no device bootstraps (which reattaches "
          "rather than restarting) and stops nothing");
  }
  {
    // The row-click path is the same plan. It is the path that silently did
    // nothing after a tray stop, because it shares the worker's short-circuit.
    const Plan row = Decide(Gesture::ConnectRow, idle(), app(true));
    const Plan btn = Decide(Gesture::Connect, idle(), app(true));
    Check(row.startTunnel == btn.startTunnel &&
              row.tearDownDevice == btn.tearDownDevice &&
              row.sdkConnect == btn.sdkConnect,
          "a LOCATION ROW click plans exactly what the connect button plans");
  }
  {
    // After a failsafe teardown the routes are gone and the reason is set. The
    // gesture that follows is an ordinary cold connect; the reason must not
    // change the plan (it is copy, not state).
    ServiceFacts f = idle();
    f.stopReason = "failsafe_no_exit";
    const Plan p = Decide(Gesture::Connect, f, app(false));
    Check(p.startTunnel && !p.stopTunnel,
          "CONNECT after a failsafe stop starts a tunnel; the stop reason is "
          "an explanation, not an input");
  }
  {
    // rpc-only: the mode whose promise is that it never touches this machine.
    ServiceFacts f = idle();
    f.state = proto::TunnelState::RpcOnly;
    f.mode = proto::StartMode::RpcOnly;
    AppFacts a = app(true);
    a.wantsTunnel = false;
    const Plan p = Decide(Gesture::Connect, f, a);
    Check(!p.startTunnel && !p.tearDownDevice && !p.stopTunnel,
          "CONNECT in rpc-only mode never claims a tunnel and never restarts "
          "the session over the absence of one");
    a.wantsTunnel = true;  // a CLAMPED session: we asked for a tunnel, got this
    const Plan clamped = Decide(Gesture::Connect, f, a);
    Check(!clamped.startTunnel && !clamped.tearDownDevice,
          "…and a session the service CLAMPED to rpc-only is not torn down and "
          "restarted on every press: the standing mode notice is what explains "
          "it, and the existing bootstrap refusals stay authoritative");
    const Plan disc = Decide(Gesture::Disconnect, f, a);
    Check(!disc.stopTunnel && !disc.tearDownDevice && disc.sdkDisconnect,
          "…and DISCONNECT there stops the controller only — there is nothing "
          "installed on this machine to give back");
  }

  // ---- the pipe is down, or the read failed: never silence ----
  {
    ServiceFacts f;  // pipeUp=false, known=false
    for (const Gesture g : {Gesture::Connect, Gesture::ConnectRow,
                            Gesture::Disconnect, Gesture::EnsureSession,
                            Gesture::ForceStopTunnel, Gesture::LiftKillSwitch}) {
      const Plan p = Decide(g, f, app(true));
      Check(p.why[0] != '\0',
            std::string("with no control channel, gesture ") +
                std::to_string(static_cast<int>(g)) +
                " still says why rather than landing in silence");
    }
    const Plan connect = Decide(Gesture::Connect, f, app(false));
    Check(connect.startTunnel,
          "…and CONNECT still runs the bootstrap with the pipe down: that is "
          "what dials it, reports the failure and arms the service-reconnect "
          "watchdog. Making it a no-op would remove the app's only recovery "
          "from 'the service is not running yet'");
    const Plan stop = Decide(Gesture::ForceStopTunnel, f, app(true));
    Check(!stop.stopTunnel,
          "…and the escape hatch stops nothing: the adapter and the dynamic "
          "WFP session died with the process that held them");
  }
  {
    // The pipe is up but get_state did not answer. The two fallbacks are
    // OPPOSITE, and deliberately so.
    ServiceFacts f;
    f.pipeUp = true;
    f.known = false;
    const Plan connect = Decide(Gesture::Connect, f, app(true));
    Check(!connect.tearDownDevice && !connect.startTunnel,
          "an UNANSWERED get_state keeps the session a CONNECT already has — a "
          "dropped reply is not evidence that the tunnel is gone");
    const Plan disconnect = Decide(Gesture::Disconnect, f, app(true));
    Check(disconnect.stopTunnel && disconnect.tearDownDevice,
          "…while a DISCONNECT is honoured in full: stop_tunnel is idempotent, "
          "so stopping nothing is a cheaper mistake than leaving the machine "
          "captured");
  }

  // ---- the tray's two recovery gates ----
  Check(!KillSwitchIsLiftable(tunnelUp()),
        "the kill-switch item is NOT offered over a connected policy");
  {
    ServiceFacts f = idle();
    f.wfpState = "connected";
    Check(!KillSwitchIsLiftable(f),
          "…nor over a connected policy with no tunnel, which is the row that "
          "made the old `wfp_state != \"off\"` gate a DEAD CONTROL: "
          "SetKillSwitch's lift arm cannot act on it and returns true having "
          "done nothing");
    f.wfpState = "armed";
    Check(KillSwitchIsLiftable(f), "…and IS offered while armed");
    f.wfpState = "connecting";
    Check(KillSwitchIsLiftable(f),
          "…and while connecting, which the service's lift arm also handles");
    f.wfpState = "off";
    Check(!KillSwitchIsLiftable(f), "…and not when nothing is in force");
    f.wfpState = "armed";
    f.pipeUp = false;
    Check(!KillSwitchIsLiftable(f), "…and never with no service to ask");
  }
  Check(MachineIsCaptured(tunnelUp()), "routes installed is a captured machine");
  {
    ServiceFacts f = idle();
    f.wfpState = "armed";
    Check(MachineIsCaptured(f),
          "…and so is a policy in force with no routes: no internet either way");
    f.wfpState = "";
    Check(!MachineIsCaptured(f),
          "…while an ABSENT wfp_state reads as off, never as a claimed policy");
  }
  Check(!MachineIsCaptured(idle()), "an idle service captures nothing");

  // ---- the button-label rule, over the six rows the UI renders ----
  Check(ActionIsDisconnect(tunnelUp(), State::Connected), "connected -> Disconnect");
  Check(ActionIsDisconnect(tunnelUp(), State::Evaluating), "evaluating -> Disconnect");
  Check(ActionIsDisconnect(tunnelUp(), State::Degraded), "degraded -> Disconnect");
  Check(ActionIsDisconnect(tunnelUp(), State::Connecting), "connecting -> Disconnect");
  Check(ActionIsDisconnect(tunnelUp(), State::Disconnected),
        "SDK DISCONNECTED with routes still installed -> Disconnect. This is the "
        "cell the owner was stuck in: the button said Connect, and Connect was "
        "the one control that could not fix a machine with no internet");
  {
    ServiceFacts f = idle();
    f.wfpState = "armed";
    Check(!ActionIsDisconnect(f, State::Disconnected),
          "blocked by the ARMED kill switch -> Connect. That capture is the "
          "switch doing its stated job on a drop nobody asked for, reconnecting "
          "is the recovery, and the two controls that lift it are named in the "
          "copy and in the tray — the button does not have to be a third");
    f.wfpState = "connected";
    Check(ActionIsDisconnect(f, State::Disconnected),
          "…but a CONNECTED policy outliving its tunnel -> Disconnect: nothing "
          "else on any surface clears that one");
    f.wfpState = "armed";
    f.routesInstalled = true;
    Check(ActionIsDisconnect(f, State::Disconnected),
          "…and routes still installed is always Disconnect, whatever the "
          "policy says");
  }
  Check(!ActionIsDisconnect(idle(), State::Disconnected),
        "idle and uncaptured -> Connect");
  {
    ServiceFacts f;  // no pipe at all
    Check(!ActionIsDisconnect(f, State::NoService), "no service -> Connect");
  }
  {
    // The tray's one item resolves through that same rule, so the tray and the
    // window can never offer opposite actions for one machine state again.
    const Plan p = Decide(Gesture::TrayToggle, tunnelUp(), app(true));
    Check(p.stopTunnel && !p.startTunnel,
          "TRAY TOGGLE over a captured machine is the DISCONNECT plan");
    const Plan cold = Decide(Gesture::TrayToggle, idle(), app(false));
    Check(cold.startTunnel && !cold.stopTunnel,
          "…and the CONNECT plan over an idle one");
  }

  // ---- the escape hatch keeps working, and stops breaking Connect ----
  {
    const Plan p = Decide(Gesture::ForceStopTunnel, tunnelUp(), app(true));
    Check(p.stopTunnel && p.tearDownDevice,
          "FORCE STOP takes the tunnel down AND drops the session pointing at "
          "it — the second half is what lets Connect work afterwards");
  }

  // ---- EnsureSession never connects to anything ----
  {
    const Plan p = Decide(Gesture::EnsureSession, idle(), app(false));
    Check(p.startTunnel && !p.sdkConnect,
          "ENSURE SESSION builds a session and connects to nothing");
    const Plan live = Decide(Gesture::EnsureSession, tunnelUp(), app(true));
    Check(!live.startTunnel && !live.stopTunnel && !live.sdkConnect,
          "…and is a no-op when the session is already live");
  }

  // ---- ...and never UNDOES a disconnect ----
  //
  // The failure this pins is a second-order one, and it arrived with the fix
  // for bug A rather than before it. Disconnect now tears the DeviceRemote
  // down, so `if (device_)` no longer accidentally makes an ensure a no-op:
  // the service-reconnect watchdog firing after a service restart would find
  // no session, bootstrap one, and put the capture routes back on a machine
  // whose owner pressed Disconnect and has not touched it since.
  {
    AppFacts a = app(false);
    a.userDisconnected = true;
    const Plan p = Decide(Gesture::EnsureSession, idle(), a);
    Check(!p.startTunnel && !p.stopTunnel && !p.sdkConnect,
          "ENSURE SESSION after a deliberate disconnect starts NOTHING: a "
          "service that died and came back is not the user asking for a VPN");
    Check(p.why[0] != '\0', "…and still says why rather than landing in silence");

    // The watchdog reaches the worker the instant the pipe reappears, i.e.
    // BEFORE this side has redialled it — so the guard has to sit ahead of the
    // unknown-state fallback, which would otherwise bootstrap on !haveDevice.
    ServiceFacts down;  // pipeUp=false, known=false
    const Plan racing = Decide(Gesture::EnsureSession, down, a);
    Check(!racing.startTunnel,
          "…including on the exact path it exists for: the service came back, "
          "the pipe is not redialled yet, and the fallback would have "
          "bootstrapped over the top of the user's decision");

    // An explicit Connect is the user asking, and is never gated by it. (The
    // worker clears the flag before it decides; this pins that the table would
    // not have refused even if it had not.)
    const Plan connect = Decide(Gesture::Connect, idle(), a);
    Check(connect.startTunnel,
          "…while CONNECT is the user asking, and is never refused by it");
    const Plan row = Decide(Gesture::ConnectRow, idle(), a);
    Check(row.startTunnel, "…and neither is a location row click");

    // A session the app is reattaching to is a session somebody else started;
    // "make sure one exists" must still adopt it rather than sit out.
    const Plan reattach = Decide(Gesture::EnsureSession, tunnelUp(), a);
    Check(reattach.startTunnel,
          "…and a LIVE tunnel is still adopted: the flag suppresses starting "
          "one, never attaching to one that is already carrying traffic");
  }

  // ---- compile-time proof of the two rows the owner reported ----
  {
    constexpr ServiceFacts kCaptured = [] {
      ServiceFacts f;
      f.pipeUp = true;
      f.known = true;
      f.state = proto::TunnelState::Up;
      f.routesInstalled = true;
      f.wfpState = "connected";
      return f;
    }();
    constexpr AppFacts kWithDevice = [] {
      AppFacts a;
      a.haveDevice = true;
      return a;
    }();
    static_assert(Decide(Gesture::Disconnect, kCaptured, kWithDevice).stopTunnel,
                  "bug A: a disconnect over a live tunnel must reach the service");
    static_assert(ActionIsDisconnect(kCaptured, State::Disconnected),
                  "bug A: no surface may offer Connect over a captured machine");
    constexpr ServiceFacts kStopped = [] {
      ServiceFacts f;
      f.pipeUp = true;
      f.known = true;
      return f;
    }();
    static_assert(Decide(Gesture::Connect, kStopped, kWithDevice).startTunnel,
                  "bug B: a connect with a stale device must start a tunnel");
    Check(true,
          "the two owner-reported rows are static_asserts — they cannot regress "
          "without failing the build");
  }
}

int RunSelfTest() {
  g_pass = 0;
  g_fail = 0;
  std::printf(
      "urnetworkd selftest — pure logic, near enough. Nothing here opens the\n"
      "filter engine, creates an adapter, writes a route or flushes a cache.\n"
      "Two checks READ the machine (its resolver list, and whether dnsapi\n"
      "exports the flush entry point); neither changes it. One — the go crash\n"
      "capture — writes, because it can only be proven by doing: it repoints\n"
      "THIS process's stderr at a file in its own temp directory, checks what\n"
      "the Go runtime would write really lands there, then puts stderr back\n"
      "and deletes the directory. It cannot prove that a\n"
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
  TestConnectGesture();
  TestRpcSessionBlob();
  TestGoCrashCapture();
  TestThreadGuard();
  TestCrashDumpChannel();
  TestHeartbeat();
  TestWireSerialization();
  TestDeadTunnelVerdict();
  TestFailsafeDisarmChoice();
  TestEgressCoalescer();

  Section("WFP object identities (for `netsh wfp show filters` diffs)");
  for (const auto& g : WfpPolicy::ObjectGuidsText())
    std::printf("  %s\n", g.c_str());

  std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}

}  // namespace urnw
