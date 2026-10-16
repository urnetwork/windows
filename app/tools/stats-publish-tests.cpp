// Executable spec for how the app publishes and applies live stats snapshots:
// read on a worker of their own, never on the caller's thread
// (App/CoalescedWorker.h), and applied in the order their reads began
// (App/StatsReadOrder.h), so a read that finishes late cannot roll the window
// or the tray back to an older state. Run against the same headers the app
// compiles, on any host with a C++20 compiler:
//
//   c++ -std=c++20 -Wall -Wextra -Werror -pthread -I ../src/App stats-publish-tests.cpp -o /tmp/stats-publish-tests && /tmp/stats-publish-tests
//
// The worker cases hold the job at a barrier the spec releases; their waits are
// bounded only so a broken worker fails instead of hanging. SdkHost,
// AppController and ConnectPage cannot be built off Windows, so
// tests/stats_publish_test.go reads their source for the wiring.
//
// SPDX-License-Identifier: MPL-2.0

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

#include "CoalescedWorker.h"
#include "StatsReadOrder.h"

namespace {

int gFailures = 0;
int gCases = 0;
std::string gCurrentCase;

// Stops at the first failing case: a broken worker can leave a held job on the
// spec's own thread, and the next case would hang behind it.
void Case(const std::string& name) {
  if (0 < gFailures) {
    std::cout << gCases << " cases, " << gFailures << " failures (stopped)\n";
    std::exit(1);
  }
  ++gCases;
  gCurrentCase = name;
}

void Check(bool condition, const std::string& message) {
  if (condition) return;
  ++gFailures;
  std::cout << "  FAIL [" << gCurrentCase << "] " << message << "\n";
}

// The worker's job as the spec drives it: counts its runs, records the thread
// of the last one, and while held blocks each run until released. Safe for
// concurrent use.
class Job {
 public:
  void Run() {
    std::unique_lock lock(stateLock_);
    ++runs_;
    thread_ = std::this_thread::get_id();
    changed_.notify_all();
    changed_.wait(lock, [this] { return !held_; });
  }

  void Hold() {
    std::scoped_lock lock(stateLock_);
    held_ = true;
  }

  void Release() {
    {
      std::scoped_lock lock(stateLock_);
      held_ = false;
    }
    changed_.notify_all();
  }

  // Whether the job has run `count` times; the wait is a hang guard.
  bool WaitForRuns(int count) {
    std::unique_lock lock(stateLock_);
    return changed_.wait_for(lock, std::chrono::seconds(10), [&] { return count <= runs_; });
  }

  int Runs() {
    std::scoped_lock lock(stateLock_);
    return runs_;
  }

  std::thread::id Thread() {
    std::scoped_lock lock(stateLock_);
    return thread_;
  }

 private:
  std::mutex stateLock_;
  std::condition_variable changed_;
  int runs_ = 0;
  bool held_ = false;
  std::thread::id thread_;
};

}  // namespace

int main() {
  // ---- the publish worker -----------------------------------------------------

  {
    Case("a request returns at once and the job runs on the worker's thread");
    Job job;
    urnw::CoalescedWorker worker([&] { job.Run(); });
    worker.Request();
    Check(job.WaitForRuns(1), "the job never ran");
    Check(job.Thread() != std::this_thread::get_id(),
          "the job ran on the caller's thread: a stats read would block the UI thread");
    worker.Stop();
    Check(job.Runs() == 1, "one request ran the job more than once");
  }
  {
    Case("requests while a run is in flight fold into one more run after it");
    Job job;
    urnw::CoalescedWorker worker([&] { job.Run(); });
    job.Hold();
    worker.Request();
    Check(job.WaitForRuns(1), "the job never ran");
    // a burst while the first read waits on a getter
    worker.Request();
    worker.Request();
    worker.Request();
    Check(job.Runs() == 1, "a second run started beside the one in flight");
    job.Release();
    Check(job.WaitForRuns(2), "a request during a run was lost: nothing read after it");
    worker.Stop();
    Check(job.Runs() == 2, "a burst during one run cost more than one more run");
  }
  {
    Case("a request after the worker went idle runs the job again");
    Job job;
    urnw::CoalescedWorker worker([&] { job.Run(); });
    worker.Request();
    Check(job.WaitForRuns(1), "the job never ran");
    worker.Request();
    Check(job.WaitForRuns(2), "a later request did not run the job again");
    worker.Stop();
    Check(job.Runs() == 2, "two requests ran the job more than twice");
  }
  {
    Case("nothing runs after Stop");
    Job job;
    urnw::CoalescedWorker worker([&] { job.Run(); });
    worker.Request();
    Check(job.WaitForRuns(1), "the job never ran");
    worker.Stop();
    worker.Request();
    // a second Stop joins whatever the request started
    worker.Stop();
    Check(job.Runs() == 1, "a request after Stop ran the job against a stopped owner");
  }
  {
    Case("a job that throws leaves the worker running");
    Job job;
    urnw::CoalescedWorker worker([&] {
      job.Run();
      if (job.Runs() == 1) throw std::runtime_error("a getter failed");
    });
    worker.Request();
    Check(job.WaitForRuns(1), "the job never ran");
    worker.Request();
    Check(job.WaitForRuns(2), "the run after a throw never came");
    worker.Stop();
  }

  // ---- the order snapshots are applied in ------------------------------------

  {
    Case("the first snapshot is applied, and so is each one read after it");
    urnw::StatsReadOrder order;
    Check(order.Admit(1), "the first snapshot was dropped");
    Check(order.Admit(2), "the next read was dropped");
    Check(order.Admit(5), "a read after skipped sequences was dropped");
  }
  {
    Case("a read that began before the applied one is dropped when it finishes late");
    urnw::StatsReadOrder order;
    // 2 began first and waited on a getter; 3 began after it and finished first
    Check(order.Admit(3), "the newer read was dropped");
    Check(!order.Admit(2), "the older read rolled the snapshot back");
    Check(order.Admit(4), "a read after the applied one was dropped");
    Check(!order.Admit(3), "a read older than the applied one was applied again");
  }
  {
    Case("the same snapshot twice is applied once");
    urnw::StatsReadOrder order;
    Check(order.Admit(7), "the snapshot was dropped");
    Check(!order.Admit(7), "the same snapshot was applied twice");
  }

  std::cout << gCases << " cases, " << gFailures << " failures\n";
  return gFailures == 0 ? 0 : 1;
}
