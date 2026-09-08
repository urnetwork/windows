// A silent tap-sequence gate: N taps, each within a window of the previous
// one, complete the sequence (the connect page's easter egg: five taps on the
// connected dot play the Pro celebration). A gap longer than the window, or a
// completed sequence, starts the count over. Pure logic, no clock of its own:
// the caller passes the time of each tap (the same helper the Linux app
// carries, with its unit tests, in app/src/TapSequenceGate.hpp).
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>

namespace urnw {

class TapSequenceGate {
 public:
  explicit TapSequenceGate(int count = 5, int64_t windowMs = 5000)
      : count_(count), windowMs_(windowMs) {}

  // Records a tap at `nowMs` (any monotonic millisecond clock). Returns true
  // exactly when this tap completes the sequence; the count then restarts.
  bool Tap(int64_t nowMs) {
    if (taps_ > 0 && nowMs - lastMs_ > windowMs_) taps_ = 0;
    lastMs_ = nowMs;
    ++taps_;
    if (taps_ < count_) return false;
    taps_ = 0;
    return true;
  }

  void Reset() { taps_ = 0; }
  int Taps() const { return taps_; }

 private:
  int count_;
  int64_t windowMs_;
  int taps_ = 0;
  int64_t lastMs_ = 0;
};

}  // namespace urnw
