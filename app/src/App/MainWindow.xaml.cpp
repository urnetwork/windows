// SPDX-License-Identifier: MPL-2.0
#include "pch.h"

#include "MainWindow.xaml.h"
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

#include <winrt/Windows.Foundation.h>

#include "AppController.h"
#include "PageContext.h"
#include "StatsFormat.h"
#include "UrColors.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace urnw::pages;

namespace winrt::URnetwork::implementation {

MainWindow::MainWindow() {
  InitializeComponent();
  // The window chrome: content extends under the system caption buttons, and
  // AppTitleBar is the DRAG region. Naming a drag region is what separates an
  // actual title bar from a wordmark drawn where one would be. The rest of the
  // native shell (backdrop, caption-button colours, size, placement) needs the
  // HWND and is applied by urnw::shell::ApplyNativeShell from AppController.
  ExtendsContentIntoTitleBar(true);
  SetTitleBar(AppTitleBar());

  // Each destination owns its own translation unit; the window keeps navigation,
  // the auth + balance relays, and the shared sheet guard. Constructed before
  // ApplyStrings because every page paints its own labels from there.
  login_ = std::make_unique<urnw::LoginPage>(*this);
  connect_ = std::make_unique<urnw::ConnectPage>(*this);
  account_ = std::make_unique<urnw::AccountPage>(*this);
  wallet_ = std::make_unique<urnw::WalletPage>(*this);
  settings_ = std::make_unique<urnw::SettingsPage>(*this);

  ApplyStrings();
  connect_->Initialize();  // charts, SDK feeds, card affordances, drawer clock

  // plan + usage cards (account panel and connect drawer)
  accountUsageBar_ = std::make_unique<urnw::UsageBar>(AccountUsageBarHost(),
                                                      AccountUsageLegend());
  drawerUsageBar_ = std::make_unique<urnw::UsageBar>(DrawerUsageBarHost(),
                                                     DrawerUsageLegend());

  // the insufficient-balance warning's action opens the upgrade flow (a guest
  // first creates a full account, like the plan card's affordance)
  {
    Button getPro;
    getPro.Content(LocBox("become_supporter"));
    getPro.Click([weak = get_weak()](auto const&, auto const&) {
      auto self = weak.get();
      if (!self) return;
      if (self->balance_.guest) {
        self->login().BeginGuestUpgrade();
      } else {
        self->ShowUpgradeSheet();
      }
    });
    BalanceWarning().ActionButton(getPro);
  }

  login_->Initialize();   // name / bonus-code debounce + resend cooldown
  wallet_->Initialize();  // wallet-address validation debounce

  // seed the plan cards from the balance store's current snapshot
  balance_ = Balance().Current();
  balancePoll_ = Balance().CurrentPoll();
  ApplyBalance();

  ApplyAuthState(Sdk().IsLoggedIn() ? urnw::AuthState::LoggedIn
                                    : urnw::AuthState::LoggedOut,
                 "");
  if (Sdk().IsLoggedIn() && Sdk().apiReady()) account_->LoadReferralInfo();
  connect_->ResyncDrawer();
}

MainWindow::~MainWindow() {
  // the page dtors stop their own timers; order them explicitly rather than
  // relying on member declaration order
  connect_.reset();
  wallet_.reset();
  login_.reset();
  account_.reset();
  settings_.reset();
}

void MainWindow::SetPresentationActive(bool active) {
  connect_->SetPresentationActive(active);
}

// ---- strings -------------------------------------------------------------
// Every label in the window, from the shared localization store (Localization.h).
// The XAML carries no user-facing text: the ids below are the store's key ids, so
// a missing key renders as the id itself instead of an empty control. Runs once,
// on the UI thread, before the window is shown; the dynamic labels (status,
// counts, verdicts) are seeded here and then owned by the state relays.

void MainWindow::ApplyStrings() {
  Title(Loc("app_name"));
  BrandText().Text(Loc("app_name"));

  // navigation
  ConnectNavItem().Content(LocBox("connect"));
  AccountNavItem().Content(LocBox("account"));
  WalletNavItem().Content(LocBox("wallet"));
  LeaderboardNavItem().Content(LocBox("leaderboard"));
  SupportNavItem().Content(LocBox("support"));
  SettingsNavItem().Content(LocBox("settings"));

  login_->ApplyStrings();
  connect_->ApplyStrings();
  account_->ApplyStrings();
  wallet_->ApplyStrings();
  settings_->ApplyStrings();
}

// ---- login / home roots ----------------------------------------------------

void MainWindow::ShowLoginRoot() {
  HomeNav().Visibility(Visibility::Collapsed);
  LoginRoot().Visibility(Visibility::Visible);
}

void MainWindow::ShowHomeRoot() {
  LoginRoot().Visibility(Visibility::Collapsed);
  HomeNav().Visibility(Visibility::Visible);
}

// ---- navigation ----------------------------------------------------------

void MainWindow::OnNavSelectionChanged(NavigationView const&,
                                       NavigationViewSelectionChangedEventArgs const& args) {
  auto item = args.SelectedItem().try_as<NavigationViewItem>();
  if (!item) return;
  const auto tag = winrt::unbox_value_or<hstring>(item.Tag(), L"connect");

  const bool wasConnectVisible = ConnectView().Visibility() == Visibility::Visible;
  ConnectView().Visibility(tag == L"connect" ? Visibility::Visible : Visibility::Collapsed);
  AccountView().Visibility(tag == L"account" ? Visibility::Visible : Visibility::Collapsed);
  WalletView().Visibility(tag == L"wallet" ? Visibility::Visible : Visibility::Collapsed);
  LeaderboardView().Visibility(tag == L"leaderboard" ? Visibility::Visible : Visibility::Collapsed);
  SupportView().Visibility(tag == L"support" ? Visibility::Visible : Visibility::Collapsed);
  SettingsView().Visibility(tag == L"settings" ? Visibility::Visible : Visibility::Collapsed);

  if (tag == L"connect" && !wasConnectVisible) connect_->AnimateDrawerIn();

  if (!Sdk().apiReady()) return;
  if (tag == L"account") {
    account_->LoadAccount();
    Balance().Refresh();  // macOS AccountRootView onAppear parity
  } else if (tag == L"wallet") {
    wallet_->LoadWallet();
  } else if (tag == L"leaderboard") {
    wallet_->LoadLeaderboard();
  }
}

// ---- balance / plan (SubscriptionBalanceStore relay) -----------------------
// This is the one surface that spans destinations: the same snapshot paints the
// account panel's plan card and the connect drawer's, so it stays at window
// level rather than being duplicated into two pages.

void MainWindow::OnBalanceChanged(urnw::BalanceSnapshot const& snapshot,
                                  urnw::BalancePollState const& poll) {
  balance_ = snapshot;
  balancePoll_ = poll;
  ApplyBalance();
}

void MainWindow::ApplyBalance() {
  // plan value: Guest / Free / Pro (macOS AccountRootView)
  const hstring plan = balance_.guest ? Loc("guest")
                       : balance_.isPro ? Loc("supporter")
                                        : Loc("free");
  AccountPlanValueText().Text(plan);
  DrawerPlanValueText().Text(plan);
  // Pro gold (android theme/Color.kt ProGold). Android spends this colour on the
  // Pro entitlement and on nothing else, so the plan value is the ONE place on
  // these cards that may wear it — until now "Supporter" was a word in the same
  // white as every other label, and the entitlement was invisible. The upgrade
  // affordance beside it stays un-gilded: it is hidden for Pro accounts anyway.
  auto planBrush = balance_.isPro ? urnw::colors::ProGoldBrush()
                                  : urnw::colors::TextBrush();
  AccountPlanValueText().Foreground(planBrush);
  DrawerPlanValueText().Foreground(planBrush);

  // the upgrade affordances show for a signed-in free account; a guest gets a
  // create-account affordance on the plan cards instead (macOS AccountRootView,
  // linux ConnectDrawer), which routes into the guest-upgrade create step
  const auto upgradeVisibility = (!balance_.isPro && !balance_.guest)
                                     ? Visibility::Visible
                                     : Visibility::Collapsed;
  AccountUpgradeButton().Content(
      LocBox(balance_.guest ? "create_an_account" : "upgrade"));
  DrawerGetProButton().Content(
      LocBox(balance_.guest ? "create_an_account" : "get_pro"));
  AccountUpgradeButton().Visibility(balance_.guest ? Visibility::Visible
                                                   : upgradeVisibility);
  DrawerGetProButton().Visibility(balance_.guest ? Visibility::Visible
                                                 : upgradeVisibility);
  // the wallet panel's checkout stays hidden for guests: an account comes first
  UpgradeButton().Visibility(upgradeVisibility);

  // the small ring while the post-checkout confirmation poll runs
  AccountPlanRing().IsActive(balancePoll_.confirming);
  AccountPlanRing().Visibility(balancePoll_.confirming ? Visibility::Visible
                                                       : Visibility::Collapsed);
  DrawerPlanRing().IsActive(balancePoll_.confirming);
  DrawerPlanRing().Visibility(balancePoll_.confirming ? Visibility::Visible
                                                      : Visibility::Collapsed);

  if (accountUsageBar_) {
    accountUsageBar_->Update(balance_.usedByteCount, balance_.pendingByteCount,
                             balance_.availableByteCount);
  }
  if (drawerUsageBar_) {
    drawerUsageBar_->Update(balance_.usedByteCount, balance_.pendingByteCount,
                            balance_.availableByteCount);
  }

  const hstring daily = H(urnw::FormatByteCountCompact(balance_.startBalanceByteCount));
  AccountDailyValue().Text(daily);
  DrawerDailyValue().Text(daily);

  // referral rows: "Total Referrals: N" and "+N*30 GiB/Month"
  const int64_t totalReferrals = account_ ? account_->totalReferrals() : 0;
  const hstring totals = hstring{urnw::Format("total_referrals_lld", totalReferrals)};
  const hstring bonus = hstring{urnw::Format("referral_bonus", totalReferrals * 30)};
  AccountReferralTotals().Text(totals);
  DrawerReferralTotals().Text(totals);
  AccountReferralBonus().Text(bonus);
  DrawerReferralBonus().Text(bonus);

  UpdateBalanceWarning();
  // the open upgrade sheet watches for the plan flip / poll timeout
  if (upgradeSheet_) upgradeSheet_->OnBalance(balance_, balancePoll_);
}

void MainWindow::SetInsufficientBalance(bool insufficient) {
  insufficientBalance_ = insufficient;
  UpdateBalanceWarning();
}

void MainWindow::UpdateBalanceWarning() {
  // macOS ConnectActions: the insufficient-balance CTA shows for a non-Pro
  // account when no confirmation poll is bridging a just-made purchase
  BalanceWarning().IsOpen(insufficientBalance_ && !balance_.isPro &&
                          !balancePoll_.confirming);
}

void MainWindow::OnOpenUpgrade(IInspectable const&, RoutedEventArgs const&) {
  // a guest first creates a full account (the plan card's affordance reads
  // "Create an account" for them); checkout is for signed-in free accounts
  if (balance_.guest) {
    login_->BeginGuestUpgrade();
    return;
  }
  ShowUpgradeSheet();
}

void MainWindow::OnOpenRedeem(IInspectable const&, RoutedEventArgs const&) {
  ShowRedeemSheet();
}

winrt::fire_and_forget MainWindow::ShowUpgradeSheet() {
  if (sheetOpen_) co_return;  // only one ContentDialog can show at a time
  auto self = get_strong();
  self->sheetOpen_ = true;
  try {
    self->upgradeSheet_ = urnw::UpgradeSheet::Create(Content().XamlRoot(), Sdk(), Balance());
    co_await self->upgradeSheet_->Dialog().ShowAsync();
  } catch (...) {
  }
  self->upgradeSheet_.reset();
  self->sheetOpen_ = false;
}

winrt::fire_and_forget MainWindow::ShowRedeemSheet() {
  if (sheetOpen_) co_return;
  auto self = get_strong();
  self->sheetOpen_ = true;
  try {
    auto weak = get_weak();
    self->redeemSheet_ = urnw::RedeemCodeSheet::Create(
        Content().XamlRoot(), Sdk(), [weak] {
          // redeemed: poll the balance up (macOS starts the confirmation poll)
          // and refresh the redeemed-codes list
          urnw::App().balance().StartConfirmationPolling();
          if (auto self = weak.get()) self->account().LoadBalanceCodes();
        });
    co_await self->redeemSheet_->Dialog().ShowAsync();
  } catch (...) {
  }
  self->redeemSheet_.reset();
  self->sheetOpen_ = false;
}

// ---- state relay ---------------------------------------------------------

void MainWindow::OnAuthStateChanged(urnw::AuthState state, std::string const& error) {
  ApplyAuthState(state, error);
}

void MainWindow::ApplyAuthState(urnw::AuthState state, std::string const& error) {
  const bool loggedIn = (state == urnw::AuthState::LoggedIn);
  const bool wasVisible = HomeNav().Visibility() == Visibility::Visible;
  LoginRoot().Visibility(loggedIn ? Visibility::Collapsed : Visibility::Visible);
  HomeNav().Visibility(loggedIn ? Visibility::Visible : Visibility::Collapsed);
  if (!error.empty()) {
    // surface the error on the sign-in step the user is looking at
    login_->ShowErrorOnCurrentStep(H(error));
  }
  if (loggedIn) login_->ClearGuestUpgrade();  // any guest upgrade resolved
  // The network name behind the idle "{name} is ready to connect" copy. Read
  // from the stored jwt once per auth change (ParsedJwt re-parses on every
  // call, and the status line is rewritten on every stats push).
  std::string networkName;
  bool guestMode = false;
  if (loggedIn) {
    if (auto jwt = Sdk().ParsedJwt()) {
      networkName = jwt->NetworkName;
      guestMode = jwt->GuestMode;
    }
  }
  connect_->SetNetworkIdentity(networkName, guestMode);  // re-renders the status
  if (loggedIn && !wasVisible) {
    // the drawer just appeared: refresh its state and play the entrance
    connect_->ResyncDrawer();
    if (Sdk().apiReady()) account_->LoadReferralInfo();  // usage-bar referral rows
    if (ConnectView().Visibility() == Visibility::Visible) connect_->AnimateDrawerIn();
  }
  if (!loggedIn && wasVisible) {
    // signed out: the flow starts over
    login_->ResetToInitialStep();
  }
  if (!loggedIn && !wasVisible && login_->IsGuestUpgrade() && !Sdk().IsLoggedIn()) {
    // the guest session ended under the upgrade form (server-side
    // invalidation): fall back to the start of the sign-in flow
    login_->ClearGuestUpgrade();
    login_->ResetToInitialStep();
  }
}

void MainWindow::OnTunnelStateChanged(urnw::proto::TunnelStatus const& status) {
  connect_->SetConnectedUi(status.state == urnw::proto::TunnelState::Up);
}

void MainWindow::OnStatsChanged(urnw::LiveStats const& stats) {
  connect_->ApplyStats(stats);
}

// ---- XAML event handlers ---------------------------------------------------
// The markup binds to these by name; each forwards to the page that owns the
// surface. Keep them one-liners — the behaviour belongs in the page unit.

void MainWindow::OnGetStarted(IInspectable const& s, RoutedEventArgs const& e) {
  login_->OnGetStarted(s, e);
}
void MainWindow::OnSignIn(IInspectable const& s, RoutedEventArgs const& e) {
  login_->OnSignIn(s, e);
}
void MainWindow::OnPasswordKeyDown(IInspectable const& s,
                                   Input::KeyRoutedEventArgs const& e) {
  login_->OnPasswordKeyDown(s, e);
}
void MainWindow::OnLoginBack(IInspectable const& s, RoutedEventArgs const& e) {
  login_->OnLoginBack(s, e);
}
void MainWindow::OnForgotPassword(IInspectable const& s, RoutedEventArgs const& e) {
  login_->OnForgotPassword(s, e);
}
void MainWindow::OnSendResetLink(IInspectable const& s, RoutedEventArgs const& e) {
  login_->OnSendResetLink(s, e);
}
void MainWindow::OnCreateNameChanged(IInspectable const& s, TextChangedEventArgs const& e) {
  login_->OnCreateNameChanged(s, e);
}
void MainWindow::OnCreateEmailChanged(IInspectable const& s, TextChangedEventArgs const& e) {
  login_->OnCreateEmailChanged(s, e);
}
void MainWindow::OnCreatePasswordChanged(IInspectable const& s, RoutedEventArgs const& e) {
  login_->OnCreatePasswordChanged(s, e);
}
void MainWindow::OnTermsChanged(IInspectable const& s, RoutedEventArgs const& e) {
  login_->OnTermsChanged(s, e);
}
void MainWindow::OnBonusCodeChanged(IInspectable const& s, TextChangedEventArgs const& e) {
  login_->OnBonusCodeChanged(s, e);
}
void MainWindow::OnCreateNetwork(IInspectable const& s, RoutedEventArgs const& e) {
  login_->OnCreateNetwork(s, e);
}
void MainWindow::OnVerifyCodeChanged(IInspectable const& s, TextChangedEventArgs const& e) {
  login_->OnVerifyCodeChanged(s, e);
}
void MainWindow::OnVerifySubmit(IInspectable const& s, RoutedEventArgs const& e) {
  login_->OnVerifySubmit(s, e);
}
void MainWindow::OnResendCode(IInspectable const& s, RoutedEventArgs const& e) {
  login_->OnResendCode(s, e);
}
void MainWindow::OnUseCode(IInspectable const& s, RoutedEventArgs const& e) {
  login_->OnUseCode(s, e);
}
void MainWindow::OnTryGuestMode(IInspectable const& s, RoutedEventArgs const& e) {
  login_->OnTryGuestMode(s, e);
}
void MainWindow::OnSignInWithBittensor(IInspectable const& s, RoutedEventArgs const& e) {
  login_->OnSignInWithBittensor(s, e);
}
void MainWindow::OnSignInWithSolana(IInspectable const& s, RoutedEventArgs const& e) {
  login_->OnSignInWithSolana(s, e);
}

void MainWindow::OnConnectToggle(IInspectable const& s, RoutedEventArgs const& e) {
  connect_->OnConnectToggle(s, e);
}
void MainWindow::OnConnectionModeChanged(SelectorBar const& s,
                                         SelectorBarSelectionChangedEventArgs const& e) {
  connect_->OnConnectionModeChanged(s, e);
}
void MainWindow::OnProvideModeChanged(SelectorBar const& s,
                                      SelectorBarSelectionChangedEventArgs const& e) {
  connect_->OnProvideModeChanged(s, e);
}
void MainWindow::OnFixedIpToggled(IInspectable const& s, RoutedEventArgs const& e) {
  connect_->OnFixedIpToggled(s, e);
}
void MainWindow::OnStrongAnonToggled(IInspectable const& s, RoutedEventArgs const& e) {
  connect_->OnStrongAnonToggled(s, e);
}
void MainWindow::OnPostQuantumToggled(IInspectable const& s, RoutedEventArgs const& e) {
  connect_->OnPostQuantumToggled(s, e);
}
void MainWindow::OnBlockerToggled(IInspectable const& s, RoutedEventArgs const& e) {
  connect_->OnBlockerToggled(s, e);
}
void MainWindow::OnClientStatsCardClick(IInspectable const& s,
                                         RoutedEventArgs const& e) {
  connect_->OnClientStatsCardClick(s, e);
}
void MainWindow::OnLocalStatsCardClick(IInspectable const& s,
                                        RoutedEventArgs const& e) {
  connect_->OnLocalStatsCardClick(s, e);
}
void MainWindow::OnDnsCardClick(IInspectable const& s,
                                 RoutedEventArgs const& e) {
  connect_->OnDnsCardClick(s, e);
}
void MainWindow::OnLocationRowClick(IInspectable const& s,
                                     RoutedEventArgs const& e) {
  connect_->OnLocationRowClick(s, e);
}
void MainWindow::OnPeersLineClick(IInspectable const& s,
                                   RoutedEventArgs const& e) {
  connect_->OnPeersLineClick(s, e);
}

void MainWindow::OnSaveNetworkName(IInspectable const& s, RoutedEventArgs const& e) {
  account_->OnSaveNetworkName(s, e);
}

void MainWindow::OnWalletAddressChanged(IInspectable const& s,
                                        TextChangedEventArgs const& e) {
  wallet_->OnWalletAddressChanged(s, e);
}
void MainWindow::OnConnectWallet(IInspectable const& s, RoutedEventArgs const& e) {
  wallet_->OnConnectWallet(s, e);
}

void MainWindow::OnManageAppSplitTunnel(IInspectable const& s, RoutedEventArgs const& e) {
  settings_->OnManageAppSplitTunnel(s, e);
}
void MainWindow::OnSignOut(IInspectable const& s, RoutedEventArgs const& e) {
  settings_->OnSignOut(s, e);
}
void MainWindow::OnSendFeedback(IInspectable const& s, RoutedEventArgs const& e) {
  settings_->OnSendFeedback(s, e);
}

}  // namespace winrt::URnetwork::implementation
