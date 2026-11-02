// The insufficient-balance gate: what the connect button offers while the
// account is out of balance, and how the app reacts to entering that state.
//
// Out of balance is a billing state, not a dropped tunnel. While the SDK still
// holds a destination the service keeps the capture routes it installed in
// place with no exit to carry them, so traffic is held in the tunnel. That is
// kept on purpose: the app never disconnects on its own, because falling back
// to the open internet without the user knowing would leak traffic outside the
// tunnel. Instead the user is told once per episode (a tray notice and the
// in-app banner) and the explicit connect button keeps a working Disconnect.
// A session that never captured has nothing held, and the notice and banner say
// what its traffic is doing instead (TrafficLineFor).
// It used to be disabled with the connect, which left the tray as the only
// way out. The round hero button is unchanged. The service's dead-tunnel failsafe
// holds for the same state, read from the same status (Service/DeadTunnelVerdict.h
// OutOfBalanceHold), so the capture stays there too.
//
// Two decisions, kept apart (owner decision 2026-10-03: "connect with no
// balance should be blocked; however, connected and then runs out of balance
// should keep the connection active"):
//   * START CONNECT. Every connect gesture (the connect button and hero, the
//     tray's Connect, a location or peer row) is admitted only outside the
//     gate. Blocked, it starts nothing and shows the upgrade path instead.
//     SdkHost's connect entry points all pass through AdmitStartConnect, so a
//     new surface cannot skip it. Out of balance there means the latch below
//     or a fresh account balance with nothing left (DecideStartConnect), so
//     the first connect after a launch on an empty account is blocked too.
//   * ALREADY CONNECTED. A live session is never dropped for balance: the
//     reaction below never disconnects, and a reattach (app launch resuming a
//     running session, a network-server change, the service watchdog) is not
//     a connect gesture and is never gated. Disconnect is always admitted.
//
// Pure, with no Windows headers or clocks (the caller passes the time), so
// tools/balance-gate-tests.cpp
// runs it on any host with a C++20 compiler.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <optional>
#include <utility>

namespace urnw::balance {

// Out of balance, not a supporter, and no confirmation poll bridging a
// just-made purchase.
inline constexpr bool OutOfBalance(bool insufficientBalance, bool supporter,
                                   bool confirming) {
  return insufficientBalance && !supporter && !confirming;
}

enum class ConnectButtonAction { Connect, Disconnect, Retry };

struct ConnectButton {
  ConnectButtonAction action = ConnectButtonAction::Connect;
  bool enabled = true;
};

// The explicit connect button (not the hero).
// failed: the rendered health is the terminal connect failure.
// actionIsDisconnect: gesture::ActionIsDisconnect for this instant.
// confirming / outOfBalance: the balance states layered over the connection.
// transitional / watchdogFired: the connecting duplicate-press guard.
inline constexpr ConnectButton DecideConnectButton(bool failed, bool actionIsDisconnect,
                                                   bool confirming, bool outOfBalance,
                                                   bool transitional,
                                                   bool watchdogFired) {
  ConnectButton b;
  // a retry reconnects, which cannot succeed out of balance; offer the way out
  if (failed && !outOfBalance) {
    b.action = ConnectButtonAction::Retry;
  } else if (actionIsDisconnect) {
    b.action = ConnectButtonAction::Disconnect;
  } else {
    b.action = ConnectButtonAction::Connect;
  }
  const bool blocked = confirming || outOfBalance;
  if (blocked && b.action == ConnectButtonAction::Disconnect) {
    // the connecting guard exists to stop a duplicate connect, not a disconnect
    b.enabled = true;
  } else {
    b.enabled = !blocked && (!transitional || watchdogFired);
  }
  return b;
}

// The out-of-balance state the start-connect gate reads. The live signal,
// ContractStatus.InsufficientBalance, does not survive a Disconnect: the SDK
// resets the contract status to an empty one on every destination change, the
// user's Disconnect included. Read raw, "held out of balance -> Disconnect ->
// tray Connect" saw a funded account and started the tunnel again, so the gate
// only held while a session was up, where the tray offers Disconnect anyway.
// The latch keeps the state until there is evidence it is over: providers
// attach on a live session, the subscription balance rises above the lowest
// value seen since it latched, or the session ends (Reset on sign-in/out).
// Matches the linux OutOfBalanceLatch.
class OutOfBalanceLatch {
 public:
  struct Observation {
    // ContractStatus.InsufficientBalance from the latest stats push
    bool insufficientBalance = false;
    // a session is up and the connect controller reports providers attached
    bool providersConnected = false;
    // the subscription balance has been fetched, and its available bytes
    bool balanceKnown = false;
    long long availableBytes = 0;
  };

  constexpr void Observe(const Observation& o) {
    if (o.insufficientBalance) {
      latched_ = true;
      NoteBalance(o);
      return;
    }
    if (!latched_) return;
    if (o.providersConnected) {
      Reset();
      return;
    }
    if (o.balanceKnown && lowKnown_ && lowBytes_ < o.availableBytes) {
      Reset();
      return;
    }
    NoteBalance(o);
  }

  constexpr void Reset() {
    latched_ = false;
    lowKnown_ = false;
    lowBytes_ = 0;
  }

  constexpr bool InsufficientBalance() const { return latched_; }

 private:
  constexpr void NoteBalance(const Observation& o) {
    if (!o.balanceKnown) return;
    if (!lowKnown_ || o.availableBytes < lowBytes_) {
      lowKnown_ = true;
      lowBytes_ = o.availableBytes;
    }
  }

  bool latched_ = false;
  bool lowKnown_ = false;
  long long lowBytes_ = 0;
};

// What a connect/disconnect gesture does: the tray's single toggle, and the
// connect button and hero. actionIsDisconnect (gesture::ActionIsDisconnect)
// wins, so a session that ran out of balance keeps its way out.
enum class ConnectGestureAction { Connect, Disconnect, Upgrade };

inline constexpr ConnectGestureAction DecideConnectGesture(bool actionIsDisconnect,
                                                           bool outOfBalance) {
  if (actionIsDisconnect) return ConnectGestureAction::Disconnect;
  if (outOfBalance) return ConnectGestureAction::Upgrade;
  return ConnectGestureAction::Connect;
}

// The subscription balance as last fetched. Times are monotonic milliseconds.
struct AccountBalance {
  bool known = false;  // fetched at least once this session
  bool pro = false;
  long long availableBytes = 0;
  long long openTransferBytes = 0;  // held by open contracts, returned unused
  long long fetchedAtMs = 0;
};

// A balance read this recently counts for a start connect; an older one is
// fetched again first. The background poll is 30s, and a balance can change
// on the web (a purchase, a redeem) at any time.
inline constexpr long long kFreshBalanceMs = 60'000;
// How long a start connect waits for that fetch before deciding without it.
inline constexpr long long kStartConnectFetchTimeoutMs = 5'000;

inline constexpr bool WithinFreshness(long long atMs, long long nowMs) {
  return atMs <= nowMs && nowMs - atMs <= kFreshBalanceMs;
}

// Nothing left: none available and none held in open contracts. Unknown and
// Pro are never exhausted. Matches android accountBalanceExhausted.
inline constexpr bool AccountBalanceExhausted(const AccountBalance& b) {
  return b.known && !b.pro && b.availableBytes <= 0 && b.openTransferBytes <= 0;
}

struct StartConnectFacts {
  bool latched = false;  // OutOfBalanceLatch::InsufficientBalance
  bool supporter = false;
  bool confirming = false;
  AccountBalance balance;
  // when the last balance fetch settled (succeeded, failed or timed out)
  bool fetchSettled = false;
  long long fetchSettledAtMs = 0;
  long long nowMs = 0;
};

enum class StartConnectStep { Connect, Upgrade, FetchBalance };

// The start-connect decision. The latch covers a session that ran out; the
// account balance covers a fresh launch on an empty account, where no contract
// status exists yet. Only a fresh balance blocks: a stale one is fetched first
// (FetchBalance), and when that fetch fails the connect starts (fail open: the
// server refuses the contract anyway, and the held state and its notice take
// over), so a funded user is never sent to upgrade on an old reading.
inline constexpr StartConnectStep DecideStartConnect(const StartConnectFacts& f) {
  if (OutOfBalance(f.latched, f.supporter, f.confirming)) return StartConnectStep::Upgrade;
  if (f.supporter || f.confirming) return StartConnectStep::Connect;
  if (f.balance.known && WithinFreshness(f.balance.fetchedAtMs, f.nowMs)) {
    return AccountBalanceExhausted(f.balance) ? StartConnectStep::Upgrade
                                              : StartConnectStep::Connect;
  }
  if (f.fetchSettled && WithinFreshness(f.fetchSettledAtMs, f.nowMs)) {
    return StartConnectStep::Connect;
  }
  return StartConnectStep::FetchBalance;
}

// The start-connect decision for an entry point that only connects. sinks
// provides Upgrade(), the upgrade path shown in place of a blocked connect,
// and FetchBalance(), which fetches the balance and asks again once the fetch
// settles. True when the connect may start now.
template <class Sinks>
constexpr bool AdmitStartConnect(const StartConnectFacts& facts, Sinks& sinks) {
  switch (DecideStartConnect(facts)) {
    case StartConnectStep::Connect:
      return true;
    case StartConnectStep::Upgrade:
      sinks.Upgrade();
      return false;
    case StartConnectStep::FetchBalance:
      sinks.FetchBalance();
      return false;
  }
  return false;
}

// Routes a toggle gesture. sinks provides Connect(), Disconnect() and
// Upgrade().
template <class Sinks>
constexpr void RouteConnectGesture(bool actionIsDisconnect, bool outOfBalance, Sinks& sinks) {
  switch (DecideConnectGesture(actionIsDisconnect, outOfBalance)) {
    case ConnectGestureAction::Disconnect:
      sinks.Disconnect();
      break;
    case ConnectGestureAction::Upgrade:
      sinks.Upgrade();
      break;
    case ConnectGestureAction::Connect:
      sinks.Connect();
      break;
  }
}

// Posts the out-of-balance notice once per episode. An episode is the span in
// which the contract reports insufficient balance; only its end re-arms the
// notice. A supporter or a running confirmation poll suppresses the notice
// until the gate actually holds within the same episode.
class GateNoticeTracker {
 public:
  // True exactly when the notice should be posted for this push.
  constexpr bool Update(bool insufficientBalance, bool supporter, bool confirming) {
    if (!insufficientBalance) {
      posted_ = false;
      return false;
    }
    if (posted_ || !OutOfBalance(insufficientBalance, supporter, confirming)) return false;
    posted_ = true;
    return true;
  }

 private:
  bool posted_ = false;
};

// The app's whole reaction to a balance or stats push. sinks provides
// Notice(), which posts the out-of-balance notice, and Disconnect(), the user
// Disconnect path. Disconnect is never called from here: capture stays until
// the user disconnects (see the file comment). It is part of the contract so
// the tests can pin that.
template <class Sinks>
constexpr void ReactToBalancePush(GateNoticeTracker& tracker, bool insufficientBalance,
                                  bool supporter, bool confirming, Sinks& sinks) {
  if (tracker.Update(insufficientBalance, supporter, confirming)) sinks.Notice();
}

// ---- what the traffic is doing while refused ---------------------------------
// "Held in the tunnel" is a claim about this machine's routes, and a session does
// not have them just because it asked. The service installs the capture only
// once a provider is proven (CaptureReadiness), so a session that starts out of
// balance stays in Preparing with nothing captured: its traffic goes out the
// normal way, or the kill switch's policy blocks it, and telling that user their
// traffic is held implies a protection they do not have. The banner and the tray
// notice read this one decision, from what the service reports, and its words
// are in App/DataInfo.h.
enum class TrafficLine {
  // no session is asking: nothing to say about the traffic
  None,
  // the capture routes are installed: traffic goes into the tunnel and waits
  Held,
  // no capture, and a firewall policy keeps the machine's traffic in
  Blocked,
  // no capture and no policy: traffic goes out the normal way
  NotProtected,
};

inline constexpr TrafficLine TrafficLineFor(bool sessionUp, bool routesInstalled,
                                            bool firewallInForce) {
  if (!sessionUp) return TrafficLine::None;
  if (routesInstalled) return TrafficLine::Held;
  return firewallInForce ? TrafficLine::Blocked : TrafficLine::NotProtected;
}

// ---- a paying account refused: the data limit ---------------------------------
// The server refuses a paying account with the same status it gives an empty
// one when a data limit stands in the way: the client's own data cap, or the
// plan's data. The gate above never applies to it (Pro is not sent to upgrade),
// so without this the account hears nothing. It gets the same kind of alert, in
// words for a limit, with no upgrade and no free refresh on offer.
//
// Pro and the status together do not always mean that. A refusal made before a
// purchase still stands right after it: the status changes only with the next
// contract result, and a held connection that sends nothing may not get one. So
// a refusal seen while the plan was known not to be Pro says nothing about the
// Pro plan until it clears, and the first balance of a launch is not an upgrade:
// a plan not fetched yet decides nothing.
//
// Fed the raw status, not the latch: the start gate needs the latch to outlive a
// Disconnect, and a paying account's start is not gated, so its alert ends with
// the refusal it describes.
class DataLimitGate {
 public:
  constexpr bool Observe(bool insufficientBalance, bool planKnown, bool pro, bool confirming) {
    if (!insufficientBalance) {
      refusedBeforePro_ = false;
      return false;
    }
    if (planKnown && !pro) refusedBeforePro_ = true;
    return planKnown && pro && !refusedBeforePro_ && !confirming;
  }

 private:
  bool refusedBeforePro_ = false;
};

// The notice for a data limit, once per episode of the raw status. sinks provides
// DataLimitNotice() and, as for ReactToBalancePush, a Disconnect() that is never
// called from here.
template <class Sinks>
constexpr void ReactToDataLimitPush(GateNoticeTracker& tracker, bool insufficientBalance,
                                    bool dataLimit, Sinks& sinks) {
  if (tracker.Update(insufficientBalance, !dataLimit, false)) sinks.DataLimitNotice();
}

// Get Pro and Why? explain a balance to refill. A paying account at a limit has
// neither to act on, and Disconnect is on the connect button.
inline constexpr bool BannerOffersUpgrade(bool dataLimit) { return !dataLimit; }

// ---- reserved or used up, and recovering by itself --------------------------
// Out of balance reads the same whether the balance is used up or only held as
// Pending by open (or abandoned) connections, which return what they do not
// use as they close. The banner says which (OutOfBalanceKindFor), and a
// connect the user asked for that the balance blocked recovers by itself
// (BalanceRecovery): a start connect the gate refused waits with the gesture,
// and a requested connection held out of balance waits too. Every balance
// reading is fed in, and once the available balance is back at
// kRecoveryThresholdBytes the connect is retried: the refused gesture runs
// again past the gate, or the held connection is rebuilt, which asks for new
// contracts and drops the latched status a held connection that sends nothing
// never clears. The same rules as android BalanceRecovery.kt.
//
// The retry is bounded:
//   * Once per recovery. A block arms it, and so does a reading below the
//     threshold; a retry disarms it. Only a reading fetched at or after the
//     arming counts, so a balance read before the block never retries.
//   * At most kRecoveryMaxRetries in a row. A new ask from the user refills
//     them, and so does a connection that stays out of the block for
//     kRecoveryBudgetResetMs after a retry.
// It never connects a user who did not ask: only a refused gesture or a
// connection already requested is retried, and the user's Disconnect, a
// sign-in or sign-out, another connect or Cancel ends the wait (Clear).

// The available balance at which data counts as back for a blocked connect,
// and the reserved balance whose return could bring it back. The server grants
// a contract down to 1 MiB, but a connection opens several at once and ramps
// each to 128 MiB, so a few MiB back would only block again.
inline constexpr long long kRecoveryThresholdBytes = 64LL * 1024 * 1024;
// Retries in a row before the user has to act again.
inline constexpr int kRecoveryMaxRetries = 3;
// How long a retried connection stays out of the block to refill the retries.
inline constexpr long long kRecoveryBudgetResetMs = 10LL * 60 * 1000;

enum class OutOfBalanceKind {
  // no balance known, Pro, or the balance reads available again: say neither
  Unknown,
  // enough is reserved (Pending) that its return could bring data back
  Reserved,
  // nothing meaningful is reserved: out until the free refresh or an upgrade
  Exhausted,
};

// Reserved or used up, by the same threshold the recovery waits for: reserved
// when at least that much is held by open connections. No time is promised
// for its return; connections close when they are used up or end.
inline constexpr OutOfBalanceKind OutOfBalanceKindFor(const AccountBalance& b) {
  if (!b.known || b.pro) return OutOfBalanceKind::Unknown;
  if (kRecoveryThresholdBytes <= b.availableBytes) return OutOfBalanceKind::Unknown;
  if (kRecoveryThresholdBytes <= b.openTransferBytes) return OutOfBalanceKind::Reserved;
  return OutOfBalanceKind::Exhausted;
}

// What the banner says about the recovery.
struct RecoveryState {
  // a refused start is waiting for the balance (Cancel clears it)
  bool startWaiting = false;
  // a retry is still allowed: "You'll be reconnected when data is available again."
  bool retriesLeft = true;
  constexpr bool operator==(const RecoveryState&) const = default;
};

enum class RecoveryStepKind {
  None,
  // run the refused gesture again (RecoveryStep::target), past the gate
  Start,
  // rebuild the held connection (connect again to its location)
  Rebuild,
};

template <class Target>
struct RecoveryStep {
  RecoveryStepKind kind = RecoveryStepKind::None;
  Target target{};
};

// Not thread safe; the app feeds it from the UI thread. Times are monotonic
// milliseconds, the clock AccountBalance::fetchedAtMs is stamped with.
template <class Target>
class BalanceRecovery {
 public:
  RecoveryState State() const { return {refusedStart_.has_value(), retries_ < kRecoveryMaxRetries}; }

  // The gate refused a start the user asked for: wait for the balance to run
  // it again. A new ask refills the retries.
  void StartRefused(Target target, long long nowMs) {
    refusedStart_ = std::move(target);
    retries_ = 0;
    Arm(nowMs);
  }

  // The user took the connect into their own hands (connected, disconnected,
  // signed in or out, or cancelled the wait): nothing is waiting any more.
  void Clear() {
    refusedStart_.reset();
    armedAtMs_.reset();
    retries_ = 0;
  }

  // One observation: whether the out-of-balance gate holds (OutOfBalance),
  // whether a connection is requested, and the last balance reading. Returns
  // the retry to make now, at most one per recovery.
  RecoveryStep<Target> Observe(bool gate, bool connectRequested, const AccountBalance& balance,
                               long long nowMs) {
    const bool heldNow = gate && connectRequested;
    if (heldNow && !held_) Arm(nowMs);  // a connection the user asked for is newly held
    held_ = heldNow;
    if (connectRequested && !heldNow && 0 < retries_ &&
        kRecoveryBudgetResetMs <= nowMs - lastRetryAtMs_) {
      retries_ = 0;
    }
    if (!refusedStart_ && !heldNow) {
      armedAtMs_.reset();
      return {};
    }
    if (!balance.known) return {};
    if (balance.availableBytes < kRecoveryThresholdBytes) {
      Arm(balance.fetchedAtMs);
      return {};
    }
    if (!armedAtMs_ || balance.fetchedAtMs < *armedAtMs_ || kRecoveryMaxRetries <= retries_) {
      return {};
    }
    armedAtMs_.reset();
    ++retries_;
    lastRetryAtMs_ = nowMs;
    RecoveryStep<Target> step;
    if (refusedStart_) {
      step.kind = RecoveryStepKind::Start;
      step.target = std::move(*refusedStart_);
      refusedStart_.reset();
    } else {
      step.kind = RecoveryStepKind::Rebuild;
    }
    return step;
  }

 private:
  void Arm(long long atMs) {
    if (!armedAtMs_) armedAtMs_ = atMs;
  }

  std::optional<Target> refusedStart_;
  bool held_ = false;
  std::optional<long long> armedAtMs_;
  int retries_ = 0;
  long long lastRetryAtMs_ = 0;
};

// The lines the out-of-balance banner adds under the refresh line
// (DataInfo.h BannerLinesFor): whether the data is reserved or used up while
// the banner is open, and the recovery's promise. A refused start waits
// outside the gate too (its gate can lift before data is back), so it opens
// the banner on its own (BannerOpen) with Cancel, the only way to stop it.
struct RecoveryLines {
  OutOfBalanceKind kind = OutOfBalanceKind::Unknown;
  // "You'll be reconnected when data is available again."
  bool willReconnect = false;
  // Cancel beside it: a refused start has no Disconnect to stop it
  bool cancel = false;
  constexpr bool operator==(const RecoveryLines&) const = default;
};

inline constexpr RecoveryLines RecoveryLinesFor(bool outOfBalance, bool sessionUp,
                                                OutOfBalanceKind kind,
                                                std::optional<RecoveryState> recovery) {
  const bool startWaiting = recovery && recovery->startWaiting;
  const bool willReconnect =
      recovery && recovery->retriesLeft && (startWaiting || (outOfBalance && sessionUp));
  return {outOfBalance ? kind : OutOfBalanceKind::Unknown, willReconnect,
          willReconnect && startWaiting};
}

// The banner shows while out of balance, and while a refused start waits.
inline constexpr bool BannerOpen(bool outOfBalance, std::optional<RecoveryState> recovery) {
  return outOfBalance || (recovery && recovery->startWaiting && recovery->retriesLeft);
}

}  // namespace urnw::balance
