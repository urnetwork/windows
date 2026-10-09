// SPDX-License-Identifier: MPL-2.0
#include "pch.h"

#include "AppController.h"

#include <algorithm>
#include <chrono>
#include <string_view>

#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Microsoft.UI.Interop.h>
#include <winrt/Windows.ApplicationModel.Activation.h>

#include "Ids.h"
#include "LaunchAtStartup.h"
#include "Localization.h"
#include "Log.h"
#include "MainWindow.xaml.h"
#include "PageContext.h"  // pages::AdvW — the tray tooltip's health words (#27)
#include "SingleInstance.h"
#include "Startup.h"
#include "Strings.h"
#include "WindowShell.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;

namespace urnw {
namespace {
std::unique_ptr<AppController> g_app;

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
// the wallet-connect bridge percent-encodes the query.
std::wstring DeepLinkFromCommandLine(std::wstring_view commandLine) {
  const std::wstring prefix = std::wstring(ids::kUriScheme) + L"://";
  const size_t start = commandLine.find(prefix);
  if (start == std::wstring_view::npos) return {};
  size_t end = commandLine.find_first_of(L" \t\"", start);
  if (end == std::wstring_view::npos) end = commandLine.size();
  return std::wstring(commandLine.substr(start, end - start));
}

// Exactly the "auth" host - "urnetwork://auth", "urnetwork://auth?..." - so
// "urnetwork://authenticator/..." cannot match.
bool IsAuthDeepLink(const std::string& url) {
  constexpr char kPrefix[] = "urnetwork://auth";
  constexpr size_t kLen = sizeof(kPrefix) - 1;
  if (url.rfind(kPrefix, 0) != 0) return false;  // the onboarding check's idiom
  if (url.size() == kLen) return true;
  const char next = url[kLen];
  return next == '?' || next == '/' || next == '#';
}

// One percent-decoded query parameter of a urnetwork:// uri, "" when absent.
// WalletConnect.cpp has the full parser (ParseQuery/Unesc) but keeps it in its
// own anonymous namespace; exporting it for a single parameter is worse than
// these twenty lines.
std::string DeepLinkQueryParam(const std::string& url, std::string_view name) {
  auto hexv = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
  };
  const size_t q = url.find('?');
  if (q == std::string::npos) return {};
  size_t i = q + 1;
  while (i < url.size()) {
    const size_t amp = url.find('&', i);
    const std::string pair =
        url.substr(i, amp == std::string::npos ? std::string::npos : amp - i);
    const size_t eq = pair.find('=');
    if (eq != std::string::npos && pair.compare(0, eq, name) == 0) {
      const std::string value = pair.substr(eq + 1);
      std::string out;
      out.reserve(value.size());
      for (size_t k = 0; k < value.size(); ++k) {
        if (value[k] == '%' && k + 2 < value.size()) {
          const int hi = hexv(value[k + 1]), lo = hexv(value[k + 2]);
          if (hi >= 0 && lo >= 0) {
            out.push_back(static_cast<char>((hi << 4) | lo));
            k += 2;
            continue;
          }
        }
        out.push_back(value[k] == '+' ? ' ' : value[k]);
      }
      return out;
    }
    if (amp == std::string::npos) break;
    i = amp + 1;
  }
  return {};
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
  // (BalanceGate.h, start connect); its Disconnect always works.
  cb.onConnectToggle = [this] {
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
    urnw::balance::RouteConnectGesture(
        gesture::ActionIsDisconnect(CurrentServiceFacts(), TrayHealth()), OutOfBalance(), sinks);
  };
  cb.isConnected = [this] {
    return gesture::ActionIsDisconnect(CurrentServiceFacts(), TrayHealth());
  };
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
  }

  // The update checker (beta spec §5): its worker owns the launch-delay check
  // and the 6h cadence, and an update runs the elevated helper and waits on
  // it. The app does not quit for the installer: the MSI closes it before it
  // replaces its files (WM_CLOSE, a close request, through CloseApplication)
  // and stops the service itself.
  updates_.Start();

  // "Launch URnetwork on system startup": a registration the user turned on
  // follows this install (its path, and --autostart), and none is made here
  // (StartupRegistration.h).
  RefreshLaunchAtStartup();

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
  //   * the two controller-owned DispatcherQueueTimers (placement save,
  //     balance poll) are stopped — a timer left running keeps a CoreMessaging
  //     callback registered into the teardown;
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
  balance_.Stop();
  tray_.Destroy();
  if (window_) window_.Close();
  window_ = nullptr;
  windowHwnd_ = nullptr;
  // The user's Quit ends the tunnel and the provider too (owner decision,
  // 2026-10-05). Last before the exit, with the window and the tray already
  // gone, because it blocks this thread: it joins SdkHost's own threads, waits
  // out a session pass in flight and makes two pipe calls, each bounded by the
  // pipe's timeout and in the service by StopBudget.h. Nothing on screen is
  // left to freeze while it does. Nothing it throws may keep the app from
  // exiting: the tray, its only way out, is already gone.
  try {
    if (plan.stopService) sdk_.Quit();
  } catch (const std::exception& e) {
    LogError("app: stopping the service on quit failed: {}", e.what());
  }
  if (auto app = Application::Current()) app.Exit();
}

void AppController::OnAuthState(AuthState state, const std::string& error) {
  const bool wasLoggedIn = (authState_ == AuthState::LoggedIn);
  authState_ = state;
  authError_ = error;
  authErrorUndelivered_ = !error.empty();
  UpdateTray();
  // balance store lifecycle follows the session. A repeated LoggedIn push is a
  // device re-registration (guest upgrade): restart the store so the plan
  // reseeds from the new jwt's claims (Guest -> Free).
  if (state == AuthState::LoggedIn) {
    balance_.Start();
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
  lastTunnelStatus_ = status;
  UpdateTray();
  if (windowVisible_ && window_) {
    if (auto self = window_.try_as<winrt::URnetwork::implementation::MainWindow>())
      self->OnTunnelStateChanged(status);
  }
}

void AppController::OnStats(const LiveStats& stats) {
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
    sdk_.RetryRefusedConnect([this] {
      if (auto location = sdk_.SelectedLocation()) {
        sdk_.Connect(*location);
      } else {
        sdk_.ConnectBestAvailable();
      }
    });
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

void AppController::UpdateTray() {
  // v1 maps provide=false; connect reflects the tunnel AND (#27) the aggregate
  // health: the connected icon means "proven working", so Evaluating/Degraded
  // map to the existing not-connected art rather than claiming green. With no
  // evidence at all (window never shown: the stats feed is presentation-scoped)
  // the tunnel's own claim stands, which is exactly the old behaviour.
  using Health = health::State;
  const Health h = trayHealth_.value_or(Health::Connected);
  const bool verified = connected_ && h == Health::Connected;
  tray_.SetState(verified ? TrayState::NoProvideConnect
                          : TrayState::NoProvideNoConnect);
  // "URnetwork — Connected": the app name, then the status. The em dash is
  // punctuation, not text. The health words reuse the connect page's exact
  // strings (same Adv ids), so the tooltip and the status line can never name
  // the same state differently.
  std::wstring tip = Localized("app_name");
  // #41: two states that are NOT "disconnected" even though no tunnel is up, and
  // that a bare app name would render as an idle machine. The icon is shared
  // with the ordinary not-connected art on purpose — a third asset would be a
  // new symbol to learn for a state that lasts until the next click — so the
  // tooltip is what carries the distinction.
  if (!connected_ && lastTunnelStatus_) {
    // ROUTES FIRST, and this row is the tooltip half of the owner's bug A. A
    // machine whose capture routes are still installed with nothing carrying
    // them has no internet — and the app used to render that as "Blocked (kill
    // switch on)" with the kill switch OFF, which is the sentence that got the
    // behaviour reported as a kill switch. After the fix this state is a
    // fraction of a second wide (the stop is in flight), so the word is the
    // transient one rather than an alarm.
    if (lastTunnelStatus_->routes_installed) {
      tray_.SetTooltip(tip + L" — " +
                       pages::AdvW("conn_tray_disconnecting",
                                   L"Disconnecting — traffic is still going "
                                   L"through the tunnel"));
      return;
    }
    if (lastTunnelStatus_->wfp_state == "armed") {
      // The one the owner has to be able to read at a glance: the machine is
      // blocked ON PURPOSE and nothing is leaking, which is a completely
      // different situation from a tunnel that failed open. Gated on "armed"
      // rather than on "not off" — that is the only state the kill switch put
      // this machine in, and the only one the menu's lift item can undo.
      tray_.SetTooltip(tip + L" — " +
                       pages::AdvW("conn_tray_blocked_kill_switch",
                                   L"Blocked (kill switch on)"));
      return;
    }
    if (lastTunnelStatus_->wfp_state != "off") {
      // A policy in force that the kill switch did not put there: a connect
      // attempt in flight, or a connected policy outliving its tunnel. Blocked
      // either way, and the honest word for it is not "kill switch".
      tray_.SetTooltip(tip + L" — " +
                       pages::AdvW("conn_tray_blocked_no_tunnel",
                                   L"Blocked — leak protection is still in "
                                   L"force with no tunnel up"));
      return;
    }
    if (proto::IsFailsafeStop(lastTunnelStatus_->stop_reason)) {
      tray_.SetTooltip(tip + L" — " +
                       pages::AdvW("conn_tray_failsafe_stopped",
                                   L"Disconnected (the tunnel failed)"));
      return;
    }
  }
  if (connected_) {
    switch (h) {
      case Health::Connected:
        tip += L" — " + Localized("connected");
        break;
      case Health::Evaluating:
        tip += L" — " + pages::AdvW("conn_finding_providers", L"Finding providers…");
        break;
      case Health::Degraded:
        tip += L" — " +
               pages::AdvW("conn_degraded", L"Connection degraded — reconnecting");
        break;
      case Health::Connecting:
        tip += L" — " + Localized("connecting_status_indicator");
        break;
      case Health::Failed:
        // the window honesty layer's terminal outcome (track 2): the page
        // shows the reason and a Retry; the tray says the same headline
        tip += L" — " + pages::AdvW("conn_failed", L"Couldn't connect");
        break;
      default:
        // NoService/Disconnected under a tunnel that reports Up: a transient
        // disagreement between the two feeds — claim nothing extra.
        break;
    }
  }
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
    // tray's health reading refreshes at the same instant the window's does.
    // The standing ERROR is replayed only while no presented window has seen it
    // (authErrorUndelivered_): a failure that landed while the window was hidden
    // is delivered once on the next show, never again after that.
    self->OnAuthStateChanged(authState_,
                             authErrorUndelivered_ ? authError_ : std::string());
    authErrorUndelivered_ = false;
    if (lastTunnelStatus_) self->OnTunnelStateChanged(*lastTunnelStatus_);
    OnStats(sdk_.CurrentStats());
    self->OnBalanceChanged(balance_.Current(), balance_.CurrentPoll());
    if (authState_ == AuthState::LoggedIn) balance_.Refresh();
  }
}

// ---- urnetwork:// protocol activation --------------------------------------

void AppController::ServeLaunch(const instance::LaunchRequest& request) {
  const instance::LaunchAction action = instance::ActionFor(request);
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
  // urnetwork://auth?code=<one-time auth code>: the browser sign-in bridge's
  // handoff (LoginPage::OnSignInWithBrowser; the site builds such urls for its
  // own app linking - urnetwork/mmm react/src/auth/urlAuthCode.js). A link
  // must never clobber a session: signed in, it is logged and ignored.
  if (IsAuthDeepLink(url)) {
    if (authState_ == AuthState::LoggedIn || sdk_.IsLoggedIn()) {
      LogWarn("app: auth deep link ignored - a session is signed in");
      return;
    }
    // the code is a credential: its value is never logged
    const std::string code = DeepLinkQueryParam(url, "code");
    if (code.empty()) {
      LogWarn("app: auth deep link carried no code");
      return;
    }
    LogInfo("app: auth deep link routes to the code login");
    if (auto self = window_.try_as<winrt::URnetwork::implementation::MainWindow>()) {
      self->login().SignInWithAuthCode(code);
    }
    return;
  }
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
