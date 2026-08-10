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
#include "UpdateChecker.h"

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
  // The update checker (beta spec §5). Owned here, not by the window: checks
  // run on launch and every 6 hours whether or not the tray was ever clicked,
  // and the window that renders the banner may not exist yet.
  UpdateChecker& updates() { return updates_; }

  // Route a urnetwork:// URI (the wallet-connect callback) into the SdkHost and
  // bring the app forward so the sign-in result is visible. UI thread only.
  void HandleDeepLink(const std::string& url);

  // Show/position the main window; anchor != nullptr positions it near the tray
  // (left-click flyout behavior), otherwise it centers. Reached from the tray's
  // window procedure, so this is the catching wrapper around ShowWindowImpl —
  // an exception must not unwind out of a WndProc.
  void ShowWindow(const POINT* anchor = nullptr);
  void HideWindow();

 private:
  void ShowWindowImpl(const POINT* anchor);
  // A fully swapped update wants this instance replaced: release the
  // single-instance key, start the NEW exe (now sitting at our own path) with
  // the handoff flag, and quit. UI thread only — it reuses the tray-quit path.
  void RelaunchOnto(std::filesystem::path const& exe);
  // AppWindow.Changed relay: notices a move or resize the user made.
  void OnWindowPlacementChanged();
  // Record the rect we just applied ourselves, so the relay can ignore it.
  void NoteAppliedPlacement();
  void OnAuthState(AuthState state, const std::string& error);
  void OnTunnelState(const proto::TunnelStatus& status);
  void OnStats(const LiveStats& stats);
  void UpdateTray();
  void ReconcileWindowPresentation();
  template <class F>
  void OnUi(F&& f);  // marshal onto the UI thread

  SdkHost sdk_;
  SubscriptionBalanceStore balance_{sdk_};
  UpdateChecker updates_;
  TrayIcon tray_;
  winrt::Microsoft::UI::Dispatching::DispatcherQueue uiThread_{nullptr};
  winrt::Microsoft::UI::Xaml::Window window_{nullptr};
  // the window's HWND, held so the native shell can read and save its
  // placement without re-deriving it from the projection each time
  HWND windowHwnd_ = nullptr;
  // The window has a placement the USER chose - restored from a previous run,
  // saved during this one, or observed being dragged/resized right now. While
  // false the tray anchor places the window (the flyout-by-the-icon default);
  // once true the anchor stops overriding a position the user picked. See
  // ShowWindowImpl and OnWindowPlacementChanged.
  bool ownPlacement_ = false;
  // AppWindow.Changed cannot say WHO moved the window, and our own moves raise
  // it too, so a programmatic move is told from a user one two ways:
  //   applyingPlacement_  set around our own moves. AppWindow.Changed is raised
  //                       SYNCHRONOUSLY from inside Move(), so this catches the
  //                       normal case - without it the tray anchor's own move
  //                       marked itself as "the user's placement" and the
  //                       anchor then never applied again.
  //   lastAppliedRect_    the backstop for anything raised later, out from
  //                       under the guard: the rect we last applied ourselves.
  bool applyingPlacement_ = false;
  RECT lastAppliedRect_{};
  // Saving on every frame of a drag would be a registry write per pixel; saving
  // only on hide/quit loses a first-ever placement if the process is killed.
  // This fires shortly after the user stops moving.
  winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer placementSaveTimer_{nullptr};

  AuthState authState_ = AuthState::LoggedOut;
  std::string authError_;
  bool connected_ = false;
  // #27: the last aggregate connection health a stats push carried, so the
  // tray can say Evaluating/Degraded instead of a false Connected. nullopt is
  // "no evidence": stats only flow while the window presents, and with none
  // the tray falls back to the tunnel's own claim (exactly its old behaviour).
  // Held across a hide — stale under-claiming beats fresh over-claiming — and
  // cleared on any tunnel transition, because exit evidence is only valid
  // within the tunnel session that produced it (see OnTunnelState).
  std::optional<health::State> trayHealth_;
  // set when the tray "Quit" is chosen, so the window's Closing handler lets it
  // close instead of hiding to tray (macOS parity: X/close hides, tray Quit exits)
  bool quitting_ = false;
  // Presentation controllers run only while the tray window is both shown and
  // active. Minimize/app deactivation suspends them just like an explicit hide.
  bool windowShown_ = false;
  bool windowActivated_ = false;
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
