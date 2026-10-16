// Executable spec for what an ending of the tray app does with the session
// worker's queued commands (Common/ExitDrain.h): the user's Disconnect is kept
// and runs before the exit, everything else queued is dropped, the wait ends at
// its cap, and Quit's stop and the host's destruction follow the result. A
// fake slot and a worker thread held at a barrier stand in for SdkHost's; no
// sleep decides anything. Run against the same header the app compiles, on
// any host with a C++20 compiler.
//
//   c++ -std=c++20 -I ../src/Common exit-drain-tests.cpp -o /tmp/exit-drain-tests && /tmp/exit-drain-tests
//
// SPDX-License-Identifier: MPL-2.0

#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "ExitDrain.h"

using namespace urnw::exitdrain;
using Clock = std::chrono::steady_clock;

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

// A gate a thread waits at until the test opens it.
class Gate {
 public:
  void Open() {
    std::scoped_lock lock(stateLock_);
    open_ = true;
    changed_.notify_all();
  }
  void Wait() {
    std::unique_lock<std::mutex> lock(stateLock_);
    changed_.wait(lock, [this] { return open_; });
  }

 private:
  std::mutex stateLock_;
  std::condition_variable changed_;
  bool open_ = false;
};

// SdkHost's session-request slot and its worker, in miniature: one request,
// last wins, a worker that serves the slot until it is empty and then exits
// under the slot's lock, notifying as SdkHost's does. The worker's first pass
// is held at `passHeld` (a pass inside an rpc), and every request it serves is
// recorded in `ran`.
class FakeHost {
 public:
  // The worker starts inside a pass of `first`, held until ReleasePass.
  explicit FakeHost(Queued first) {
    {
      std::scoped_lock lock(pendingMutex);
      workerAlive_ = true;
    }
    worker_ = std::thread([this, first] {
      passStarted_.Open();
      passHeld_.Wait();
      Record(first);
      for (;;) {
        Queued next;
        {
          std::scoped_lock lock(pendingMutex);
          if (!requested_) {
            workerAlive_ = false;
            changed.notify_all();
            return;
          }
          next = kind_;
          requested_ = false;
        }
        Record(next);
      }
    });
    passStarted_.Wait();
  }
  ~FakeHost() {
    passHeld_.Open();
    worker_.join();
  }

  // A request recorded while the worker is busy, as RequestSession records it.
  void Queue(Queued queued) {
    std::scoped_lock lock(pendingMutex);
    kind_ = queued;
    requested_ = true;
    changed.notify_all();
  }
  void ReleasePass() { passHeld_.Open(); }
  // Returns once an ending's wait has applied its drop and is waiting.
  void WaitUntilSettling() { waiting_.Wait(); }
  void JoinWorker() { worker_.join(); worker_ = std::thread([] {}); }

  std::vector<Queued> Ran() {
    std::scoped_lock lock(ranLock_);
    return ran_;
  }

  // The slot as SettleQueued reads it, under pendingMutex.
  struct Slot {
    FakeHost& host;
    bool Requested() { return host.requested_; }
    Queued Kind() { return host.kind_; }
    void Drop() { host.requested_ = false; host.dropped_.push_back(host.kind_); }
    // The wait's predicate, first read after the drop: the ending is waiting
    // from here, and the test may let the pass return.
    bool WorkerAlive() {
      host.waiting_.Open();
      return host.workerAlive_;
    }
  };

  std::vector<Queued> Dropped() {
    std::scoped_lock lock(pendingMutex);
    return dropped_;
  }

  std::mutex pendingMutex;
  std::condition_variable changed;

 private:
  void Record(Queued queued) {
    std::scoped_lock lock(ranLock_);
    ran_.push_back(queued);
  }

  bool requested_ = false;
  Queued kind_ = Queued::Ensure;
  bool workerAlive_ = false;
  std::vector<Queued> dropped_;
  std::mutex ranLock_;
  std::vector<Queued> ran_;
  Gate passStarted_;
  Gate passHeld_;
  Gate waiting_;
  std::thread worker_;
};

// SettleQueued on its own thread, so a negative control that waits too long
// is reported rather than hanging the run. The answer itself decides the case;
// the guard only bounds a broken build.
std::optional<bool> SettleOffThread(FakeHost& host, Clock::time_point deadline,
                                    std::function<void()> whileWaiting = nullptr) {
  auto settled = std::async(std::launch::async, [&host, deadline] {
    std::unique_lock<std::mutex> lock(host.pendingMutex);
    FakeHost::Slot slot{host};
    return SettleQueued(slot, lock, host.changed, deadline);
  });
  if (whileWaiting) {
    host.WaitUntilSettling();
    whileWaiting();
  }
  if (settled.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
    // a broken build waits past its deadline: let the pass return so the
    // wait, and the future's destructor with it, can end
    host.ReleasePass();
    return std::nullopt;
  }
  return settled.get();
}

void TestOnlyTheDisconnectIsKept() {
  Check(KeepsQueued(Queued::Disconnect), "keep: the user's Disconnect stays queued for the exit");
  Check(!KeepsQueued(Queued::Connect), "keep: a connect queued for an exiting app is dropped");
  Check(!KeepsQueued(Queued::Ensure), "keep: an ensure queued for an exiting app is dropped");
  Check(!KeepsQueued(Queued::Provider), "keep: a provider reconcile queued for an exiting app is dropped");
}

// The defect: a Disconnect tapped while a pass was in flight sat in the slot,
// and the ending emptied the slot before the worker reached it.
void TestADisconnectQueuedBehindAPassRunsBeforeTheExit() {
  FakeHost host(Queued::Connect);
  host.Queue(Queued::Disconnect);
  // the ending begins while the connect is still inside its rpc; the pass
  // returns while the ending waits
  const std::optional<bool> settled =
      SettleOffThread(host, Clock::now() + std::chrono::hours(1), [&host] { host.ReleasePass(); });
  Check(settled.has_value(), "disconnect: the ending's wait returned");
  Check(settled.value_or(false), "disconnect: the worker went idle within the cap");
  const std::vector<Queued> ran = host.Ran();
  Check(ran == std::vector<Queued>{Queued::Connect, Queued::Disconnect},
        "disconnect: the user's Disconnect runs after the pass in flight, before the exit");
  Check(host.Dropped().empty(), "disconnect: nothing the user asked for is dropped");
}

void TestAConnectQueuedForTheExitIsDropped() {
  FakeHost host(Queued::Disconnect);
  host.Queue(Queued::Connect);
  const std::optional<bool> settled =
      SettleOffThread(host, Clock::now() + std::chrono::hours(1), [&host] { host.ReleasePass(); });
  Check(settled.value_or(false), "connect: the worker went idle within the cap");
  Check(host.Ran() == std::vector<Queued>{Queued::Disconnect},
        "connect: a connect queued for an exiting app never runs");
  Check(host.Dropped() == std::vector<Queued>{Queued::Connect},
        "connect: the queued connect is dropped");
}

// A pass inside an rpc the service never answers: the wait ends at its
// deadline, here one already passed, and says the commands did not settle.
void TestTheWaitEndsAtItsCap() {
  FakeHost host(Queued::Connect);
  host.Queue(Queued::Disconnect);
  const std::optional<bool> settled = SettleOffThread(host, Clock::now() - std::chrono::milliseconds(1));
  Check(settled.has_value(), "cap: the wait returns at its deadline with the pass still held");
  Check(settled.has_value() && !*settled, "cap: a pass still in its rpc is reported unsettled");
  Check(host.Ran().empty(), "cap: nothing ran while the pass was held");
  host.ReleasePass();
}

void TestNothingQueuedSettlesAtOnce() {
  FakeHost host(Queued::Disconnect);
  host.ReleasePass();
  host.JoinWorker();
  const std::optional<bool> settled = SettleOffThread(host, Clock::now() - std::chrono::milliseconds(1));
  Check(settled.value_or(false), "idle: with the worker idle the ending goes on at once");
  Check(host.Dropped().empty(), "idle: nothing is dropped");
}

void TestTheBounds() {
  Check(kCommandsCap == std::chrono::milliseconds(5000),
        "bounds: the queued commands get 5 s, as on macOS");
  Check(kQuitCap == std::chrono::milliseconds(10000),
        "bounds: Quit returns within 10 s, the bound a sign-out of Windows waits on");
  Check(kCommandsCap < kQuitCap, "bounds: the service stop has time after the commands");
}

void TestTheStopRoute() {
  Check(StopRouteFor(true) == StopRoute::Locked,
        "stop: after the commands the stop takes the session lock, with the teardown");
  Check(StopRouteFor(false) == StopRoute::Unlocked,
        "stop: a pass still holding the session lock does not hold the service stop");
}

void TestTheHostOutlivesWhatRunsInIt() {
  Check(HostDestroyableAtExit(true, true), "host: destroyed at exit once nothing runs in it");
  Check(!HostDestroyableAtExit(false, true),
        "host: a pass still inside an rpc keeps the host alive to the process exit");
  Check(!HostDestroyableAtExit(true, false),
        "host: a quit still running keeps the host alive to the process exit");
  Check(!HostDestroyableAtExit(false, false), "host: neither settled, the host is kept");
}

void TestCompletion() {
  Completion completion;
  Check(!completion.Finished(), "completion: not finished before Finish");
  Check(!completion.WaitUntil(Clock::now() - std::chrono::milliseconds(1)),
        "completion: a wait past its deadline says not finished");
  Gate start;
  std::thread finisher([&] {
    start.Wait();
    completion.Finish();
  });
  start.Open();
  Check(completion.WaitUntil(Clock::now() + std::chrono::hours(1)),
        "completion: a wait returns once another thread finished it");
  finisher.join();
  Check(completion.Finished(), "completion: finished stays finished");
  Check(completion.WaitUntil(Clock::now() - std::chrono::milliseconds(1)),
        "completion: once finished, even a late wait says so");
}

void TestNames() {
  Check(std::string(ToString(Queued::Disconnect)) == "disconnect", "names: disconnect");
  Check(std::string(ToString(Queued::Connect)) == "connect", "names: connect");
  Check(std::string(ToString(Queued::Ensure)) == "ensure", "names: ensure");
  Check(std::string(ToString(Queued::Provider)) == "provider", "names: provider");
}

}  // namespace

int main() {
  TestOnlyTheDisconnectIsKept();
  TestADisconnectQueuedBehindAPassRunsBeforeTheExit();
  TestAConnectQueuedForTheExitIsDropped();
  TestTheWaitEndsAtItsCap();
  TestNothingQueuedSettlesAtOnce();
  TestTheBounds();
  TestTheStopRoute();
  TestTheHostOutlivesWhatRunsInIt();
  TestCompletion();
  TestNames();
  std::cout << (gCases - gFailures) << "/" << gCases << " exit drain checks passed\n";
  return gFailures == 0 ? 0 : 1;
}
