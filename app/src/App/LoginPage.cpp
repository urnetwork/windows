// SPDX-License-Identifier: MPL-2.0
#include "pch.h"

#include "LoginPage.h"

#include <chrono>

#include <winrt/Windows.System.h>

#include "MainWindow.xaml.h"
#include "PageContext.h"
#include "Strings.h"
#include "UrColors.h"
#include "UrComponents.h"
#include "WalletConnect.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace urnw::pages;

namespace urnw {

// winrt::implements makes IInspectable a member typedef of every C++/WinRT
// implementation type, which is why MainWindow could name it unqualified. A
// plain class outside that hierarchy has to bring it in.
using winrt::Windows::Foundation::IInspectable;

namespace {
// network names must be 6 characters or more (macOS CreateNetworkViewModel)
constexpr size_t kMinNetworkNameLength = 6;
// passwords must be at least 12 characters (macOS CreateNetworkViewModel)
constexpr size_t kMinPasswordLength = 12;
// a verification code is 6 digits (macOS CreateNetworkVerifyViewModel)
constexpr size_t kVerifyCodeLength = 6;
}  // namespace

LoginPage::LoginPage(winrt::URnetwork::implementation::MainWindow& window)
    : w_(window) {}

LoginPage::~LoginPage() {
  if (nameCheckTimer_) nameCheckTimer_.Stop();
  if (bonusCheckTimer_) bonusCheckTimer_.Stop();
  if (resendCooldownTimer_) resendCooldownTimer_.Stop();
}

void LoginPage::Initialize() {
  auto queue = w_.DispatcherQueue();

  // debounce the sign-up network-name availability check (macOS: 250ms)
  nameCheckTimer_ = queue.CreateTimer();
  nameCheckTimer_.Interval(std::chrono::milliseconds(300));
  nameCheckTimer_.IsRepeating(false);
  nameCheckTimer_.Tick([weak = w_.get_weak()](auto const&, auto const&) {
    if (auto self = weak.get()) self->login().CheckCreateNameNow();
  });

  // debounce the bonus referral code validation
  bonusCheckTimer_ = queue.CreateTimer();
  bonusCheckTimer_.Interval(std::chrono::milliseconds(400));
  bonusCheckTimer_.IsRepeating(false);
  bonusCheckTimer_.Tick([weak = w_.get_weak()](auto const&, auto const&) {
    if (auto self = weak.get()) self->login().ValidateBonusCodeNow();
  });

  // resend-code cooldown: re-enable the resend button after 15s (macOS parity)
  resendCooldownTimer_ = queue.CreateTimer();
  resendCooldownTimer_.Interval(std::chrono::seconds(15));
  resendCooldownTimer_.IsRepeating(false);
  resendCooldownTimer_.Tick([weak = w_.get_weak()](auto const&, auto const&) {
    if (auto self = weak.get()) self->ResendCodeButton().IsEnabled(true);
  });
}

// ---- strings ---------------------------------------------------------------

void LoginPage::ApplyStrings() {
  // sign in — initial (account discovery). Order and wording follow android
  // ungoogle/LoginInitial.kt: the label sits above the field as its own
  // URTextInputLabel, and the wallet options are separated from Get started by
  // a bare centred "or" rather than by a section heading.
  w_.EmailLabel().Text(Loc("user_auth_label"));
  w_.EmailBox().PlaceholderText(Loc("user_auth_input_placeholder"));
  w_.GetStartedButton().Content(LocBox("get_started"));
  w_.OrDivider().Text(Loc("or"));
  w_.BittensorSignInText().Text(Loc("bittensor_sign_in"));
  w_.SolanaSignInText().Text(Loc("solana_sign_in"));
  // SdkHost::LoginWithCode is the auth-code login the other platforms ship
  w_.AuthCodeButtonText().Text(Loc("auth_code_login_button_text"));

  // sign in — password step
  w_.PasswordBackButton().Content(LocBox("back"));
  w_.PasswordHeading().Text(Loc("its_nice_to_see_you_again"));
  w_.PasswordBox().Header(LocBox("password_label"));
  w_.SignInButton().Content(LocBox("sign_in"));
  w_.ForgotPasswordLabel().Text(Loc("forgot_password"));
  w_.ForgotResetButton().Content(LocBox("reset_it"));

  // sign in — create network step
  w_.CreateBackButton().Content(LocBox("back"));
  w_.CreateHeading().Text(Loc("join_urnetwork"));
  w_.WalletCreateNote().Text(Loc("wallet_needs_network"));
  // guest upgrade: status + call to action, two store sentences on two lines
  w_.GuestUpgradeNote().Text(hstring{urnw::Localized("in_guest_mode") + L"\n" +
                                     urnw::Localized("start_earning_join")});
  w_.CreateEmailBox().Header(LocBox("user_auth_label"));
  w_.CreateEmailBox().PlaceholderText(Loc("user_auth_input_placeholder"));
  w_.CreateNameBox().Header(LocBox("network_name_label"));
  w_.CreateNameBox().PlaceholderText(Loc("enter_a_name_for_your_network"));
  w_.CreateNameStatusText().Text(Loc("network_name_length_error"));
  w_.CreatePasswordBox().Header(LocBox("password_label"));
  w_.CreatePasswordHint().Text(Loc("password_must_be_at_least_12_characters_long"));
  // tappable terms / privacy links inside the checkbox label
  urnw::SetTermsMarkerText(w_.TermsText(), urnw::Localized("terms_checkbox"), 12);
  w_.BonusCodeBox().Header(LocBox("bonus_referral_code_label"));
  w_.BonusCodeBox().PlaceholderText(Loc("enter_a_bonus_referral_code"));
  w_.CreateButton().Content(LocBox("continue_txt"));

  // sign in — verify step
  w_.VerifyBackButton().Content(LocBox("back"));
  w_.VerifyHeading().Text(Loc("login_verify_header"));
  w_.VerifyExplanationText().Text(Loc("verify_explanation"));
  w_.VerifyCodeBox().Header(LocBox("verify_input_label"));
  w_.VerifyButton().Content(LocBox("verify"));
  w_.DontSeeItLabel().Text(Loc("dont_see_it"));
  w_.ResendCodeButton().Content(LocBox("resend_verify_code"));

  // sign in — password reset step
  w_.ResetBackButton().Content(LocBox("back"));
  w_.ResetHeading().Text(Loc("forgot_password"));
  w_.ResetSpamNote().Text(Loc("you_may_need_to_your_check_spam_folder_or"));
  w_.SendResetButton().Content(LocBox("send_reset_link_2"));
}

// ---- window-level calls ----------------------------------------------------

void LoginPage::ResetToInitialStep() { ShowLoginStep(LoginStep::Initial); }

void LoginPage::ShowErrorOnCurrentStep(hstring const& message) {
  ShowLoginErrorFor(loginStep_, message);
}

bool LoginPage::IsGuestUpgrade() const {
  return createMode_ == CreateMode::GuestUpgrade;
}

void LoginPage::ClearGuestUpgrade() { createMode_ = CreateMode::Password; }

void LoginPage::BeginGuestUpgrade() {
  // The create step in guest-upgrade mode (email + name + password ->
  // Api::upgradeGuest), shown over the login flow while the guest session
  // stays live. macOS presents the same flow as a sheet over the account view;
  // linux navigates its create page in UpgradeGuest mode. Back returns home
  // (OnLoginBack); success re-registers the device and the LoggedIn push
  // restores the home view.
  w_.ShowLoginRoot();
  EnterCreateStep(std::string(), CreateMode::GuestUpgrade);
}

// ---- sign-in flow ----------------------------------------------------------
// macOS Authenticate/** parity: the initial step discovers the account for a
// user auth (authLogin), then routes to the password, create-network or verify
// step. The wallet buttons and the auth-code login stay on the initial step.

void LoginPage::ShowLoginStep(LoginStep step) {
  loginStep_ = step;
  w_.LoginPanel().Visibility(step == LoginStep::Initial ? Visibility::Visible
                                                        : Visibility::Collapsed);
  w_.PasswordPanel().Visibility(step == LoginStep::Password ? Visibility::Visible
                                                            : Visibility::Collapsed);
  w_.CreatePanel().Visibility(step == LoginStep::Create ? Visibility::Visible
                                                        : Visibility::Collapsed);
  w_.VerifyPanel().Visibility(step == LoginStep::Verify ? Visibility::Visible
                                                        : Visibility::Collapsed);
  w_.ResetPanel().Visibility(step == LoginStep::Reset ? Visibility::Visible
                                                      : Visibility::Collapsed);
}

// The initial step shows android's URInlineErrorText - a line of Red400 body
// text under the buttons - rather than an InfoBar. The later steps still use
// InfoBars; they are not part of the login-parity port.
void LoginPage::SetInitialLoginError(hstring const& message) {
  if (message.empty()) {
    w_.LoginErrorText().Text(L"");
    w_.LoginErrorText().Visibility(Visibility::Collapsed);
    return;
  }
  w_.LoginErrorText().Text(message);
  w_.LoginErrorText().Visibility(Visibility::Visible);
}

void LoginPage::ShowLoginErrorFor(LoginStep step, hstring const& message) {
  auto show = [&message](InfoBar const& bar) {
    bar.Severity(InfoBarSeverity::Error);
    bar.Message(message);
    bar.IsOpen(true);
  };
  switch (step) {
    case LoginStep::Password: show(w_.PasswordError()); break;
    case LoginStep::Create: show(w_.CreateError()); break;
    case LoginStep::Verify: show(w_.VerifyInfo()); break;
    case LoginStep::Reset: show(w_.ResetInfo()); break;
    default: SetInitialLoginError(message); break;
  }
}

void LoginPage::OnGetStarted(IInspectable const&, RoutedEventArgs const&) {
  const std::string userAuth = TrimWhitespace(urnw::Narrow(w_.EmailBox().Text().c_str()));
  if (discoveringLogin_ || !LooksLikeUserAuth(userAuth)) return;
  discoveringLogin_ = true;
  w_.GetStartedButton().IsEnabled(false);
  SetInitialLoginError(hstring());

  auto queue = w_.DispatcherQueue();
  auto weak = w_.get_weak();
  Sdk().StartLogin(userAuth, [queue, weak](urnw::LoginRouting routing) {
    queue.TryEnqueue([weak, routing = std::move(routing)] {
      if (auto self = weak.get()) self->login().ApplyLoginRouting(routing);
    });
  });
}

void LoginPage::ApplyLoginRouting(urnw::LoginRouting const& routing) {
  discoveringLogin_ = false;
  w_.GetStartedButton().IsEnabled(true);
  switch (routing.route) {
    case urnw::LoginRoute::Login:
      // the auth state relay swaps the panel for the home view
      break;
    case urnw::LoginRoute::Password:
      loginUserAuth_ = routing.userAuth;
      w_.PasswordEmailText().Text(H(loginUserAuth_));
      w_.PasswordBox().Password(L"");
      w_.PasswordError().IsOpen(false);
      ShowLoginStep(LoginStep::Password);
      w_.PasswordBox().Focus(FocusState::Programmatic);
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

void LoginPage::OnLoginBack(IInspectable const& sender, RoutedEventArgs const&) {
  // backing out of the guest-upgrade create/verify step returns to the home
  // view: the guest session never went away
  if (createMode_ == CreateMode::GuestUpgrade && Sdk().IsLoggedIn()) {
    createMode_ = CreateMode::Password;
    ShowLoginStep(LoginStep::Initial);  // leave the flow ready for a real sign-out
    w_.ShowHomeRoot();
    return;
  }
  IInspectable tagValue{nullptr};
  if (auto element = sender.try_as<FrameworkElement>()) tagValue = element.Tag();
  const auto tag = winrt::unbox_value_or<hstring>(tagValue, L"initial");
  ShowLoginStep(tag == L"password" && !loginUserAuth_.empty() ? LoginStep::Password
                                                              : LoginStep::Initial);
}

void LoginPage::OnPasswordKeyDown(IInspectable const&,
                                  Input::KeyRoutedEventArgs const& args) {
  if (args.Key() == winrt::Windows::System::VirtualKey::Enter) {
    OnSignIn(nullptr, nullptr);
  }
}

void LoginPage::OnSignIn(IInspectable const&, RoutedEventArgs const&) {
  const std::string password = urnw::Narrow(w_.PasswordBox().Password().c_str());
  if (loginUserAuth_.empty() || password.empty()) return;
  w_.PasswordError().IsOpen(false);
  w_.SignInButton().IsEnabled(false);

  auto queue = w_.DispatcherQueue();
  auto weak = w_.get_weak();
  Sdk().LoginWithPassword(loginUserAuth_, password, [queue, weak](urnw::AuthResult r) {
    queue.TryEnqueue([weak, r] {
      auto self = weak.get();
      if (!self) return;
      self->SignInButton().IsEnabled(true);
      auto& page = self->login();
      if (r.verification_required) {
        // the account still needs its code — route into the verify step
        // instead of dead-ending on an info bar
        page.EnterVerifyStep(page.loginUserAuth_);
        self->VerifyInfo().Severity(InfoBarSeverity::Informational);
        self->VerifyInfo().Message(Loc("verification_code_sent"));
        self->VerifyInfo().IsOpen(true);
      } else if (!r.ok && !r.error.empty()) {
        page.ShowLoginErrorFor(LoginStep::Password, H(r.error));
      }
    });
  });
}

void LoginPage::OnForgotPassword(IInspectable const&, RoutedEventArgs const&) {
  if (loginUserAuth_.empty()) return;
  w_.ResetEmailText().Text(H(loginUserAuth_));
  w_.ResetInfo().IsOpen(false);
  w_.SendResetButton().IsEnabled(true);
  ShowLoginStep(LoginStep::Reset);
}

void LoginPage::OnSendResetLink(IInspectable const&, RoutedEventArgs const&) {
  if (sendingReset_ || loginUserAuth_.empty()) return;
  sendingReset_ = true;
  w_.SendResetButton().IsEnabled(false);
  w_.ResetInfo().IsOpen(false);

  auto queue = w_.DispatcherQueue();
  auto weak = w_.get_weak();
  Sdk().SendPasswordResetLink(loginUserAuth_, [queue, weak](bool ok) {
    queue.TryEnqueue([weak, ok] {
      auto self = weak.get();
      if (!self) return;
      auto& page = self->login();
      page.sendingReset_ = false;
      self->SendResetButton().IsEnabled(true);
      self->ResetInfo().Severity(ok ? InfoBarSeverity::Success
                                    : InfoBarSeverity::Error);
      // "Reset link sent to" + the address it went to (the address is data)
      self->ResetInfo().Message(ok ? hstring{urnw::Localized("reset_link_sent_to") +
                                             L" " + urnw::Widen(page.loginUserAuth_)}
                                   : Loc("something_went_wrong"));
      self->ResetInfo().IsOpen(true);
    });
  });
}

// ---- create network (sign-up) ----

void LoginPage::EnterCreateStep(std::string const& userAuth, CreateMode mode) {
  loginUserAuth_ = userAuth;
  createMode_ = mode;
  const bool walletMode = (mode == CreateMode::Wallet);
  const bool guestUpgrade = (mode == CreateMode::GuestUpgrade);
  // wallet mode: the wallet signature is the credential — name + terms only.
  // guest upgrade: the guest enters an email here (nothing was carried in).
  w_.WalletCreateNote().Visibility(walletMode ? Visibility::Visible : Visibility::Collapsed);
  w_.GuestUpgradeNote().Visibility(guestUpgrade ? Visibility::Visible : Visibility::Collapsed);
  w_.CreateEmailText().Text(H(userAuth));
  w_.CreateEmailText().Visibility(mode == CreateMode::Password ? Visibility::Visible
                                                               : Visibility::Collapsed);
  w_.CreateEmailBox().Text(L"");
  w_.CreateEmailBox().Visibility(guestUpgrade ? Visibility::Visible : Visibility::Collapsed);
  w_.CreatePasswordBox().Visibility(walletMode ? Visibility::Collapsed : Visibility::Visible);
  w_.CreatePasswordHint().Visibility(walletMode ? Visibility::Collapsed : Visibility::Visible);
  // UpgradeGuestArgs carries no referral code (the bonus only applies to a
  // fresh create — the sdk/api shape, not a UI choice): hide the bonus row
  w_.BonusCodeBox().Visibility(guestUpgrade ? Visibility::Collapsed : Visibility::Visible);
  w_.BonusStatusText().Visibility(guestUpgrade ? Visibility::Collapsed : Visibility::Visible);

  w_.CreateNameBox().Text(L"");
  w_.CreatePasswordBox().Password(L"");
  w_.TermsCheck().IsChecked(false);
  w_.BonusCodeBox().Text(L"");
  w_.BonusStatusText().Text(L"");
  kit::ApplySupportingText(w_.CreateNameStatusText(), Loc("network_name_length_error"),
                           kit::ValidationState::NotChecked);
  nameAvailable_ = false;
  nameChecking_ = false;
  ++nameCheckGeneration_;
  bonusValid_ = false;
  bonusCapped_ = false;
  ++bonusCheckGeneration_;
  w_.CreateError().IsOpen(false);
  ValidateCreateForm();
  ShowLoginStep(LoginStep::Create);
  if (guestUpgrade) {
    w_.CreateEmailBox().Focus(FocusState::Programmatic);  // the first empty field
  } else {
    w_.CreateNameBox().Focus(FocusState::Programmatic);
  }
}

void LoginPage::OnCreateNameChanged(IInspectable const&, TextChangedEventArgs const&) {
  ++nameCheckGeneration_;  // drop any availability check still in flight
  nameAvailable_ = false;
  w_.CreateError().IsOpen(false);
  if (nameCheckTimer_) nameCheckTimer_.Stop();

  const std::string name = TrimWhitespace(urnw::Narrow(w_.CreateNameBox().Text().c_str()));
  if (name.size() < kMinNetworkNameLength) {
    nameChecking_ = false;
    kit::ApplySupportingText(w_.CreateNameStatusText(), Loc("network_name_length_error"),
                             kit::ValidationState::NotChecked);
  } else {
    nameChecking_ = true;
    kit::ApplySupportingText(w_.CreateNameStatusText(), hstring(),
                             kit::ValidationState::Validating);
    if (nameCheckTimer_) nameCheckTimer_.Start();  // debounce, then check
  }
  ValidateCreateForm();
}

void LoginPage::CheckCreateNameNow() {
  const std::string name = TrimWhitespace(urnw::Narrow(w_.CreateNameBox().Text().c_str()));
  if (name.size() < kMinNetworkNameLength || !Sdk().apiReady()) return;
  const uint32_t generation = nameCheckGeneration_;

  auto queue = w_.DispatcherQueue();
  auto weak = w_.get_weak();
  Sdk().CheckNetworkName(name, [queue, weak, generation](bool ok, bool available) {
    queue.TryEnqueue([weak, generation, ok, available] {
      if (auto self = weak.get()) self->login().ApplyNameCheck(generation, ok, available);
    });
  });
}

void LoginPage::ApplyNameCheck(uint32_t generation, bool ok, bool available) {
  if (generation != nameCheckGeneration_) return;  // a later edit superseded this
  nameChecking_ = false;
  auto const line = w_.CreateNameStatusText();
  if (!ok) {
    nameAvailable_ = false;
    kit::ApplySupportingText(line, Loc("there_was_an_error_checking_the_network_name"),
                             kit::ValidationState::Invalid);
  } else if (available) {
    nameAvailable_ = true;
    kit::ApplySupportingText(line, Loc("nice_this_network_name_is_available"),
                             kit::ValidationState::Valid);
  } else {
    nameAvailable_ = false;
    kit::ApplySupportingText(line, Loc("network_name_taken"), kit::ValidationState::Invalid);
  }
  ValidateCreateForm();
}

void LoginPage::OnCreateEmailChanged(IInspectable const&, TextChangedEventArgs const&) {
  w_.CreateError().IsOpen(false);
  ValidateCreateForm();
}

void LoginPage::OnCreatePasswordChanged(IInspectable const&, RoutedEventArgs const&) {
  w_.CreateError().IsOpen(false);
  ValidateCreateForm();
}

void LoginPage::OnTermsChanged(IInspectable const&, RoutedEventArgs const&) {
  w_.CreateError().IsOpen(false);
  ValidateCreateForm();
}

void LoginPage::OnBonusCodeChanged(IInspectable const&, TextChangedEventArgs const&) {
  ++bonusCheckGeneration_;  // drop any validation still in flight
  bonusValid_ = false;
  bonusCapped_ = false;
  kit::ApplySupportingText(w_.BonusStatusText(), hstring(),
                           kit::ValidationState::NotChecked);
  if (bonusCheckTimer_) {
    bonusCheckTimer_.Stop();
    const std::string code = TrimWhitespace(urnw::Narrow(w_.BonusCodeBox().Text().c_str()));
    if (!code.empty()) bonusCheckTimer_.Start();
  }
}

void LoginPage::ValidateBonusCodeNow() {
  const std::string code = TrimWhitespace(urnw::Narrow(w_.BonusCodeBox().Text().c_str()));
  if (code.empty() || !Sdk().apiReady()) return;
  const uint32_t generation = bonusCheckGeneration_;

  urnet::ValidateReferralCodeArgs args;
  args.referral_code = code;
  auto queue = w_.DispatcherQueue();
  auto weak = w_.get_weak();
  Sdk().api().validateReferralCode(
      args, [queue, weak, generation](std::optional<urnet::ValidateReferralCodeResult> result,
                                      std::optional<std::string> err) {
        const bool ok = !err && result.has_value();
        const bool valid = ok && result->is_valid;
        const bool capped = ok && result->is_capped;
        queue.TryEnqueue([weak, generation, ok, valid, capped] {
          if (auto self = weak.get())
            self->login().ApplyBonusValidation(generation, ok, valid, capped);
        });
      });
}

void LoginPage::ApplyBonusValidation(uint32_t generation, bool ok, bool valid,
                                     bool capped) {
  if (generation != bonusCheckGeneration_) return;
  bonusValid_ = ok && valid;
  bonusCapped_ = ok && capped;
  auto const line = w_.BonusStatusText();
  if (!ok) {
    kit::ApplySupportingText(line, Loc("something_went_wrong"), kit::ValidationState::Invalid);
  } else if (bonusValid_ && !bonusCapped_) {
    kit::ApplySupportingText(line, Loc("referral_bonus_applied_2"), kit::ValidationState::Valid);
  } else if (bonusCapped_) {
    kit::ApplySupportingText(line, Loc("referral_code_capped"), kit::ValidationState::Invalid);
  } else {
    kit::ApplySupportingText(line, Loc("invalid_referral_code"), kit::ValidationState::Invalid);
  }
}

void LoginPage::ValidateCreateForm() {
  const std::string password = urnw::Narrow(w_.CreatePasswordBox().Password().c_str());
  const bool passwordOk = createMode_ == CreateMode::Wallet ||
                          password.size() >= kMinPasswordLength;
  // the guest upgrade collects the email on this step (the other modes carry a
  // discovered / wallet credential in); the server is the real validator
  const bool emailOk =
      createMode_ != CreateMode::GuestUpgrade ||
      LooksLikeUserAuth(TrimWhitespace(urnw::Narrow(w_.CreateEmailBox().Text().c_str())));
  const bool termsOk = w_.TermsCheck().IsChecked() && w_.TermsCheck().IsChecked().Value();
  w_.CreateButton().IsEnabled(nameAvailable_ && !nameChecking_ && passwordOk && emailOk &&
                              termsOk && !creatingNetwork_);
}

void LoginPage::OnCreateNetwork(IInspectable const&, RoutedEventArgs const&) {
  if (creatingNetwork_) return;
  const std::string networkName =
      TrimWhitespace(urnw::Narrow(w_.CreateNameBox().Text().c_str()));
  const std::string password = urnw::Narrow(w_.CreatePasswordBox().Password().c_str());
  creatingNetwork_ = true;
  w_.CreateButton().IsEnabled(false);
  w_.CreateError().IsOpen(false);

  auto queue = w_.DispatcherQueue();
  auto weak = w_.get_weak();
  auto done = [queue, weak](urnw::AuthResult r) {
    queue.TryEnqueue([weak, r] {
      auto self = weak.get();
      if (!self) return;
      auto& page = self->login();
      page.creatingNetwork_ = false;
      page.ValidateCreateForm();
      if (r.verification_required) {
        page.EnterVerifyStep(page.loginUserAuth_);
      } else if (!r.ok && !r.error.empty()) {
        page.ShowLoginErrorFor(LoginStep::Create, H(r.error));
      }
      // success: the auth state relay swaps the panel for the home view
    });
  };

  if (createMode_ == CreateMode::GuestUpgrade) {
    // the email entered here also drives the verify step, should one be needed
    loginUserAuth_ = TrimWhitespace(urnw::Narrow(w_.CreateEmailBox().Text().c_str()));
    Sdk().UpgradeGuest(networkName, loginUserAuth_, password, done);
    return;
  }

  urnw::CreateNetworkParams params;
  params.networkName = networkName;
  params.terms = w_.TermsCheck().IsChecked() && w_.TermsCheck().IsChecked().Value();
  if (createMode_ == CreateMode::Wallet) {
    params.useWalletAuth = true;
  } else {
    params.userAuth = loginUserAuth_;
    params.password = password;
  }
  if (bonusValid_ && !bonusCapped_) {
    params.referralCode = TrimWhitespace(urnw::Narrow(w_.BonusCodeBox().Text().c_str()));
  }
  Sdk().CreateNetwork(params, done);
}

// ---- verify code ----

void LoginPage::EnterVerifyStep(std::string const& userAuth) {
  loginUserAuth_ = userAuth;
  // "You've got mail" for an email auth; "Check your phone" for a number
  w_.VerifyHeading().Text(userAuth.find('@') != std::string::npos
                              ? Loc("login_verify_header")
                              : Loc("login_verify_check_phone"));
  w_.VerifyCodeBox().Text(L"");
  w_.VerifyButton().IsEnabled(false);
  w_.VerifyInfo().IsOpen(false);
  w_.ResendCodeButton().IsEnabled(true);
  ShowLoginStep(LoginStep::Verify);
  w_.VerifyCodeBox().Focus(FocusState::Programmatic);
}

void LoginPage::OnVerifyCodeChanged(IInspectable const&, TextChangedEventArgs const&) {
  const std::string code = TrimWhitespace(urnw::Narrow(w_.VerifyCodeBox().Text().c_str()));
  // typing dismisses a stale verdict; a programmatic clear must not close the
  // "code sent" info bar that was just raised
  if (!code.empty()) w_.VerifyInfo().IsOpen(false);
  w_.VerifyButton().IsEnabled(code.size() == kVerifyCodeLength && !verifying_);
  // a full code submits itself (macOS parity)
  if (code.size() == kVerifyCodeLength && !verifying_) SubmitVerifyCode();
}

void LoginPage::OnVerifySubmit(IInspectable const&, RoutedEventArgs const&) {
  SubmitVerifyCode();
}

void LoginPage::SubmitVerifyCode() {
  const std::string code = TrimWhitespace(urnw::Narrow(w_.VerifyCodeBox().Text().c_str()));
  if (verifying_ || code.size() != kVerifyCodeLength || loginUserAuth_.empty()) return;
  verifying_ = true;
  w_.VerifyButton().IsEnabled(false);

  auto queue = w_.DispatcherQueue();
  auto weak = w_.get_weak();
  Sdk().VerifyCode(loginUserAuth_, code, [queue, weak](urnw::AuthResult r) {
    queue.TryEnqueue([weak, r] {
      auto self = weak.get();
      if (!self) return;
      auto& page = self->login();
      page.verifying_ = false;
      if (!r.ok) {
        // clear the entered code so retyping can resubmit (macOS parity)
        self->VerifyCodeBox().Text(L"");
        page.ShowLoginErrorFor(LoginStep::Verify, Loc("verify_input_invalid"));
      }
      // success: the auth state relay swaps the panel for the home view
    });
  });
}

void LoginPage::OnResendCode(IInspectable const&, RoutedEventArgs const&) {
  if (loginUserAuth_.empty()) return;
  w_.ResendCodeButton().IsEnabled(false);
  w_.VerifyInfo().IsOpen(false);

  auto queue = w_.DispatcherQueue();
  auto weak = w_.get_weak();
  Sdk().ResendVerifyCode(loginUserAuth_, [queue, weak](bool ok) {
    queue.TryEnqueue([weak, ok] {
      auto self = weak.get();
      if (!self) return;
      auto& page = self->login();
      if (ok) {
        self->VerifyInfo().Severity(InfoBarSeverity::Success);
        self->VerifyInfo().Message(Loc("verification_code_sent"));
        self->VerifyInfo().IsOpen(true);
        // 15s cooldown before another resend (macOS parity)
        if (page.resendCooldownTimer_) page.resendCooldownTimer_.Start();
      } else {
        self->ResendCodeButton().IsEnabled(true);
        page.ShowLoginErrorFor(LoginStep::Verify, Loc("something_went_wrong"));
      }
    });
  });
}

// ---- auth code login ----

// android presents AuthCodeLoginSheet - a modal with its own field - instead of
// an inline box on the login screen. A ContentDialog is the desktop equivalent.
winrt::fire_and_forget LoginPage::OnUseCode(IInspectable const&, RoutedEventArgs const&) {
  if (w_.sheetOpen()) co_return;  // only one ContentDialog can show at a time
  auto self = w_.get_strong();    // keeps the window — and so this page — alive
  SetInitialLoginError(hstring());

  TextBox field;
  field.PlaceholderText(Loc("auth_code"));
  if (auto style = Application::Current()
                       .Resources()
                       .TryLookup(winrt::box_value(L"UrTextInputStyle"))
                       .try_as<Style>()) {
    field.Style(style);
  }

  ContentDialog dialog;
  dialog.XamlRoot(self->Content().XamlRoot());
  dialog.Title(winrt::box_value(Loc("auth_code_login_sheet_header")));
  dialog.Content(field);
  dialog.PrimaryButtonText(Loc("auth_code_login_button_text"));
  dialog.CloseButtonText(Loc("cancel"));
  dialog.DefaultButton(ContentDialogButton::Primary);

  w_.SetSheetOpen(true);
  ContentDialogResult result{ContentDialogResult::None};
  try {
    result = co_await dialog.ShowAsync();
  } catch (...) {
  }
  w_.SetSheetOpen(false);
  if (result != ContentDialogResult::Primary) co_return;

  const std::string code = TrimWhitespace(urnw::Narrow(field.Text().c_str()));
  if (code.empty()) co_return;

  SetWalletSignInEnabled(false);
  auto queue = self->DispatcherQueue();
  auto weak = self->get_weak();
  Sdk().LoginWithCode(code, [queue, weak](urnw::AuthResult r) {
    queue.TryEnqueue([weak, r] {
      auto self = weak.get();
      if (!self) return;
      auto& page = self->login();
      page.SetWalletSignInEnabled(true);
      if (!r.ok && !r.error.empty()) {
        page.ShowLoginErrorFor(LoginStep::Initial, H(r.error));
      }
    });
  });
}

// ---- guest mode (macOS GuestModeSheet parity) ------------------------------
// One tap creates a throwaway network: the sheet collects the terms consent,
// SdkHost::LoginAsGuest creates and registers it, and the auth-state relay
// swaps the panel for the home view. The plan cards later offer the upgrade to
// a full account (BeginGuestUpgrade).

void LoginPage::OnTryGuestMode(IInspectable const&, RoutedEventArgs const&) {
  SetInitialLoginError(hstring());
  ShowGuestModeSheet();
}

winrt::fire_and_forget LoginPage::ShowGuestModeSheet() {
  if (w_.sheetOpen()) co_return;  // only one ContentDialog can show at a time
  auto self = w_.get_strong();
  w_.SetSheetOpen(true);
  try {
    guestSheet_ = urnw::GuestModeSheet::Create(self->Content().XamlRoot(), Sdk());
    co_await guestSheet_->Dialog().ShowAsync();
  } catch (...) {
  }
  guestSheet_.reset();
  w_.SetSheetOpen(false);
}

// ---- wallet sign in ------------------------------------------------------
// Both wallets sign in through the ur.io/wallet-connect browser bridge (desktop
// wallets are browser extensions): the bridge drives the wallet and returns via
// the urnetwork:// scheme, which protocol activation routes back into SdkHost
// (main.cpp -> App::OnLaunched -> AppController::HandleDeepLink). `done` fires on
// an SDK thread, so hop to the UI thread before touching the panel.

void LoginPage::OnSignInWithBittensor(IInspectable const&, RoutedEventArgs const&) {
  SetInitialLoginError(hstring());
  SetWalletSignInEnabled(false);
  auto queue = w_.DispatcherQueue();
  auto weak = w_.get_weak();
  Sdk().SignInWithBittensor([queue, weak](urnw::AuthResult r) {
    queue.TryEnqueue([weak, r] {
      if (auto self = weak.get()) self->login().ApplyWalletSignInResult(r);
    });
  });
}

// android shows ONE "Sign in with Solana" button and lets Mobile Wallet Adapter
// put up the wallet picker. The browser bridge has no such picker - it needs the
// provider baked into the deeplink it opens - so ask here, which keeps the
// button list identical to android's and still lets the user choose.
winrt::fire_and_forget LoginPage::OnSignInWithSolana(IInspectable const&,
                                                     RoutedEventArgs const&) {
  if (w_.sheetOpen()) co_return;  // only one ContentDialog can show at a time
  auto self = w_.get_strong();
  SetInitialLoginError(hstring());

  ContentDialog dialog;
  dialog.XamlRoot(self->Content().XamlRoot());
  dialog.Title(winrt::box_value(Loc("solana_sign_in")));
  dialog.PrimaryButtonText(Loc("phantom"));
  dialog.SecondaryButtonText(Loc("solflare"));
  dialog.CloseButtonText(Loc("cancel"));
  dialog.DefaultButton(ContentDialogButton::Primary);

  w_.SetSheetOpen(true);
  ContentDialogResult result{ContentDialogResult::None};
  try {
    result = co_await dialog.ShowAsync();
  } catch (...) {
  }
  w_.SetSheetOpen(false);
  if (result == ContentDialogResult::None) co_return;

  const auto provider = (result == ContentDialogResult::Secondary)
                            ? urnw::WalletConnect::Provider::Solflare
                            : urnw::WalletConnect::Provider::Phantom;
  SetWalletSignInEnabled(false);
  auto queue = self->DispatcherQueue();
  auto weak = self->get_weak();
  Sdk().SignInWithSolana(provider, [queue, weak](urnw::AuthResult r) {
    queue.TryEnqueue([weak, r] {
      if (auto self = weak.get()) self->login().ApplyWalletSignInResult(r);
    });
  });
}

// android disables every sign-in affordance while any one of them is in
// flight (isLoginInProgress), not just the wallet pair
void LoginPage::SetWalletSignInEnabled(bool enabled) {
  w_.BittensorSignInButton().IsEnabled(enabled);
  w_.SolanaSignInButton().IsEnabled(enabled);
  w_.AuthCodeButton().IsEnabled(enabled);
  w_.GetStartedButton().IsEnabled(enabled);
}

void LoginPage::ApplyWalletSignInResult(urnw::AuthResult const& result) {
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

}  // namespace urnw
