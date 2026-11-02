// Orchestrates one tunnel session, mirroring the macOS PacketTunnelProvider:
// build the NetworkSpace + DeviceLocal from the app's config, start the mTLS RPC
// listener the app's DeviceRemote dials, bring up the wintun adapter, apply
// network settings, wire the packet pump, and keep R1 egress current. While
// there is no tunnel session it can instead run a provider-only DeviceLocal
// (StartProvider) that touches none of that and never coexists with a session.
//
// Thread-safety: Start/Stop are serialized by the ControlServer (single client).
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#include "AppLogHandles.h"
#include "CoalescedWorker.h"
#include "EgressMonitor.h"
#include "FlowOwner.h"
#include "LogUpload.h"
#include "NetworkChangeNotify.h"
#include "NetworkConfig.h"
#include "NetworkCountry.h"
#include "PacketPump.h"
#include "Protocol.h"
#include "Sdk.h"
#include "SplitTunnelClient.h"
#include "TunnelWatchdog.h"
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
  //   StartMode::Tunnel  — RPC-ready Preparing; capture activates asynchronously
  //                        after current-destination provider proof.
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
  // (~7.7 s worst case; see StopBudget.h, and note that the machine's network is
  // back inside the first ~1.4 s of that) whether or not the SDK cooperates, and
  // the machine's routes, DNS and firewall policy are given back BEFORE any part
  // of the teardown that can block on the network is attempted. When the SDK
  // half does not finish in budget it is abandoned rather than waited on. That
  // permanently commits the process to leaving by TerminateProcess
  // (TeardownAbandoned(), read by main()) and TEMPORARILY blocks a new start —
  // temporarily, because the abandoned worker's gate is retained, so StartLocked
  // re-reads it and lets the start through the moment the worker finishes. See
  // the two questions at the top of StopBudget.h.
  void Stop();

  // THE DEAD-TUNNEL FAILSAFE'S ONLY ENTRY POINT. Called by TunnelWatchdog, from
  // the watchdog's own thread, when a live tunnel has been proven unable to
  // carry traffic (see DeadTunnelVerdict.h for the verdict, and
  // TunnelWatchdogSettings.h for its thresholds). An account out of balance only
  // arrives here for a wedged sdk: a tunnel that cannot carry because the platform
  // refuses its contracts stays up, with its traffic in it (OutOfBalanceHold).
  //
  // It goes through StopLocked, the SAME teardown a user pressing Disconnect
  // takes, and there is deliberately NO second unwind path — a bespoke one is
  // how routes end up with nobody to revert them. The one thing it decides for
  // itself is `finalDisarm`, and it decides it with FailsafeFinalDisarm():
  //
  //   kill switch OFF -> the firewall policy is LIFTED and the machine gets its
  //                      internet back in the clear. Byte for byte a user
  //                      Disconnect. This is the whole point.
  //   kill switch ON  -> the policy NARROWS to Armed and the machine stays
  //                      blocked. Silently restoring the internet would break
  //                      the promise that setting makes; the state is surfaced
  //                      instead, and one click lifts it.
  //
  // IT NEVER RECONNECTS. That is absolute, and it is what makes thrash
  // structurally impossible: re-entering this state costs a full user-initiated
  // start plus a fresh grace period.
  //
  // BOUNDED like Stop(), and for a stronger reason: a wedged connect holds
  // mutex_ forever, and "give the internet back" cannot be conditional on a
  // lock. On the timeout path it reverts the routes lock-free AND — with the
  // kill switch off — drops the firewall policy directly, which Stop() used to
  // leave to process death.
  void FailsafeStop(DeadTunnelReason reason);

  // Called, OUTSIDE mutex_, whenever this controller changes state on its own
  // initiative rather than in reply to a request: deferred activation, the
  // dead-tunnel failsafe, and the egress binding moving under a live tunnel
  // (from a worker of its own, egressPush_). These have no control reply to
  // ride on.
  void SetOnStateChanged(std::function<void()> handler);

  // End the egress pushes for good, waiting out one in flight. For the control
  // server's stop, after it has cleared the handler and stopped the pipe that a
  // push writes to: nothing it stops may then be reached by a push.
  void StopStatePushes() { egressPush_.Stop(); }

  // Update the split-tunnel app set + mode (driver, if present). allowlist=false:
  // excludedPaths bypass the tunnel; allowlist=true: ONLY excludedPaths tunnel.
  bool SetSplitTunnel(const std::vector<std::string>& excludedPaths, bool allowlist);

  // The user's kill-switch preference, pushed from the app. Stored for the next
  // transition; it does NOT retroactively change the policy in force.
  //
  //   * While the tunnel is UP with a policy already installed nothing changes
  //     either way: the connected policy pins DNS to the tunnel's IPv4
  //     resolvers. IPv6 is not configured on Wintun and remains on the
  //     underlying network rather than being blackholed.
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

  // The account signed out (logout; Common/SignOut.h). Ends any session and
  // the provider-only device as Stop() does, deletes this machine's device
  // identity, and logs out what the sdk stored in `networkSpaceJson`'s space
  // (mirrors the macOS logout message clearing LocalState): the client
  // credential and instance a DeviceLocal persists when it starts, among the
  // rest, so nothing kept here belongs to the account that left. An empty
  // space, from an older app, deletes the identity alone.
  //
  // Timed like Stop() and StopProvider(): false, with nothing cleared, when the
  // session lock is not free within kStopLockBudget, and false when the sdk
  // state could not be logged out. The app keeps a refused logout owed and
  // sends it again; the stop_tunnel it sends first gives the machine back
  // without the lock.
  bool Logout(const std::string& networkSpaceJson);

  // The provider-only device (start_provider; Common/ProvideLifecycle.h). Keeps
  // providing while there is no tunnel session: a DeviceLocal built from the
  // persisted identity and the request's credentials, network space, provider
  // transport policy and provide mode — and nothing else. No wintun adapter, no
  // route, no DNS entry, no firewall policy, no active marker and no device RPC
  // listener, so this machine's own traffic is routed exactly as it would be
  // without URnetwork, and no client can drive the device.
  //
  // Refused (false, `error` set) while a tunnel session exists or is starting,
  // while the kill switch's armed floor is in force, while an abandoned
  // teardown still holds a device under this identity, and for a mode that does
  // not provide while disconnected (provide::ProviderStartRefusal) — every
  // refusal before anything is built. The same request again keeps the running
  // device and only applies the mode (proto::SameProviderDevice); any other
  // request replaces it. Every teardown retires it (StopLocked: the first SDK
  // teardown, right after the machine is given back), and every bring-up opens
  // with a teardown, so a Connect never runs two devices under one identity.
  bool StartProvider(const proto::StartProvider& request, std::string& error);
  // stop_provider: retire the provider-only device and touch nothing else — no
  // tunnel, no firewall policy. False only when the session lock could not be
  // taken in budget.
  bool StopProvider();
  // get_provider_stats (Protocol.h ProviderStats): the provider-only device's
  // statistics, for the screens a session's DeviceRemote feeds. Never blocks on
  // the session lock or the device, for Status()'s reason: it is served on the
  // control pipe, and every later get_state would queue behind it. It copies
  // the client id, the peer count, the extender status and the extender
  // setting the build and the listeners left, and reads the device's
  // ContractViewController, whose getters answer from its own sampled state
  // and never call into the device. `available` is false while none runs.
  proto::ProviderStats ProviderStats();
  // The Connect page's Extender switch while there is no session
  // (set_provide_extender). Writes the provider extender setting where
  // provide::ExtenderSettingTargetFor says: through the provider-only device
  // (persisted in its space, applied at once, and in the next
  // get_provider_stats), a session's device that came up meanwhile, or with
  // neither the space the last device ran in, which the next start reads.
  // Timed like StartProvider: a bring-up holding the session lock refuses it.
  // False with `error` when nothing took the write.
  bool SetProvideExtender(bool on, std::string& error);
  // The lag.dev speed tests switch while there is no session
  // (set_measurements_enabled, lag.dev/PLAN.md C4). Writes the measurements
  // setting where SetProvideExtender writes the provide extender setting, by
  // the same rule and under the same budget: the provider-only device (applied
  // at once, and in the next get_provider_stats), a session's device, or the
  // space the last device ran in. False with `error` when nothing took it.
  bool SetMeasurementsEnabled(bool on, std::string& error);
  // reset_extenders (Protocol.h ResetExtenders; connect EXTENDER.md E7): the
  // app reset its own space and hands the reset's id here. Applied to the
  // space spaceManager_ holds under the request's key, which is the one object
  // the session's device and the provider-only device run in (networkSpace_
  // and providerSpace_ are the manager's own for the key), so one call resets
  // both: the space's extender network client and node restart and relearn as
  // on a first run, and their live extender paths keep running. The result's
  // `reset` says whether the space was held and the reset new to it; with no
  // manager or no such space nothing is held, and the next import of the
  // space applies the reset its values carry.
  //
  // The session lock is taken in budget, for StartProvider's reason, and only
  // to look the space up: the reset joins the space's extender network client,
  // so it runs with the lock released. Not ok, with `error`, when the lock was
  // not free -- `busy` then, which the app answers by sending the reset again
  // once the operation holding the lock ends (Common/ExtenderReset.h) -- or
  // when the reset failed.
  struct ExtenderResetResult {
    bool ok = false;
    bool busy = false;
    bool reset = false;
    std::string error;
  };
  ExtenderResetResult ResetExtenders(const proto::ResetExtenders& request);
  // set_ingress_dns (Protocol.h SetIngressDns; sdk control_doh.go): the
  // connection region and the bootstrap DNS-over-HTTPS servers the app's space
  // holds. Written through the space's own setters (setIngressCountryCode,
  // setControlDohUrls) into the space spaceManager_ holds under the request's
  // key, the one object the session's device and the provider-only device run
  // in, where they apply in place: the space's DoH cache is swapped, and a
  // connection attempt resolves URnetwork's own names through them from its
  // next lookup, without a restart. The result's `held` says whether the space
  // was held; with no manager or no such space nothing is written, and the
  // next import of the space carries the values.
  //
  // The session lock is taken in budget, for StartProvider's reason, and only
  // to look the space up: the setters join the lookups in flight on the cache
  // they replace, so they run with the lock released. Not ok, with `error`,
  // when the lock was not free -- `busy` then, which the app answers by
  // sending the values again once the operation holding the lock ends
  // (Common/IngressDnsSync.h) -- or when the space refused the values.
  //
  // Nothing here logs the region or the servers: where a user says they
  // connect from, and which resolvers they can reach, is theirs.
  struct IngressDnsResult {
    bool ok = false;
    bool busy = false;
    bool held = false;
    std::string error;
  };
  IngressDnsResult SetIngressDns(const proto::SetIngressDns& request);

  // The network country the app read (Common/NetworkCountry.h; open bug P052):
  // the country of the mobile broadband network carrying the default route, ""
  // for none. The extender dials of this process's devices front with that
  // country's spoof list while the extender hint cannot be fetched.
  //
  // urnet::setNetworkCountryCode is process-wide inside this service's sdk, so
  // one call reaches the tunnel session's device and the provider-only device
  // alike, in place from their next extender dial. start_tunnel and
  // start_provider apply theirs before the space is imported (StartLocked,
  // StartProvider), so a new device's first dials already have it; the app's
  // set_network_country brings every later change. Normalized on the way in
  // (netcountry::Normalized) and logged when it changes.
  //
  // No session lock: a connect wedged inside the sdk holds mutex_ for as long as
  // the process lives, and the app's push must not queue behind it. Its own
  // lock, innermost, held across the one store into the sdk.
  void SetNetworkCountry(const std::string& code, const std::string& source);
  // The country in force, none until the app first sends one: what the log
  // feedback uploads says ([app][network-country], ServiceDiagnostics). Under
  // the same lock, never mutex_.
  std::optional<netcountry::Reading> NetworkCountry();

  // The kill-switch preference the app last sent (start_tunnel or
  // set_kill_switch), for the diagnostic lines: the firewall state a status
  // reports does not say it, and "on" over a firewall that is off is the
  // difference between a guarantee and a hope. Lock-free, like killSwitch_.
  bool KillSwitchPreference() const { return killSwitch_.load(); }

  // upload_logs (Protocol.h; Common/LogUpload.h carries the lifecycle): "send
  // feedback with logs" uploads this service's glog files whether or not a
  // tunnel runs. The sdk's UploadLogs runs on the session's device, else on the
  // provider-only device, else on a standalone device built from the request's
  // credentials exactly as the provider-only device is (provide mode never,
  // and no adapter, route, DNS entry, firewall policy or listener). That one is
  // retired once its upload reports or after
  // logupload::kStandaloneDeviceMaxLifetime (a waiter thread that owns only
  // the device's slot), and before any other device under this identity is
  // built (every teardown, start_provider, the next upload).
  //
  // The sdk's call (the zip, then the post started) runs on the upload's own
  // thread (logupload::Flight), never under the session lock; it holds the
  // device's handle, and the device stays alive until the call returns (each
  // teardown hands it to the flight while the call is on it). One upload at a
  // time: `busy` while one is in flight. Refused too when the session lock is
  // not free in budget, and for a standalone device while a held device or a
  // restart is pending; the app then falls back to its DeviceRemote. Returns
  // once the upload is admitted, with `carrier` naming the device
  // (logupload::ToString) and `uploadId` the id status reports its outcome
  // under; the service pushes its status when the upload ends. `noteCarrier`
  // gets the carrier's name once the device is chosen and before the upload's
  // thread starts the zip, under the session lock, so the line it writes is in
  // this upload (ControlServer passes ServiceDiagnostics::NoteLogUpload).
  //
  // `appLogFiles` are the app's own log files, opened as the app
  // (Common/AppLogFiles.h): the zip carries them under app/, and they are
  // closed once the sdk's call has read them, or when the request ends without
  // one.
  struct LogUploadResult {
    bool ok = false;
    bool busy = false;
    int64_t uploadId = 0;
    std::string carrier;
    std::string error;
  };
  LogUploadResult UploadLogs(const proto::UploadLogs& request,
                             const std::function<void(std::string_view)>& noteCarrier,
                             AppLogHandles appLogFiles);

  // THE STATUS THE APP DECIDES ON, and it must never block.
  //
  // The app's whole connect/disconnect lifecycle now hangs off one get_state per
  // gesture (Common/ConnectAction.h): "is there a tunnel right now" is answered
  // here, by the process that owns the routes, instead of being inferred on the
  // far side from a pointer. That makes two properties load-bearing.
  //
  //   NON-BLOCKING. A get_state that queues behind a wedged connect is useless
  //   exactly when it matters — the same argument StopBudget.h makes about Stop.
  //   This takes NO session lock: it copies a snapshot published at every write
  //   site, plus the two lock-free publishers below.
  //
  //   COHERENT. Lock-free by CONSTRUCTION, not by omission. It used to read
  //   state_, the netConfig_ unique_ptr, wfp_'s state and two std::strings with
  //   no synchronisation at all against StartLocked/StopLocked — the scalar
  //   reads were races, and the string reads could observe a buffer being freed
  //   underneath them. A whole-snapshot mirror also rules out combinations that
  //   were never true at any instant (state=up with routes=false).
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
  // THE ONLY WAY state_ IS WRITTEN, so that the lock-free mirror the ~1 Hz
  // heartbeat reads cannot drift from the real state.
  //
  // The heartbeat CANNOT compose a status: that reads session state under
  // mutex_, and a connect attempt wedged inside the SDK holds mutex_ forever
  // (see the connecting-window note below) — which is exactly the state a
  // heartbeat most needs to be able to record. Publishing on every write is
  // what lets a lock-free reader be correct rather than merely non-blocking.
  // The same discipline now backs Status(); see ComposeStatusLocked.
  //
  // Nothing here may block: this runs on the teardown path ahead of the route
  // revert. The glog flush that belongs to a transition lives at the RPC
  // boundary instead (ControlServer::PushState) for exactly that reason.
  //
  // Caller holds mutex_.
  void SetStateLocked(proto::TunnelState next);
  // Build a status from the objects that OWN each fact — the old Status() body,
  // unchanged, minus the two lock-free publishers Status() overlays live.
  // Caller holds mutex_.
  proto::TunnelStatus ComposeStatusLocked();
  // Compose, publish to the mirror Status() serves, and return it. Every
  // internal `return Status()` is this: the composition is what the caller
  // wants AND the publish the lock-free reader needs, so the two cannot drift.
  // Caller holds mutex_.
  proto::TunnelStatus StatusLocked();
  // The same publish with the value discarded, for the write sites that change
  // a status-visible fact without composing one. THE RULE: anything that moves
  // state_, netConfig_, wfp_'s state, startMode_, rpcHostPort_, error_ or
  // upSinceMillis_ must be followed by a publish before mutex_ is released.
  // SetStateLocked and ApplyWfpLocked are the two funnels that cover most of
  // it; the rest are named at their call sites. Caller holds mutex_.
  void PublishStatusLocked();
  // The same publish for the three paths that request machine-state changes
  // WITHOUT mutex_ — the connecting watchdog narrowing the DNS window, and
  // Stop()/FailsafeStop() escaping a wedged lock through CrashRevert. They
  // cannot compose a status (that reads session state), so they patch the two
  // ownership/application flags, not independently observed kernel state. The
  // app's disconnect decision reads these flags even during a wedged teardown.
  //
  // `routesReverted` means CrashRevert was requested, not verified. The watchdog
  // passes false because it only ever moves the firewall.
  void RepublishMachineFactsLockFree(bool routesReverted);
  proto::TunnelStatus StartLocked(const proto::StartTunnel& config);
  // Provider-proof-gated machine transaction: pump/split routing, firewall,
  // routes/DNS, then connected policy. Caller holds mutex_. Waiting means proof
  // changed and any applied stages received rollback; Halted means a debug stop ran.
  CaptureResult BringUpTunnelLocked(CaptureReadiness& readiness,
                                    CaptureTicket ticket, const char*& step);
  // Terminal callback from the preparing watchdog. The ticket belongs only to
  // this session; cancellation is published before Stop waits for mutex_.
  void ActivateCapture(std::shared_ptr<CaptureReadiness> readiness,
                       CaptureTicket ticket);
  void WatchForCaptureLocked();
  void CancelCapture();
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
  // Phase 1 of StopLocked: request route, DNS, resolver-cache, marker and policy
  // cleanup before SDK teardown. Releases configuration ownership but does not
  // independently verify OS removal. Caller holds mutex_.
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
  // Full path of the UI process, for the bootstrap/connected app permit (WfpConfig::
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
  // Steps 3 and 4 of a bring-up, shared by the tunnel session and the
  // provider-only device so both come from one copy of the identity rules:
  // the space imported into the service's own storage, and a DeviceLocal with
  // the persisted key material (a new identity persisted only when none was
  // stored), sized at the host's memory tier, with enable_rpc=false — a device
  // gets a listener only from setRpcServer, which the provider-only device
  // never calls. `who` prefixes the log line. Both throw on failure. Caller
  // holds mutex_.
  urnet::NetworkSpace ImportNetworkSpaceLocked(const std::string& networkSpaceJson);
  urnet::DeviceLocal NewDeviceLocked(const urnet::NetworkSpace& space,
                                     const std::string& byJwt,
                                     const std::string& deviceDescription,
                                     const std::string& deviceSpec,
                                     const std::string& appVersion,
                                     const std::string& instanceId, const char* who);
  // Retire the provider-only device: clear and publish its status, then hand
  // the device and its space to a bounded worker that closes them
  // (RunBounded, AbandonHazard::HoldsSessionDevice — while that worker is
  // outstanding a second device would run under the same identity, so it
  // refuses a start exactly like an abandoned session teardown). A no-op
  // without one. Caller holds mutex_.
  void RetireProviderDeviceLocked();
  // Read the provider-only device's live tier and network-key bit into the
  // fields ComposeStatusLocked reports. SDK calls, so never from a publish
  // path: only after the device is built or re-moded. Caller holds mutex_.
  void ReadProviderFactsLocked();
  // Open what get_provider_stats reads on a freshly built provider-only
  // device: its client id, a ContractViewController (the controller the app
  // opens on the DeviceRemote, sampling once a second, whose extender series
  // is the extender plot's), a network peers listener that keeps the client
  // count, and an extender status listener with the setting read beside it
  // (the Earnings extender row and its plot's gate). Best effort: providing
  // does not depend on it, a failure leaves the statistics unavailable, and a
  // failure of the extender reading alone leaves the role unreported. Caller
  // holds mutex_.
  void OpenProviderStatsLocked();
  // Read the provider-only device's extender status and setting again after
  // set_provide_extender wrote it, into the copies get_provider_stats answers,
  // so the next answer carries the write rather than waiting for the status
  // listener's next push. Nothing when the role's reading never opened. SDK
  // calls; caller holds mutex_.
  void RefreshProviderExtenderLocked();
  // Read the provider-only device's measurements setting again after
  // set_measurements_enabled wrote it through this device, into the copy
  // get_provider_stats answers. Nothing when the role's reading never opened.
  // An SDK call; caller holds mutex_.
  void RefreshProviderMeasurementsLocked();
  // Tell the provider-only device the network moved, as the tunnel session's
  // device is told by TunnelWatchdog's sampler, which EgressMonitor feeds. An
  // observe-only EgressMonitor (it binds nothing, so the device's sockets
  // still follow the route table) feeds a NetworkChangeNotifier, whose own
  // thread calls networkChanged() once per burst and networkQualityChanged()
  // for a Wi-Fi signal change. Best effort, like the statistics. Caller holds
  // mutex_.
  void WatchProviderNetworkLocked();
  // The standalone device a log upload runs on while neither the session's nor
  // the provider-only device exists (UploadLogs). Shared by this controller and
  // the waiter thread that retires it, so either may take the device and the
  // waiter needs nothing of this object: it owns a share of the slot, never
  // `this`. The slot's mutex is innermost and never held across a call into
  // the device; the upload callback only sets uploadReported under it.
  struct LogUploadDevice {
    std::mutex mutex;
    std::condition_variable reported;
    bool uploadReported = false;
    std::unique_ptr<urnet::DeviceLocal> device;
    std::optional<urnet::NetworkSpace> space;
    // the flight whose call may still be on the device when it is closed
    std::shared_ptr<logupload::Flight> flight;
  };
  // Takes the device out of `slot` and closes it on a bounded worker, as the
  // provider-only device's retire does (AbandonHazard::HoldsSessionDevice). A
  // no-op once taken. Static: the waiter calls it after this object may be
  // gone.
  static void CloseLogUploadDevice(const std::shared_ptr<LogUploadDevice>& slot);
  // Retires the standalone log upload device, if any. Caller holds mutex_.
  void RetireLogUploadDeviceLocked();
  void PushExcludedToDriver(const std::vector<std::string>& paths, bool allowlist);
  // Follow a binding that moved: the interface Status() reports (and pushes, so
  // the app re-pins its own sdk), and the driver's physical interface and
  // source address. Runs from the egress monitor's change callback, on a system
  // worker thread, and from Start()'s baseline on the starting thread.
  void OnEgressChanged(EgressInterfaces egress);
  // Shared by both; the caller holds splitMutex_.
  void PushPhysicalAddressesLocked(const EgressInterfaces& egress);
  // Invoke onStateChanged_ if one is set. MUST be called with mutex_ RELEASED:
  // the handler pushes a status, which takes mutex_ to read it.
  void NotifyStateChanged();
  // Drop the firewall policy from a path that holds NO lock, when the kill
  // switch permits it. Shared by Stop()'s and FailsafeStop()'s timeout paths;
  // `who` names the caller in the log. See the definition for why relying on
  // process death to remove the filters is not good enough here.
  void DropFirewallOnEscape(const char* who);

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
  // Exact identity of the DeviceLocal and the per-start mTLS generation. Both
  // are echoed in live status so an app may adopt only its own credentials;
  // host:port alone is reusable and therefore is not an identity.
  std::string activeInstanceId_;
  std::string rpcSessionId_;
  int64_t upSinceMillis_ = 0;

  std::filesystem::path storageDir_;

  // Per-flow app attribution (FlowOwner.h), fed to the SDK via
  // device_->setFlowOwnerLookup at step 4/8. Like wfp_, DELIBERATELY OUTLIVES
  // individual sessions — Start() is idempotent, so a reconnect does not spin
  // up a second worker thread — and DECLARED BEFORE device_ so that
  // destruction order (members die in REVERSE declaration order) keeps it
  // alive until AFTER every DeviceLocal that might still be calling into its
  // lookup lambda is gone. The opposite ordering trace_ uses below is not a
  // contradiction: trace_ holds a raw pointer INTO device_ and so must die
  // BEFORE it; flowOwner_ is called BY device_ and so must die AFTER it.
  FlowOwner flowOwner_;

  // SDK objects. NetworkSpaceManager persists across sessions; the rest are
  // per-session.
  std::optional<urnet::NetworkSpaceManager> spaceManager_;
  // The key of the space ImportNetworkSpaceLocked imported last: the space the
  // last device ran in, which spaceManager_ keeps after that device is gone and
  // the next start of either device imports again. set_provide_extender writes
  // into it when no device runs. Guarded by mutex_.
  std::optional<urnet::NetworkSpaceKey> lastSpaceKey_;
  std::optional<urnet::NetworkSpace> networkSpace_;
  std::optional<urnet::DeviceLocal> device_;
  // The provider-only device (StartProvider), its space and the request it was
  // built from. Separate slots from networkSpace_ and device_, which keep their
  // one meaning — the tunnel session's — for every check that reads them (the
  // capture precondition, the teardown, the published identity). Never engaged
  // together with a tunnel session. providerTier_ and providerNetworkKey_ are
  // what ReadProviderFactsLocked last read off the device. Guarded by mutex_.
  //
  // The device lives in a unique_ptr because the network notifier's calls hold
  // a raw pointer to it: its address does not move when the retire hands it to
  // the teardown worker, which ends the notifier (a join) before it closes the
  // device.
  std::optional<urnet::NetworkSpace> providerSpace_;
  std::unique_ptr<urnet::DeviceLocal> providerDevice_;
  proto::StartProvider providerRequest_;
  int64_t providerTier_ = 0;
  bool providerNetworkKey_ = false;
  // The provider-only device's network watch (WatchProviderNetworkLocked).
  // Guarded by mutex_; retired with the device.
  std::unique_ptr<EgressMonitor> providerEgress_;
  std::unique_ptr<NetworkChangeNotifier> providerNetwork_;
  // The provider extender role's last status (EXTENDER.md N2): what its
  // listener pushed last, or what the statistics' opening read before the
  // first push. Shared with that listener, never the controller, as the peer
  // count is, and locked inside its own two calls only.
  class LatestExtenderProvideStatus {
   public:
    void Store(std::optional<urnet::ExtenderProvideStatus> status) {
      std::scoped_lock lock(mutex_);
      status_ = std::move(status);
    }
    std::optional<urnet::ExtenderProvideStatus> Load() const {
      std::scoped_lock lock(mutex_);
      return status_;
    }

   private:
    mutable std::mutex mutex_;
    std::optional<urnet::ExtenderProvideStatus> status_;
  };
  // What get_provider_stats reads (ProviderStats), under a mutex of its own,
  // never mutex_: the request is served on the control pipe and must not queue
  // behind a bring-up holding the session lock. Innermost — taken under mutex_
  // by the build and the retire, alone by ProviderStats() — and never held
  // across a call into the device. providerClients_ and providerExtender_ are
  // shared with the peers and extender status listeners, which can still be
  // running after the retire has dropped them. providerExtenderSetting_ is the
  // provide extender setting, read when the statistics open and again after
  // set_provide_extender writes it through this device
  // (RefreshProviderExtenderLocked); a session's device, the only other
  // writer, never runs beside this one.
  std::mutex providerStatsMutex_;
  std::optional<urnet::ContractViewController> providerStatsVc_;
  urnet::Sub providerPeersSub_;
  std::shared_ptr<std::atomic<int64_t>> providerClients_;
  std::string providerClientId_;
  urnet::Sub providerExtenderSub_;
  std::shared_ptr<LatestExtenderProvideStatus> providerExtender_;
  bool providerExtenderSetting_ = false;
  // The lag.dev measurements setting, read beside providerExtenderSetting_ and
  // again after set_measurements_enabled writes it through this device
  // (RefreshProviderMeasurementsLocked). On by default, as the sdk's.
  bool providerMeasurementsSetting_ = true;
  // The standalone log upload device's slot (LogUploadDevice), empty while
  // none was built. A third device slot, never engaged beside device_ or
  // providerDevice_: both are built only after RetireLogUploadDeviceLocked.
  // Guarded by mutex_.
  std::shared_ptr<LogUploadDevice> logUpload_;
  // The log upload in flight, shared with its thread, its callback and the
  // teardown workers, which hold nothing else of this object
  // (logupload::Flight: its own lock). Its finish hook pushes the status; the
  // destructor clears it before anything it uses is gone.
  std::shared_ptr<logupload::Flight> logUploadFlight_;

  // The network country last applied to this process's sdk
  // (SetNetworkCountry), empty until the app first sends one. Guarded by
  // networkCountryMutex_ alone.
  std::mutex networkCountryMutex_;
  std::optional<netcountry::Reading> networkCountry_;

  // Native tunnel plumbing.
  std::unique_ptr<Wintun> wintun_;
  std::unique_ptr<WintunAdapter> adapter_;
  std::unique_ptr<NetworkConfig> netConfig_;
  std::unique_ptr<EgressMonitor> egress_;
  std::unique_ptr<PacketPump> pump_;
  // Accessed with atomic_load/store so Stop can cancel a pending transaction
  // without first taking the potentially blocked session mutex.
  std::shared_ptr<CaptureReadiness> captureReadiness_;
  std::atomic<uint64_t> stopGeneration_{0};
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
  //
  // ATOMIC, because the failsafe's lock-free escape has to read it while a
  // wedged connect holds mutex_ — and what it decides there is whether this
  // machine gets its internet back or stays blocked, which is the one question
  // that must never be answered by waiting for a lock. Every other read and
  // write still happens under mutex_; std::atomic<bool> is a superset, so
  // nothing else changes.
  std::atomic<bool> killSwitch_{false};

  // --- what the app is told about the last teardown -------------------------
  //
  // A bare const char* into the string literals DeadTunnelVerdict.h defines, for
  // Heartbeat.h's reason: it is published from the watchdog thread and read from
  // the RPC thread, and pointing at static storage means the read side needs no
  // allocation, no lock and no lifetime rule.
  std::atomic<const char*> lastStopReason_{kStopReasonNone};

  // --- the snapshot Status() serves without a session lock -------------------
  //
  // Written under mutex_ (by PublishStatusLocked, at every write site), read
  // under statusMirrorMutex_ ALONE. That lock is never held across anything
  // that can block — a struct copy in, a struct copy out — and it is always
  // innermost, so it cannot participate in the wedge it exists to survive.
  // A whole-snapshot mirror rather than per-field atomics on purpose: the app
  // reads (IsSessionLive(state), routes_installed) as a PAIR, and per-field
  // publication would let it observe a combination that was never true.
  //
  // The two lock-free publishers above (lastStopReason_, the watchdog's armed
  // flag) are deliberately NOT mirrored: Status() overlays them live, so they
  // are still correct on the one path where they matter most — a status served
  // while a failsafe teardown is in flight, i.e. before the publish that
  // teardown will eventually do.
  mutable std::mutex statusMirrorMutex_;
  proto::TunnelStatus statusMirror_;
  // urnet::version(), READ ONCE AT CONSTRUCTION AND NEVER AGAIN.
  //
  // It looks like a constant and it is not: it is a cgo call into the Go
  // runtime. Composing a status now happens inside SetStateLocked, which runs
  // on the teardown path AHEAD of the route revert and whose contract is that
  // nothing in it may block — and Status() itself has to stay answerable while
  // a connect attempt is wedged inside the SDK, which is exactly the state in
  // which a cgo call can fail to return. Putting an SDK re-entry on both of
  // those paths would undo the two properties they exist to have. Once, at
  // construction, when nothing is wedged.
  const std::string sdkVersion_;
  // upSinceMillis_'s mirror, kept separate so Status() can age it against a
  // live clock rather than serve the uptime as of the last publish.
  std::atomic<int64_t> upSinceMirror_{0};
  // The session monitor's binding, kept separate for the same reason: written
  // by the start and by OnEgressChanged, whose system thread may not take
  // mutex_, and overlaid by Status() whenever the status reports a binding at
  // all. The mirror's copy is the one composed at the last publish, and the
  // egress moving publishes nothing, so on its own the app was told the
  // interface it had left.
  std::atomic<int64_t> liveEgressIndex4_{0};
  std::atomic<int64_t> liveEgressIndex6_{0};

  // The dead-tunnel failsafe. DECLARED LAST so that destruction order — members
  // die in reverse declaration order — stops its threads BEFORE device_ and the
  // pump they read, exactly as trace_ is declared after device_ for the same
  // reason. The explicit Stop() at the top of StopLocked is the real mechanism;
  // this is the belt to that braces.
  TunnelWatchdog deadTunnelWatchdog_;

  // Set once by ControlServer at wiring time; read on the failsafe path.
  std::mutex stateChangedMutex_;
  std::function<void()> onStateChanged_;
  // The resolvers actually handed to the tun, kept so the firewall's DNS permit
  // and the interface's DNS settings are built from the same list rather than
  // recomputed. One list per family; the v6 list is empty for a v4-only tunnel.
  std::vector<std::string> appliedResolvers_;
  std::vector<std::string> appliedResolversV6_;

  // Pushes a status when the egress binding moved under a live tunnel
  // (OnEgressChanged), so the app re-pins its own sdk now rather than at the
  // next transition. Never on the thread that saw the move: that is a system
  // thread EgressMonitor::Stop() waits for, and a push writes to the control
  // pipe, which blocks while the app is not reading — the watchdog's sampler
  // would be no better, since a sampler held there reads as an unresponsive sdk.
  // Moves coalesce into the last one. DECLARED LAST, so it is destroyed first
  // and a push in flight is waited out before anything it reads goes; the
  // control server stops it earlier still (StopStatePushes).
  CoalescedWorker egressPush_{[this] { NotifyStateChanged(); }};
};

}  // namespace urnw
