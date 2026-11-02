// Executable spec for the insufficient-balance gate (Common/BalanceGate.h):
// the explicit connect button keeps a working Disconnect while the account is
// out of balance, the app never disconnects on its own, and the notice posts
// once per out-of-balance episode, never for a supporter or during a
// confirmation poll. A connect gesture from any entry point (the tray's
// Connect, a location row, the connect button) starts nothing out of balance
// and shows the upgrade path, including after the user's Disconnect resets the
// contract status (OutOfBalanceLatch); a session already connected is never
// dropped. The first connect after a launch on an empty account is blocked
// too, but only on a fresh balance: a stale one is fetched first, and a failed
// fetch never blocks. Out of balance the banner says whether the data is
// reserved or used up, and a connect the balance blocked is retried by itself
// once data is back (BalanceRecovery): once per recovery, at most three in a
// row, never for a user who did not ask. Run against the same header the app
// compiles, on any host with a C++20 compiler. No clocks: the time is an
// explicit input.
//
//   c++ -std=c++20 -I ../src/Common -I ../src/App balance-gate-tests.cpp -o /tmp/balance-gate-tests && /tmp/balance-gate-tests
//
// SPDX-License-Identifier: MPL-2.0

#include <functional>
#include <iostream>
#include <optional>
#include <string>

#include "BalanceGate.h"
#include "DataInfo.h"

using namespace urnw::balance;
using urnw::datainfo::AlertTitleKey;
using urnw::datainfo::TrafficLineKey;

namespace {

int gFailures = 0;
int gCases = 0;

void Check(bool condition, const std::string& what) {
  ++gCases;
  if (!condition) {
    ++gFailures;
    std::cout << "  FAIL " << what << "\n";
  }
}

const char* Name(ConnectButtonAction a) {
  switch (a) {
    case ConnectButtonAction::Connect:
      return "connect";
    case ConnectButtonAction::Disconnect:
      return "disconnect";
    case ConnectButtonAction::Retry:
      return "retry";
  }
  return "?";
}

// Counts what the reaction and the connect routing asked for.
struct FakeSinks {
  int notices = 0;
  int disconnects = 0;
  int connects = 0;
  int upgrades = 0;
  int dataLimitNotices = 0;
  void Notice() { ++notices; }
  void DataLimitNotice() { ++dataLimitNotices; }
  void Disconnect() { ++disconnects; }
  void Connect() { ++connects; }
  void Upgrade() { ++upgrades; }
};

constexpr long long kNow = 10'000'000;

// A balance read at `at` with `available` bytes left and nothing held.
AccountBalance Read(long long available, long long at = kNow, long long open = 0,
                    bool pro = false) {
  AccountBalance b;
  b.known = true;
  b.pro = pro;
  b.availableBytes = available;
  b.openTransferBytes = open;
  b.fetchedAtMs = at;
  return b;
}

// A funded account read just now: what a gate with no balance concern sees.
StartConnectFacts Funded() {
  StartConnectFacts f;
  f.balance = Read(1'000'000);
  f.nowMs = kNow;
  return f;
}

// A stand-in for SdkHost's connect entry points: each asks AdmitStartConnect
// before it records a session request, exactly as the app's do, and a
// FetchBalance holds the gesture until the test settles the fetch.
struct FakeHost {
  FakeSinks& sinks;
  StartConnectFacts facts = Funded();
  bool live = false;  // a session is up
  int sessionRequests = 0;
  int fetches = 0;
  std::function<void()> pending = nullptr;  // the gesture, asked again once the fetch settles
  void ConnectFromRow() {
    struct Gate {
      FakeHost& host;
      void Upgrade() { host.sinks.Upgrade(); }
      void FetchBalance() {
        ++host.fetches;
        host.pending = [h = &host] { h->ConnectFromRow(); };
      }
    } gate{*this};
    if (!AdmitStartConnect(facts, gate)) return;
    ++sessionRequests;
    live = true;
  }
  // The fetch settles now: a read balance, or nullopt when it failed or timed
  // out. Then the held gesture is asked again.
  void SettleFetch(std::optional<AccountBalance> read) {
    if (read) facts.balance = *read;
    facts.fetchSettled = true;
    facts.fetchSettledAtMs = facts.nowMs;
    auto again = std::move(pending);
    pending = nullptr;
    if (again) again();
  }
  // a reattach (launch resume, server change, watchdog) is not a connect
  void EnsureSession() { ++sessionRequests; }
};

void TestGate() {
  Check(OutOfBalance(true, false, false), "gate: insufficient, basic, not polling is gated");
  Check(!OutOfBalance(true, true, false), "gate: a supporter is never gated");
  Check(!OutOfBalance(true, false, true), "gate: a confirmation poll suspends the gate");
  Check(!OutOfBalance(false, false, false), "gate: a funded account is not gated");
}

// (a) the explicit Disconnect is offered and enabled in the gate
void TestConnectButton() {
  struct Case {
    const char* what;
    bool failed, actionIsDisconnect, confirming, outOfBalance, transitional, watchdogFired;
    ConnectButtonAction action;
    bool enabled;
  };
  const Case cases[] = {
      {"button: out of balance, connected: disconnect enabled", false, true, false, true,
       false, false, ConnectButtonAction::Disconnect, true},
      {"button: out of balance, connecting: disconnect enabled", false, true, false, true,
       true, false, ConnectButtonAction::Disconnect, true},
      {"button: out of balance, failed: disconnect, not a retry that cannot succeed", true,
       true, false, true, false, false, ConnectButtonAction::Disconnect, true},
      {"button: out of balance, disconnected: connect stays blocked", false, false, false,
       true, false, false, ConnectButtonAction::Connect, false},
      {"button: confirming, connected: disconnect enabled", false, true, true, false, false,
       false, ConnectButtonAction::Disconnect, true},
      {"button: confirming, disconnected: connect stays blocked", false, false, true, false,
       false, false, ConnectButtonAction::Connect, false},
      {"button: confirming, failed: retry stays blocked", true, true, true, false, false,
       false, ConnectButtonAction::Retry, false},
      // unchanged outside the gate
      {"button: funded, disconnected: connect", false, false, false, false, false, false,
       ConnectButtonAction::Connect, true},
      {"button: funded, connected: disconnect", false, true, false, false, false, false,
       ConnectButtonAction::Disconnect, true},
      {"button: funded, connecting: guarded until the watchdog", false, true, false, false,
       true, false, ConnectButtonAction::Disconnect, false},
      {"button: funded, connecting past the watchdog: enabled", false, true, false, false,
       true, true, ConnectButtonAction::Disconnect, true},
      {"button: funded, failed: retry", true, true, false, false, false, false,
       ConnectButtonAction::Retry, true},
  };
  for (const auto& c : cases) {
    const ConnectButton b = DecideConnectButton(c.failed, c.actionIsDisconnect, c.confirming,
                                                c.outOfBalance, c.transitional,
                                                c.watchdogFired);
    Check(b.action == c.action,
          std::string(c.what) + ": action " + Name(b.action) + ", want " + Name(c.action));
    Check(b.enabled == c.enabled, std::string(c.what) + ": enabled " +
                                      (b.enabled ? "true" : "false") + ", want " +
                                      (c.enabled ? "true" : "false"));
  }
}

// (b) no automatic disconnect, however long the gate holds
void TestNoAutoDisconnect() {
  GateNoticeTracker tracker;
  FakeSinks sinks;
  for (int push = 0; push < 1000; ++push) {
    ReactToBalancePush(tracker, true, false, false, sinks);
  }
  Check(sinks.disconnects == 0,
        "no auto-disconnect: 1000 out-of-balance pushes disconnected " +
            std::to_string(sinks.disconnects) + " times, want 0");
  // the gate clearing and re-entering does not disconnect either
  ReactToBalancePush(tracker, false, false, false, sinks);
  ReactToBalancePush(tracker, true, false, false, sinks);
  ReactToBalancePush(tracker, true, true, false, sinks);
  ReactToBalancePush(tracker, true, false, true, sinks);
  Check(sinks.disconnects == 0, "no auto-disconnect: across episodes, plan and poll changes");
}

// (c) the notice posts once per entry, re-arms after the episode ends, and
// never for a supporter or during a confirmation poll
void TestNotice() {
  {
    GateNoticeTracker tracker;
    FakeSinks sinks;
    ReactToBalancePush(tracker, true, false, false, sinks);
    Check(sinks.notices == 1, "notice: posted on entering the gate");
    for (int push = 0; push < 100; ++push) ReactToBalancePush(tracker, true, false, false, sinks);
    Check(sinks.notices == 1, "notice: never repeated within the episode");
    // a poll or a plan flip within the episode does not re-arm it
    ReactToBalancePush(tracker, true, false, true, sinks);
    ReactToBalancePush(tracker, true, false, false, sinks);
    ReactToBalancePush(tracker, true, true, false, sinks);
    ReactToBalancePush(tracker, true, false, false, sinks);
    Check(sinks.notices == 1, "notice: poll or plan changes within the episode do not repeat it");
    ReactToBalancePush(tracker, false, false, false, sinks);
    Check(sinks.notices == 1, "notice: not posted when the episode ends");
    ReactToBalancePush(tracker, true, false, false, sinks);
    Check(sinks.notices == 2, "notice: re-armed by the end of the episode");
  }
  {
    GateNoticeTracker tracker;
    FakeSinks sinks;
    for (int push = 0; push < 10; ++push) ReactToBalancePush(tracker, true, true, false, sinks);
    Check(sinks.notices == 0, "notice: never for a supporter");
    for (int push = 0; push < 10; ++push) ReactToBalancePush(tracker, true, false, true, sinks);
    Check(sinks.notices == 0, "notice: never during a confirmation poll");
    ReactToBalancePush(tracker, true, false, false, sinks);
    Check(sinks.notices == 1, "notice: posted once the gate holds within the same episode");
  }
  {
    GateNoticeTracker tracker;
    FakeSinks sinks;
    ReactToBalancePush(tracker, false, false, false, sinks);
    ReactToBalancePush(tracker, false, true, true, sinks);
    Check(sinks.notices == 0, "notice: never for a funded account");
  }
}

// (d) start connect: the tray's Connect out of balance starts nothing and
// shows the upgrade path. It used to call ConnectBestAvailable regardless.
void TestTrayConnectBlockedOutOfBalance() {
  {
    FakeSinks sinks;
    RouteConnectGesture(/*actionIsDisconnect=*/false, OutOfBalance(true, false, false), sinks);
    Check(sinks.connects == 0, "tray: connect out of balance started the tunnel " +
                                   std::to_string(sinks.connects) + " times, want 0");
    Check(sinks.upgrades == 1, "tray: connect out of balance shows the upgrade path: " +
                                   std::to_string(sinks.upgrades) + ", want 1");
    Check(sinks.disconnects == 0, "tray: a blocked connect does not disconnect");
  }
  struct Case {
    const char* what;
    bool actionIsDisconnect, insufficient, supporter, confirming;
    ConnectGestureAction action;
  };
  const Case cases[] = {
      {"gesture: out of balance, disconnected: upgrade", false, true, false, false,
       ConnectGestureAction::Upgrade},
      {"gesture: out of balance, connected: disconnect still works", true, true, false, false,
       ConnectGestureAction::Disconnect},
      {"gesture: supporter: connect", false, true, true, false, ConnectGestureAction::Connect},
      {"gesture: confirmation poll: connect", false, true, false, true,
       ConnectGestureAction::Connect},
      {"gesture: funded: connect", false, false, false, false, ConnectGestureAction::Connect},
      {"gesture: funded, connected: disconnect", true, false, false, false,
       ConnectGestureAction::Disconnect},
  };
  for (const auto& c : cases) {
    const ConnectGestureAction a = DecideConnectGesture(
        c.actionIsDisconnect, OutOfBalance(c.insufficient, c.supporter, c.confirming));
    Check(a == c.action, c.what);
  }
}

// (e) start connect: the connect-only entry points (location and peer rows,
// the connect button) out of balance record no session request
void TestStartConnectBlockedOutOfBalance() {
  FakeSinks sinks;
  FakeHost host{sinks};
  host.facts.latched = true;
  host.ConnectFromRow();
  Check(host.sessionRequests == 0, "row: connect out of balance requested a session " +
                                       std::to_string(host.sessionRequests) + " times, want 0");
  Check(!host.live, "row: connect out of balance started the tunnel");
  Check(sinks.upgrades == 1, "row: a blocked connect shows the upgrade path");
  host.facts.supporter = true;
  host.ConnectFromRow();
  Check(host.sessionRequests == 1 && host.live, "row: a supporter connects");
}

// (f) already connected: running out of balance never drops the session,
// a reattach is not gated, and only the user's Disconnect ends it
void TestAlreadyConnectedKept() {
  FakeSinks sinks;
  FakeHost host{sinks};
  host.ConnectFromRow();  // funded: connects
  Check(host.live, "connected: a funded connect starts");
  // the balance runs out while connected
  host.facts.latched = true;
  host.facts.balance = Read(0);
  GateNoticeTracker tracker;
  for (int push = 0; push < 1000; ++push) ReactToBalancePush(tracker, true, false, false, sinks);
  Check(sinks.disconnects == 0 && host.live,
        "connected: running out of balance dropped the session " +
            std::to_string(sinks.disconnects) + " times, want 0");
  // the app relaunches into the live session: a reattach, never blocked
  const int before = host.sessionRequests;
  host.EnsureSession();
  Check(host.sessionRequests == before + 1 && sinks.upgrades == 0,
        "connected: a reattach out of balance is not gated");
  // the tray offers Disconnect for the live session, not the upgrade path
  RouteConnectGesture(/*actionIsDisconnect=*/true, true, sinks);
  Check(sinks.disconnects == 1 && sinks.upgrades == 0,
        "connected: the user's Disconnect works out of balance");
}

// (g) held out of balance -> the user's Disconnect (the SDK resets the
// contract status to an empty one) -> tray Connect. The gate read the raw
// contract status, so the Connect started the tunnel again.
void TestTrayConnectAfterDisconnectStaysBlocked() {
  using Obs = OutOfBalanceLatch::Observation;
  OutOfBalanceLatch latch;
  FakeSinks sinks;
  // connected and funded, providers attached
  latch.Observe(Obs{false, true, true, 1000});
  // the balance runs out: traffic is held
  latch.Observe(Obs{true, false, true, 0});
  // the user's Disconnect: the contract status is reset to empty
  latch.Observe(Obs{false, false, true, 0});
  RouteConnectGesture(/*actionIsDisconnect=*/false,
                      OutOfBalance(latch.InsufficientBalance(), false, false), sinks);
  Check(sinks.connects == 0, "latch: tray Connect after Disconnect out of balance started the "
                             "tunnel " + std::to_string(sinks.connects) + " times, want 0");
  Check(sinks.upgrades == 1, "latch: tray Connect after Disconnect shows the upgrade path");
  // before the balance has ever been fetched, the reset still keeps the state
  OutOfBalanceLatch unknown;
  unknown.Observe(Obs{true, false, false, 0});
  unknown.Observe(Obs{false, false, false, 0});
  Check(unknown.InsufficientBalance(), "latch: kept across the reset with no balance read");
}

// (h) the latch ends only on evidence that the state is over
void TestOutOfBalanceLatchClears() {
  using Obs = OutOfBalanceLatch::Observation;
  {
    OutOfBalanceLatch latch;
    latch.Observe(Obs{false, false, true, 0});
    Check(!latch.InsufficientBalance(), "latch: a funded account never latches");
  }
  {
    OutOfBalanceLatch latch;
    latch.Observe(Obs{true, false, true, 100});
    latch.Observe(Obs{false, false, true, 50});   // fell further
    latch.Observe(Obs{false, false, true, 100});  // rose above the low (50)
    Check(!latch.InsufficientBalance(), "latch: cleared by a balance above the lowest seen");
  }
  {
    OutOfBalanceLatch latch;
    latch.Observe(Obs{true, false, true, 100});
    latch.Observe(Obs{false, false, true, 100});
    latch.Observe(Obs{false, false, false, 0});  // an unknown balance is no evidence
    Check(latch.InsufficientBalance(), "latch: kept by an unchanged or unknown balance");
    latch.Observe(Obs{false, true, true, 100});
    Check(!latch.InsufficientBalance(), "latch: cleared by providers attached on a live session");
  }
  {
    OutOfBalanceLatch latch;
    latch.Observe(Obs{true, false, true, 0});
    latch.Observe(Obs{true, true, true, 0});  // still reported: providers do not clear it
    Check(latch.InsufficientBalance(), "latch: held while the contract status reports it");
    latch.Reset();  // sign-in or sign-out
    Check(!latch.InsufficientBalance(), "latch: cleared by a sign-in or sign-out");
  }
  {
    // a supporter is never gated, latched or not
    OutOfBalanceLatch latch;
    latch.Observe(Obs{true, false, true, 0});
    Check(!OutOfBalance(latch.InsufficientBalance(), true, false),
          "latch: a supporter is not gated");
  }
}

// (i) a fresh launch on an account that is already empty: no contract status
// exists before the first connect, so the latch is clear and only the
// subscription balance can tell. The connect used to start the tunnel.
void TestFreshStartEmptyAccountBlocked() {
  FakeSinks sinks;
  FakeHost host{sinks};
  host.facts.balance = Read(0, kNow - 1'000);
  host.ConnectFromRow();
  Check(host.sessionRequests == 0 && !host.live,
        "fresh start: connect on an empty account started the tunnel " +
            std::to_string(host.sessionRequests) + " times, want 0");
  Check(sinks.upgrades == 1, "fresh start: an empty account shows the upgrade path");
  Check(host.fetches == 0, "fresh start: a fresh balance is not fetched again");
}

// (j) only a fresh balance blocks; a stale one is fetched first
void TestStaleBalanceFetchedFirst() {
  {
    // stale and empty, the fetch fails: fail open
    FakeSinks sinks;
    FakeHost host{sinks};
    host.facts.balance = Read(0, kNow - kFreshBalanceMs - 1);
    host.ConnectFromRow();
    Check(host.fetches == 1 && host.sessionRequests == 0,
          "stale: a stale empty balance is fetched before deciding");
    host.SettleFetch(std::nullopt);
    Check(host.sessionRequests == 1 && sinks.upgrades == 0,
          "stale: a failed fetch does not block the connect");
    Check(host.fetches == 1, "stale: a settled fetch is not repeated");
  }
  {
    // stale and empty, the fetch finds a funded account: connects
    FakeSinks sinks;
    FakeHost host{sinks};
    host.facts.balance = Read(0, kNow - 30 * 60'000);
    host.ConnectFromRow();
    host.SettleFetch(Read(5'000));
    Check(host.sessionRequests == 1 && sinks.upgrades == 0,
          "stale: a funded user is not sent to upgrade on an old empty reading");
  }
  {
    // stale and funded, the fetch finds it empty: blocked
    FakeSinks sinks;
    FakeHost host{sinks};
    host.facts.balance = Read(5'000, kNow - kFreshBalanceMs - 1);
    host.ConnectFromRow();
    host.SettleFetch(Read(0));
    Check(host.sessionRequests == 0 && sinks.upgrades == 1,
          "stale: a fetch that reads an empty account blocks");
  }
  {
    // never read this session (launch before the first fetch lands)
    FakeSinks sinks;
    FakeHost host{sinks};
    host.facts.balance = AccountBalance{};
    host.ConnectFromRow();
    Check(host.fetches == 1 && host.sessionRequests == 0,
          "unknown: an unread balance is fetched before deciding");
    host.SettleFetch(Read(0));
    Check(host.sessionRequests == 0 && sinks.upgrades == 1,
          "unknown: the fetched empty balance blocks");
  }
  {
    // fresh and funded: connects at once
    FakeSinks sinks;
    FakeHost host{sinks};
    host.facts.balance = Read(5'000, kNow - kFreshBalanceMs);
    host.ConnectFromRow();
    Check(host.sessionRequests == 1 && host.fetches == 0 && sinks.upgrades == 0,
          "fresh: a funded balance read 60s ago connects without a fetch");
  }
  // a balance from the future (a clock reset) is not fresh
  {
    StartConnectFacts f = Funded();
    f.balance = Read(0, kNow + 1);
    Check(DecideStartConnect(f) == StartConnectStep::FetchBalance,
          "fresh: a reading stamped after now is fetched again");
  }
}

// (k) what an exhausted account is: nothing available and nothing held
void TestAccountBalanceExhausted() {
  Check(AccountBalanceExhausted(Read(0)), "exhausted: nothing available or held");
  Check(AccountBalanceExhausted(Read(-5)), "exhausted: a negative balance");
  Check(!AccountBalanceExhausted(Read(1)), "exhausted: one byte left is not exhausted");
  Check(!AccountBalanceExhausted(Read(0, kNow, 100)),
        "exhausted: bytes held in open contracts are not exhausted");
  Check(!AccountBalanceExhausted(Read(0, kNow, 0, true)), "exhausted: Pro never is");
  Check(!AccountBalanceExhausted(AccountBalance{}), "exhausted: an unknown balance never is");
  StartConnectFacts f = Funded();
  f.balance = Read(0);
  f.supporter = true;
  Check(DecideStartConnect(f) == StartConnectStep::Connect, "start: a supporter connects");
  f.supporter = false;
  f.confirming = true;
  Check(DecideStartConnect(f) == StartConnectStep::Connect,
        "start: a confirmation poll bridging a purchase connects");
  f.confirming = false;
  f.latched = true;
  f.balance = Read(5'000, kNow - kFreshBalanceMs - 1);
  Check(DecideStartConnect(f) == StartConnectStep::Upgrade,
        "start: the latch blocks without waiting for a fetch");
}

// (l) a session that is up when the balance reads empty is kept: the fresh
// balance only ever decides a start connect
void TestLiveSessionKeptOnEmptyBalance() {
  FakeSinks sinks;
  FakeHost host{sinks};
  host.ConnectFromRow();
  Check(host.live, "live: a funded connect starts");
  host.facts.balance = Read(0);
  GateNoticeTracker tracker;
  for (int push = 0; push < 100; ++push) ReactToBalancePush(tracker, true, false, false, sinks);
  host.EnsureSession();
  Check(host.live && sinks.disconnects == 0 && sinks.upgrades == 0 && host.sessionRequests == 2,
        "live: an empty balance dropped or gated the live session");
  RouteConnectGesture(/*actionIsDisconnect=*/true, true, sinks);
  Check(sinks.disconnects == 1, "live: the user's Disconnect works on an empty balance");
}

// ---- reserved or used up, and recovering by itself ----------------------

constexpr long long kMinute = 60'000;
constexpr long long kGib = 1024LL * 1024 * 1024;
constexpr long long kLow = kRecoveryThresholdBytes - 1;
constexpr long long kBack = kRecoveryThresholdBytes;

using Recovery = BalanceRecovery<std::string>;

RecoveryStepKind Held(Recovery& r, long long available, long long at) {
  return r.Observe(/*gate=*/true, /*connectRequested=*/true, Read(available, at), at).kind;
}

RecoveryStepKind Disconnected(Recovery& r, long long available, long long at, bool gate = false) {
  return r.Observe(gate, /*connectRequested=*/false, Read(available, at), at).kind;
}

// (m) the banner says whether the data is reserved or used up
void TestOutOfBalanceKind() {
  // the reported case: barely used, but open (or abandoned) connections hold
  // the balance as Pending
  Check(OutOfBalanceKindFor(Read(0, kNow, 30 * kGib)) == OutOfBalanceKind::Reserved,
        "kind: a balance held as Pending is reserved");
  Check(OutOfBalanceKindFor(Read(kLow, kNow, kBack)) == OutOfBalanceKind::Reserved,
        "kind: reserved from the threshold");
  Check(OutOfBalanceKindFor(Read(0, kNow, 0)) == OutOfBalanceKind::Exhausted,
        "kind: nothing held is used up");
  Check(OutOfBalanceKindFor(Read(0, kNow, kLow)) == OutOfBalanceKind::Exhausted,
        "kind: less than the threshold held is used up");
  Check(OutOfBalanceKindFor(AccountBalance{}) == OutOfBalanceKind::Unknown,
        "kind: an unknown balance says neither");
  Check(OutOfBalanceKindFor(Read(0, kNow, 0, /*pro=*/true)) == OutOfBalanceKind::Unknown,
        "kind: Pro says neither");
  Check(OutOfBalanceKindFor(Read(kBack, kNow, 30 * kGib)) == OutOfBalanceKind::Unknown,
        "kind: a balance that reads available says neither");
}

// (n) the balance running out and coming back never connects a user who did
// not ask to connect
void TestRecoveryNeverConnectsWithoutAnAsk() {
  Recovery r;
  long long t = kNow;
  bool any = false;
  for (int round = 0; round < 5; ++round) {
    for (bool gate : {true, false}) {
      t += kMinute;
      any |= Disconnected(r, 0, t, gate) != RecoveryStepKind::None;
      t += kMinute;
      any |= Disconnected(r, kBack, t, gate) != RecoveryStepKind::None;
    }
    t += kMinute;
    any |= r.Observe(false, true, Read(kBack, t), t).kind != RecoveryStepKind::None;
  }
  Check(!any && !r.State().startWaiting, "recovery: connected a user who did not ask");
}

// (o) a refused start runs again once, when data is back, to what was asked
void TestRecoveryRefusedStartOnce() {
  Recovery r;
  r.StartRefused("de", kNow);
  Check(r.State().startWaiting, "recovery: a refused start waits");
  Check(Disconnected(r, 0, kNow + kMinute, true) == RecoveryStepKind::None,
        "recovery: an empty reading waits");
  const RecoveryStep<std::string> step = r.Observe(false, false, Read(kBack, kNow + 2 * kMinute),
                                                   kNow + 2 * kMinute);
  Check(step.kind == RecoveryStepKind::Start && step.target == "de",
        "recovery: data back runs the refused start to its target");
  Check(!r.State().startWaiting, "recovery: the start no longer waits once run");
  Check(Disconnected(r, kBack, kNow + 3 * kMinute) == RecoveryStepKind::None,
        "recovery: the refused start runs once");
}

// (p) a balance read before the block, or at the start of a hold, never
// retries
void TestRecoveryStaleReadingsNeverRetry() {
  Recovery r;
  r.StartRefused("de", kNow);
  Check(Disconnected(r, kBack, kNow - kMinute) == RecoveryStepKind::None,
        "recovery: a reading from before the block retried");
  Check(Disconnected(r, kBack, kNow + kMinute) == RecoveryStepKind::Start,
        "recovery: a reading after the block decides");

  Recovery hold;
  Check(hold.Observe(true, true, Read(kBack, kNow - 10 * kMinute), kNow).kind ==
            RecoveryStepKind::None,
        "recovery: the reading from before a hold rebuilt it");
  Check(Held(hold, 0, kNow + kMinute) == RecoveryStepKind::None, "recovery: the hold waits");
  Check(Held(hold, kBack, kNow + 2 * kMinute) == RecoveryStepKind::Rebuild,
        "recovery: a held connection is rebuilt when reserved data returns");
}

// (q) one retry per recovery, at most three in a row
void TestRecoveryOncePerRecoveryAndBounded() {
  Recovery r;
  Held(r, 0, kNow);
  Check(Held(r, kBack, kNow + kMinute) == RecoveryStepKind::Rebuild, "recovery: first rebuild");
  Check(Held(r, kBack, kNow + 2 * kMinute) == RecoveryStepKind::None &&
            Held(r, kBack, kNow + 3 * kMinute) == RecoveryStepKind::None,
        "recovery: rebuilt again without the balance running out and coming back");
  Held(r, 0, kNow + 4 * kMinute);
  Check(Held(r, kBack, kNow + 5 * kMinute) == RecoveryStepKind::Rebuild,
        "recovery: a second recovery rebuilds again");

  Recovery bounded;
  int rebuilds = 0;
  long long t = kNow;
  for (int round = 0; round < 10; ++round) {
    t += kMinute;
    Held(bounded, 0, t);
    t += kMinute;
    rebuilds += Held(bounded, kBack, t) == RecoveryStepKind::Rebuild ? 1 : 0;
  }
  Check(rebuilds == kRecoveryMaxRetries, "recovery: " + std::to_string(rebuilds) +
                                             " rebuilds in a row, want " +
                                             std::to_string(kRecoveryMaxRetries));
  Check(!bounded.State().retriesLeft, "recovery: no retries left after the bound");

  // a new ask from the user refills them
  t += kMinute;
  bounded.StartRefused("fr", t);
  Check(bounded.State().retriesLeft, "recovery: a new ask refills the retries");
  t += kMinute;
  Check(Held(bounded, kBack, t) == RecoveryStepKind::Start, "recovery: the new ask runs");

  // and so does a retried connection that stays out of the block
  Recovery up;
  t = kNow;
  for (int round = 0; round < kRecoveryMaxRetries; ++round) {
    t += kMinute;
    Held(up, 0, t);
    t += kMinute;
    Held(up, kBack, t);
  }
  const long long lastRetry = t;
  up.Observe(false, true, Read(kBack, t + 1), lastRetry + kMinute);
  Check(!up.State().retriesLeft, "recovery: refilled before the connection stayed up long enough");
  up.Observe(false, true, Read(kBack, lastRetry + kRecoveryBudgetResetMs),
             lastRetry + kRecoveryBudgetResetMs);
  Check(up.State().retriesLeft, "recovery: a connection that stayed up did not refill the retries");
}

// (r) Cancel and the user's Disconnect end the wait; a hold that ends by itself
// is not rebuilt; data is back only at the threshold; the latest refused start
// wins over the held connection
void TestRecoveryEndsAndEdges() {
  Recovery cancelled;
  cancelled.StartRefused("de", kNow);
  cancelled.Clear();
  Check(!cancelled.State().startWaiting &&
            Disconnected(cancelled, 0, kNow + kMinute) == RecoveryStepKind::None &&
            Disconnected(cancelled, kBack, kNow + 2 * kMinute) == RecoveryStepKind::None,
        "recovery: a cancelled start ran");

  Recovery disconnected;
  Held(disconnected, 0, kNow);
  disconnected.Clear();
  Check(Disconnected(disconnected, 0, kNow + kMinute, true) == RecoveryStepKind::None &&
            Disconnected(disconnected, kBack, kNow + 2 * kMinute, true) == RecoveryStepKind::None,
        "recovery: a held connection the user disconnected was reconnected");

  Recovery recovered;
  Held(recovered, 0, kNow);
  recovered.Observe(false, true, Read(kBack, kNow + kMinute), kNow + kMinute);
  Check(recovered.Observe(false, true, Read(kBack, kNow + 2 * kMinute), kNow + 2 * kMinute).kind ==
            RecoveryStepKind::None,
        "recovery: a hold that ended by itself was rebuilt");

  Recovery threshold;
  threshold.StartRefused("", kNow);
  Check(Disconnected(threshold, kLow, kNow + kMinute) == RecoveryStepKind::None,
        "recovery: data counted back below the threshold");
  Check(threshold.Observe(true, false, AccountBalance{}, kNow + 2 * kMinute).kind ==
            RecoveryStepKind::None,
        "recovery: an unknown balance retried");
  Check(Disconnected(threshold, kBack, kNow + 3 * kMinute) == RecoveryStepKind::Start,
        "recovery: data back at the threshold");

  Recovery latest;
  Held(latest, 0, kNow);
  latest.StartRefused("jp", kNow + kMinute / 2);
  const RecoveryStep<std::string> step =
      latest.Observe(true, true, Read(kBack, kNow + kMinute), kNow + kMinute);
  Check(step.kind == RecoveryStepKind::Start && step.target == "jp",
        "recovery: the latest refused start wins over the held connection");
}

// (s) the app's wiring in miniature: the gate's Upgrade records the refused
// gesture, and the recovery runs it again past the gate once data is back
void TestRecoveryRunsTheRefusedGesturePastTheGate() {
  FakeSinks sinks;
  FakeHost host{sinks};
  BalanceRecovery<std::function<void()>> recovery;
  bool bypass = false;  // SdkHost::RetryRefusedConnect
  std::function<void()> connect;
  connect = [&] {
    struct Gate {
      FakeHost& host;
      BalanceRecovery<std::function<void()>>& recovery;
      std::function<void()>& again;
      void Upgrade() {
        host.sinks.Upgrade();
        recovery.StartRefused(again, host.facts.nowMs);
      }
      void FetchBalance() {}
    } gate{host, recovery, connect};
    if (!bypass && !AdmitStartConnect(host.facts, gate)) return;
    ++host.sessionRequests;
    host.live = true;
  };
  host.facts.latched = true;
  connect();
  Check(sinks.upgrades == 1 && host.sessionRequests == 0 && recovery.State().startWaiting,
        "wiring: the refused gesture does not wait on the balance");
  host.facts.nowMs += kMinute;
  RecoveryStep<std::function<void()>> step =
      recovery.Observe(true, false, Read(0, host.facts.nowMs), host.facts.nowMs);
  Check(step.kind == RecoveryStepKind::None && host.sessionRequests == 0,
        "wiring: an empty balance ran the gesture");
  host.facts.nowMs += kMinute;
  step = recovery.Observe(true, false, Read(30 * kGib, host.facts.nowMs), host.facts.nowMs);
  if (step.kind == RecoveryStepKind::Start && step.target) {
    bypass = true;
    step.target();
    bypass = false;
  }
  Check(host.sessionRequests == 1 && host.live && sinks.upgrades == 1,
        "wiring: data back did not run the refused gesture past the gate");
}

// (t) the banner's lines for the kind and the recovery, and when it opens
void TestRecoveryLines() {
  const RecoveryState waiting{false, true};
  const RecoveryState startWaiting{true, true};
  const RecoveryState spent{false, false};
  for (OutOfBalanceKind kind :
       {OutOfBalanceKind::Unknown, OutOfBalanceKind::Reserved, OutOfBalanceKind::Exhausted}) {
    Check(RecoveryLinesFor(true, true, kind, std::nullopt).kind == kind,
          "lines: the kind shows while out of balance");
    Check(RecoveryLinesFor(false, false, kind, std::nullopt).kind == OutOfBalanceKind::Unknown,
          "lines: the kind shows outside the gate");
  }
  Check(RecoveryLinesFor(true, true, OutOfBalanceKind::Reserved, waiting) ==
            RecoveryLines{OutOfBalanceKind::Reserved, true, false},
        "lines: a held connection is promised a reconnect, with no Cancel");
  Check(RecoveryLinesFor(false, false, OutOfBalanceKind::Unknown, startWaiting) ==
            RecoveryLines{OutOfBalanceKind::Unknown, true, true},
        "lines: a refused start waits with Cancel outside the gate");
  Check(!RecoveryLinesFor(true, false, OutOfBalanceKind::Exhausted, waiting).willReconnect,
        "lines: promised a reconnect with nothing waiting");
  Check(!RecoveryLinesFor(true, true, OutOfBalanceKind::Exhausted, spent).willReconnect &&
            !RecoveryLinesFor(true, true, OutOfBalanceKind::Exhausted, std::nullopt).willReconnect,
        "lines: promised a reconnect with no retries left");
  Check(BannerOpen(true, std::nullopt) && BannerOpen(false, startWaiting) &&
            !BannerOpen(false, waiting) && !BannerOpen(false, std::nullopt),
        "lines: the banner opens out of balance and for a waiting start only");
}

// What the alert says about the traffic is what the service reports: the routes
// decide whether anything is held, and with none the firewall decides whether
// the machine is blocked or out in the clear.
void TestTrafficLineSaysWhatTheServiceReports() {
  Check(TrafficLineFor(true, true, true) == TrafficLine::Held,
        "traffic line: captured with a firewall policy is held");
  Check(TrafficLineFor(true, true, false) == TrafficLine::Held,
        "traffic line: captured with no firewall policy is still held");
  Check(TrafficLineFor(true, false, false) == TrafficLine::NotProtected,
        "traffic line: a session with no capture and no policy is not protected");
  Check(TrafficLineFor(true, false, true) == TrafficLine::Blocked,
        "traffic line: a session with no capture under a policy is blocked");
  Check(TrafficLineFor(false, false, false) == TrafficLine::None,
        "traffic line: no session says nothing about the traffic");
  Check(TrafficLineFor(false, true, true) == TrafficLine::None,
        "traffic line: no session says nothing, whatever is left installed");
}

bool Same(const char* a, const std::string& b) { return a != nullptr && b == a; }

// The strings for those lines, one wording for each reason.
void TestTrafficLineStrings() {
  Check(Same(TrafficLineKey(false, TrafficLine::Held), "insufficient_balance_held_notice"),
        "strings: out of balance and held names the upgrade");
  Check(Same(TrafficLineKey(true, TrafficLine::Held), "data_limit_held_notice"),
        "strings: at a data limit and held offers no upgrade");
  for (bool dataLimit : {false, true}) {
    Check(Same(TrafficLineKey(dataLimit, TrafficLine::NotProtected), "conn_not_protected"),
          "strings: no capture and no policy says it is not protected");
    Check(Same(TrafficLineKey(dataLimit, TrafficLine::Blocked), "conn_traffic_blocked"),
          "strings: no capture under a policy says it is blocked, not exposed");
  }
  Check(Same(TrafficLineKey(false, TrafficLine::None), "insufficient_balance_message"),
        "strings: out of balance with no session asks for balance");
  Check(TrafficLineKey(true, TrafficLine::None) == nullptr,
        "strings: a data limit with no session has nothing to ask for");
  Check(Same(AlertTitleKey(false), "insufficient_balance"), "strings: the balance's title");
  Check(Same(AlertTitleKey(true), "data_limit_reached"), "strings: the data limit's title");
}

// The defect: a session that starts out of balance sits in Preparing, with no
// routes and no policy, and both surfaces told the user their traffic was held in
// the tunnel. The same decision serves both, so neither can say it now.
void TestSessionThatNeverCapturedIsNotToldItIsHeld() {
  const bool sessionUp = true;
  const bool routesInstalled = false;  // Preparing: no provider proof, no capture
  const bool firewallInForce = false;  // the kill switch is off
  FakeSinks sinks;
  GateNoticeTracker tracker;
  ReactToBalancePush(tracker, /*insufficientBalance=*/true, /*supporter=*/false,
                     /*confirming=*/false, sinks);
  Check(sinks.notices == 1, "never captured: the user is still told");
  const TrafficLine line = TrafficLineFor(sessionUp, routesInstalled, firewallInForce);
  Check(!Same(TrafficLineKey(false, line), "insufficient_balance_held_notice"),
        "never captured: the notice does not claim the traffic is held");
  Check(Same(TrafficLineKey(false, line), "conn_not_protected"),
        "never captured: the notice says the traffic is not protected");
  Check(Same(TrafficLineKey(false, TrafficLineFor(sessionUp, routesInstalled, true)),
             "conn_traffic_blocked"),
        "never captured, kill switch on: blocked, not unprotected");
  Check(sinks.disconnects == 0, "never captured: nothing disconnects");
}

void TestDataLimitIsAPayingAccountsRefusal() {
  {
    DataLimitGate gate;
    Check(gate.Observe(true, true, true, false), "data limit: Pro refused reads as the limit");
    Check(!OutOfBalance(true, true, false), "data limit: Pro stays outside the upgrade gate");
    Check(!gate.Observe(false, true, true, false), "data limit: ends with the refusal");
  }
  {
    DataLimitGate gate;
    Check(!gate.Observe(true, true, false, false), "data limit: not Pro is out of balance instead");
    Check(OutOfBalance(true, false, false), "data limit: not Pro is in the upgrade gate");
  }
  {
    DataLimitGate gate;
    Check(!gate.Observe(false, true, true, false), "data limit: Pro with no refusal is not one");
  }
  {
    DataLimitGate gate;
    Check(!gate.Observe(true, true, true, true), "data limit: a confirmation poll defers it");
    Check(gate.Observe(true, true, true, false), "data limit: and it reads as one after the poll");
  }
}

// The trap in the alert: a purchase while held turns the plan Pro with the old
// refusal still standing. That refusal was about the balance the purchase fixed.
void TestDataLimitIgnoresARefusalFromBeforePro() {
  DataLimitGate gate;
  Check(!gate.Observe(true, true, false, false), "upgrade: refused before Pro, no limit");
  Check(!gate.Observe(true, true, false, true), "upgrade: the confirmation poll, no limit");
  Check(!gate.Observe(true, true, true, true), "upgrade: Pro under the poll, no limit");
  Check(!gate.Observe(true, true, true, false),
        "upgrade: the old refusal after the purchase does not read as a limit");
  Check(!gate.Observe(true, true, true, false), "upgrade: nor on the next push");
  Check(!gate.Observe(false, true, true, false), "upgrade: the refusal clears");
  Check(gate.Observe(true, true, true, false),
        "upgrade: a refusal the Pro plan got is a limit, after the old one cleared");
}

// The plan not fetched yet is not a plan: the first balance of a launch is not
// an upgrade, whichever way it reads.
void TestDataLimitWaitsForTheFirstBalance() {
  {
    DataLimitGate gate;
    Check(!gate.Observe(true, false, false, false), "first balance: unknown plan, no limit yet");
    Check(gate.Observe(true, true, true, false),
          "first balance: Pro on the first reading is no upgrade, the refusal is a limit");
  }
  {
    DataLimitGate gate;
    Check(!gate.Observe(true, false, false, false), "first balance: unknown plan again");
    Check(!gate.Observe(true, true, false, false), "first balance: not Pro is out of balance");
    Check(!gate.Observe(true, true, true, false), "first balance: then a purchase, no limit");
  }
}

void TestDataLimitNoticeOncePerEpisodeAndNeverDisconnects() {
  FakeSinks sinks;
  GateNoticeTracker tracker;
  DataLimitGate gate;
  const auto push = [&](bool refused, bool known, bool pro, bool confirming) {
    ReactToDataLimitPush(tracker, refused, gate.Observe(refused, known, pro, confirming), sinks);
  };
  push(true, false, false, false);
  Check(sinks.dataLimitNotices == 0, "notice: not before the plan is known");
  push(true, true, true, true);
  Check(sinks.dataLimitNotices == 0, "notice: not during a confirmation poll");
  push(true, true, true, false);
  Check(sinks.dataLimitNotices == 1, "notice: posted for the limit");
  push(true, true, true, false);
  push(true, true, true, false);
  Check(sinks.dataLimitNotices == 1, "notice: once per episode");
  push(false, true, true, false);
  push(true, true, true, false);
  Check(sinks.dataLimitNotices == 2, "notice: a new refusal is a new episode");
  Check(sinks.notices == 0, "notice: the balance's notice stays out of it");
  Check(sinks.disconnects == 0, "notice: a data limit never disconnects");

  FakeSinks upgraded;
  GateNoticeTracker upgradedTracker;
  DataLimitGate upgradedGate;
  ReactToDataLimitPush(upgradedTracker, true, upgradedGate.Observe(true, true, false, false), upgraded);
  ReactToDataLimitPush(upgradedTracker, true, upgradedGate.Observe(true, true, true, false), upgraded);
  Check(upgraded.dataLimitNotices == 0, "notice: a purchase does not announce a data limit");
}

void TestDataLimitBannerHasNothingToBuy() {
  Check(!BannerOffersUpgrade(true), "banner: a data limit offers no upgrade");
  Check(BannerOffersUpgrade(false), "banner: out of balance keeps its upgrade");
  Check(BannerOpen(/*outOfBalance or data limit=*/true, RecoveryState{}), "banner: opens for the limit");
  const RecoveryLines lines = RecoveryLinesFor(/*outOfBalance=*/false, /*sessionUp=*/true,
                                               OutOfBalanceKind::Exhausted, RecoveryState{});
  Check(!lines.willReconnect, "banner: the limit promises no reconnect, nothing is waiting on it");
  Check(lines.kind == OutOfBalanceKind::Unknown, "banner: the limit says nothing about the balance");
}

}  // namespace

int main() {
  TestGate();
  TestConnectButton();
  TestNoAutoDisconnect();
  TestNotice();
  TestTrayConnectBlockedOutOfBalance();
  TestStartConnectBlockedOutOfBalance();
  TestAlreadyConnectedKept();
  TestTrayConnectAfterDisconnectStaysBlocked();
  TestOutOfBalanceLatchClears();
  TestFreshStartEmptyAccountBlocked();
  TestStaleBalanceFetchedFirst();
  TestAccountBalanceExhausted();
  TestLiveSessionKeptOnEmptyBalance();
  TestOutOfBalanceKind();
  TestRecoveryNeverConnectsWithoutAnAsk();
  TestRecoveryRefusedStartOnce();
  TestRecoveryStaleReadingsNeverRetry();
  TestRecoveryOncePerRecoveryAndBounded();
  TestRecoveryEndsAndEdges();
  TestRecoveryRunsTheRefusedGesturePastTheGate();
  TestRecoveryLines();
  TestTrafficLineSaysWhatTheServiceReports();
  TestTrafficLineStrings();
  TestSessionThatNeverCapturedIsNotToldItIsHeld();
  TestDataLimitIsAPayingAccountsRefusal();
  TestDataLimitIgnoresARefusalFromBeforePro();
  TestDataLimitWaitsForTheFirstBalance();
  TestDataLimitNoticeOncePerEpisodeAndNeverDisconnects();
  TestDataLimitBannerHasNothingToBuy();
  std::cout << (gCases - gFailures) << "/" << gCases << " balance gate checks passed\n";
  return gFailures == 0 ? 0 : 1;
}
