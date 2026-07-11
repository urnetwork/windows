// SPDX-License-Identifier: MPL-2.0
#include "SplitTunnelClient.h"

#include <winioctl.h>

#include "Ids.h"
#include "Log.h"
#include "Strings.h"
#include "../../driver/Ioctl.h"

namespace urnw {

SplitTunnelClient::~SplitTunnelClient() { Close(); }

bool SplitTunnelClient::Open() {
  HANDLE h = ::CreateFileW(ids::kSplitTunnelDevicePath, GENERIC_READ | GENERIC_WRITE,
                           0, nullptr, OPEN_EXISTING, 0, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    LogInfo("split-tunnel: driver not available ({}), split tunneling disabled",
            ::GetLastError());
    device_ = nullptr;
    return false;
  }
  device_ = h;
  LogInfo("split-tunnel: driver opened");
  return true;
}

void SplitTunnelClient::Close() {
  if (IsAvailable()) {
    Clear();
    ::CloseHandle(static_cast<HANDLE>(device_));
  }
  device_ = nullptr;
}

bool SplitTunnelClient::Ioctl(uint32_t code, void* input, uint32_t inputSize) {
  if (!IsAvailable()) return false;
  DWORD returned = 0;
  BOOL ok = ::DeviceIoControl(static_cast<HANDLE>(device_), code, input,
                              inputSize, nullptr, 0, &returned, nullptr);
  if (!ok) LogWarn("split-tunnel: ioctl {:#x} failed: {}", code, ::GetLastError());
  return ok != 0;
}

bool SplitTunnelClient::SetEnabled(bool enabled) {
  URST_ENABLED payload{enabled ? 1u : 0u};
  return Ioctl(IOCTL_URST_SET_ENABLED, &payload, sizeof(payload));
}

bool SplitTunnelClient::SetPhysicalAddresses(uint32_t ifIndex4,
                                             const uint8_t addr4[4],
                                             uint32_t ifIndex6,
                                             const uint8_t addr6[16]) {
  URST_PHYSICAL_ADDRS payload{};
  payload.InterfaceIndex4 = ifIndex4;
  payload.InterfaceIndex6 = ifIndex6;
  if (addr4) std::memcpy(payload.Address4, addr4, sizeof(payload.Address4));
  if (addr6) std::memcpy(payload.Address6, addr6, sizeof(payload.Address6));
  return Ioctl(IOCTL_URST_SET_PHYSICAL_ADDRS, &payload, sizeof(payload));
}

bool SplitTunnelClient::SetExcludedPaths(const std::vector<std::string>& utf8Paths) {
  if (!IsAvailable()) return false;
  // Pack: UINT32 Count, then per entry { UINT16 LengthChars; WCHAR[] }.
  std::vector<uint8_t> buffer;
  auto append = [&](const void* p, size_t n) {
    const auto* b = static_cast<const uint8_t*>(p);
    buffer.insert(buffer.end(), b, b + n);
  };
  uint32_t count = static_cast<uint32_t>(utf8Paths.size());
  append(&count, sizeof(count));
  for (const auto& utf8 : utf8Paths) {
    std::wstring w = Widen(utf8);
    uint16_t lenChars = static_cast<uint16_t>(w.size());
    append(&lenChars, sizeof(lenChars));
    append(w.data(), static_cast<size_t>(lenChars) * sizeof(wchar_t));
  }
  return Ioctl(IOCTL_URST_SET_EXCLUDED_PATHS, buffer.data(),
               static_cast<uint32_t>(buffer.size()));
}

bool SplitTunnelClient::Clear() { return Ioctl(IOCTL_URST_CLEAR, nullptr, 0); }

}  // namespace urnw
