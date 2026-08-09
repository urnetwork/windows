// Orchestrates one tunnel session, mirroring the macOS PacketTunnelProvider:
// build the NetworkSpace + DeviceLocal from the app's config, start the mTLS RPC
// listener the app's DeviceRemote dials, bring up the wintun adapter, apply
// network settings, wire the packet pump, and keep R1 egress current.
//
// Thread-safety: Start/Stop are serialized by the ControlServer (single client).
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "EgressMonitor.h"
#include "NetworkConfig.h"
#include "PacketPump.h"
#include "Protocol.h"
#include "Sdk.h"
#include "SplitTunnelClient.h"
#include "WfpPolicy.h"
#include "WindowTrace.h"
#include "Wintun.h"

namespace urnw {

class TunnelController {
 public:
  TunnelController();
  ~TunnelController();

  // Bring a session up from the app's config. Stops any prior session first.
  // Returns a status; on failure state == Error with error set.
  //
  // config.mode selects what "up" means:
  //   StartMode::Tunnel  — all eight steps; rewrites routes and DNS.
  //   StartMode::RpcOnly — steps 2-5 only (no wintun adapter, so no elevation
  //                        needed), ending at the RPC listener. It returns
  //                        BEFORE step 6/8, the first call that touches the
  //                        machine's network, and reports state RpcOnly.
  proto::TunnelStatus Start(const proto::StartTunnel& config);

  // Force every subsequent Start into StartMode::RpcOnly regardless of what was
  // asked for. One-way and irreversible by design: `urnetworkd console
  // --rpc-only` calls this before the control pipe is served, so from then on
  // no client request of any shape can make this process reach step 6. The
  // downgrade is reported in the log and in TunnelStatus::mode, so a caller
  // that wanted a tunnel can see it did not get one.
  void ClampToRpcOnly();

  // DEBUG-ONLY STAGED BRING-UP (`urnetworkd console --stop-after=<N>`).
  //
  // Halt every subsequent Start after step N of 8, cleanly, through the ordinary
  // teardown. Set once from RunConsole before the control pipe is served, for the
  // same reason ClampToRpcOnly is: the flag has to be in force before any client
  // request can be served, not decided per request.
  //
  // It exists because StartLocked's 1/8 -> 8/8 was one uninterruptible sequence
  // once a client sent start_tunnel, and --rpc-only's stop-at-5 SKIPS step 1 — so
  // there was no way to create the wintun adapter and stop before step 6/8, the
  // first call that rewrites this machine's routes and DNS and the only code here
  // that has never run. Aborting by Ctrl+C instead is a race against that rewrite.
  //
  // IT ONLY EVER STOPS THINGS EARLIER. It enables nothing, it does not touch
  // startMode_, and it cannot lift the rpc-only clamp: in a clamped process
  // StartLocked still returns at the fence, so the two compose as a minimum (see
  // EffectiveStopStep in ConsoleArgs.h). `step` is clamped into 1..8 towards the
  // EARLIEST stop, because the one unacceptable reading of a nonsense value is
  // "run all eight".
  void SetStopAfterStep(int step);

  // Tear the tunnel down and restore the network.
  //
  // BOUNDED. It returns within roughly kStopLockBudget + kSdkTeardownBudget
  // (~3 s; see StopBudget.h) whether or not the SDK cooperates, and the
  // machine's routes, DNS and firewall policy are given back BEFORE any part of
  // the teardown that can block on the network is attempted. When the SDK half
  // does not finish in budget it is abandoned rather than waited on, which
  // latches TeardownAbandoned() — see the note there for what the two callers
  // who care then do.
  void Stop();

  // Update the split-tunnel app set + mode (driver, if present). allowlist=false:
  // excludedPaths bypass the tunnel; allowlist=true: ONLY excludedPaths tunnel.
  bool SetSplitTunnel(const std::vector<std::string>& excludedPaths, bool allowlist);

  // The user's kill-switch preference, pushed from the app. Stored for the next
  // transition; it does NOT retroactively change the policy in force.
  //
  //   * While the tunnel is UP with a policy already installed nothing changes
  //     either way: the connected policy is the leak fix (IPv6 blocked, DNS
  //     pinned to the tunnel's resolvers) and it applies whether or not the kill
  //     switch is on. Leak prevention is not a preference.
  //   * Turning it OFF while armed-and-disconnected lifts the block
  //     immediately, because the user asking for their network back is the one
  //     case where waiting for a transition is the wrong answer.
  //   * Turning it ON while the tunnel is UP and the policy is OFF RETRIES THE
  //     INSTALL IMMEDIATELY, and returns false if it still cannot be installed.
  //     "Decide it at the next transition" was wrong here and silently so: step
  //     6/8's install is non-fatal with the switch off, so this combination is
  //     reachable, and from it every StopLocked branch that could arm required a
  //     policy to already exist — leaving the drop unprotected with nothing that
  //     would ever retry. StopLocked's hadRoutes branch is the backstop.
  //   * Turning it ON while DISCONNECTED still does NOT arm. That is the line
  //     between a kill switch and lockdown, and this build stops at it
  //     deliberately — see StopLocked.
  bool SetKillSwitch(bool on);

  // Clear persisted auth/session state (mirrors the macOS logout message).
  void Logout();

  proto::TunnelStatus Status();

  // "Routes are installed right now" marker under the service storage root.
  // Written when the network config is applied and removed when it is reverted,
  // so the NEXT start can tell an orderly shutdown from a crash. Purely a
  // reporting aid — the machine's actual restore path is the wintun adapter
  // going away with the process (see NetworkConfig.h).
  static std::filesystem::path ActiveMarkerPath();
  static void SetActiveMarker(bool active);
  // True if a marker was left behind; clears it either way.
  static bool TakeActiveMarker();
  // True if a marker was left behind, WITHOUT clearing it. For a run that is
  // only observing — the unelevated rpc-only mode — which must not consume the
  // evidence that an earlier elevated run died with routes installed. Taking it
  // there makes the next real start report a clean exit it did not have, and
  // that report is the only way the owner learns a crash cost them their
  // network.
  static bool PeekActiveMarker();

 private:
  proto::TunnelStatus StartLocked(const proto::StartTunnel& config);
  // Steps 6-8: network settings, split tunnel, packet pump. Split out of
  // StartLocked so the destructive half of the sequence is a named unit with
  // its OWN precondition, checked against the stored mode rather than against
  // the caller's argument. StartLocked already returns before reaching it in
  // rpc-only mode; this is the second, independent gate, and the one a future
  // caller cannot get wrong. Caller holds mutex_; `step` is the diagnostic
  // cursor StartLocked reports on failure.
  //
  // Returns true when all three steps ran. False means --stop-after halted the
  // sequence at one of them AND THE TEARDOWN HAS ALREADY RUN — the caller must
  // not go on to report the session up. Reported as a return value rather than a
  // flag on the object so the caller cannot forget to look at it.
  bool BringUpTunnelLocked(const proto::StartTunnel& config, const char*& step);
  // The staged bring-up stop point. Returns false — instantly, before reading
  // anything — unless --stop-after was passed and this sequence has reached it.
  // When it has: log what step `step` left behind, run the ORDINARY teardown,
  // log what is actually left applied afterwards, and return true so the caller
  // returns out of the sequence.
  //
  // The teardown is StopLocked(finalDisarm=true), i.e. byte for byte the path a
  // user pressing disconnect takes — same revert, same resolver-cache flush, same
  // marker handling, same kill-switch rules. There is deliberately NO second
  // teardown path: a bespoke unwind is how routes end up with nobody to revert
  // them. `reached` names what this step left in place, for the log. Caller holds
  // mutex_.
  bool HaltAfterStepLocked(int step, const char* reached);
  // `finalDisarm` distinguishes "this session is ending" from "this session is
  // being replaced". A reconnect must NOT drop the firewall policy to Off in
  // the gap — that gap is exactly what a kill switch is for — so the internal
  // teardown inside StartLocked passes false and leaves the policy Armed, while
  // the public Stop() (user disconnect, service shutdown) passes true and lifts
  // it. Caller holds mutex_.
  void StopLocked(bool finalDisarm);
  // Phase 1 of StopLocked: give this MACHINE back — routes, DNS, the resolver
  // cache, the active marker and the firewall policy. Split out so the ordering
  // is a fact about the code rather than a comment: everything here is local,
  // cheap and measured in single-digit milliseconds, and NONE of it can block on
  // a network that has already failed. It therefore runs FIRST, before any part
  // of the teardown that talks to the SDK. Caller holds mutex_.
  //
  // Deliberately NOT wrapped in a budget. It is the thing whose completion the
  // budgets exist to guarantee; abandoning it would abandon the revert, which is
  // the opposite of the goal. If even this could wedge, the floor below it is
  // NetworkConfig::CrashRevert() from the console handler.
  void RevertMachineStateLocked(bool finalDisarm, bool hadRoutes);
  // Phase 2 of StopLocked: hand every per-session SDK and native object to a
  // worker that OWNS them and wait kSdkTeardownBudget for it. Returns false if
  // the budget expired and the worker was abandoned. Caller holds mutex_.
  bool TearDownSessionLocked();
  // Build the WFP config for the current session and push a state. Returns
  // false when the policy could not be installed; the caller decides whether
  // that is fatal. Caller holds mutex_.
  //
  // THE SINGLE CHOKE POINT FOR THE CONNECTING WINDOW. Every state change goes
  // through here, so the watchdog is armed by "the state we just applied is
  // Connecting" and cancelled by "it is not" — rather than by a pair of calls
  // some future path can forget half of.
  bool ApplyWfpLocked(WfpState state);
  // The parts of the WFP config that depend on NOTHING in the session: LAN, v6,
  // and this executable's path. Static for the reason spelled out on the
  // watchdog below — the armed policy has to be rebuildable from a thread that
  // holds no lock and can read no session state.
  static WfpConfig BaseWfpConfig();

  // --- the connecting window ------------------------------------------------
  //
  // WfpState::Connecting installs a MACHINE-WIDE plaintext-DNS permit (filter
  // 9b; it cannot be scoped to us, because our own resolution is issued by
  // Dnscache inside svchost.exe). Every path that enters it also leaves it —
  // StartLocked ends in Connected on success and in StopLocked -> Armed on
  // failure — but "every path that returns" is not the same as "every path",
  // and the case that has no return is the one that matters: a connect attempt
  // wedged inside the SDK holds mutex_ forever, so nothing else in this class
  // can run and the hole stays open for as long as the process lives.
  //
  // Hence a watchdog, and hence it runs on its OWN thread and takes NO lock of
  // this class. Taking mutex_ would block it on precisely the hang it exists to
  // bound. It only ever narrows (Connecting -> Armed), it rebuilds the armed
  // policy from BaseWfpConfig() alone, and WfpPolicy is internally synchronised
  // so it can race the connect thread's own Apply safely.
  void ArmConnectingWatchdogLocked();
  void CancelConnectingWatchdogLocked();
  // Full path of THIS executable, for the ALE_APP_ID self-exemption. Resolved
  // once: without it the machine can be armed, blocked and unable to reconnect.
  static std::wstring ServiceImagePath();
  // Full path of the UI process, for the Connected-only app permit (WfpConfig::
  // app_image_path). Empty when it cannot be found, which is a supported answer
  // — the permit is then simply not emitted.
  //
  // DERIVED FROM OUR OWN DIRECTORY, NEVER FROM THE RPC. The app could trivially
  // send its own path in StartTunnel and that would even be more accurate in a
  // split dev layout, but it would also mean an RPC peer choosing which
  // executable gets a firewall permit. The channel is mTLS on loopback and the
  // peer is our own app, so the risk is small — and the mitigation is smaller
  // still: URnetwork.exe ships in the same directory as urnetworkd.exe
  // (installer/Package.wxs puts both in INSTALLFOLDER), so the sibling lookup
  // is right for every shipped configuration and is not influenced by anything
  // outside this process.
  static std::wstring AppImagePath();
  // Load persisted DeviceLocalKeyMaterial blobs, or return nullopt on first run.
  std::optional<urnet::DeviceLocalKeyMaterial> LoadKeyMaterial();
  void PersistKeyMaterial(const urnet::DeviceLocalKeyMaterial& km);
  void PushExcludedToDriver(const std::vector<std::string>& paths, bool allowlist);
  // Re-point the driver at a new physical interface. Runs from the egress
  // monitor's change callback, on a system worker thread.
  void OnEgressChanged(EgressInterfaces egress);
  // Shared by both; the caller holds splitMutex_.
  void PushPhysicalAddressesLocked(const EgressInterfaces& egress);

  // TIMED, not plain. Stop() must be able to give up on acquiring it: this
  // class's own connecting-watchdog note (below) already establishes that a
  // wedged connect attempt holds this lock forever, and Stop() used to walk
  // straight into that with an infinite `scoped_lock` — before it had logged
  // anything, and before it had given the operator a single route back. Every
  // other caller still takes it with scoped_lock; std::timed_mutex is a
  // superset, so nothing else changes.
  std::timed_mutex mutex_;
  // Guards calls into splitTunnel_ ONLY, and is always the inner lock when both
  // are held. The egress change callback has to reach the driver WITHOUT
  // mutex_: StopLocked holds mutex_ while EgressMonitor::Stop() waits for that
  // very callback to return, so taking mutex_ there would deadlock the two
  // against each other. Correspondingly, nothing may hold splitMutex_ across a
  // call into the egress monitor.
  std::mutex splitMutex_;
  proto::TunnelState state_ = proto::TunnelState::Stopped;
  // The mode of the session being started / running. Set once at the top of
  // StartLocked and read by BringUpTunnelLocked's precondition and by Status();
  // both the reported state and the reported mode derive from this one value,
  // so they cannot disagree.
  proto::StartMode startMode_ = proto::StartMode::Tunnel;
  // Process-level clamp (see ClampToRpcOnly). Atomic and never cleared.
  std::atomic<bool> rpcOnlyClamp_{false};
  // Process-level staged bring-up stop point (see SetStopAfterStep). 0 = the
  // flag was never passed, which is the only value that lets a start run to
  // completion — so every path that fails to set it fails towards a NORMAL run,
  // and every path that sets it fails towards a shorter one. Atomic for the same
  // reason rpcOnlyClamp_ is: written once before the pipe is served, read on
  // every start.
  std::atomic<int> stopAfterStep_{0};
  std::string error_;
  std::string rpcHostPort_;
  int64_t upSinceMillis_ = 0;

  std::filesystem::path storageDir_;

  // SDK objects. NetworkSpaceManager persists across sessions; the rest are
  // per-session.
  std::optional<urnet::NetworkSpaceManager> spaceManager_;
  std::optional<urnet::NetworkSpace> networkSpace_;
  std::optional<urnet::DeviceLocal> device_;

  // Native tunnel plumbing.
  std::unique_ptr<Wintun> wintun_;
  std::unique_ptr<WintunAdapter> adapter_;
  std::unique_ptr<NetworkConfig> netConfig_;
  std::unique_ptr<EgressMonitor> egress_;
  std::unique_ptr<PacketPump> pump_;
  SplitTunnelClient splitTunnel_;
  std::vector<std::string> excludedPaths_;
  bool allowlist_ = false;

  // The window trace (URNETWORK_SDK_TRACE). Inert unless the variable is set.
  //
  // DECLARED AFTER device_ ON PURPOSE, so that even if some future path forgets
  // the explicit trace_.Stop() at the top of StopLocked, destruction order —
  // members die in reverse declaration order — joins the trace thread BEFORE
  // ~optional<DeviceLocal> runs. Belt and braces over a raw pointer into another
  // member.
  WindowTrace trace_;

  // The connecting watchdog's own state. Guarded by watchdogMutex_ and NEVER by
  // mutex_ — that separation is the whole reason the watchdog can fire while a
  // connect attempt is wedged holding mutex_. Nothing here reads session state.
  std::mutex watchdogMutex_;
  std::condition_variable watchdogWake_;
  std::thread watchdog_;
  bool watchdogCancelled_ = false;

  // The leak-prevention layer. Deliberately OUTLIVES individual sessions: the
  // interesting state is the gap between one session ending and the next
  // starting, and an object recreated per session cannot hold a policy across
  // it. Its destructor closes the dynamic WFP session, which is also what
  // removes every filter.
  WfpPolicy wfp_;
  // The last kill-switch preference the app pushed (start_tunnel or
  // set_kill_switch). Read on every transition.
  bool killSwitch_ = false;
  // The resolvers actually handed to the tun, kept so the firewall's DNS permit
  // and the interface's DNS settings are built from the same list rather than
  // recomputed.
  std::vector<std::string> appliedResolvers_;
};

}  // namespace urnw
