// SPDX-License-Identifier: MPL-2.0
#include "pch.h"

#include "AppController.h"

#include <algorithm>
#include <string_view>

#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Microsoft.UI.Interop.h>
#include <winrt/Windows.ApplicationModel.Activation.h>

#include "Ids.h"
#include "Localization.h"
#include "Log.h"
#include "MainWindow.xaml.h"
#include "Startup.h"
#include "Strings.h"
#include "WindowShell.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;

namespace urnw {
namespace {
std::unique_ptr<AppController> g_app;

constexpr bool WindowPresentationShouldRun(bool shown, bool activated) {
  return shown && activated;
}

static_assert(WindowPresentationShouldRun(true, true));
static_assert(!WindowPresentationShouldRun(true, false));
static_assert(!WindowPresentationShouldRun(false, true));
static_assert(!WindowPresentationShouldRun(false, false));

// Pull a urnetwork:// uri out of a command line. The shell appends it as an
// argument (possibly quoted); the uri itself never contains whitespace because
// the wallet-connect bridge percent-encodes the query.
std::wstring DeepLinkFromCommandLine(std::wstring_view commandLine) {
  const std::wstring prefix = std::wstring(ids::kUriScheme) + L"://";
  const size_t start = commandLine.find(prefix);
  if (start == std::wstring_view::npos) return {};
  size_t end = commandLine.find_first_of(L" \t\"", start);
  if (end == std::wstring_view::npos) end = commandLine.size();
  return std::wstring(commandLine.substr(start, end - start));
}

}  // namespace

AppController& App() { return *g_app; }
void SetApp(std::unique_ptr<AppController> app) { g_app = std::move(app); }

AppController::AppController() {
  uiThread_ = winrt::Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();
}

AppController::~AppController() {
  if (placementSaveTimer_) placementSaveTimer_.Stop();
}

template <class F>
void AppController::OnUi(F&& f) {
  if (uiThread_) {
    uiThread_.TryEnqueue([f = std::forward<F>(f)]() mutable { f(); });
  }
}

void AppController::Start() {
  // Tray icon (Win32) on the UI thread so its WndProc is pumped by the WinUI
  // message loop.
  TrayIcon::Callbacks cb;
  cb.onLeftClick = [this](POINT anchor) { ShowWindow(&anchor); };
  cb.onShowWindow = [this] { ShowWindow(nullptr); };
  cb.onConnectToggle = [this] {
    if (connected_)
      sdk_.Disconnect();
    else
      sdk_.ConnectBestAvailable();
  };
  cb.isConnected = [this] { return connected_; };
  cb.onQuit = [this] { Shutdown(); };
  // The tray icon is the app's ONLY affordance on launch — no icon means no way
  // in, and from outside that is indistinguishable from a process that died. Say
  // so on screen. The app keeps running: TrayIcon re-adds itself on
  // TaskbarCreated, so an Explorer that is still starting up recovers by itself.
  if (!tray_.Create(::GetModuleHandleW(nullptr), std::move(cb))) {
    FailVisible(
        L"URnetwork is running, but it could not put its icon in the "
        L"notification area — so there is no way to open it.\n\n"
        L"It will appear if Windows Explorer restarts. Until then you can end "
        L"URnetwork.exe from Task Manager.",
        L"See the log for the failing Shell_NotifyIcon call.");
  }

  // SDK state -> tray + window (marshaled onto the UI thread).
  sdk_.SetAuthStateHandler([this](AuthState s, const std::string& e) {
    OnUi([this, s, e] { OnAuthState(s, e); });
  });
  // The server rejected the stored auth (e.g. the client was removed): log out
  // and return to the login panel. Logout() fires the auth-state handler.
  sdk_.SetAuthInvalidHandler([this] {
    OnUi([this] { sdk_.Logout(); });
  });
  sdk_.SetJwtRefreshedHandler([this] {
    OnUi([this] { balance_.OnJwtRefreshed(); });
  });
  sdk_.SetTunnelStateHandler([this](const proto::TunnelStatus& st) {
    OnUi([this, st] { OnTunnelState(st); });
  });
  sdk_.SetStatsHandler([this](const LiveStats& s) {
    OnUi([this, s] { OnStats(s); });
  });

  // Subscription balance / plan store (Api::subscriptionBalance + polling).
  // Its timers live on this (UI) thread; its change handler already runs here.
  balance_.Initialize(uiThread_);
  balance_.SetChangeHandler([this](const BalanceSnapshot& snapshot,
                                   const BalancePollState& poll) {
    if (windowVisible_ && window_) {
      if (auto self = window_.try_as<winrt::URnetwork::implementation::MainWindow>())
        self->OnBalanceChanged(snapshot, poll);
    }
  });

  // Debounced placement save. Restarted on every move/resize the user makes,
  // so it lands once they settle rather than once per drag frame.
  if (uiThread_) {
    placementSaveTimer_ = uiThread_.CreateTimer();
    placementSaveTimer_.Interval(std::chrono::milliseconds(700));
    placementSaveTimer_.IsRepeating(false);
    placementSaveTimer_.Tick([this](auto const&, auto const&) {
      if (shell::SaveWindowPlacement(windowHwnd_)) ownPlacement_ = true;
    });
  }

  // The update checker (beta spec §5): its worker owns the launch-delay check
  // and the 6h cadence; a successful swap comes back through this handler.
  // Marshalled onto the UI thread because the handoff reuses the tray-quit
  // teardown, which is UI-thread machinery end to end.
  updates_.SetRelaunchHandler([this](std::filesystem::path exe) {
    OnUi([this, exe = std::move(exe)] { RelaunchOnto(exe); });
  });
  updates_.Start();

  LogInfo("app: initializing the sdk host");
  if (!sdk_.Initialize()) {
    // The tray icon is up by now, so from outside the app looks fine: an icon,
    // a menu, and nothing behind them — sign-in and connect would each fail
    // later, separately, with no explanation. Say it once, here, where the
    // cause is still known.
    LogError("app: sdk host initialization FAILED");
    FailVisible(
        L"URnetwork started, but its network SDK could not be initialized.\n\n"
        L"Signing in and connecting will not work. The app is in the "
        L"notification area — quit it from there.",
        L"SdkHost::Initialize returned false; the SDK's own error is in the log.");
  }
  UpdateTray();
  LogInfo("app: started");
}

void AppController::Shutdown() {
  LogInfo("app: shutdown requested (tray quit)");
  // First, and joined: the checker's worker is the one thread here that does
  // long blocking I/O (a zip download), and it polls its stop flag between
  // reads, so this is bounded — see UpdateChecker::Stop.
  updates_.Stop();
  // ...and quitting is the other
  if (shell::SaveWindowPlacement(windowHwnd_)) ownPlacement_ = true;
  quitting_ = true;  // let the window's Closing handler close instead of hiding
  tray_.Destroy();
  if (window_) window_.Close();
  if (auto app = Application::Current()) app.Exit();
}

// The relaunch half of a swapped update. Order is load-bearing:
//
//   1. UnregisterKey FIRST, so the key is free before the new process exists.
//      Without this the new launch finds this (dying) instance holding the
//      key and redirects its activation into a teardown.
//   2. Spawn the new exe with --relaunched: its bounded key retry (main.cpp)
//      covers the case where the unregister failed or this process is slow to
//      die — tolerance, not the mechanism.
//   3. Quit through the ordinary tray-quit path, so placement is saved and
//      the SDK host tears down exactly as it does every day.
//
// A spawn failure does NOT quit: the swap is already complete on disk, so the
// worst outcome of staying alive is running the old image until the user
// restarts by hand — strictly better than exiting to nothing.
void AppController::RelaunchOnto(std::filesystem::path const& exe) {
  LogInfo("update: relaunching onto {}", Narrow(exe.wstring()));
  try {
    winrt::Microsoft::Windows::AppLifecycle::AppInstance::GetCurrent().UnregisterKey();
  } catch (winrt::hresult_error const& e) {
    LogWarn("update: UnregisterKey failed ({}); the new instance will retry",
            Narrow(std::wstring{e.message()}));
  }
  std::wstring cmd = L"\"" + exe.wstring() + L"\" --relaunched";
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  if (::CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0,
                       nullptr, nullptr, &si, &pi)) {
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    Shutdown();
  } else {
    LogError("update: relaunch CreateProcess failed: {} — the update takes "
             "effect on the next manual start",
             ::GetLastError());
  }
}

void AppController::OnAuthState(AuthState state, const std::string& error) {
  const bool wasLoggedIn = (authState_ == AuthState::LoggedIn);
  authState_ = state;
  authError_ = error;
  UpdateTray();
  // balance store lifecycle follows the session. A repeated LoggedIn push is a
  // device re-registration (guest upgrade): restart the store so the plan
  // reseeds from the new jwt's claims (Guest -> Free).
  if (state == AuthState::LoggedIn) {
    balance_.Start();
  } else if (state == AuthState::LoggedOut && wasLoggedIn) {
    balance_.Stop();
  }
  // the tray always reflects state; only push into the window when it is
  // actually visible (resynced on show) so a hidden window doesn't churn.
  if (windowVisible_ && window_) {
    if (auto self = window_.try_as<winrt::URnetwork::implementation::MainWindow>())
      self->OnAuthStateChanged(state, error);
  }
  if (state == AuthState::LoggedIn) {
    tray_.ShowBalloon(Localized("app_name"), Localized("signed_in"));
  }
}

void AppController::OnTunnelState(const proto::TunnelStatus& status) {
  connected_ = (status.state == proto::TunnelState::Up);
  lastTunnelStatus_ = status;
  UpdateTray();
  if (windowVisible_ && window_) {
    if (auto self = window_.try_as<winrt::URnetwork::implementation::MainWindow>())
      self->OnTunnelStateChanged(status);
  }
}

void AppController::OnStats(const LiveStats& stats) {
  // Live stats only matter to the window (no tray surface); push only when visible.
  if (windowVisible_ && window_) {
    if (auto self = window_.try_as<winrt::URnetwork::implementation::MainWindow>())
      self->OnStatsChanged(stats);
  }
}

void AppController::UpdateTray() {
  // v1 maps provide=false; connect reflects the tunnel/connection state.
  tray_.SetState(connected_ ? TrayState::NoProvideConnect
                            : TrayState::NoProvideNoConnect);
  // "URnetwork — Connected": the app name, then the status. Both come from the
  // store; the em dash is punctuation, not text.
  std::wstring tip = Localized("app_name");
  if (connected_) tip += L" — " + Localized("connected");
  tray_.SetTooltip(tip);
}

void AppController::ShowWindow(const POINT* anchor) {
  // Every caller of this arrives from the tray icon's window procedure, and a
  // C++ exception must not unwind out of one (TrayIcon::WndProc catches as the
  // last resort, but by then the message is generic). Everything this path does
  // — creating the window, asking it for its HWND, wiring its events, moving it,
  // and starting the presentation controllers in ReconcileWindowPresentation —
  // is a WinRT call that can throw.
  try {
    ShowWindowImpl(anchor);
  } catch (winrt::hresult_error const& e) {
    LogError("app: showing the window failed: {}", Narrow(std::wstring{e.message()}));
    FailVisible(L"URnetwork's window could not be opened.\n\n"
                L"The app is still running in the notification area.",
                std::wstring{e.message()});
  } catch (const std::exception& e) {
    LogError("app: showing the window failed: {}", e.what());
    FailVisible(L"URnetwork's window could not be opened.\n\n"
                L"The app is still running in the notification area.",
                Widen(e.what()));
  }
}

void AppController::ShowWindowImpl(const POINT* anchor) {
  if (!window_) {
    // First open. This is where XAML actually parses MainWindow.xaml and the
    // resource lookups run, so it is the second place the app can fail with
    // nothing on screen (the first is the tray icon): a click that produces no
    // window and no message would send the owner back to guessing.
    LogInfo("app: creating the main window");
    window_ = winrt::make<winrt::URnetwork::implementation::MainWindow>();
    // Closing the window hides to tray (the tunnel keeps running); the tray
    // "Quit" is the only real exit (macOS parity). Wired once, on creation.
    if (auto native = window_.try_as<::IWindowNative>()) {
      HWND hwnd = nullptr;
      native->get_WindowHandle(&hwnd);
      windowHwnd_ = hwnd;
      LogInfo("app: main window created (hwnd {})", reinterpret_cast<uintptr_t>(hwnd));
      // The native shell: Mica (or the solid fallback), brand caption buttons,
      // the compact default size and the saved placement. Before this the window
      // was never sized at all and opened filling the whole work area.
      //
      // The return value matters: if it restored a position the user chose, the
      // anchor block below must not overwrite it one statement later.
      applyingPlacement_ = true;
      ownPlacement_ = shell::ApplyNativeShell(window_, hwnd);
      applyingPlacement_ = false;
      auto windowId = winrt::Microsoft::UI::GetWindowIdFromWindow(hwnd);
      auto appWindow = winrt::Microsoft::UI::Windowing::AppWindow::GetFromWindowId(windowId);
      appWindow.Closing(
          [this](winrt::Microsoft::UI::Windowing::AppWindow const&,
                 winrt::Microsoft::UI::Windowing::AppWindowClosingEventArgs const& args) {
            if (quitting_) return;  // real quit from the tray — allow the close
            args.Cancel(true);      // otherwise closing hides to tray
            HideWindow();
          });
      // The user moving or resizing the window is what makes the placement
      // THEIRS - and that happens long before the first save. Without this the
      // anchor kept overriding a drag on a first run, which is the state every
      // new user is in.
      appWindow.Changed(
          [this](winrt::Microsoft::UI::Windowing::AppWindow const&,
                 winrt::Microsoft::UI::Windowing::AppWindowChangedEventArgs const& args) {
            if (!args.DidPositionChange() && !args.DidSizeChange()) return;
            OnWindowPlacementChanged();
          });
      // whatever ApplyNativeShell just did was OURS, not the user's
      NoteAppliedPlacement();
    }
    // --preview-ui: the signed-in shell with no session, so the screens behind
    // sign-in can be looked at before they are called done (Startup.h).
    if (const std::string preview = PreviewUiDestination(); !preview.empty()) {
      if (auto self = window_.try_as<winrt::URnetwork::implementation::MainWindow>())
        self->EnterPreviewUi(preview);
    }
    window_.Activated([this](auto const&, auto const& args) {
      windowActivated_ =
          args.WindowActivationState() != WindowActivationState::Deactivated;
      ReconcileWindowPresentation();
    });
  }

  // Position near the tray anchor for the flyout-style left-click - but ONLY
  // while the user has not placed the window themselves.
  //
  // This app is not a volume-flyout: it has a title bar, caption buttons and a
  // resize border, so moving it is an expression of intent. Re-anchoring on
  // every tray click threw that away, and threw away the position half of
  // placement persistence with it: the shell would restore (300,200), log that
  // it had, and this block would move the window to the tray corner in the same
  // call. The anchor is the DEFAULT placement, not an override.
  if (anchor && !ownPlacement_) {
    if (auto native = window_.try_as<::IWindowNative>()) {
      HWND hwnd = nullptr;
      native->get_WindowHandle(&hwnd);
      auto windowId = winrt::Microsoft::UI::GetWindowIdFromWindow(hwnd);
      auto appWindow = winrt::Microsoft::UI::Windowing::AppWindow::GetFromWindowId(windowId);
      auto size = appWindow.Size();
      // place the window's bottom-right near the tray icon (bottom-right corner)
      int x = anchor->x - size.Width;
      int y = anchor->y - size.Height;

      // ...then clamp into the WORK AREA of the monitor the anchor is on, or
      // the window lands where nobody can see it. Unclamped, this put the
      // window at (-1136,-875) on a two-monitor desktop -- entirely off both
      // screens, with the tray still saying the app was open. Every anchor
      // near a top or left edge does it: the taskbar is not always bottom
      // right (it can be moved, and the icon may live in the overflow flyout),
      // a secondary monitor can sit at negative coordinates, and the window
      // can be taller than the screen it is anchored on. MONITOR_DEFAULTTONEAREST
      // keeps a bogus anchor on a real monitor rather than failing.
      POINT anchorPt{anchor->x, anchor->y};
      HMONITOR mon = ::MonitorFromPoint(anchorPt, MONITOR_DEFAULTTONEAREST);
      MONITORINFO mi{};
      mi.cbSize = sizeof(mi);
      if (mon && ::GetMonitorInfoW(mon, &mi)) {
        // RECT is LONG and AppWindow's size is int32_t; std::min/max deduce a
        // single T and will not mix the two. Narrow the work area to int once,
        // here, instead of casting at each call.
        const int workLeft = static_cast<int>(mi.rcWork.left);
        const int workTop = static_cast<int>(mi.rcWork.top);
        const int workRight = static_cast<int>(mi.rcWork.right);
        const int workBottom = static_cast<int>(mi.rcWork.bottom);
        // max() after min() so a window larger than the work area still has
        // its TOP-LEFT on screen (the title bar and close button) rather than
        // its bottom-right
        x = (std::max)(workLeft, (std::min)(x, workRight - size.Width));
        y = (std::max)(workTop, (std::min)(y, workBottom - size.Height));
      }

      winrt::Windows::Graphics::PointInt32 pos{x, y};
      LogInfo("app: window anchor ({},{}) size {}x{} -> position ({},{})",
              anchor->x, anchor->y, size.Width, size.Height, pos.X, pos.Y);
      // ours, not the user's: Changed is raised from inside Move()
      applyingPlacement_ = true;
      appWindow.Move(pos);
      applyingPlacement_ = false;
      NoteAppliedPlacement();
    }
  }
  // What the window actually did, as opposed to what any one step intended.
  // The shell's "restored placement" line was true when it was written and
  // false a moment later, which is the failure mode this project keeps paying
  // for: a mechanism whose signal describes an intermediate state.
  if (windowHwnd_) {
    RECT rc{};
    if (::GetWindowRect(windowHwnd_, &rc)) {
      LogInfo("app: window shown at ({},{}) {}x{} - {}", rc.left, rc.top,
              rc.right - rc.left, rc.bottom - rc.top,
              ownPlacement_ ? "the placement the user chose"
                            : (anchor ? "tray anchor" : "default placement"));
    }
  }
  windowShown_ = true;
  windowActivated_ = true;
  window_.Activate();
  ReconcileWindowPresentation();
}

void AppController::NoteAppliedPlacement() {
  if (!windowHwnd_) return;
  ::GetWindowRect(windowHwnd_, &lastAppliedRect_);
}

// The window moved or resized. AppWindow.Changed cannot say WHO did it, and our
// own moves raise it too, so the test is by result: if the rect is the one we
// last applied, it was us.
//
// This is the half of placement persistence that was missing. ownPlacement_ was
// only ever set by a restore or a save, so on a FIRST run - no saved placement
// yet - a user could drag the window somewhere, click the tray icon again, and
// be yanked straight back to the anchor. That is the exact state every new user
// is in, and it is the defect this whole mechanism exists to prevent.
void AppController::OnWindowPlacementChanged() {
  if (applyingPlacement_) return;  // a move we made
  if (!windowHwnd_) return;
  RECT rc{};
  if (!::GetWindowRect(windowHwnd_, &rc)) return;
  if (rc.left == lastAppliedRect_.left && rc.top == lastAppliedRect_.top &&
      rc.right == lastAppliedRect_.right && rc.bottom == lastAppliedRect_.bottom) {
    return;  // our own move
  }
  if (::IsIconic(windowHwnd_)) return;  // minimize is not a placement

  if (!ownPlacement_) {
    LogInfo("app: the window was moved or resized - its placement is the user's now");
  }
  ownPlacement_ = true;
  // ...and persist it shortly, so a process that is killed rather than closed
  // does not silently lose a placement it had already started honouring.
  if (placementSaveTimer_) {
    placementSaveTimer_.Stop();
    placementSaveTimer_.Start();
  }
}

void AppController::HideWindow() {
  // Hiding to the tray is one of the two ways this window goes away; record
  // where the user left it before it does. A successful save means there is now
  // a placement worth honouring, so the anchor stops moving the window on the
  // next tray click - within this session as well as across restarts.
  if (shell::SaveWindowPlacement(windowHwnd_)) ownPlacement_ = true;
  windowShown_ = false;
  windowActivated_ = false;
  ReconcileWindowPresentation();
  if (window_) window_.try_as<Window>().AppWindow().Hide();
}

void AppController::ReconcileWindowPresentation() {
  const bool active =
      WindowPresentationShouldRun(windowShown_, windowActivated_);
  if (windowVisible_ == active) return;
  windowVisible_ = active;

  sdk_.SetPresentationActive(active);
  balance_.SetVisible(active);
  if (!window_) return;
  if (auto self = window_.try_as<winrt::URnetwork::implementation::MainWindow>()) {
    self->SetPresentationActive(active);
    if (!active) return;

    // Re-apply the current state after the hidden/inactive interval.
    self->OnAuthStateChanged(authState_, authError_);
    if (lastTunnelStatus_) self->OnTunnelStateChanged(*lastTunnelStatus_);
    self->OnStatsChanged(sdk_.CurrentStats());
    self->OnBalanceChanged(balance_.Current(), balance_.CurrentPoll());
    if (authState_ == AuthState::LoggedIn) balance_.Refresh();
  }
}

// ---- urnetwork:// protocol activation --------------------------------------

void AppController::HandleDeepLink(const std::string& url) {
  // never log the uri itself: a wallet callback carries the address + signature
  LogInfo("app: deep link received");
  // the callback comes back through the browser, so the app is in the background
  // (and may be hidden to the tray) — bring the window forward for the result
  ShowWindow(nullptr);
  sdk_.HandleDeepLink(url);
}

std::string DeepLinkFromActivation(
    winrt::Microsoft::Windows::AppLifecycle::AppActivationArguments const& args) {
  namespace lifecycle = winrt::Microsoft::Windows::AppLifecycle;
  namespace activation = winrt::Windows::ApplicationModel::Activation;
  if (!args) return {};

  // Typed: the scheme was registered through ActivationRegistrationManager.
  if (args.Kind() == lifecycle::ExtendedActivationKind::Protocol) {
    if (auto protocolArgs = args.Data().try_as<activation::IProtocolActivatedEventArgs>()) {
      return Narrow(protocolArgs.Uri().RawUri());
    }
    return {};
  }
  // Plain: the MSI registers `"URnetwork.exe" "%1"`, so the uri arrives as a
  // launch argument. For a redirected activation this command line — the one the
  // other instance was launched with — is all we get, so scan it for the scheme.
  if (args.Kind() == lifecycle::ExtendedActivationKind::Launch) {
    if (auto launchArgs = args.Data().try_as<activation::ILaunchActivatedEventArgs>()) {
      return Narrow(DeepLinkFromCommandLine(launchArgs.Arguments()));
    }
  }
  return {};
}

std::string LaunchDeepLink() { return Narrow(DeepLinkFromCommandLine(::GetCommandLineW())); }

}  // namespace urnw
