// A thread of its own that runs one fixed job on request, off the caller's
// thread: SdkHost's stats publish, whose DeviceRemote getters can wait up to
// the rpc timeout behind a held command and so must never run on the UI
// thread.
//
// Requests coalesce. A request while a run is in flight is folded into one more
// run after it, however many land, so a burst costs at most two runs and the
// last one starts after the last request. One run at a time, never two. The
// thread exits when nothing is asked and starts again for the next request;
// a finished thread is joined before the next starts. After Stop nothing runs
// again, and Stop joins a run in flight, so the owner stops it before it tears
// down anything the job reads.
//
// The job must not throw; a throw is swallowed so the thread survives it, and
// the run counts as done. Request and Stop are safe for concurrent use; Stop is
// called from one thread at a time.
//
// Header-only and WinRT-free, so tools/stats-publish-tests.cpp runs it on any
// host with a C++20 compiler.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <functional>
#include <mutex>
#include <thread>
#include <utility>

namespace urnw {

class CoalescedWorker {
 public:
  explicit CoalescedWorker(std::function<void()> job) : job_(std::move(job)) {}
  ~CoalescedWorker() { Stop(); }

  CoalescedWorker(const CoalescedWorker&) = delete;
  CoalescedWorker& operator=(const CoalescedWorker&) = delete;

  // Asks for a run and returns at once: folded into the run in flight's next
  // one, or on a new thread.
  void Request() {
    std::scoped_lock lock(stateLock_);
    if (stopped_) return;
    dirty_ = true;
    if (running_) return;
    running_ = true;
    // A thread that has returned still leaves a joinable object; it is past
    // every line that takes stateLock_, so joining under it cannot wait on us.
    if (thread_.joinable()) thread_.join();
    thread_ = std::thread([this] { Loop(); });
  }

  // No run starts after this; a run in flight is waited out.
  void Stop() {
    {
      std::scoped_lock lock(stateLock_);
      stopped_ = true;
      dirty_ = false;
    }
    // stopped_ keeps Request from replacing the thread from here on
    if (thread_.joinable()) thread_.join();
  }

 private:
  void Loop() {
    for (;;) {
      {
        std::scoped_lock lock(stateLock_);
        if (stopped_ || !dirty_) {
          running_ = false;
          return;
        }
        dirty_ = false;
      }
      try {
        job_();
      } catch (...) {
      }
    }
  }

  const std::function<void()> job_;
  std::mutex stateLock_;
  std::thread thread_;
  // a thread is in Loop
  bool running_ = false;
  // a request came since the run in flight began
  bool dirty_ = false;
  bool stopped_ = false;
};

}  // namespace urnw
