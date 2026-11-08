// The dead-tunnel failsafe's decision (TunnelWatchdog.h): the verdict, the balance
// hold that keeps an account out of balance from it, the client limit hold that
// keeps a device the platform holds off from it, and the trackers that turn raw
// counters and SDK readings into what it reads. Pure C++ with no Windows or SDK
// header, so tools/dead-tunnel-verdict-tests.cpp runs it on any host. The holds
// are the same as urnetwork/linux's (TunnelWatchdog.hpp).
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>

#include "TunnelWatchdogSettings.h"

namespace urnw {

// A timestamp is THIS session's evidence only if it was taken after the session
// began; anything older belongs to a tunnel, or to a stretch of time, that this
// verdict has no business judging. Folding it to -1 hands it to AgeSince, which
// already means "never in this session" by counting from the start instead.
//
// Used on the freeze path: after a rebase, the sampler's last completed sample
// and the last proven reading are both from before the machine went away, and
// treating them as current is exactly the false UNRESPONSIVE the freeze bar
// exists for (TunnelWatchdogSettings::frozenMillis).
inline constexpr int64_t StampInSession(int64_t stampMillis,
                                        int64_t sessionStartMillis) {
  return stampMillis >= sessionStartMillis ? stampMillis : -1;
}

// ---------------------------------------------------------------------------
// the balance hold
// ---------------------------------------------------------------------------

// What one sample of the SDK says about the balance: ContractStatus.
// InsufficientBalance, the same reading the app's banner and tray notice take
// from this device over rpc, and the window status read after it.
struct BalanceReading {
  // Both were read. False says nothing about the balance, and the hold stays as
  // it is.
  bool sampled = false;
  // ContractStatus::InsufficientBalance: the platform's last word on this
  // destination's contracts was that the balance falls short.
  bool insufficientBalance = false;
  // ContractStatus::NoPermission, which can push a refusal out of the status
  // with no grant in between (the SDK's status window keeps the kind that fills
  // it). It says nothing about the balance.
  bool noPermission = false;
  // WindowStatus::ConnectionGeneration. The status has to be read first, so a
  // destination change between the two reads can never pair a status it reset
  // with the generation before it.
  int64_t connectionGeneration = 0;
  // WindowStatus::ProviderStateAdded: the providers that destination's window
  // admitted. Admission takes an answered ping, which took a granted contract.
  int64_t providerAddedCount = 0;
};

// What one reading did to the hold.
enum class BalanceStep {
  None,
  // First seen out of balance: the traffic rules hold from here.
  Entered,
  // Contracts are going through again: every window starts again.
  Released,
};

// Whether the session is out of balance, kept across the readings that cannot
// tell. Raised by the first reading of insufficient balance and kept until the
// platform has said contracts go through, because the two ways to get it wrong
// do not cost the same: stopping an account that is out of balance lets its
// traffic out in the clear, and holding one that is not leaves it captured and
// told so, with Disconnect working.
//
// The SDK's status only says that the last result on this destination was not a
// refusal, and it reads the same for a grant and for nothing yet: a refusal
// raises it, a granted contract clears it, a result about something else leaves
// it, and a new destination resets it with the connection it replaces (a location
// change, a Retry). So:
//   * not raised, same generation as the refusal: a contract was granted.
//   * not raised, a newer generation: the reset says nothing about the balance,
//     and the hold stays until that destination's window admits a provider.
//   * no reading, or a no-permission result in its place: says nothing, either way.
//
// The failure directions, each pinned (tools/dead-tunnel-verdict-tests.cpp):
//   * A stale or missing signal never stops an account out of balance: an
//     unreadable status, a reset, and a window that never forms all keep the hold.
//   * A dead tunnel on an account in balance is stopped as documented. A session
//     never told it is out of balance is judged by the rules alone, and this
//     includes a balance nobody has reported yet: a refusal comes back within
//     seconds of the first contract asked for, far inside the shortest window,
//     and with no answer at all there is nothing to tell a dead tunnel from a
//     billing state by, which is the failure the failsafe exists for. A start
//     out of balance is refused before it gets here (BalanceGate.h).
//   * Once the account is back in balance but no new provider has been admitted
//     since a reset, the session stays held: nothing has said so yet. A funded
//     account whose rebuilt window never forms is the one left captured until
//     its user acts, told "No working provider right now". That cost is the one
//     this chooses over a leak.
class OutOfBalanceHold {
 public:
  constexpr BalanceStep Observe(const BalanceReading& reading) {
    if (!reading.sampled) return BalanceStep::None;
    if (reading.insufficientBalance) {
      refusedGeneration_ = reading.connectionGeneration;
      if (held_) return BalanceStep::None;
      held_ = true;
      return BalanceStep::Entered;
    }
    if (!held_ || reading.noPermission) return BalanceStep::None;
    const bool granted = reading.connectionGeneration <= refusedGeneration_ ||
                         reading.providerAddedCount > 0;
    if (!granted) return BalanceStep::None;
    held_ = false;
    return BalanceStep::Released;
  }

  constexpr bool held() const { return held_; }

 private:
  bool held_ = false;
  int64_t refusedGeneration_ = 0;
};

// ---------------------------------------------------------------------------
// the client limit hold
// ---------------------------------------------------------------------------

// What one sample of the SDK says about the client limit: Device.
// GetClientLimitStatus, the same reading the app's notice takes from this device
// over rpc.
struct ClientLimitReading {
  // The status was read. False says nothing about the limit, and the hold stays
  // as it is.
  bool sampled = false;
  // ClientLimitStatus::Status is ClientLimitStatusExceeded: the platform closed
  // this device for its network's concurrent client limit, and the device holds
  // every platform transport of its client off until the hold's retry time (15
  // to 20 minutes) or the app's Retry (Device.RetryClientLimit).
  bool exceeded = false;
};

// What one reading did to the hold.
enum class ClientLimitStep {
  None,
  // First seen held: the traffic rules hold from here.
  Entered,
  // The device's hold ended: every window starts again.
  Released,
};

// Whether the platform holds this session's device off for its network's
// concurrent client limit (owner, 2026-11-08: Windows and Linux at parity with
// the apple health audit, which reads such a tunnel as healthy). Such a session
// cannot form a window, so every reading the traffic rules take says dead, and a
// stop is worse here than leaving it: the teardown lets the traffic out in the
// clear (with the kill switch off), and the device that replaces this one (the
// provider-only device the app reconciles after the drop, or the next Connect)
// dials the platform at once, past the hold the sdk keeps per device, which is a
// retry storm against the limit. The app's notice tells the user what to do
// instead, and Disconnect is theirs to press, as out of balance.
//
// Unlike the balance, the sdk reports the hold itself, so a reading says it
// either way: exceeded holds, not exceeded ends it, and only no reading leaves it
// as it is. The device dials again when its hold ends, and a refusal holds it
// again within seconds, inside the fresh windows the end gave it. The rule the
// hold never takes: SdkUnresponsive, since a wedged device answers nothing, its
// hold included.
class ClientLimitHold {
 public:
  constexpr ClientLimitStep Observe(const ClientLimitReading& reading) {
    if (!reading.sampled || reading.exceeded == held_) return ClientLimitStep::None;
    held_ = reading.exceeded;
    return held_ ? ClientLimitStep::Entered : ClientLimitStep::Released;
  }

  constexpr bool held() const { return held_; }

 private:
  bool held_ = false;
};

// ---------------------------------------------------------------------------
// the verdict
// ---------------------------------------------------------------------------

enum class DeadTunnelReason {
  None,
  // No proven exit for DeadTunnelSettings::deadSlowMillis. The tunnel never had, or has entirely
  // lost, anything to carry traffic to.
  NoExit,
  // Committed outbound traffic and ZERO inbound for DeadTunnelSettings::deadFastMillis. The
  // strongest evidence available: it measures the hole rather than inferring it.
  NoInbound,
  // The SDK has not answered in DeadTunnelSettings::sdkUnresponsiveMillis. Everything else here is
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

  // The beginning of the traffic generation being judged by DEAD_FAST. A
  // destination replacement may honestly spend longer than the DEAD_FAST window
  // forming; its traffic clock starts only when that new provider window first
  // becomes ready. 0 retains the session-start behavior for callers that have
  // no separate traffic boundary.
  int64_t trafficStartMillis = 0;
  // False only during the initial formation of a new destination generation.
  // Once that generation has been ready, later window churn cannot turn this
  // false again and reset the watchdog forever; ConnectionEpochTracker owns
  // that one-way rule.
  bool providerWindowReady = true;

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

  // ---- from the balance hold ----
  // The account is out of balance: the two traffic rules hold (OutOfBalanceHold).
  bool outOfBalance = false;

  // ---- from the client limit hold ----
  // The platform holds this device off for its network's concurrent client
  // limit: the two traffic rules hold (ClientLimitHold).
  bool clientLimitHeld = false;
};

struct DeadTunnelVerdict {
  DeadTunnelReason reason = DeadTunnelReason::None;
  // A countdown is running RIGHT NOW and is within DeadTunnelSettings::failsafeNoticeMillis of
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

// THE VERDICT, under `settings`. Pure, total, and the only place the failsafe's
// mind is made up.
//
// WHY THIS CANNOT FIRE WHILE TRAFFIC IS GENUINELY FLOWING. Not as an argument
// about SDK internals but as the FIRST RULE IN THE FUNCTION: a packet that came
// back out of the tun within the DEAD_FAST window vetoes every verdict below it, so
// no claim about exits and no silence from the SDK can tear down a tunnel this
// machine is measurably using. The connected policy
// blocks every non-tun path, so a working tunnel's host stack retransmits and
// ANY single inbound packet from ANY exit resets the fast window. A genuinely
// idle machine has outboundSinceInbound == 0 and never trips DEAD_FAST — which
// is correct, because an idle blocked machine is indistinguishable from an idle
// working one. The moment the owner tries to use the network the evidence
// appears and the verdict follows within the DEAD_FAST window.
//
// Recovery is ONE-SIDED AND IMMEDIATE, mirroring ConnectionHealth::Tracker: one
// proven exit or one inbound packet clears everything at once. Only loss is
// held.
inline constexpr DeadTunnelVerdict Evaluate(const DeadTunnelSignals& s,
                                            int64_t nowMillis,
                                            const DeadTunnelSettings& settings) {
  DeadTunnelVerdict v;
  // Nothing to judge unless this machine is actually pointed at a tunnel.
  if (!s.tunnelUp || !s.routesInstalled || s.upSinceMillis <= 0) return v;

  const int64_t sampleAge = detail::AgeSince(s.lastSampleMillis, s.upSinceMillis, nowMillis);
  const int64_t provenAge = detail::AgeSince(s.lastProvenMillis, s.upSinceMillis, nowMillis);
  const int64_t trafficStartMillis =
      s.trafficStartMillis > 0 ? s.trafficStartMillis : s.upSinceMillis;
  const int64_t inboundAge =
      detail::AgeSince(s.lastInboundMillis, trafficStartMillis, nowMillis);

  // ---- THE MEASUREMENT OVERRULES THE CLAIM, AND IT DOES SO FIRST ------------
  //
  // A packet that came back OUT of the tun is the only FIRST-HAND evidence in
  // this process that this tunnel can carry traffic. Everything else below is
  // second-hand: provenCount is the sdk's claim ABOUT its transports, and a
  // completed getExits() is a claim about the sdk answering questions. Neither
  // may overrule a measurement, so while anything is still coming back NOTHING
  // fires and no countdown is shown.
  //
  // This is not a softening. It is what turns "the failsafe cannot tear down a
  // working tunnel" from an argument about sdk internals into a property of
  // this function, and it closes three false teardowns that were reachable:
  //
  //   * getExits() blocking on the sdk's state lock while the DATA PATH — a
  //     different set of goroutines entirely, reaching this process through
  //     addReceivePacket — keeps delivering. UNRESPONSIVE would call that a
  //     wedge, tear down a tunnel the user was using, and report a reason that
  //     was not true.
  //   * the sampler thread's OWN networkChanged() being slow during exactly
  //     the roam that made it necessary — the watchdog's own sdk work
  //     presenting as sdk silence.
  //   * an exit that carries but is never marked Proven. Exit.Proven is a probe
  //     result; task #15/S2 exists precisely because exit state is known to be
  //     incomplete. NO_EXIT is explicitly "whatever the traffic", and that is
  //     the one place this file let an inference outrank its own measurement.
  //
  // It cannot mask DEAD_FAST, which already requires inboundAge >= this same
  // window, and it cannot mask a genuine wedge: a wedged sdk delivers nothing,
  // so the veto lapses within the DEAD_FAST window of the wedge beginning.
  if (s.lastInboundMillis >= 0 && inboundAge < settings.deadFastMillis) return v;

  // ---- UNRESPONSIVE first ---------------------------------------------------
  // Deliberately ahead of the other two: provenCount is a reading FROM the SDK,
  // so if the SDK has stopped answering, the other two rules would be judging
  // stale data. This rule judges the answering itself. It stays out of balance
  // and under a client limit hold too, because it is about a hung process and
  // not about traffic: no balance or free slot brings a wedged device back, it
  // cannot say whether it is out of balance or held, and a Disconnect would only
  // wait on the same lock.
  if (sampleAge >= settings.sdkUnresponsiveMillis) {
    v.reason = DeadTunnelReason::SdkUnresponsive;
    return v;
  }

  // ---- DEAD_FAST ------------------------------------------------------------
  // Committed outbound, zero inbound, nothing proven — all three, continuously.
  // Never out of balance or under a client limit hold: that tunnel is meant to
  // carry nothing until the user acts, stopping it lets the traffic out in the
  // clear, and a held device's replacement dials past its hold.
  const bool fastApplies =
      !s.outOfBalance && !s.clientLimitHeld && s.providerWindowReady && s.provenCount == 0 &&
      s.outboundSinceInbound >= settings.deadFastOutboundPackets;
  if (fastApplies && inboundAge >= settings.deadFastMillis &&
      provenAge >= settings.deadFastMillis) {
    v.reason = DeadTunnelReason::NoInbound;
    return v;
  }

  // ---- DEAD_SLOW ------------------------------------------------------------
  // No proven exit for a long time, and nothing has come back to say otherwise
  // (the veto above already returned if it had). A tunnel with nothing to carry
  // to cannot carry, and holding the machine's routes for it is the block the
  // owner reported.
  //
  // Gated on the sampler having ANSWERED AT LEAST ONCE. "We never got a reading"
  // is the unresponsive rule's business, not this one's, and firing here on an
  // absence of evidence would be exactly the kind of guess this file refuses.
  // Never out of balance or under a client limit hold, as above.
  const bool slowApplies = !s.outOfBalance && !s.clientLimitHeld && s.provenCount == 0 &&
                           s.lastSampleMillis >= 0;
  if (slowApplies && provenAge >= settings.deadSlowMillis) {
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
  if (sampleAge >= settings.sdkUnresponsiveArmMillis)
    consider(settings.sdkUnresponsiveMillis - sampleAge);
  if (fastApplies) {
    const int64_t elapsed = inboundAge < provenAge ? inboundAge : provenAge;
    consider(settings.deadFastMillis - elapsed);
  }
  if (slowApplies) consider(settings.deadSlowMillis - provenAge);

  if (soonest >= 0 && soonest <= settings.failsafeNoticeMillis) {
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
// the destination-generation tracker (pure)
// ---------------------------------------------------------------------------

// What one WindowStatus observation changes in the evaluator. A destination
// rebuild resets every verdict clock; the first ready edge of that generation
// resets only DEAD_FAST's packet/traffic clock. Keeping the two explicit is the
// distinction that prevents a 27-second honest peer formation from inheriting
// the old provider's 20-second failure window without giving every reconnect a
// second 90-second no-exit grace after it is ready.
struct ConnectionEpochUpdate {
  bool verdictClockReset = false;
  bool trafficClockReset = false;
};

// Consumes the monotonic generation and MinSatisfied values carried by the
// SDK's WindowStatus. Safe against late callbacks from a retired window: an
// older generation is ignored. Readiness is one-way inside a generation, so
// ordinary resize/liveness churn cannot keep resetting the failsafe forever.
class ConnectionEpochTracker {
 public:
  ConnectionEpochUpdate Observe(int64_t connectionGeneration,
                                bool minSatisfied) {
    ConnectionEpochUpdate update;
    if (!initialized_) {
      initialized_ = true;
      connectionGeneration_ = connectionGeneration;
      forming_ = !minSatisfied;
      return update;
    }
    if (connectionGeneration < connectionGeneration_) return update;
    if (connectionGeneration > connectionGeneration_) {
      connectionGeneration_ = connectionGeneration;
      forming_ = !minSatisfied;
      update.verdictClockReset = true;
      update.trafficClockReset = true;
      return update;
    }
    if (forming_ && minSatisfied) {
      forming_ = false;
      update.trafficClockReset = true;
    }
    return update;
  }

  bool FastVerdictEligible() const { return initialized_ && !forming_; }
  int64_t connectionGeneration() const { return connectionGeneration_; }

 private:
  bool initialized_ = false;
  bool forming_ = false;
  int64_t connectionGeneration_ = 0;
};

}  // namespace urnw
