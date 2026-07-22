// SPDX-License-Identifier: MPL-2.0
#include "pch.h"

#include "MainWindow.xaml.h"
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

#include <winrt/Windows.Storage.Pickers.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Microsoft.UI.Interop.h>
#include <winrt/Microsoft.UI.Xaml.Media.Animation.h>
#include <winrt/Microsoft.UI.Xaml.Shapes.h>  // PeerDot Ellipse.Fill

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>

#include "AppController.h"
#include "Localization.h"
#include "StatsFormat.h"
#include "Strings.h"
#include "UrColors.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;

namespace winrt::URnetwork::implementation {

namespace {
urnw::SdkHost& Sdk() { return urnw::App().sdk(); }
urnw::SubscriptionBalanceStore& Balance() { return urnw::App().balance(); }
hstring H(std::string const& s) { return winrt::to_hstring(s); }

// A UI string from the shared localization store, by key id. Every user-facing
// string in this window comes through Loc/LocBox, urnw::Format or urnw::Plural —
// there are no literals (see MainWindow.xaml).
hstring Loc(std::string_view key) { return hstring{urnw::Localized(key)}; }
IInspectable LocBox(std::string_view key) { return winrt::box_value(Loc(key)); }

std::string TrimWhitespace(std::string const& value) {
  const auto begin = value.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) return "";
  const auto end = value.find_last_not_of(" \t\r\n");
  return value.substr(begin, end - begin + 1);
}

// a user auth is an email or a phone number (light shape check; the server is
// the real validator — macOS ValidationUtils parity in spirit)
bool LooksLikeUserAuth(std::string const& value) {
  if (value.find('@') != std::string::npos) return value.size() >= 3;
  size_t digits = 0;
  for (char c : value) {
    if (c >= '0' && c <= '9') ++digits;
  }
  return digits >= 7;
}

// network names must be 6 characters or more (macOS CreateNetworkViewModel)
constexpr size_t kMinNetworkNameLength = 6;
// passwords must be at least 12 characters (macOS CreateNetworkViewModel)
constexpr size_t kMinPasswordLength = 12;
// a verification code is 6 digits (macOS CreateNetworkVerifyViewModel)
constexpr size_t kVerifyCodeLength = 6;

// the YYYY-MM-DD prefix of an ISO timestamp (redeemed-codes list dates)
std::string IsoDate(std::string const& isoTime) {
  return isoTime.size() >= 10 ? isoTime.substr(0, 10) : isoTime;
}

// first 3 ... last 3 of a redeemed code's secret (macOS TransferBalanceCodesView)
std::string MaskSecret(std::string const& secret) {
  constexpr size_t keep = 3;
  if (secret.size() <= keep * 2) return std::string(secret.size(), '.');
  return secret.substr(0, keep) + "..." + secret.substr(secret.size() - keep);
}

// Display name of an account wallet's blockchain (macOS/android parity). Chain
// names are product names: the store carries them untranslated.
std::wstring ChainName(std::string const& blockchain) {
  if (blockchain == urnet::SOL) return urnw::Localized("solana");
  if (blockchain == urnet::TAO) return urnw::Localized("bittensor");
  if (blockchain == urnet::MATIC) return urnw::Localized("polygon");
  return urnw::Widen(blockchain);  // an unknown chain: its raw id
}

// country-code case folding for the dns pill: the sdk recommendation/color
// lookups are keyed on the lowercase code; ToUpper is the display fallback when
// the connected location has no country name (StatsSheets parity).
std::string ToLower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}
std::string ToUpper(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  return s;
}

// "AABBCC" / "#AABBCC" / "AARRGGBB" -> Color (fallback muted gray). Mirrors the
// StatsSheets helper; used to fill the dns pill's country-color dot.
winrt::Windows::UI::Color ColorFromHex(std::string hex) {
  if (!hex.empty() && hex[0] == '#') hex = hex.substr(1);
  auto parse = [&](size_t offset) {
    return static_cast<uint8_t>(std::stoul(hex.substr(offset, 2), nullptr, 16));
  };
  try {
    if (hex.size() == 6) return {255, parse(0), parse(2), parse(4)};
    if (hex.size() == 8) return {parse(0), parse(2), parse(4), parse(6)};
  } catch (...) {
  }
  return urnw::colors::kTextMuted;
}

// Two applied dns snapshots are equivalent when every resolver flag and every
// server list matches; an absent list and an empty list are the same. This is
// the same field-for-field comparison DnsEditorSheet makes on its Draft and the
// iOS DnsSettings ==, so the pill agrees with the editor's recommendation panel.
bool DnsSettingsEquivalent(urnet::DnsResolverSettings const& a,
                           urnet::DnsResolverSettings const& b) {
  auto list = [](std::optional<urnet::StringList> const& v) {
    return v ? *v : urnet::StringList{};
  };
  return a.EnableRemoteDoh == b.EnableRemoteDoh && a.EnableLocalDoh == b.EnableLocalDoh &&
         a.EnableRemoteDns == b.EnableRemoteDns && a.EnableLocalDns == b.EnableLocalDns &&
         a.EnableFallback == b.EnableFallback &&
         list(a.RemoteDohUrlsIpv4) == list(b.RemoteDohUrlsIpv4) &&
         list(a.RemoteDohUrlsIpv6) == list(b.RemoteDohUrlsIpv6) &&
         list(a.LocalDohUrlsIpv4) == list(b.LocalDohUrlsIpv4) &&
         list(a.LocalDohUrlsIpv6) == list(b.LocalDohUrlsIpv6) &&
         list(a.RemoteDnsIpv4) == list(b.RemoteDnsIpv4) &&
         list(a.RemoteDnsIpv6) == list(b.RemoteDnsIpv6) &&
         list(a.LocalDnsIpv4) == list(b.LocalDnsIpv4) &&
         list(a.LocalDnsIpv6) == list(b.LocalDnsIpv6);
}
}  // namespace

MainWindow::MainWindow() {
  InitializeComponent();
  ExtendsContentIntoTitleBar(true);
  ApplyStrings();
  BuildCharts();
  WireDrawerFeeds();
  WireCardAffordances();

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
        self->BeginGuestUpgrade();
      } else {
        self->ShowUpgradeSheet();
      }
    });
    BalanceWarning().ActionButton(getPro);
  }

  // shared drawer clock: ~10 fps chart redraw, plus 1s relative-time refresh
  chartTimer_ = DispatcherQueue().CreateTimer();
  chartTimer_.Interval(std::chrono::milliseconds(100));
  chartTimer_.Tick([weak = get_weak()](auto const&, auto const&) {
    if (auto self = weak.get()) self->OnChartTick();
  });
  chartTimer_.Start();

  // debounce the connect-wallet address validation while typing (apple parity):
  // each keystroke restarts the window, and only the pause validates.
  walletValidateTimer_ = DispatcherQueue().CreateTimer();
  walletValidateTimer_.Interval(std::chrono::milliseconds(300));
  walletValidateTimer_.IsRepeating(false);
  walletValidateTimer_.Tick([weak = get_weak()](auto const&, auto const&) {
    if (auto self = weak.get()) self->ValidateWalletAddress();
  });

  // debounce the sign-up network-name availability check (macOS: 250ms)
  nameCheckTimer_ = DispatcherQueue().CreateTimer();
  nameCheckTimer_.Interval(std::chrono::milliseconds(300));
  nameCheckTimer_.IsRepeating(false);
  nameCheckTimer_.Tick([weak = get_weak()](auto const&, auto const&) {
    if (auto self = weak.get()) self->CheckCreateNameNow();
  });

  // debounce the bonus referral code validation
  bonusCheckTimer_ = DispatcherQueue().CreateTimer();
  bonusCheckTimer_.Interval(std::chrono::milliseconds(400));
  bonusCheckTimer_.IsRepeating(false);
  bonusCheckTimer_.Tick([weak = get_weak()](auto const&, auto const&) {
    if (auto self = weak.get()) self->ValidateBonusCodeNow();
  });

  // resend-code cooldown: re-enable the resend button after 15s (macOS parity)
  resendCooldownTimer_ = DispatcherQueue().CreateTimer();
  resendCooldownTimer_.Interval(std::chrono::seconds(15));
  resendCooldownTimer_.IsRepeating(false);
  resendCooldownTimer_.Tick([weak = get_weak()](auto const&, auto const&) {
    if (auto self = weak.get()) self->ResendCodeButton().IsEnabled(true);
  });

  // seed the plan cards from the balance store's current snapshot
  balance_ = Balance().Current();
  balancePoll_ = Balance().CurrentPoll();
  ApplyBalance();

  ApplyAuthState(Sdk().IsLoggedIn() ? urnw::AuthState::LoggedIn
                                    : urnw::AuthState::LoggedOut,
                 "");
  if (Sdk().IsLoggedIn() && Sdk().apiReady()) LoadReferralInfo();
  ResyncDrawer();
}

MainWindow::~MainWindow() {
  if (chartTimer_) chartTimer_.Stop();
  if (walletValidateTimer_) walletValidateTimer_.Stop();
  if (nameCheckTimer_) nameCheckTimer_.Stop();
  if (bonusCheckTimer_) bonusCheckTimer_.Stop();
  if (resendCooldownTimer_) resendCooldownTimer_.Stop();
}

// ---- strings -------------------------------------------------------------
// Every label in the window, from the shared localization store (Localization.h).
// The XAML carries no user-facing text: the ids below are the store's key ids, so
// a missing key renders as the id itself instead of an empty control. Runs once,
// on the UI thread, before the window is shown; the dynamic labels (status,
// counts, verdicts) are seeded here and then owned by the state relays below.

void MainWindow::ApplyStrings() {
  Title(Loc("app_name"));
  BrandText().Text(Loc("app_name"));

  // sign in — initial (account discovery)
  SignInHeading().Text(Loc("sign_in"));
  EmailBox().Header(LocBox("user_auth_label"));
  EmailBox().PlaceholderText(Loc("user_auth_input_placeholder"));
  GetStartedButton().Content(LocBox("get_started"));
  WalletSignInLabel().Text(Loc("or_use_a_wallet"));
  TauGlyph().Text(Loc("symbol_tao"));
  BittensorSignInText().Text(Loc("bittensor_sign_in"));
  SolanaSignInLabel().Text(Loc("solana_sign_in"));
  PhantomSignInButton().Content(LocBox("phantom"));
  SolflareSignInButton().Content(LocBox("solflare"));
  // SdkHost::LoginWithCode is the auth-code login the other platforms ship
  AuthCodeLabel().Text(Loc("auth_code_login_sheet_header"));
  CodeBox().PlaceholderText(Loc("auth_code"));
  CodeButton().Content(LocBox("auth_code_login_button_text"));
  GuestModeButton().Content(LocBox("try_guest_mode"));

  // sign in — password step
  PasswordBackButton().Content(LocBox("back"));
  PasswordHeading().Text(Loc("its_nice_to_see_you_again"));
  PasswordBox().Header(LocBox("password_label"));
  SignInButton().Content(LocBox("sign_in"));
  ForgotPasswordLabel().Text(Loc("forgot_password"));
  ForgotResetButton().Content(LocBox("reset_it"));

  // sign in — create network step
  CreateBackButton().Content(LocBox("back"));
  CreateHeading().Text(Loc("join_urnetwork"));
  WalletCreateNote().Text(Loc("wallet_needs_network"));
  // guest upgrade: status + call to action, two store sentences on two lines
  GuestUpgradeNote().Text(hstring{urnw::Localized("in_guest_mode") + L"\n" +
                                  urnw::Localized("start_earning_join")});
  CreateEmailBox().Header(LocBox("user_auth_label"));
  CreateEmailBox().PlaceholderText(Loc("user_auth_input_placeholder"));
  CreateNameBox().Header(LocBox("network_name_label"));
  CreateNameBox().PlaceholderText(Loc("enter_a_name_for_your_network"));
  CreateNameStatusText().Text(Loc("network_name_length_error"));
  CreatePasswordBox().Header(LocBox("password_label"));
  CreatePasswordHint().Text(Loc("password_must_be_at_least_12_characters_long"));
  // tappable terms / privacy links inside the checkbox label
  urnw::SetTermsMarkerText(TermsText(), urnw::Localized("terms_checkbox"), 12);
  BonusCodeBox().Header(LocBox("bonus_referral_code_label"));
  BonusCodeBox().PlaceholderText(Loc("enter_a_bonus_referral_code"));
  CreateButton().Content(LocBox("continue_txt"));

  // sign in — verify step
  VerifyBackButton().Content(LocBox("back"));
  VerifyHeading().Text(Loc("login_verify_header"));
  VerifyExplanationText().Text(Loc("verify_explanation"));
  VerifyCodeBox().Header(LocBox("verify_input_label"));
  VerifyButton().Content(LocBox("verify"));
  DontSeeItLabel().Text(Loc("dont_see_it"));
  ResendCodeButton().Content(LocBox("resend_verify_code"));

  // sign in — password reset step
  ResetBackButton().Content(LocBox("back"));
  ResetHeading().Text(Loc("forgot_password"));
  ResetSpamNote().Text(Loc("you_may_need_to_your_check_spam_folder_or"));
  SendResetButton().Content(LocBox("send_reset_link_2"));

  // navigation
  ConnectNavItem().Content(LocBox("connect"));
  AccountNavItem().Content(LocBox("account"));
  WalletNavItem().Content(LocBox("wallet"));
  LeaderboardNavItem().Content(LocBox("leaderboard"));
  SupportNavItem().Content(LocBox("support"));
  SettingsNavItem().Content(LocBox("settings"));

  // connect drawer
  StatusText().Text(Loc("disconnected"));
  SelectedProviderLabel().Text(Loc("selected_provider"));
  LocationText().Text(Loc("best_available_provider"));
  ApplyPeerCount(std::nullopt);   // seed the peers status line ("0 peers" + dot)
  ConnectButton().Content(LocBox("connect"));
  BalanceWarning().Title(Loc("insufficient_balance"));
  BalanceWarning().Message(Loc("insufficient_balance_message"));
  ConnectOptionsLabel().Text(Loc("connect_options"));
  ModeAutoItem().Text(Loc("window_type_auto"));
  ModeWebItem().Text(Loc("window_type_quality"));
  ModeStreamingItem().Text(Loc("window_type_speed"));
  ProvideModeLabel().Text(Loc("provide_mode"));
  ProvideAutoItem().Text(Loc("auto"));
  ProvideAlwaysItem().Text(Loc("always"));
  ProvideNetworkItem().Text(Loc("network"));
  ProvideNeverItem().Text(Loc("never"));
  FixedIpLabel().Text(Loc("fixed_ip"));
  StrongAnonLabel().Text(Loc("strong_anonymization"));
  PostQuantumLabel().Text(Loc("post_quantum_encryption"));
  ClientStatsLabel().Text(Loc("client_statistics"));
  LocalStatsLabel().Text(Loc("local_statistics"));
  DnsCardLabel().Text(Loc("custom_dns"));
  DohLabel().Text(Loc("dns_over_https"));
  UdnsLabel().Text(Loc("unencrypted_dns"));
  LdnsLabel().Text(Loc("local_dns"));
  FallbackLabel().Text(Loc("local_dns_fallback"));
  DohState().Text(Loc("off"));
  UdnsState().Text(Loc("off"));
  LdnsState().Text(Loc("off"));
  FallbackState().Text(Loc("off"));
  DnsUnavailableText().Text(Loc("dns_settings_unavailable"));
  BlockerLabel().Text(Loc("block_ads_and_trackers"));
  // drawer plan + usage card (macOS ConnectActions parity)
  DrawerPlanLabel().Text(Loc("plan"));
  DrawerPlanValueText().Text(Loc("free"));
  DrawerGetProButton().Content(LocBox("get_pro"));
  DrawerDailyLabel().Text(Loc("daily_data_balance_label"));

  // account — plan + usage card, redeemed codes, profile, referrals
  AccountPlanLabel().Text(Loc("plan"));
  AccountPlanValueText().Text(Loc("free"));
  AccountUpgradeButton().Content(LocBox("upgrade"));
  AccountDailyLabel().Text(Loc("daily_data_balance_label"));
  RedeemRowText().Text(Loc("redeem_balance_code"));
  BalanceCodesLabel().Text(Loc("balance_codes_title"));
  BalanceCodesEmptyText().Text(Loc("no_balance_codes_found"));
  AccountHeading().Text(Loc("account"));
  NetworkNameBox().Header(LocBox("network_name_label"));
  SaveNameButton().Content(LocBox("save"));
  ReferralsHeading().Text(Loc("referrals"));
  RoyaltyText().Text(Loc("referral_royalty"));

  // wallet
  PlanHeading().Text(Loc("plan"));
  UpgradeButton().Content(LocBox("upgrade_with_stripe"));
  PayoutWalletsHeading().Text(Loc("payout_wallets"));
  ConnectWalletHeading().Text(Loc("connect_a_wallet"));
  // supported chains, then the bittensor caveat: two store sentences rather than a
  // third near-duplicate of both
  ConnectWalletChainsText().Text(
      hstring{urnw::Localized("connect_external_wallet_supported_chains") + L" " +
              urnw::Localized("bittensor_wallet_future_use")});
  WalletAddressBox().PlaceholderText(Loc("enter_wallet_address"));
  ConnectWalletButton().Content(LocBox("connect"));

  // leaderboard + support
  LeaderboardHeading().Text(Loc("leaderboard"));
  FeedbackHeading().Text(Loc("feedback"));
  FeedbackRating().Caption(Loc("how_are_we_doing"));
  FeedbackText().Header(LocBox("anything_else"));
  SendFeedbackButton().Content(LocBox("send"));

  // settings
  SplitTunnelHeading().Text(Loc("app_split_rules"));
  SplitTunnelDescription().Text(Loc("apps_listed_bypass_vpn"));
  ManageAppSplitButton().Content(LocBox("manage_apps"));
  SettingsAccountHeading().Text(Loc("account"));
  SignOutButton().Content(LocBox("sign_out"));
  ProtocolLink().Content(LocBox("uses_ur_protocol"));
}

// ---- sign-in flow ----------------------------------------------------------
// macOS Authenticate/** parity: the initial step discovers the account for a
// user auth (authLogin), then routes to the password, create-network or verify
// step. The wallet buttons and the auth-code login stay on the initial step.

void MainWindow::ShowLoginStep(LoginStep step) {
  loginStep_ = step;
  LoginPanel().Visibility(step == LoginStep::Initial ? Visibility::Visible
                                                     : Visibility::Collapsed);
  PasswordPanel().Visibility(step == LoginStep::Password ? Visibility::Visible
                                                         : Visibility::Collapsed);
  CreatePanel().Visibility(step == LoginStep::Create ? Visibility::Visible
                                                     : Visibility::Collapsed);
  VerifyPanel().Visibility(step == LoginStep::Verify ? Visibility::Visible
                                                     : Visibility::Collapsed);
  ResetPanel().Visibility(step == LoginStep::Reset ? Visibility::Visible
                                                   : Visibility::Collapsed);
}

void MainWindow::ShowLoginErrorFor(LoginStep step, hstring const& message) {
  auto show = [&message](InfoBar const& bar) {
    bar.Severity(InfoBarSeverity::Error);
    bar.Message(message);
    bar.IsOpen(true);
  };
  switch (step) {
    case LoginStep::Password: show(PasswordError()); break;
    case LoginStep::Create: show(CreateError()); break;
    case LoginStep::Verify: show(VerifyInfo()); break;
    case LoginStep::Reset: show(ResetInfo()); break;
    default: show(LoginError()); break;
  }
}

void MainWindow::OnGetStarted(IInspectable const&, RoutedEventArgs const&) {
  const std::string userAuth = TrimWhitespace(urnw::Narrow(EmailBox().Text().c_str()));
  if (discoveringLogin_ || !LooksLikeUserAuth(userAuth)) return;
  discoveringLogin_ = true;
  GetStartedButton().IsEnabled(false);
  LoginError().IsOpen(false);

  auto queue = DispatcherQueue();
  auto weak = get_weak();
  Sdk().StartLogin(userAuth, [queue, weak](urnw::LoginRouting routing) {
    queue.TryEnqueue([weak, routing = std::move(routing)] {
      if (auto self = weak.get()) self->ApplyLoginRouting(routing);
    });
  });
}

void MainWindow::ApplyLoginRouting(urnw::LoginRouting const& routing) {
  discoveringLogin_ = false;
  GetStartedButton().IsEnabled(true);
  switch (routing.route) {
    case urnw::LoginRoute::Login:
      // the auth state relay swaps the panel for the home view
      break;
    case urnw::LoginRoute::Password:
      loginUserAuth_ = routing.userAuth;
      PasswordEmailText().Text(H(loginUserAuth_));
      PasswordBox().Password(L"");
      PasswordError().IsOpen(false);
      ShowLoginStep(LoginStep::Password);
      PasswordBox().Focus(FocusState::Programmatic);
      break;
    case urnw::LoginRoute::Create:
      EnterCreateStep(routing.userAuth, CreateMode::Password);
      break;
    case urnw::LoginRoute::Verify:
      EnterVerifyStep(routing.userAuth);
      break;
    case urnw::LoginRoute::IncorrectAuth:
      // the account exists under another sign-in method
      ShowLoginErrorFor(LoginStep::Initial,
                        hstring{urnw::Format("login_error_auth_allowed",
                                             urnw::Widen(routing.authAllowed))});
      break;
    case urnw::LoginRoute::Error:
    default:
      ShowLoginErrorFor(LoginStep::Initial, routing.error.empty()
                                                ? Loc("there_was_an_error_logging_in")
                                                : H(routing.error));
      break;
  }
}

void MainWindow::OnLoginBack(IInspectable const& sender, RoutedEventArgs const&) {
  // backing out of the guest-upgrade create/verify step returns to the home
  // view: the guest session never went away
  if (createMode_ == CreateMode::GuestUpgrade && Sdk().IsLoggedIn()) {
    createMode_ = CreateMode::Password;
    ShowLoginStep(LoginStep::Initial);  // leave the flow ready for a real sign-out
    LoginRoot().Visibility(Visibility::Collapsed);
    HomeNav().Visibility(Visibility::Visible);
    return;
  }
  IInspectable tagValue{nullptr};
  if (auto element = sender.try_as<FrameworkElement>()) tagValue = element.Tag();
  const auto tag = winrt::unbox_value_or<hstring>(tagValue, L"initial");
  ShowLoginStep(tag == L"password" && !loginUserAuth_.empty() ? LoginStep::Password
                                                              : LoginStep::Initial);
}

void MainWindow::OnPasswordKeyDown(IInspectable const&,
                                   Input::KeyRoutedEventArgs const& args) {
  if (args.Key() == winrt::Windows::System::VirtualKey::Enter) {
    OnSignIn(nullptr, nullptr);
  }
}

void MainWindow::OnSignIn(IInspectable const&, RoutedEventArgs const&) {
  const std::string password = urnw::Narrow(PasswordBox().Password().c_str());
  if (loginUserAuth_.empty() || password.empty()) return;
  PasswordError().IsOpen(false);
  SignInButton().IsEnabled(false);

  auto queue = DispatcherQueue();
  auto weak = get_weak();
  Sdk().LoginWithPassword(loginUserAuth_, password, [queue, weak](urnw::AuthResult r) {
    queue.TryEnqueue([weak, r] {
      auto self = weak.get();
      if (!self) return;
      self->SignInButton().IsEnabled(true);
      if (r.verification_required) {
        // the account still needs its code — route into the verify step
        // instead of dead-ending on an info bar
        self->EnterVerifyStep(self->loginUserAuth_);
        self->VerifyInfo().Severity(InfoBarSeverity::Informational);
        self->VerifyInfo().Message(Loc("verification_code_sent"));
        self->VerifyInfo().IsOpen(true);
      } else if (!r.ok && !r.error.empty()) {
        self->ShowLoginErrorFor(LoginStep::Password, H(r.error));
      }
    });
  });
}

void MainWindow::OnForgotPassword(IInspectable const&, RoutedEventArgs const&) {
  if (loginUserAuth_.empty()) return;
  ResetEmailText().Text(H(loginUserAuth_));
  ResetInfo().IsOpen(false);
  SendResetButton().IsEnabled(true);
  ShowLoginStep(LoginStep::Reset);
}

void MainWindow::OnSendResetLink(IInspectable const&, RoutedEventArgs const&) {
  if (sendingReset_ || loginUserAuth_.empty()) return;
  sendingReset_ = true;
  SendResetButton().IsEnabled(false);
  ResetInfo().IsOpen(false);

  auto queue = DispatcherQueue();
  auto weak = get_weak();
  Sdk().SendPasswordResetLink(loginUserAuth_, [queue, weak](bool ok) {
    queue.TryEnqueue([weak, ok] {
      auto self = weak.get();
      if (!self) return;
      self->sendingReset_ = false;
      self->SendResetButton().IsEnabled(true);
      self->ResetInfo().Severity(ok ? InfoBarSeverity::Success
                                    : InfoBarSeverity::Error);
      // "Reset link sent to" + the address it went to (the address is data)
      self->ResetInfo().Message(ok ? hstring{urnw::Localized("reset_link_sent_to") +
                                             L" " + urnw::Widen(self->loginUserAuth_)}
                                   : Loc("something_went_wrong"));
      self->ResetInfo().IsOpen(true);
    });
  });
}

// ---- create network (sign-up) ----

void MainWindow::EnterCreateStep(std::string const& userAuth, CreateMode mode) {
  loginUserAuth_ = userAuth;
  createMode_ = mode;
  const bool walletMode = (mode == CreateMode::Wallet);
  const bool guestUpgrade = (mode == CreateMode::GuestUpgrade);
  // wallet mode: the wallet signature is the credential — name + terms only.
  // guest upgrade: the guest enters an email here (nothing was carried in).
  WalletCreateNote().Visibility(walletMode ? Visibility::Visible : Visibility::Collapsed);
  GuestUpgradeNote().Visibility(guestUpgrade ? Visibility::Visible : Visibility::Collapsed);
  CreateEmailText().Text(H(userAuth));
  CreateEmailText().Visibility(mode == CreateMode::Password ? Visibility::Visible
                                                            : Visibility::Collapsed);
  CreateEmailBox().Text(L"");
  CreateEmailBox().Visibility(guestUpgrade ? Visibility::Visible : Visibility::Collapsed);
  CreatePasswordBox().Visibility(walletMode ? Visibility::Collapsed : Visibility::Visible);
  CreatePasswordHint().Visibility(walletMode ? Visibility::Collapsed : Visibility::Visible);
  // UpgradeGuestArgs carries no referral code (the bonus only applies to a
  // fresh create — the sdk/api shape, not a UI choice): hide the bonus row
  BonusCodeBox().Visibility(guestUpgrade ? Visibility::Collapsed : Visibility::Visible);
  BonusStatusText().Visibility(guestUpgrade ? Visibility::Collapsed : Visibility::Visible);

  CreateNameBox().Text(L"");
  CreatePasswordBox().Password(L"");
  TermsCheck().IsChecked(false);
  BonusCodeBox().Text(L"");
  BonusStatusText().Text(L"");
  CreateNameStatusText().Text(Loc("network_name_length_error"));
  CreateNameStatusText().Foreground(urnw::colors::MutedBrush());
  nameAvailable_ = false;
  nameChecking_ = false;
  ++nameCheckGeneration_;
  bonusValid_ = false;
  bonusCapped_ = false;
  ++bonusCheckGeneration_;
  CreateError().IsOpen(false);
  ValidateCreateForm();
  ShowLoginStep(LoginStep::Create);
  if (guestUpgrade) {
    CreateEmailBox().Focus(FocusState::Programmatic);  // the first empty field
  } else {
    CreateNameBox().Focus(FocusState::Programmatic);
  }
}

void MainWindow::OnCreateNameChanged(IInspectable const&, TextChangedEventArgs const&) {
  ++nameCheckGeneration_;  // drop any availability check still in flight
  nameAvailable_ = false;
  CreateError().IsOpen(false);
  if (nameCheckTimer_) nameCheckTimer_.Stop();

  const std::string name = TrimWhitespace(urnw::Narrow(CreateNameBox().Text().c_str()));
  if (name.size() < kMinNetworkNameLength) {
    nameChecking_ = false;
    CreateNameStatusText().Text(Loc("network_name_length_error"));
    CreateNameStatusText().Foreground(urnw::colors::MutedBrush());
  } else {
    nameChecking_ = true;
    CreateNameStatusText().Text(L"");
    if (nameCheckTimer_) nameCheckTimer_.Start();  // debounce, then check
  }
  ValidateCreateForm();
}

void MainWindow::CheckCreateNameNow() {
  const std::string name = TrimWhitespace(urnw::Narrow(CreateNameBox().Text().c_str()));
  if (name.size() < kMinNetworkNameLength || !Sdk().apiReady()) return;
  const uint32_t generation = nameCheckGeneration_;

  auto queue = DispatcherQueue();
  auto weak = get_weak();
  Sdk().CheckNetworkName(name, [queue, weak, generation](bool ok, bool available) {
    queue.TryEnqueue([weak, generation, ok, available] {
      if (auto self = weak.get()) self->ApplyNameCheck(generation, ok, available);
    });
  });
}

void MainWindow::ApplyNameCheck(uint32_t generation, bool ok, bool available) {
  if (generation != nameCheckGeneration_) return;  // a later edit superseded this
  nameChecking_ = false;
  if (!ok) {
    nameAvailable_ = false;
    CreateNameStatusText().Text(Loc("there_was_an_error_checking_the_network_name"));
    CreateNameStatusText().Foreground(urnw::colors::DangerBrush());
  } else if (available) {
    nameAvailable_ = true;
    CreateNameStatusText().Text(Loc("nice_this_network_name_is_available"));
    CreateNameStatusText().Foreground(urnw::colors::MakeBrush(urnw::colors::kUrGreen));
  } else {
    nameAvailable_ = false;
    CreateNameStatusText().Text(Loc("network_name_taken"));
    CreateNameStatusText().Foreground(urnw::colors::DangerBrush());
  }
  ValidateCreateForm();
}

void MainWindow::OnCreateEmailChanged(IInspectable const&, TextChangedEventArgs const&) {
  CreateError().IsOpen(false);
  ValidateCreateForm();
}

void MainWindow::OnCreatePasswordChanged(IInspectable const&, RoutedEventArgs const&) {
  CreateError().IsOpen(false);
  ValidateCreateForm();
}

void MainWindow::OnTermsChanged(IInspectable const&, RoutedEventArgs const&) {
  CreateError().IsOpen(false);
  ValidateCreateForm();
}

void MainWindow::OnBonusCodeChanged(IInspectable const&, TextChangedEventArgs const&) {
  ++bonusCheckGeneration_;  // drop any validation still in flight
  bonusValid_ = false;
  bonusCapped_ = false;
  BonusStatusText().Text(L"");
  if (bonusCheckTimer_) {
    bonusCheckTimer_.Stop();
    const std::string code = TrimWhitespace(urnw::Narrow(BonusCodeBox().Text().c_str()));
    if (!code.empty()) bonusCheckTimer_.Start();
  }
}

void MainWindow::ValidateBonusCodeNow() {
  const std::string code = TrimWhitespace(urnw::Narrow(BonusCodeBox().Text().c_str()));
  if (code.empty() || !Sdk().apiReady()) return;
  const uint32_t generation = bonusCheckGeneration_;

  urnet::ValidateReferralCodeArgs args;
  args.referral_code = code;
  auto queue = DispatcherQueue();
  auto weak = get_weak();
  Sdk().api().validateReferralCode(
      args, [queue, weak, generation](std::optional<urnet::ValidateReferralCodeResult> result,
                                      std::optional<std::string> err) {
        const bool ok = !err && result.has_value();
        const bool valid = ok && result->is_valid;
        const bool capped = ok && result->is_capped;
        queue.TryEnqueue([weak, generation, ok, valid, capped] {
          if (auto self = weak.get()) self->ApplyBonusValidation(generation, ok, valid, capped);
        });
      });
}

void MainWindow::ApplyBonusValidation(uint32_t generation, bool ok, bool valid,
                                      bool capped) {
  if (generation != bonusCheckGeneration_) return;
  bonusValid_ = ok && valid;
  bonusCapped_ = ok && capped;
  if (!ok) {
    BonusStatusText().Text(Loc("something_went_wrong"));
    BonusStatusText().Foreground(urnw::colors::DangerBrush());
  } else if (bonusValid_ && !bonusCapped_) {
    BonusStatusText().Text(Loc("referral_bonus_applied_2"));
    BonusStatusText().Foreground(urnw::colors::MakeBrush(urnw::colors::kUrGreen));
  } else if (bonusCapped_) {
    BonusStatusText().Text(Loc("referral_code_capped"));
    BonusStatusText().Foreground(urnw::colors::DangerBrush());
  } else {
    BonusStatusText().Text(Loc("invalid_referral_code"));
    BonusStatusText().Foreground(urnw::colors::DangerBrush());
  }
}

void MainWindow::ValidateCreateForm() {
  const std::string password = urnw::Narrow(CreatePasswordBox().Password().c_str());
  const bool passwordOk = createMode_ == CreateMode::Wallet ||
                          password.size() >= kMinPasswordLength;
  // the guest upgrade collects the email on this step (the other modes carry a
  // discovered / wallet credential in); the server is the real validator
  const bool emailOk =
      createMode_ != CreateMode::GuestUpgrade ||
      LooksLikeUserAuth(TrimWhitespace(urnw::Narrow(CreateEmailBox().Text().c_str())));
  const bool termsOk = TermsCheck().IsChecked() && TermsCheck().IsChecked().Value();
  CreateButton().IsEnabled(nameAvailable_ && !nameChecking_ && passwordOk && emailOk &&
                           termsOk && !creatingNetwork_);
}

void MainWindow::OnCreateNetwork(IInspectable const&, RoutedEventArgs const&) {
  if (creatingNetwork_) return;
  const std::string networkName =
      TrimWhitespace(urnw::Narrow(CreateNameBox().Text().c_str()));
  const std::string password = urnw::Narrow(CreatePasswordBox().Password().c_str());
  creatingNetwork_ = true;
  CreateButton().IsEnabled(false);
  CreateError().IsOpen(false);

  auto queue = DispatcherQueue();
  auto weak = get_weak();
  auto done = [queue, weak](urnw::AuthResult r) {
    queue.TryEnqueue([weak, r] {
      auto self = weak.get();
      if (!self) return;
      self->creatingNetwork_ = false;
      self->ValidateCreateForm();
      if (r.verification_required) {
        self->EnterVerifyStep(self->loginUserAuth_);
      } else if (!r.ok && !r.error.empty()) {
        self->ShowLoginErrorFor(LoginStep::Create, H(r.error));
      }
      // success: the auth state relay swaps the panel for the home view
    });
  };

  if (createMode_ == CreateMode::GuestUpgrade) {
    // the email entered here also drives the verify step, should one be needed
    loginUserAuth_ = TrimWhitespace(urnw::Narrow(CreateEmailBox().Text().c_str()));
    Sdk().UpgradeGuest(networkName, loginUserAuth_, password, done);
    return;
  }

  urnw::CreateNetworkParams params;
  params.networkName = networkName;
  params.terms = TermsCheck().IsChecked() && TermsCheck().IsChecked().Value();
  if (createMode_ == CreateMode::Wallet) {
    params.useWalletAuth = true;
  } else {
    params.userAuth = loginUserAuth_;
    params.password = password;
  }
  if (bonusValid_ && !bonusCapped_) {
    params.referralCode = TrimWhitespace(urnw::Narrow(BonusCodeBox().Text().c_str()));
  }
  Sdk().CreateNetwork(params, done);
}

// ---- verify code ----

void MainWindow::EnterVerifyStep(std::string const& userAuth) {
  loginUserAuth_ = userAuth;
  // "You've got mail" for an email auth; "Check your phone" for a number
  VerifyHeading().Text(userAuth.find('@') != std::string::npos
                           ? Loc("login_verify_header")
                           : Loc("login_verify_check_phone"));
  VerifyCodeBox().Text(L"");
  VerifyButton().IsEnabled(false);
  VerifyInfo().IsOpen(false);
  ResendCodeButton().IsEnabled(true);
  ShowLoginStep(LoginStep::Verify);
  VerifyCodeBox().Focus(FocusState::Programmatic);
}

void MainWindow::OnVerifyCodeChanged(IInspectable const&, TextChangedEventArgs const&) {
  const std::string code = TrimWhitespace(urnw::Narrow(VerifyCodeBox().Text().c_str()));
  // typing dismisses a stale verdict; a programmatic clear must not close the
  // "code sent" info bar that was just raised
  if (!code.empty()) VerifyInfo().IsOpen(false);
  VerifyButton().IsEnabled(code.size() == kVerifyCodeLength && !verifying_);
  // a full code submits itself (macOS parity)
  if (code.size() == kVerifyCodeLength && !verifying_) SubmitVerifyCode();
}

void MainWindow::OnVerifySubmit(IInspectable const&, RoutedEventArgs const&) {
  SubmitVerifyCode();
}

void MainWindow::SubmitVerifyCode() {
  const std::string code = TrimWhitespace(urnw::Narrow(VerifyCodeBox().Text().c_str()));
  if (verifying_ || code.size() != kVerifyCodeLength || loginUserAuth_.empty()) return;
  verifying_ = true;
  VerifyButton().IsEnabled(false);

  auto queue = DispatcherQueue();
  auto weak = get_weak();
  Sdk().VerifyCode(loginUserAuth_, code, [queue, weak](urnw::AuthResult r) {
    queue.TryEnqueue([weak, r] {
      auto self = weak.get();
      if (!self) return;
      self->verifying_ = false;
      if (!r.ok) {
        // clear the entered code so retyping can resubmit (macOS parity)
        self->VerifyCodeBox().Text(L"");
        self->ShowLoginErrorFor(LoginStep::Verify, Loc("verify_input_invalid"));
      }
      // success: the auth state relay swaps the panel for the home view
    });
  });
}

void MainWindow::OnResendCode(IInspectable const&, RoutedEventArgs const&) {
  if (loginUserAuth_.empty()) return;
  ResendCodeButton().IsEnabled(false);
  VerifyInfo().IsOpen(false);

  auto queue = DispatcherQueue();
  auto weak = get_weak();
  Sdk().ResendVerifyCode(loginUserAuth_, [queue, weak](bool ok) {
    queue.TryEnqueue([weak, ok] {
      auto self = weak.get();
      if (!self) return;
      if (ok) {
        self->VerifyInfo().Severity(InfoBarSeverity::Success);
        self->VerifyInfo().Message(Loc("verification_code_sent"));
        self->VerifyInfo().IsOpen(true);
        // 15s cooldown before another resend (macOS parity)
        if (self->resendCooldownTimer_) self->resendCooldownTimer_.Start();
      } else {
        self->ResendCodeButton().IsEnabled(true);
        self->ShowLoginErrorFor(LoginStep::Verify, Loc("something_went_wrong"));
      }
    });
  });
}

// ---- auth code login ----

void MainWindow::OnUseCode(IInspectable const&, RoutedEventArgs const&) {
  const std::string code = urnw::Narrow(CodeBox().Text().c_str());
  if (code.empty()) return;
  CodeButton().IsEnabled(false);
  auto queue = DispatcherQueue();
  auto weak = get_weak();
  Sdk().LoginWithCode(code, [queue, weak](urnw::AuthResult r) {
    queue.TryEnqueue([weak, r] {
      auto self = weak.get();
      if (!self) return;
      self->CodeButton().IsEnabled(true);
      if (!r.ok && !r.error.empty()) {
        self->ShowLoginErrorFor(LoginStep::Initial, H(r.error));
      }
    });
  });
}

// ---- guest mode (macOS GuestModeSheet parity) ------------------------------
// One tap creates a throwaway network: the sheet collects the terms consent,
// SdkHost::LoginAsGuest creates and registers it, and the auth-state relay
// swaps the panel for the home view. The plan cards later offer the upgrade to
// a full account (BeginGuestUpgrade).

void MainWindow::OnTryGuestMode(IInspectable const&, RoutedEventArgs const&) {
  LoginError().IsOpen(false);
  ShowGuestModeSheet();
}

winrt::fire_and_forget MainWindow::ShowGuestModeSheet() {
  if (sheetOpen_) co_return;  // only one ContentDialog can show at a time
  auto self = get_strong();
  self->sheetOpen_ = true;
  try {
    self->guestSheet_ = urnw::GuestModeSheet::Create(Content().XamlRoot(), Sdk());
    co_await self->guestSheet_->Dialog().ShowAsync();
  } catch (...) {
  }
  self->guestSheet_.reset();
  self->sheetOpen_ = false;
}

void MainWindow::BeginGuestUpgrade() {
  // The create step in guest-upgrade mode (email + name + password ->
  // Api::upgradeGuest), shown over the login flow while the guest session
  // stays live. macOS presents the same flow as a sheet over the account view;
  // linux navigates its create page in UpgradeGuest mode. Back returns home
  // (OnLoginBack); success re-registers the device and the LoggedIn push
  // restores the home view.
  HomeNav().Visibility(Visibility::Collapsed);
  LoginRoot().Visibility(Visibility::Visible);
  EnterCreateStep(std::string(), CreateMode::GuestUpgrade);
}

// ---- wallet sign in ------------------------------------------------------
// Both wallets sign in through the ur.io/wallet-connect browser bridge (desktop
// wallets are browser extensions): the bridge drives the wallet and returns via
// the urnetwork:// scheme, which protocol activation routes back into SdkHost
// (main.cpp -> App::OnLaunched -> AppController::HandleDeepLink). `done` fires on
// an SDK thread, so hop to the UI thread before touching the panel.

void MainWindow::OnSignInWithBittensor(IInspectable const&, RoutedEventArgs const&) {
  LoginError().IsOpen(false);
  SetWalletSignInEnabled(false);
  auto queue = DispatcherQueue();
  auto weak = get_weak();
  Sdk().SignInWithBittensor([queue, weak](urnw::AuthResult r) {
    queue.TryEnqueue([weak, r] {
      if (auto self = weak.get()) self->ApplyWalletSignInResult(r);
    });
  });
}

void MainWindow::OnSignInWithSolana(IInspectable const& sender, RoutedEventArgs const&) {
  auto button = sender.try_as<Button>();
  if (!button) return;
  const auto tag = winrt::unbox_value_or<hstring>(button.Tag(), L"phantom");
  const auto provider = (tag == L"solflare") ? urnw::WalletConnect::Provider::Solflare
                                             : urnw::WalletConnect::Provider::Phantom;
  LoginError().IsOpen(false);
  SetWalletSignInEnabled(false);
  auto queue = DispatcherQueue();
  auto weak = get_weak();
  Sdk().SignInWithSolana(provider, [queue, weak](urnw::AuthResult r) {
    queue.TryEnqueue([weak, r] {
      if (auto self = weak.get()) self->ApplyWalletSignInResult(r);
    });
  });
}

void MainWindow::SetWalletSignInEnabled(bool enabled) {
  BittensorSignInButton().IsEnabled(enabled);
  PhantomSignInButton().IsEnabled(enabled);
  SolflareSignInButton().IsEnabled(enabled);
}

void MainWindow::ApplyWalletSignInResult(urnw::AuthResult const& result) {
  SetWalletSignInEnabled(true);
  // the wallet authenticated but has no network yet: finish sign-up with a
  // network name + terms; the retained wallet auth is the credential
  if (result.wallet_needs_network) {
    EnterCreateStep(std::string(), CreateMode::Wallet);
    return;
  }
  // on success ApplyAuthState swaps the panel for the home view; only an error
  // needs to be surfaced here
  if (result.ok || result.error.empty()) return;
  ShowLoginErrorFor(LoginStep::Initial, H(result.error));
}

void MainWindow::OnSignOut(IInspectable const&, RoutedEventArgs const&) {
  Sdk().Logout();
}

// ---- connect -------------------------------------------------------------

void MainWindow::OnConnectToggle(IInspectable const&, RoutedEventArgs const&) {
  if (connected_)
    Sdk().Disconnect();
  else
    Sdk().ConnectBestAvailable();
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

  if (tag == L"connect" && !wasConnectVisible) AnimateDrawerIn();

  if (!Sdk().apiReady()) return;
  if (tag == L"account") {
    LoadAccount();
    Balance().Refresh();  // macOS AccountRootView onAppear parity
  } else if (tag == L"wallet") {
    LoadWallet();
  } else if (tag == L"leaderboard") {
    LoadLeaderboard();
  }
}

// ---- account -------------------------------------------------------------

void MainWindow::LoadAccount() {
  auto queue = DispatcherQueue();
  auto weak = get_weak();
  Sdk().api().getNetworkUser([queue, weak](std::optional<urnet::GetNetworkUserResult> result,
                                           std::optional<std::string>) {
    if (!result || !result->network_user) return;
    urnet::NetworkUser u = *result->network_user;
    queue.TryEnqueue([weak, u] {
      auto self = weak.get();
      if (!self) return;
      self->NetworkNameBox().Text(H(u.network_name));
      const std::wstring auth = urnw::Widen(u.user_auth ? *u.user_auth : std::string());
      self->AccountAuthText().Text(hstring{urnw::Format(
          u.verified ? "account_auth_verified" : "account_auth_unverified", auth)});
    });
  });
  LoadReferralInfo();
  LoadBalanceCodes();
}

void MainWindow::LoadReferralInfo() {
  auto queue = DispatcherQueue();
  auto weak = get_weak();
  Sdk().api().getNetworkReferralCode(
      [queue, weak](std::optional<urnet::GetNetworkReferralCodeResult> result,
                    std::optional<std::string>) {
        if (!result) return;
        std::string code = result->referral_code ? *result->referral_code : "—";
        int64_t total = result->total_referrals;
        queue.TryEnqueue([weak, code, total] {
          auto self = weak.get();
          if (!self) return;
          self->referralCode_ = code;
          self->totalReferrals_ = total;
          // referrals no longer use deep links; friends enter the code on sign up
          self->ReferralText().Text(
              hstring{urnw::Format("referral_summary", urnw::Widen(code), total)});
          // referral royalty: at least one referral earns the crowned frog
          // mascot (same as the ur.io site)
          self->RoyaltyBadge().Visibility(0 < total ? Visibility::Visible
                                                    : Visibility::Collapsed);
          self->ApplyBalance();  // the usage-bar referral rows
        });
      });
}

void MainWindow::LoadBalanceCodes() {
  auto queue = DispatcherQueue();
  auto weak = get_weak();
  Sdk().api().getNetworkRedeemedBalanceCodes(
      [queue, weak](std::optional<urnet::GetNetworkRedeemedBalanceCodesResult> result,
                    std::optional<std::string>) {
        urnet::RedeemedBalanceCodeList codes;
        if (result && result->balance_codes) codes = *result->balance_codes;
        queue.TryEnqueue([weak, codes = std::move(codes)] {
          auto self = weak.get();
          if (!self) return;
          auto panel = self->BalanceCodesPanel();
          panel.Children().Clear();
          self->BalanceCodesEmptyText().Visibility(codes.empty() ? Visibility::Visible
                                                                 : Visibility::Collapsed);
          if (codes.empty()) return;

          // header + one row per redeemed code: code / data / redeemed / expires
          auto makeRow = [](hstring const& c0, hstring const& c1, hstring const& c2,
                            hstring const& c3, bool header) {
            Grid row;
            ColumnDefinition d0, d1, d2, d3;
            d0.Width(GridLength{1, GridUnitType::Star});
            d1.Width(GridLength{0, GridUnitType::Auto});
            d1.MinWidth(84);
            d2.Width(GridLength{0, GridUnitType::Auto});
            d2.MinWidth(96);
            d3.Width(GridLength{0, GridUnitType::Auto});
            d3.MinWidth(96);
            row.ColumnDefinitions().Append(d0);
            row.ColumnDefinitions().Append(d1);
            row.ColumnDefinitions().Append(d2);
            row.ColumnDefinitions().Append(d3);
            const std::array<hstring, 4> cells = {c0, c1, c2, c3};
            for (int i = 0; i < 4; ++i) {
              TextBlock cell;
              cell.Text(cells[i]);
              cell.FontSize(12);
              if (header) cell.Foreground(urnw::colors::MutedBrush());
              Grid::SetColumn(cell, i);
              row.Children().Append(cell);
            }
            return row;
          };
          panel.Children().Append(makeRow(Loc("code"), Loc("data"), Loc("redeemed"),
                                          Loc("expires"), /*header=*/true));
          for (auto const& code : codes) {
            panel.Children().Append(makeRow(
                H(MaskSecret(code.secret)),
                H("+" + urnw::FormatByteCountCompact(code.balance_byte_count)),
                H(code.redeem_time ? IsoDate(*code.redeem_time) : std::string()),
                H(code.end_time ? IsoDate(*code.end_time) : std::string()),
                /*header=*/false));
          }
        });
      });
}

void MainWindow::OnSaveNetworkName(IInspectable const&, RoutedEventArgs const&) {
  urnet::NetworkUserUpdateArgs args;
  args.network_name = urnw::Narrow(NetworkNameBox().Text().c_str());
  Sdk().api().networkUserUpdate(
      args, [](std::optional<urnet::NetworkUserUpdateResult>, std::optional<std::string>) {});
}

// ---- balance / plan (SubscriptionBalanceStore relay) -----------------------

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
  const hstring totals = hstring{urnw::Format("total_referrals_lld", totalReferrals_)};
  const hstring bonus = hstring{urnw::Format("referral_bonus", totalReferrals_ * 30)};
  AccountReferralTotals().Text(totals);
  DrawerReferralTotals().Text(totals);
  AccountReferralBonus().Text(bonus);
  DrawerReferralBonus().Text(bonus);

  UpdateBalanceWarning();
  // the open upgrade sheet watches for the plan flip / poll timeout
  if (upgradeSheet_) upgradeSheet_->OnBalance(balance_, balancePoll_);
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
    BeginGuestUpgrade();
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
          if (auto self = weak.get()) self->LoadBalanceCodes();
        });
    co_await self->redeemSheet_->Dialog().ShowAsync();
  } catch (...) {
  }
  self->redeemSheet_.reset();
  self->sheetOpen_ = false;
}

// ---- wallet --------------------------------------------------------------

void MainWindow::LoadWallet() {
  auto queue = DispatcherQueue();
  auto weak = get_weak();
  Sdk().api().getAccountWallets(
      [queue, weak](std::optional<urnet::GetAccountWalletsResult> result,
                    std::optional<std::string>) {
        if (!result || !result->wallets) return;
        urnet::AccountWalletsList wallets = *result->wallets;
        queue.TryEnqueue([weak, wallets] {
          if (auto self = weak.get()) self->ApplyWallets(wallets);
        });
      });
}

void MainWindow::ApplyWallets(urnet::AccountWalletsList const& wallets) {
  WalletsList().Items().Clear();
  for (auto const& w : wallets) {
    std::wstring label = ChainName(w.blockchain) + L"  " + urnw::Widen(w.wallet_address);
    // TAO (bittensor) wallets are recorded for future use only: the server
    // refuses to make one the payout wallet and skips the auto-default on
    // creation, so the row says so (macOS/android parity)
    if (w.blockchain == urnet::TAO) {
      label += L"  -  " + urnw::Localized("stored_for_future_use");
    }
    WalletsList().Items().Append(winrt::box_value(hstring{label}));
  }
}

// ---- connect wallet (external wallet, by address) -------------------------
// Paste an address; the server validates it per chain and the first chain that
// accepts it wins. Bittensor connects by address only (no signature), matching
// apple/android — the signed bridge flow is sign-in, not wallet connect.

void MainWindow::OnWalletAddressChanged(IInspectable const&, TextChangedEventArgs const&) {
  walletValidation_ = {};
  walletChain_.clear();
  ++walletValidateGeneration_;  // drop any validation still in flight
  ConnectWalletButton().IsEnabled(false);
  WalletChainText().Text(L"");
  if (walletValidateTimer_) {
    walletValidateTimer_.Stop();  // restart the debounce window on every keystroke
    walletValidateTimer_.Start();
  }
}

void MainWindow::ValidateWalletAddress() {
  const std::string address = urnw::Narrow(WalletAddressBox().Text().c_str());
  // the shortest supported address (solana base58) is 32 characters
  if (address.size() < 32 || !Sdk().apiReady()) return;
  const uint32_t generation = ++walletValidateGeneration_;

  auto queue = DispatcherQueue();
  auto weak = get_weak();
  const std::string chains[] = {urnet::SOL, urnet::MATIC, urnet::TAO};
  for (std::string const& chain : chains) {
    urnet::WalletValidateAddressArgs args;
    args.address = address;
    args.chain = chain;
    Sdk().api().walletValidateAddress(
        args, [queue, weak, chain, generation](
                  std::optional<urnet::WalletValidateAddressResult> result,
                  std::optional<std::string>) {
          const bool valid = result && result->valid && *result->valid;
          queue.TryEnqueue([weak, chain, generation, valid] {
            if (auto self = weak.get()) self->ApplyWalletValidation(chain, generation, valid);
          });
        });
  }
}

void MainWindow::ApplyWalletValidation(std::string const& chain, uint32_t generation,
                                       bool valid) {
  if (generation != walletValidateGeneration_) return;  // a later edit superseded this
  if (chain == urnet::SOL) walletValidation_.sol = valid;
  else if (chain == urnet::MATIC) walletValidation_.matic = valid;
  else if (chain == urnet::TAO) walletValidation_.tao = valid;

  if (walletValidation_.sol) walletChain_ = urnet::SOL;
  else if (walletValidation_.matic) walletChain_ = urnet::MATIC;
  else if (walletValidation_.tao) walletChain_ = urnet::TAO;
  else walletChain_.clear();

  ConnectWalletButton().IsEnabled(!walletChain_.empty() && !connectingWallet_);
  if (walletChain_ == urnet::TAO) {
    WalletChainText().Text(Loc("bittensor_wallet_future_use"));
  } else if (!walletChain_.empty()) {
    WalletChainText().Text(
        hstring{urnw::Format("wallet_provider_lower", ChainName(walletChain_))});
  } else {
    WalletChainText().Text(L"");
  }
}

void MainWindow::OnConnectWallet(IInspectable const&, RoutedEventArgs const&) {
  const std::string address = urnw::Narrow(WalletAddressBox().Text().c_str());
  if (address.empty() || walletChain_.empty() || connectingWallet_) return;
  connectingWallet_ = true;
  ConnectWalletButton().IsEnabled(false);

  // WalletViewController::addExternalWallet parity: the account wallet is created
  // on the chain the server validated, with the USDC token type.
  urnet::CreateAccountWalletArgs args;
  args.blockchain = walletChain_;
  args.wallet_address = address;
  args.default_token_type = "USDC";

  auto queue = DispatcherQueue();
  auto weak = get_weak();
  Sdk().api().createAccountWallet(
      args, [queue, weak](std::optional<urnet::CreateAccountWalletResult> result,
                          std::optional<std::string> err) {
        const bool ok = result && result->wallet_id && !result->wallet_id->empty();
        // this runs on an sdk thread; hand the raw outcome to the ui thread and
        // let it do the lookup (the store is read from the ui thread throughout)
        const std::string error = ok ? std::string() : (err ? *err : std::string());
        queue.TryEnqueue([weak, ok, error] {
          if (auto self = weak.get()) self->ApplyWalletConnectResult(ok, error);
        });
      });
}

void MainWindow::ApplyWalletConnectResult(bool ok, std::string const& serverError) {
  connectingWallet_ = false;
  WalletInfo().Severity(ok ? InfoBarSeverity::Success : InfoBarSeverity::Error);
  // a server error is not localizable; show it when there is one
  WalletInfo().Message(ok ? Loc("wallet_connected")
                          : (serverError.empty() ? Loc("wallet_connect_failed")
                                                 : H(serverError)));
  WalletInfo().IsOpen(true);
  if (!ok) {
    ConnectWalletButton().IsEnabled(!walletChain_.empty());
    return;
  }
  WalletAddressBox().Text(L"");  // clears the verdict through OnWalletAddressChanged
  LoadWallet();                  // refresh the list with the new wallet
}

// ---- leaderboard ---------------------------------------------------------

void MainWindow::LoadLeaderboard() {
  urnet::GetLeaderboardArgs args;
  Sdk().api().getLeaderboard(
      args, [this](std::optional<urnet::LeaderboardResult> result,
                   std::optional<std::string>) {
        if (!result || !result->earners) return;
        urnet::LeaderboardEarnersList earners = *result->earners;
        DispatcherQueue().TryEnqueue([this, earners] {
          LeaderboardList().Items().Clear();
          int rank = 1;
          for (auto const& e : earners) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%d.  %s  —  %.1f MiB", rank++,
                          e.network_name.c_str(), e.net_mib_count);
            LeaderboardList().Items().Append(winrt::box_value(winrt::to_hstring(buf)));
          }
        });
      });
}

// ---- support -------------------------------------------------------------

void MainWindow::OnSendFeedback(IInspectable const&, RoutedEventArgs const&) {
  urnet::FeedbackSendArgs args;
  args.star_count = static_cast<int64_t>(FeedbackRating().Value());
  const std::string text = urnw::Narrow(FeedbackText().Text().c_str());
  if (!text.empty()) {
    urnet::FeedbackSendNeeds needs;
    needs.other = text;
    args.needs = needs;
  }
  Sdk().api().sendFeedback(
      args, [this](std::optional<urnet::FeedbackSendResult>, std::optional<std::string>) {
        DispatcherQueue().TryEnqueue([this] {
          SupportInfo().Severity(InfoBarSeverity::Success);
          SupportInfo().Message(Loc("thanks_for_the_feedback"));
          SupportInfo().IsOpen(true);
          FeedbackText().Text(L"");
        });
      });
}

// ---- split tunnel (settings) ---------------------------------------------

void MainWindow::OnManageAppSplitTunnel(IInspectable const&, RoutedEventArgs const&) {
  ShowAppRulesSheet();
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
    ShowLoginErrorFor(loginStep_, H(error));
  }
  if (loggedIn) createMode_ = CreateMode::Password;  // any guest upgrade resolved
  if (loggedIn && !wasVisible) {
    // the drawer just appeared: refresh its state and play the entrance
    ResyncDrawer();
    if (Sdk().apiReady()) LoadReferralInfo();  // usage-bar referral rows
    if (ConnectView().Visibility() == Visibility::Visible) AnimateDrawerIn();
  }
  if (!loggedIn && wasVisible) {
    // signed out: the flow starts over
    ShowLoginStep(LoginStep::Initial);
  }
  if (!loggedIn && !wasVisible && createMode_ == CreateMode::GuestUpgrade &&
      !Sdk().IsLoggedIn()) {
    // the guest session ended under the upgrade form (server-side
    // invalidation): fall back to the start of the sign-in flow
    createMode_ = CreateMode::Password;
    ShowLoginStep(LoginStep::Initial);
  }
}

void MainWindow::OnTunnelStateChanged(urnw::proto::TunnelStatus const& status) {
  SetConnectedUi(status.state == urnw::proto::TunnelState::Up);
}

void MainWindow::SetConnectedUi(bool connected) {
  connected_ = connected;
  StatusText().Text(connected ? Loc("connected") : Loc("disconnected"));
  ConnectButton().Content(connected ? LocBox("disconnect") : LocBox("connect"));
}

// ---- live stats (macOS parity) -------------------------------------------

void MainWindow::OnStatsChanged(urnw::LiveStats const& stats) { ApplyStats(stats); }

void MainWindow::ApplyStats(urnw::LiveStats const& stats) {
  // Selected provider row. When the selected location is a connected network
  // peer, show its device name instead of the raw client id (req4): resolve it
  // from the live peer list by client id, like the linux drawer does.
  std::string locationName = stats.locationName;
  const auto peers = Sdk().ConnectedProvidePeers();
  if (auto selected = Sdk().SelectedLocation();
      peers && selected && selected->connect_location_id &&
      selected->connect_location_id->client_id &&
      !selected->connect_location_id->client_id->empty()) {
    const auto& clientId = *selected->connect_location_id->client_id;
    for (const auto& peer : *peers) {
      if (peer.ClientId && *peer.ClientId == clientId) {
        locationName = urnw::PeerDisplayName(peer);
        break;
      }
    }
  }
  LocationText().Text(locationName.empty() ? Loc("best_available_provider")
                                           : H(locationName));
  ApplyPeerCount(peers);  // the peers status line below the connect button (req1)
  // the connected country drives the dns-card recommendation pill; only refresh
  // it when the country actually changes (stats push on every throughput tick).
  const bool countryChanged =
      countryCode_ != stats.countryCode || countryName_ != stats.countryName;
  countryCode_ = stats.countryCode;
  countryName_ = stats.countryName;
  if (countryChanged) ApplyDnsRecommendationPill();

  // Provider window size ("Connected to N providers"), like macOS. The count is a
  // CLDR plural in the store: select the form, never inflect here.
  ProviderCountText().Text(
      stats.connected
          ? hstring{urnw::Plural("connected_provider_count", stats.providerCount)}
          : hstring{L""});

  // Live throughput feed: down / up bit rate. Arrows + rates, no prose.
  ThroughputText().Text(stats.connected
                            ? H("↓ " + urnw::FormatBitRate(stats.downBitsPerSecond) +
                                "    ↑ " + urnw::FormatBitRate(stats.upBitsPerSecond))
                            : hstring(L""));

  // Insufficient-balance warning (auto-disconnect happens in the SDK). The
  // action button opens the upgrade flow; Pro / a running confirmation poll
  // suppress it (UpdateBalanceWarning).
  insufficientBalance_ = stats.insufficientBalance;
  UpdateBalanceWarning();

  // Provide stats.
  hstring provide{L""};
  if (stats.provideEnabled) {
    provide = stats.providePaused
                  ? Loc("providing_paused")
                  : hstring{urnw::Plural("providing_client_count", stats.provideClients)};
  }
  ProvideStatsText().Text(provide);

  // provide indicator (apple parity). The effective provide mode is a bit set
  // (0 none, 1 network, 2 friends-and-family, 3 public) — per-case only.
  // Solid dot = Network tier; dot + outer ring = Public tier (amber while
  // paused — pause stops public only); coral = not providing.
  auto provideColor = urnw::colors::kUrCoral;
  bool provideRing = false;
  switch (stats.provideMode) {
    case 3:  // public
      provideColor = stats.providePaused ? urnw::colors::kUrAmber : urnw::colors::kUrGreen;
      provideRing = true;
      break;
    case 1:  // network (also Auto while idle)
    case 2:  // friends-and-family
      provideColor = urnw::colors::kUrGreen;
      break;
    default:
      break;
  }
  // discoverability line (apple/android parity): a paused device stays
  // discoverable — pause stops public provide only
  DiscoverableText().Text(Loc(stats.provideEnabled && stats.provideHasNetworkKey
                                  ? "device_discoverable"
                                  : "device_not_discoverable"));
  ProvideModeDot().Fill(urnw::colors::MakeBrush(provideColor));
  ProvideModeRing().Stroke(urnw::colors::MakeBrush(provideColor));
  ProvideModeRing().Visibility(provideRing ? winrt::Microsoft::UI::Xaml::Visibility::Visible
                                           : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
}

// ---- connect drawer --------------------------------------------------------
// macOS ConnectActions parity: three stats cards over live SDK feeds, the
// blocker toggle, the connect options (performance profile), and the plan +
// usage card.

void MainWindow::BuildCharts() {
  remoteChart_ = std::make_unique<urnw::TransferChart>(
      RemoteChartHost(), urnw::Localized("remote"), urnw::ThroughputRoute::Remote,
      urnw::colors::kUrGreen, urnw::colors::kUrPink);
  blockedChart_ = std::make_unique<urnw::TransferChart>(
      BlockedChartHost(), urnw::Localized("blocked"), urnw::ThroughputRoute::Block,
      urnw::colors::kUrCoral, urnw::colors::kUrMutedCoral);
  localChart_ = std::make_unique<urnw::TransferChart>(
      LocalChartHost(), urnw::Localized("local"), urnw::ThroughputRoute::Local,
      urnw::colors::kUrGreen, urnw::colors::kUrPink);
}

void MainWindow::WireDrawerFeeds() {
  // SdkHost handlers fire on SDK callback threads. Capture the (agile)
  // DispatcherQueue here on the UI thread and hop through it, resolving the
  // weak window ref only on the UI side.
  auto queue = DispatcherQueue();
  auto weak = get_weak();
  auto& sdk = Sdk();

  sdk.SetThroughputHandler([queue, weak](std::vector<urnet::ThroughputPoint> points,
                                         int64_t windowSeconds) {
    queue.TryEnqueue([weak, points = std::move(points), windowSeconds] {
      if (auto self = weak.get()) {
        self->remoteChart_->SetPoints(points, windowSeconds);
        self->blockedChart_->SetPoints(points, windowSeconds);
        self->localChart_->SetPoints(points, windowSeconds);
      }
    });
  });
  // The ContractDetailsViewController already coalesces the egress + ingress
  // change streams into one settled ContractRowsChanged (no intermediate
  // one-list-updated aggregate reaches us), so the UI can apply each push
  // directly -- re-reading the settled snapshot on the UI thread (macOS
  // ContractDetailsStore.update parity).
  sdk.SetContractRowsHandler([queue, weak](std::vector<urnw::ContractPeerRow>) {
    queue.TryEnqueue([weak] {
      if (auto self = weak.get()) {
        self->contractRows_ = Sdk().CurrentContractRows();
        if (self->contractsSheet_) self->contractsSheet_->Update(self->contractRows_);
      }
    });
  });
  sdk.SetBlockActionsHandler([queue, weak](std::vector<urnw::BlockActionItem> actions) {
    queue.TryEnqueue([weak, actions = std::move(actions)] {
      if (auto self = weak.get()) {
        self->blockActions_ = actions;
        if (self->splitRulesSheet_) {
          self->splitRulesSheet_->Update(self->splitRules_, self->blockActions_,
                                         self->allowedCount_, self->blockedCount_);
        }
      }
    });
  });
  sdk.SetBlockStatsHandler([queue, weak](int64_t allowed, int64_t blocked) {
    queue.TryEnqueue([weak, allowed, blocked] {
      if (auto self = weak.get()) {
        self->allowedCount_ = allowed;
        self->blockedCount_ = blocked;
        if (self->splitRulesSheet_) {
          self->splitRulesSheet_->Update(self->splitRules_, self->blockActions_, allowed,
                                         blocked);
        }
      }
    });
  });
  sdk.SetSplitRulesHandler([queue, weak](std::vector<urnw::SplitRule> rules) {
    queue.TryEnqueue([weak, rules = std::move(rules)] {
      if (auto self = weak.get()) {
        self->splitRules_ = rules;
        self->ApplySplitRuleCount();
        if (self->splitRulesSheet_) {
          self->splitRulesSheet_->Update(self->splitRules_, self->blockActions_,
                                         self->allowedCount_, self->blockedCount_);
        }
      }
    });
  });
  sdk.SetDnsSettingsHandler([queue, weak](std::optional<urnet::DnsResolverSettings> settings) {
    queue.TryEnqueue([weak, settings = std::move(settings)] {
      if (auto self = weak.get()) {
        self->dnsSettings_ = settings;
        self->ApplyDnsCard(settings);
      }
    });
  });
  sdk.SetBlockerEnabledHandler([queue, weak](bool on) {
    queue.TryEnqueue([weak, on] {
      if (auto self = weak.get()) self->ApplyBlockerUi(on);
    });
  });
  // location/provider chooser: the bucketed locations feed the open sheet; the
  // peers feed both the sheet's pinned section and the drawer's peer-count label
  sdk.SetLocationsHandler([queue, weak](std::optional<urnet::FilteredLocations> locations,
                                        std::string) {
    queue.TryEnqueue([weak, locations = std::move(locations)] {
      if (auto self = weak.get(); self && self->locationSheet_) {
        self->locationSheet_->Update(locations, Sdk().ConnectedProvidePeers());
      }
    });
  });
  sdk.SetPeersHandler([queue, weak](std::optional<urnet::NetworkPeerList> peers) {
    queue.TryEnqueue([weak, peers = std::move(peers)] {
      if (auto self = weak.get()) {
        self->ApplyPeerCount(peers);
        if (self->locationSheet_) {
          self->locationSheet_->Update(Sdk().CurrentFilteredLocations(), peers);
        }
      }
    });
  });
}

void MainWindow::WireCardAffordances() {
  // desktop hover/pressed feedback for the tappable cards; resolve the card
  // from the sender so the handlers hold no strong element refs
  auto wire = [](Border const& card) {
    auto setBg = [](IInspectable const& sender, winrt::Windows::UI::Color color) {
      if (auto border = sender.try_as<Border>()) {
        border.Background(urnw::colors::MakeBrush(color));
      }
    };
    card.PointerEntered(
        [setBg](IInspectable const& sender, auto const&) { setBg(sender, urnw::colors::kCardHover); });
    card.PointerExited(
        [setBg](IInspectable const& sender, auto const&) { setBg(sender, urnw::colors::kCard); });
    card.PointerPressed(
        [setBg](IInspectable const& sender, auto const&) { setBg(sender, urnw::colors::kCardPressed); });
    card.PointerReleased(
        [setBg](IInspectable const& sender, auto const&) { setBg(sender, urnw::colors::kCardHover); });
    card.PointerCanceled(
        [setBg](IInspectable const& sender, auto const&) { setBg(sender, urnw::colors::kCard); });
  };
  wire(LocationRow());
  wire(PeersLine());
  wire(ClientStatsCard());
  wire(LocalStatsCard());
  wire(DnsCard());
}

void MainWindow::ResyncDrawer() {
  auto& sdk = Sdk();
  // open the locations + peers feeds so the drawer's "N network peers" label is
  // live from login, not only after the chooser is first opened (idempotent and
  // session-guarded; one provider fetch per session, matching the Linux app).
  sdk.EnsureLocations();
  int64_t windowSeconds = 60;
  auto points = sdk.CurrentThroughputPoints(windowSeconds);
  remoteChart_->SetPoints(points, windowSeconds);
  blockedChart_->SetPoints(points, windowSeconds);
  localChart_->SetPoints(points, windowSeconds);
  contractRows_ = sdk.CurrentContractRows();
  blockActions_ = sdk.CurrentBlockActions();
  sdk.CurrentBlockCounts(allowedCount_, blockedCount_);
  splitRules_ = sdk.CurrentSplitRules();
  dnsSettings_ = sdk.CurrentDnsSettings();
  ApplySplitRuleCount();
  ApplyDnsCard(dnsSettings_);
  SeedConnectControls();
}

void MainWindow::SeedConnectControls() {
  updatingControls_ = true;
  const urnw::PerformanceSettings settings = Sdk().CurrentPerformanceSettings();
  switch (settings.mode) {
    case urnw::ConnectionMode::Auto: ConnectionModeBar().SelectedItem(ModeAutoItem()); break;
    case urnw::ConnectionMode::Web: ConnectionModeBar().SelectedItem(ModeWebItem()); break;
    case urnw::ConnectionMode::Streaming:
      ConnectionModeBar().SelectedItem(ModeStreamingItem());
      break;
  }
  FixedIpToggle().IsOn(settings.fixedIp);
  FixedIpToggle().IsEnabled(settings.mode != urnw::ConnectionMode::Auto);
  // "Strong Anonymization" is the inverse of allowDirect
  StrongAnonToggle().IsOn(!settings.allowDirect);
  PostQuantumToggle().IsOn(settings.postQuantum);
  BlockerToggle().IsOn(Sdk().CurrentBlockerEnabled());
  // provide control mode ("manual"/unknown land on Never, the SDK's
  // conservative default case)
  const std::string provideMode = Sdk().CurrentProvideControlMode();
  if (provideMode == "auto") {
    ProvideModeBar().SelectedItem(ProvideAutoItem());
  } else if (provideMode == "always") {
    ProvideModeBar().SelectedItem(ProvideAlwaysItem());
  } else if (provideMode == "network") {
    ProvideModeBar().SelectedItem(ProvideNetworkItem());
  } else {
    ProvideModeBar().SelectedItem(ProvideNeverItem());
  }
  updatingControls_ = false;
}

urnw::ConnectionMode MainWindow::SelectedMode() {
  auto selected = ConnectionModeBar().SelectedItem();
  if (selected == ModeWebItem()) return urnw::ConnectionMode::Web;
  if (selected == ModeStreamingItem()) return urnw::ConnectionMode::Streaming;
  return urnw::ConnectionMode::Auto;
}

void MainWindow::PushPerformanceSettings() {
  urnw::PerformanceSettings settings;
  settings.mode = SelectedMode();
  settings.fixedIp = FixedIpToggle().IsOn();
  settings.allowDirect = !StrongAnonToggle().IsOn();
  settings.postQuantum = PostQuantumToggle().IsOn();
  Sdk().SetPerformanceSettings(settings);
}

void MainWindow::OnConnectionModeChanged(SelectorBar const&,
                                         SelectorBarSelectionChangedEventArgs const&) {
  if (updatingControls_) return;
  const urnw::ConnectionMode mode = SelectedMode();
  if (mode == urnw::ConnectionMode::Auto && FixedIpToggle().IsOn()) {
    // Auto forces Fixed IP off (macOS parity); update quietly, push once below
    updatingControls_ = true;
    FixedIpToggle().IsOn(false);
    updatingControls_ = false;
  }
  FixedIpToggle().IsEnabled(mode != urnw::ConnectionMode::Auto);
  PushPerformanceSettings();
}

std::string MainWindow::SelectedProvideMode() {
  auto selected = ProvideModeBar().SelectedItem();
  if (selected == ProvideAutoItem()) return "auto";
  if (selected == ProvideAlwaysItem()) return "always";
  if (selected == ProvideNetworkItem()) return "network";
  return "never";
}

void MainWindow::OnProvideModeChanged(SelectorBar const&,
                                      SelectorBarSelectionChangedEventArgs const&) {
  if (updatingControls_) return;
  Sdk().SetProvideControlMode(SelectedProvideMode());
}

void MainWindow::OnFixedIpToggled(IInspectable const&, RoutedEventArgs const&) {
  if (updatingControls_) return;
  PushPerformanceSettings();
}

void MainWindow::OnStrongAnonToggled(IInspectable const&, RoutedEventArgs const&) {
  if (updatingControls_) return;
  PushPerformanceSettings();
}

void MainWindow::OnPostQuantumToggled(IInspectable const&, RoutedEventArgs const&) {
  if (updatingControls_) return;
  PushPerformanceSettings();
}

void MainWindow::OnBlockerToggled(IInspectable const&, RoutedEventArgs const&) {
  if (updatingControls_) return;
  // the device applies and persists the blocker; the app stores nothing
  Sdk().SetBlockerEnabled(BlockerToggle().IsOn());
}

void MainWindow::ApplyBlockerUi(bool on) {
  if (BlockerToggle().IsOn() == on) return;
  updatingControls_ = true;
  BlockerToggle().IsOn(on);
  updatingControls_ = false;
}

void MainWindow::ApplySplitRuleCount() {
  SplitRuleCountText().Text(
      hstring{urnw::Plural("split_rule_count", static_cast<int64_t>(splitRules_.size()))});
}

void MainWindow::ApplyDnsCard(std::optional<urnet::DnsResolverSettings> const& settings) {
  DnsRowsPanel().Visibility(settings ? Visibility::Visible : Visibility::Collapsed);
  DnsUnavailableText().Visibility(settings ? Visibility::Collapsed : Visibility::Visible);
  // the applied settings just changed: re-evaluate the recommendation pill (it
  // reads dnsSettings_, already updated to `settings` by the caller). Runs in
  // the unavailable path too so the pill collapses with the rows.
  ApplyDnsRecommendationPill();
  if (!settings) return;

  // looked up once for the four rows; the lambda runs here, so capturing by
  // reference is safe
  const hstring onText = Loc("on");
  const hstring offText = Loc("off");
  auto applyRow = [&onText, &offText](Microsoft::UI::Xaml::Shapes::Ellipse const& dot,
                                      TextBlock const& state, bool on) {
    dot.Fill(urnw::colors::MakeBrush(
        on ? urnw::colors::kUrGreen
           : urnw::colors::WithAlpha(urnw::colors::kTextFaint, 102)));
    state.Text(on ? onText : offText);
    state.Foreground(on ? urnw::colors::MakeBrush(urnw::colors::kUrGreen)
                        : urnw::colors::MutedBrush());
  };
  applyRow(DohDot(), DohState(), settings->EnableRemoteDoh || settings->EnableLocalDoh);
  applyRow(UdnsDot(), UdnsState(), settings->EnableRemoteDns || settings->EnableLocalDns);
  applyRow(LdnsDot(), LdnsState(), settings->EnableLocalDoh || settings->EnableLocalDns);
  applyRow(FallbackDot(), FallbackState(), settings->EnableFallback);
}

// The unapplied-recommendation pill atop the dns card (iOS DnsRecommendationPill
// parity). Priority, matching the iOS computed `recommendation`:
//   1. no applied settings -> hidden (nothing to compare; the card shows
//      "unavailable").
//   2. a connected country whose regional recommendation differs from the
//      applied settings -> pill "...recommended settings for {country}" with the
//      country-color dot. If that recommendation IS already applied, hide and do
//      NOT fall through to the default nudge.
//   3. otherwise (no country, or the country has no regional recommendation) and
//      the safe defaults are not applied -> pill "default safe settings are not
//      applied", no dot.
//   4. hidden otherwise.
void MainWindow::ApplyDnsRecommendationPill() {
  const auto& current = dnsSettings_;
  if (!current) {
    DnsRecPill().Visibility(Visibility::Collapsed);
    return;
  }
  if (!countryCode_.empty()) {
    const std::string code = ToLower(countryCode_);
    if (auto rec = urnet::getRecommendedDnsResolverSettings(code)) {
      if (!DnsSettingsEquivalent(*current, *rec)) {
        const std::wstring name = countryName_.empty() ? urnw::Widen(ToUpper(countryCode_))
                                                        : urnw::Widen(countryName_);
        DnsRecText().Text(hstring{urnw::Format("dns_pill_recommended", name)});
        DnsRecDot().Fill(urnw::colors::MakeBrush(ColorFromHex(urnet::getColorHex(code))));
        DnsRecDot().Visibility(Visibility::Visible);
        DnsRecPill().Visibility(Visibility::Visible);
      } else {
        DnsRecPill().Visibility(Visibility::Collapsed);
      }
      return;  // the country has a recommendation: never fall through to defaults
    }
  }
  if (auto def = urnet::getDefaultDnsResolverSettings();
      def && !DnsSettingsEquivalent(*current, *def)) {
    DnsRecText().Text(Loc("dns_pill_default"));
    DnsRecDot().Visibility(Visibility::Collapsed);
    DnsRecPill().Visibility(Visibility::Visible);
    return;
  }
  DnsRecPill().Visibility(Visibility::Collapsed);
}

void MainWindow::OnChartTick() {
  // skip the redraw work while the window is hidden (tray) or on another tab
  if (!Visible()) return;
  if (ConnectView().Visibility() != Visibility::Visible && !sheetOpen_) return;
  if (ConnectView().Visibility() == Visibility::Visible) {
    remoteChart_->Tick();
    blockedChart_->Tick();
    localChart_->Tick();
  }
  if (contractsSheet_) contractsSheet_->Tick();  // ring/disc easing + slide animations
  if (++chartTickCount_ % 10 == 0) {  // ~1s cadence
    if (splitRulesSheet_) splitRulesSheet_->RefreshTimes();  // "Ns ago" labels
  }
  // the contract-details activity resort now lives in the SDK view controller;
  // the sheet just reports scroll and renders the ordered rows (no local tick)
}

void MainWindow::AnimateDrawerIn() {
  // fade + slight slide-up, 300ms ease-out, staggered across the cards. Played
  // once per window: replaying would fight the finished animations' hold
  // values and flash the cards.
  if (drawerAnimated_) return;
  drawerAnimated_ = true;
  namespace anim = winrt::Microsoft::UI::Xaml::Media::Animation;
  const std::array<FrameworkElement, 6> cards = {
      ControlsCard(), ClientStatsCard(), LocalStatsCard(), DnsCard(), BlockerCard(),
      DrawerPlanCard()};
  const auto duration = Duration{std::chrono::duration_cast<winrt::Windows::Foundation::TimeSpan>(
                                     std::chrono::milliseconds(300)),
                                 DurationType::TimeSpan};
  int index = 0;
  for (auto const& card : cards) {
    auto shift = Media::TranslateTransform();
    shift.Y(16);
    card.RenderTransform(shift);
    card.Opacity(0);

    anim::CubicEase ease;
    ease.EasingMode(anim::EasingMode::EaseOut);
    const auto beginTime = std::chrono::duration_cast<winrt::Windows::Foundation::TimeSpan>(
        std::chrono::milliseconds(50 * index));

    anim::Storyboard storyboard;
    anim::DoubleAnimation fade;
    fade.From(0.0);
    fade.To(1.0);
    fade.Duration(duration);
    fade.BeginTime(beginTime);
    fade.EasingFunction(ease);
    anim::Storyboard::SetTarget(fade, card);
    anim::Storyboard::SetTargetProperty(fade, L"Opacity");
    storyboard.Children().Append(fade);

    anim::DoubleAnimation slide;
    slide.From(16.0);
    slide.To(0.0);
    slide.Duration(duration);
    slide.BeginTime(beginTime);
    slide.EasingFunction(ease);
    anim::Storyboard::SetTarget(slide, shift);
    anim::Storyboard::SetTargetProperty(slide, L"Y");
    storyboard.Children().Append(slide);

    storyboard.Begin();
    ++index;
  }
}

// ---- drawer sheets (ContentDialogs) ----------------------------------------

void MainWindow::OnClientStatsCardTapped(IInspectable const&,
                                         Input::TappedRoutedEventArgs const&) {
  ShowClientContractsSheet();
}

void MainWindow::OnLocalStatsCardTapped(IInspectable const&,
                                        Input::TappedRoutedEventArgs const&) {
  ShowSplitRulesSheet();
}

void MainWindow::OnDnsCardTapped(IInspectable const&, Input::TappedRoutedEventArgs const&) {
  ShowDnsSheet();
}

void MainWindow::OnLocationRowTapped(IInspectable const&, Input::TappedRoutedEventArgs const&) {
  ShowLocationChooserSheet();
}

void MainWindow::OnPeersLineTapped(IInspectable const&, Input::TappedRoutedEventArgs const&) {
  ShowLocationChooserSheet();
}

winrt::fire_and_forget MainWindow::ShowClientContractsSheet() {
  if (sheetOpen_) co_return;  // only one ContentDialog can show at a time
  auto self = get_strong();
  self->sheetOpen_ = true;
  try {
    self->contractsSheet_ = urnw::ClientContractsSheet::Create(Content().XamlRoot(), Sdk());
    self->contractsSheet_->Update(self->contractRows_);
    co_await self->contractsSheet_->Dialog().ShowAsync();
  } catch (...) {
  }
  self->contractsSheet_.reset();
  self->sheetOpen_ = false;
  // the sheet drove the VC's at-top state; leave it at the top on close so the
  // controller isn't stuck frozen (collecting a pending count) with nobody viewing
  Sdk().SetContractsAtTop(true);
}

winrt::fire_and_forget MainWindow::ShowSplitRulesSheet() {
  if (sheetOpen_) co_return;
  auto self = get_strong();
  self->sheetOpen_ = true;
  try {
    self->splitRulesSheet_ = urnw::SplitRulesSheet::Create(Content().XamlRoot(), Sdk());
    self->splitRulesSheet_->Update(self->splitRules_, self->blockActions_,
                                   self->allowedCount_, self->blockedCount_);
    co_await self->splitRulesSheet_->Dialog().ShowAsync();
  } catch (...) {
  }
  self->splitRulesSheet_.reset();
  self->sheetOpen_ = false;
}

winrt::fire_and_forget MainWindow::ShowAppRulesSheet() {
  if (sheetOpen_) co_return;
  auto self = get_strong();
  self->sheetOpen_ = true;
  try {
    self->appRulesSheet_ = urnw::AppRulesSheet::Create(Content().XamlRoot(), Sdk());
    co_await self->appRulesSheet_->Dialog().ShowAsync();
  } catch (...) {
  }
  self->appRulesSheet_.reset();
  self->sheetOpen_ = false;
}

winrt::fire_and_forget MainWindow::ShowDnsSheet() {
  if (sheetOpen_) co_return;
  auto self = get_strong();
  self->sheetOpen_ = true;
  try {
    // draft edits apply together on Update; live store pushes don't reset the
    // open editor (macOS parity)
    self->dnsSheet_ = urnw::DnsEditorSheet::Create(Content().XamlRoot(), Sdk(),
                                                   self->dnsSettings_, self->countryCode_,
                                                   self->countryName_);
    co_await self->dnsSheet_->Dialog().ShowAsync();
  } catch (...) {
  }
  self->dnsSheet_.reset();
  self->sheetOpen_ = false;
}

winrt::fire_and_forget MainWindow::ShowLocationChooserSheet() {
  if (sheetOpen_) co_return;  // only one ContentDialog can show at a time
  auto self = get_strong();
  self->sheetOpen_ = true;
  // open the locations + peers view controllers (idempotent) and push an initial
  // snapshot before seeding the sheet from the current values
  Sdk().EnsureLocations();
  try {
    self->locationSheet_ = urnw::LocationChooserSheet::Create(Content().XamlRoot(), Sdk());
    self->locationSheet_->Update(Sdk().CurrentFilteredLocations(),
                                 Sdk().ConnectedProvidePeers());
    co_await self->locationSheet_->Dialog().ShowAsync();
  } catch (...) {
  }
  self->locationSheet_.reset();
  self->sheetOpen_ = false;
}

void MainWindow::ApplyPeerCount(std::optional<urnet::NetworkPeerList> const& peers) {
  // ALL connected devices (online, provide or not); the chooser's peers
  // section stays provide-filtered (connectable only). The list argument is
  // the update trigger; the count reads the unfiltered value.
  (void)peers;
  const int64_t count = Sdk().ConnectedPeerCount();
  // the standalone peers status line below the connect button, always shown:
  // "{n} peers" + a filled dot, green when providing peers are online and amber
  // at zero (apple ConnectActions parity)
  PeerCountText().Text(hstring{urnw::Plural("network_peer_count", count)});
  PeerDot().Fill(urnw::colors::MakeBrush(0 < count ? urnw::colors::kUrGreen
                                                   : urnw::colors::kUrAmber));
}

}  // namespace winrt::URnetwork::implementation
