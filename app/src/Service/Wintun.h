// Thin loader for wintun.dll (the upstream-signed Wintun DLL). We link it
// dynamically at runtime so the service starts even if the DLL is momentarily
// unavailable, and so the driver installs on first WintunCreateAdapter (which
// requires SYSTEM/admin — the service context). See third_party/README.md.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>

#include "wintun.h"

namespace urnw {

// Loads wintun.dll and resolves its exports. One per process.
class Wintun {
 public:
  static std::unique_ptr<Wintun> Load(const std::filesystem::path& dllPath);
  ~Wintun();

  Wintun(const Wintun&) = delete;
  Wintun& operator=(const Wintun&) = delete;

  WINTUN_CREATE_ADAPTER_FUNC* CreateAdapter = nullptr;
  WINTUN_CLOSE_ADAPTER_FUNC* CloseAdapter = nullptr;
  WINTUN_OPEN_ADAPTER_FUNC* OpenAdapter = nullptr;
  WINTUN_GET_ADAPTER_LUID_FUNC* GetAdapterLuid = nullptr;
  WINTUN_START_SESSION_FUNC* StartSession = nullptr;
  WINTUN_END_SESSION_FUNC* EndSession = nullptr;
  WINTUN_GET_READ_WAIT_EVENT_FUNC* GetReadWaitEvent = nullptr;
  WINTUN_RECEIVE_PACKET_FUNC* ReceivePacket = nullptr;
  WINTUN_RELEASE_RECEIVE_PACKET_FUNC* ReleaseReceivePacket = nullptr;
  WINTUN_ALLOCATE_SEND_PACKET_FUNC* AllocateSendPacket = nullptr;
  WINTUN_SEND_PACKET_FUNC* SendPacket = nullptr;
  WINTUN_DELETE_DRIVER_FUNC* DeleteDriver = nullptr;
  WINTUN_SET_LOGGER_FUNC* SetLogger = nullptr;

 private:
  Wintun() = default;
  HMODULE module_ = nullptr;
};

// RAII adapter + session pair. The adapter is created (installing the driver on
// first use) and a ring session started; packets are read/written through it.
class WintunAdapter {
 public:
  // ringCapacity must be a power of two between WINTUN_MIN_RING_CAPACITY and
  // WINTUN_MAX_RING_CAPACITY.
  static std::unique_ptr<WintunAdapter> Create(Wintun& api, const wchar_t* name,
                                               const GUID& requestedGuid,
                                               DWORD ringCapacity);
  ~WintunAdapter();

  WintunAdapter(const WintunAdapter&) = delete;
  WintunAdapter& operator=(const WintunAdapter&) = delete;

  NET_LUID Luid() const { return luid_; }

  // Event signaled when packets are available to receive.
  HANDLE ReadWaitEvent() const { return api_.GetReadWaitEvent(session_); }

  // Receive one outbound packet (host -> tunnel). Returns empty span when the
  // ring is momentarily empty (caller waits on ReadWaitEvent). The returned
  // memory is owned by the ring until ReleaseReceived is called.
  std::span<const uint8_t> Receive();
  void ReleaseReceived(std::span<const uint8_t> packet);

  // Send one inbound packet (tunnel -> host). Copies into a ring slot.
  bool Send(std::span<const uint8_t> packet);

 private:
  WintunAdapter(Wintun& api) : api_(api) {}
  Wintun& api_;
  WINTUN_ADAPTER_HANDLE adapter_ = nullptr;
  WINTUN_SESSION_HANDLE session_ = nullptr;
  NET_LUID luid_{};
};

}  // namespace urnw
