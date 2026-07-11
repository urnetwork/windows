// Owns the app's runtime: the SdkHost (DeviceRemote/session), the tray icon, and
// the main window. Subscribes to SDK auth/tunnel state and fans it out to the
// tray and window, marshaling onto the UI thread via the DispatcherQueue.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <memory>
#include <optional>
#include <string>

#include <winrt/Microsoft.UI.Dispatching.h>

#include "SdkHost.h"
#include "TrayIcon.h"

namespace winrt::URnetwork::implementation {
struct MainWindow;
}

namespace urnw {

class AppController {
 public:
  // Constructed on the UI thread (captures its DispatcherQueue).
  AppController();
  ~AppController();

  void Start();
  void Shutdown();

  SdkHost& sdk() { return sdk_; }

  // Show/position the main window; anchor != nullptr positions it near the tray
  // (left-click flyout behavior), otherwise it centers.
  void ShowWindow(const POINT* anchor = nullptr);
  void HideWindow();

 private:
  void OnAuthState(AuthState state, const std::string& error);
  void OnTunnelState(const proto::TunnelStatus& status);
  void OnStats(const LiveStats& stats);
  void UpdateTray();
  template <class F>
  void OnUi(F&& f);  // marshal onto the UI thread

  SdkHost sdk_;
  TrayIcon tray_;
  winrt::Microsoft::UI::Dispatching::DispatcherQueue uiThread_{nullptr};
  winrt::Microsoft::UI::Xaml::Window window_{nullptr};

  AuthState authState_ = AuthState::LoggedOut;
  std::string authError_;
  bool connected_ = false;
  // set when the tray "Quit" is chosen, so the window's Closing handler lets it
  // close instead of hiding to tray (macOS parity: X/close hides, tray Quit exits)
  bool quitting_ = false;
  // tray app: skip pushing state into the window while it is hidden (resynced on
  // show) so a hidden window doesn't churn on high-frequency SDK updates
  bool windowVisible_ = false;
  std::optional<proto::TunnelStatus> lastTunnelStatus_;
};

// The single app controller instance (created in App::OnLaunched).
AppController& App();
void SetApp(std::unique_ptr<AppController> app);

}  // namespace urnw
