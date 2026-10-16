// Executable spec for SdkHost's ordered write worker (App/OrderedWorker.h,
// owner report 2026-10-16): every job posted runs, in the order posted, off
// the caller's thread; a post returns at once while a job is held; Stop lets
// the queued jobs run and refuses new ones; a throwing job does not stop the
// ones behind it. Run on any host with a C++20 compiler:
//
//   c++ -std=c++20 -I ../src/App ordered-worker-tests.cpp -o /tmp/ow && /tmp/ow
//
// Jobs are held at gates the test opens, so no result depends on timing. The
// one timeout is a 5 s guard that keeps a broken negative control (a job run
// on the posting thread, a job never run) from hanging the run.
//
// SPDX-License-Identifier: MPL-2.0

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "OrderedWorker.h"

using urnw::OrderedWorker;

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

constexpr auto kGuard = std::chrono::seconds(5);

// A gate a job waits at until the test opens it (or the guard runs out).
class Gate {
 public:
  void Open() {
    std::scoped_lock lock(mutex_);
    open_ = true;
    cv_.notify_all();
  }
  void Wait() {
    std::unique_lock lock(mutex_);
    cv_.wait_for(lock, kGuard, [this] { return open_; });
  }

 private:
  std::mutex mutex_;
  std::condition_variable cv_;
  bool open_ = false;
};

// What ran, in order, and on which thread.
class Record {
 public:
  void Ran(const std::string& name) {
    std::scoped_lock lock(mutex_);
    names_.push_back(name);
    threads_.push_back(std::this_thread::get_id());
    cv_.notify_all();
  }
  // Waits for `count` jobs (or the guard); the names that ran.
  std::vector<std::string> WaitFor(size_t count) {
    std::unique_lock lock(mutex_);
    cv_.wait_for(lock, kGuard, [&] { return names_.size() >= count; });
    return names_;
  }
  std::vector<std::thread::id> Threads() {
    std::scoped_lock lock(mutex_);
    return threads_;
  }

 private:
  std::mutex mutex_;
  std::condition_variable cv_;
  std::vector<std::string> names_;
  std::vector<std::thread::id> threads_;
};

std::string Join(const std::vector<std::string>& items) {
  std::string out;
  for (const auto& item : items) out += (out.empty() ? "" : ", ") + item;
  return "[" + out + "]";
}

void TestJobsRunInOrderOffTheCallersThread() {
  OrderedWorker worker;
  Record record;
  for (const char* name : {"a", "b", "c"}) {
    worker.Post([&record, name] { record.Ran(name); });
  }
  const auto ran = record.WaitFor(3);
  Check(ran == std::vector<std::string>{"a", "b", "c"}, "jobs run in the order posted: " + Join(ran));
  bool offCaller = !ran.empty();
  for (const auto& id : record.Threads()) offCaller = offCaller && id != std::this_thread::get_id();
  Check(offCaller, "jobs run off the caller's thread");
  worker.Stop();
}

void TestAPostReturnsWhileAJobIsHeld() {
  OrderedWorker worker;
  Record record;
  Gate gate;
  std::atomic<bool> heldFinished{false};
  worker.Post([&] {
    gate.Wait();
    heldFinished = true;
    record.Ran("held");
  });
  worker.Post([&] { record.Ran("next"); });
  Check(!heldFinished.load(), "a post returns at once while the job before it is held");
  gate.Open();
  const auto ran = record.WaitFor(2);
  Check(ran == std::vector<std::string>{"held", "next"},
        "the job posted behind a held one runs after it: " + Join(ran));
  worker.Stop();
}

void TestEveryJobRunsNothingCoalesces() {
  OrderedWorker worker;
  Record record;
  Gate gate;
  worker.Post([&] {
    gate.Wait();
    record.Ran("held");
  });
  for (const char* name : {"on", "off", "on again"}) {
    worker.Post([&record, name] { record.Ran(name); });
  }
  gate.Open();
  const auto ran = record.WaitFor(4);
  Check(ran == std::vector<std::string>{"held", "on", "off", "on again"},
        "every job posted runs, in order, none folded into another: " + Join(ran));
  worker.Stop();
}

void TestStopRunsTheQueuedJobsThenRefuses() {
  OrderedWorker worker;
  Record record;
  Gate gate;
  worker.Post([&] {
    gate.Wait();
    record.Ran("held");
  });
  worker.Post([&] { record.Ran("queued"); });
  std::thread stopper([&] { worker.Stop(); });
  // probes until the stop refuses one: everything accepted before it must run
  std::atomic<int> probesRan{0};
  int accepted = 0;
  for (int i = 0; i < 50000000; ++i) {
    if (!worker.Post([&probesRan] { ++probesRan; })) break;
    ++accepted;
  }
  gate.Open();
  stopper.join();
  const auto ran = record.WaitFor(2);
  Check(ran == std::vector<std::string>{"held", "queued"},
        "a job queued when the stop came still runs: " + Join(ran));
  Check(probesRan.load() == accepted, "every post the worker accepted before its stop ran");
  bool late = false;
  Check(!worker.Post([&late] { late = true; }), "a post after the stop is refused");
  Check(!late, "and never runs");
}

void TestAThrowingJobDoesNotStopTheJobsBehindIt() {
  OrderedWorker worker;
  Record record;
  worker.Post([] { throw std::runtime_error("a write threw"); });
  worker.Post([&] { record.Ran("after"); });
  const auto ran = record.WaitFor(1);
  Check(ran == std::vector<std::string>{"after"}, "a throwing job does not stop the jobs behind it");
  worker.Stop();
}

void TestAPostAfterIdleStartsAgain() {
  OrderedWorker worker;
  Record record;
  worker.Post([&] { record.Ran("first"); });
  record.WaitFor(1);
  worker.Post([&] { record.Ran("second"); });
  const auto ran = record.WaitFor(2);
  Check(ran == std::vector<std::string>{"first", "second"},
        "a post after the worker went idle runs too: " + Join(ran));
  worker.Stop();
}

}  // namespace

int main() {
  TestJobsRunInOrderOffTheCallersThread();
  TestAPostReturnsWhileAJobIsHeld();
  TestEveryJobRunsNothingCoalesces();
  TestStopRunsTheQueuedJobsThenRefuses();
  TestAThrowingJobDoesNotStopTheJobsBehindIt();
  TestAPostAfterIdleStartsAgain();
  std::cout << (gCases - gFailures) << "/" << gCases << " ordered worker checks passed\n";
  return gFailures == 0 ? 0 : 1;
}
