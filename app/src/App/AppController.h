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
#include <winrt/Microsoft.Windows.AppLifecycle.h>

#include "SdkHost.h"
#include "SubscriptionBalance.h"
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
  // The subscription balance / plan store (fetch + polling; Phase 1 keystone).
  SubscriptionBalanceStore& balance() { return balance_; }

  // Route a urnetwork:// URI (the wallet-connect callback) into the SdkHost and
  // bring the app forward so the sign-in result is visible. UI thread only.
  void HandleDeepLink(const std::string& url);

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
  SubscriptionBalanceStore balance_{sdk_};
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

// ---- urnetwork:// protocol activation --------------------------------------
// The MSI registers the scheme (installer/Package.wxs) as
// `"URnetwork.exe" "%1"`, so the shell hands the callback uri to the app as a
// launch argument. Launches while the app is already running are redirected to
// it by AppInstance (see main.cpp) and arrive on AppInstance::Activated.

// The urnetwork:// uri carried by an activation, or empty when it carries none.
// Handles both shapes: a typed Protocol activation (if the scheme is ever
// registered through ActivationRegistrationManager) and the plain Launch
// activation the MSI registration produces.
std::string DeepLinkFromActivation(
    winrt::Microsoft::Windows::AppLifecycle::AppActivationArguments const& args);

// The urnetwork:// uri this process was launched with, or empty. Cold-launch
// fallback for the Launch case, read straight from our own command line.
std::string LaunchDeepLink();

}  // namespace urnw
