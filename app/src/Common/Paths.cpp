// SPDX-License-Identifier: MPL-2.0
#include "Paths.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>

#include <system_error>

#pragma comment(lib, "shell32.lib")

namespace urnw {
namespace {

std::filesystem::path KnownFolder(REFKNOWNFOLDERID id) {
  PWSTR raw = nullptr;
  std::filesystem::path result;
  if (SUCCEEDED(::SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &raw))) {
    result = raw;
  }
  if (raw) ::CoTaskMemFree(raw);
  return result;
}

std::filesystem::path EnsureDir(std::filesystem::path p) {
  std::error_code ec;
  std::filesystem::create_directories(p, ec);
  return p;
}

}  // namespace

std::filesystem::path StorageRoot(bool isService) {
  // FOLDERID_ProgramData -> C:\ProgramData (machine-wide, service)
  // FOLDERID_LocalAppData -> C:\Users\<u>\AppData\Local (per user, app)
  std::filesystem::path base =
      isService ? KnownFolder(FOLDERID_ProgramData)
                : KnownFolder(FOLDERID_LocalAppData);
  return EnsureDir(base / L"URnetwork" / (isService ? L"service" : L"app"));
}

std::filesystem::path SdkStorageDir(bool isService) {
  return EnsureDir(StorageRoot(isService) / L"storage");
}

std::filesystem::path LogDir(bool isService) {
  return EnsureDir(StorageRoot(isService) / L"logs");
}

std::filesystem::path RpcSessionFile() {
  return StorageRoot(/*isService=*/false) / L"rpc_session.json";
}

}  // namespace urnw
