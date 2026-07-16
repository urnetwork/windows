// SPDX-License-Identifier: MPL-2.0
#pragma once

// The cppwinrt projection base. It transitively includes the markup MainWindow.xaml.g.h,
// which references MainWindow_base defined HERE - so this is the correct include (do NOT
// swap it for MainWindow.xaml.g.h, which then can't find MainWindow_base). The generated
// InitializeComponent/Connect impls are compiled from the XamlTypeInfo*.g.cpp units that
// App.vcxproj's UrnCompileGeneratedXamlImpl adds to the build, not from this header.
#include "MainWindow.g.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "AuthSheets.h"
#include "BalanceSheets.h"
#include "Protocol.h"
#include "SdkHost.h"
#include "StatsSheets.h"
#include "SubscriptionBalance.h"
#include "TransferChart.h"
#include "UsageBar.h"

namespace winrt::URnetwork::implementation {

struct MainWindow : MainWindowT<MainWindow> {
  MainWindow();
  ~MainWindow();

  // XAML event handlers — sign-in flow (initial → password / create / verify /
  // reset; macOS Authenticate/** parity)
  void OnGetStarted(winrt::Windows::Foundation::IInspectable const&,
                    winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnSignIn(winrt::Windows::Foundation::IInspectable const&,
                winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnPasswordKeyDown(winrt::Windows::Foundation::IInspectable const&,
                         winrt::Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const&);
  // one handler for every back affordance; the button Tag names the step
  void OnLoginBack(winrt::Windows::Foundation::IInspectable const&,
                   winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnForgotPassword(winrt::Windows::Foundation::IInspectable const&,
                        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnSendResetLink(winrt::Windows::Foundation::IInspectable const&,
                       winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnCreateNameChanged(winrt::Windows::Foundation::IInspectable const&,
                           winrt::Microsoft::UI::Xaml::Controls::TextChangedEventArgs const&);
  // the guest upgrade collects the email on the create step (the other modes
  // carry it in from the initial step)
  void OnCreateEmailChanged(winrt::Windows::Foundation::IInspectable const&,
                            winrt::Microsoft::UI::Xaml::Controls::TextChangedEventArgs const&);
  void OnCreatePasswordChanged(winrt::Windows::Foundation::IInspectable const&,
                               winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnTermsChanged(winrt::Windows::Foundation::IInspectable const&,
                      winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnBonusCodeChanged(winrt::Windows::Foundation::IInspectable const&,
                          winrt::Microsoft::UI::Xaml::Controls::TextChangedEventArgs const&);
  void OnCreateNetwork(winrt::Windows::Foundation::IInspectable const&,
                       winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnVerifyCodeChanged(winrt::Windows::Foundation::IInspectable const&,
                           winrt::Microsoft::UI::Xaml::Controls::TextChangedEventArgs const&);
  void OnVerifySubmit(winrt::Windows::Foundation::IInspectable const&,
                      winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnResendCode(winrt::Windows::Foundation::IInspectable const&,
                    winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnUseCode(winrt::Windows::Foundation::IInspectable const&,
                 winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  // guest mode: opens the terms-consent sheet (macOS GuestModeSheet parity)
  void OnTryGuestMode(winrt::Windows::Foundation::IInspectable const&,
                      winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnSignInWithBittensor(winrt::Windows::Foundation::IInspectable const&,
                             winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  // one handler for both Solana wallets; the button Tag carries the provider
  void OnSignInWithSolana(winrt::Windows::Foundation::IInspectable const&,
                          winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);

  // XAML event handlers — home
  void OnConnectToggle(winrt::Windows::Foundation::IInspectable const&,
                       winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnNavSelectionChanged(
      winrt::Microsoft::UI::Xaml::Controls::NavigationView const&,
      winrt::Microsoft::UI::Xaml::Controls::NavigationViewSelectionChangedEventArgs const&);
  void OnManageAppSplitTunnel(winrt::Windows::Foundation::IInspectable const&,
                              winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnSignOut(winrt::Windows::Foundation::IInspectable const&,
                 winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnSaveNetworkName(winrt::Windows::Foundation::IInspectable const&,
                         winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  // upgrade (hosted checkout) + redeem, as ContentDialogs
  void OnOpenUpgrade(winrt::Windows::Foundation::IInspectable const&,
                     winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnOpenRedeem(winrt::Windows::Foundation::IInspectable const&,
                    winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnSendFeedback(winrt::Windows::Foundation::IInspectable const&,
                      winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnWalletAddressChanged(
      winrt::Windows::Foundation::IInspectable const&,
      winrt::Microsoft::UI::Xaml::Controls::TextChangedEventArgs const&);
  void OnConnectWallet(winrt::Windows::Foundation::IInspectable const&,
                       winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);

  // Connect drawer handlers
  void OnConnectionModeChanged(
      winrt::Microsoft::UI::Xaml::Controls::SelectorBar const&,
      winrt::Microsoft::UI::Xaml::Controls::SelectorBarSelectionChangedEventArgs const&);
  void OnFixedIpToggled(winrt::Windows::Foundation::IInspectable const&,
                        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnStrongAnonToggled(winrt::Windows::Foundation::IInspectable const&,
                           winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnBlockerToggled(winrt::Windows::Foundation::IInspectable const&,
                        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnClientStatsCardTapped(winrt::Windows::Foundation::IInspectable const&,
                               winrt::Microsoft::UI::Xaml::Input::TappedRoutedEventArgs const&);
  void OnLocalStatsCardTapped(winrt::Windows::Foundation::IInspectable const&,
                              winrt::Microsoft::UI::Xaml::Input::TappedRoutedEventArgs const&);
  void OnDnsCardTapped(winrt::Windows::Foundation::IInspectable const&,
                       winrt::Microsoft::UI::Xaml::Input::TappedRoutedEventArgs const&);

  // Called by AppController (already marshaled onto the UI thread).
  void OnAuthStateChanged(urnw::AuthState state, std::string const& error);
  void OnTunnelStateChanged(urnw::proto::TunnelStatus const& status);
  void OnStatsChanged(urnw::LiveStats const& stats);
  void OnBalanceChanged(urnw::BalanceSnapshot const& snapshot,
                        urnw::BalancePollState const& poll);

 private:
  // ---- sign-in flow ----
  enum class LoginStep { Initial, Password, Create, Verify, Reset };
  // What the create step submits: a fresh network with email + password, one
  // with the retained wallet auth, or the guest network's upgrade to a full
  // account (Api::upgradeGuest; linux CreateNetworkPage::Mode parity).
  enum class CreateMode { Password, Wallet, GuestUpgrade };

  // every label in the window, from the shared localization store (Localization.h)
  void ApplyStrings();
  void ApplyAuthState(urnw::AuthState state, std::string const& error);
  void ShowLoginStep(LoginStep step);
  void ApplyLoginRouting(urnw::LoginRouting const& routing);
  void EnterCreateStep(std::string const& userAuth, CreateMode mode);
  void EnterVerifyStep(std::string const& userAuth);
  void ShowLoginErrorFor(LoginStep step, winrt::hstring const& message);
  void CheckCreateNameNow();   // debounce elapsed: run the availability check
  void ApplyNameCheck(uint32_t generation, bool ok, bool available);
  void ValidateBonusCodeNow();
  void ApplyBonusValidation(uint32_t generation, bool ok, bool valid, bool capped);
  void ValidateCreateForm();   // gates the Continue button
  void SubmitVerifyCode();

  void SetConnectedUi(bool connected);
  void ApplyStats(urnw::LiveStats const& stats);
  void LoadAccount();
  void LoadBalanceCodes();     // redeemed-codes list (account panel)
  void LoadReferralInfo();     // referral code + totals (usage-bar rows)
  void LoadWallet();
  void LoadLeaderboard();

  // ---- balance / plan (SubscriptionBalanceStore relay) ----
  void ApplyBalance();          // snapshot + poll state -> both plan cards
  void UpdateBalanceWarning();  // insufficient-balance InfoBar gating
  winrt::fire_and_forget ShowUpgradeSheet();
  winrt::fire_and_forget ShowRedeemSheet();

  // ---- guest mode ----
  winrt::fire_and_forget ShowGuestModeSheet();  // terms consent -> LoginAsGuest
  // The plan card's create-account affordance for a guest: the create step in
  // guest-upgrade mode, shown over the login flow while the session stays live.
  void BeginGuestUpgrade();

  // ---- wallet sign in (ur.io/wallet-connect bridge) ----
  void SetWalletSignInEnabled(bool enabled);
  void ApplyWalletSignInResult(urnw::AuthResult const& result);

  // ---- connect wallet (external wallet, by address) ----
  void ApplyWallets(urnet::AccountWalletsList const& wallets);
  void ValidateWalletAddress();  // debounced; the server validates per chain
  void ApplyWalletValidation(std::string const& chain, uint32_t generation, bool valid);
  // `serverError` is the api's own (unlocalizable) message, empty when there is none
  void ApplyWalletConnectResult(bool ok, std::string const& serverError);

  // ---- connect drawer (macOS ConnectActions parity) ----
  void BuildCharts();
  void WireDrawerFeeds();      // SdkHost push handlers -> UI thread -> caches/cards
  void WireCardAffordances();  // hover/pressed feedback on the tappable cards
  void ResyncDrawer();         // seed the caches/cards from SdkHost snapshots
  void SeedConnectControls();  // performance profile + blocker toggle state
  void PushPerformanceSettings();
  // Non-const: reads the ConnectionModeBar / ModeWebItem / ModeStreamingItem x:Name
  // accessors, which C++/WinRT generates as non-const members of the .xaml.g.h base.
  urnw::ConnectionMode SelectedMode();
  void ApplyDnsCard(std::optional<urnet::DnsResolverSettings> const& settings);
  void ApplySplitRuleCount();
  void ApplyBlockerUi(bool on);
  void OnChartTick();
  void AnimateDrawerIn();  // fade + slide-up entrance, staggered across cards
  winrt::fire_and_forget ShowClientContractsSheet();
  winrt::fire_and_forget ShowSplitRulesSheet();
  winrt::fire_and_forget ShowAppRulesSheet();
  winrt::fire_and_forget ShowDnsSheet();

  bool connected_ = false;

  // sign-in flow state (UI thread only)
  LoginStep loginStep_ = LoginStep::Initial;
  std::string loginUserAuth_;      // the echoed user auth driving the current step
  bool discoveringLogin_ = false;  // authLogin discovery in flight
  CreateMode createMode_ = CreateMode::Password;  // what the create step submits
  bool creatingNetwork_ = false;
  bool verifying_ = false;
  bool sendingReset_ = false;
  // create-network name availability (debounced; the generation drops stale checks)
  winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer nameCheckTimer_{nullptr};
  uint32_t nameCheckGeneration_ = 0;
  bool nameChecking_ = false;
  bool nameAvailable_ = false;
  // bonus referral code validation (debounced)
  winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer bonusCheckTimer_{nullptr};
  uint32_t bonusCheckGeneration_ = 0;
  bool bonusValid_ = false;
  bool bonusCapped_ = false;
  // resend-code cooldown (15s, macOS parity)
  winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer resendCooldownTimer_{nullptr};

  // balance / plan state (UI thread only; pushed by the store via AppController)
  urnw::BalanceSnapshot balance_;
  urnw::BalancePollState balancePoll_;
  std::unique_ptr<urnw::UsageBar> accountUsageBar_;
  std::unique_ptr<urnw::UsageBar> drawerUsageBar_;
  int64_t totalReferrals_ = 0;
  std::string referralCode_;
  bool insufficientBalance_ = false;  // last ContractStatus push
  std::shared_ptr<urnw::UpgradeSheet> upgradeSheet_;
  std::shared_ptr<urnw::RedeemCodeSheet> redeemSheet_;
  std::shared_ptr<urnw::GuestModeSheet> guestSheet_;

  // connect-wallet state (UI thread only). The address is validated against each
  // supported chain; the generation drops results from a superseded edit.
  struct WalletValidation {
    bool sol = false;
    bool matic = false;
    bool tao = false;
  };
  winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer walletValidateTimer_{nullptr};
  WalletValidation walletValidation_;
  uint32_t walletValidateGeneration_ = 0;
  std::string walletChain_;          // the chain that accepted the address ("" = none)
  bool connectingWallet_ = false;

  // drawer state (UI thread only)
  std::unique_ptr<urnw::TransferChart> remoteChart_;
  std::unique_ptr<urnw::TransferChart> blockedChart_;
  std::unique_ptr<urnw::TransferChart> localChart_;
  winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer chartTimer_{nullptr};
  uint32_t chartTickCount_ = 0;
  std::vector<urnw::ContractClientRow> contractRows_;
  std::vector<urnw::BlockActionItem> blockActions_;
  std::vector<urnw::SplitRule> splitRules_;
  int64_t allowedCount_ = 0;
  int64_t blockedCount_ = 0;
  std::optional<urnet::DnsResolverSettings> dnsSettings_;
  std::string countryCode_;  // selected location country (dns recommendations)
  std::string countryName_;
  bool updatingControls_ = false;  // guards programmatic toggle/segment updates
  bool drawerAnimated_ = false;    // entrance plays once per window
  bool sheetOpen_ = false;         // only one ContentDialog can show at a time
  std::shared_ptr<urnw::ClientContractsSheet> contractsSheet_;
  std::shared_ptr<urnw::SplitRulesSheet> splitRulesSheet_;
  std::shared_ptr<urnw::AppRulesSheet> appRulesSheet_;
  std::shared_ptr<urnw::DnsEditorSheet> dnsSheet_;
};

}  // namespace winrt::URnetwork::implementation

namespace winrt::URnetwork::factory_implementation {
struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow> {};
}  // namespace winrt::URnetwork::factory_implementation
