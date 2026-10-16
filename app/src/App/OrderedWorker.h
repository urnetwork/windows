// A thread of its own that runs queued jobs one at a time, in the order they
// were posted, off the caller's thread: SdkHost's user-edit writes. Each takes
// mutex_, which the session worker holds across whole bootstraps and connect
// passes, and a DeviceRemote setter, which holds the device's state lock
// through its rpc for up to the rpc timeout, so none may run on the UI thread.
//
// Unlike CoalescedWorker, nothing coalesces: every job posted runs, after the
// ones posted before it, so a setting flipped twice lands on its last value
// and two edits never reorder. The thread exits when the queue is empty and
// starts again for the next post; a finished thread is joined before the next
// starts. Stop refuses new posts, lets the jobs already queued run, in order,
// and joins the thread, so the owner stops it before it tears down anything a
// job reads; a job still queued at a quit is the user's edit and still runs.
//
// A job must not throw; a throw is swallowed so the thread survives it and the
// jobs behind it run. Post and Stop are safe for concurrent use; Stop is called
// from one thread at a time.
//
// Header-only and WinRT-free, so tools/ordered-worker-tests.cpp runs it on any
// host with a C++20 compiler.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>

namespace urnw {

class OrderedWorker {
 public:
  OrderedWorker() = default;
  ~OrderedWorker() { Stop(); }

  OrderedWorker(const OrderedWorker&) = delete;
  OrderedWorker& operator=(const OrderedWorker&) = delete;

  // Queues `job` after every job posted so far and returns at once. False once
  // stopped: the job never runs.
  bool Post(std::function<void()> job) {
    std::scoped_lock lock(stateLock_);
    if (stopped_) return false;
    jobs_.push_back(std::move(job));
    if (running_) return true;
    running_ = true;
    // A thread that has returned still leaves a joinable object; it is past
    // every line that takes stateLock_, so joining under it cannot wait on us.
    if (thread_.joinable()) thread_.join();
    thread_ = std::thread([this] { Loop(); });
    return true;
  }

  // No post is taken after this; the jobs already queued run, then the thread
  // is joined.
  void Stop() {
    {
      std::scoped_lock lock(stateLock_);
      stopped_ = true;
    }
    // stopped_ keeps Post from replacing the thread from here on
    if (thread_.joinable()) thread_.join();
  }

 private:
  void Loop() {
    for (;;) {
      std::function<void()> job;
      {
        std::scoped_lock lock(stateLock_);
        if (jobs_.empty()) {
          running_ = false;
          return;
        }
        job = std::move(jobs_.front());
        jobs_.pop_front();
      }
      try {
        if (job) job();
      } catch (...) {
      }
    }
  }

  std::mutex stateLock_;
  std::deque<std::function<void()>> jobs_;
  std::thread thread_;
  // a thread is in Loop
  bool running_ = false;
  bool stopped_ = false;
};

}  // namespace urnw
