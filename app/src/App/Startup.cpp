// SPDX-License-Identifier: MPL-2.0
// the project compiles with /Yu"pch.h" (App.vcxproj), so every translation unit
// must include it first
#include "pch.h"

#include "Startup.h"

#include <shellapi.h>  // CommandLineToArgvW

#include <filesystem>
#include <format>

#include "Ids.h"
#include "Log.h"
#include "Paths.h"
#include "Strings.h"

namespace urnw {
namespace {

// The Windows App Runtime's own dll. An unpackaged app (WindowsPackageType=None)
// reaches it through the bootstrapper, which adds the framework package
// directory to the process dll search path; so "can this name be resolved" is
// the same question as "did the bootstrapper find a runtime", and the resolved
// path carries the version (…\Microsoft.WindowsAppRuntime.<major>.<minor>_<ver>…).
constexpr wchar_t kAppRuntimeDll[] = L"Microsoft.WindowsAppRuntime.dll";

// Shipped next to the exe by the Windows App SDK targets; the auto-initializer
// imports it, so a missing one means the process never starts at all.
constexpr wchar_t kBootstrapDll[] = L"Microsoft.WindowsAppRuntime.Bootstrap.dll";

std::filesystem::path ExePath() {
  wchar_t path[MAX_PATH]{};
  const DWORD n = ::GetModuleFileNameW(nullptr, path, MAX_PATH);
  return std::filesystem::path(std::wstring(path, n));
}

// "present (12345 bytes)" / "MISSING". Never throws: file_size takes the
// error_code overload, because a diagnostic that dies while diagnosing is worse
// than no diagnostic.
std::wstring Presence(const std::filesystem::path& file) {
  std::error_code ec;
  const auto size = std::filesystem::file_size(file, ec);
  if (ec) return L"MISSING";
  return std::format(L"present ({} bytes)", size);
}

std::wstring OsVersion() {
  // GetVersionExW lies to a manifest that does not opt in; RtlGetVersion does
  // not. It is ntdll's, so it is resolved dynamically.
  using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
  HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
  auto rtlGetVersion =
      ntdll ? reinterpret_cast<RtlGetVersionFn>(::GetProcAddress(ntdll, "RtlGetVersion"))
            : nullptr;
  if (!rtlGetVersion) return L"unknown";
  RTL_OSVERSIONINFOW info{};
  info.dwOSVersionInfoSize = static_cast<ULONG>(sizeof(info));
  if (rtlGetVersion(&info) != 0) return L"unknown";
  return std::format(L"{}.{}.{}", info.dwMajorVersion, info.dwMinorVersion,
                     info.dwBuildNumber);
}

// Cause 2 of the four look-alikes. Loading the dll by name asks the loader the
// same question the App SDK asked before wWinMain ran; if we get here at all the
// bootstrapper already succeeded, so this is really a "which runtime" readout —
// except when the auto-initializer was configured not to be fatal, where it is
// the failure itself.
std::wstring AppRuntimeProbe() {
  bool loadedHere = false;
  HMODULE runtime = ::GetModuleHandleW(kAppRuntimeDll);
  if (!runtime) {
    runtime = ::LoadLibraryExW(kAppRuntimeDll, nullptr, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    loadedHere = (runtime != nullptr);
  }
  if (!runtime) {
    return std::format(
        L"NOT FOUND (LoadLibrary error {}) — the Windows App Runtime is not "
        L"installed; the app cannot start without it (NEXTSTEPS.md)",
        ::GetLastError());
  }
  wchar_t path[MAX_PATH]{};
  const DWORD n = ::GetModuleFileNameW(runtime, path, MAX_PATH);
  std::wstring resolved(path, n);
  if (loadedHere) ::FreeLibrary(runtime);
  return resolved.empty() ? std::wstring(L"present") : L"present: " + resolved;
}

// Informational only: the service is a separate install and is expected to be
// absent until WP2 lands, so this is never reported as an error.
std::wstring ServicePipeProbe() {
  if (::WaitNamedPipeW(ids::kControlPipeName, NMPWAIT_NOWAIT)) return L"listening";
  const DWORD err = ::GetLastError();
  if (err == ERROR_FILE_NOT_FOUND)
    return L"not listening (urnetworkd is not running — expected until the service is installed)";
  return std::format(L"not available (error {})", err);
}

// stdout for a /SUBSYSTEM:WINDOWS process: a console handle takes WriteConsoleW
// (wide, so non-ascii paths survive), a redirected handle takes utf-8 bytes.
void WriteStdout(std::wstring_view text) {
  HANDLE out = ::GetStdHandle(STD_OUTPUT_HANDLE);
  if (out == nullptr || out == INVALID_HANDLE_VALUE) return;
  DWORD mode = 0;
  if (::GetConsoleMode(out, &mode)) {
    DWORD written = 0;
    ::WriteConsoleW(out, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    return;
  }
  const std::string utf8 = Narrow(text);
  DWORD written = 0;
  ::WriteFile(out, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
}

}  // namespace

void StartupLogInit() {
  const std::filesystem::path logFile = LogDir(/*isService=*/false) / L"urnetwork-app.log";
  const bool opened = LogInit(logFile, "app");
  // Every line below also goes to OutputDebugString, so a failed open costs the
  // file but not the log. It is reported in the diagnostics rather than in a
  // message box: it does not stop the app, and a box on every launch of a
  // machine with a full disk would be its own bug.
  LogInfo("startup: ----------------------------------------------------------");
  LogInfo("startup: wWinMain (pid {}) exe={}", ::GetCurrentProcessId(),
          Narrow(ExePath().wstring()));
  if (!opened) LogWarn("startup: could not open {} — logging to the debugger only",
                       Narrow(logFile.wstring()));
}

std::vector<std::wstring> CollectDiagnostics() {
  const std::filesystem::path exe = ExePath();
  const std::filesystem::path dir = exe.parent_path();
  const std::filesystem::path log = LogFilePath();

#if defined(_M_ARM64)
  constexpr wchar_t kArch[] = L"ARM64";
#elif defined(_M_X64)
  constexpr wchar_t kArch[] = L"x64";
#else
  constexpr wchar_t kArch[] = L"unknown";
#endif

  std::vector<std::wstring> lines;
  lines.push_back(L"URnetwork startup diagnostics");
  lines.push_back(std::format(L"  build            : {} ({} {})", kArch,
                              Widen(__DATE__), Widen(__TIME__)));
  lines.push_back(std::format(L"  windows          : {}", OsVersion()));
  lines.push_back(std::format(L"  process          : pid {}", ::GetCurrentProcessId()));
  lines.push_back(std::format(L"  executable       : {}", exe.wstring()));
  lines.push_back(std::format(L"  command line     : {}", ::GetCommandLineW()));
  lines.push_back(std::format(L"  log file         : {}",
                              log.empty() ? std::wstring(L"(none — debugger only)")
                                          : log.wstring()));
  lines.push_back(std::format(L"  storage root     : {}",
                              StorageRoot(/*isService=*/false).wstring()));
  lines.push_back(std::format(L"  app runtime      : {}", AppRuntimeProbe()));
  lines.push_back(std::format(L"  bootstrap dll    : {}", Presence(dir / kBootstrapDll)));
  // resources.pri missing is cause 4's cheap half: the app still runs but every
  // string renders as its key id (Localization.cpp falls back to the key).
  lines.push_back(std::format(L"  resources.pri    : {}", Presence(dir / L"resources.pri")));
  lines.push_back(std::format(L"  URnetworkSdk.dll : {}", Presence(dir / L"URnetworkSdk.dll")));
  lines.push_back(std::format(L"  service pipe     : {}", ServicePipeProbe()));
  return lines;
}

void LogDiagnostics(const std::vector<std::wstring>& lines) {
  for (const auto& line : lines) LogInfo("startup: {}", Narrow(line));
}

int WriteDiagnosticsToConsole(const std::vector<std::wstring>& lines) {
  // A GUI-subsystem process has no console of its own. Attaching to the parent's
  // is what makes `URnetwork.exe --diagnose` from a terminal print there; the
  // shell has already returned its prompt (it does not wait on a GUI app), so
  // the output lands under it. Double-clicked, there is no parent console and
  // the message box below is the whole output.
  const bool console = ::AttachConsole(ATTACH_PARENT_PROCESS) != FALSE;

  std::wstring text;
  for (const auto& line : lines) text += line + L"\r\n";

  if (console) {
    WriteStdout(L"\r\n");
    WriteStdout(text);
    ::FreeConsole();
  } else {
    ::MessageBoxW(nullptr, text.c_str(), L"URnetwork diagnostics",
                  MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
  }
  return 0;
}

bool WantsDiagnose() {
  int argc = 0;
  wchar_t** argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
  if (!argv) return false;
  bool wants = false;
  for (int i = 1; i < argc && !wants; ++i) {
    const std::wstring_view arg = argv[i];
    wants = (arg == L"--diagnose" || arg == L"-diagnose" || arg == L"/diagnose" ||
             arg == L"diagnose");
  }
  ::LocalFree(argv);
  return wants;
}

void FailVisible(std::wstring_view cause, std::wstring_view detail) {
  LogError("startup: FAILED: {}{}{}", Narrow(cause), detail.empty() ? "" : " | ",
           Narrow(detail));

  std::wstring body(cause);
  if (!detail.empty()) body += L"\n\n" + std::wstring(detail);
  const std::filesystem::path log = LogFilePath();
  if (!log.empty()) body += L"\n\nLog file:\n" + log.wstring();
  body += L"\n\nFor the full startup diagnostics, run in a terminal:\n"
          L"    URnetwork.exe --diagnose";

  // MB_SETFOREGROUND + MB_TOPMOST: nothing else of this app is on screen, and a
  // failure box behind the window that launched us is a silent failure again.
  ::MessageBoxW(nullptr, body.c_str(), L"URnetwork could not start",
                MB_OK | MB_ICONERROR | MB_SETFOREGROUND | MB_TOPMOST);
}

}  // namespace urnw
