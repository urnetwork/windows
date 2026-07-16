// SPDX-License-Identifier: MPL-2.0
#include "SplitTunnelClient.h"

#include <winioctl.h>
#include <fwpmu.h>
#pragma comment(lib, "fwpuclnt.lib")

#include <filesystem>

#include "Ids.h"
#include "Log.h"
#include "Strings.h"
#include "../../driver/Ioctl.h"

namespace urnw {

namespace {

// Full path to SplitTunnel.sys, laid down next to urnetworkd.exe by the MSI.
std::filesystem::path DriverImagePath() {
  wchar_t buf[MAX_PATH];
  DWORD n = ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return {};
  return std::filesystem::path(std::wstring(buf, n)).parent_path() /
         ids::kSplitTunnelSysFileName;
}

// Convert a DOS image path (C:\...) to the WFP "app id" form - the NT device path
// (\device\harddiskvolumeN\..., lowercase) the kernel reports as the ALE_APP_ID
// fixed value and in the process-create image name. Sending this form is what lets
// the driver match the RUNNING process image, not just newly-created ones, and
// fixes the DOS-vs-NT path mismatch. Best-effort: returns the input unchanged on
// failure (the driver simply won't match that entry).
std::wstring NormalizeToAppId(const std::wstring& dosPath) {
  FWP_BYTE_BLOB* blob = nullptr;
  std::wstring appId;
  if (::FwpmGetAppIdFromFileName0(dosPath.c_str(), &blob) == ERROR_SUCCESS && blob) {
    if (blob->data && blob->size >= sizeof(wchar_t)) {
      appId.assign(reinterpret_cast<const wchar_t*>(blob->data),
                   blob->size / sizeof(wchar_t) - 1);  // drop the null terminator
    }
    ::FwpmFreeMemory0(reinterpret_cast<void**>(&blob));
  }
  return appId.empty() ? dosPath : appId;
}

// Register (if needed) and start the split-tunnel kernel service so its device
// can be opened. The MSI ships only the .sys; the LocalSystem service owns the
// driver lifecycle (Windows Installer can't host a kernel-driver service). The
// caller running as LocalSystem is why CreateService/StartService are permitted.
// Returns false if the driver isn't shipped or won't load — split tunneling is
// then simply disabled and the tunnel still works.
bool StartDriverService() {
  std::error_code ec;
  std::filesystem::path sys = DriverImagePath();
  if (sys.empty() || !std::filesystem::exists(sys, ec)) {
    LogInfo("split-tunnel: SplitTunnel.sys not installed, split tunneling disabled");
    return false;
  }

  SC_HANDLE scm = ::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
  if (!scm) {
    LogWarn("split-tunnel: OpenSCManager failed: {}", ::GetLastError());
    return false;
  }

  SC_HANDLE svc = ::OpenServiceW(scm, ids::kSplitTunnelServiceName,
                                 SERVICE_START | SERVICE_QUERY_STATUS);
  if (!svc) {
    svc = ::CreateServiceW(scm, ids::kSplitTunnelServiceName,
                           ids::kSplitTunnelServiceName,
                           SERVICE_START | SERVICE_QUERY_STATUS,
                           SERVICE_KERNEL_DRIVER, SERVICE_DEMAND_START,
                           SERVICE_ERROR_NORMAL, sys.c_str(), nullptr, nullptr,
                           nullptr, nullptr, nullptr);
    if (!svc) {
      LogWarn("split-tunnel: CreateService failed: {}", ::GetLastError());
      ::CloseServiceHandle(scm);
      return false;
    }
  }

  bool started = ::StartServiceW(svc, 0, nullptr) != 0;
  if (!started) {
    DWORD e = ::GetLastError();
    started = (e == ERROR_SERVICE_ALREADY_RUNNING);
    if (!started) LogWarn("split-tunnel: StartService failed: {}", e);
  }
  ::CloseServiceHandle(svc);
  ::CloseServiceHandle(scm);
  return started;
}

// Stop (unload) and delete the split-tunnel kernel service. Best-effort and
// idempotent — a no-op if it was never created — so the driver stays ephemeral:
// nothing persists across sessions or after uninstall.
void StopAndRemoveDriverService() {
  SC_HANDLE scm = ::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
  if (!scm) return;
  SC_HANDLE svc = ::OpenServiceW(scm, ids::kSplitTunnelServiceName, SERVICE_STOP | DELETE);
  if (svc) {
    SERVICE_STATUS status{};
    ::ControlService(svc, SERVICE_CONTROL_STOP, &status);  // handle already closed -> can unload
    ::DeleteService(svc);
    ::CloseServiceHandle(svc);
  }
  ::CloseServiceHandle(scm);
}

}  // namespace

SplitTunnelClient::~SplitTunnelClient() { Close(); }

bool SplitTunnelClient::Open() {
  // Load the driver on demand: register + start the kernel service, then open its
  // device. If the driver isn't shipped or won't load, split tunneling is
  // disabled and the tunnel still works.
  if (!StartDriverService()) {
    device_ = nullptr;
    return false;
  }
  HANDLE h = ::CreateFileW(ids::kSplitTunnelDevicePath, GENERIC_READ | GENERIC_WRITE,
                           0, nullptr, OPEN_EXISTING, 0, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    LogInfo("split-tunnel: device not available after load ({}), disabled",
            ::GetLastError());
    StopAndRemoveDriverService();
    device_ = nullptr;
    return false;
  }
  device_ = h;
  LogInfo("split-tunnel: driver loaded");
  return true;
}

void SplitTunnelClient::Close() {
  if (IsAvailable()) {
    Clear();
    ::CloseHandle(static_cast<HANDLE>(device_));
  }
  device_ = nullptr;
  // Unload + deregister the driver (ephemeral; see StartDriverService).
  StopAndRemoveDriverService();
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

bool SplitTunnelClient::SetMode(bool allowlist) {
  URST_MODE payload{allowlist ? 1u : 0u};
  return Ioctl(IOCTL_URST_SET_MODE, &payload, sizeof(payload));
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
    std::wstring w = NormalizeToAppId(Widen(utf8));
    uint16_t lenChars = static_cast<uint16_t>(w.size());
    append(&lenChars, sizeof(lenChars));
    append(w.data(), static_cast<size_t>(lenChars) * sizeof(wchar_t));
  }
  return Ioctl(IOCTL_URST_SET_EXCLUDED_PATHS, buffer.data(),
               static_cast<uint32_t>(buffer.size()));
}

bool SplitTunnelClient::Clear() { return Ioctl(IOCTL_URST_CLEAR, nullptr, 0); }

}  // namespace urnw
