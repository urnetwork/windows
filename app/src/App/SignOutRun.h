// A sign-out on a thread of its own (SdkHost::Logout), and the sign-ins it
// orders after it.
//
// A sign-out has two halves. The click's half, on the UI thread, is quick and
// takes no lock the session worker holds: the flows the account started are
// answered, a start waiting on its balance ends, the session-request slot
// empties and the app reads as signed out, so no connect is admitted
// (SignOutConnectFence.h). The tail waits for the session lock, which a pass in
// flight keeps through its rpcs up to the sdk's 60 s rpc timeout, then logs the
// local state out, clears the api credential, delivers the service's sign-out
// over the pipe and closes the device, and ends with the signed-out state. Run
// on the UI thread, the tail froze the window for as long as it waited.
//
// One sign-out at a time: a click while one runs joins it. A sign-in that would
// install its credential while the tail runs waits for the tail and runs after
// it, so the tail never logs out, clears the credential of or tears down an
// account that signed in after the sign-out began. A newer such sign-in
// replaces an older one, which is handed back to be told. A registration that
// began before the sign-out learns from the epoch, which every sign-out
// advances, not to install its credential.
//
// Pure, no Windows dependencies; tools/sign-out-run-tests.cpp is the spec.
// Safe for concurrent use: the click is on the UI thread, a sign-in on an sdk
// callback thread, and the tail on its own.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <utility>

namespace urnw::signoutrun {

template <class SignIn>
class SignOutRun {
 public:
  // The click. True when it starts a sign-out; false when one is running,
  // which the click joins.
  bool Begin() {
    std::scoped_lock lock(stateLock_);
    if (running_) return false;
    running_ = true;
    ++epoch_;
    return true;
  }

  // The tail is done. The sign-in that waited for it, if any, runs now.
  std::optional<SignIn> Finish() {
    std::optional<SignIn> next;
    {
      std::scoped_lock lock(stateLock_);
      running_ = false;
      next = std::exchange(deferred_, std::nullopt);
    }
    idle_.notify_all();
    return next;
  }

  // A sign-in about to install its credential. True when a sign-out runs: it
  // waits for the tail, and an older one waiting is handed back in `replaced`
  // to be told. False when none runs: it goes ahead now.
  bool Defer(SignIn signIn, std::optional<SignIn>& replaced) {
    std::scoped_lock lock(stateLock_);
    if (!running_) return false;
    replaced = std::exchange(deferred_, std::move(signIn));
    return true;
  }

  // Advanced by every sign-out: a registration read it when it began, and one
  // that finds it moved has been signed out.
  std::uint64_t Epoch() const {
    std::scoped_lock lock(stateLock_);
    return epoch_;
  }

  bool Running() const {
    std::scoped_lock lock(stateLock_);
    return running_;
  }

  // For an exit: true once no sign-out runs, false at the deadline.
  bool WaitIdleUntil(std::chrono::steady_clock::time_point deadline) {
    std::unique_lock lock(stateLock_);
    return idle_.wait_until(lock, deadline, [this] { return !running_; });
  }

 private:
  mutable std::mutex stateLock_;
  std::condition_variable idle_;
  bool running_ = false;
  std::uint64_t epoch_ = 0;
  std::optional<SignIn> deferred_;
};

}  // namespace urnw::signoutrun
