// URnetwork tray app entry point. Single-instance, registers the AppUserModelId
// (for toasts + tray grouping), then hands off to the WinUI 3 Application, which
// creates the tray + SDK host in OnLaunched. The user's launch opens the
// window; an autostart at sign-in (--autostart) shows only the tray icon.
//
// Single-instancing is the Windows App SDK's (AppInstance), not a bare mutex,
// because a second launch is not always a no-op: the MSI registers the
// urnetwork:// scheme (installer/Package.wxs), so the browser returning from the
// ur.io/wallet-connect bridge launches the app with the wallet callback uri. That
// launch is redirected to the running instance, which receives it on
// AppInstance::Activated — the common case, since the user started the sign-in
// from the running app. A launch that reaches an instance which is exiting (the
// tray's Quit holds it for as long as stopping the service takes) waits for
// that instance to end and then starts the app itself (Common/InstanceHandover.h).
//
// This file is also the app's black box. There is no UI until the tray icon
// exists, so every step from here to OnLaunched is logged (Startup.cpp) and
// every failure is turned into a message box naming the cause and the log file:
// a tray app that exits silently is indistinguishable from a tray app that
// started and was not noticed. `--diagnose` prints the same facts and exits.
//
// SPDX-License-Identifier: MPL-2.0
#include "pch.h"

#include <shellapi.h>  // CommandLineToArgvW (IsRelaunchHandoff)
#include <shobjidl_core.h>

#include <atomic>
#include <chrono>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "App.xaml.h"
#include "AppController.h"  // DeepLinkFromActivation
#include "Ids.h"
#include "InstanceHandover.h"
#include "Log.h"
#include "SingleInstance.h"
#include "Startup.h"
#include "Strings.h"

using namespace winrt::Microsoft::Windows::AppLifecycle;

namespace {

// Key for the single instance. Lives in Ids.h with the other app identities,
// where every site that registers it must read the SAME key.
constexpr const wchar_t* kInstanceKey = urnw::ids::kSingleInstanceKey;

std::wstring HresultDetail(winrt::hresult_error const& e) {
  return std::format(L"HRESULT 0x{:08X}: {}",
                     static_cast<uint32_t>(static_cast<int32_t>(e.code())),
                     std::wstring_view(e.message()));
}

// The box for a key registration that threw, on the first attempt or a later
// one.
constexpr wchar_t kAppSdkUnavailable[] =
    L"The Windows App SDK is not available, so the app cannot start.\n"
    L"AppInstance::FindOrRegisterForKey failed.\n\n"
    L"The usual cause is a missing or mismatched Windows App Runtime — see "
    L"NEXTSTEPS.md, \"Install the Windows App Runtime\".";

// The box for a launch that neither started the app nor handed its activation
// over. Only an instance that still runs, is not exiting and did not take the
// launch is reported as already running.
void ReportLaunchFailure(urnw::instance::LaunchResult result, const std::wstring& detail) {
  using urnw::instance::LaunchResult;
  switch (result) {
    case LaunchResult::NoResponse:
      urnw::FailVisible(
          L"URnetwork is already running, but it did not respond.\n\n"
          L"Look for its icon in the notification area. If it is not responding, "
          L"end URnetwork.exe from Task Manager and start it again.",
          detail);
      return;
    case LaunchResult::Unreachable:
      urnw::FailVisible(L"URnetwork is already running, but this launch could not reach it.",
                        detail);
      return;
    case LaunchResult::Refused:
      urnw::FailVisible(L"URnetwork is already running, but it refused this launch's request.",
                        detail);
      return;
    case LaunchResult::NotStarted:
      urnw::FailVisible(
          L"URnetwork is already running, but this launch could not hand over to it.", detail);
      return;
    case LaunchResult::StillClosing:
      // the detail names the wait that ran out, or else the rounds did
      urnw::FailVisible(
          L"URnetwork is still closing, so it could not be started again yet.\n\n"
          L"Wait a moment and start it again. If this keeps happening, end "
          L"URnetwork.exe from Task Manager and start it again.",
          detail.empty() ? std::wstring(L"The key kept passing between exiting instances.")
                         : detail);
      return;
    case LaunchResult::RegistrationFailed:
      urnw::FailVisible(kAppSdkUnavailable, detail);
      return;
    case LaunchResult::Holder:
    case LaunchResult::HandedOver:
    case LaunchResult::Updating:
      return;
  }
}

// A launch redirected to this instance, on the App SDK's threadpool thread.
// Plain launches, autostarts and deep links alike go through the gate, which
// holds them until the UI is up, posts them to the UI thread after that, and
// refuses them once this instance is exiting. Returning only once the instance
// has decided is what lets the launch tell a served launch from a refused one
// (InstanceHandover.h). The deep link itself is never logged: a wallet
// callback carries the address and its signature.
void TakeRedirectedLaunch(AppActivationArguments const& redirected) {
  try {
    urnw::instance::LaunchRequest request = urnw::LaunchRequestFromActivation(redirected);
    const char* kind = !request.deepLink.empty() ? "deep link"
                       : request.autostart       ? "autostart"
                                                 : "plain launch";
    const urnw::instance::ActivationGate::Outcome outcome = urnw::Activations().Take(
        std::move(request), std::chrono::steady_clock::now() + urnw::instance::kServeBudget);
    urnw::LogInfo("app: a {} reached this instance: {}", kind, urnw::instance::ToString(outcome));
  } catch (winrt::hresult_error const& e) {
    urnw::LogError("app: a launch that reached this instance could not be taken: {}",
                   urnw::Narrow(HresultDetail(e)));
  } catch (const std::exception& e) {
    urnw::LogError("app: a launch that reached this instance could not be taken: {}", e.what());
  }
}

// One launch's side of the single instance, driven by instance::Launch
// (InstanceHandover.h): the instance that holds the key, as this launch knows
// it, and the hand-over of this launch's activation to it.
class Launcher {
 public:
  Launcher(AppActivationArguments args, AppInstance holder)
      : args_(std::move(args)), holder_(std::move(holder)) {}

  // This process holds the key.
  bool OwnsKey() const { return holder_.IsCurrent(); }

  // The updater's installer still runs (UpdateMarker.h).
  bool UpdateInProgress() const { return urnw::UpdateInProgress(); }

  // Find or register for the key again; false when the App SDK threw.
  bool Register() {
    // the watch goes with the holder it watched: its handles keep that
    // process's objects alive, its exiting signal among them
    watch_.reset();
    try {
      holder_ = AppInstance::FindOrRegisterForKey(kInstanceKey);
      return true;
    } catch (winrt::hresult_error const& e) {
      failure_ = HresultDetail(e);
      return false;
    }
  }

  urnw::instance::RedirectAttempt Redirect();

  // Waits, at most kExitingHolderBudget, for the holder's process to end.
  bool AwaitHolderExit() {
    if (watch_ && watch_->AwaitExit(urnw::instance::kExitingHolderBudget)) return true;
    closingFailure_ = std::format(
        L"The running instance was exiting and had not ended after {} ms.",
        urnw::instance::kExitingHolderBudget.count());
    return false;
  }

  // The detail for the message box: the last redirect's failure, or for a
  // launch that ends still closing, the wait that ran out (empty when the
  // rounds did).
  const std::wstring& Failure(urnw::instance::LaunchResult result) const {
    return result == urnw::instance::LaunchResult::StillClosing ? closingFailure_ : failure_;
  }

 private:
  AppActivationArguments args_;
  AppInstance holder_;
  std::optional<urnw::HolderWatch> watch_;
  std::wstring failure_;
  std::wstring closingFailure_;
};

// Hand this launch's activation to the instance that holds the key.
//
// RedirectActivationToAsync must not be waited on directly from this STA thread
// (it would deadlock), so it runs on a worker while this thread keeps pumping —
// the pattern from the Windows App SDK instancing sample. The wait watches the
// holder's process too: the App SDK's redirect waits on an event that only the
// holder sets, so a holder that ends first would hold this launch for the whole
// budget and then be reported as a running app that does not respond.
urnw::instance::RedirectAttempt Launcher::Redirect() {
  using urnw::instance::RedirectResult;
  failure_.clear();
  // Watched before anything is handed to it, so that its exiting signal reads
  // as raised even when its process has gone by the time the redirect ends.
  watch_.emplace(holder_.ProcessId());
  if (watch_->Gone()) return {.result = RedirectResult::HolderGone, .holderExiting = true};

  // THE FOREGROUND RIGHT. This process was just launched by whatever the user
  // clicked - the browser's "Open URnetwork?" after a sign-in, an email link -
  // so for a moment it may hold the right to take the foreground, which the
  // running instance needs to raise its window (shell::RaiseToFront). It has no
  // window to use it on and is about to exit. The Windows App SDK's
  // RedirectActivationToAsync already passes the right on (AppInstance::
  // QueueRequest calls AllowSetForegroundWindow), so this explicit grant is
  // belt-and-braces against that changing - and its log line is the only record
  // of whether THIS launch held a right at all. "No foreground right" means a
  // launcher with no foreground ancestry (a scheduled task, a service): there
  // only RaiseToFront's z-order fallback can put the window in front.
  try {
    if (::AllowSetForegroundWindow(holder_.ProcessId())) {
      urnw::LogInfo("startup: handed the foreground right to the running instance");
    } else {
      urnw::LogWarn("startup: no foreground right to hand to the running instance "
                    "(error {})", ::GetLastError());
    }
  } catch (...) {
    // never let this stand between the user's launch and the redirect below
  }

  // The worker can outlive a wait that ended first, so everything it touches is
  // owned by shared state rather than by this stack frame.
  struct Pending {
    winrt::handle done;
    std::atomic<bool> ok{false};
  };
  auto state = std::make_shared<Pending>();
  state->done.attach(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
  if (!state->done) {
    failure_ = std::format(L"CreateEvent failed: {}", ::GetLastError());
    return {.result = RedirectResult::NotStarted, .holderExiting = watch_->Exiting()};
  }

  // The winrt objects are copied into the worker (refcounted handles), so a
  // worker that outlives this frame still holds valid references. An exception
  // escaping a std::thread is std::terminate — the "second launch does nothing
  // at all" bug — so it catches everything.
  std::thread worker([state, holder = holder_, args = args_] {
    try {
      holder.RedirectActivationToAsync(args).get();
      state->ok.store(true);
    } catch (winrt::hresult_error const& e) {
      urnw::LogError("startup: redirect to the running instance failed: {}",
                     urnw::Narrow(HresultDetail(e)));
    } catch (...) {
      urnw::LogError("startup: redirect to the running instance failed (unknown)");
    }
    ::SetEvent(state->done.get());
  });

  long waitFailure = S_OK;
  const urnw::HolderWatch::RedirectWait wait =
      watch_->AwaitRedirect(state->done.get(), urnw::instance::kRedirectBudget, &waitFailure);
  RedirectResult result = RedirectResult::Taken;
  if (wait == urnw::HolderWatch::RedirectWait::Done) {
    worker.join();  // signalled: the worker has only ::SetEvent left to run
    if (!state->ok.load()) {
      result = RedirectResult::Failed;
      failure_ = L"The activation redirect reported a failure; see the log.";
    }
  } else {
    // Never join a wait that did not complete: join() does not pump messages,
    // so it would block the apartment the redirect needs and turn a slow
    // redirect into the permanent hang the budget exists to prevent. A worker
    // left behind a holder that ended stays parked on an event nobody sets,
    // and ends with this process.
    worker.detach();
    if (wait == urnw::HolderWatch::RedirectWait::HolderGone) {
      result = RedirectResult::HolderGone;
    } else if (wait == urnw::HolderWatch::RedirectWait::TimedOut) {
      result = RedirectResult::TimedOut;
      failure_ = std::format(L"The activation redirect timed out after {} ms.",
                             urnw::instance::kRedirectBudget.count());
    } else {
      result = RedirectResult::WaitFailed;
      failure_ = std::format(L"CoWaitForMultipleObjects failed: HRESULT 0x{:08X}",
                             static_cast<uint32_t>(waitFailure));
    }
  }
  return {.result = result, .holderExiting = watch_->Exiting()};
}

// Was this process spawned by an update relaunch (the portable builds' former
// rename-swap updater, beta spec §5)? Same argv scan as Startup.cpp's
// WantsDiagnose. The flag never
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

  // The installer's relaunch after an update starts while the update helper
  // still waits on msiexec, and the update marker names the helper: it waits
  // for the update to end instead of being refused by it, then asks like every
  // launch (InstanceHandover.h kAfterUpdateArgument).
  if (urnw::LaunchedAfterUpdate()) urnw::AwaitUpdateEnd();

  // This process's exiting signal and its handler for launches redirected to
  // it come before the key: a later launch can find this process the moment the
  // key is its own, and the App SDK consumes a redirect that arrives with no
  // handler, so a launch in that moment would be lost. Every launch sets both
  // up; only the one that comes to hold the key is ever redirected to.
  urnw::CreateExitingSignal();

  // The Windows App SDK's single-instance registration — the first call into
  // the App SDK proper. If the runtime is present but broken, or unusable by
  // this user, it throws here.
  AppActivationArguments args{nullptr};
  AppInstance primary{nullptr};
  bool isPrimary = false;
  try {
    AppInstance current = AppInstance::GetCurrent();
    args = current.GetActivatedEventArgs();
    current.Activated([](winrt::Windows::Foundation::IInspectable const&,
                         AppActivationArguments const& redirected) {
      TakeRedirectedLaunch(redirected);
    });
    primary = AppInstance::FindOrRegisterForKey(kInstanceKey);
    isPrimary = primary.IsCurrent();
  } catch (winrt::hresult_error const& e) {
    urnw::FailVisible(kAppSdkUnavailable, HresultDetail(e));
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

  // The first launch owns the key. Every later launch hands its activation (a
  // urnetwork:// wallet callback, or a plain relaunch) to the instance that
  // holds the key and exits; when that instance is exiting, the launch waits
  // for it to end and starts the app itself (InstanceHandover.h). While the
  // updater's installer runs, no launch, the first included, starts the app.
  {
    Launcher launcher(args, primary);
    const urnw::instance::LaunchResult launch = urnw::instance::Launch(launcher);
    urnw::LogInfo("startup: launch: {}", urnw::instance::ToString(launch));
    switch (launch) {
      case urnw::instance::LaunchResult::Holder:
        break;
      case urnw::instance::LaunchResult::HandedOver:
        return 0;
      case urnw::instance::LaunchResult::Updating:
        // an autostart leaves without a word; the user's launch is told why
        if (!urnw::LaunchedByAutostart()) urnw::ShowUpdatingNotice();
        // as below: a redirect's worker may still be blocked in its call
        ::ExitProcess(0);
      default:
        ReportLaunchFailure(launch, launcher.Failure(launch));
        // A worker thread may still be blocked inside the redirect call. Ending
        // the process outright is the honest close for a launch that has already
        // shown its error: unwinding would race that thread against the CRT
        // teardown it logs through.
        ::ExitProcess(1);
    }
  }

  // Not fatal on its own (it costs toast delivery and tray grouping), so it is
  // logged rather than shown.
  if (HRESULT hr = ::SetCurrentProcessExplicitAppUserModelID(urnw::ids::kAppUserModelId);
      FAILED(hr)) {
    urnw::LogWarn("startup: SetCurrentProcessExplicitAppUserModelID failed: 0x{:08X}",
                  static_cast<uint32_t>(hr));
  }

  // Every exit from here on begins exiting before its message box, as
  // AppController::Shutdown does for the app's own endings: a launch that
  // reaches this instance meanwhile is refused and starts the app once this
  // process has ended (SingleInstance.h).
  urnw::LogInfo("startup: Application::Start (XAML)");
  try {
    winrt::Microsoft::UI::Xaml::Application::Start([](auto&&) {
      winrt::make<winrt::URnetwork::implementation::App>();
    });
  } catch (winrt::hresult_error const& e) {
    urnw::BeginExiting();
    urnw::FailVisible(
        L"The app's user interface failed to start (XAML).\n\n"
        L"This usually means the Windows App Runtime or the app's resources "
        L"(resources.pri, next to URnetwork.exe) could not be loaded.",
        HresultDetail(e));
    return 1;
  } catch (std::exception const& e) {
    urnw::BeginExiting();
    urnw::FailVisible(L"The app's user interface failed to start (XAML).",
                      urnw::Widen(e.what()));
    return 1;
  } catch (...) {
    urnw::BeginExiting();
    urnw::FailVisible(L"The app's user interface failed to start (XAML).",
                      L"An unknown exception escaped Application::Start.");
    return 1;
  }

  // Application::Start does not return until the app exits (tray "Quit"). An
  // ending that ran Shutdown began exiting already; this covers the rest.
  urnw::BeginExiting();
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
