// SPDX-License-Identifier: MPL-2.0
#include "pch.h"

#include "AppController.h"

#include <string_view>

#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Microsoft.UI.Interop.h>
#include <winrt/Windows.ApplicationModel.Activation.h>

#include "Ids.h"
#include "Localization.h"
#include "Log.h"
#include "MainWindow.xaml.h"
#include "Strings.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;

namespace urnw {
namespace {
std::unique_ptr<AppController> g_app;

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

AppController::~AppController() = default;

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
  tray_.Create(::GetModuleHandleW(nullptr), std::move(cb));

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
  balance_.SetVisibilityGate([this] { return windowVisible_; });
  balance_.SetChangeHandler([this](const BalanceSnapshot& snapshot,
                                   const BalancePollState& poll) {
    if (windowVisible_ && window_) {
      if (auto self = window_.try_as<winrt::URnetwork::implementation::MainWindow>())
        self->OnBalanceChanged(snapshot, poll);
    }
  });

  sdk_.Initialize();
  UpdateTray();
  LogInfo("app: started");
}

void AppController::Shutdown() {
  quitting_ = true;  // let the window's Closing handler close instead of hiding
  tray_.Destroy();
  if (window_) window_.Close();
  if (auto app = Application::Current()) app.Exit();
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
  if (!window_) {
    window_ = winrt::make<winrt::URnetwork::implementation::MainWindow>();
    // Closing the window hides to tray (the tunnel keeps running); the tray
    // "Quit" is the only real exit (macOS parity). Wired once, on creation.
    if (auto native = window_.try_as<::IWindowNative>()) {
      HWND hwnd = nullptr;
      native->get_WindowHandle(&hwnd);
      auto windowId = winrt::Microsoft::UI::GetWindowIdFromWindow(hwnd);
      auto appWindow = winrt::Microsoft::UI::Windowing::AppWindow::GetFromWindowId(windowId);
      appWindow.Closing(
          [this](winrt::Microsoft::UI::Windowing::AppWindow const&,
                 winrt::Microsoft::UI::Windowing::AppWindowClosingEventArgs const& args) {
            if (quitting_) return;  // real quit from the tray — allow the close
            args.Cancel(true);      // otherwise closing hides to tray
            HideWindow();
          });
    }
  }
  // Position near the tray anchor for the flyout-style left-click; otherwise
  // let it appear at its default location.
  if (anchor) {
    if (auto native = window_.try_as<::IWindowNative>()) {
      HWND hwnd = nullptr;
      native->get_WindowHandle(&hwnd);
      auto windowId = winrt::Microsoft::UI::GetWindowIdFromWindow(hwnd);
      auto appWindow = winrt::Microsoft::UI::Windowing::AppWindow::GetFromWindowId(windowId);
      auto size = appWindow.Size();
      // place the window's bottom-right near the tray icon (bottom-right corner)
      winrt::Windows::Graphics::PointInt32 pos{anchor->x - size.Width,
                                               anchor->y - size.Height};
      appWindow.Move(pos);
    }
  }
  window_.Activate();
  windowVisible_ = true;
  // the window is created lazily and only updated while visible; re-apply the
  // current auth + tunnel state so it reflects anything that changed while hidden
  if (auto self = window_.try_as<winrt::URnetwork::implementation::MainWindow>()) {
    self->OnAuthStateChanged(authState_, authError_);
    if (lastTunnelStatus_) self->OnTunnelStateChanged(*lastTunnelStatus_);
    self->OnStatsChanged(sdk_.CurrentStats());  // resync live stats on show
    // resync the plan cards, then fetch fresh (the background poll was gated
    // off while the window was hidden)
    self->OnBalanceChanged(balance_.Current(), balance_.CurrentPoll());
    if (authState_ == AuthState::LoggedIn) balance_.Refresh();
  }
}

void AppController::HideWindow() {
  windowVisible_ = false;
  if (window_) window_.try_as<Window>().AppWindow().Hide();
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
