// Executable spec for a start connect waiting on its balance
// (App/StartConnectWait.h, owner report 2026-10-16): the gate fetches a stale
// balance and asks again once the fetch settles. The wait shows as connecting;
// a repeated tap does not stack; a disconnect or a sign-out during the wait
// ends it, and the start never connects then; a refused start goes back to the
// reported status before the upgrade path opens; an admitted one keeps
// connecting into its connect with no step back in between. Run on any host
// with a C++20 compiler:
//
//   c++ -std=c++20 -I ../src/App start-connect-wait-tests.cpp -o /tmp/scw && /tmp/scw
//
// The host is modeled as SdkHost runs it (AdmitStartConnect, WaitForBalance,
// TellStartWaitOutcome, CancelStartWait, Disconnect, Logout), and those lines
// are pinned in SdkHost.cpp by tests/start_connect_wait_test.go. The balance
// store is modeled as SubscriptionBalanceStore::FetchThen and Stop: it keeps
// every settle it was handed and calls them all when the fetch settles or the
// store stops at a sign-out. Nothing here waits on a clock.
//
// SPDX-License-Identifier: MPL-2.0

#include <functional>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "StartConnectWait.h"

using urnw::StartConnectWait;
using urnw::StartWaitChange;

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

std::string Join(const std::vector<std::string>& items) {
  std::string out;
  for (const std::string& item : items) {
    if (!out.empty()) out += ", ";
    out += item;
  }
  return "[" + out + "]";
}

const char* Name(StartWaitChange change) {
  switch (change) {
    case StartWaitChange::Waiting: return "waiting";
    case StartWaitChange::Admitted: return "admitted";
    case StartWaitChange::Ended: return "ended";
  }
  return "?";
}

// The gate's answer for this instant (BalanceGate.h DecideStartConnect).
enum class Gate { Connect, Upgrade, FetchBalance };

// SdkHost's start gate and its connect entry point, with the balance store.
// `events` records what the views were told and what reached the sdk, in
// order.
struct Host {
  StartConnectWait wait;
  Gate gate = Gate::FetchBalance;
  std::vector<std::function<void()>> storeWaiters;
  std::vector<std::string> events;

  void Tell(StartWaitChange change) { events.push_back(Name(change)); }

  // SdkHost::TellStartWaitOutcome
  void TellOutcome(bool admitted) {
    StartWaitChange change = StartWaitChange::Ended;
    if (wait.TakeOutcome(admitted, change)) Tell(change);
  }

  // SdkHost::WaitForBalance
  void WaitForBalance(std::function<void()> again) {
    const auto token = wait.Begin();
    Tell(StartWaitChange::Waiting);
    storeWaiters.push_back([this, token, again = std::move(again)] {
      if (!wait.Settle(token)) return;
      if (again) again();
      StartWaitChange change = StartWaitChange::Ended;
      if (wait.TakeUnanswered(change)) Tell(change);
    });
  }

  // SdkHost::AdmitStartConnect
  bool Admit(std::function<void()> again) {
    switch (gate) {
      case Gate::Connect:
        TellOutcome(true);
        return true;
      case Gate::Upgrade:
        TellOutcome(false);
        events.push_back("upgrade path");
        return false;
      case Gate::FetchBalance:
        WaitForBalance(std::move(again));
        return false;
    }
    return false;
  }

  // SdkHost::ConnectBestAvailable
  void Connect() {
    if (!Admit([this] { Connect(); })) return;
    events.push_back("connect");
  }

  // SdkHost::CancelStartWait
  void CancelWait() {
    if (wait.Cancel()) Tell(StartWaitChange::Ended);
  }

  // SdkHost::Disconnect
  void Disconnect() {
    CancelWait();
    events.push_back("disconnect");
  }

  // SdkHost::Logout, then the store's Stop at the LoggedOut push, which
  // settles every waiter (they ask again and find no session)
  void SignOut() {
    CancelWait();
    events.push_back("sign out");
    gate = Gate::Connect;  // with no session the gate admits
    SettleStore();
  }

  // the fetch settles: the store calls every waiter it holds
  void SettleStore() {
    auto waiters = std::move(storeWaiters);
    storeWaiters.clear();
    for (auto& waiter : waiters) waiter();
  }

  int Count(const std::string& event) const {
    int n = 0;
    for (const auto& e : events) n += e == event;
    return n;
  }
};

void TestATapWaitingOnItsBalanceShowsConnectingThenConnects() {
  Host host;
  host.Connect();
  Check(host.events == std::vector<std::string>{"waiting"},
        "a tap on a stale balance shows connecting at once: " + Join(host.events));
  Check(host.wait.Waiting(), "the start waits");
  host.gate = Gate::Connect;
  host.SettleStore();
  Check(host.events == std::vector<std::string>{"waiting", "admitted", "connect"},
        "admitted: connecting is kept into the connect, with no step back: " +
            Join(host.events));
  Check(!host.wait.Waiting(), "the wait is over");
}

void TestARepeatedTapDoesNotStack() {
  Host host;
  host.Connect();
  host.Connect();
  host.Connect();
  host.gate = Gate::Connect;
  host.SettleStore();
  Check(host.Count("connect") == 1,
        "three taps during one wait connect once: " + Join(host.events));
  Check(host.Count("admitted") == 1, "and say admitted once: " + Join(host.events));
}

void TestADisconnectDuringTheWaitEndsIt() {
  Host host;
  host.Connect();
  host.Disconnect();
  Check(host.events == std::vector<std::string>{"waiting", "ended", "disconnect"},
        "the disconnect ends the wait and the view goes back: " + Join(host.events));
  host.gate = Gate::Connect;
  host.SettleStore();
  Check(host.Count("connect") == 0,
        "the start never connects after the user's disconnect: " + Join(host.events));
}

void TestASignOutDuringTheWaitEndsIt() {
  Host host;
  host.Connect();
  host.SignOut();
  Check(host.Count("connect") == 0,
        "the start never connects after the sign-out, though the store settles every "
        "waiter as it stops: " +
            Join(host.events));
  Check(host.Count("ended") == 1, "the view goes back once: " + Join(host.events));
}

void TestARefusedStartGoesBackBeforeTheUpgradePath() {
  Host host;
  host.Connect();
  host.gate = Gate::Upgrade;
  host.SettleStore();
  Check(host.events == std::vector<std::string>{"waiting", "ended", "upgrade path"},
        "refused: back to the reported status before the upgrade path opens: " +
            Join(host.events));
  Check(host.Count("connect") == 0, "and nothing connects");
}

void TestAStartThatWaitsAgainStaysConnecting() {
  Host host;
  host.Connect();
  host.SettleStore();  // the gate still wants a fresh balance
  Check(host.events == std::vector<std::string>{"waiting", "waiting"},
        "waiting again keeps connecting, with no step back: " + Join(host.events));
  Check(host.wait.Waiting(), "the newer wait is the wait");
  host.gate = Gate::Connect;
  host.SettleStore();
  Check(host.Count("connect") == 1, "and it connects once: " + Join(host.events));
}

void TestARetryThroughTheWaitIsOneGesture() {
  Host host;
  // the connect page's Retry: its gesture disconnects, then connects
  host.WaitForBalance([&host] {
    host.Disconnect();
    host.Connect();
  });
  host.gate = Gate::Connect;
  host.SettleStore();
  Check(host.events ==
            std::vector<std::string>{"waiting", "disconnect", "admitted", "connect"},
        "a retry's own disconnect does not end the start it is part of: " +
            Join(host.events));
}

void TestAGestureThatReturnsFirstGoesBack() {
  Host host;
  // a re-click of the location already connected returns before the gate
  host.WaitForBalance([] {});
  host.gate = Gate::Connect;
  host.SettleStore();
  Check(host.events == std::vector<std::string>{"waiting", "ended"},
        "a start that never reaches the gate goes back once: " + Join(host.events));
}

void TestATapThatDidNotWaitTellsNothing() {
  Host host;
  host.gate = Gate::Connect;
  host.Connect();
  host.Disconnect();
  Check(host.events == std::vector<std::string>{"connect", "disconnect"},
        "a gesture that did not wait tells the views nothing: " + Join(host.events));
}

}  // namespace

int main() {
  TestATapWaitingOnItsBalanceShowsConnectingThenConnects();
  TestARepeatedTapDoesNotStack();
  TestADisconnectDuringTheWaitEndsIt();
  TestASignOutDuringTheWaitEndsIt();
  TestARefusedStartGoesBackBeforeTheUpgradePath();
  TestAStartThatWaitsAgainStaysConnecting();
  TestARetryThroughTheWaitIsOneGesture();
  TestAGestureThatReturnsFirstGoesBack();
  TestATapThatDidNotWaitTellsNothing();
  std::cout << (gCases - gFailures) << "/" << gCases << " start connect wait checks passed\n";
  return gFailures == 0 ? 0 : 1;
}
