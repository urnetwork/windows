// Pure retry schedule for app-to-service recovery.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <array>
#include <chrono>
#include <cstddef>

namespace urnw::recovery {

inline constexpr std::array<std::chrono::seconds, 9> kServiceRetryDelays{
    std::chrono::seconds{1},   std::chrono::seconds{2},
    std::chrono::seconds{4},   std::chrono::seconds{8},
    std::chrono::seconds{15},  std::chrono::seconds{30},
    std::chrono::seconds{60},  std::chrono::seconds{120},
    std::chrono::seconds{300},
};

constexpr std::chrono::seconds ServiceRetryDelay(std::size_t attempt) {
  return kServiceRetryDelays[
      attempt < kServiceRetryDelays.size() ? attempt
                                           : kServiceRetryDelays.size() - 1];
}

}  // namespace urnw::recovery
