// The status decorations of the transport settings editor, computed as one
// pure step so every display rule is deterministic and testable:
// - decorations render only for Auto, only with a known status, and only
//   while the draft equals the applied policy the status was computed for
//   (a status must never be interpreted against an unrelated draft)
// - auto_degraded is authoritative: no degradation, no decorations
// - a transport is constrained when it is enabled under Auto and absent from
//   the status's eligible modes; the banner can render with no constrained
//   rows when the ineligible modes are vocabulary this app does not know
// - the memory constraint has its own copy; any other constraint uses the
//   generic system-constraint copy (an unknown future constraint must not be
//   presented as a memory limit)
//
// Header-only and sdk/WinRT-free.
#pragma once

#include <set>
#include <string>
#include <vector>

namespace urnw {

struct TransportStatusPresentation {
  bool showBanner = false;
  bool memoryConstraint = false;
  std::set<std::string> constrainedModes;

  friend bool operator==(const TransportStatusPresentation& a,
                         const TransportStatusPresentation& b) {
    return a.showBanner == b.showBanner && a.memoryConstraint == b.memoryConstraint &&
           a.constrainedModes == b.constrainedModes;
  }
};

// mirror of urnet::TransportConstraintMemory (keeps this header sdk-free)
inline constexpr const char* kTransportConstraintMemory = "memory";

inline TransportStatusPresentation TransportStatusDecorations(
    bool isAuto, bool draftMatchesStatusPolicy, const std::vector<std::string>& autoModes,
    bool statusKnown, bool autoDegraded, const std::vector<std::string>& autoEligibleModes,
    const std::string& autoConstraint) {
  if (!statusKnown || !autoDegraded || !isAuto || !draftMatchesStatusPolicy) return {};
  TransportStatusPresentation out;
  out.showBanner = true;
  out.memoryConstraint = autoConstraint == kTransportConstraintMemory;
  const std::set<std::string> eligible(autoEligibleModes.begin(), autoEligibleModes.end());
  for (const auto& mode : autoModes) {
    if (!eligible.count(mode)) out.constrainedModes.insert(mode);
  }
  return out;
}

}  // namespace urnw
