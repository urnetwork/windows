// What an ending of the tray app (AppLifetime.h) does with the session
// worker's queued commands, and how long it waits for them. Apple parity
// (apple 57a39432, 2bffa3c8): every quit waits for the queued connect commands
// for at most 5 s and answers within 10 s, and nothing waits on a command held
// inside an rpc past that.
//
// The defect. A connect or disconnect runs on the session worker under the
// session lock, and one pass can hold it for a long time: a bootstrap's pipe
// calls wait up to 30 s each, and a DeviceRemote rpc holds the device's state
// lock up to the sdk's 60 s rpc timeout. A Disconnect tapped while a pass was
// in flight waited in the session-request slot, and every ending emptied the
// slot before it ran: Quit by its own drain, a close request (taskkill, an
// installer's WM_CLOSE or the Restart Manager) by the host's destructor. The
// service kept what the pass made, so after a close request the next launch
// reattached to a connection the user had ended. No ending bounded its wait
// either: Quit took the session lock behind the pass on the UI thread, inside
// WM_ENDSESSION for a sign-out of Windows or a shutdown, and the destructor
// took it again at exit.
//
// The rules:
//   * the user's Disconnect stays queued and runs before the app exits; a
//     connect, an ensure or a provider reconcile queued for an app that is
//     exiting is dropped, as before;
//   * the wait for the queued commands ends at kCommandsCap; a command still
//     inside an rpc then is left to the process exit;
//   * Quit's service stop follows the commands; with a pass still holding the
//     session lock at the cap it is sent without that lock first, as the tray's
//     force-stop sends it (the pipe serializes calls);
//   * Quit returns within kQuitCap whatever the service does;
//   * the host is destroyed at exit only when nothing runs inside it any more,
//     else it is left to the process exit.
//
// Pure and header-only, no Windows headers, like AppLifetime.h:
// tools/exit-drain-tests.cpp runs it on any host, and SdkHost and
// AppController::Shutdown follow it.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>

namespace urnw::exitdrain {

// A request waiting in the session-request slot, in this header's words
// (SdkHost::QueuedOf maps its ConnectKind here).
enum class Queued { Ensure, Connect, Disconnect, Provider };

// For logs.
constexpr const char* ToString(Queued queued) {
  switch (queued) {
    case Queued::Ensure: return "ensure";
    case Queued::Connect: return "connect";
    case Queued::Disconnect: return "disconnect";
    case Queued::Provider: return "provider";
  }
  return "unknown";
}

// Whether an ending keeps a queued request: only the user's Disconnect, so it
// is saved and the next launch does not come back connected. Nothing else may
// start anything for an app that is exiting.
constexpr bool KeepsQueued(Queued queued) { return queued == Queued::Disconnect; }

// An ending's wait for the session worker, with the slot's lock held in
// `lock`: a queued request the ending does not keep is dropped, then the wait
// for the worker to finish what is left (the user's Disconnect, and a pass in
// flight) ends at `deadline`. True when the worker went idle. `changed` is the
// slot's condition variable: the worker notifies it as it exits, and a drop
// notifies it so a worker sleeping out a row click's settle wakes to the empty
// slot. Slot reads and changes the slot under that lock:
//   bool Requested(), Queued Kind(), void Drop(), bool WorkerAlive().
template <class Slot>
bool SettleQueued(Slot& slot, std::unique_lock<std::mutex>& lock,
                  std::condition_variable& changed,
                  std::chrono::steady_clock::time_point deadline) {
  if (slot.Requested() && !KeepsQueued(slot.Kind())) {
    slot.Drop();
    changed.notify_all();
  }
  return changed.wait_until(lock, deadline, [&slot] { return !slot.WorkerAlive(); });
}

// The longest an ending waits for the queued commands. One the service
// answers returns in well under a second.
inline constexpr std::chrono::milliseconds kCommandsCap{5000};
// The longest Quit takes, the service stop included: the bound a sign-out of
// Windows or a shutdown waits on in WM_ENDSESSION.
inline constexpr std::chrono::milliseconds kQuitCap{10000};

// How Quit's service stop runs once the commands' wait is over.
enum class StopRoute {
  // Under the session lock, after the commands, with the app-side teardown.
  Locked,
  // A pass still holds the session lock inside an rpc: the stop goes to the
  // service without it first, and the locked stop follows if the pass returns
  // before the process ends.
  Unlocked,
};

constexpr StopRoute StopRouteFor(bool commandsSettled) {
  return commandsSettled ? StopRoute::Locked : StopRoute::Unlocked;
}

// Whether the host may be destroyed at exit: the session worker is idle and
// Quit, when it ran, has finished. A thread still inside the host owns it.
constexpr bool HostDestroyableAtExit(bool commandsSettled, bool quitFinished) {
  return commandsSettled && quitFinished;
}

// One piece of exit work, finished once, waited on with a deadline. Safe for
// concurrent use.
class Completion {
 public:
  void Finish() {
    std::scoped_lock lock(stateLock_);
    done_ = true;
    finished_.notify_all();
  }

  // True once finished, false at the deadline.
  bool WaitUntil(std::chrono::steady_clock::time_point deadline) {
    std::unique_lock<std::mutex> lock(stateLock_);
    return finished_.wait_until(lock, deadline, [this] { return done_; });
  }

  bool Finished() {
    std::scoped_lock lock(stateLock_);
    return done_;
  }

 private:
  std::mutex stateLock_;
  std::condition_variable finished_;
  bool done_ = false;
};

}  // namespace urnw::exitdrain
