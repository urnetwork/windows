// A start connect waiting on its balance (BalanceGate.h, StartConnectStep::
// FetchBalance): the gate fetches a stale balance, for up to the store's settle
// timeout, and asks again once the fetch settles. The connect views show the
// wait as connecting, so a tap never looks ignored meanwhile.
//
// One wait at a time. A repeated tap replaces the wait, and only the newest
// one asks again when the fetch settles: the store calls every settle it was
// handed (SubscriptionBalanceStore::FetchThen), so the older ones must not
// connect. A disconnect or a sign-out ends the wait, and it never connects
// then: the store settles its waiters when it stops at a sign-out, and the
// user's disconnect is the last thing they asked for.
//
// The views are told how the wait ended at the moment the gate decides: a
// refused start goes back to the reported status before the upgrade path
// opens, an admitted one keeps the connecting state into its connect with no
// step back in between, and one that waits again stays connecting.
//
// Pure, no Windows dependencies; tools/start-connect-wait-tests.cpp is the
// spec. Not safe for concurrent use: SdkHost uses it on the UI thread, where
// every connect entry point, the fetch's settle, Disconnect and Logout run.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>

namespace urnw {

// What the connect views are told about a start waiting on its balance.
enum class StartWaitChange {
  Waiting,   // it waits: show connecting
  Admitted,  // the gate let it start: keep connecting, its connect follows
  Ended,     // refused, cancelled or dropped: back to the reported status
};

class StartConnectWait {
 public:
  // A start begins waiting, replacing any earlier wait. The token is for
  // Settle.
  std::uint64_t Begin() {
    waiting_ = true;
    settling_ = false;
    return ++generation_;
  }

  // The fetch handed `token` settled. True when that start is still the wait,
  // which ends here and asks again; false for one replaced or cancelled since.
  bool Settle(std::uint64_t token) {
    if (!waiting_ || token != generation_) return false;
    waiting_ = false;
    settling_ = true;
    return true;
  }

  // A disconnect or a sign-out. True when a start was waiting, so the views go
  // back to the reported status. A start already asking again is left to the
  // gate's answer: on this one thread only its own gesture can call here then
  // (a Retry disconnects before it connects).
  bool Cancel() {
    const bool was = waiting_;
    waiting_ = false;
    ++generation_;
    return was;
  }

  // The gate's answer to the start asking again, once: Admitted or Ended
  // while it asks, nothing otherwise (a fresh tap, or a wait already told).
  // A start that waits again is a new Begin, which tells Waiting itself.
  bool TakeOutcome(bool admitted, StartWaitChange& change) {
    if (!settling_) return false;
    settling_ = false;
    change = admitted ? StartWaitChange::Admitted : StartWaitChange::Ended;
    return true;
  }

  // The start asked again and never reached the gate (its entry point
  // returned first): Ended, once.
  bool TakeUnanswered(StartWaitChange& change) { return TakeOutcome(false, change); }

  bool Waiting() const { return waiting_; }

 private:
  std::uint64_t generation_ = 0;
  bool waiting_ = false;
  // between Settle and the gate's answer to the start asking again
  bool settling_ = false;
};

}  // namespace urnw
