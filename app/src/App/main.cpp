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
#include <shobjidl_core.h>

#include <format>
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

std::wstring HresultDetail(winrt::hresult_error const& e) {
  return std::format(L"HRESULT 0x{:08X}: {}",
                     static_cast<uint32_t>(static_cast<int32_t>(e.code())),
                     std::wstring_view(e.message()));
}

// Hand this launch's activation to the instance that owns the key, then exit.
// RedirectActivationToAsync must not be waited on directly from this STA thread
// (it would deadlock), so it runs on a worker while this thread keeps pumping —
// the pattern from the Windows App SDK instancing sample.
void RedirectActivation(AppInstance const& primary, AppActivationArguments const& args) {
  winrt::handle redirected{::CreateEventW(nullptr, TRUE, FALSE, nullptr)};
  if (!redirected) {
    urnw::LogError("startup: redirect event could not be created: {}", ::GetLastError());
    return;
  }
  // The worker owns the only call that can throw here, and an exception on a
  // std::thread is std::terminate — i.e. the "second launch does nothing at
  // all" bug. Catch it, log it, and let the wait below finish.
  std::thread worker([&] {
    try {
      primary.RedirectActivationToAsync(args).get();
    } catch (winrt::hresult_error const& e) {
      urnw::LogError("startup: redirect to the running instance failed: {}",
                     urnw::Narrow(HresultDetail(e)));
    } catch (...) {
      urnw::LogError("startup: redirect to the running instance failed (unknown)");
    }
    ::SetEvent(redirected.get());
  });
  HANDLE handles[] = {redirected.get()};
  DWORD index = 0;
  ::CoWaitForMultipleObjects(CWMO_DEFAULT, INFINITE, 1, handles, &index);
  worker.join();
}

}  // namespace

int __stdcall wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  // First instruction. The log line this writes is the proof the process got
  // this far: if a launch the user reports leaves no "startup: wWinMain" line,
  // the process died BEFORE its own entry point, which for an unpackaged WinUI 3
  // app means the Windows App SDK bootstrapper found no Windows App Runtime (it
  // runs from a CRT initializer, ahead of everything here). See NEXTSTEPS.md.
  urnw::StartupLogInit();

  std::vector<std::wstring> diagnostics = urnw::CollectDiagnostics();
  urnw::LogDiagnostics(diagnostics);
  const bool diagnose = urnw::WantsDiagnose();

  // COM/WinRT for this thread. An exception escaping wWinMain is std::terminate
  // — a silent exit, the symptom this path exists to remove — so every WinRT
  // call below is inside a catch that ends in a message box.
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

  // The Windows App SDK's single-instance registration. This is the first call
  // into the App SDK proper: if the runtime is present but broken, or the
  // package is not usable by this user, it throws here — one of the four causes
  // of "opened it, saw nothing".
  AppActivationArguments args{nullptr};
  AppInstance primary{nullptr};
  std::wstring instanceLine;
  try {
    args = AppInstance::GetCurrent().GetActivatedEventArgs();
    primary = AppInstance::FindOrRegisterForKey(kInstanceKey);
    instanceLine = primary.IsCurrent()
                       ? L"this process owns the key (no other instance running)"
                       : L"another instance already owns the key — the app is running";
  } catch (winrt::hresult_error const& e) {
    if (diagnose) {
      diagnostics.push_back(L"  app instance     : FAILED: " + HresultDetail(e));
      return urnw::WriteDiagnosticsToConsole(diagnostics);
    }
    urnw::FailVisible(
        L"The Windows App SDK is not available, so the app cannot start.\n"
        L"AppInstance::FindOrRegisterForKey failed.\n\n"
        L"The usual cause is a missing or mismatched Windows App Runtime — see "
        L"NEXTSTEPS.md, \"Install the Windows App Runtime\".",
        HresultDetail(e));
    return 1;
  }
  urnw::LogInfo("startup: single instance: {}", urnw::Narrow(instanceLine));

  if (diagnose) {
    diagnostics.push_back(L"  app instance     : " + instanceLine);
    return urnw::WriteDiagnosticsToConsole(diagnostics);
  }

  // The first launch owns the key; every later launch redirects its activation
  // (a urnetwork:// wallet callback, or a plain relaunch) to it and exits.
  if (!primary.IsCurrent()) {
    urnw::LogInfo("startup: redirecting this activation to the running instance");
    RedirectActivation(primary, args);
    urnw::LogInfo("startup: redirected; exiting this launch");
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
  return 0;
}
