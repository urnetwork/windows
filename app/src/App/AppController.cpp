// SPDX-License-Identifier: MPL-2.0
#include "pch.h"

#include "AppController.h"

#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Microsoft.UI.Interop.h>

#include "Log.h"
#include "MainWindow.xaml.h"
#include "Strings.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;

namespace urnw {
namespace {
std::unique_ptr<AppController> g_app;
}

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
  sdk_.SetTunnelStateHandler([this](const proto::TunnelStatus& st) {
    OnUi([this, st] { OnTunnelState(st); });
  });
  sdk_.SetStatsHandler([this](const LiveStats& s) {
    OnUi([this, s] { OnStats(s); });
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
  authState_ = state;
  authError_ = error;
  UpdateTray();
  // the tray always reflects state; only push into the window when it is
  // actually visible (resynced on show) so a hidden window doesn't churn.
  if (windowVisible_ && window_) {
    if (auto self = window_.try_as<winrt::URnetwork::implementation::MainWindow>())
      self->OnAuthStateChanged(state, error);
  }
  if (state == AuthState::LoggedIn) tray_.ShowBalloon(L"URnetwork", L"Signed in");
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
  tray_.SetTooltip(connected_ ? L"URnetwork — Connected" : L"URnetwork");
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
  }
}

void AppController::HideWindow() {
  windowVisible_ = false;
  if (window_) window_.try_as<Window>().AppWindow().Hide();
}

}  // namespace urnw
