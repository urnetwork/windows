// Executable spec for the dead-tunnel failsafe's decision
// (Service/DeadTunnelVerdict.h) on an account out of balance (owner decisions
// 2026-10-02 and 2026-11-02): the session is never stopped by a rule about
// traffic, its traffic stays captured, the banner and the tray notice tell the
// user, and Disconnect is theirs to press. The watchdog itself needs Windows and
// the sdk, so Session below takes the steps its evaluator takes on a tick
// (TunnelWatchdog::RunEvaluator; tests/dead_tunnel_verdict_test.go reads that they
// are the ones it takes) over the readings the sdk gives such a session: a window
// that never admits a provider, or one that loses its exits once its contracts run
// out, with the sampler answering all along. Run against the same headers the
// service compiles, on any host with a C++20 compiler. No clocks: the time is an
// explicit input.
//
//   c++ -std=c++20 -Wall -Wextra -Werror -pthread -I ../src/Service -I ../src/Common -I ../src/App dead-tunnel-verdict-tests.cpp -o /tmp/dead-tunnel-verdict-tests && /tmp/dead-tunnel-verdict-tests
//
// SPDX-License-Identifier: MPL-2.0

#include <cstdint>
#include <iostream>
#include <string>

#include "BalanceGate.h"
#include "DataInfo.h"
#include "DeadTunnelVerdict.h"

using namespace urnw;

namespace {

int gFailures = 0;
int gCases = 0;

void Check(bool condition, const std::string& what) {
  ++gCases;
  if (!condition) {
    ++gFailures;
    // flushed: a negative control may end the process before main returns
    std::cout << "  FAIL " << what << std::endl;
  }
}

const TunnelWatchdogSettings kWatchdog;
const DeadTunnelSettings& kDead = kWatchdog.deadTunnel;

// What the sampler reads each interval, and what the pump's counters do between.
struct Readings {
  int64_t generation = 1;
  int64_t added = 0;
  int64_t proven = 0;
  bool minSatisfied = false;
  bool insufficientBalance = false;
  bool noPermission = false;
  bool balanceRead = true;
  bool answering = true;
  uint64_t outPerTick = 50;
  uint64_t inPerTick = 0;
};

struct Tick {
  DeadTunnelVerdict verdict;
  BalanceStep balanceStep = BalanceStep::None;
  bool outOfBalance = false;
};

// One watched Up session, ticked as RunEvaluator ticks it.
struct Session {
  Readings readings;
  int64_t now = 1000000;
  int64_t ticks = 0;
  bool armedEver = false;
  // the channel
  int64_t proven = 0;
  int64_t lastProvenMillis = -1;
  int64_t lastSampleMillis = -1;
  BalanceReading balance;
  int64_t connectionGeneration = 1;
  bool providerWindowMinSatisfied = false;
  // the pump
  uint64_t outbound = 0;
  uint64_t inbound = 0;
  // the evaluator
  int64_t sessionStartMillis = now;
  int64_t trafficStartMillis = now;
  TrafficTracker traffic;
  ConnectionEpochTracker connectionEpoch;
  OutOfBalanceHold outOfBalance;

  Session() { connectionEpoch.Observe(connectionGeneration, providerWindowMinSatisfied); }

  void Sample() {
    if (!readings.answering) return;
    proven = readings.proven;
    if (readings.proven > 0) lastProvenMillis = now;
    lastSampleMillis = now;
    connectionGeneration = readings.generation;
    providerWindowMinSatisfied = readings.minSatisfied;
    if (readings.balanceRead) {
      balance = BalanceReading{.sampled = true,
                               .insufficientBalance = readings.insufficientBalance,
                               .noPermission = readings.noPermission,
                               .connectionGeneration = readings.generation,
                               .providerAddedCount = readings.added};
    }
  }

  void Rebase() {
    sessionStartMillis = now;
    trafficStartMillis = now;
    traffic.Reset(outbound, inbound);
    connectionEpoch.Observe(connectionGeneration, providerWindowMinSatisfied);
  }

  // A tick: `step` after the last, and a freeze if that is at the freeze bar.
  Tick Step(int64_t step = kWatchdog.evaluateIntervalMillis) {
    now += step;
    outbound += readings.outPerTick;
    inbound += readings.inPerTick;

    Tick tick;
    tick.balanceStep = outOfBalance.Observe(balance);
    tick.outOfBalance = outOfBalance.held();
    if (step >= kWatchdog.frozenMillis || tick.balanceStep == BalanceStep::Released) {
      Rebase();
    } else {
      const ConnectionEpochUpdate update =
          connectionEpoch.Observe(connectionGeneration, providerWindowMinSatisfied);
      if (update.verdictClockReset) sessionStartMillis = now;
      if (update.trafficClockReset) {
        trafficStartMillis = now;
        traffic.Reset(outbound, inbound);
      }
      traffic.Observe(outbound, inbound, now);

      DeadTunnelSignals signals;
      signals.tunnelUp = true;
      signals.routesInstalled = true;
      signals.upSinceMillis = sessionStartMillis;
      signals.trafficStartMillis = trafficStartMillis;
      signals.providerWindowReady = connectionEpoch.FastVerdictEligible();
      signals.provenCount = proven;
      signals.lastProvenMillis = StampInSession(lastProvenMillis, sessionStartMillis);
      signals.lastSampleMillis = StampInSession(lastSampleMillis, sessionStartMillis);
      signals.lastInboundMillis = traffic.lastInboundMillis();
      signals.outboundSinceInbound = traffic.outboundSinceInbound();
      signals.outOfBalance = outOfBalance.held();
      tick.verdict = Evaluate(signals, now, kDead);
    }
    if (tick.verdict.armed) armedEver = true;
    if (++ticks % (kWatchdog.sampleIntervalMillis / kWatchdog.evaluateIntervalMillis) == 0) Sample();
    return tick;
  }

  // The elapsed time at the verdict and its reason, or -1 for none in `millis`.
  int64_t RunUntilDead(int64_t millis, DeadTunnelReason* reason = nullptr) {
    for (int64_t elapsed = kWatchdog.evaluateIntervalMillis; elapsed <= millis;
         elapsed += kWatchdog.evaluateIntervalMillis) {
      const Tick tick = Step();
      if (tick.verdict.reason != DeadTunnelReason::None) {
        if (reason != nullptr) *reason = tick.verdict.reason;
        return elapsed;
      }
    }
    return -1;
  }

  void Run(int64_t millis) {
    for (int64_t elapsed = 0; elapsed < millis; elapsed += kWatchdog.evaluateIntervalMillis) Step();
  }
};

void FormAndCarry(Session& session) {
  session.readings.added = 2;
  session.readings.proven = 2;
  session.readings.minSatisfied = true;
  session.readings.inPerTick = 40;
  session.Sample();
  session.Run(60000);
}

void LoseExits(Session& session) {
  session.readings.added = 0;
  session.readings.proven = 0;
  session.readings.inPerTick = 0;
}

const int64_t kLong = 10 * kDead.deadSlowMillis;

void TestOutOfBalanceIsNotStopped() {
  {
    Session session;
    session.readings.insufficientBalance = true;
    session.Sample();
    Check(session.RunUntilDead(kLong) == -1,
          "a session that starts out of balance is not stopped past NoExit's window");
    Check(!session.armedEver, "...and no countdown ever said it would be");
  }
  {
    Session session;
    FormAndCarry(session);
    session.readings.insufficientBalance = true;
    LoseExits(session);
    Check(session.RunUntilDead(kLong) == -1,
          "a session that runs out while up is not stopped past NoInbound's window");
    Check(!session.armedEver, "...and no countdown ever said it would be");
  }
  {
    Session session;
    FormAndCarry(session);
    session.readings.insufficientBalance = true;
    session.readings.inPerTick = 0;
    Check(session.RunUntilDead(30 * 60000) == -1, "exits that still read proven after the refusal");
    LoseExits(session);
    Check(session.RunUntilDead(kLong) == -1, "...and that go later, are not stopped either");
  }
}

void TestInBalanceIsStoppedAsDocumented() {
  {
    Session session;
    session.Sample();
    DeadTunnelReason reason = DeadTunnelReason::None;
    const int64_t at = session.RunUntilDead(kLong, &reason);
    Check(at == kDead.deadSlowMillis && reason == DeadTunnelReason::NoExit,
          "the same session in balance stops with NoExit, at its window");
    Check(session.armedEver, "...and the countdown said so");
  }
  {
    Session session;
    FormAndCarry(session);
    LoseExits(session);
    DeadTunnelReason reason = DeadTunnelReason::None;
    const int64_t at = session.RunUntilDead(kLong, &reason);
    Check(reason == DeadTunnelReason::NoInbound && at >= kDead.deadFastMillis &&
              at <= kDead.deadFastMillis + kWatchdog.sampleIntervalMillis,
          "the same session in balance that ran dead while up stops with NoInbound, at its window");
  }
}

void TestTheBalanceReturning() {
  {
    Session session;
    session.readings.insufficientBalance = true;
    session.Sample();
    session.Run(5 * 60000);
    session.readings.insufficientBalance = false;
    session.Sample();
    const Tick released = session.Step();
    Check(released.balanceStep == BalanceStep::Released && !released.outOfBalance,
          "a contract granted on the refused destination ends the hold");
    Check(released.verdict.reason == DeadTunnelReason::None && !released.verdict.armed,
          "...and the rule it kept back does not fire on that tick");
    DeadTunnelReason reason = DeadTunnelReason::None;
    const int64_t at = session.RunUntilDead(kLong, &reason);
    Check(at == kDead.deadSlowMillis && reason == DeadTunnelReason::NoExit,
          "...and a tunnel still dead a full window later is stopped");
  }
  {
    Session session;
    session.readings.insufficientBalance = true;
    session.Sample();
    session.Run(5 * 60000);
    session.readings.insufficientBalance = false;
    session.readings.added = 2;
    session.readings.proven = 2;
    session.readings.minSatisfied = true;
    session.readings.inPerTick = 40;
    session.Sample();
    Check(session.RunUntilDead(kLong) == -1, "the exits coming back with the contracts stop nothing");
  }
  {
    Session session;
    bool neverStopped = true;
    for (int round = 0; round < 8; ++round) {
      session.readings.insufficientBalance = true;
      session.Sample();
      neverStopped = session.RunUntilDead(2 * kDead.deadSlowMillis) == -1 && neverStopped;
      session.readings.insufficientBalance = false;
      session.Sample();
      neverStopped = session.RunUntilDead(kDead.deadSlowMillis / 2) == -1 && neverStopped;
    }
    Check(neverStopped, "a small grant and a refusal taking turns near zero cost no stop");
  }
}

void TestAStaleOrMissingSignalKeepsTheHold() {
  {
    Session session;
    session.readings.insufficientBalance = true;
    session.Sample();
    session.Run(60000);
    session.readings.insufficientBalance = false;
    session.readings.generation = 2;
    session.Sample();
    Check(session.RunUntilDead(kLong) == -1,
          "a reset by a new destination, with nothing admitted, keeps the hold");
    Check(!session.armedEver, "...with no countdown");
    session.readings.added = 1;
    session.Sample();
    Check(session.Step().balanceStep == BalanceStep::Released,
          "a provider admitted past the reset ends it");
    DeadTunnelReason reason = DeadTunnelReason::None;
    Check(session.RunUntilDead(kLong, &reason) == kDead.deadSlowMillis &&
              reason == DeadTunnelReason::NoExit,
          "...and that provider going silent is a dead tunnel in balance, stopped");
  }
  {
    Session session;
    session.readings.insufficientBalance = true;
    session.Sample();
    session.Run(60000);
    session.readings.balanceRead = false;
    session.readings.insufficientBalance = false;
    session.balance = BalanceReading{};
    session.Sample();
    Check(session.RunUntilDead(kLong) == -1, "a status that can no longer be read keeps the hold");
    Check(!session.armedEver, "...with no countdown");
  }
  {
    Session session;
    session.readings.insufficientBalance = true;
    session.Sample();
    session.Run(60000);
    session.readings.insufficientBalance = false;
    session.readings.noPermission = true;
    session.Sample();
    Check(session.RunUntilDead(kLong) == -1,
          "no-permission results pushing the refusal out of the status keep the hold");
    session.readings.noPermission = false;
    session.Sample();
    Check(session.Step().balanceStep == BalanceStep::Released, "a grant, which clears both, ends it");
  }
  {
    Session session;
    FormAndCarry(session);
    session.readings.insufficientBalance = true;
    session.readings.proven = 0;
    session.readings.inPerTick = 0;
    session.Sample();
    Check(session.RunUntilDead(kLong) == -1,
          "providers admitted before the refusal, still counted Added, keep the hold");
  }
  {
    Session session;
    session.readings.insufficientBalance = true;
    session.Sample();
    session.Run(60000);
    const Tick frozen = session.Step(2 * kWatchdog.frozenMillis);
    Check(frozen.outOfBalance, "a resume restarts the windows and keeps the hold");
    Check(session.RunUntilDead(kLong) == -1, "...and nothing fires after it");
  }
}

void TestTheBalanceUnknown() {
  {
    Session session;
    session.readings.balanceRead = false;
    session.Sample();
    DeadTunnelReason reason = DeadTunnelReason::None;
    Check(session.RunUntilDead(kLong, &reason) == kDead.deadSlowMillis &&
              reason == DeadTunnelReason::NoExit,
          "a status never readable is judged by the rules: that is the dead tunnel this is for");
  }
  {
    Session session;
    session.Sample();
    Check(session.RunUntilDead(kDead.deadSlowMillis - 2 * kWatchdog.sampleIntervalMillis) == -1,
          "no verdict ahead of the window");
    session.readings.insufficientBalance = true;
    session.Sample();
    Check(session.RunUntilDead(kLong) == -1, "a refusal ahead of the window is in time");
  }
}

void TestAHungSdkStillStops() {
  for (const bool outOfBalance : {true, false}) {
    Session session;
    session.readings.insufficientBalance = outOfBalance;
    session.readings.proven = 2;
    session.readings.added = 2;
    session.readings.minSatisfied = true;
    session.Sample();
    session.Run(10000);
    session.readings.answering = false;
    DeadTunnelReason reason = DeadTunnelReason::None;
    const int64_t at = session.RunUntilDead(kLong, &reason);
    Check(reason == DeadTunnelReason::SdkUnresponsive &&
              at >= kDead.sdkUnresponsiveMillis - kWatchdog.sampleIntervalMillis &&
              at <= kDead.sdkUnresponsiveMillis,
          std::string("a wedged sdk stops the session, ") +
              (outOfBalance ? "out of balance" : "in balance"));
    Check(session.armedEver, "...with its countdown");
  }
}

void TestTheVerdictAlone() {
  DeadTunnelSignals s;
  s.tunnelUp = true;
  s.routesInstalled = true;
  s.upSinceMillis = 1000;
  const int64_t now = 1000 + kDead.deadSlowMillis;
  s.lastSampleMillis = now;
  s.outboundSinceInbound = kDead.deadFastOutboundPackets;
  Check(Evaluate(s, now, kDead).reason == DeadTunnelReason::NoInbound, "in balance: NoInbound");
  s.outboundSinceInbound = 0;
  Check(Evaluate(s, now, kDead).reason == DeadTunnelReason::NoExit, "in balance: NoExit");
  s.outOfBalance = true;
  Check(Evaluate(s, now, kDead).reason == DeadTunnelReason::None && !Evaluate(s, now, kDead).armed,
        "out of balance: NoExit holds, with no countdown");
  s.outboundSinceInbound = kDead.deadFastOutboundPackets;
  Check(Evaluate(s, now, kDead).reason == DeadTunnelReason::None && !Evaluate(s, now, kDead).armed,
        "out of balance: NoInbound holds, with no countdown");
  s.lastSampleMillis = now - kDead.sdkUnresponsiveMillis;
  Check(Evaluate(s, now, kDead).reason == DeadTunnelReason::SdkUnresponsive,
        "out of balance: SdkUnresponsive fires");
  s.lastSampleMillis = now - kDead.sdkUnresponsiveArmMillis;
  const DeadTunnelVerdict armed = Evaluate(s, now, kDead);
  Check(armed.reason == DeadTunnelReason::None && armed.armed &&
            armed.millisToFailsafe == kDead.sdkUnresponsiveMillis - kDead.sdkUnresponsiveArmMillis,
        "out of balance: its countdown is the only one that runs");
}

// The notice's sinks. Disconnect is here only to prove it is never asked for.
struct Sinks {
  int notices = 0;
  int disconnects = 0;
  void Notice() { ++notices; }
  void Disconnect() { ++disconnects; }
};

// The whole of what the user is promised, for as long as the session is out of
// balance: the tray notice once, the banner saying the traffic is held, Disconnect
// on the button, and no warning that the tunnel is about to be turned off.
// Stopping the session would take all of it away and leave "held" untrue.
void TestTheAlertAndNoticeStayForTheWholeHold() {
  Session session;
  session.readings.insufficientBalance = true;
  session.Sample();
  balance::GateNoticeTracker tracker;
  Sinks sinks;
  bool sessionUp = true;
  bool warned = false;
  for (int64_t elapsed = 0; elapsed < kLong; elapsed += kWatchdog.evaluateIntervalMillis) {
    const Tick tick = session.Step();
    if (tick.verdict.reason != DeadTunnelReason::None) sessionUp = false;
    if (tick.verdict.armed) warned = true;
    balance::ReactToBalancePush(tracker, sessionUp, /*supporter=*/false, /*confirming=*/false, sinks);
  }
  Check(sinks.notices == 1, "the tray notice posts once");
  Check(sinks.disconnects == 0, "the reaction never disconnects");
  Check(sessionUp, "the session is still up for the banner to describe");
  Check(!warned, "no warning that the tunnel will be turned off");
  const bool outOfBalance = balance::OutOfBalance(sessionUp, false, false);
  Check(datainfo::BannerLinesFor(outOfBalance, sessionUp) == datainfo::BannerLines{true, true},
        "the banner says the traffic is held");
  const balance::ConnectButton button = balance::DecideConnectButton(
      /*failed=*/false, /*actionIsDisconnect=*/sessionUp, /*confirming=*/false, outOfBalance,
      /*transitional=*/false, /*watchdogFired=*/false);
  Check(button.action == balance::ConnectButtonAction::Disconnect && button.enabled,
        "the button offers a working Disconnect");
}

void TestTheHoldsTable() {
  const auto reading = [](bool insufficient, int64_t generation, int64_t added) {
    return BalanceReading{.sampled = true,
                          .insufficientBalance = insufficient,
                          .connectionGeneration = generation,
                          .providerAddedCount = added};
  };
  OutOfBalanceHold hold;
  Check(hold.Observe(BalanceReading{}) == BalanceStep::None &&
            hold.Observe(reading(false, 1, 0)) == BalanceStep::None &&
            hold.Observe(reading(false, 1, 3)) == BalanceStep::None && !hold.held(),
        "not told: an unread status and one that never rose hold nothing");
  Check(hold.Observe(reading(true, 1, 3)) == BalanceStep::Entered &&
            hold.Observe(reading(true, 1, 3)) == BalanceStep::None && hold.held(),
        "told, once");
  Check(hold.Observe(reading(false, 1, 0)) == BalanceStep::Released && !hold.held(),
        "a grant on the destination that was refused");
  Check(hold.Observe(reading(true, 1, 0)) == BalanceStep::Entered &&
            hold.Observe(reading(false, 2, 0)) == BalanceStep::None &&
            hold.Observe(BalanceReading{}) == BalanceStep::None && hold.held(),
        "a reset is not a grant, and an unreadable status is neither");
  BalanceReading noPermission = reading(false, 2, 1);
  noPermission.noPermission = true;
  Check(hold.Observe(noPermission) == BalanceStep::None && hold.held(),
        "nor is a refusal of another kind, even with a provider admitted");
  Check(hold.Observe(reading(false, 2, 1)) == BalanceStep::Released && !hold.held(),
        "a provider admitted past the reset is");
  Check(hold.Observe(reading(true, 2, 1)) == BalanceStep::Entered &&
            hold.Observe(reading(false, 3, 0)) == BalanceStep::None &&
            hold.Observe(reading(true, 3, 0)) == BalanceStep::None &&
            hold.Observe(reading(false, 3, 0)) == BalanceStep::Released,
        "a refusal on the newer destination moves the bar with it");
}

}  // namespace

int main() {
  TestOutOfBalanceIsNotStopped();
  TestInBalanceIsStoppedAsDocumented();
  TestTheBalanceReturning();
  TestAStaleOrMissingSignalKeepsTheHold();
  TestTheBalanceUnknown();
  TestAHungSdkStillStops();
  TestTheVerdictAlone();
  TestTheAlertAndNoticeStayForTheWholeHold();
  TestTheHoldsTable();
  std::cout << (gCases - gFailures) << "/" << gCases << " dead tunnel verdict checks passed\n";
  return gFailures == 0 ? 0 : 1;
}
