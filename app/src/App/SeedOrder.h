// A value the UI seeds from a read made off the UI thread, and that its
// listener also pushes: the DNS settings, the blocker and the transport
// policies the connect drawer shows (SdkHost::RequestDrawerSeed). The read can
// wait behind a held command for the rpc timeout, so a push may land between
// the request and the read's answer, and the push is the newer value: the
// read applies only when no push came after the request it answers. Requests
// coalesce into one read, which answers the last of them.
//
// Pure, no Windows dependencies; tools/seed-order-tests.cpp is the spec. Not
// safe for concurrent use: the connect page keeps one per value on the UI
// thread, where the pushes and the read's answer land.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>

namespace urnw {

class SeedOrder {
 public:
  // The listener pushed the value.
  void Pushed() { ++pushes_; }
  // A read is asked for.
  void Asked() { asked_ = pushes_; }
  // The read answered: whether it is still the newest value.
  bool Admits() const { return pushes_ == asked_; }

 private:
  std::uint64_t pushes_ = 0;
  std::uint64_t asked_ = 0;
};

}  // namespace urnw
