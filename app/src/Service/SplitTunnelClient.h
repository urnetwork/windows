// User-mode client for the split-tunnel driver. Opens the driver device and
// pushes the excluded-app set and physical-interface addresses via IOCTL. If
// the driver is not installed (split tunneling disabled or not yet shipped),
// all operations are graceful no-ops so the tunnel still works.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace urnw {

class SplitTunnelClient {
 public:
  SplitTunnelClient() = default;
  ~SplitTunnelClient();

  // Try to open the driver device. Returns false (and disables all ops) if the
  // driver is not present.
  bool Open();
  bool IsAvailable() const { return device_ != nullptr && device_ != INVALID_HANDLE_VALUE; }
  void Close();

  bool SetEnabled(bool enabled);
  // Physical interface addresses excluded flows rebind to (network byte order).
  bool SetPhysicalAddresses(uint32_t ifIndex4, const uint8_t addr4[4],
                            uint32_t ifIndex6, const uint8_t addr6[16]);
  // Replace the excluded image-path set (full paths, matched case-insensitively).
  bool SetExcludedPaths(const std::vector<std::string>& utf8Paths);
  bool Clear();

 private:
  bool Ioctl(uint32_t code, void* input, uint32_t inputSize);
  void* device_ = nullptr;  // HANDLE
};

}  // namespace urnw
