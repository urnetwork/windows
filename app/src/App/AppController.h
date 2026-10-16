// Owns the app's runtime: the SdkHost (DeviceRemote/session), the tray icon, and
// the main window. Subscribes to SDK auth/tunnel state and fans it out to the
// tray and window, marshaling onto the UI thread via the DispatcherQueue.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.Windows.AppLifecycle.h>

#include "AppLifetime.h"
#include "AuthLogoutNotice.h"
#include "BalanceGate.h"
#include "InstanceHandover.h"
#include "SdkHost.h"
#include "StatsReadOrder.h"
#include "SubscriptionBalance.h"
#include "TrayConnectionState.h"
#include "TrayIcon.h"
#include "UpdateChecker.h"
#include "UrIoApproval.h"

namespace winrt::URnetwork::implementation {
struct MainWindow;
}

namespace urnw {

class LoginPage;

class AppController {
 public:
  // Constructed on the UI thread (captures its DispatcherQueue).
  AppController();
  ~AppController();

  void Start();
  // End the app, once: the first call wins and every later one returns at
  // once. Every ending stops the app's own work and exits; the tray menu's
  // Quit also stops the tunnel and the provider in the service, after the
  // window and the tray are gone (AppLifetime.h, SdkHost::Quit). UI thread.
  void Shutdown(lifetime::Ending ending);

  SdkHost& sdk() { return sdk_; }
  // The subscription balance / plan store (fetch + polling; Phase 1 keystone).
  SubscriptionBalanceStore& balance() { return balance_; }
  // The update checker (beta spec §5). Owned here, not by the window: checks
  // run on launch and every 6 hours whether or not the tray was ever clicked,
  // and the window that renders the banner may not exist yet.
  UpdateChecker& updates() { return updates_; }

  // Route a urnetwork:// URI: the wallet-connect callback and the Google and
  // Apple returns into the SdkHost, the campaign emails' links into the window.
  // All but the auth host bring the app forward so the result is visible; the
  // auth host (whatever the case of its spelling) is judged first
  // (HandleAuthLink), and a link that answers no sign-in this app started raises
  // nothing, except for the first launch of a freshly started process. UI
  // thread only.
  void HandleDeepLink(const std::string& url);

  // Act on a launch, this instance's own or one redirected to it: route its
  // deep link, open the window for the user's launch, or leave an autostart in
  // the tray (instance::ActionFor). UI thread only.
  void ServeLaunch(const instance::LaunchRequest& request);

  // Show/position the main window; anchor != nullptr positions it near the tray
  // (left-click flyout behavior), otherwise it centers. Reached from the tray's
  // window procedure, so this is the catching wrapper around ShowWindowImpl —
  // an exception must not unwind out of a WndProc.
  void ShowWindow(const POINT* anchor = nullptr);
  void HideWindow();

  // The banner's Cancel (and every gesture that takes the connect into the
  // user's own hands): a connect waiting on the balance is not run by itself
  // any more (BalanceGate.h, BalanceRecovery). UI thread.
  void ClearBalanceRecovery();

  // What the sign-in page says about the sign-out the server made, once
  // (AuthLogoutNotice.h): the window takes it whenever it shows that page,
  // so a window opened after the sign-out still says it. UI thread.
  authlogout::Notice TakeSignedOutNotice() { return signedOutNotice_.Take(); }

  // ---- a sign-in approved on a web page (UrIoApproval.h) ---------------------
  // "Continue in browser" in the browser sign-in sheet. The attempt lives here, not
  // in the sheet: a hide to the tray closes the sheet and the browser may still
  // answer. UI thread only, all of it.

  // Whether the action is offered at all: the build flag (config::kBrowserApprovalEnabled,
  // off), a network the request can name (the official one, or another whose API url is
  // https://host[:port]: ApprovalNetwork.h), a process that is not elevated, an app
  // that is signed out, the approval page live in this run and listing the contract version
  // that network needs (ApprovalCheck.h: 1 for the official network, 2 for another), and no
  // other program holding the urnetwork:// scheme for this user (ProtocolHandlerStatus).
  // Anything else and the sheet is what it always was. Logs a closed word for why not.
  bool ApprovalOffered();
  // Starts an attempt, or, with one live, reads the same request back (a second click
  // re-opens it; it is not extended, and an attempt that another flow has superseded
  // since, or whose request the page would refuse as expired by now, is ended and
  // replaced, not read back). Before anything starts the user's urnetwork://
  // handler is made to start THIS exe and read back (EnsureProtocolHandler), and the
  // flows waiting for the browser or a wallet are superseded. In the official network
  // the request is version 1; in another it is version 2 and names the API this app
  // talks to, and the attempt belongs to that network (a network change ends it).
  // nullopt when the action is not offered or any of that fails, and the paste flow
  // stays. The View is a secret until the request it builds is opened.
  std::optional<approval::Slot::View> BeginApproval();
  // The request for a View: config::kApprovalPageUrl and the fragment the page expects
  // (approval::BuildRequestUrl). Empty if anything is off, and then nothing is opened.
  std::string ApprovalRequestUrl(const approval::Slot::View& view) const;
  // Ends the attempt, whatever its state, wiping its secrets, and stops its timer. The
  // sheet's cancel passes ClearReason::Cancel; the auth-state hooks, the timer, a quit
  // and the gate pass theirs.
  void CancelApproval(approval::ClearReason reason);
  // The window is between ShowWindow and HideWindow: false while it is hidden to the tray.
  bool WindowShown() const { return windowShown_; }
  // How many times the window has been hidden to the tray, counted by HideWindow before it
  // sweeps the sheets. A sheet takes it when it is built and asks again when it closes: a
  // hide keeps the attempt (the browser may still answer) and the person closing the sheet
  // cancels it, and the count says which, as soon as the hide happens. A dialog's Closed event
  // comes after its popup's closing transition, so what the window is doing by then is no
  // answer to what closed the sheet.
  std::uint64_t WindowHides() const { return windowHides_; }

 private:
  // The gate for urnetwork://auth: the parser, then the verdict on whether a
  // sign-in this app started is waiting for the link. UI thread only.
  void HandleAuthLink(const std::string& url);
  // What a link that answers no attempt earns: nothing, except that the first launch of
  // a freshly started process shows the window once with a neutral line.
  void ShowStrayNotice();
  approval::Context ApprovalContext();
  // ApprovalOffered for a network that has been read already: BeginApproval reads it once and
  // uses that one for the decision and for the request, so an attempt cannot be offered for one
  // network and name another.
  bool ApprovalOfferedIn(const approval::Network& network);
  // The five minutes are up.
  void OnApprovalTimer();
  // Begins the approval page's one check of this run where the action could be offered at all
  // (nothing, with the build flag off). Called wherever a person lands on a signed-out window:
  // when one is shown, and when the app signs out or switches network under a window that is
  // already on screen.
  void StartApprovalCheck();
  // The login page of the open window, or null while there is no window.
  LoginPage* LoginPageOrNull();
  void ShowWindowImpl(const POINT* anchor);
  // AppWindow.Changed relay: notices a move or resize the user made.
  void OnWindowPlacementChanged();
  // Record the rect we just applied ourselves, so the relay can ignore it.
  void NoteAppliedPlacement();
  // Re-read IsIconic into windowMinimized_. Called from Window.VisibilityChanged
  // (minimize and restore both raise it) and before the reconcile on show.
  void SyncWindowMinimized();
  void OnAuthState(AuthState state, const std::string& error);
  // The server rejected the stored sign-in (SdkHost's auth-invalid handler,
  // marshalled here with the cause its listener read): the app signs out,
  // once for the Api's and the device's reports of one rejection, and none
  // for a report behind a sign-out already done (AuthLogoutNotice.h).
  void OnAuthInvalid(const std::string& cause);
  void OnTunnelState(const proto::TunnelStatus& status);
  void OnStats(const LiveStats& stats);
  // The reaction to a stats or balance push in the insufficient-balance gate
  // (BalanceGate.h): a tray notice once per out-of-balance episode, and never
  // a disconnect.
  void ReactToBalance();
  // Feed the out-of-balance latch the last stats push and the current balance,
  // and read insufficientBalance_ back from it.
  void ObserveBalanceLatch();
  // The start-connect gate's inputs for this instant (BalanceGate.h): out of
  // balance, not Pro, and no confirmation poll bridging a purchase.
  bool OutOfBalance() const;
  // Everything a start connect decides on (BalanceGate.h, DecideStartConnect):
  // the gate above plus the subscription balance and when it was read, so the
  // first connect after a launch on an empty account is blocked too.
  urnw::balance::StartConnectFacts CurrentStartConnectFacts() const;
  // In place of a blocked connect: bring the window forward on its upgrade
  // path (the upgrade sheet, or guest conversion), the same one the in-app
  // banner's Get Pro opens.
  void ShowUpgradeForBlockedConnect();
  // A connect gesture the gate refused waits on the balance and runs again,
  // past the gate, once data is back (BalanceGate.h, BalanceRecovery).
  void WaitOnBalance(std::function<void()> refused);
  // Feed the balance recovery the gate, the connect request and the current
  // balance after a stats or balance push, and make the retry it decides on.
  void ObserveBalanceRecovery();
  // Push the recovery's state to the banner (visible window only; resynced on
  // show through OnStats).
  void PublishBalanceRecovery();
  void UpdateTray();
  // THE LAST STATUS THE SERVICE PUSHED, in the vocabulary of the shared
  // decision table (Common/ConnectAction.h). The tray reads this rather than
  // any app-side belief for the reason its recovery items exist at all: they
  // have to be right when the app's own view of the world is the thing that has
  // gone wrong. The string_views point into lastTunnelStatus_, which outlives
  // every caller here.
  gesture::ServiceFacts CurrentServiceFacts() const;
  // The aggregate to judge the button label by. Falls back to the tunnel's own
  // claim when no stats have ever flowed (the feed is presentation-scoped, so a
  // window that has never been shown has no evidence either way).
  health::State TrayHealth() const;
  // What the tray decides its status, icon and Connect/Disconnect item on
  // (TrayConnectionState.h): the sign-in, the service's last status and the
  // last stats push's health. The wfp state points into lastTunnelStatus_.
  tray::Facts CurrentTrayFacts() const;
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

  // The browser approval's one attempt, the neutral line a stray may earn, and the
  // one-shot timer that ends the attempt five minutes after it began (restarted by
  // BeginApproval, stopped by CancelApproval), so the secret is wiped and an open sheet
  // told even when nothing else happens.
  approval::Slot approval_;
  approval::NeutralLine neutralLine_;
  winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer approvalTimer_{nullptr};
  // ServeLaunch has served a launch. The first is this process's own (App::OnLaunched
  // serves it before the gate opens to the launches redirected to this instance), and
  // freshProcessLaunch_ is true from the moment ServeLaunch begins serving it until a stray
  // has used it or the next launch is served: the only launch whose stray auth link may show
  // the window.
  bool launchServed_ = false;
  bool freshProcessLaunch_ = false;

  AuthState authState_ = AuthState::LoggedOut;
  std::string authError_;
  // True while authError_ has not yet been shown in a PRESENTED window. The
  // window is told of a push only while it is presented, and replays the
  // standing state when it next is (ReconcileWindowPresentation) - but an error
  // that was already delivered must not come back on every later show: it would
  // put a sentence the user has moved past on whichever step they are on by
  // then (a sign-in failure inside the password step's bar), and force-scroll
  // the page to it.
  bool authErrorUndelivered_ = false;
  // the one sign-out a rejection takes, and the sign-in page's notice for it
  authlogout::SignedOutNotice signedOutNotice_;
  bool connected_ = false;
  // #27: the last aggregate connection health a stats push carried, so the
  // tray can say Evaluating/Degraded instead of a false Connected. nullopt is
  // "no evidence": stats only flow while the window presents, and with none
  // the tray falls back to the tunnel's own claim (exactly its old behaviour).
  // Held across a hide — stale under-claiming beats fresh over-claiming — and
  // cleared on any tunnel transition, because exit evidence is only valid
  // within the tunnel session that produced it (see OnTunnelState).
  std::optional<health::State> trayHealth_;
  // Set when the app ends (Shutdown: the tray "Quit", a close request, the
  // end of the Windows session), so the window's Closing handler lets it close instead
  // of hiding to tray (macOS parity: X/close hides, tray Quit exits). Atomic
  // since D3: OnUi reads it from SDK callback threads as the
  // "stop marshalling, the DispatcherQueue is tearing down" gate — a completion
  // that resumes on the queue after shutdown does not get to throw from inside
  // CoreMessaging, it simply is not queued.
  std::atomic<bool> quitting_{false};
  // Presentation controllers (the stats feed, chart ticks, canvas animation,
  // the balance poll) run whenever the window is actually on screen. Focus is
  // deliberately NOT part of that: the owner watches the graphs while another
  // app is foreground, and gating on activation reset all of it on every click
  // away. Only the states nobody can see tear it down — minimized, or hidden
  // to the tray — where the CPU save is real and the rebuild-on-return is fine.
  // The one exception is the purchase-confirmation poll, which also pauses on
  // focus loss (Window.Activated -> SubscriptionBalanceStore::SetFocused).
  bool windowShown_ = false;      // between ShowWindow and HideWindow (tray-level intent)
  std::uint64_t windowHides_ = 0;  // how many HideWindow calls there have been (WindowHides)
  bool windowMinimized_ = false;  // IsIconic, synced by SyncWindowMinimized
  bool windowVisible_ = false;    // the reconciled result: the presentation is running
  // which stats snapshots OnStats still applies: the newest read wins
  urnw::StatsReadOrder statsReadOrder_;
  std::optional<proto::TunnelStatus> lastTunnelStatus_;
  // the out-of-balance state, latched (OutOfBalanceLatch) because the contract
  // status is reset with the destination and the raw push alone forgets it on
  // Disconnect; the last raw push; and the once-per-episode notice
  bool insufficientBalance_ = false;
  bool rawInsufficientBalance_ = false;
  bool providersConnected_ = false;  // CONNECTED with providers in the window
  urnw::balance::OutOfBalanceLatch balanceLatch_;
  urnw::balance::GateNoticeTracker balanceNotice_;
  // a connect the balance blocked, retried by itself once data is back: the
  // refused gesture, or the connection held out of balance (a destination is
  // set: LiveStats.connected from the last push)
  urnw::balance::BalanceRecovery<std::function<void()>> balanceRecovery_;
  bool connectRequested_ = false;
};

// The single app controller instance (created in App::OnLaunched).
AppController& App();
void SetApp(std::unique_ptr<AppController> app);

// ---- urnetwork:// protocol activation --------------------------------------
// The MSI registers the scheme (installer/Package.wxs) as
// `"URnetwork.exe" "%1"`, so the shell hands the callback uri to the app as a
// launch argument. A portable copy or a development build has no such key; a
// browser sign-in that is about to start writes the same command for the current
// user (ProtocolHandler.h, HKEY_CURRENT_USER only) and refreshes it at each launch.
// Launches while the app is already running are redirected to
// it by AppInstance (see main.cpp), arrive on AppInstance::Activated and go
// through the activation gate (SingleInstance.h), which App::OnLaunched opens.
// Each is served as its LaunchRequest says (AppController::ServeLaunch).

// The urnetwork:// uri carried by an activation, or empty when it carries none.
// Handles both shapes: a typed Protocol activation (if the scheme is ever
// registered through ActivationRegistrationManager) and the plain Launch
// activation the MSI registration produces.
std::string DeepLinkFromActivation(
    winrt::Microsoft::Windows::AppLifecycle::AppActivationArguments const& args);

// The urnetwork:// uri this process was launched with, or empty. Cold-launch
// fallback for the Launch case, read straight from our own command line.
std::string LaunchDeepLink();

// What a launch redirected to this instance asks: its deep link, and whether
// its command line (the one the other process was started with) carries
// instance::kAutostartArgument.
instance::LaunchRequest LaunchRequestFromActivation(
    winrt::Microsoft::Windows::AppLifecycle::AppActivationArguments const& args);

// What this instance's own launch asks: its activation's deep link, or one on
// its command line, and whether an autostart started it.
instance::LaunchRequest OwnLaunchRequest();

}  // namespace urnw
