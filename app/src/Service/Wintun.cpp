// SPDX-License-Identifier: MPL-2.0
#include "Wintun.h"

#include "Log.h"

namespace urnw {
namespace {
constexpr wchar_t kTunnelType[] = L"URnetwork";
}  // namespace

std::unique_ptr<Wintun> Wintun::Load(const std::filesystem::path& dllPath) {
  HMODULE mod = ::LoadLibraryExW(dllPath.c_str(), nullptr,
                                 LOAD_LIBRARY_SEARCH_APPLICATION_DIR |
                                     LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!mod) {
    LogError("wintun: LoadLibrary({}) failed: {}", dllPath.string(),
             ::GetLastError());
    return nullptr;
  }
  auto api = std::unique_ptr<Wintun>(new Wintun());
  api->module_ = mod;

  auto resolve = [&](auto& fn, const char* name) -> bool {
    fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(
        ::GetProcAddress(mod, name));
    if (!fn) LogError("wintun: missing export {}", name);
    return fn != nullptr;
  };

  bool ok = true;
  ok &= resolve(api->CreateAdapter, "WintunCreateAdapter");
  ok &= resolve(api->CloseAdapter, "WintunCloseAdapter");
  ok &= resolve(api->OpenAdapter, "WintunOpenAdapter");
  ok &= resolve(api->GetAdapterLuid, "WintunGetAdapterLUID");
  ok &= resolve(api->StartSession, "WintunStartSession");
  ok &= resolve(api->EndSession, "WintunEndSession");
  ok &= resolve(api->GetReadWaitEvent, "WintunGetReadWaitEvent");
  ok &= resolve(api->ReceivePacket, "WintunReceivePacket");
  ok &= resolve(api->ReleaseReceivePacket, "WintunReleaseReceivePacket");
  ok &= resolve(api->AllocateSendPacket, "WintunAllocateSendPacket");
  ok &= resolve(api->SendPacket, "WintunSendPacket");
  ok &= resolve(api->DeleteDriver, "WintunDeleteDriver");
  ok &= resolve(api->SetLogger, "WintunSetLogger");
  if (!ok) return nullptr;
  return api;
}

Wintun::~Wintun() {
  if (module_) ::FreeLibrary(module_);
}

std::unique_ptr<WintunAdapter> WintunAdapter::Create(Wintun& api,
                                                     const wchar_t* name,
                                                     const GUID& requestedGuid,
                                                     DWORD ringCapacity) {
  auto self = std::unique_ptr<WintunAdapter>(new WintunAdapter(api));
  // installs the embedded driver on first use (requires SYSTEM/admin)
  self->adapter_ = api.CreateAdapter(name, kTunnelType, &requestedGuid);
  if (!self->adapter_) {
    LogError("wintun: CreateAdapter failed: {}", ::GetLastError());
    return nullptr;
  }
  api.GetAdapterLuid(self->adapter_, &self->luid_);
  self->session_ = api.StartSession(self->adapter_, ringCapacity);
  if (!self->session_) {
    LogError("wintun: StartSession failed: {}", ::GetLastError());
    api.CloseAdapter(self->adapter_);
    self->adapter_ = nullptr;
    return nullptr;
  }
  return self;
}

WintunAdapter::~WintunAdapter() {
  if (session_) api_.EndSession(session_);
  if (adapter_) api_.CloseAdapter(adapter_);
}

std::span<const uint8_t> WintunAdapter::Receive() {
  DWORD size = 0;
  BYTE* packet = api_.ReceivePacket(session_, &size);
  if (!packet) return {};  // ERROR_NO_MORE_ITEMS -> wait on ReadWaitEvent
  return std::span<const uint8_t>(packet, size);
}

void WintunAdapter::ReleaseReceived(std::span<const uint8_t> packet) {
  if (!packet.empty())
    api_.ReleaseReceivePacket(session_, const_cast<uint8_t*>(packet.data()));
}

bool WintunAdapter::Send(std::span<const uint8_t> packet) {
  if (packet.empty()) return true;
  BYTE* slot = api_.AllocateSendPacket(session_, static_cast<DWORD>(packet.size()));
  if (!slot) return false;  // ring full; drop (upper layers retransmit)
  std::memcpy(slot, packet.data(), packet.size());
  api_.SendPacket(session_, slot);
  return true;
}

}  // namespace urnw
