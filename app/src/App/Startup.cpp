// SPDX-License-Identifier: MPL-2.0
// the project compiles with /Yu"pch.h" (App.vcxproj), so every translation unit
// must include it first
#include "pch.h"

#include "Startup.h"

#include <shellapi.h>  // CommandLineToArgvW

#include <atomic>
#include <filesystem>
#include <format>
#include <optional>

#include "Ids.h"
#include "Localization.h"
#include "Log.h"
#include "Paths.h"
#include "Strings.h"

// The Windows App SDK version this binary was BUILT against, injected from the
// single MSBuild property that also drives the PackageReference (App.vcxproj),
// so the two cannot drift. Printed next to the runtime actually loaded: a
// major.minor mismatch is then one line to read instead of an invisible
// incompatibility.
#if !defined(URN_WINDOWSAPPSDK_VERSION)
#define URN_WINDOWSAPPSDK_VERSION L"(not injected by the build)"
#endif

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

// Whether StartupLogInit got the log file open. Unset until it has run, so the
// diagnostics never claim anything about a log nobody tried to open yet.
std::optional<bool> g_logOpened;

// Written on the UI thread in OnLaunched, read on the same thread after the
// message loop ends; atomic anyway, because a flag that decides whether the
// owner gets told anything is not the place to be clever.
std::atomic<bool> g_launched{false};

// Set by FailVisible, which can fire from the UI thread, a tray WndProc or the
// XAML unhandled-exception handler.
std::atomic<bool> g_failed{false};

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

// Where a dll the exe imports at LOAD TIME actually came from. "MISSING" is
// unreachable for these — the process could not have reached wWinMain without
// them — so the question worth answering is WHICH copy is loaded when there is
// more than one drop on disk.
std::wstring LoadedModule(const wchar_t* name, const std::filesystem::path& beside) {
  if (HMODULE mod = ::GetModuleHandleW(name)) {
    wchar_t path[MAX_PATH]{};
    const DWORD n = ::GetModuleFileNameW(mod, path, MAX_PATH);
    if (n) return L"loaded from " + std::wstring(path, n);
    return L"loaded (path unavailable)";
  }
  return std::format(L"NOT LOADED (unexpected for a load-time import) — on disk: {}",
                     Presence(beside));
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
// (wide, so non-ascii paths survive), a redirected one (`--diagnose > out.txt`,
// or a pipe into PowerShell) takes utf-8 bytes. False when there is nothing to
// write to, which is the caller's cue to use a message box instead.
bool WriteStdout(HANDLE out, std::wstring_view text) {
  if (out == nullptr || out == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0;
  DWORD mode = 0;
  if (::GetConsoleMode(out, &mode)) {
    return ::WriteConsoleW(out, text.data(), static_cast<DWORD>(text.size()), &written,
                           nullptr) != FALSE;
  }
  const std::string utf8 = Narrow(text);
  return ::WriteFile(out, utf8.data(), static_cast<DWORD>(utf8.size()), &written,
                     nullptr) != FALSE;
}

}  // namespace

void StartupLogInit() {
  const std::filesystem::path logFile = LogDir(/*isService=*/false) / L"urnetwork-app.log";
  const bool opened = LogInit(logFile, "app");
  g_logOpened = opened;
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
  std::wstring logLine = log.empty() ? std::wstring(L"(none — debugger only)") : log.wstring();
  if (g_logOpened && !*g_logOpened)
    logLine += L"  ** COULD NOT BE OPENED — nothing is being written to it **";
  lines.push_back(std::format(L"  log file         : {}", logLine));
  lines.push_back(std::format(L"  storage root     : {}",
                              StorageRoot(/*isService=*/false).wstring()));
  lines.push_back(std::format(L"  built against    : Windows App SDK {}",
                              URN_WINDOWSAPPSDK_VERSION));
  lines.push_back(std::format(L"  app runtime      : {}", AppRuntimeProbe()));
  lines.push_back(std::format(L"  bootstrap dll    : {}",
                              LoadedModule(kBootstrapDll, dir / kBootstrapDll)));
  // The file check only answers half of cause 4 — see ResourceProbe(), which
  // answers the half that matters (does MRT actually resolve a key) once there
  // is an apartment to ask from.
  lines.push_back(std::format(L"  resources.pri    : {}", Presence(dir / L"resources.pri")));
  lines.push_back(std::format(L"  URnetworkSdk.dll : {}",
                              LoadedModule(L"URnetworkSdk.dll", dir / L"URnetworkSdk.dll")));
  lines.push_back(std::format(L"  service pipe     : {}", ServicePipeProbe()));
  return lines;
}

void LogDiagnostics(const std::vector<std::wstring>& lines) {
  for (const auto& line : lines) LogInfo("startup: {}", Narrow(line));
}

std::wstring ResourceProbe() {
  // The app's own honest test. Localized() returns the key id itself when MRT
  // could not resolve it (Localization.cpp), so "app_name" coming back as
  // "app_name" means the pri did not load — the UI would render raw keys where
  // the product name and every button label belong. A file-size check cannot
  // tell that apart from a working one.
  std::wstring value;
  try {
    value = Localized("app_name");
  } catch (...) {
    return L"  resources (mrt)  : FAILED — the resource loader threw";
  }
  if (value == L"app_name") {
    return L"  resources (mrt)  : NOT RESOLVING — the UI would render key ids "
           L"(\"app_name\") instead of text; resources.pri is missing, unindexed, "
           L"or not beside the exe";
  }
  return std::format(L"  resources (mrt)  : resolving (app_name -> \"{}\")", value);
}

int WriteDiagnosticsToConsole(const std::vector<std::wstring>& lines) {
  std::wstring text(L"\r\n");
  for (const auto& line : lines) text += line + L"\r\n";

  // A GUI-subsystem process has no console of its own. Attaching to the
  // parent's is what makes `URnetwork.exe --diagnose` from a terminal print
  // there; the shell has already returned its prompt (it does not wait on a GUI
  // app), so the output lands under it. Redirected or piped, the std handle is
  // already valid and this is a no-op.
  const bool attached = ::AttachConsole(ATTACH_PARENT_PROCESS) != FALSE;

  // Attaching a console does not reliably give this process std handles — a
  // process launched without inheritable handles keeps its null ones — so open
  // the console's own device when they are missing. Getting this wrong prints
  // nothing at all, which is the failure mode this command exists to end.
  HANDLE out = ::GetStdHandle(STD_OUTPUT_HANDLE);
  bool ownsHandle = false;
  if ((out == nullptr || out == INVALID_HANDLE_VALUE) && attached) {
    out = ::CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        nullptr, OPEN_EXISTING, 0, nullptr);
    ownsHandle = (out != INVALID_HANDLE_VALUE);
  }

  const bool wrote = WriteStdout(out, text);
  if (ownsHandle) ::CloseHandle(out);
  if (attached) ::FreeConsole();

  // Double-clicked from Explorer there is no console and no redirection, so the
  // box is the whole output.
  if (!wrote) {
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

void MarkLaunched() { g_launched.store(true); }
bool WasLaunched() { return g_launched.load(); }
bool HadVisibleFailure() { return g_failed.load(); }

void FailVisible(std::wstring_view cause, std::wstring_view detail) {
  g_failed.store(true);
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
