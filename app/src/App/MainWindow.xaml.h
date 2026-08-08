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
#include <vector>

#include "AccountPage.h"
#include "BalanceSheets.h"
#include "ConnectPage.h"
#include "DeveloperPage.h"
#include "LoginPage.h"
#include "Protocol.h"
#include "SdkHost.h"
#include "SettingsPage.h"
#include "SubscriptionBalance.h"
#include "UrComponents.h"
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
  urnw::DeveloperPage& developer() { return *developer_; }

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
  // ---- the persistent status strip (D4) ----
  // ProtonVPN's bottom line, at window level: it is true whichever destination
  // is on screen, so it cannot live on a page.
  //
  // The CONNECTION half is pushed in by ConnectPage::ApplyConnectStatus rather
  // than recomputed here, on the same principle that already keeps the hero and
  // the balance InfoBar in step: one function derives the state, colour and
  // wording, and every surface that shows them reads that one derivation. A
  // second switch over LiveStats.connectionStatus in this file would be a
  // second place for the strip and the connect screen to disagree.
  void ApplyStatusStripConnection(winrt::hstring const& text,
                                  winrt::Windows::UI::Color dot);
  // The hero canvas's two non-connection states (iOS ConnectButtonView parity).
  // Deliberately the SAME two expressions UpdateBalanceWarning gates the InfoBar
  // on, read from here rather than recomputed in ConnectPage, so the hero and
  // the InfoBar cannot end up disagreeing about the account's state.
  bool balanceConfirming() const { return balancePoll_.confirming; }
  bool balanceBlocked() const { return insufficientBalance_ && !balance_.isPro; }

  // ---- Advanced Mode (D5) ----
  // The window's copy of SdkHost's standing value, kept so a page can ask
  // synchronously while it is BUILDING a row rather than having to have been
  // listening. SdkHost stays the authority (SdkHost::CurrentAdvancedMode); this
  // is seeded from it and then written only by the handler that fans the mode
  // out to the pages, so the two cannot drift.
  bool advancedMode() const { return advancedMode_; }
  // Fan the mode out across the whole app. Not a page — a reading every surface
  // has two of — so this is the ApplyStrings() of Advanced Mode: one function,
  // called once per change, after which every surface has re-read itself.
  void ApplyAdvancedMode(bool on);
  // The Settings toggle's write path. Persists through SdkHost, which publishes
  // back through the handler, which lands in ApplyAdvancedMode — so the toggle
  // never applies the mode itself and there is exactly one apply path.
  void SetAdvancedMode(bool on);

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
  // gates Get started on a non-empty field
  void OnUserAuthChanged(winrt::Windows::Foundation::IInspectable const&,
                         winrt::Microsoft::UI::Xaml::Controls::TextChangedEventArgs const&);
  // Google SSO through the system browser (loopback OAuth + PKCE)
  void OnSignInWithGoogle(winrt::Windows::Foundation::IInspectable const&,
                          winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  // seedphrase sign-in, and the instant (seedphrase-only) account
  void OnSignInWithSeedphrase(winrt::Windows::Foundation::IInspectable const&,
                              winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnSeedphraseChanged(winrt::Windows::Foundation::IInspectable const&,
                           winrt::Microsoft::UI::Xaml::Controls::TextChangedEventArgs const&);
  void OnSeedphraseSubmit(winrt::Windows::Foundation::IInspectable const&,
                          winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnCreateInstantAccount(winrt::Windows::Foundation::IInspectable const&,
                              winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnInstantTermsChanged(winrt::Windows::Foundation::IInspectable const&,
                             winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnCreateInstantSubmit(winrt::Windows::Foundation::IInspectable const&,
                             winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  // the bottom-left "Change Network API" text affordance
  void OnChangeNetworkServer(winrt::Windows::Foundation::IInspectable const&,
                             winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  // the title-bar avatar's menu (identity, create account, share, sign out)
  void OnAccountMenu(winrt::Windows::Foundation::IInspectable const&,
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
  // wallet: Seeker-token multiplier verification. leaderboard: the
  // public/private switch.
  void OnVerifySeeker(winrt::Windows::Foundation::IInspectable const&,
                      winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnLeaderboardPublicToggled(winrt::Windows::Foundation::IInspectable const&,
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
  // D5: the connection inspector's "clear the selection" action.
  void OnInspectorClear(winrt::Windows::Foundation::IInspectable const&,
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

  // ---- the one responsive switch (D4) ----
  // Every destination declares a Narrow and a Wide visual state in markup; this
  // is the only thing that changes between them, and it runs on one SizeChanged
  // at window level rather than seven per-page handlers. AdaptiveTrigger would
  // have been the native mechanism and does not work here - see
  // kit::kWideBreakpointDip for what was measured.
  void ApplyBreakpoint();

  // ---- the persistent status strip ----
  // Built once, from kit::MakeStatusField, so the fields Advanced Mode adds
  // later (egress interface, rpc port, session mode, the raw pre-clamp
  // connection status) cost one more call each and no layout change.
  void BuildStatusStrip();
  // Writes the state field unconditionally. The three callers each decide
  // whether they are ALLOWED to write it (ApplyStatusStripConnection refuses a
  // signed-out push; the preview sample refuses nothing); this just renders.
  void RenderStatusState(winrt::hstring const& text,
                         winrt::Windows::UI::Color dot);
  // Re-renders the fields the strip owns itself (network, provider, traffic)
  // and gates the whole strip on there being a session to describe.
  void ApplyStatusStrip();
  // --preview-ui + URNETWORK_PREVIEW_SAMPLE only: one obviously synthetic
  // snapshot through the SAME apply path, so a POPULATED strip can be looked
  // at from a build with no account. Same two gates and the same log warning
  // as WalletPage's sample rows; touches no network and no stored state.
  void PreviewSampleStatusStrip();

  // ---- balance / plan (SubscriptionBalanceStore relay) ----
  void UpdateBalanceWarning();  // insufficient-balance InfoBar gating
  winrt::fire_and_forget ShowUpgradeSheet();
  winrt::fire_and_forget ShowRedeemSheet();

  std::unique_ptr<urnw::LoginPage> login_;
  std::unique_ptr<urnw::ConnectPage> connect_;
  std::unique_ptr<urnw::AccountPage> account_;
  std::unique_ptr<urnw::WalletPage> wallet_;
  std::unique_ptr<urnw::SettingsPage> settings_;
  std::unique_ptr<urnw::DeveloperPage> developer_;

  // balance / plan state (UI thread only; pushed by the store via AppController)
  urnw::BalanceSnapshot balance_;
  urnw::BalancePollState balancePoll_;
  std::unique_ptr<urnw::UsageBar> accountUsageBar_;
  bool insufficientBalance_ = false;  // last ContractStatus push
  std::shared_ptr<urnw::UpgradeSheet> upgradeSheet_;
  std::shared_ptr<urnw::RedeemCodeSheet> redeemSheet_;

  bool sheetOpen_ = false;  // only one ContentDialog can show at a time
  // --preview-ui: the home view is pinned regardless of auth state
  bool previewUi_ = false;

  // ---- status strip state (UI thread only) ----
  urnw::kit::StatusField statusState_;     // dot + the connection wording
  urnw::kit::StatusField statusNetwork_;
  urnw::kit::StatusField statusProvider_;
  urnw::kit::StatusField statusTraffic_;
  // ---- the strip's ADVANCED density (D5) ----
  // Four more fields, four more MakeStatusField calls and no layout change,
  // which is exactly what the strip was built as a horizontal panel of fields
  // for. Present only while Advanced Mode is on; BuildStatusStrip rebuilds.
  urnw::kit::StatusField statusMode_;      // session mode: tunnel / rpc-only
  urnw::kit::StatusField statusRoutes_;    // are routes+DNS actually installed
  urnw::kit::StatusField statusRpcPort_;   // the service rpc endpoint
  urnw::kit::StatusField statusRaw_;       // the PRE-CLAMP connection status
  // The last TunnelStatus, for the three advanced fields that read it. Cached
  // because the strip is rebuilt on a mode change, which is not a tunnel event:
  // without this a rebuild would show three blanks until the service next
  // happened to push, and on a settled session that is never.
  std::string statusRpcHostPort_;
  urnw::proto::StartMode statusSessionMode_ = urnw::proto::StartMode::Tunnel;
  bool statusRoutesInstalled_ = false;
  // LiveStats' pre-clamp reading, likewise cached across a rebuild.
  std::string statusRawConnection_;
  // What RenderStatusState last drew. The state field is written by callers that
  // are ALLOWED to write it (and the preview sample pins itself), so a rebuild
  // that did not replay it would blank the one field the strip is named after
  // until the next push — which under a pinned preview sample never comes. Same
  // lesson as sessionFailure_, one layer up.
  winrt::hstring statusStateText_;
  winrt::Windows::UI::Color statusStateDot_{};
  // Everything after the state field, hidden as a group when there is no
  // session: three captions over three blanks says less than one honest line.
  std::vector<winrt::Microsoft::UI::Xaml::UIElement> statusSessionParts_;
  std::string statusNetworkName_;
  bool statusGuest_ = false;
  bool statusSignedIn_ = false;
  bool statusConnected_ = false;
  std::string statusLocationName_;
  int64_t statusDownBps_ = 0;
  int64_t statusUpBps_ = 0;
  // the sample above owns the strip's values: the real relays keep running
  // (there is no session, so they push emptiness) and must not overwrite it
  bool statusSamplePinned_ = false;
  // last state ApplyBreakpoint drove, so a resize that does not cross the
  // breakpoint costs nothing (SizeChanged fires on every pixel of a drag)
  // ---- Advanced Mode (D5) ----
  // Seeded from SdkHost::CurrentAdvancedMode() in the ctor and written only by
  // ApplyAdvancedMode. See the accessor above.
  bool advancedMode_ = false;

  bool wideLayout_ = false;
  // Home's second breakpoint (kUltraWideDip): the third column. Tracked
  // separately so a drag across 1800 re-runs the layout even though `wide` did
  // not change - the early-out has to test every state it applies, not one.
  bool ultraLayout_ = false;
  bool breakpointApplied_ = false;
};

}  // namespace winrt::URnetwork::implementation

namespace winrt::URnetwork::factory_implementation {
struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow> {};
}  // namespace winrt::URnetwork::factory_implementation
