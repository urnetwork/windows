// SPDX-License-Identifier: MPL-2.0
#include "pch.h"

#include "AppController.h"

#include <algorithm>
#include <bcrypt.h>
#include <chrono>
#include <exception>
#include <string_view>
#include <vector>

#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Microsoft.UI.Interop.h>
#include <winrt/Windows.ApplicationModel.Activation.h>

#include "ApprovalCheck.h"
#include "ApprovalUnseal.h"
#include "AuthLink.h"
#include "Config.h"
#include "Ids.h"
#include "LaunchAtStartup.h"
#include "Localization.h"
#include "Log.h"
#include "MainWindow.xaml.h"
#include "PageContext.h"  // pages::AdvW — the tray tooltip's health words (#27)
#include "ProtocolHandler.h"
#include "SingleInstance.h"
#include "Startup.h"
#include "Strings.h"
#include "Top200Binding.h"
#include "WindowShell.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;

namespace urnw {
namespace {
std::unique_ptr<AppController> g_app;

// The controller, and the SdkHost in it, outlive a thread still running inside
// them at exit: released, never destroyed, so the process exit ends that
// thread rather than a destructor freeing what it uses
// (exitdrain::HostDestroyableAtExit).
void LeaveAppToProcessExit() {
  LogWarn("app: a session pass or the quit is still running; the app is left to the "
          "process exit");
  (void)g_app.release();
}

// The presentation gate. `shown` is the tray-level intent (between ShowWindow
// and HideWindow); `minimized` is the taskbar state. Focus is deliberately
// absent: this used to be `shown && activated`, and stopping on mere
// deactivation meant clicking into any other app froze the graphs and reset
// them when the window was clicked back (the feeds rebuild from scratch on
// resume). The owner wants the window watchable while something else has
// focus; minimize and hide-to-tray still stop everything, which keeps the CPU
// save exactly where nobody is looking.
constexpr bool WindowPresentationShouldRun(bool shown, bool minimized) {
  return shown && !minimized;
}

static_assert(WindowPresentationShouldRun(true, /*minimized=*/false));
static_assert(!WindowPresentationShouldRun(true, /*minimized=*/true));
static_assert(!WindowPresentationShouldRun(false, /*minimized=*/false));
static_assert(!WindowPresentationShouldRun(false, /*minimized=*/true));

// Pull a urnetwork:// uri out of a command line. The shell appends it as an
// argument (possibly quoted); the uri itself never contains whitespace because
// the wallet-connect bridge percent-encodes the query. The scheme is matched
// without regard to case, as the router matches the auth host, so a link spelled
// URNETWORK://... reaches the gate and is judged there. What it returns can
// carry a credential (a sign-in return), so it never reaches a log or the UI.
std::wstring DeepLinkFromCommandLine(std::wstring_view commandLine) {
  return std::wstring(authlink::LinkInCommandLine(commandLine));
}

// Why the gate decided what it did, for the log: a closed token. A link that was not
// even well formed says so with the parser's own reason, which is the more useful
// word when the gate has only "nothing is waiting" or "this is no answer" to add;
// anything else is the gate's.
std::string_view AuthLinkWhy(const authlink::Return& link, const approval::Decision& decision) {
  const bool parserKnowsBetter = decision.why == approval::Why::NoAttempt || decision.why == approval::Why::InvalidShape;
  if (link.kind == authlink::Kind::Invalid && parserKnowsBetter) return authlink::ToString(link.reject);
  return approval::ToString(decision.why);
}

// The state, from the OS: its entropy is the only thing that makes a link worth acting on. (The
// key pair is the sdk's, drawn by approval::NewKeyPair.)
bool RandomBytes(unsigned char* out, std::size_t length) {
  return ::BCryptGenRandom(nullptr, out, static_cast<ULONG>(length), BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0;
}

}  // namespace

AppController& App() { return *g_app; }
void SetApp(std::unique_ptr<AppController> app) { g_app = std::move(app); }

AppController::AppController() {
  uiThread_ = winrt::Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();
}

AppController::~AppController() {
  if (placementSaveTimer_) placementSaveTimer_.Stop();
  if (approvalTimer_) approvalTimer_.Stop();
}

template <class F>
void AppController::OnUi(F&& f) {
  // D3: nothing is marshalled onto a queue that is tearing down. Every SDK
  // callback funnels through here, and a lambda that lands after Shutdown()
  // would run against a closed window during the DispatcherQueue's drain —
  // the class of late completion whose unobserved failure was the tray-quit
  // 0xc000027b. Dropping it is correct by definition: the process is exiting
  // and there is no UI left for the work to mean anything to.
  if (quitting_.load(std::memory_order_acquire)) return;
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
  // ONE RULE, SHARED WITH THE WINDOW. The tray used to test `state == Up` while
  // the connect page tested the SDK's connect status, and in the state the owner
  // hit — SDK disconnected, tunnel still installed, machine blocked — the two
  // disagreed: the page offered "Disconnect" and the tray offered "Connect",
  // over a machine that needed exactly one of them. Both now ask the same
  // predicate, which says Disconnect whenever there is anything for a disconnect
  // to DO.
  //
  // Out of balance the tray's Connect starts nothing and opens the upgrade path
  // (BalanceGate.h, start connect); its Disconnect always works. Neither is
  // offered without a status to act on (TrayConnectionState.h), and a click
  // that finds none since the menu opened does nothing.
  cb.onConnectToggle = [this] {
    const tray::Action action = tray::ActionFor(CurrentTrayFacts());
    if (action == tray::Action::None) return;
    struct Sinks {
      AppController& app;
      void Connect() { app.sdk_.ConnectBestAvailable(); }
      void Disconnect() { app.sdk_.Disconnect(); }
      void Upgrade() {
        LogInfo("app: tray connect blocked: out of balance");
        // the refused Connect waits on the balance and runs again once it is back
        app.WaitOnBalance([self = &app] { self->sdk_.ConnectBestAvailable(); });
        app.ShowUpgradeForBlockedConnect();
      }
    } sinks{*this};
    urnw::balance::RouteConnectGesture(action == tray::Action::Disconnect, OutOfBalance(),
                                       sinks);
  };
  cb.connectAction = [this] { return tray::ActionFor(CurrentTrayFacts()); };
  // The two recovery items. Both read the LAST STATUS THE SERVICE PUSHED rather
  // than any app-side belief: the whole point is that they are reachable when
  // the app's own view of the world is the thing that has gone wrong (the window
  // is closed, the connect controller is stuck, the tunnel is up over nothing).
  //
  // NARROWED, now that Disconnect does this job on the happy path. Two items
  // promising "restore my internet" is a worse menu than one; this one is what
  // is left when the ordinary Disconnect is not the obvious answer — a machine
  // still captured while the app has no live session, or a bring-up wedged
  // before it ever reported Up. The armed kill switch is excluded because the
  // item below is the one that lifts it, with a label that says so.
  cb.canStopTunnel = [this] {
    const gesture::ServiceFacts f = CurrentServiceFacts();
    return gesture::MachineIsCaptured(f) && !gesture::BlockedByKillSwitch(f) &&
           !(connected_ && sdk_.HasSession());
  };
  cb.onStopTunnel = [this] {
    LogInfo("app: tray -> force the tunnel off (recovery)");
    OnTunnelState(sdk_.StopServiceTunnel());
  };
  // "The kill switch is holding this machine blocked, AND the service can lift
  // it if asked." The second half is the part that was missing: the old gate was
  // `wfp_state != "off"`, which offered "unblock this machine" over a CONNECTED
  // policy — where TunnelController::SetKillSwitch's lift arm does nothing and
  // returns true. That was a control that existed and provably could not do what
  // its label said, offered in exactly the state the owner was stuck in.
  cb.canLiftKillSwitch = [this] {
    return gesture::KillSwitchIsLiftable(CurrentServiceFacts());
  };
  cb.onLiftKillSwitch = [this] {
    LogInfo("app: tray -> turn the kill switch off");
    sdk_.SetKillSwitch(false);
  };
  // The user's Quit stops the tunnel and the provider with the app; a close
  // from outside the app only ends the app (AppLifetime.h).
  cb.onQuit = [this] {
    LogInfo("app: tray -> quit");
    Shutdown(lifetime::Ending::Quit);
  };
  cb.onCloseRequest = [this] { Shutdown(lifetime::Ending::CloseRequest); };
  // Signing out of Windows, or a shutdown, stops them as Quit does (owner
  // decision, 2026-10-05).
  cb.onSessionEnd = [this] {
    LogInfo("app: tray -> the Windows session is ending");
    Shutdown(lifetime::Ending::SessionEnd);
  };
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

  // Every connect entry point (button, hero, tray, location and peer rows)
  // passes through this before it starts anything (BalanceGate.h).
  sdk_.SetStartConnectGate([this] { return CurrentStartConnectFacts(); },
                           [this](std::function<void()> refused) {
                             WaitOnBalance(std::move(refused));
                             ShowUpgradeForBlockedConnect();
                           },
                           [this](std::function<void()> settled) {
                             balance_.FetchThen(std::move(settled));
                           });
  // another connect, or the user's Disconnect, ends any wait on the balance
  sdk_.SetConnectGestureObserver([this] { ClearBalanceRecovery(); },
                                 [this] { ClearBalanceRecovery(); });
  // A start waiting on its balance shows as connecting (StartConnectWait.h).
  // Through the queue, so a refused start's way back renders before the
  // upgrade path it opens (ShowUpgradeForBlockedConnect queues after it).
  // A sign-out's tail runs off this thread (SdkHost::Logout): the window shows
  // it running, and the tray offers no action meanwhile.
  sdk_.SetSignOutProgressHandler([this](bool signingOut) {
    OnUi([this, signingOut] {
      UpdateTray();
      if (!windowVisible_ || !window_) return;  // resynced on show
      if (auto self = window_.try_as<winrt::URnetwork::implementation::MainWindow>())
        self->SetSigningOut(signingOut);
    });
  });
  sdk_.SetStartWaitHandler([this](StartWaitChange change) {
    OnUi([this, change] {
      if (!windowVisible_ || !window_) return;  // resynced on show
      if (auto self = window_.try_as<winrt::URnetwork::implementation::MainWindow>())
        self->OnStartWaitChanged(change);
    });
  });

  // SDK state -> tray + window (marshaled onto the UI thread).
  sdk_.SetAuthStateHandler([this](AuthState s, const std::string& e) {
    OnUi([this, s, e] { OnAuthState(s, e); });
  });
  // The server rejected the stored auth (e.g. the client was removed, or
  // another device signed this session out): log out and return to the login
  // panel, which says why when the server said so. Logout() fires the
  // auth-state handler.
  sdk_.SetAuthInvalidHandler([this](std::string cause) {
    OnUi([this, cause] { OnAuthInvalid(cause); });
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
    // a plan flip or a confirmation poll changes the gate, and a balance that
    // rose ends a latched out-of-balance state
    ObserveBalanceLatch();
    ReactToBalance();
    // and a balance that is back runs a connect the balance blocked
    ObserveBalanceRecovery();
    if (windowVisible_ && window_) {
      if (auto self = window_.try_as<winrt::URnetwork::implementation::MainWindow>()) {
        self->SetInsufficientBalance(insufficientBalance_);
        self->OnBalanceChanged(snapshot, poll);
      }
    }
  });
  // Referral celebrations (the king-frog gold moments). The store only polls
  // while the window is visible, so a celebration always has a window to land
  // in; the visibility check is belt-and-suspenders.
  balance_.SetReferralCelebrationHandler([this](const ReferralCelebration& celebration) {
    if (windowVisible_ && window_) {
      if (auto self = window_.try_as<winrt::URnetwork::implementation::MainWindow>())
        self->OnReferralCelebration(celebration);
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

    // A browser approval's five minutes (UrIoApproval.h). One-shot: BeginApproval starts it
    // and CancelApproval stops it, so the attempt's secret is wiped, and an open sheet told,
    // when the time is up even though nothing else happens.
    approvalTimer_ = uiThread_.CreateTimer();
    approvalTimer_.Interval(approval::kLife);
    approvalTimer_.IsRepeating(false);
    approvalTimer_.Tick([this](auto const&, auto const&) { OnApprovalTimer(); });
  }

  // The update checker (beta spec §5): its worker owns the launch-delay check
  // and the 6h cadence, and an update runs the elevated helper and waits on
  // it. The app does not quit for the installer: the MSI closes it before it
  // replaces its files (WM_CLOSE, a close request, through CloseApplication)
  // and stops the service itself.
  //
  // What the service last told this process of the tunnel, pushed or
  // answered: an update whose hand-off finds no status pushed starts its note
  // of the VPN from it (UpdateMarker.h VpnNote, the lead's G10a). The
  // service's word, not the sign-in state: a tunnel from another session on
  // this machine is still the VPN's state.
  updates_.SetVpnAnswerProbe([this]() -> std::optional<bool> {
    const std::optional<proto::TunnelState> state = sdk_.AdoptedServiceState();
    if (!state) return std::nullopt;
    return *state == proto::TunnelState::Up;
  });
  updates_.Start();

  // "Launch URnetwork on system startup": a registration the user turned on
  // follows this install (its path, and --autostart), and none is made here
  // (StartupRegistration.h).
  RefreshLaunchAtStartup();
  // The urnetwork:// handler a browser sign-in wrote for this user follows this install the
  // same way (ProtocolRegistration.h): refreshed here, never created here - that is for an
  // attempt to do, when the person clicks.
  RefreshProtocolHandler();

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
  // A build that points the approval at a page of its own says so, whatever the flag is: the override is
  // defined on a compiler's command line (Config.h), where an environment variable left set can reach a build
  // nobody meant it for, and nothing else would show it. The host only, never a link.
  if constexpr (config::kApprovalOriginIsATest) {
    LogWarn("approval: this build talks to a test origin: {}", config::kApprovalHost);
  }
  LogInfo("app: started");
}

void AppController::Shutdown(lifetime::Ending ending) {
  // Once. A double Quit click, or the relaunch handoff racing a tray quit,
  // must not run the teardown below twice against a window that is half gone.
  // exchange() also flips the OnUi gate before anything is torn down, so no
  // SDK callback can queue new UI work into the drain that follows.
  if (quitting_.exchange(true, std::memory_order_acq_rel)) return;
  const lifetime::Plan plan = lifetime::PlanFor(ending);
  LogInfo("app: shutdown requested ({}): {}", lifetime::ToString(ending), plan.why);
  // Before anything holds this thread: a launch that reaches this instance
  // from here on is refused with the exiting signal raised, waits for this
  // process to end and starts the app itself, instead of being queued for a
  // window this thread will never show (InstanceHandover.h).
  BeginExiting();
  // First, and joined: the checker's worker is the one thread here that does
  // long blocking I/O (a zip download), and it polls its stop flag between
  // reads, so this is bounded — see UpdateChecker::Stop.
  updates_.Stop();
  // ...and quitting is the other
  if (shell::SaveWindowPlacement(windowHwnd_)) ownPlacement_ = true;
  // D3: DRAIN THE ASYNC MACHINERY WHILE THE QUEUE STILL DISPATCHES. The
  // tray-quit crash (0xc000027b, CoreMessagingXP, ERROR_INVALID_OPERATION)
  // is what a late completion looks like when it resumes on a DispatcherQueue
  // that has begun tearing down and nothing observes the throw. Everything
  // that can produce such a completion is stopped HERE, on the UI thread,
  // before Exit() ends the loop:
  //
  //   * the controller-owned DispatcherQueueTimers (placement save, the
  //     browser approval's five minutes, the balance poll) are stopped — a
  //     timer left running keeps a CoreMessaging callback registered into the
  //     teardown;
  //   * the window is closed AND RELEASED. The release is the load-bearing
  //     half: window_ used to hold the MainWindow alive until static
  //     destruction, long after the queue was gone, so every page timer
  //     (charts, developer poll, login debounces, snackbar auto-hide) was
  //     still scheduled while the queue shut down. Dropping the reference on
  //     this thread runs ~MainWindow now — the page destructors stop their
  //     own timers, by their documented contract — and every weak-ref lambda
  //     already queued finds null and no-ops instead of touching a dead tree.
  //
  // Nothing up to the service stop below waits on anything unbounded:
  // updates_.Stop() above is the only join, and it is bounded by design.
  if (placementSaveTimer_) placementSaveTimer_.Stop();
  // a browser approval does not outlive the app: its key is wiped and its timer stopped
  CancelApproval(approval::ClearReason::Shutdown);
  balance_.Stop();
  tray_.Destroy();
  if (window_) window_.Close();
  window_ = nullptr;
  windowHwnd_ = nullptr;
  // The user's Quit ends the tunnel and the provider too (owner decision,
  // 2026-10-05). Last before the exit, with the window and the tray already
  // gone, because it holds this thread: the user's Disconnect still queued
  // runs first, then the service stops, and none of it holds the exit past
  // exitdrain::kQuitCap, a sign-out of Windows waiting in WM_ENDSESSION
  // included (ExitDrain.h). Every other ending leaves the service as it is,
  // but the user's Disconnect still reaches it first, for at most the
  // commands' cap, so the next launch does not reattach to a connection the
  // user ended. Nothing on screen is left to freeze meanwhile. Nothing it
  // throws may keep the app from exiting: the tray, its only way out, is
  // already gone.
  try {
    if (plan.stopService) sdk_.Quit();
    else sdk_.SettleCommandsForExit(std::chrono::steady_clock::now() + exitdrain::kCommandsCap);
    // A sign-out's tail ends before the exit too, or the next launch is still
    // signed in: Quit waits for it among its joins, every other ending here, for
    // at most the commands' cap again, within the quit's own bound.
    if (!plan.stopService) {
      sdk_.SettleSignOutForExit(std::chrono::steady_clock::now() + exitdrain::kCommandsCap);
    }
  } catch (const std::exception& e) {
    LogError("app: stopping the service on quit failed: {}", e.what());
  }
  // A pass or the quit still inside an rpc owns the host
  if (!sdk_.ExitSettled()) LeaveAppToProcessExit();
  if (auto app = Application::Current()) app.Exit();
}

void AppController::OnAuthInvalid(const std::string& cause) {
  // With a session up the Api's listener and the device's both report the
  // rejection, and a sign-out the user made may have run before either: only
  // the first report to find the app signed in signs it out, and leaves the
  // sign-in page its notice.
  if (!signedOutNotice_.Rejected(sdk_.IsLoggedIn(), cause)) return;
  LogInfo("app: the server ended this sign-in (cause \"{}\"); signing out", cause);
  sdk_.Logout();
}

void AppController::OnAuthState(AuthState state, const std::string& error) {
  const bool wasLoggedIn = (authState_ == AuthState::LoggedIn);
  authState_ = state;
  authError_ = error;
  authErrorUndelivered_ = !error.empty();
  // A browser approval is a sign-in this app started and waits on. Any OTHER sign-in
  // starting (every one pushes Authenticating first) or succeeding, and a sign-out, ends
  // it, so a late answer from the browser is a stray (UrIoApproval.h). The approval's own
  // redeem cannot trip this: its attempt is consumed before the redeem, and holding the
  // redeemed network pushes no state.
  if (state == AuthState::Authenticating) {
    CancelApproval(approval::ClearReason::SignInStarted);
  } else if (state == AuthState::LoggedIn) {
    CancelApproval(approval::ClearReason::SignedIn);
  } else if (state == AuthState::LoggedOut && wasLoggedIn) {
    CancelApproval(approval::ClearReason::SignedOut);
  } else if (approval_.Live() && approval_.Flow() != sdk_.CurrentBrowserFlow()) {
    // A server change takes the next flow number and pushes only LoggedOut, which a signed-out app
    // does not count as a sign-out: what is left behind is an attempt for the other server.
    CancelApproval(approval::ClearReason::Superseded);
  }
  // A person who lands on the sign-in screen with the window already on screen (a sign-out, or a switch to
  // another network) is who the page check is for, and no window show follows to begin it
  // (ShowWindowImpl). One check a run, so this is safe to repeat; with no window shown (a launch, an
  // autostart) it begins nothing.
  if (state == AuthState::LoggedOut && windowShown_) StartApprovalCheck();
  UpdateTray();
  // balance store lifecycle follows the session. A repeated LoggedIn push is a
  // device re-registration (guest upgrade): restart the store so the plan
  // reseeds from the new jwt's claims (Guest -> Free).
  if (state == AuthState::LoggedIn) {
    balance_.Start();
    // signed in again: an earlier sign-out's notice is not shown
    signedOutNotice_.Clear();
  } else if (state == AuthState::LoggedOut && wasLoggedIn) {
    balance_.Stop();
  }
  // a sign-in or sign-out ends the session the out-of-balance latch describes
  if (state == AuthState::LoggedIn || (state == AuthState::LoggedOut && wasLoggedIn)) {
    balanceLatch_.Reset();
    rawInsufficientBalance_ = false;
    providersConnected_ = false;
    insufficientBalance_ = false;
    // a connect the previous session asked for must not start in this one
    connectRequested_ = false;
    ClearBalanceRecovery();
  }
  // the tray always reflects state; only push into the window when it is
  // actually visible (resynced on show) so a hidden window doesn't churn.
  if (windowVisible_ && window_) {
    if (auto self = window_.try_as<winrt::URnetwork::implementation::MainWindow>()) {
      self->OnAuthStateChanged(state, error);
      authErrorUndelivered_ = false;  // a presented window has it now
    }
  }
  if (state == AuthState::LoggedIn) {
    tray_.ShowBalloon(Localized("app_name"), Localized("signed_in"));
  }
}

void AppController::OnTunnelState(const proto::TunnelStatus& status) {
  const bool isUp = (status.state == proto::TunnelState::Up);
  // #27: health evidence is scoped to the tunnel session that produced it. Any
  // transition — down, up-from-down, error — invalidates it; only a steady Up
  // keeps it. Without this, a tray showing Degraded when the window was hidden
  // would keep saying so across a reconnect that fixed everything (stats stop
  // flowing while hidden, so nothing else would ever correct it).
  if (!(connected_ && isUp)) trayHealth_.reset();
  // --- #41: the service turned the tunnel off BY ITSELF ----------------------
  //
  // The one transition the user did not ask for, and therefore the only one that
  // has to announce itself. It is announced ON THE EDGE — the first status that
  // carries a failsafe reason this session — because the reason persists in
  // every subsequent status until the next start, and a balloon per poll would
  // be worse than silence.
  //
  // The balloon is deliberately the channel: this fires when the window may not
  // exist (closed to tray), and it is exactly then that the owner is looking at
  // a machine whose internet just changed with no explanation on screen.
  const bool failsafe = proto::IsFailsafeStop(status.stop_reason);
  const bool wasFailsafe =
      lastTunnelStatus_ && proto::IsFailsafeStop(lastTunnelStatus_->stop_reason);
  if (failsafe && !wasFailsafe) {
    const bool stillBlocked = status.wfp_state != "off";
    LogWarn("app: the service stopped the tunnel by itself ({}); the machine is "
            "{}",
            status.stop_reason,
            stillBlocked ? "STILL BLOCKED (kill switch on)"
                         : "back on its normal connection, unprotected");
    tray_.ShowBalloon(
        Localized("app_name"),
        stillBlocked
            // Kill switch ON. The promise is being kept, so the copy leads with
            // that rather than apologising: nothing is leaking, and the escape
            // is named because it is one click away in this very menu.
            ? pages::AdvW(
                  "conn_tray_failsafe_blocked",
                  L"The tunnel could not carry traffic, so URnetwork shut it "
                  L"down. The kill switch is on, so this machine stays blocked "
                  L"and nothing is leaking. Reconnect, or turn off the kill "
                  L"switch from this menu.")
            // Kill switch OFF. Two facts, in the order that matters to someone
            // whose internet just came back: what happened, then that they are
            // no longer protected.
            : pages::AdvW(
                  "conn_failsafe_restored",
                  L"URnetwork disconnected you to keep you online: the tunnel "
                  L"was up but nothing was getting through. Your traffic is "
                  L"going out normally now and is NOT protected. Press Connect "
                  L"to try again."));
  }
  connected_ = isUp;
  // an update notes it as it begins: its report says to connect again only
  // when there was a VPN to lose
  updates_.SetVpnConnected(isUp);
  lastTunnelStatus_ = status;
  UpdateTray();
  if (windowVisible_ && window_) {
    if (auto self = window_.try_as<winrt::URnetwork::implementation::MainWindow>())
      self->OnTunnelStateChanged(status);
  }
}

void AppController::OnStats(const LiveStats& stats) {
  // Reads on different threads finish out of order: one older than a snapshot
  // already applied would roll the tray and the window back (StatsReadOrder.h).
  if (!statsReadOrder_.Admit(stats.readSequence)) return;
  // #27: the tray is a stats consumer now — the aggregate health rides every
  // snapshot. Change-gated: stats push on every throughput tick, and a
  // Shell_NotifyIcon per second for an unchanged icon is churn for nothing.
  if (trayHealth_ != std::optional<health::State>{stats.health}) {
    trayHealth_ = stats.health;
    UpdateTray();
  }
  // The contract status is reset with the destination, the user's Disconnect
  // included, so the gate reads the latch, not the raw push (BalanceGate.h).
  rawInsufficientBalance_ = stats.insufficientBalance;
  providersConnected_ = stats.connectionStatus == "CONNECTED" && 0 < stats.providerCount;
  connectRequested_ = stats.connected;
  ObserveBalanceLatch();
  ReactToBalance();
  ObserveBalanceRecovery();
  // Live stats otherwise only matter to the window; push only when visible.
  // The window's gate and banner read the same latched state.
  if (windowVisible_ && window_) {
    if (auto self = window_.try_as<winrt::URnetwork::implementation::MainWindow>()) {
      LiveStats latched = stats;
      latched.insufficientBalance = insufficientBalance_;
      self->OnStatsChanged(latched);
    }
  }
}

void AppController::ObserveBalanceLatch() {
  urnw::balance::OutOfBalanceLatch::Observation o;
  o.insufficientBalance = rawInsufficientBalance_;
  o.providersConnected = providersConnected_;
  const BalanceSnapshot balance = balance_.Current();
  o.balanceKnown = balance.loaded;
  o.availableBytes = balance.availableByteCount;
  balanceLatch_.Observe(o);
  insufficientBalance_ = balanceLatch_.InsufficientBalance();
}

void AppController::ReactToBalance() {
  // The sinks the shared reaction may use. Disconnect is the user path and is
  // never called from there: out of balance, capture stays until the user
  // disconnects, so nothing leaves outside the tunnel without them knowing.
  struct Sinks {
    AppController& app;
    void Notice() {
      // one line per posted notice: the acceptance driver counts these, since
      // a balloon cannot be read back from the shell
      LogInfo("app: insufficient balance notice posted");
      app.tray_.ShowBalloon(Localized("insufficient_balance"),
                            Localized("insufficient_balance_held_notice"));
    }
    void Disconnect() { app.sdk_.Disconnect(); }
  } sinks{*this};
  urnw::balance::ReactToBalancePush(balanceNotice_, insufficientBalance_,
                                    balance_.Current().isPro,
                                    balance_.CurrentPoll().confirming, sinks);
}

bool AppController::OutOfBalance() const {
  return urnw::balance::OutOfBalance(insufficientBalance_, balance_.Current().isPro,
                                     balance_.CurrentPoll().confirming);
}

urnw::balance::StartConnectFacts AppController::CurrentStartConnectFacts() const {
  const BalanceSnapshot balance = balance_.Current();
  urnw::balance::StartConnectFacts f;
  f.latched = insufficientBalance_;
  f.supporter = balance.isPro;
  f.confirming = balance_.CurrentPoll().confirming;
  f.balance.known = balance.loaded;
  f.balance.pro = balance.isPro;
  f.balance.availableBytes = balance.availableByteCount;
  f.balance.openTransferBytes = balance.pendingByteCount;
  f.balance.fetchedAtMs = balance.fetchedAtMillis;
  f.fetchSettled = balance_.FetchSettledAtMillis() != 0;
  f.fetchSettledAtMs = balance_.FetchSettledAtMillis();
  // the same monotonic clock the store stamps its fetches with
  f.nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count();
  return f;
}

void AppController::ShowUpgradeForBlockedConnect() {
  // Deferred: a row click lands here from inside the location chooser, whose
  // dialog must close before the upgrade sheet can show.
  OnUi([this] {
    ShowWindow(nullptr);
    if (auto self = window_.try_as<winrt::URnetwork::implementation::MainWindow>())
      self->OpenUpgradeForBlockedConnect();
  });
}

void AppController::WaitOnBalance(std::function<void()> refused) {
  if (!refused) return;
  balanceRecovery_.StartRefused(std::move(refused), CurrentStartConnectFacts().nowMs);
  PublishBalanceRecovery();
}

void AppController::ClearBalanceRecovery() {
  balanceRecovery_.Clear();
  PublishBalanceRecovery();
}

void AppController::ObserveBalanceRecovery() {
  // the balance as the start-connect gate reads it, on the same monotonic clock
  const urnw::balance::StartConnectFacts facts = CurrentStartConnectFacts();
  auto step = balanceRecovery_.Observe(OutOfBalance(), connectRequested_, facts.balance, facts.nowMs);
  PublishBalanceRecovery();
  if (step.kind == urnw::balance::RecoveryStepKind::None) return;
  LogInfo("app: the balance is back: retrying the connect it blocked");
  // Past the gate: the recovery decided on a fresh balance, and a held
  // connection keeps the gate latched until the rebuild replaces it.
  if (step.kind == urnw::balance::RecoveryStepKind::Start) {
    sdk_.RetryRefusedConnect(step.target);
  } else {
    // the held connection's selection, read on the session worker
    sdk_.RetryRefusedConnect([this] { sdk_.ConnectSelected(); });
  }
  tray_.ShowBalloon(Localized("app_name"), Localized("insufficient_balance_reconnecting"));
}

void AppController::PublishBalanceRecovery() {
  if (!windowVisible_ || !window_) return;
  if (auto self = window_.try_as<winrt::URnetwork::implementation::MainWindow>())
    self->SetBalanceRecovery(balanceRecovery_.State());
}

gesture::ServiceFacts AppController::CurrentServiceFacts() const {
  gesture::ServiceFacts f;
  if (!lastTunnelStatus_) return f;
  // The pipe is up if the service has ever pushed us a status and has not since
  // told us it went away (OnServiceDisconnected pushes the honest all-defaults
  // status, which lands here).
  f.pipeUp = sdk_.ServiceConnected();
  f.known = f.pipeUp;
  f.state = lastTunnelStatus_->state;
  f.mode = lastTunnelStatus_->mode;
  f.routesInstalled = lastTunnelStatus_->routes_installed;
  f.wfpState = lastTunnelStatus_->wfp_state;
  f.stopReason = lastTunnelStatus_->stop_reason;
  return f;
}

health::State AppController::TrayHealth() const {
  if (trayHealth_) return *trayHealth_;
  return connected_ ? health::State::Connected : health::State::Disconnected;
}

tray::Facts AppController::CurrentTrayFacts() const {
  tray::Facts facts;
  // a sign-out's tail still running: signed out from its click, no action
  facts.signedIn = authState_ == AuthState::LoggedIn && sdk_.IsLoggedIn();
  facts.statusKnown = lastTunnelStatus_.has_value();
  if (lastTunnelStatus_) {
    facts.tunnel = tray::TunnelOf(lastTunnelStatus_->state);
    facts.routesInstalled = lastTunnelStatus_->routes_installed;
    facts.wfpState = lastTunnelStatus_->wfp_state;
    facts.failsafeStop = proto::IsFailsafeStop(lastTunnelStatus_->stop_reason);
  }
  facts.health = trayHealth_;
  facts.actionIsDisconnect = gesture::ActionIsDisconnect(CurrentServiceFacts(), TrayHealth());
  return facts;
}

void AppController::UpdateTray() {
  // v1 maps provide=false. The icon shows connected, and the status says so,
  // only with the tunnel up and (#27) the aggregate health connected: "proven
  // working", so Evaluating/Degraded keep the not-connected art. With no stats
  // since the tunnel came up its own claim stands (TrayConnectionState.h).
  const tray::View view = tray::Decide(CurrentTrayFacts());
  tray_.SetState(view.connectedIcon ? TrayState::NoProvideConnect
                                    : TrayState::NoProvideNoConnect);
  // "URnetwork — Connected": the app name, then the status. The em dash is
  // punctuation, not text. The health words reuse the connect page's exact
  // strings (same ids), so the tooltip and the status line can never name the
  // same state differently.
  std::wstring status;
  switch (view.status) {
    case tray::Status::None:
      break;
    case tray::Status::Connected:
      status = Localized("connected");
      break;
    case tray::Status::FindingProviders:
      status = pages::AdvW("conn_finding_providers", L"Finding providers…");
      break;
    case tray::Status::Degraded:
      status = pages::AdvW("conn_degraded", L"Connection degraded — reconnecting");
      break;
    case tray::Status::Connecting:
      // a connect in progress, the tunnel starting or up with no provider yet
      status = Localized("connecting_status_indicator");
      break;
    case tray::Status::Failed:
      // a start the service could not finish, or the window honesty layer's
      // terminal outcome (track 2): the page shows the reason and a Retry
      status = pages::AdvW("conn_failed", L"Couldn't connect");
      break;
    // #41: states that are not "disconnected" though no tunnel is up, which the
    // app name alone would render as an idle machine. The icon keeps the
    // ordinary not-connected art, so the tooltip carries the distinction.
    case tray::Status::Disconnecting:
      // routes still installed with nothing carrying them (the owner's bug A):
      // the transient word, as the stop is in flight
      status = pages::AdvW("conn_tray_disconnecting",
                           L"Disconnecting — traffic is still going through the tunnel");
      break;
    case tray::Status::BlockedKillSwitch:
      // blocked on purpose and nothing is leaking; the menu's lift item undoes it
      status = pages::AdvW("conn_tray_blocked_kill_switch", L"Blocked (kill switch on)");
      break;
    case tray::Status::BlockedNoTunnel:
      // a policy the kill switch did not put there, outliving its tunnel: blocked,
      // and the honest word for it is not "kill switch"
      status = pages::AdvW("conn_tray_blocked_no_tunnel",
                           L"Blocked — leak protection is still in force with no tunnel up");
      break;
    case tray::Status::FailsafeStopped:
      status = pages::AdvW("conn_tray_failsafe_stopped", L"Disconnected (the tunnel failed)");
      break;
  }
  std::wstring tip = Localized("app_name");
  if (!status.empty()) tip += L" — " + status;
  tray_.SetTooltip(tip);
}

void AppController::ShowWindow(const POINT* anchor) {
  // Every caller of this arrives from the tray icon's window procedure, and a
  // C++ exception must not unwind out of one (TrayIcon::WndProc catches as the
  // last resort, but by then the message is generic). Everything this path does
  // — creating the window, asking it for its HWND, wiring its events, moving it,
  // and starting the presentation controllers in ReconcileWindowPresentation —
  // is a WinRT call that can throw.
  // A quit is in progress: the window reference was just released on purpose,
  // and rebuilding it from a racing activation (deep link, relaunch) would
  // resurrect the exact timers the shutdown drain exists to stop.
  if (quitting_.load(std::memory_order_acquire)) return;
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
    // Minimize/restore tracking. windowShown_ only knows about the tray
    // (ShowWindow/HideWindow); the minimize box passes through neither, so
    // without this the presentation would keep burning CPU into a taskbar
    // button. XAML's Window.VisibilityChanged reports Visible=false on
    // minimize as well as on AppWindow.Hide(), so it covers both teardown
    // states; the handler re-reads IsIconic instead of trusting args.Visible()
    // (see SyncWindowMinimized for why). Window.Activated does not feed the
    // presentation gate: focus loss used to stop the presentation, and the
    // rebuild-on-refocus was the reset the owner reported.
    window_.VisibilityChanged([this](auto const&, auto const&) {
      SyncWindowMinimized();
      ReconcileWindowPresentation();
    });
    // Activation feeds only the purchase-confirmation poll. A hosted checkout
    // leaves this window visible behind the browser, so without focus the
    // confirmation budget burned while the user paid and came back to a
    // false "timed out" (UPGRADE.md D1).
    window_.Activated([this](auto const&, WindowActivatedEventArgs const& args) {
      balance_.SetFocused(args.WindowActivationState() != WindowActivationState::Deactivated);
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
  // A tray-show reaching a MINIMIZED window must restore it first: Activate()
  // does not un-minimize (a long-standing WinUI quirk), and with the gate keyed
  // on visibility the click would otherwise read as dead — shown, still iconic,
  // presentation off. Before the rect log below, so that line reports the real
  // placement instead of the iconic placeholder rect (-32000,-32000).
  if (windowHwnd_ && ::IsIconic(windowHwnd_)) ::ShowWindow(windowHwnd_, SW_RESTORE);
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
  SyncWindowMinimized();
  // The browser approval's page check begins when someone is looking at a signed-out window: that is
  // when the action could be wanted, and when the network is likely to be up (a check made at launch,
  // before it was, would keep the action hidden for the whole run).
  if (!ApprovalContext().signedIn) StartApprovalCheck();
  window_.Activate();
  // Activate() is ShowWindow + SetActiveWindow: it never asks for the foreground,
  // so it leaves the window BEHIND the browser a sign-in just came back from,
  // painting a result no one can see (measured). RaiseToFront asks -
  // SetForegroundWindow, which the SDK's redirect has made permissible for a
  // browser click or any launch that held a right - and falls back to lifting
  // the window without focus when Windows refuses. A tray click owns the
  // foreground already and returns on its first line.
  shell::RaiseToFront(windowHwnd_);
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
  // Counted here, before the sweep below, so that a sheet the sweep closes can tell this from the person
  // closing it by asking whether the count moved, whenever its own Closed event arrives (WindowHides).
  ++windowHides_;
  // A sheet may not outlive the surface that feeds it: the window only hides, and
  // a dialog left open would come back on a draft read before the hide, so every
  // open sheet closes here. A minimize reaches only VisibilityChanged, never this,
  // and keeps its sheet, as it keeps the window.
  if (window_) {
    if (auto self = window_.try_as<winrt::URnetwork::implementation::MainWindow>()) {
      self->CloseSheetsForHide();
    }
  }
  ReconcileWindowPresentation();
  if (window_) window_.try_as<Window>().AppWindow().Hide();
}

// The minimize half of the gate. IsIconic is read fresh rather than carried
// from the VisibilityChanged payload: AppWindow.Hide() also raises
// Visible=false, and letting that set "minimized" would latch the flag across
// a hide → tray-show cycle whose window comes back restored.
void AppController::SyncWindowMinimized() {
  windowMinimized_ = windowHwnd_ != nullptr && ::IsIconic(windowHwnd_) != FALSE;
}

void AppController::ReconcileWindowPresentation() {
  const bool active =
      WindowPresentationShouldRun(windowShown_, windowMinimized_);
  if (windowVisible_ == active) return;
  windowVisible_ = active;

  // One line per transition, with the gate's inputs: a "stopped" here while
  // the window is plainly on screen is the focus-loss regression coming back.
  LogInfo("app: presentation {} (shown={}, minimized={})",
          active ? "started" : "stopped", windowShown_, windowMinimized_);

  sdk_.SetPresentationActive(active);
  balance_.SetVisible(active);
  if (!window_) return;
  if (auto self = window_.try_as<winrt::URnetwork::implementation::MainWindow>()) {
    self->SetPresentationActive(active);
    if (!active) return;

    // Re-apply the current state after the hidden/inactive interval. The stats
    // snapshot goes through OnStats (#27), not straight to the window, so the
    // tray's health reading refreshes at the same instant the window's does,
    // and it is read on SdkHost's publish worker: its DeviceRemote getters wait
    // on the device's state lock, which a connect write holds through its rpc
    // for up to the rpc timeout, and a show must not freeze this thread for
    // that long. It arrives through the stats handler, in read order.
    // The standing ERROR is replayed only while no presented window has seen it
    // (authErrorUndelivered_): a failure that landed while the window was hidden
    // is delivered once on the next show, never again after that.
    self->OnAuthStateChanged(authState_,
                             authErrorUndelivered_ ? authError_ : std::string());
    authErrorUndelivered_ = false;
    if (lastTunnelStatus_) self->OnTunnelStateChanged(*lastTunnelStatus_);
    sdk_.RepublishStats();
    self->OnBalanceChanged(balance_.Current(), balance_.CurrentPoll());
    // a start that began waiting on its balance while the window was hidden
    // a sign-out that began while the window was hidden
    self->SetSigningOut(sdk_.SigningOut());
    self->OnStartWaitChanged(sdk_.StartConnectWaiting() ? StartWaitChange::Waiting
                                                        : StartWaitChange::Ended);
    if (authState_ == AuthState::LoggedIn) balance_.Refresh();
  }
}

// ---- urnetwork:// protocol activation --------------------------------------

void AppController::ServeLaunch(const instance::LaunchRequest& request) {
  const instance::LaunchAction action = instance::ActionFor(request);
  // The first launch served is this process's own: App::OnLaunched serves it before the gate
  // opens to the launches redirected to the instance that runs. A browser's "Open URnetwork?"
  // that STARTED the app is that launch, and it is the only one whose stray auth link may show
  // the window (ShowStrayNotice); a link handed to a running instance never does.
  freshProcessLaunch_ = !launchServed_;
  launchServed_ = true;
  LogInfo("app: launch{}: {}", request.autostart ? " (autostart)" : "",
          instance::ToString(action));
  switch (action) {
    case instance::LaunchAction::HandleDeepLink:
      HandleDeepLink(request.deepLink);
      return;
    case instance::LaunchAction::ShowWindow:
      ShowWindow(nullptr);
      return;
    case instance::LaunchAction::TrayOnly:
      return;
  }
}

void AppController::HandleDeepLink(const std::string& url) {
  // never log the uri itself: a wallet callback carries the address + signature
  LogInfo("app: deep link received");
  // urnetwork://auth is judged before anything is raised or shown: the answer to
  // a sign-in this app started comes back this way, and so does any link anyone
  // fires at the app, which must not move the window or touch a screen
  if (authlink::IsAuthHost(url)) {
    HandleAuthLink(url);
    return;
  }
  // the callback comes back through the browser, so the app is in the background
  // (and may be hidden to the tray) — bring the window forward for the result
  ShowWindow(nullptr);
  // the campaign emails' buttons land on the window's destinations
  if (url.rfind("urnetwork://onboarding/", 0) == 0) {
    if (auto self = window_.try_as<winrt::URnetwork::implementation::MainWindow>()) {
      self->HandleOnboardingLink(url);
    }
    return;
  }
  // the ur.io Top 200 page's "Open in the app": the sign sheet, pre-filled only
  if (top200sign::ParseSignLink(url).sign) {
    if (auto self = window_.try_as<winrt::URnetwork::implementation::MainWindow>()) {
      self->HandleTop200SignLink(url);
    }
    return;
  }
  try {
    sdk_.HandleDeepLink(url);
  } catch (const std::exception&) {
    // The wallet bridge's crypto throws on a malformed link, and any program can
    // fire one at the app. The link is dropped and its text is not logged.
    LogWarn("app: a deep link could not be handled");
  }
}

// The gate for urnetwork://auth: every answer to a browser sign-in passes here,
// on the UI thread, before the window is raised, a screen is touched or a code
// is used. In this order: the parser (AuthLink.h) says whether the link is a
// sealed or a denied answer at all; the attempt (UrIoApproval.h) says whether it
// answers the sign-in this app started; for an approval the box is then opened
// with the attempt's own key, and only a box that holds the attempt's state is an
// answer. Only then is the attempt consumed, the window raised and the code
// handed on. A denial for the live attempt raises the window to say so. Anything
// else - a link nothing waits for, another attempt's, a box that does not open -
// raises nothing, resets nothing and disturbs no attempt that is alive (one
// exception: ShowStrayNotice). The log gets closed tokens, never the link or a
// value in it.
//
// The switch below has no default, and C4062 is an error here, so a verdict added
// to the model is a build error until this gate has decided what it does.
#pragma warning(push)
#pragma warning(error : 4062)
void AppController::HandleAuthLink(const std::string& url) {
  if (quitting_.load(std::memory_order_acquire)) return;
  const authlink::Return link = authlink::Parse(url);
  approval::Decision decision = approval_.Check(link, approval::Now(), ApprovalContext());
  std::optional<std::string> code;
  if (decision.verdict == approval::Verdict::Accepted) {
    code = approval::Unseal(approval_.SecretKey(), link);
    if (!code) decision = approval::Decision{approval::Verdict::Stray, approval::Why::BadSeal};
  }
  const std::string_view why = AuthLinkWhy(link, decision);
  LogInfo("app: auth link: {} ({})", approval::ToString(decision.verdict), why);
  switch (decision.verdict) {
    case approval::Verdict::Ignored:
      return;
    case approval::Verdict::Stray:
      // An attempt the gate found dead and wiped (its deadline passed on a clock the timer has not
      // read yet, or another flow took over) leaves a sheet waiting for an answer that cannot come:
      // it is told, as the timer tells it, and says so only if it is the page that was waiting.
      if (decision.why == approval::Why::Expired || decision.why == approval::Why::Superseded) {
        if (LoginPage* login = LoginPageOrNull()) login->OnApprovalExpired();
      }
      ShowStrayNotice();
      return;
    case approval::Verdict::Denied:
      ShowWindow(nullptr);
      if (LoginPage* login = LoginPageOrNull()) login->OnApprovalDenied();
      return;
    case approval::Verdict::Accepted:
      if (!approval_.Consume(link.state)) return;
      ShowWindow(nullptr);
      if (LoginPage* login = LoginPageOrNull()) login->OnApprovedCode(std::move(*code));
      return;
  }
}
#pragma warning(pop)

// A link that answers no attempt raises nothing, resets nothing and shows nothing.
// The one exception is the first launch of a freshly started process: the browser's
// "Open URnetwork?" started the app for a link whose attempt cannot have survived the
// restart (an attempt lives in memory only), and a launch that opened nothing looks as
// if it did nothing. That launch shows the window once, with a neutral line. A link
// handed to a running instance never does.
void AppController::ShowStrayNotice() {
  if (!freshProcessLaunch_) return;
  freshProcessLaunch_ = false;
  if (!neutralLine_.Allow(std::chrono::steady_clock::now())) return;
  ShowWindow(nullptr);
  if (LoginPage* login = LoginPageOrNull()) login->ShowStrayNotice();
}

approval::Context AppController::ApprovalContext() {
  approval::Context context;
  context.signedIn = authState_ == AuthState::LoggedIn || sdk_.IsLoggedIn();
  context.flow = sdk_.CurrentBrowserFlow();
  return context;
}

LoginPage* AppController::LoginPageOrNull() {
  if (!window_) return nullptr;
  if (auto self = window_.try_as<winrt::URnetwork::implementation::MainWindow>()) return &self->login();
  return nullptr;
}

// ---- a sign-in approved on a web page (UrIoApproval.h) ---------------------

bool AppController::ApprovalOffered() {
  return ApprovalOfferedIn(sdk_.CurrentApprovalNetwork());
}

// The decision for a network that has been read already: BeginApproval reads it once and uses that one for this and for the
// request, so an attempt cannot be offered for one network and name another.
bool AppController::ApprovalOfferedIn(const approval::Network& network) {
  if (!config::kBrowserApprovalEnabled) return false;
  if (network.kind == approval::NetworkKind::None) {
    LogInfo("approval: not offered: this network's API cannot be named in a request");
    return false;
  }
  if (ProcessIsElevated()) {
    LogInfo("approval: not offered: this process is elevated");
    return false;
  }
  if (ApprovalContext().signedIn) {
    LogInfo("approval: not offered: the app is signed in");
    return false;
  }
  StartApprovalPageCheck();
  if (ApprovalPageState() != ApprovalPage::Live) {
    LogInfo("approval: not offered: the page is not known to be live");
    return false;
  }
  // the official network needs the page to list version 1, another network version 2: a page that lists only 1 is never
  // sent a request that names a network
  if (!ApprovalPageLists(approval::ContractFor(network.kind))) {
    LogInfo("approval: not offered: the page does not list this network's contract version");
    return false;
  }
  // A key that starts something else is left alone, and then there is nothing for the browser to
  // answer to: the action stays hidden and the paste flow is all there is.
  switch (ProtocolHandlerStatus()) {
    case ProtocolHandlerState::Ours:
    case ProtocolHandlerState::Absent:
      return true;
    case ProtocolHandlerState::Foreign:
      LogInfo("approval: not offered: another program handles urnetwork:// links");
      return false;
    case ProtocolHandlerState::Unsafe:
      LogInfo("approval: not offered: this exe's path cannot be registered as the handler");
      return false;
    case ProtocolHandlerState::Failed:
      LogInfo("approval: not offered: the urnetwork:// handler could not be looked at");
      return false;
  }
  return false;
}

// The approval page's descriptor is fetched once per run (ApprovalCheck.h), and only where the action could
// be offered at all, in the official network or in another whose API a request can name: with the build flag
// off this app makes no request for it.
void AppController::StartApprovalCheck() {
  if constexpr (config::kBrowserApprovalEnabled) {
    if (sdk_.CurrentApprovalNetwork().kind != approval::NetworkKind::None && !ProcessIsElevated()) StartApprovalPageCheck();
  }
}

std::optional<approval::Slot::View> AppController::BeginApproval() {
  // A second click reads the same request back, and does not extend it - unless another flow took the number since
  // (a server change does, and pushes no sign-in state): that attempt is dead, its answer would be a stray, and
  // a request for it would send the person to approve something that can never finish. The same goes for a
  // request the page would refuse as expired by now (kReopenWindow): the click starts a new attempt.
  const approval::Clocks now = approval::Now();
  if (std::optional<approval::Slot::View> live = approval_.Peek(now)) {
    if (approval_.Flow() == sdk_.CurrentBrowserFlow() && approval_.Reopenable(now)) return live;
    CancelApproval(approval::ClearReason::Superseded);
  }
  // read once: the same network decides whether the action is offered and what the request names
  const approval::Network network = sdk_.CurrentApprovalNetwork();
  if (!ApprovalOfferedIn(network)) return std::nullopt;
  // The answer comes back as a urnetwork:// launch, so before the browser is opened the user's
  // handler for the scheme has to start THIS exe, and the shell has to say so
  if (!EnsureProtocolHandler()) {
    LogWarn("approval: the urnetwork:// handler is not this app's; the paste flow stays");
    return std::nullopt;
  }
  std::optional<approval::KeyPair> keys = approval::FreshKeyPair(approval::NewKeyPair);
  if (!keys) {
    LogWarn("approval: no usable key pair; the paste flow stays");
    return std::nullopt;
  }
  approval::Slot::Params params;
  params.publicKey = std::move(keys->publicKey);
  params.secretKey = std::move(keys->secretKey);
  // empty for the official network, which makes the request version 1; another network names its API (version 2)
  params.api = network.api;
  params.now = approval::Now();
  // last, so nothing that can still fail has already told the other flows they were superseded
  params.flow = sdk_.BeginBrowserApproval();
  std::optional<approval::Slot::View> view = approval_.Begin(std::move(params), RandomBytes);
  if (!view) {
    LogWarn("approval: no entropy for a state; the paste flow stays");
    return std::nullopt;
  }
  if (approvalTimer_) {
    approvalTimer_.Stop();
    approvalTimer_.Start();
  }
  LogInfo("approval: an attempt started (version {})", approval::ContractFor(network.kind));
  return view;
}

std::string AppController::ApprovalRequestUrl(const approval::Slot::View& view) const {
  return approval::BuildRequestUrl(config::kApprovalPageUrl, view);
}

void AppController::CancelApproval(approval::ClearReason reason) {
  if (approvalTimer_) approvalTimer_.Stop();
  if (!approval_.Live()) return;
  approval_.Clear(reason);
  LogInfo("approval: the attempt ended: {}", approval::ToString(reason));
}

void AppController::OnApprovalTimer() {
  CancelApproval(approval::ClearReason::TimedOut);
  // An attempt the gate already wiped (it found the deadline passed on the wall clock, or another flow
  // took over) leaves a sheet waiting for an answer that cannot come, so the sheet is told whether or
  // not the slot still held the attempt, and only a sheet that is waiting says anything.
  if (LoginPage* login = LoginPageOrNull()) login->OnApprovalExpired();
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

instance::LaunchRequest LaunchRequestFromActivation(
    winrt::Microsoft::Windows::AppLifecycle::AppActivationArguments const& args) {
  namespace lifecycle = winrt::Microsoft::Windows::AppLifecycle;
  namespace activation = winrt::Windows::ApplicationModel::Activation;
  instance::LaunchRequest request{.deepLink = DeepLinkFromActivation(args)};
  if (args && args.Kind() == lifecycle::ExtendedActivationKind::Launch) {
    if (auto launchArgs = args.Data().try_as<activation::ILaunchActivatedEventArgs>()) {
      request.autostart =
          instance::HasArgument(launchArgs.Arguments(), instance::kAutostartArgument);
    }
  }
  return request;
}

instance::LaunchRequest OwnLaunchRequest() {
  namespace lifecycle = winrt::Microsoft::Windows::AppLifecycle;
  const auto activation = lifecycle::AppInstance::GetCurrent().GetActivatedEventArgs();
  instance::LaunchRequest request{.deepLink = DeepLinkFromActivation(activation),
                                  .autostart = LaunchedByAutostart()};
  if (request.deepLink.empty()) request.deepLink = LaunchDeepLink();  // our own command line
  return request;
}

}  // namespace urnw
