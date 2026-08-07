// SPDX-License-Identifier: MPL-2.0
#pragma once

// The cppwinrt projection base. It transitively includes the markup MainWindow.xaml.g.h,
// which references MainWindow_base defined HERE - so this is the correct include (do NOT
// swap it for MainWindow.xaml.g.h, which then can't find MainWindow_base). The generated
// InitializeComponent/Connect impls are compiled from the XamlTypeInfo*.g.cpp units that
// App.vcxproj's UrnCompileGeneratedXamlImpl adds to the build, not from this header.
#include "MainWindow.g.h"

#include <memory>
#include <string>

#include "AccountPage.h"
#include "BalanceSheets.h"
#include "ConnectPage.h"
#include "LoginPage.h"
#include "Protocol.h"
#include "SdkHost.h"
#include "SettingsPage.h"
#include "SubscriptionBalance.h"
#include "UsageBar.h"
#include "WalletPage.h"

namespace winrt::URnetwork::implementation {

// The window itself: navigation between the six destinations, the auth and
// subscription-balance relays that write across more than one of them, the
// shared one-dialog-at-a-time guard, and the login/home root swap.
//
// Every per-destination surface lives in its own unit (LoginPage, ConnectPage,
// AccountPage, WalletPage, SettingsPage). The XAML event handlers stay here
// because the markup binds to them by name; each is a one-line forwarder.
struct MainWindow : MainWindowT<MainWindow> {
  MainWindow();
  ~MainWindow();
  void SetPresentationActive(bool active);

  // ---- page units ----
  // Public because the pages' own UI-thread callbacks resolve the window's weak
  // reference and then reach back for their page.
  urnw::LoginPage& login() { return *login_; }
  urnw::ConnectPage& connect() { return *connect_; }
  urnw::AccountPage& account() { return *account_; }
  urnw::WalletPage& wallet() { return *wallet_; }
  urnw::SettingsPage& settings() { return *settings_; }

  // ---- shared window-level state the pages need ----
  // only one ContentDialog can show at a time
  bool sheetOpen() const { return sheetOpen_; }
  void SetSheetOpen(bool open) { sheetOpen_ = open; }
  // the login flow over the home view, and back
  void ShowLoginRoot();
  void ShowHomeRoot();
  // --preview-ui (see Startup.h): show the signed-in shell at `destination`
  // with no session, so a screen behind sign-in can actually be looked at.
  // Pins the home view — a later auth push must not yank it away.
  void EnterPreviewUi(std::string const& destination);
  // True while --preview-ui is showing the shell with no session. Read by the
  // navigation relay, which must not fire the per-destination API loads: there
  // is no token, so every one of them would be an unauthenticated request to
  // the production API from whatever machine this is running on.
  bool previewUi() const { return previewUi_; }
  // the SubscriptionBalanceStore relay: it paints the account panel AND the
  // connect drawer from one snapshot, so it stays at window level
  void ApplyBalance();
  // last ContractStatus push (ConnectPage::ApplyStats) -> the warning InfoBar
  void SetInsufficientBalance(bool insufficient);

  // XAML event handlers — sign-in flow (initial → password / create / verify /
  // reset; macOS Authenticate/** parity). Forwarded to LoginPage.
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
  // opens android's AuthCodeLoginSheet as a dialog
  void OnUseCode(winrt::Windows::Foundation::IInspectable const&,
                 winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  // guest mode: opens the terms-consent sheet (macOS GuestModeSheet parity).
  // No longer reachable from the login screen - the android login has no guest
  // affordance and guest mode is superseded by the seedphrase system - but the
  // sheet and BeginGuestUpgrade stay for existing guest sessions.
  void OnTryGuestMode(winrt::Windows::Foundation::IInspectable const&,
                      winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnSignInWithBittensor(winrt::Windows::Foundation::IInspectable const&,
                             winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  // one Solana button, as android has; the wallet is chosen in a dialog because
  // the browser bridge needs the provider before it can build its deeplink
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

  // Connect drawer handlers (forwarded to ConnectPage)
  void OnConnectionModeChanged(
      winrt::Microsoft::UI::Xaml::Controls::SelectorBar const&,
      winrt::Microsoft::UI::Xaml::Controls::SelectorBarSelectionChangedEventArgs const&);
  void OnProvideModeChanged(
      winrt::Microsoft::UI::Xaml::Controls::SelectorBar const&,
      winrt::Microsoft::UI::Xaml::Controls::SelectorBarSelectionChangedEventArgs const&);
  void OnFixedIpToggled(winrt::Windows::Foundation::IInspectable const&,
                        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnStrongAnonToggled(winrt::Windows::Foundation::IInspectable const&,
                           winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnPostQuantumToggled(winrt::Windows::Foundation::IInspectable const&,
                            winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnBlockerToggled(winrt::Windows::Foundation::IInspectable const&,
                        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnClientStatsCardClick(winrt::Windows::Foundation::IInspectable const&,
                               winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnLocalStatsCardClick(winrt::Windows::Foundation::IInspectable const&,
                              winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnDnsCardClick(winrt::Windows::Foundation::IInspectable const&,
                       winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnLocationRowClick(winrt::Windows::Foundation::IInspectable const&,
                           winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnPeersLineClick(winrt::Windows::Foundation::IInspectable const&,
                         winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);

  // Called by AppController (already marshaled onto the UI thread).
  void OnAuthStateChanged(urnw::AuthState state, std::string const& error);
  void OnTunnelStateChanged(urnw::proto::TunnelStatus const& status);
  void OnStatsChanged(urnw::LiveStats const& stats);
  void OnBalanceChanged(urnw::BalanceSnapshot const& snapshot,
                        urnw::BalancePollState const& poll);

 private:
  // every label in the window: the window's own chrome and nav, then each page's
  void ApplyStrings();
  // The selected destination's API loads. Called by the navigation relay AND by
  // the auth relay - the selection survives a sign-out, so a new session must
  // re-read whatever page is already on screen (see the definition).
  void LoadCurrentDestination();
  void ApplyAuthState(urnw::AuthState state, std::string const& error);

  // ---- balance / plan (SubscriptionBalanceStore relay) ----
  void UpdateBalanceWarning();  // insufficient-balance InfoBar gating
  winrt::fire_and_forget ShowUpgradeSheet();
  winrt::fire_and_forget ShowRedeemSheet();

  std::unique_ptr<urnw::LoginPage> login_;
  std::unique_ptr<urnw::ConnectPage> connect_;
  std::unique_ptr<urnw::AccountPage> account_;
  std::unique_ptr<urnw::WalletPage> wallet_;
  std::unique_ptr<urnw::SettingsPage> settings_;

  // balance / plan state (UI thread only; pushed by the store via AppController)
  urnw::BalanceSnapshot balance_;
  urnw::BalancePollState balancePoll_;
  std::unique_ptr<urnw::UsageBar> accountUsageBar_;
  std::unique_ptr<urnw::UsageBar> drawerUsageBar_;
  bool insufficientBalance_ = false;  // last ContractStatus push
  std::shared_ptr<urnw::UpgradeSheet> upgradeSheet_;
  std::shared_ptr<urnw::RedeemCodeSheet> redeemSheet_;

  bool sheetOpen_ = false;  // only one ContentDialog can show at a time
  // --preview-ui: the home view is pinned regardless of auth state
  bool previewUi_ = false;
};

}  // namespace winrt::URnetwork::implementation

namespace winrt::URnetwork::factory_implementation {
struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow> {};
}  // namespace winrt::URnetwork::factory_implementation
