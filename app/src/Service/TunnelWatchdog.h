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
// table already do, and `urnetworkd selftest` pins the whole table.
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
#include "Sdk.h"

namespace urnw {

// ---------------------------------------------------------------------------
// the thresholds
// ---------------------------------------------------------------------------
//
// Every number here is justified against something already measured or already
// recorded in this repo, not guessed. A failsafe with invented thresholds is a
// second bug wearing the first one's clothes.

// DEAD_FAST. The host is sending and NOTHING is coming back.
//
// Must exceed the longest HONEST machine-wide inbound gap. The Windows DNS
// client's query schedule across a server list runs ~12-13 s and a black-holed
// TCP connect costs ~21 s (both recorded on kConnectingWindow in
// TunnelController.cpp) — but the TCP one stalls ONE FLOW, not every flow,
// while this window is machine-wide. Measured window formation on the owner's
// box is sub-second ("UP in 658ms"). 20 s is >1.5x the DNS schedule and under
// the ~30 s at which a human has already concluded the internet is broken.
inline constexpr int64_t kDeadFastMillis = 20000;

// ...and it takes REAL committed traffic to arm, not one stray packet. With the
// connected policy in force the host stack retransmits, so a machine that is
// genuinely trying produces this in well under a second; a machine that is idle
// never does, and MUST NOT — an idle blocked machine is indistinguishable from
// an idle working one, and firing there would be a teardown with no evidence.
inline constexpr uint64_t kDeadFastOutboundPackets = 8;

// DEAD_SLOW. No proven exit at all, whatever the traffic.
//
// One honest COLD attempt after a roam is DNS (<=13 s) + TCP (<=21 s) +
// handshake, call it ~35 s (the same note). 90 s is ~2.5x that, so a genuine
// roam that recovers on its second attempt is never torn down. It also bounds
// the never-proven connect on a hostile network at ~90 s instead of forever.
inline constexpr int64_t kDeadSlowMillis = 90000;

// UNRESPONSIVE. The SDK sampler has not COMPLETED a getExits() in this long.
//
// The sampler runs at 2 s, so this is 15 consecutive missed samples. A cgo call
// that has not returned in 30 s is wedged, not slow — and this is the ONLY
// condition that can fire during the silent-death family of task #39, because
// it needs no cooperation from the SDK whatsoever.
inline constexpr int64_t kSdkUnresponsiveMillis = 30000;

// How overdue a sample must be before the unresponsive countdown is worth
// SHOWING. Below this it is just the normal 2 s cadence and reporting "armed"
// would mean reporting it forever.
inline constexpr int64_t kSdkUnresponsiveArmMillis = 10000;

// How close a verdict must be before the UI is told a countdown is running. The
// teardown must never be a surprise, and a warning 90 s early is noise that
// teaches the owner to ignore the one that matters.
inline constexpr int64_t kFailsafeNoticeMillis = 30000;

// How often the SDK is sampled, and how often the verdict is recomputed. The
// verdict thread is deliberately faster AND touches no SDK, so a wedged sampler
// cannot stop the verdict being reached.
inline constexpr int64_t kSdkSampleIntervalMillis = 2000;
inline constexpr int64_t kEvaluateIntervalMillis = 1000;

// One SDK network-change notification per this many milliseconds.
//
// A roam produces dozens of OS notifications in a second. Below this two
// notifications never describe different states; above it the burst of a single
// roam would be split into several kicks.
inline constexpr int64_t kNetworkNotifyDebounceMillis = 750;

// How long TunnelWatchdog::Stop() waits for the SDK sampler before abandoning
// it. Same discipline, and the same reasoning, as kSdkTeardownBudget: a healthy
// getExits() returns in microseconds, so this can only expire on a wedge — and
// a stop that waits on a wedge is the failure this whole area exists to remove.
inline constexpr int64_t kWatchdogJoinBudgetMillis = 250;

// ---------------------------------------------------------------------------
// the verdict
// ---------------------------------------------------------------------------

enum class DeadTunnelReason {
  None,
  // No proven exit for kDeadSlowMillis. The tunnel never had, or has entirely
  // lost, anything to carry traffic to.
  NoExit,
  // Committed outbound traffic and ZERO inbound for kDeadFastMillis. The
  // strongest evidence available: it measures the hole rather than inferring it.
  NoInbound,
  // The SDK has not answered in kSdkUnresponsiveMillis. Everything else here is
  // built on readings from it, so this outranks them.
  SdkUnresponsive,
};

// The wire value carried in TunnelStatus::stop_reason. String literals in
// static storage, like Heartbeat.h's published state, so a lock-free publisher
// can hand one out as a bare const char*.
inline constexpr const char* kStopReasonNone = "";
inline constexpr const char* kStopReasonUser = "user";
inline constexpr const char* kStopReasonNoExit = "failsafe_no_exit";
inline constexpr const char* kStopReasonNoInbound = "failsafe_no_inbound";
inline constexpr const char* kStopReasonUnresponsive = "failsafe_sdk_unresponsive";

inline constexpr const char* StopReasonOf(DeadTunnelReason r) {
  switch (r) {
    case DeadTunnelReason::None: return kStopReasonNone;
    case DeadTunnelReason::NoExit: return kStopReasonNoExit;
    case DeadTunnelReason::NoInbound: return kStopReasonNoInbound;
    case DeadTunnelReason::SdkUnresponsive: return kStopReasonUnresponsive;
  }
  return kStopReasonNone;
}

// Human text for the one loud log block the teardown writes, and for the app's
// reason clause. Not localized: the log is not, and the app's own store strings
// carry the user-facing sentence around it.
inline constexpr const char* DescribeDeadTunnel(DeadTunnelReason r) {
  switch (r) {
    case DeadTunnelReason::None: return "the tunnel is carrying traffic";
    case DeadTunnelReason::NoExit:
      return "no provider was ever proven, or every proven provider has been "
             "gone continuously";
    case DeadTunnelReason::NoInbound:
      return "this machine kept sending into the tunnel and NOTHING came back";
    case DeadTunnelReason::SdkUnresponsive:
      return "the sdk stopped answering, so nothing can be said about the "
             "tunnel except that it is not being managed";
  }
  return "unknown";
}

// THE DISARM CHOICE, as a one-line function so nobody can invert it in a
// refactor. Pinned by the selftest.
//
//   kill switch OFF -> finalDisarm TRUE.  Policy goes Off; the machine gets its
//                      internet back in the clear. This is byte for byte the
//                      path a user pressing Disconnect takes.
//   kill switch ON  -> finalDisarm FALSE. Policy narrows Connected -> Armed and
//                      the machine STAYS blocked. Silently restoring the
//                      internet here would break the promise the setting makes;
//                      the escape is one click (SetKillSwitch(false)), and the
//                      narrowing is a pure subset, so nothing leaks.
inline constexpr bool FailsafeFinalDisarm(bool killSwitchOn) {
  return !killSwitchOn;
}

// Everything the verdict consumes, and nothing else. Filled by the evaluator
// from atomics it already holds; NOTHING here issues a call, takes a lock, or
// reads a clock — `nowMillis` is the only clock in the decision, which is what
// makes the hysteresis testable.
//
// Every timestamp is a monotonic millisecond count in the SAME clock as
// `nowMillis`, and -1 means "never in this session".
struct DeadTunnelSignals {
  // The tunnel reports Up AND routes are installed. Both, because the verdict's
  // whole subject is "this machine is pointed at a tunnel": an rpc-only session
  // has neither, and a session mid-teardown may have one without the other.
  bool tunnelUp = false;
  bool routesInstalled = false;
  // When this session reached Up. 0 means there is no session to judge.
  int64_t upSinceMillis = 0;

  // ---- from the SDK sampler ----
  // The proven-exit count from the last COMPLETED getExits().
  int64_t provenCount = 0;
  // When a completed sample last reported provenCount >= 1.
  int64_t lastProvenMillis = -1;
  // When a getExits() last COMPLETED, whatever it returned. This is the wedge
  // detector: it is a fact about the SDK answering, not about what it said.
  int64_t lastSampleMillis = -1;

  // ---- from the packet pump ----
  // When the inbound counter last MOVED. One packet — a TCP ACK, a DNS answer,
  // a keepalive — is enough, and resets the fast window outright.
  int64_t lastInboundMillis = -1;
  // Outbound packets since that inbound packet (or since Up, if there has never
  // been one).
  uint64_t outboundSinceInbound = 0;
};

struct DeadTunnelVerdict {
  DeadTunnelReason reason = DeadTunnelReason::None;
  // A countdown is running RIGHT NOW and is within kFailsafeNoticeMillis of
  // firing. This is what TunnelStatus::failsafe_armed carries, so the UI can
  // warn BEFORE the teardown rather than explain it afterwards.
  bool armed = false;
  // Milliseconds until the earliest applicable deadline, or 0 when none is
  // running. Only meaningful while `armed`.
  int64_t millisToFailsafe = 0;
};

namespace detail {
// "How long has it been", where -1 means "it never happened in this session" and
// therefore counts from the session's start. Folding the two cases here is what
// keeps every rule below a single comparison.
inline constexpr int64_t AgeSince(int64_t stampMillis, int64_t upSinceMillis,
                                  int64_t nowMillis) {
  return nowMillis - (stampMillis < 0 ? upSinceMillis : stampMillis);
}
}  // namespace detail

// THE VERDICT. Pure, total, and the only place the failsafe's mind is made up.
//
// WHY THIS CANNOT FIRE WHILE TRAFFIC IS GENUINELY FLOWING. The connected policy
// blocks every non-tun path, so a working tunnel's host stack retransmits and
// ANY single inbound packet from ANY exit resets the fast window. A genuinely
// idle machine has outboundSinceInbound == 0 and never trips DEAD_FAST — which
// is correct, because an idle blocked machine is indistinguishable from an idle
// working one. The moment the owner tries to use the network the evidence
// appears and the verdict follows within kDeadFastMillis.
//
// Recovery is ONE-SIDED AND IMMEDIATE, mirroring ConnectionHealth::Tracker: one
// proven exit or one inbound packet clears everything at once. Only loss is
// held.
inline constexpr DeadTunnelVerdict Evaluate(const DeadTunnelSignals& s,
                                            int64_t nowMillis) {
  DeadTunnelVerdict v;
  // Nothing to judge unless this machine is actually pointed at a tunnel.
  if (!s.tunnelUp || !s.routesInstalled || s.upSinceMillis <= 0) return v;

  const int64_t sampleAge = detail::AgeSince(s.lastSampleMillis, s.upSinceMillis, nowMillis);
  const int64_t provenAge = detail::AgeSince(s.lastProvenMillis, s.upSinceMillis, nowMillis);
  const int64_t inboundAge = detail::AgeSince(s.lastInboundMillis, s.upSinceMillis, nowMillis);

  // ---- UNRESPONSIVE first ---------------------------------------------------
  // Deliberately ahead of the other two: provenCount is a reading FROM the SDK,
  // so if the SDK has stopped answering, the other two rules would be judging
  // stale data. This rule judges the answering itself.
  if (sampleAge >= kSdkUnresponsiveMillis) {
    v.reason = DeadTunnelReason::SdkUnresponsive;
    return v;
  }

  // ---- DEAD_FAST ------------------------------------------------------------
  // Committed outbound, zero inbound, nothing proven — all three, continuously.
  const bool fastApplies =
      s.provenCount == 0 && s.outboundSinceInbound >= kDeadFastOutboundPackets;
  if (fastApplies && inboundAge >= kDeadFastMillis && provenAge >= kDeadFastMillis) {
    v.reason = DeadTunnelReason::NoInbound;
    return v;
  }

  // ---- DEAD_SLOW ------------------------------------------------------------
  // No proven exit for a long time, whatever the traffic. A tunnel with nothing
  // to carry to cannot carry, and holding the machine's routes for it is the
  // block the owner reported.
  //
  // Gated on the sampler having ANSWERED AT LEAST ONCE. "We never got a reading"
  // is the unresponsive rule's business, not this one's, and firing here on an
  // absence of evidence would be exactly the kind of guess this file refuses.
  if (s.provenCount == 0 && s.lastSampleMillis >= 0 && provenAge >= kDeadSlowMillis) {
    v.reason = DeadTunnelReason::NoExit;
    return v;
  }

  // ---- nothing has fired: is a countdown running, and how close is it? ------
  //
  // `armed` exists so the teardown is never a surprise. It reports only the
  // countdowns that are actually accumulating: the unresponsive one is always
  // ticking (a sample lands every 2 s) so it contributes only once genuinely
  // overdue, and the fast one contributes only while its traffic precondition
  // holds.
  int64_t soonest = -1;
  auto consider = [&soonest](int64_t remaining) {
    if (remaining < 0) remaining = 0;
    if (soonest < 0 || remaining < soonest) soonest = remaining;
  };
  if (sampleAge >= kSdkUnresponsiveArmMillis)
    consider(kSdkUnresponsiveMillis - sampleAge);
  if (fastApplies) {
    const int64_t elapsed = inboundAge < provenAge ? inboundAge : provenAge;
    consider(kDeadFastMillis - elapsed);
  }
  if (s.provenCount == 0 && s.lastSampleMillis >= 0)
    consider(kDeadSlowMillis - provenAge);

  if (soonest >= 0 && soonest <= kFailsafeNoticeMillis) {
    v.armed = true;
    v.millisToFailsafe = soonest;
  }
  return v;
}

// ---------------------------------------------------------------------------
// the packet-counter tracker (pure)
// ---------------------------------------------------------------------------
//
// Turns the pump's two raw monotonic counters into the EDGE TIMESTAMPS the
// verdict wants. Separate from Evaluate so the verdict stays a function of
// facts rather than of a sampling history, and pure so both halves are pinned
// by the same table test.
class TrafficTracker {
 public:
  // A new session. Everything before this instant is another tunnel's evidence.
  void Reset(uint64_t outbound, uint64_t inbound) {
    lastOutbound_ = outbound;
    lastInbound_ = inbound;
    outboundAtLastInbound_ = outbound;
    lastInboundMillis_ = -1;
  }

  void Observe(uint64_t outbound, uint64_t inbound, int64_t nowMillis) {
    if (inbound > lastInbound_) {
      lastInbound_ = inbound;
      lastInboundMillis_ = nowMillis;
      // The fast window measures outbound SINCE the last thing that came back,
      // so a single inbound packet resets the commitment as well as the clock.
      outboundAtLastInbound_ = outbound;
    }
    if (outbound > lastOutbound_) lastOutbound_ = outbound;
  }

  int64_t lastInboundMillis() const { return lastInboundMillis_; }
  uint64_t outboundSinceInbound() const {
    // Guarded rather than assumed non-negative: the counters are relaxed
    // atomics read from another thread, so a sample can straddle an increment
    // and read an outbound older than the one recorded at the last inbound.
    return lastOutbound_ > outboundAtLastInbound_
               ? lastOutbound_ - outboundAtLastInbound_
               : 0;
  }

 private:
  uint64_t lastOutbound_ = 0;
  uint64_t lastInbound_ = 0;
  uint64_t outboundAtLastInbound_ = 0;
  int64_t lastInboundMillis_ = -1;
};

// ---------------------------------------------------------------------------
// the network-change coalescer (pure)
// ---------------------------------------------------------------------------
//
// A roam produces dozens of OS notifications inside a second, and each one
// would otherwise become a cgo call into the SDK that kicks every transport in
// the process. This folds a burst into exactly one notification.
//
// TRAILING FIRE ON A FIXED WINDOW, not a re-extending debounce: the deadline is
// set by the FIRST observation of a burst and never pushed out. A re-extending
// debounce can be starved indefinitely by a link that keeps flapping, which is
// precisely the condition in which the SDK most needs to be told.
class NotifyCoalescer {
 public:
  // An observation arrived. Never notifies anything; it only records.
  void Observe(int64_t nowMillis) {
    ++coalesced_;
    if (pending_) return;
    pending_ = true;
    deadlineMillis_ = nowMillis + kNetworkNotifyDebounceMillis;
  }

  // Is a notification due? Consumes the pending burst when it says yes, so a
  // caller that fires on true cannot fire twice for one burst.
  bool TakeDue(int64_t nowMillis) {
    if (!pending_ || nowMillis < deadlineMillis_) return false;
    pending_ = false;
    lastBurstSize_ = coalesced_;
    coalesced_ = 0;
    return true;
  }

  bool pending() const { return pending_; }
  int64_t deadlineMillis() const { return deadlineMillis_; }
  // How many observations the notification just taken folded together. For the
  // log line, so a roam reads as one kick over N events rather than as a
  // suspiciously quiet single event.
  int64_t lastBurstSize() const { return lastBurstSize_; }

 private:
  bool pending_ = false;
  int64_t deadlineMillis_ = 0;
  int64_t coalesced_ = 0;
  int64_t lastBurstSize_ = 0;
};

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

  // the network-change coalescer, guarded by `mutex`
  NotifyCoalescer coalescer;

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

  TunnelWatchdog() = default;
  ~TunnelWatchdog();
  TunnelWatchdog(const TunnelWatchdog&) = delete;
  TunnelWatchdog& operator=(const TunnelWatchdog&) = delete;

  // Begin watching `device` for the session that has just reached Up.
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
             std::shared_ptr<PacketCounters> counters, DeadHandler onDead);

  // Stop watching. Idempotent, safe when nothing was started, and SAFE TO CALL
  // FROM INSIDE the DeadHandler — which is not a nicety: the handler tears the
  // session down, and the teardown stops the watchdog. From the evaluator's own
  // thread this cancels and detaches instead of joining itself.
  void Stop();

  // Cancel WITHOUT joining anything. For the one path that must not wait: the
  // failsafe's lock-free escape, taken when a wedged connect holds the session
  // lock and there is therefore no orderly teardown to hang this off.
  void Cancel();

  // An OS network event was observed. Cheap, non-blocking, and safe from a
  // system worker thread: it records a timestamp and wakes the sampler. THE SDK
  // IS NEVER CALLED FROM HERE — EgressMonitor::Stop() waits for in-flight
  // callbacks, so a blocking one would wedge the teardown.
  void NoteNetworkEvent();

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
  // Wait up to kWatchdogJoinBudgetMillis for a thread's done-flag. True means
  // it has returned and joining it will not block; false means the caller must
  // abandon it rather than wait.
  static bool JoinWithin(const std::shared_ptr<WatchdogChannel>& channel,
                         std::atomic<bool>* done);

 private:
  // Guards channel_ and the two thread handles. NEVER held across a join:
  // NoteNetworkEvent runs on a system worker thread that EgressMonitor::Stop()
  // waits for, so anything that blocks while holding this lock blocks a
  // teardown.
  std::mutex stateMutex_;
  std::shared_ptr<WatchdogChannel> channel_;
  std::thread sampler_;
  std::thread evaluator_;

  std::atomic<bool> armed_{false};
  std::atomic<int64_t> millisToFailsafe_{0};
};

}  // namespace urnw
