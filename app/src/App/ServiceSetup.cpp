// SPDX-License-Identifier: MPL-2.0
#include "pch.h"

#include "ServiceSetup.h"

// winsvc.h and shellapi.h sit inside windows.h's WIN32_LEAN_AND_MEAN guard, and
// winver.h is pulled in explicitly for the same reason: this is the one unit in
// the app that talks to the SCM, the shell-elevation API and the version
// resource reader, so the includes live here rather than widening pch.h.
#include <shellapi.h>
#include <winsvc.h>
#include <winver.h>

#include <vector>

#include "Ids.h"
#include "Log.h"
#include "Strings.h"

namespace urnw {

namespace {

// Is anything serving the control pipe right now? Same probe, same reasoning
// as ControlPipeInUse in Service/main.cpp and the reconnect watchdog in
// SdkHost: WaitNamedPipe with a ~zero timeout opens nothing and disturbs
// nothing. ERROR_FILE_NOT_FOUND is the only answer that means "no pipe" — a
// busy pipe (our own ServiceClient may hold the single instance) fails the
// wait with ERROR_SEM_TIMEOUT and is very much alive.
bool ControlPipeAlive() {
  if (::WaitNamedPipeW(ids::kControlPipeName, 1)) return true;
  return ::GetLastError() != ERROR_FILE_NOT_FOUND;
}

// RAII for the two SCM handles; the early returns in Classify are exactly the
// leaks this prevents. (A user-declared destructor makes this a non-aggregate,
// so the converting constructor is spelled out.)
struct ScHandle {
  SC_HANDLE h = nullptr;
  explicit ScHandle(SC_HANDLE handle) : h(handle) {}
  ScHandle(const ScHandle&) = delete;
  ScHandle& operator=(const ScHandle&) = delete;
  ~ScHandle() {
    if (h) ::CloseServiceHandle(h);
  }
  explicit operator bool() const { return h != nullptr; }
};

}  // namespace

std::filesystem::path ServiceSetup::SiblingServiceExe() {
  wchar_t buffer[MAX_PATH]{};
  const DWORD n = ::GetModuleFileNameW(nullptr, buffer, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return {};
  return std::filesystem::path(buffer).parent_path() / L"urnetworkd.exe";
}

std::wstring ServiceSetup::ExeFromBinPath(std::wstring const& binPath) {
  const size_t start = binPath.find_first_not_of(L' ');
  if (start == std::wstring::npos) return {};
  if (binPath[start] == L'"') {
    const size_t end = binPath.find(L'"', start + 1);
    if (end == std::wstring::npos) return {};  // torn config; claim nothing
    return binPath.substr(start + 1, end - start - 1);
  }
  // Unquoted: the first space-delimited token, which is the most an unquoted
  // binPath can unambiguously mean (and the reason the install verb always
  // quotes — InstallVerb.h). Arguments after the token are not the exe.
  const size_t space = binPath.find(L' ', start);
  return binPath.substr(start, space == std::wstring::npos ? std::wstring::npos
                                                           : space - start);
}

std::wstring ServiceSetup::FileProductVersion(std::wstring const& exePath) {
  if (exePath.empty()) return {};
  DWORD ignored = 0;
  const DWORD size = ::GetFileVersionInfoSizeW(exePath.c_str(), &ignored);
  if (size == 0) return {};
  std::vector<unsigned char> data(size);
  if (!::GetFileVersionInfoW(exePath.c_str(), 0, size, data.data())) return {};

  // Ask the resource which language it carries rather than hard-coding
  // 040904B0: the .rc files write en-US today, but a reader that assumes so
  // returns empty the day that changes, and empty here quietly disables the
  // mismatch banner forever.
  struct LangCodePage {
    WORD language;
    WORD codePage;
  };
  LangCodePage* translations = nullptr;
  UINT bytes = 0;
  if (!::VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation",
                        reinterpret_cast<void**>(&translations), &bytes) ||
      bytes < sizeof(LangCodePage)) {
    return {};
  }
  wchar_t query[64]{};
  ::swprintf_s(query, L"\\StringFileInfo\\%04x%04x\\ProductVersion",
               translations[0].language, translations[0].codePage);
  wchar_t* value = nullptr;
  UINT length = 0;
  if (!::VerQueryValueW(data.data(), query, reinterpret_cast<void**>(&value),
                        &length) ||
      value == nullptr || length == 0) {
    return {};
  }
  // length counts the terminator when present; never include it in the string.
  std::wstring result(value, length);
  while (!result.empty() && result.back() == L'\0') result.pop_back();
  return result;
}

ServiceSetup::Observation ServiceSetup::Classify() {
  Observation obs;

  // Without a sibling urnetworkd.exe the banner's one action cannot exist, so
  // there is nothing honest to offer whatever the SCM says. Seen when the app
  // exe is copied somewhere on its own; the portable zip and the build output
  // always carry both.
  const std::filesystem::path sibling = SiblingServiceExe();
  std::error_code ec;
  if (sibling.empty() || !std::filesystem::exists(sibling, ec)) {
    LogWarn("servicesetup: no urnetworkd.exe beside the app ({}) — showing "
            "nothing",
            Narrow(sibling.native()));
    return obs;  // Unknown
  }

  ScHandle scm{::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT)};
  if (!scm) {
    LogWarn("servicesetup: OpenSCManager failed: {}", ::GetLastError());
    return obs;  // Unknown — cannot even ask
  }

  // Status + config wanted; config gracefully dropped on access-denied. The
  // default service DACL grants both to authenticated users, but a hardened
  // box may not, and status-only still supports every banner except the
  // mismatch one — which is exactly the banner that must not fire without
  // reading the installed exe's version.
  bool configReadable = true;
  ScHandle svc{::OpenServiceW(scm.h, ids::kServiceName,
                              SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG)};
  if (!svc && ::GetLastError() == ERROR_ACCESS_DENIED) {
    configReadable = false;
    svc.h = ::OpenServiceW(scm.h, ids::kServiceName, SERVICE_QUERY_STATUS);
  }
  if (!svc) {
    const DWORD err = ::GetLastError();
    if (err == ERROR_SERVICE_DOES_NOT_EXIST) {
      obs.state = ControlPipeAlive() ? State::ConsoleMode : State::NotInstalled;
    } else {
      LogWarn("servicesetup: OpenService failed: {}", err);
    }
    return obs;
  }

  SERVICE_STATUS_PROCESS status{};
  DWORD needed = 0;
  if (!::QueryServiceStatusEx(svc.h, SC_STATUS_PROCESS_INFO,
                              reinterpret_cast<BYTE*>(&status), sizeof(status),
                              &needed)) {
    LogWarn("servicesetup: QueryServiceStatusEx failed: {}", ::GetLastError());
    return obs;  // Unknown — a service exists but its state does not
  }
  // START_PENDING counts as running: the healthy first seconds after an
  // install must not flash a "Start" banner at the user who just clicked it.
  const bool running = status.dwCurrentState == SERVICE_RUNNING ||
                       status.dwCurrentState == SERVICE_START_PENDING;

  // A live pipe that the RUNNING service does not explain is a developer's
  // `urnetworkd console` (the pipe is single-instance; the service refuses to
  // start against it). The spec row covers the unregistered case; a registered
  // -but-stopped service under a live console is the same situation with stale
  // registration on top, and a "Start" banner there would click straight into
  // the install verb's pipe-busy refusal. Show nothing; the developer is
  // driving.
  if (!running && ControlPipeAlive()) {
    obs.state = State::ConsoleMode;
    return obs;
  }

  // The mismatch check needs BOTH ProductVersion strings. Either side missing
  // (denied config, stripped resource, deleted binPath exe) means no mismatch
  // claim — degrade to the plain status reading.
  if (configReadable) {
    DWORD configBytes = 0;
    ::QueryServiceConfigW(svc.h, nullptr, 0, &configBytes);
    if (::GetLastError() == ERROR_INSUFFICIENT_BUFFER && configBytes > 0) {
      std::vector<unsigned char> raw(configBytes);
      auto* config = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(raw.data());
      if (::QueryServiceConfigW(svc.h, config, configBytes, &configBytes) &&
          config->lpBinaryPathName) {
        obs.installedVersion =
            FileProductVersion(ExeFromBinPath(config->lpBinaryPathName));
        obs.siblingVersion = FileProductVersion(sibling.native());
        if (!obs.installedVersion.empty() && !obs.siblingVersion.empty() &&
            obs.installedVersion != obs.siblingVersion) {
          obs.state = State::VersionMismatch;
          return obs;
        }
      }
    }
  }

  obs.state = running ? State::Running : State::Stopped;
  return obs;
}

ServiceSetup::Observation ServiceSetup::AwaitState(State goal,
                                                   unsigned long budgetMs) {
  const ULONGLONG deadline = ::GetTickCount64() + budgetMs;
  Observation obs = Classify();
  while (obs.state != goal && ::GetTickCount64() < deadline) {
    ::Sleep(500);
    obs = Classify();
  }
  return obs;
}

ServiceSetup::ElevatedResult ServiceSetup::RunElevatedVerb(
    const wchar_t* verb, unsigned long waitMs) {
  ElevatedResult result;
  const std::wstring exe = SiblingServiceExe().native();

  SHELLEXECUTEINFOW info{};
  info.cbSize = sizeof(info);
  // NOCLOSEPROCESS for the wait below; NOASYNC because this runs on a worker
  // thread and the shell must finish the launch before we return, not on some
  // message loop this thread does not pump.
  info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
  info.lpVerb = L"runas";
  info.lpFile = exe.c_str();
  info.lpParameters = verb;
  // The verb has no console and prints to nobody; its exit code is the entire
  // interface (InstallVerb.h). A flashing empty console window would only make
  // the elevation look broken.
  info.nShow = SW_HIDE;

  if (!::ShellExecuteExW(&info)) {
    const DWORD err = ::GetLastError();
    result.declined = err == ERROR_CANCELLED;
    if (!result.declined) {
      LogWarn("servicesetup: ShellExecuteEx runas {} failed: {}", Narrow(verb),
              err);
    }
    return result;
  }
  result.launched = true;
  if (info.hProcess) {
    if (::WaitForSingleObject(info.hProcess, waitMs) == WAIT_OBJECT_0) {
      DWORD code = 1;
      if (::GetExitCodeProcess(info.hProcess, &code)) {
        result.exited = true;
        result.exitCode = code;
      }
    } else {
      LogWarn("servicesetup: elevated {} did not exit within {}ms",
              Narrow(verb), waitMs);
    }
    ::CloseHandle(info.hProcess);
  }
  return result;
}

}  // namespace urnw
