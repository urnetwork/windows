// The order the app applies live stats snapshots in (SdkHost::ReadStats).
//
// Snapshots are read on several threads at once: the SDK's listener threads,
// the session worker at the end of a pass, and SdkHost's stats publish worker.
// They reach the UI thread in the order they finished, not the order they were
// read, and a read can take as long as its slowest DeviceRemote getter (up to
// the rpc timeout behind a held command). Each snapshot carries the sequence
// its read began with (LiveStats::readSequence), and only a snapshot newer than
// every one applied so far is applied, so a slow read that finishes after a
// newer one cannot roll the window or the tray back.
//
// Header-only and WinRT-free, so tools/stats-publish-tests.cpp runs it on any
// host with a C++20 compiler. Not safe for concurrent use: the UI thread owns
// the one instance (AppController).
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>

namespace urnw {

class StatsReadOrder {
 public:
  // True when the snapshot read at `readSequence` is newer than every one
  // applied so far, which it then is; false for an older or a repeated one.
  bool Admit(std::uint64_t readSequence) {
    if (readSequence <= applied_) return false;
    applied_ = readSequence;
    return true;
  }

 private:
  // the newest read applied, 0 before the first
  std::uint64_t applied_ = 0;
};

}  // namespace urnw
