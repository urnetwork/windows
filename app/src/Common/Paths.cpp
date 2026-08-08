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
  // URNETWORK_APP_ROOT overrides the per-user app root. This exists because
  // several agents build and run this repo CONCURRENTLY from separate git
  // worktrees, and every one of them otherwise shares a single
  // %LOCALAPPDATA%\URnetwork\app: one SDK LocalState (the JWT and instance id),
  // one rpc_session.json and one log file, with two unsynchronised writers.
  // That is a state-corruption risk, not just noisy logs — and it silently
  // makes one agent's run appear in another agent's evidence.
  //
  // Point each worktree at its own root:
  //   $env:URNETWORK_APP_ROOT = 'C:\...\wt-p1\.localstate'
  //
  // Deliberately app-only. The service root is machine-wide by nature (it is
  // LocalSystem state and the control pipe is a single machine-wide instance),
  // so splitting it would give a false sense of isolation the service does not
  // actually have.
  if (!isService) {
    wchar_t buf[MAX_PATH];
    const DWORD n =
        ::GetEnvironmentVariableW(L"URNETWORK_APP_ROOT", buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) return EnsureDir(std::filesystem::path(buf, buf + n));
  }
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

std::filesystem::path AppPrefsFile() {
  return StorageRoot(/*isService=*/false) / L"app_prefs.json";
}

}  // namespace urnw
