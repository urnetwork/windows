// URnetwork tray app entry point. Single-instance, registers the AppUserModelId
// (for toasts + tray grouping), then hands off to the WinUI 3 Application, which
// creates the tray + SDK host in OnLaunched. The window is opened from the tray.
//
// Single-instancing is the Windows App SDK's (AppInstance), not a bare mutex,
// because a second launch is not always a no-op: the MSI registers the
// urnetwork:// scheme (installer/Package.wxs), so the browser returning from the
// ur.io/wallet-connect bridge launches the app with the wallet callback uri. That
// launch is redirected to the running instance, which receives it on
// AppInstance::Activated (see App::OnLaunched) — the common case, since the user
// started the sign-in from the running app.
//
// This file is also the app's black box. There is no UI until the tray icon
// exists, so every step from here to OnLaunched is logged (Startup.cpp) and
// every failure is turned into a message box naming the cause and the log file:
// a tray app that exits silently is indistinguishable from a tray app that
// started and was not noticed. `--diagnose` prints the same facts and exits.
//
// SPDX-License-Identifier: MPL-2.0
#include "pch.h"

#include <objbase.h>  // CoWaitForMultipleObjects
#include <shellapi.h>  // CommandLineToArgvW (IsRelaunchHandoff)
#include <shobjidl_core.h>

#include <atomic>
#include <format>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "App.xaml.h"
#include "Ids.h"
#include "Log.h"
#include "Startup.h"
#include "Strings.h"

using namespace winrt::Microsoft::Windows::AppLifecycle;

namespace {

// Key for the single instance. Stable, like the other app identities (Ids.h).
constexpr wchar_t kInstanceKey[] = L"URnetwork.Desktop";

// How long a second launch waits for the running instance to accept its
// activation. This wait used to be INFINITE: if the primary is wedged — or if
// the wait itself fails, which also removes the message pumping this
// cross-apartment call depends on — the second launch hangs forever with no
// window, no message and no exit. That is a process which MANUFACTURES the
// symptom this work package exists to remove, and "nothing happened, so I ran
// it again" is exactly how a user gets there.
constexpr DWORD kRedirectTimeoutMs = 15000;

std::wstring HresultDetail(winrt::hresult_error const& e) {
  return std::format(L"HRESULT 0x{:08X}: {}",
                     static_cast<uint32_t>(static_cast<int32_t>(e.code())),
                     std::wstring_view(e.message()));
}

// Hand this launch's activation to the instance that owns the key. True when it
// was accepted; false means it was not, and the user has already been told.
//
// RedirectActivationToAsync must not be waited on directly from this STA thread
// (it would deadlock), so it runs on a worker while this thread keeps pumping —
// the pattern from the Windows App SDK instancing sample.
bool RedirectActivation(AppInstance const& primary, AppActivationArguments const& args) {
  // The worker can outlive a wait that timed out, so everything it touches is
  // owned by shared state rather than by this stack frame.
  struct Redirect {
    winrt::handle done;
    std::atomic<bool> ok{false};
  };
  auto state = std::make_shared<Redirect>();
  state->done.attach(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
  if (!state->done) {
    urnw::FailVisible(
        L"URnetwork is already running, but this launch could not hand over to it.",
        std::format(L"CreateEvent failed: {}", ::GetLastError()));
    return false;
  }

  // The winrt objects are copied into the worker (refcounted handles), so a
  // worker that outlives this frame still holds valid references. An exception
  // escaping a std::thread is std::terminate — the "second launch does nothing
  // at all" bug — so it catches everything.
  std::thread worker([state, primary, args] {
    try {
      primary.RedirectActivationToAsync(args).get();
      state->ok.store(true);
    } catch (winrt::hresult_error const& e) {
      urnw::LogError("startup: redirect to the running instance failed: {}",
                     urnw::Narrow(HresultDetail(e)));
    } catch (...) {
      urnw::LogError("startup: redirect to the running instance failed (unknown)");
    }
    ::SetEvent(state->done.get());
  });

  HANDLE handles[] = {state->done.get()};
  DWORD index = 0;
  const HRESULT hr =
      ::CoWaitForMultipleObjects(CWMO_DEFAULT, kRedirectTimeoutMs, 1, handles, &index);

  if (SUCCEEDED(hr) && state->ok.load()) {
    worker.join();  // signalled: the worker has only ::SetEvent left to run
    return true;
  }

  // Never join a wait that did not complete: join() does not pump messages, so
  // it would block the apartment the redirect needs and turn a slow redirect
  // into the permanent hang this timeout exists to prevent. The caller ends the
  // process without unwinding instead (see wWinMain).
  worker.detach();
  if (hr == RPC_S_CALLPENDING) {
    urnw::FailVisible(
        L"URnetwork is already running, but it did not respond.\n\n"
        L"Look for its icon in the notification area. If it is not responding, "
        L"end URnetwork.exe from Task Manager and start it again.",
        std::format(L"The activation redirect timed out after {} ms.",
                    kRedirectTimeoutMs));
  } else if (FAILED(hr)) {
    urnw::FailVisible(
        L"URnetwork is already running, but this launch could not reach it.",
        std::format(L"CoWaitForMultipleObjects failed: HRESULT 0x{:08X}",
                    static_cast<uint32_t>(hr)));
  } else {
    urnw::FailVisible(
        L"URnetwork is already running, but it refused this launch's request.",
        L"The activation redirect reported a failure; see the log.");
  }
  return false;
}

// Was this process spawned by the update relaunch (AppController::RelaunchOnto,
// beta spec §5)? Same argv scan as Startup.cpp's WantsDiagnose. The flag never
// GRANTS anything — it only buys the bounded key retry below, so a user typing
// it by hand merely waits a few seconds longer before redirecting.
bool IsRelaunchHandoff() {
  int argc = 0;
  wchar_t** argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
  if (!argv) return false;
  bool relaunched = false;
  for (int i = 1; i < argc && !relaunched; ++i)
    relaunched = std::wstring_view(argv[i]) == L"--relaunched";
  ::LocalFree(argv);
  return relaunched;
}

// Is another instance holding the single-instance key? Answered WITHOUT
// registering anything: a --diagnose run must not briefly become the app's
// primary instance and have a real launch redirected to it.
std::wstring InstanceProbe() {
  try {
    for (auto const& instance : AppInstance::GetInstances()) {
      const winrt::hstring key = instance.Key();
      if (std::wstring_view(key) == kInstanceKey)
        return L"  app instance     : another instance holds the key — the app is running";
    }
    return L"  app instance     : no instance holds the key — the app is not running";
  } catch (winrt::hresult_error const& e) {
    // The App SDK itself is unusable: cause 3 of the four look-alikes.
    return L"  app instance     : FAILED (the Windows App SDK is not usable) — " +
           HresultDetail(e);
  }
}

}  // namespace

int __stdcall wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  std::vector<std::wstring> diagnostics;
  bool diagnose = false;

  // The first instructions, and themselves guarded: they do filesystem work,
  // formatting and a LoadLibrary, and an exception escaping wWinMain is
  // std::terminate — a silent exit produced by the very code whose job is to
  // make failure visible. The fallback box is hard-coded and allocates nothing.
  try {
    // The log line this writes is the proof the process got this far: if a
    // launch the user reports leaves no "startup: wWinMain" line, the process
    // died BEFORE its own entry point, which for an unpackaged WinUI 3 app
    // means the Windows App SDK bootstrapper found no usable Windows App
    // Runtime (it runs from a CRT initializer, ahead of everything here).
    // NEXTSTEPS.md §0 has how to tell that apart from "it never ran at all".
    urnw::StartupLogInit();
    diagnostics = urnw::CollectDiagnostics();
    urnw::LogDiagnostics(diagnostics);
    diagnose = urnw::WantsDiagnose();
  } catch (...) {
    ::MessageBoxW(nullptr,
                  L"URnetwork could not start: its own startup diagnostics failed.\n\n"
                  L"This usually means %LOCALAPPDATA% is not writable for this user.",
                  L"URnetwork could not start",
                  MB_OK | MB_ICONERROR | MB_SETFOREGROUND | MB_TOPMOST);
    return 1;
  }

  // COM/WinRT for this thread. Everything past here can throw hresult_error, so
  // every call is inside a catch that ends in a message box.
  try {
    winrt::init_apartment(winrt::apartment_type::single_threaded);
  } catch (winrt::hresult_error const& e) {
    if (diagnose) {
      diagnostics.push_back(L"  com apartment    : FAILED: " + HresultDetail(e));
      return urnw::WriteDiagnosticsToConsole(diagnostics);
    }
    urnw::FailVisible(L"COM could not be initialized for this process.", HresultDetail(e));
    return 1;
  }

  // --diagnose exits here, before the single-instance registration.
  if (diagnose) {
    // Both of these need the apartment, and ResourceProbe is deliberately only
    // run HERE and in OnLaunched — see its declaration: it would otherwise be
    // the first caller of Localized(), whose loader is cached on first use, and
    // a probe that fails early would make the whole UI render key ids.
    diagnostics.push_back(urnw::ResourceProbe());
    diagnostics.push_back(InstanceProbe());
    return urnw::WriteDiagnosticsToConsole(diagnostics);
  }

  // The Windows App SDK's single-instance registration — the first call into
  // the App SDK proper. If the runtime is present but broken, or unusable by
  // this user, it throws here.
  AppActivationArguments args{nullptr};
  AppInstance primary{nullptr};
  bool isPrimary = false;
  try {
    args = AppInstance::GetCurrent().GetActivatedEventArgs();
    primary = AppInstance::FindOrRegisterForKey(kInstanceKey);
    isPrimary = primary.IsCurrent();
  } catch (winrt::hresult_error const& e) {
    urnw::FailVisible(
        L"The Windows App SDK is not available, so the app cannot start.\n"
        L"AppInstance::FindOrRegisterForKey failed.\n\n"
        L"The usual cause is a missing or mismatched Windows App Runtime — see "
        L"NEXTSTEPS.md, \"Install the Windows App Runtime\".",
        HresultDetail(e));
    return 1;
  }
  urnw::LogInfo("startup: single instance: this process {}",
                isPrimary ? "owns the key" : "is a second launch");

  // The update relaunch (spec §5): the old instance unregisters its key,
  // spawns this process, then tears itself down — so finding the key still
  // held here is a RACE against a process that is already exiting, not a
  // second launch. Retry the registration, bounded, instead of redirecting an
  // activation into a teardown: the old instance is past pumping messages, so
  // that redirect could only time out after 15s and show an error for a
  // situation that resolves itself in under a second. If the key never frees
  // (the old instance is genuinely wedged), fall through to the ordinary
  // redirect path and its honest failure box.
  if (!isPrimary && IsRelaunchHandoff()) {
    urnw::LogInfo("startup: relaunch handoff — waiting for the old instance to release the key");
    for (int attempt = 0; attempt < 40 && !isPrimary; ++attempt) {
      ::Sleep(250);
      try {
        primary = AppInstance::FindOrRegisterForKey(kInstanceKey);
        isPrimary = primary.IsCurrent();
      } catch (winrt::hresult_error const& e) {
        urnw::LogError("startup: relaunch key retry failed: {}",
                       urnw::Narrow(HresultDetail(e)));
        break;
      }
    }
    urnw::LogInfo("startup: relaunch handoff {}",
                  isPrimary ? "took the key" : "timed out — redirecting");
  }

  // The first launch owns the key; every later launch redirects its activation
  // (a urnetwork:// wallet callback, or a plain relaunch) to it and exits.
  if (!isPrimary) {
    const bool redirected = RedirectActivation(primary, args);
    urnw::LogInfo("startup: second launch exiting ({})",
                  redirected ? "redirected" : "redirect FAILED");
    if (!redirected) {
      // A worker thread may still be blocked inside the redirect call. Ending
      // the process outright is the honest close for a launch that has already
      // shown its error: unwinding would race that thread against the CRT
      // teardown it logs through.
      ::ExitProcess(1);
    }
    return 0;
  }

  // Not fatal on its own (it costs toast delivery and tray grouping), so it is
  // logged rather than shown.
  if (HRESULT hr = ::SetCurrentProcessExplicitAppUserModelID(urnw::ids::kAppUserModelId);
      FAILED(hr)) {
    urnw::LogWarn("startup: SetCurrentProcessExplicitAppUserModelID failed: 0x{:08X}",
                  static_cast<uint32_t>(hr));
  }

  urnw::LogInfo("startup: Application::Start (XAML)");
  try {
    winrt::Microsoft::UI::Xaml::Application::Start([](auto&&) {
      winrt::make<winrt::URnetwork::implementation::App>();
    });
  } catch (winrt::hresult_error const& e) {
    urnw::FailVisible(
        L"The app's user interface failed to start (XAML).\n\n"
        L"This usually means the Windows App Runtime or the app's resources "
        L"(resources.pri, next to URnetwork.exe) could not be loaded.",
        HresultDetail(e));
    return 1;
  } catch (std::exception const& e) {
    urnw::FailVisible(L"The app's user interface failed to start (XAML).",
                      urnw::Widen(e.what()));
    return 1;
  } catch (...) {
    urnw::FailVisible(L"The app's user interface failed to start (XAML).",
                      L"An unknown exception escaped Application::Start.");
    return 1;
  }

  // Application::Start does not return until the app exits (tray "Quit").
  if (!urnw::WasLaunched()) {
    urnw::FailVisible(
        L"URnetwork started but its user interface never launched, so it has "
        L"exited.\n\n"
        L"The app's resources (resources.pri, next to URnetwork.exe) are the "
        L"usual cause.",
        L"Application::Start returned without OnLaunched ever running, and "
        L"without reporting an error.");
    return 1;
  }
  urnw::LogInfo("startup: message loop exited; process ending normally");
  // A run that showed the user a failure box must not also report success to
  // the shell, or to whatever script launched it.
  return urnw::HadVisibleFailure() ? 1 : 0;
}
