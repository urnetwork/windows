// Executable spec for a sign-out on a thread of its own (App/SignOutRun.h,
// owner report 2026-10-16): one sign-out at a time, a second click joins it; a
// sign-in during the tail waits and runs after it, so the tail never signs out
// the account that signed in after it began; a registration that began before
// the sign-out does not install its credential; and an exit can wait for the
// tail, bounded. Run on any host with a C++20 compiler:
//
//   c++ -std=c++20 -I ../src/App sign-out-run-tests.cpp -o /tmp/sor && /tmp/sor
//
// The host is modeled as SdkHost runs it (Logout's click, SignOutTail,
// RegisterNetworkClient's top and its callback), and those lines are pinned in
// SdkHost.cpp by tests/sign_out_run_test.go. The tail and the registration
// callback run where the test says, so every order is explicit; the threaded
// cases are released at a barrier and hold for any interleaving.
//
// SPDX-License-Identifier: MPL-2.0

#include <atomic>
#include <chrono>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "SignOutRun.h"

using urnw::signoutrun::SignOutRun;

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

// A sign-in waiting for the tail: its credential and who asked.
struct SignIn {
  std::string credential;
};

// SdkHost's sign-out and sign-in halves over a stored credential.
struct Host {
  SignOutRun<SignIn> run;
  std::string storedCredential = "old";
  bool loggedIn = true;
  int tails = 0;
  std::vector<std::string> events;

  // Logout, the click's half: a click while one runs joins it.
  bool Click() {
    if (!run.Begin()) {
      events.push_back("click joins");
      return false;
    }
    loggedIn = false;
    events.push_back("click");
    return true;
  }

  // SignOutTail: the local logout, then the signed-out state, then the
  // sign-in that waited.
  void Tail() {
    ++tails;
    storedCredential.clear();
    events.push_back("logged out");
    std::optional<SignIn> next = run.Finish();
    events.push_back("signed out");
    if (next) Register(next->credential);
  }

  // RegisterNetworkClient's top: nullopt when it waits for the tail, else the
  // epoch its callback compares.
  std::optional<std::uint64_t> BeginRegister(const std::string& credential) {
    std::optional<SignIn> replaced;
    if (run.Defer(SignIn{credential}, replaced)) {
      if (replaced) events.push_back("told " + replaced->credential + " it was replaced");
      events.push_back(credential + " waits");
      return std::nullopt;
    }
    return run.Epoch();
  }

  // The registration's callback, under the session lock.
  void FinishRegister(const std::string& credential, std::uint64_t epoch) {
    if (run.Epoch() != epoch) {
      events.push_back(credential + " dropped");
      return;
    }
    storedCredential = credential;
    loggedIn = true;
    events.push_back("signed in " + credential);
  }

  void Register(const std::string& credential) {
    if (auto epoch = BeginRegister(credential)) FinishRegister(credential, *epoch);
  }
};

void TestASecondClickJoinsTheSignOutRunning() {
  Host host;
  Check(host.Click(), "the first click starts a sign-out");
  Check(!host.Click(), "a second click joins the sign-out running");
  host.Tail();
  Check(host.tails == 1, "one tail for the two clicks");
  Check(host.Click(), "a click after the tail starts a new sign-out");
}

void TestASignInDuringTheTailWaitsForIt() {
  Host host;
  host.Click();
  host.Register("new");
  Check(host.storedCredential == "old", "nothing is installed while the tail is to come");
  host.Tail();
  Check(host.events == std::vector<std::string>{"click", "new waits", "logged out", "signed out",
                                                "signed in new"},
        "the sign-in runs after the tail's logout and signed-out state: " + Join(host.events));
  Check(host.storedCredential == "new" && host.loggedIn,
        "the account that signed in after the sign-out began is not signed out: " +
            host.storedCredential);
}

void TestANewerSignInReplacesAnOlderOneWaiting() {
  Host host;
  host.Click();
  host.Register("first");
  host.Register("second");
  host.Tail();
  Check(host.storedCredential == "second", "the newer sign-in is the one that runs");
  bool told = false;
  for (const auto& e : host.events) told = told || e == "told first it was replaced";
  Check(told, "an older sign-in waiting is told it was replaced: " + Join(host.events));
}

void TestARegistrationFromBeforeTheSignOutDoesNotInstall() {
  Host host;
  const auto epoch = host.BeginRegister("early");
  Check(epoch.has_value(), "a registration with no sign-out running goes ahead");
  host.Click();
  host.FinishRegister("early", *epoch);
  host.Tail();
  Check(host.storedCredential.empty() && !host.loggedIn,
        "a registration that began before the sign-out does not install its credential: " +
            Join(host.events));
}

void TestARegistrationAfterTheTailInstalls() {
  Host host;
  host.Click();
  host.Tail();
  host.Register("next");
  Check(host.storedCredential == "next" && host.loggedIn,
        "the next sign-in after the tail installs: " + Join(host.events));
}

void TestFinishAndDeferNeverLoseTheSignIn() {
  // Defer first: the tail hands it back.
  {
    SignOutRun<SignIn> run;
    run.Begin();
    std::optional<SignIn> replaced;
    Check(run.Defer(SignIn{"a"}, replaced), "a sign-in during the tail waits");
    const auto next = run.Finish();
    Check(next && next->credential == "a", "the tail hands back the sign-in that waited");
  }
  // Finish first: the sign-in goes ahead itself.
  {
    SignOutRun<SignIn> run;
    run.Begin();
    run.Finish();
    std::optional<SignIn> replaced;
    Check(!run.Defer(SignIn{"a"}, replaced), "a sign-in after the tail goes ahead itself");
  }
  // Released together: whichever order the two take, the sign-in is run once.
  for (int round = 0; round < 200; ++round) {
    SignOutRun<SignIn> run;
    run.Begin();
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    std::atomic<int> ran{0};
    std::thread signIn([&] {
      ready.fetch_add(1);
      while (!go.load()) std::this_thread::yield();
      std::optional<SignIn> replaced;
      if (!run.Defer(SignIn{"a"}, replaced)) ran.fetch_add(1);
    });
    std::thread tail([&] {
      ready.fetch_add(1);
      while (!go.load()) std::this_thread::yield();
      if (run.Finish()) ran.fetch_add(1);
    });
    while (ready.load() < 2) std::this_thread::yield();
    go.store(true);
    signIn.join();
    tail.join();
    if (ran.load() != 1) {
      Check(false, "a sign-in racing the tail runs exactly once");
      break;
    }
  }
}

void TestAnExitWaitsForTheTailBounded() {
  SignOutRun<SignIn> run;
  Check(run.WaitIdleUntil(std::chrono::steady_clock::now()), "no sign-out: an exit does not wait");
  run.Begin();
  Check(!run.WaitIdleUntil(std::chrono::steady_clock::now()),
        "a sign-out still running at the deadline is reported, not waited out");
  std::atomic<bool> released{false};
  std::thread waiter([&] {
    // a far deadline is only a guard: the tail's end releases the wait
    released.store(run.WaitIdleUntil(std::chrono::steady_clock::now() + std::chrono::seconds(30)));
  });
  run.Finish();
  waiter.join();
  Check(released.load(), "the tail's end releases an exit waiting for it");
}

}  // namespace

int main() {
  TestASecondClickJoinsTheSignOutRunning();
  TestASignInDuringTheTailWaitsForIt();
  TestANewerSignInReplacesAnOlderOneWaiting();
  TestARegistrationFromBeforeTheSignOutDoesNotInstall();
  TestARegistrationAfterTheTailInstalls();
  TestFinishAndDeferNeverLoseTheSignIn();
  TestAnExitWaitsForTheTailBounded();
  std::cout << (gCases - gFailures) << "/" << gCases << " sign-out run checks passed\n";
  return gFailures == 0 ? 0 : 1;
}
