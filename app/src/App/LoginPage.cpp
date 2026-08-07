// SPDX-License-Identifier: MPL-2.0
#include "pch.h"

#include "LoginPage.h"

#include <chrono>
#include <cmath>

#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Windows.System.h>

#include "Log.h"
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

// The carousel's slot is elastic (LoginPage::ApplyLoginLayout): it never grows
// past the first number, and below the second it is hidden rather than shown
// as a squashed sliver.
constexpr double kCarouselMaxHeight = 200;
constexpr double kCarouselMinHeight = 80;   // hide below this once shown
constexpr double kCarouselShowHeight = 96;  // show again only above this

// A BIP-39 seedphrase is 12 or 24 words; nothing between is valid and the
// server would only reject it (macOS LoginSeedphraseViewModel.isSeedphraseValid).
constexpr size_t kShortSeedphraseWords = 12;
constexpr size_t kLongSeedphraseWords = 24;

// How many whitespace-separated words are in `value`. Counting rather than
// splitting: the phrase itself is a credential and is not copied around here.
size_t CountWords(std::string const& value) {
  size_t count = 0;
  bool inWord = false;
  for (unsigned char c : value) {
    const bool space = (c == ' ' || c == '\t' || c == '\r' || c == '\n');
    if (!space && !inWord) ++count;
    inWord = !space;
  }
  return count;
}
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

  // window-level acknowledgements (the account menu's referral copy)
  snackbar_ = std::make_unique<urnw::kit::Snackbar>(w_.AccountSnackbar(), queue);

  // the hero carousel; ApplyStrings has already run, so paint its first slide
  carousel_ = std::make_unique<urnw::LoginCarousel>(w_.LoginCarouselHost(), queue);
  carousel_->ApplyStrings();

  // Keep the sign-in affordances on screen (see ApplyLoginLayout). Both hooks
  // are needed: the ScrollViewer tells us how much room there is, and the panel
  // tells us how much the content wants — which changes with the window WIDTH
  // as the terms sentence and the error line re-wrap, not only with its height.
  w_.LoginRoot().SizeChanged([weak = w_.get_weak()](auto const&, auto const&) {
    if (auto self = weak.get()) self->login().ApplyLoginLayout();
  });
  w_.LoginPanel().SizeChanged([weak = w_.get_weak()](auto const&, auto const&) {
    if (auto self = weak.get()) self->login().ApplyLoginLayout();
  });
  ApplyLoginLayout();
}

// THE THREE AFFORDANCES THE OWNER ASKED FOR BY NAME — Sign in with Seedphrase,
// Create Instant Account and Change Network API — MUST BE REACHABLE WITHOUT
// SCROLLING. They are the last three things on the initial step, so they are
// the first three things a too-tall column loses, and the carousel's fixed
// 200px slot was enough on its own to lose all three: at the ~480x760 default
// and still at 1300x800, they sat below the fold.
//
// The fix is an order of precedence rather than a magic number. The affordances
// are content and get their height first; the carousel is the hero and takes
// whatever is left, down to nothing. Everything is measured, so it holds at
// window sizes nobody tried — which matters here because the shell PERSISTS
// window placement, so "correct at the default" is not correct.
void LoginPage::ApplyLoginLayout() {
  auto root = w_.LoginRoot();
  auto panel = w_.LoginPanel();
  auto host = w_.LoginCarouselHost();
  if (!root || !panel || !host) return;
  // Only the initial step owns this panel; the later steps collapse it.
  if (panel.Visibility() != Visibility::Visible) return;
  if (inLayoutPass_) return;  // re-entrancy: setting a height raises SizeChanged

  const double viewport =
      root.ViewportHeight() > 0 ? root.ViewportHeight() : root.ActualHeight();
  if (viewport <= 0) return;

  // What the column needs WITHOUT the hero, from the children's DESIRED sizes
  // and NOT from panel.ActualHeight() - host.ActualHeight().
  //
  // The ActualHeight form is the obvious one and it is a layout cycle: both
  // terms move while the pass this runs inside is still settling, so `rest`
  // jittered, `want` jittered with it, and every SizeChanged set a new height
  // that provoked the next. XAML caught it exactly as it should —
  // "Layout cycle detected. Layout could not complete." — and took the process
  // with it on the first resize. Desired sizes of the OTHER children do not
  // depend on what the hero's height is, so this converges in one pass.
  const double spacing = panel.Spacing();
  double rest = 0;
  int visibleOthers = 0;
  for (auto const& child : panel.Children()) {
    auto element = child.try_as<FrameworkElement>();
    if (!element || element.Visibility() != Visibility::Visible) continue;
    if (element == host.try_as<FrameworkElement>()) continue;
    rest += element.DesiredSize().Height;
    ++visibleOthers;
  }
  if (visibleOthers == 0 || rest <= 0) return;  // nothing measured yet

  const double margins = panel.Margin().Top + panel.Margin().Bottom;
  const double hostMargins = host.Margin().Top + host.Margin().Bottom;
  // gaps between the others, plus the one the hero would add
  const double gaps = spacing * visibleOthers;
  double want = viewport - rest - gaps - margins - hostMargins;
  if (want < 0) want = 0;
  if (want > kCarouselMaxHeight) want = kCarouselMaxHeight;

  // A sliver of globe with a headline crushed on top of it is worse than no
  // hero at all, so below the floor it goes away entirely — collapsing it, not
  // just zeroing it, because a Grid does not clip and a zero-height slot would
  // spill its headline over the field below. Hysteresis between the two
  // thresholds: hiding the slot also removes its StackPanel gap, which gives
  // `want` back more than the gap is worth and would otherwise flap.
  const bool showing = host.Visibility() == Visibility::Visible;
  const bool show = showing ? (want >= kCarouselMinHeight) : (want >= kCarouselShowHeight);

  inLayoutPass_ = true;
  if (showing != show) host.Visibility(show ? Visibility::Visible : Visibility::Collapsed);
  // Epsilon, so an unchanged answer does not write the property at all.
  if (show && std::abs(host.Height() - want) > 0.5) host.Height(want);
  inLayoutPass_ = false;
  UpdateCarouselRunning();
}

void LoginPage::SetPresentationActive(bool active) {
  presentationActive_ = active;
  UpdateCarouselRunning();
}

// Only the initial step shows the carousel; the later steps collapse the whole
// panel, and a short window collapses the slot itself (ApplyLoginLayout). In
// any of those cases the timer would be animating a tree nobody can see.
void LoginPage::UpdateCarouselRunning() {
  if (!carousel_) return;
  const bool slotVisible = w_.LoginCarouselHost() &&
                           w_.LoginCarouselHost().Visibility() == Visibility::Visible;
  carousel_->SetActive(presentationActive_ && loginStep_ == LoginStep::Initial &&
                       slotVisible);
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
  w_.GoogleSignInText().Text(Loc("sign_in_with_google"));
  w_.BittensorSignInText().Text(Loc("bittensor_sign_in"));
  w_.SolanaSignInText().Text(Loc("solana_sign_in"));
  // SdkHost::LoginWithCode is the auth-code login the other platforms ship
  w_.AuthCodeButtonText().Text(Loc("auth_code_login_button_text"));
  // The seedphrase pair (macOS LoginSeedphrase / CreateNetworkInstant). Every
  // seedphrase / instant-account string was ABSENT from the shared store —
  // macOS hardcodes all fourteen as Swift literals, so `npm run gen` has never
  // seen them — and they were added to Strings/en/Resources.resw here. They
  // still have to land in urnetwork/localizations for the other 27 locales;
  // see this branch's report.
  w_.SeedphraseSignInButton().Content(LocBox("sign_in_with_seedphrase"));
  w_.InstantAccountButton().Content(LocBox("create_instant_account"));
  // bottom-left, quiet text: point the client at another network API
  w_.NetworkServerLink().Content(LocBox("change_network_api"));
  // MainWindow calls ApplyStrings BEFORE Initialize, so on the first pass there
  // is no carousel yet; Initialize paints it once it exists.
  if (carousel_) carousel_->ApplyStrings();

  UpdateGoogleSignInVisibility();

  // sign in — seedphrase step
  w_.SeedphraseBackButton().Content(LocBox("back"));
  w_.SeedphraseHeading().Text(Loc("sign_in_with_seedphrase"));
  w_.SeedphraseBox().PlaceholderText(Loc("seedphrase_input_placeholder"));
  w_.SeedphraseSubmitButton().Content(LocBox("sign_in"));

  // sign in — instant account step
  w_.InstantBackButton().Content(LocBox("back"));
  w_.InstantHeading().Text(Loc("create_instant_account"));
  w_.InstantExplanationText().Text(Loc("instant_account_explainer"));
  urnw::SetTermsMarkerText(w_.InstantTermsText(), urnw::Localized("terms_checkbox"), 12);
  // after SetTermsMarkerText: it is the inlines that call built which get put
  // back into the content view (see PairTermsLabel)
  urnw::PairTermsLabel(w_.InstantTermsCheck(), w_.InstantTermsText());
  w_.InstantCreateButton().Content(LocBox("create_account_2"));

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
  urnw::PairTermsLabel(w_.TermsCheck(), w_.TermsText());
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

// Whether "Sign in with Google" is offered at all.
//
// SdkHost::SsoGoogleEnabled() is the purpose-built answer — GoogleSignIn::
// Configured() (is an OAuth client id compiled in?) AND the active network
// space's getSsoGoogle() (does this server offer it?). It had ZERO callers;
// the gate here was apiReady(), which is api_.has_value(), true from SDK init
// on every build. The result was a Google button shipped visible and always
// failing in every default build, and three comments (GoogleSignIn.h,
// Config.h, SdkHost::SignInWithGoogle) that all described the opposite. Those
// three were right, so this now agrees with them.
//
// Called from ApplyStrings AND after a network-server switch: the space is
// what supplies half the answer, and switching spaces replaces it.
void LoginPage::UpdateGoogleSignInVisibility() {
  const bool enabled = Sdk().SsoGoogleEnabled();
  w_.GoogleSignInButton().Visibility(enabled ? Visibility::Visible
                                             : Visibility::Collapsed);
  // The button's content is a Viewbox + a TextBlock inside a StackPanel, not a
  // string, so ContentControl derived NO name from it and UIA announced it as
  // an unnamed button. Every other pill on this screen has the same shape.
  Automation::AutomationProperties::SetName(w_.GoogleSignInButton(),
                                            Loc("sign_in_with_google"));
  Automation::AutomationProperties::SetName(w_.BittensorSignInButton(),
                                            Loc("bittensor_sign_in"));
  Automation::AutomationProperties::SetName(w_.SolanaSignInButton(),
                                            Loc("solana_sign_in"));
  Automation::AutomationProperties::SetName(w_.AuthCodeButton(),
                                            Loc("auth_code_login_button_text"));
}

// ---- window-level calls ----------------------------------------------------

// "This app is not carrying traffic, and here is why." The message is composed
// by SdkHost and is already a complete sentence in the app's own words — it is
// NOT from the localization store, which is why nothing is looked up here and
// why this adds no new store key.
void LoginPage::ShowModeNotice(hstring const& message, bool failed) {
  if (!snackbar_ || message.empty()) return;
  snackbar_->Show(message, failed ? InfoBarSeverity::Error
                                  : InfoBarSeverity::Informational);
}

void LoginPage::ResetToInitialStep() {
  // Anything that resets the flow — a sign-out, a network-server switch —
  // must not leave a seedphrase behind in the field for the next person at
  // this machine, or for anything reading the UIA tree.
  ClearSeedphraseField();
  ShowLoginStep(LoginStep::Initial);
}

// The one place the credential field is emptied, so every caller gets the same
// treatment. TextBox has no "zero the backing buffer" API — the hstring it
// holds is immutable and refcounted — so replacing the value is the whole of
// what is available here.
void LoginPage::ClearSeedphraseField() {
  if (w_.SeedphraseBox()) w_.SeedphraseBox().Text(L"");
}

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
  w_.SeedphrasePanel().Visibility(step == LoginStep::Seedphrase ? Visibility::Visible
                                                                : Visibility::Collapsed);
  w_.InstantPanel().Visibility(step == LoginStep::Instant ? Visibility::Visible
                                                          : Visibility::Collapsed);
  UpdateCarouselRunning();
  if (step == LoginStep::Initial) ApplyLoginLayout();
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
  // the seedphrase / instant steps use android's URInlineErrorText, like the
  // initial step, rather than an InfoBar
  auto showInline = [&message](TextBlock const& line) {
    line.Text(message);
    line.Visibility(message.empty() ? Visibility::Collapsed : Visibility::Visible);
  };
  switch (step) {
    case LoginStep::Password: show(w_.PasswordError()); break;
    case LoginStep::Create: show(w_.CreateError()); break;
    case LoginStep::Verify: show(w_.VerifyInfo()); break;
    case LoginStep::Reset: show(w_.ResetInfo()); break;
    case LoginStep::Seedphrase: showInline(w_.SeedphraseErrorText()); break;
    case LoginStep::Instant: showInline(w_.InstantErrorText()); break;
    default: SetInitialLoginError(message); break;
  }
}

// Get started is gated on the field having something in it (iOS/android): an
// enabled primary button over an empty field promises an action that cannot
// happen. The shape check stays where it was — on submit — so a half-typed
// address does not flicker the button on and off as it is entered.
void LoginPage::UpdateGetStartedEnabled() {
  const std::string value = TrimWhitespace(urnw::Narrow(w_.EmailBox().Text().c_str()));
  w_.GetStartedButton().IsEnabled(!value.empty() && !discoveringLogin_);
}

void LoginPage::OnUserAuthChanged(IInspectable const&, TextChangedEventArgs const&) {
  UpdateGetStartedEnabled();
  SetInitialLoginError(hstring());
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
  UpdateGetStartedEnabled();
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
  // wallet auth and an SSO id token are the same shape on this step: the
  // credential is already held by SdkHost, so all the form collects is a
  // network name and the terms consent.
  const bool walletMode = (mode == CreateMode::Wallet || mode == CreateMode::AuthJwt);
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
                          createMode_ == CreateMode::AuthJwt ||
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
  } else if (createMode_ == CreateMode::AuthJwt) {
    params.useAuthJwt = true;
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
  } catch (winrt::hresult_error const& e) {
    urnw::LogError("auth code sheet: {} (0x{:08x})",
                   urnw::Narrow(std::wstring{e.message()}),
                   static_cast<uint32_t>(e.code()));
  } catch (std::exception const& e) {
    urnw::LogError("auth code sheet: {}", e.what());
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
  } catch (winrt::hresult_error const& e) {
    urnw::LogError("guest mode sheet: {} (0x{:08x})",
                   urnw::Narrow(std::wstring{e.message()}),
                   static_cast<uint32_t>(e.code()));
  } catch (std::exception const& e) {
    urnw::LogError("guest mode sheet: {}", e.what());
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
  } catch (winrt::hresult_error const& e) {
    urnw::LogError("solana wallet picker: {} (0x{:08x})",
                   urnw::Narrow(std::wstring{e.message()}),
                   static_cast<uint32_t>(e.code()));
  } catch (std::exception const& e) {
    urnw::LogError("solana wallet picker: {}", e.what());
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
  w_.GoogleSignInButton().IsEnabled(enabled);
  w_.SeedphraseSignInButton().IsEnabled(enabled);
  w_.InstantAccountButton().IsEnabled(enabled);
  // NOT a flat `IsEnabled(enabled)`: Get started also depends on the field
  // having something in it, and writing true here re-enabled it over an empty
  // box every time another sign-in method finished.
  if (enabled) {
    UpdateGetStartedEnabled();
  } else {
    w_.GetStartedButton().IsEnabled(false);
  }
}

void LoginPage::ApplyWalletSignInResult(urnw::AuthResult const& result) {
  SetWalletSignInEnabled(true);
  // the wallet authenticated but has no network yet: finish sign-up with a
  // network name + terms; the retained wallet auth is the credential
  if (result.wallet_needs_network) {
    EnterCreateStep(std::string(), CreateMode::Wallet);
    return;
  }
  // the same shape for an SSO identity, with the retained id token instead
  if (result.auth_needs_network) {
    EnterCreateStep(std::string(), CreateMode::AuthJwt);
    return;
  }
  // on success ApplyAuthState swaps the panel for the home view; only an error
  // needs to be surfaced here
  if (result.ok || result.error.empty()) return;
  ShowLoginErrorFor(LoginStep::Initial, H(result.error));
}

// ---- Sign in with Google (system browser) ----------------------------------
// The round trip is GoogleSignIn's: it opens the browser, waits on a loopback
// socket and exchanges the code. Everything here does is disable the sign-in
// affordances while that is happening and surface whatever comes back.

void LoginPage::OnSignInWithGoogle(IInspectable const&, RoutedEventArgs const&) {
  SetInitialLoginError(hstring());
  SetWalletSignInEnabled(false);
  auto queue = w_.DispatcherQueue();
  auto weak = w_.get_weak();
  Sdk().SignInWithGoogle([queue, weak](urnw::AuthResult r) {
    queue.TryEnqueue([weak, r] {
      // ApplyWalletSignInResult already handles "authenticated but no network
      // yet" for both credentials and re-enables the buttons.
      if (auto self = weak.get()) self->login().ApplyWalletSignInResult(r);
    });
  });
}

// ---- Sign in with a seedphrase (macOS LoginSeedphraseView) -----------------

void LoginPage::OnSignInWithSeedphrase(IInspectable const&, RoutedEventArgs const&) {
  SetInitialLoginError(hstring());
  ClearSeedphraseField();
  w_.SeedphraseErrorText().Visibility(Visibility::Collapsed);
  seedphraseLoggingIn_ = false;
  ValidateSeedphrase();
  ShowLoginStep(LoginStep::Seedphrase);
  w_.SeedphraseBox().Focus(FocusState::Programmatic);
}

void LoginPage::OnSeedphraseChanged(IInspectable const&, TextChangedEventArgs const&) {
  w_.SeedphraseErrorText().Visibility(Visibility::Collapsed);
  ValidateSeedphrase();
}

void LoginPage::ValidateSeedphrase() {
  const size_t words = CountWords(urnw::Narrow(w_.SeedphraseBox().Text().c_str()));
  const bool valid = (words == kShortSeedphraseWords || words == kLongSeedphraseWords);
  w_.SeedphraseSubmitButton().IsEnabled(valid && !seedphraseLoggingIn_);

  auto const line = w_.SeedphraseWordCountText();
  if (words == 0) {
    // nothing typed yet: no verdict to give
    kit::ApplySupportingText(line, hstring(), kit::ValidationState::NotChecked);
  } else if (valid) {
    kit::ApplySupportingText(line, hstring(), kit::ValidationState::Valid);
  } else {
    // The count is the whole diagnostic — "invalid seedphrase" would not tell
    // anyone that they pasted 23 words.
    kit::ApplySupportingText(
        line, hstring{urnw::Format("seedphrase_word_count_warning", words)},
        kit::ValidationState::Invalid);
  }
}

void LoginPage::OnSeedphraseSubmit(IInspectable const&, RoutedEventArgs const&) {
  // The phrase leaves the box, goes to the SDK and is not retained here.
  const std::string phrase = urnw::Narrow(w_.SeedphraseBox().Text().c_str());
  const size_t words = CountWords(phrase);
  if (seedphraseLoggingIn_ ||
      (words != kShortSeedphraseWords && words != kLongSeedphraseWords)) {
    return;
  }
  seedphraseLoggingIn_ = true;
  w_.SeedphraseSubmitButton().IsEnabled(false);
  w_.SeedphraseErrorText().Visibility(Visibility::Collapsed);
  // CLEAR THE FIELD NOW, not on the next entry to this step. A TextBox holds
  // its value for any process that asks UIA for it — read back verbatim from
  // an unrelated unprivileged process as `Edit id='SeedphraseBox' VALUE=[...]`,
  // and writable through the same pattern. A successful sign-in never returns
  // to this step, so "cleared on re-entry" meant "never cleared". The cost is
  // that a rejected phrase has to be pasted again; a credential sitting in an
  // automation-readable control is not a trade worth making.
  ClearSeedphraseField();

  auto queue = w_.DispatcherQueue();
  auto weak = w_.get_weak();
  Sdk().LoginWithSeedphrase(phrase, [queue, weak](urnw::AuthResult r) {
    queue.TryEnqueue([weak, r] {
      auto self = weak.get();
      if (!self) return;
      auto& page = self->login();
      page.seedphraseLoggingIn_ = false;
      page.ValidateSeedphrase();
      if (!r.ok) {
        page.ShowLoginErrorFor(LoginStep::Seedphrase,
                               r.error.empty() ? Loc("seedphrase_login_failed") : H(r.error));
      }
      // success: the auth state relay swaps the panel for the home view
    });
  });
}

// ---- Create an instant account (macOS CreateNetworkInstantView) -------------
// networkCreate with nothing but the terms consent mints a network whose only
// credential is a seedphrase. The seedphrase sheet is shown BEFORE the device
// is registered (SdkHost::CreateInstantAccount / ConfirmInstantAccount), so a
// dismissed sheet cannot leave a signed-in account nobody can ever recover.

void LoginPage::OnCreateInstantAccount(IInspectable const&, RoutedEventArgs const&) {
  SetInitialLoginError(hstring());
  w_.InstantTermsCheck().IsChecked(false);
  w_.InstantTermsCheck().IsEnabled(true);
  w_.InstantErrorText().Visibility(Visibility::Collapsed);
  creatingInstant_ = false;
  w_.InstantCreateButton().IsEnabled(false);
  ShowLoginStep(LoginStep::Instant);
}

void LoginPage::OnInstantTermsChanged(IInspectable const&, RoutedEventArgs const&) {
  const bool agreed =
      w_.InstantTermsCheck().IsChecked() && w_.InstantTermsCheck().IsChecked().Value();
  w_.InstantCreateButton().IsEnabled(agreed && !creatingInstant_);
  w_.InstantErrorText().Visibility(Visibility::Collapsed);
}

void LoginPage::OnCreateInstantSubmit(IInspectable const&, RoutedEventArgs const&) {
  const bool agreed =
      w_.InstantTermsCheck().IsChecked() && w_.InstantTermsCheck().IsChecked().Value();
  if (creatingInstant_ || !agreed || !Sdk().apiReady()) return;
  creatingInstant_ = true;
  w_.InstantCreateButton().IsEnabled(false);
  w_.InstantTermsCheck().IsEnabled(false);
  w_.InstantErrorText().Visibility(Visibility::Collapsed);

  auto queue = w_.DispatcherQueue();
  auto weak = w_.get_weak();
  Sdk().CreateInstantAccount([queue, weak](urnw::SdkHost::InstantAccount account) {
    // `mutable`: the captured account carries the credential, and this lambda
    // is the last thing holding it once the sheet has taken its own copy.
    queue.TryEnqueue([weak, account = std::move(account)]() mutable {
      auto self = weak.get();
      if (!self) return;
      auto& page = self->login();
      page.creatingInstant_ = false;
      self->InstantTermsCheck().IsEnabled(true);
      const bool agreed = self->InstantTermsCheck().IsChecked() &&
                          self->InstantTermsCheck().IsChecked().Value();
      self->InstantCreateButton().IsEnabled(agreed);
      if (!account.ok) {
        page.ShowLoginErrorFor(LoginStep::Instant, account.error.empty()
                                                       ? Loc("instant_account_failed")
                                                       : H(account.error));
        return;
      }
      page.ShowSeedphraseSheet(account.seedphrase);
      account.seedphrase.assign(account.seedphrase.size(), '\0');
      account.seedphrase.clear();
    });
  });
}

void LoginPage::ShowPreviewSeedphraseSheet() {
  // EnterPreviewUi runs while the window is still being set up: the content
  // exists but is not in a tree yet, so Content().XamlRoot() is NULL and
  // ContentDialog::ShowAsync throws "This element does not have a XamlRoot"
  // — into the catch in ShowSeedphraseSheet, which is why the sheet silently
  // never appeared. Posting to the dispatcher was not enough; the root is
  // attached at Loaded, so wait for that. (The real path opens from a button
  // click long after load and never sees this.)
  //
  // The phrase is the all-"abandon" vector from BIP-39's own test suite,
  // ending in "about": it is printed in the specification, every wallet
  // library ships it as a fixture, and it secures nothing anywhere. It is not
  // a credential and there is nothing here to leak.
  static constexpr const char* kTestVector =
      "abandon abandon abandon abandon abandon abandon abandon abandon "
      "abandon abandon abandon about";

  auto root = w_.Content().try_as<FrameworkElement>();
  if (root && !w_.Content().XamlRoot()) {
    auto token = std::make_shared<winrt::event_token>();
    *token = root.Loaded([weak = w_.get_weak(), root, token](auto const&, auto const&) {
      root.Loaded(*token);  // once
      if (auto self = weak.get()) self->login().ShowSeedphraseSheet(kTestVector);
    });
    return;
  }
  ShowSeedphraseSheet(kTestVector);
}

winrt::fire_and_forget LoginPage::ShowSeedphraseSheet(std::string seedphrase) {
  if (w_.sheetOpen()) {
    // Nothing else can be open on this screen, but if it somehow is, the
    // account must not be silently abandoned with its phrase unread.
    Sdk().DiscardInstantAccount();
    ShowLoginErrorFor(LoginStep::Instant, Loc("something_went_wrong"));
    co_return;
  }
  auto self = w_.get_strong();
  auto weak = w_.get_weak();
  w_.SetSheetOpen(true);
  // shared, not a captured local: the confirm callback fires while this
  // coroutine is suspended inside ShowAsync, and a reference into a coroutine
  // frame is exactly the kind of lifetime nobody should have to reason about.
  auto confirmed = std::make_shared<bool>(false);
  try {
    seedphraseSheet_ = urnw::SeedphraseDisplaySheet::Create(
        self->Content().XamlRoot(), seedphrase,
        [weak] {
          if (auto self = weak.get()) {
            self->login().snackbar_->Show(Loc("seedphrase_copied_to_clipboard"),
                                          InfoBarSeverity::Success);
          }
        },
        [confirmed] {
          *confirmed = true;
          // Only now does a session exist. The auth-state relay swaps the
          // panel for the home view when registration lands.
          Sdk().ConfirmInstantAccount([](urnw::AuthResult) {});
        });
    co_await seedphraseSheet_->Dialog().ShowAsync();
  } catch (winrt::hresult_error const& e) {
    urnw::LogError("seedphrase sheet: {} (0x{:08x})", urnw::Narrow(std::wstring{e.message()}),
                   static_cast<uint32_t>(e.code()));
  } catch (std::exception const& e) {
    urnw::LogError("seedphrase sheet: {}", e.what());
  }
  seedphraseSheet_.reset();
  w_.SetSheetOpen(false);
  // The phrase was passed in by value; this frame is the app's last copy of it
  // outside the sheet, which is already gone.
  seedphrase.assign(seedphrase.size(), '\0');
  seedphrase.clear();
  if (!*confirmed) {
    // The sheet went away without a confirmation. The sheet itself now refuses
    // Esc (SeedphraseDisplaySheet's Closing handler), so this is the residual
    // path only — a torn-down XamlRoot, or a ShowAsync that threw. Drop the
    // pending jwt AND SAY SO: the account is gone, and the previous silence
    // dumped the user back onto the Instant panel with the terms still ticked
    // and no clue that anything had happened.
    Sdk().DiscardInstantAccount();
    ShowLoginErrorFor(LoginStep::Instant, Loc("instant_account_failed"));
  }
}

// ---- Change Network API (iOS NetworkServerSheet) ---------------------------

winrt::fire_and_forget LoginPage::OnChangeNetworkServer(IInspectable const&,
                                                        RoutedEventArgs const&) {
  if (w_.sheetOpen()) co_return;  // only one ContentDialog can show at a time
  auto self = w_.get_strong();
  w_.SetSheetOpen(true);
  try {
    networkServerSheet_ =
        urnw::NetworkServerSheet::Create(self->Content().XamlRoot(), Sdk());
    co_await networkServerSheet_->Dialog().ShowAsync();
  } catch (winrt::hresult_error const& e) {
    urnw::LogError("network server sheet: {} (0x{:08x})",
                   urnw::Narrow(std::wstring{e.message()}),
                   static_cast<uint32_t>(e.code()));
  } catch (std::exception const& e) {
    urnw::LogError("network server sheet: {}", e.what());
  }
  networkServerSheet_.reset();
  w_.SetSheetOpen(false);
  // A switch re-derives the Api and the LocalState, so the flow starts over on
  // whatever the new server says about this client — including whether that
  // server offers Google SSO, which is otherwise only read once at startup.
  UpdateGoogleSignInVisibility();
  ResetToInitialStep();
}

// ---- Account menu (iOS Shared/Views/AccountMenu.swift) ----------------------

void LoginPage::ApplyAccountIdentity(std::string const& networkName, bool guest, bool pro,
                                     bool signedIn) {
  // The avatar only exists for a session: signed out there is no identity to
  // show and no action in the menu that would make sense.
  w_.AccountMenuButton().Visibility(signedIn ? Visibility::Visible : Visibility::Collapsed);
  if (!signedIn) return;
  // The button's content is an avatar with no text, so like the terms
  // checkboxes it had NO accessible name and did not appear in the UIA tree at
  // all — it named the destination it opens, and nothing announced it.
  Automation::AutomationProperties::SetName(w_.AccountMenuButton(), Loc("account"));
  // PersonPicture derives its initials from DisplayName; a guest has no
  // network name to derive from and gets the store's word for it.
  w_.AccountAvatar().DisplayName(networkName.empty() ? Loc("guest") : H(networkName));
  w_.AccountProRing().Stroke(urnw::colors::ProGoldBrush());
  w_.AccountProRing().Visibility(pro ? Visibility::Visible : Visibility::Collapsed);
  accountGuest_ = guest;
  accountNetworkName_ = networkName;
}

void LoginPage::OnAccountMenu(IInspectable const&, RoutedEventArgs const&) {
  auto weak = w_.get_weak();
  urnw::AccountMenuActions actions;
  if (accountGuest_) {
    actions.onCreateAccount = [weak] {
      if (auto self = weak.get()) self->login().BeginGuestUpgrade();
    };
  }
  actions.onSignOut = [] { Sdk().Logout(); };
  actions.onShared = [weak] {
    if (auto self = weak.get()) {
      self->login().snackbar_->Show(Loc("bonus_referral_code_copied_to_clipboard"),
                                    InfoBarSeverity::Success);
    }
  };
  urnw::ShowAccountMenu(w_.AccountMenuButton(), Sdk(), accountNetworkName_, accountGuest_,
                        std::move(actions));
}

}  // namespace urnw
