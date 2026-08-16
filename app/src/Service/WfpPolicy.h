// The leak-prevention layer: a user-mode Windows Filtering Platform policy
// owned by urnetworkd. This closes R6 (DNS leaks to other adapters' resolvers)
// and is what the kill-switch toggle actually drives. The connected tunnel is
// deliberately IPv4-only without capturing or blocking host IPv6; the IPv6
// floor below applies only while the kill switch has no connected tunnel.
//
// Spec: docs/superpowers/research/2026-08-08-windows-leak-prevention-wfp.md
//
// ---------------------------------------------------------------------------
// WHY WFP AND NOT setRouteLocal
//
// Device::setRouteLocal is a branch inside the SDK's DeviceLocal.sendPacket. It
// only ever sees packets the OS ALREADY ROUTED INTO THE TUN, so it structurally
// cannot cover IPv6 (no v6 route on the tun), LAN (deliberately excluded from
// the capture set), another adapter's resolver, a split-tunnel exclusion, or —
// the case a kill switch exists for — a dead urnetworkd, where there is no code
// of ours left running to drop anything. The SDK default is DefaultRouteLocal =
// true, i.e. leak. The toggle and its persistence are unchanged; what it drives
// moved here.
//
// ---------------------------------------------------------------------------
// WHY THIS NEEDS NO DRIVER
//
// FwpmEngineOpen0 / FwpmFilterAdd0 / FwpmTransaction* are fwpuclnt.dll, user
// mode. The ALE layers are kernel LAYERS but they are MANAGED from user mode;
// WireGuard-for-Windows' entire firewall and Mullvad's boot-time/persistent
// filters are both added from a user-mode daemon. Only per-app socket
// REDIRECTION (split tunnelling) needs a callout driver, and that is already
// SplitTunnel.sys, already gated on attestation signing, and out of scope here.
// Nothing in this file is blocked by app/SIGNING.md.
//
// ---------------------------------------------------------------------------
// CRASH SAFETY IS THE DESIGN CENTRE
//
// A persistent block filter left behind by a crashed VPN is the worst bug in
// this category, so the policy is registered on an engine opened with
// FWPM_SESSION_FLAG_DYNAMIC: BFE destroys the provider, both sublayers and
// every filter when this process dies, for ANY reason — orderly exit,
// std::terminate, a Go panic inside the SDK, TerminateProcess, a bugcheck. That
// is deliberately the SAME structural guarantee the wintun adapter already
// gives the route table (NetworkConfig.h), inherited rather than reinvented.
// The correct behaviour when urnetworkd dies is FAIL OPEN: the user gets their
// network back and the app says the kill switch is no longer in force. A kill
// switch that survives its own enforcement process is not more secure, only
// harder to undo.
//
// Consequently NetworkConfig::CrashRevert has NO WFP path, on purpose — see the
// note next to its declaration.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace urnw {

// The four states the policy has.
//
// ARMED AND CONNECTING ARE DIFFERENT POLICIES, and the difference is exactly one
// filter: 9b, the DNS permit for the host's own resolvers.
//
// They were one state until 2026-08-08, on the argument that "disconnected but
// armed" and "trying to connect" must allow exactly the same things or the
// transition between them is a window where the policy is briefly weaker. That
// argument is right about the DIRECTION a difference may take and wrong about
// the price of this particular one. Filter 9b CANNOT BE SCOPED TO OUR PROCESS —
// Go resolves through GetAddrInfoW, which Windows serves from Dnscache inside
// svchost.exe, so the permit is address-scoped and machine-wide by construction
// (see host_resolvers_v4 below). Merged, that meant an armed, idle machine let
// EVERY process on it resolve in plaintext for as long as the kill switch was
// on. A kill switch that leaves DNS open while nothing is even connecting is not
// a kill switch, and "idle" is the state it spends almost all of its time in.
//
// The split keeps the safety property the merge was protecting by making the
// difference ONE-DIRECTIONAL: Connecting is a strict SUPERSET of Armed. The
// Armed -> Connecting transition only widens, the Connecting -> Armed transition
// only narrows, and there is no instant at which something both states permit is
// blocked. The selftest asserts the superset relation with 9b as the single
// named difference, so a second name silently joining it fails the build's tests.
//
// The cost is stated plainly because it is real: while a connection attempt is
// in flight, plaintext DNS to the host's own resolvers is open MACHINE-WIDE, not
// just to us. That window is bounded (TunnelController's connecting watchdog),
// logged at both edges, and disclosed in the kill-switch UI copy.
enum class WfpState {
  // Nothing installed. The machine's network is exactly as WFP found it.
  Off,
  // Kill switch on, idle, NO connection attempt in flight: our service,
  // loopback, LAN, DHCP and NDP only. NO DNS PERMIT — the port-53 hard block is
  // in force with nothing lifted through it, so nothing on this machine
  // resolves. That is not a bug and it is not the "no usable resolver"
  // stand-down; it is what the state means.
  Armed,
  // A connection attempt IS in flight. Armed plus filter 9b, so our own name
  // resolution (which leaves svchost.exe, not this process) can reach the
  // platform host. Entered before any resolution is attempted and left as soon
  // as the attempt succeeds (-> Connected) or fails (-> Armed).
  Connecting,
  // Tunnel up. Connecting's superset minus 9b: the tun interface and the
  // tunnel's OWN resolvers replace the host's, which is the R6 fix.
  Connected,
};

const char* ToString(WfpState s);

// Is a connection attempt made from this state?
//
// THE INVARIANT, RESTATED. It used to be "the port-53 block is installed only if
// a DNS path was emitted", which existed to keep the unrecoverable state — armed,
// blocked, unable to resolve, therefore unable to reconnect, therefore still
// armed — unreachable by construction.
//
// That form does not survive the Armed/Connecting split, because under the split
// "armed, port-53 blocked, no DNS path" is the CORRECT policy rather than the
// unrecoverable one. Read literally it would stand the block down in Armed, i.e.
// it would undo the whole change and reopen plaintext DNS machine-wide on an idle
// machine.
//
// So the invariant moves to the states it was always really about:
//
//   EVERY STATE FROM WHICH A CONNECTION ATTEMPT IS MADE MUST HAVE A DNS PATH.
//
// which is Connecting (the host's own resolvers, filter 9b) and Connected (the
// tunnel's, filter 10). The fail-safe that stands the block down when the host
// has no usable resolver at all moves with it: it applies to those two states and
// MUST NOT apply to Armed. Armed with a block and no path is the design.
//
// Declared here, next to the enum, so BuildFilterSet and the selftest ask the
// same question of the same function rather than each spelling out a list of
// states that can drift apart.
constexpr bool AttemptsConnection(WfpState s) {
  return s == WfpState::Connecting || s == WfpState::Connected;
}

struct WfpConfig {
  // NET_LUID.Value of the wintun adapter, or 0 when there is no tun. Kept as a
  // LUID and never an interface INDEX: indices are recycled, so an index-based
  // permit can end up permitting traffic out an unrelated adapter that later
  // took the number. Silent leak, and the reason NetworkConfig works in LUIDs.
  uint64_t tun_luid = 0;

  // The resolvers actually applied to the tun. Only these are permitted on
  // port 53, and only over the tun interface.
  std::vector<std::string> tunnel_resolvers_v4;

  // The resolvers the HOST is configured with on its own adapters. READ ONLY IN
  // Connecting: there is no tun yet, so there is no tunnel resolver, and this is
  // the only path our own name resolution has.
  //
  // This exists because the service's own name resolution DOES NOT COME OUT OF
  // urnetworkd.exe. Go on Windows resolves through the OS resolver
  // (net/lookup_windows.go -> GetAddrInfoW), which is an RPC into the DNS
  // Client service; the wire query is issued by svchost.exe. So the app-id
  // permit in the DNS sublayer cannot match it, and without an ADDRESS-scoped
  // permit a connecting machine cannot resolve the platform host and therefore
  // cannot connect at all. See filter 9b in WfpPolicy.cpp.
  //
  // THE SAME FACT IS WHY Armed IGNORES THIS FIELD. An address-scoped permit is
  // machine-wide — it cannot be narrowed to us, because the query is not ours by
  // the time it hits the filter engine — so keeping it installed while merely
  // idle opens plaintext DNS for every process on the box. BuildFilterSet
  // therefore emits 9b for Connecting only, and Armed's filter set does not
  // depend on this field at all. That independence is load-bearing: it is what
  // lets the connecting watchdog rebuild the armed policy off-thread without
  // reading any session state (TunnelController::BaseWfpConfig).
  //
  // Populated by TunnelController from NetworkConfig::HostResolversV4 on every
  // policy application, so a roam that changes the machine's resolvers is
  // picked up at the next connection attempt rather than needing its own
  // notification path.
  std::vector<std::string> host_resolvers_v4;

  // Permit the ranges in NetPolicy.h's kLocalBypassV4. Must stay true while the
  // route set excludes them, or the firewall and the routing table disagree and
  // the LAN silently stops working. Present as a field so the coupling is
  // visible, not so it can be flipped casually.
  bool allow_lan = true;

  // Block IPv6 at the two v6 ALE layers while Armed or Connecting. Connected
  // deliberately leaves host IPv6 on the physical network: Wintun receives no
  // IPv6 address, route, or DNS server, and we do not turn that absence into a
  // blackhole. NOT DisabledComponents and NOT
  // Set-NetAdapterBinding: Microsoft calls unbinding an unsupported
  // configuration, it is per-adapter so a dock or hotspot leaks anyway, and it
  // is persistent machine state that survives our process dying. Route
  // blackholes (::/1 + 8000::/1) do not stop link-local or the on-link /64 from
  // an RA, and they make connections hang to TCP timeout instead of failing
  // fast — which defeats Happy Eyeballs. A WFP block fails instantly, so v4
  // fallback happens in milliseconds.
  bool block_ipv6_when_disconnected = true;

  // Block LLMNR (5355), mDNS (5353) and NetBIOS name service (137/138/139).
  // Port 53 is not the only way a name becomes an address, and LLMNR in
  // particular fires on ANY DNS failure — which is precisely the state this
  // policy creates. Costs local device discovery (printers, casting) while
  // connected; that is the trade, and it is why this is a field.
  bool block_local_name_resolution = true;

  // Full path to urnetworkd.exe. The single most important exemption: without
  // it the machine is armed, blocked, and unable to reconnect.
  std::wstring service_image_path;

  // Full path to URnetwork.exe, the UI process — or empty when it cannot be
  // located, in which case no app permit is emitted at all.
  //
  // READ IN Connected AND NOWHERE ELSE. That scoping is the entire design of
  // this field and it is enforced in BuildFilterSet, asserted by the selftest,
  // and must not be relaxed:
  //
  //   Armed      — NOT permitted. The owner's standing ruling is that the armed
  //                state permits urnetworkd and nothing else, so that a kill
  //                switch cannot put user traffic on the physical NIC in the
  //                clear. Nothing here touches that.
  //   Connecting — NOT permitted. Connecting is Armed plus filter 9b and the
  //                selftest pins that as the SINGLE difference; a second name
  //                joining it would break the one-directional-widening property
  //                the Armed/Connecting split rests on.
  //   Connected  — PERMITTED, and only here.
  //
  // WHY Connected NEEDS IT AT ALL. URnetwork.exe runs its OWN SDK instance: a
  // second URnetworkSdk.dll, a second Go runtime, a second set of platform
  // sockets for account/auth/JWT refresh. Once the tunnel is up those sockets
  // follow the route table into the tun like every other process's, so the UI's
  // platform traffic is carried by the very tunnel it exists to report on — and
  // when that tunnel has no working exit the UI cannot reach the platform at
  // exactly the moment the user is looking at it to find out why. Observed
  // 2026-08-08: "[dtm]failed to refresh JWT: Timeout." logged by URnetwork.exe
  // (pid 2584) while a tunnel was up.
  //
  // The service already solves this for itself, at step 2/8, by binding its SDK
  // sockets to the physical interface (EgressMonitor -> setEgressInterfaceIndex,
  // i.e. IP_UNICAST_IF). That is R1 self-exclusion, and it is PROCESS-GLOBAL
  // inside the SDK — so it covers urnetworkd's DLL instance and cannot reach the
  // app's. The app now performs the same bind on its own instance, from the
  // egress index the service reports in TunnelStatus.
  //
  // THIS FILTER IS THE OTHER HALF OF THAT, AND NEITHER HALF WORKS ALONE:
  //   * the bind alone moves the app's sockets off the tun and straight into the
  //     weight-0 baseline floor, because only cfg.service_image_path is
  //     permitted there — the app would fail FASTER, not succeed;
  //   * this permit alone changes nothing, because WFP permits do not reroute:
  //     without the bind the app's packets are still inside the tun, where they
  //     are already permitted by the tun-LUID filter.
  //
  // WHAT IT COSTS, stated plainly. While Connected, URnetwork.exe may send and
  // receive on the physical NIC in the clear. That is a real exposure and it is
  // bounded to: this one binary, this one state, and the traffic it actually
  // makes — platform API/auth calls to the same hosts urnetworkd is ALREADY
  // reaching in the clear on that NIC by necessity. It is not a new class of
  // observable, and it does not touch what the kill switch promises, because the
  // kill switch is about Armed.
  //
  // NOT REPEATED IN THE DNS SUBLAYER, for filter 9's reason: the app is Go too,
  // Go on Windows resolves through GetAddrInfoW, and the wire query leaves
  // svchost.exe, so an app-id permit there would match nothing. The app's name
  // resolution therefore still goes to the tunnel's resolvers over the tun. See
  // the note in TunnelController::AppImagePath.
  std::wstring app_image_path;
};

// ---------------------------------------------------------------------------
// The describable filter set.
//
// BuildFilterSet is a PURE FUNCTION over (state, config). It touches no Windows
// API and never contacts the Base Filtering Engine, which is what makes the
// policy unit-testable on a machine where adding a real filter is impossible
// (it needs elevation). WfpPolicy::Apply is the only thing that turns these
// descriptions into FWPM_FILTER0s.
// ---------------------------------------------------------------------------

enum class WfpLayer {
  ConnectV4,      // FWPM_LAYER_ALE_AUTH_CONNECT_V4
  ConnectV6,      // FWPM_LAYER_ALE_AUTH_CONNECT_V6
  RecvAcceptV4,   // FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V4
  RecvAcceptV6,   // FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V6
};

// Two sublayers, and the split is load-bearing rather than tidiness. DNS has to
// be decided independently of "is this LAN": the LAN permit covers
// 192.168.0.0/16, which contains the router's resolver, so if DNS lived in the
// same sublayer the LAN permit would win on weight and the query would leave.
// Lifting port 53 to a soft permit in Baseline and hard-blocking it in Dns gets
// both — LAN works, LAN DNS does not — because across sublayers every sublayer
// is evaluated and BLOCK BEATS PERMIT.
enum class WfpSublayer {
  Baseline,
  Dns,
  // Never installed by this build. See BuildPersistentFilterSet.
  Persistent,
};

enum class WfpField {
  AppId,          // FWPM_CONDITION_ALE_APP_ID
  Flags,          // FWPM_CONDITION_FLAGS
  LocalInterface, // FWPM_CONDITION_IP_LOCAL_INTERFACE (LUID, not index)
  Protocol,       // FWPM_CONDITION_IP_PROTOCOL
  LocalPort,      // FWPM_CONDITION_IP_LOCAL_PORT   (== ICMP TYPE on ALE)
  RemotePort,     // FWPM_CONDITION_IP_REMOTE_PORT  (== ICMP CODE on ALE)
  LocalAddrV4,
  RemoteAddrV4,
  LocalAddrV6,
  RemoteAddrV6,
};

enum class WfpMatch { Equal, FlagsAllSet };

// One condition. Consecutive conditions on the SAME field are OR'd by WFP;
// different fields are AND'd. Every builder below keeps same-field conditions
// adjacent, because that OR is the only way to express "UDP or TCP" or "any of
// these LAN networks" in a single filter.
struct WfpCondition {
  WfpField field;
  WfpMatch match = WfpMatch::Equal;

  // Ports, protocol numbers, the interface LUID, the flags mask.
  // PORTS ARE IN HOST BYTE ORDER. FWP_UINT16 is not network order; passing
  // htons(53) yields a filter matching port 13568 and a leak that reads clean.
  uint64_t number = 0;

  uint32_t v4_addr = 0;   // host byte order
  uint8_t v4_prefix = 32;

  uint8_t v6_addr[16] = {};
  uint8_t v6_prefix = 128;

  std::wstring app_path;  // AppId only; resolved to a byte blob at Apply time
};

struct WfpFilterSpec {
  // Stable and greppable: this is the name that shows up in
  // `netsh wfp show filters`, and it is how a support report saying "the VPN
  // broke my printer" gets attributed to one line of this file.
  std::string name;
  WfpLayer layer;
  WfpSublayer sublayer;
  bool block = false;
  // libwfp weight CLASS, 0..15, not a raw 64-bit weight. BFE assigns the real
  // weight inside the band; raw weights collide unpredictably with other
  // providers. 15 = Max, 7 = Medium, 0 = Min.
  uint8_t weight = 0;
  std::vector<WfpCondition> conditions;
};

// The policy for one state. Deterministic and side-effect free.
std::vector<WfpFilterSpec> BuildFilterSet(WfpState state, const WfpConfig& cfg);

// ---------------------------------------------------------------------------
// Two questions about a spec, answered from its STRUCTURE.
//
// Apply() has to log both facts, and it used to answer them by comparing
// spec.name against string literals ("urnetwork-block-dns-v4",
// "urnetwork-permit-dns-host-resolver"). That is the wrong axis twice over: a
// rename makes the log silently stop reporting the DNS window without failing
// anything, and — worse — a NEW filter with the same shape and a different name
// would open the window without the log ever mentioning it. The log line is the
// only evidence outside the filter engine that machine-wide plaintext DNS was
// open, so it has to key off what the filter DOES.
//
// Declared here rather than kept private so the selftest asks these questions of
// the same functions Apply() does.
// ---------------------------------------------------------------------------

// A port-53 PERMIT in the DNS sublayer that a query from ANY process on this
// machine can match, and that is scoped to NO interface: address-scoped, not
// narrowed by app id, not narrowed by the loopback flag, not pinned to the tun.
//
// That is exactly what makes filter 9b machine-wide — the query our own name
// resolution produces is issued by Dnscache inside svchost.exe, so identity
// cannot narrow it, and 9b carries no interface condition either. Filter 10 (the
// tunnel's resolvers) is the same shape PLUS an IP_LOCAL_INTERFACE condition, so
// it is deliberately NOT machine-wide and this returns false for it.
bool IsMachineWideDnsPermit(const WfpFilterSpec& f);

// The DNS sublayer's hard block on remote port 53 (filter 12), at either ALE
// connect layer. Keyed on "blocks, in the DNS sublayer, on remote port 53", so
// the LLMNR/mDNS/NetBIOS blocks — same sublayer, same action, different ports —
// do not count.
bool IsDnsPort53Block(const WfpFilterSpec& f);

// The "armed across reboot" set (persistent + boot-time block-all).
//
// THIS BUILD NEVER INSTALLS IT. It is here so the purge path knows what to
// look for and so the shape is reviewable, and it is gated off because the
// research's load-bearing claim — that a BOOT-TIME filter honours the provider
// serviceName gate the way a persistent object does — IS UNVERIFIED, and
// verifying it needs an elevated run plus reboots in a VM. Persistent objects
// are documented to be enabled at boot only when their provider names no
// service, or names an auto-start one; boot-time filters are applied by
// netio/tcpip from the BFE BootTime policy key BEFORE BFE consults the SCM, so
// they may not be gated at all. Shipping a persistent block on that assumption
// risks the exact failure this whole design exists to prevent: a machine
// permanently blocked by a service that no longer exists. A persistent block
// orphaned by a crash is worse than the leak it prevents.
std::vector<WfpFilterSpec> BuildPersistentFilterSet();

// ---------------------------------------------------------------------------

// Every public member is INTERNALLY SYNCHRONISED, and that is a requirement
// rather than a courtesy: the connecting-window watchdog has to be able to
// narrow the policy back to Armed from its own thread while the thread that
// opened the window is still inside a connect attempt. Those two can genuinely
// race — a hung attempt is exactly when the watchdog fires — and Apply() mutates
// a filter-id list and a BFE session handle, so an unsynchronised second caller
// would leak filters or delete ids twice.
//
// It cannot be the caller's lock instead: TunnelController::mutex_ is HELD for
// the whole of a connect attempt, so a watchdog that took it would be blocked by
// precisely the hang it exists to bound.
class WfpPolicy {
 public:
  WfpPolicy() = default;
  ~WfpPolicy();
  WfpPolicy(const WfpPolicy&) = delete;
  WfpPolicy& operator=(const WfpPolicy&) = delete;

  WfpState State() const;

  // Install the filter set for `state` in ONE transaction: the previous set is
  // deleted and the new one added atomically, so there is no window in which
  // either everything is blocked or everything leaks. Off tears the whole
  // session down. Returns false and leaves the PREVIOUS state in force on
  // failure; LastError() says why.
  //
  // Needs FWPM_ACTRL_ADD / ADD_LINK on the engine, i.e. LocalSystem or an
  // elevated administrator. Unelevated it fails at FwpmEngineOpen0 with
  // ERROR_ACCESS_DENIED and reports so — the same shape as step 1/8's wintun
  // failure, and the same reason rpc-only mode can never reach it.
  bool Apply(WfpState state, const WfpConfig& cfg);

  // Tear the policy down and close the session. Equivalent to Apply(Off, {}).
  void Revert();

  // By value, not by reference: the string is guarded, and handing out a
  // reference to it would hand out an unsynchronised read of guarded state.
  std::string LastError() const;

  // How many filters are in force right now (0 when Off). Reported in the log
  // and by the selftest.
  size_t FilterCount() const;

  // Startup purge by provider GUID, mirroring SweepOrphanedTunnel's contract
  // and running in the same slot. Deletes every filter, sublayer and provider
  // carrying either of our provider GUIDs, in one transaction, filters first.
  // Returns how many objects were found (whether or not they were removed).
  //
  // remove=false OBSERVES ONLY. That is what unelevated rpc-only mode uses: a
  // mode whose entire promise is "this will not touch your network" must not
  // open by rewriting the filter engine, and the deletion needs a privilege it
  // does not have anyway. Reporting an orphan it cannot clean is still worth
  // doing — it tells the owner to run `urnetworkd revert` elevated.
  //
  // With a dynamic session there should never BE anything to find. Finding
  // something means a build once installed static or persistent objects, which
  // is the case belt-and-braces exists for.
  static int SweepOrphanedObjects(bool remove);

  // The provider/sublayer GUIDs as registry-style text, for logs and for the
  // owner's `netsh wfp show filters` diff.
  static std::vector<std::string> ObjectGuidsText();

 private:
  bool OpenEngine();
  void CloseEngine();
  // The bodies of Apply/Revert, with mutex_ already held. Apply(Off) is defined
  // as Revert(), so without this split the one would re-lock the other.
  bool ApplyLocked(WfpState state, const WfpConfig& cfg);
  void RevertLocked();

  mutable std::mutex mutex_;
  void* engine_ = nullptr;  // HANDLE; kept opaque so this header stays clean
  WfpState state_ = WfpState::Off;
  std::vector<uint64_t> filterIds_;
  std::string lastError_;
};

}  // namespace urnw
