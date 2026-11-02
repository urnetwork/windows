// THE DEAD-TUNNEL FAILSAFE, AND THE NETWORK-CHANGE REACTION IT SITS BESIDE.
//
// ---------------------------------------------------------------------------
// THE FAILURE THIS EXISTS FOR (owner report, live, 2026-08-11)
//
//   "if I disconnect my internet is blocked completely, same if I kill the
//    app... I think the tunnel needs to be turned off to allow normal internet."
//
// A CLEAN disconnect already works perfectly — routes reverted, policy lifted,
// "this machine's network is BACK" in the log. What did not work was every path
// that is NOT a clean disconnect, and the reason is that NOTHING IN THIS
// SERVICE EVER RE-EVALUATED A LIVE TUNNEL. There was no timer, no poll and no
// second look anywhere between "tunnel: UP" and the next request from the app.
//
// So when the physical link dropped:
//   * EgressMonitor watched interface changes only — and a default route
//     disappearing is a ROUTE event. The observation could simply never happen.
//   * Even when it did, DiscoverEgress deliberately falls back to a down
//     interface's default route and Refresh deliberately retains the last good
//     index, so `changed` was false and the one handler that existed never ran.
//   * The SDK was never told the network moved, so its transports died on
//     timeouts against an epoch nothing rebased.
//   * The 31 capture routes stayed installed and the WFP policy stayed in its
//     Connected state — which blocks every non-tun path — over a tunnel whose
//     transports were dead.
//
// Result: total block, no recovery when the link came back, and no way out
// short of an elevated `urnetworkd revert`.
//
// ---------------------------------------------------------------------------
// THE PRINCIPLE
//
// WITH THE KILL SWITCH OFF — the default — THE MACHINE MUST NEVER BE LEFT
// BLOCKED BY A TUNNEL THAT CANNOT CARRY TRAFFIC. Failing closed is correct only
// while the kill switch is ON, and even then the user needs an obvious escape.
// That is the soft kill switch this product deliberately chose over
// Proton/Mullvad-style permanent lockdown.
//
// ---------------------------------------------------------------------------
// WHY THE DECISION IS A PURE FUNCTION IN A HEADER
//
// The I/O cannot be tested on this box: it needs an elevated service, a wintun
// adapter, 31 routes, 47 filters and a network you can physically unplug. The
// DECISION can be, and it is the part that must never be wrong — a false
// positive tears down a working VPN, a false negative leaves the owner with no
// internet. So Evaluate() reads a struct and a clock and returns a verdict,
// exactly as ConnectionHealth::Tracker, DecideConsoleStop and the NetPolicy
// table already do. It lives in DeadTunnelVerdict.h, with no Windows or SDK
// header, so tools/dead-tunnel-verdict-tests.cpp runs it on any host, and
// `urnetworkd selftest` pins the whole table on this one.
//
// ---------------------------------------------------------------------------
// THE EXCEPTION: AN ACCOUNT OUT OF BALANCE (owner decisions 2026-10-02, 2026-11-02)
//
// An account out of balance has a tunnel that cannot carry traffic, by decision
// of the platform, and the principle above stops at it. Giving this machine its
// internet back there means its traffic leaves in the clear because of a billing
// state, so the two traffic rules hold (OutOfBalanceHold, DeadTunnelVerdict.h),
// the capture stays, the app's banner and tray notice tell the user, and their
// Disconnect is the way out.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "PacketPump.h"  // PacketCounters
#include "CaptureReadiness.h"
#include "DeadTunnelVerdict.h"
#include "NetworkChangeNotify.h"  // NotifyCoalescer, FreezeResponseFor
#include "TunnelWatchdogSettings.h"
#include "Sdk.h"

namespace urnw {

// ---------------------------------------------------------------------------
// the thresholds
// ---------------------------------------------------------------------------
//
// Every tunable of this file — the verdict's windows (DeadTunnelSettings), the
// cadence of its two threads, the freeze bars, the join budget and the capture
// timing — is a field of TunnelWatchdogSettings (TunnelWatchdogSettings.h),
// with its default and the measurement that justifies it there. The watchdog is
// built with an instance and its threads read it from their channel; nothing
// here reads a timing from anywhere else, so a test or a caller can move one.

// True when the gap between two evaluator ticks can only be explained by the
// process not having run, at `settings`' freeze bar. Pure, so the selftest pins
// the discrimination rather than the wall-clock behaviour of a laptop lid.
inline constexpr bool EvaluatorFroze(int64_t tickGapMillis,
                                     const TunnelWatchdogSettings& settings) {
  return FreezeResponseFor(tickGapMillis, settings.frozenMillis,
                           settings.freezeNetworkChangeMillis)
      .rebase;
}

// One SDK network-change notification per window (NetworkNotifySettings,
// TunnelWatchdogSettings::networkNotify), which lives with the coalescer in
// NetworkChangeNotify.h: the provider-only device's notifications follow the
// same rule.

// ---------------------------------------------------------------------------
// the live object
// ---------------------------------------------------------------------------

// Everything the two threads share, and everything an ABANDONED sampler may
// still touch after Stop() has given up on it.
//
// A SHARED BLOCK RATHER THAN MEMBERS, for PacketPump::ReceiveGate's reason: the
// sampler calls into the SDK, the SDK can wedge, and Stop() must not be allowed
// to wait forever for it. So Stop() abandons the thread — and an abandoned
// thread that publishes into its owner's members is a use-after-free waiting
// for a bad day. It owns a share of this instead.
struct WatchdogChannel {
  explicit WatchdogChannel(const TunnelWatchdogSettings& timing)
      : settings(timing),
        coalescer(timing.networkNotify),
        qualityCoalescer(timing.networkNotify) {}

  // The watchdog's timing, as it was constructed with: fixed before either
  // thread starts and only read after, so it needs no lock. The threads read
  // their intervals and bars here, never from constants.
  const TunnelWatchdogSettings settings;
  // Retained in both Preparing and Up so late callbacks cannot queue work for
  // a replacement session. captureReadiness is set only while capture is pending.
  std::shared_ptr<CaptureReadiness> sessionReadiness;
  std::shared_ptr<CaptureReadiness> captureReadiness;
  std::function<void(CaptureTicket)> onReady;
  std::mutex mutex;
  std::condition_variable wake;
  // Set by Stop(). Checked by the sampler immediately before and immediately
  // after every SDK call.
  std::atomic<bool> cancelled{false};
  // Cleared by Stop() BEFORE the join, so a sampler that wakes late finds
  // nothing to call rather than a device being torn down.
  std::atomic<urnet::DeviceLocal*> device{nullptr};
  // The two threads have RETURNED. std::thread has no timed join, and these are
  // the whole mechanism for bounding one: Stop() waits on the flag, joins if it
  // is set, and abandons the thread if it is not.
  //
  // The EVALUATOR needs it for a case that is easy to miss: the user presses
  // Disconnect at the same instant the failsafe fires. The RPC thread takes the
  // session lock and calls Stop(), which joins the evaluator — while the
  // evaluator is inside the failsafe handler waiting for that same lock. The
  // failsafe's acquire is timed, so this resolves rather than deadlocking, but
  // an unbounded join would hold the machine's routes installed for the whole of
  // kStopLockBudget in the one scenario where somebody is pressing Disconnect
  // because their internet is already gone. See Stop().
  std::atomic<bool> samplerDone{false};
  std::atomic<bool> evaluatorDone{false};

  // published by the sampler, read by the evaluator
  std::atomic<int64_t> provenCount{0};
  std::atomic<int64_t> exitCount{0};
  std::atomic<int64_t> lastSampleMillis{-1};
  std::atomic<int64_t> lastProvenMillis{-1};

  // The current SDK destination generation and its window readiness, guarded
  // by `mutex`. WindowStatus carries both in one snapshot; keeping them under
  // one lock here prevents the evaluator from pairing a new generation with
  // the retired one's readiness.
  int64_t connectionGeneration = 0;
  bool providerWindowMinSatisfied = true;

  // The balance as the sampler last could read it, guarded by `mutex`, so a
  // status is never paired with another sample's generation (BalanceReading).
  BalanceReading balance;

  // the network-change coalescers, on the settings' window, guarded by `mutex`
  NotifyCoalescer coalescer;
  NotifyCoalescer qualityCoalescer;

  // WHAT THE FAILSAFE DOES, AND IT LIVES HERE RATHER THAN ON THE WATCHDOG.
  // Guarded by `mutex`; cleared by Stop() and Cancel().
  //
  // This is the last thing that made the firing path depend on the TunnelWatchdog
  // OBJECT rather than on state it shares. With the handler here, an evaluator
  // that Stop() had to abandon touches nothing but this block — of which it owns
  // a share — for the whole of its remaining life. The abandonment argument then
  // holds without a lifetime caveat, instead of holding "in every path we have
  // thought of".
  std::function<void(DeadTunnelReason)> onDead;
};

class TunnelWatchdog {
 public:
  // What the failsafe does when it decides. Invoked ON THE EVALUATOR THREAD,
  // once, as the LAST thing that thread does — see Run() for why that ordering
  // is a safety property and not a style choice.
  using DeadHandler = std::function<void(DeadTunnelReason)>;

  // The service constructs it with the defaults (TunnelWatchdogSettings); every
  // session it watches runs on these.
  explicit TunnelWatchdog(TunnelWatchdogSettings settings = {}) : settings_(settings) {}
  // What it was built with: the controller hands the capture part to each
  // session's CaptureReadiness, so the two read the same timing.
  const TunnelWatchdogSettings& settings() const { return settings_; }
  ~TunnelWatchdog();
  TunnelWatchdog(const TunnelWatchdog&) = delete;
  TunnelWatchdog& operator=(const TunnelWatchdog&) = delete;

  // Watch this session while preparing (onReady supplied) or after capture.
  //
  // The session's start instant is stamped HERE, from this class's own steady
  // clock, rather than taken as an argument: TunnelController's upSinceMillis_
  // is a WALL clock (it is reported to the app as an uptime), and mixing the
  // two would make every threshold in this file wrong by whatever the machine's
  // clock did — which on a laptop resuming from sleep is hours. Evaluate() still
  // takes the instant as data, so the table test is unaffected.
  //
  // THE CALLER OWNS `device` AND MUST Stop() BEFORE CLOSING IT, exactly as
  // WindowTrace requires — with the difference that this Stop() is BOUNDED and
  // will abandon a wedged sampler rather than hold the teardown hostage.
  void Start(urnet::DeviceLocal* device,
             std::shared_ptr<PacketCounters> counters, DeadHandler onDead,
             std::shared_ptr<CaptureReadiness> sessionReadiness,
             std::function<void(CaptureTicket)> onReady = {});

  // Stop watching. Idempotent, safe when nothing was started, and SAFE TO CALL
  // FROM INSIDE the DeadHandler — which is not a nicety: the handler tears the
  // session down, and the teardown stops the watchdog. From the evaluator's own
  // thread this cancels and detaches instead of joining itself.
  void Stop();

  // Cancel WITHOUT joining anything. For the one path that must not wait: the
  // failsafe's lock-free escape, taken when a wedged connect holds the session
  // lock and there is therefore no orderly teardown to hang this off.
  void Cancel();

  // EgressMonitor observed the network move (an OS event that moved what the
  // sdk's sockets ride: EgressChangeFilter). Cheap, non-blocking, and safe from
  // a system worker thread: it records a timestamp and wakes the sampler, whose
  // sleep it ends (PlanSamplerSleep). THE SDK IS NEVER CALLED FROM HERE —
  // EgressMonitor::Stop() waits for in-flight callbacks, so a blocking one would
  // wedge the teardown.
  bool NoteNetworkEvent(const std::shared_ptr<CaptureReadiness>& session,
                        int64_t eventMillis);

  // A radio-quality update keeps transports alive and only remeasures pacing.
  // Like NoteNetworkEvent, this records work for the SDK sampler thread.
  void NoteNetworkQualityEvent();

  // "A countdown is running right now", for TunnelStatus::failsafe_armed. Read
  // from the RPC thread while a connect may be wedged holding the session lock,
  // so it is an atomic and not a call into anything.
  bool FailsafeArmed() const { return armed_.load(std::memory_order_relaxed); }
  // Milliseconds until the earliest deadline, or 0. Only meaningful while armed.
  int64_t MillisToFailsafe() const { return millisToFailsafe_.load(std::memory_order_relaxed); }

  void RunSampler(std::shared_ptr<WatchdogChannel> channel);
  void RunEvaluator(std::shared_ptr<WatchdogChannel> channel,
                    std::shared_ptr<PacketCounters> counters,
                    int64_t upSinceMillis);
  // Copies the handler out of the channel under its lock and invokes it with
  // the lock released. Static, and that is the point: the firing path must not
  // depend on this object still existing (see WatchdogChannel::onDead).
  static void Fire(const std::shared_ptr<WatchdogChannel>& channel,
                   DeadTunnelReason reason);
  // Wait up to the channel's join budget (TunnelWatchdogSettings::
  // joinBudgetMillis) for a thread's done-flag. True means
  // it has returned and joining it will not block; false means the caller must
  // abandon it rather than wait.
  static bool JoinWithin(const std::shared_ptr<WatchdogChannel>& channel,
                         std::atomic<bool>* done);

 private:
  // What each session's channel is built with (WatchdogChannel::settings).
  const TunnelWatchdogSettings settings_;
  // Guards channel_ and the two thread handles. NEVER held across a join:
  // NoteNetworkEvent runs on a system worker thread that EgressMonitor::Stop()
  // waits for, so anything that blocks while holding this lock blocks a
  // teardown.
  std::mutex stateMutex_;
  std::shared_ptr<WatchdogChannel> channel_;
  // Emits every real destination rebuild (including same-location Reconnect)
  // and every readiness transition. The callback captures only a weak channel,
  // never this object; Stop unsubscribes before the DeviceLocal is closed.
  urnet::Sub windowStatusSub_;
  std::thread sampler_;
  std::thread evaluator_;

  std::atomic<bool> armed_{false};
  std::atomic<int64_t> millisToFailsafe_{0};
};

}  // namespace urnw
