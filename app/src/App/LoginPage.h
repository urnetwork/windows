// The signed-out surface: account discovery, password, create-network, verify,
// password reset, the wallet / auth-code sign-in options and the guest-mode
// sheet. macOS Authenticate/** parity.
//
// Split out of MainWindow.xaml.cpp. The page owns its own state and drives the
// x:Name elements of the login panels through the window reference; MainWindow
// keeps the XAML event handlers (the markup binds to them by name) as one-line
// forwarders, plus the window-level LoginRoot/HomeNav swap.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <functional>
#include <memory>
#include <string>

#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>

#include "AuthSheets.h"
#include "ControlDohSettings.h"
#include "LoginCarousel.h"
#include "NetworkNameCheck.h"
#include "SdkHost.h"
#include "SettingsSheets.h"
#include "UrComponents.h"
#include "VlessSheet.h"

namespace winrt::URnetwork::implementation {
struct MainWindow;
}

namespace urnw {

class LoginPage {
 public:
  explicit LoginPage(winrt::URnetwork::implementation::MainWindow& window);
  ~LoginPage();

  // debounce / cooldown timers; separate from the constructor so the window
  // controls the order in which the pages come up
  void Initialize();

  // every label on the sign-in panels, from the shared localization store
  void ApplyStrings();

  // ---- window-level calls ----
  void ResetToInitialStep();
  // Raise SdkHost's "nothing is connected, and here is why" on the window-level
  // snackbar. The sentence comes from SdkHost, already composed.
  void ShowModeNotice(winrt::hstring const& message, bool failed);
  // The carousel animates only while the window is on screen AND the flow is on
  // the initial step. A tray app is hidden most of its life, and a slideshow
  // nobody can see is pure wakeups (iOS gates the same timer on
  // presentationActive).
  void SetPresentationActive(bool active);
  // Coming back to the window ends a browser sign-in flow from the user's
  // side, but the SSO / wallet flows answer ONLY through the deep-link
  // callback and a closed browser sends nothing - without this the affordances
  // they disabled stayed grey until the app restarted. The SDK attempt is NOT
  // cancelled here: a late completion still lands (SdkHost's on_sso matches
  // the attempt), and a fresh click supersedes it through the SDK's answer
  // semantics, so there is nothing to undo.
  void OnWindowReactivated();
  // --preview-ui=seedphrase (Startup.h). Raise the seedphrase display sheet on
  // the BIP-39 test vector so its word grid, its refusal to be dismissed and
  // its copy button can be looked at without creating a real account — the
  // only way this sheet is otherwise reachable. Confirming does NOT register a
  // device: there is no pending instant account in preview.
  void ShowPreviewSeedphraseSheet();
  // The newly minted phrase, shown once, gating the device registration.
  // Public only because ShowPreviewSeedphraseSheet posts back into it through
  // the window's page accessor.
  winrt::fire_and_forget ShowSeedphraseSheet(std::string seedphrase);
  // The title-bar account menu's identity: avatar initials, the Pro ring, and
  // whether the menu offers "Create account". Pushed from the window's auth
  // relay, which already parses the jwt.
  void ApplyAccountIdentity(std::string const& networkName, bool guest, bool pro,
                            bool signedIn);
  // Surfaces an auth error on whichever step the user is looking at. Takes the
  // RAW string the auth relay carries: the machine tokens it can contain are
  // mapped to their friendly copy here (MapAuthErrorForDisplay), because the
  // relay also replays an error that landed while the window was away, and an
  // unmapped replay used to replace the mapped sentence with the raw token.
  void ShowErrorOnCurrentStep(std::string const& error);

  // True once, right after this sign-in created a network (sign-up, its
  // verification step, or an instant account): the window shows the
  // onboarding flow for it. An existing account signing in never sets it.
  bool ConsumeNewNetwork();
  // Every create-account and purchase affordance for a legacy guest network
  // (the plan card, the account menu, Get Pro, the upgrade sheet) opens the
  // in-place conversion (GuestConversionSheet): a sign-in is added to THIS
  // network and verified, so its plan and balance stay. Signing out would
  // abandon the network for good (it has no login to come back to).
  // `onClosed(done)` runs once the sheet has closed (done: the sign-in was
  // added and verified), so a purchase entry can continue to its checkout.
  winrt::fire_and_forget OpenGuestConversion(std::function<void(bool done)> onClosed = {});

  // Sign in with a one-time auth code, exactly as if it had been typed into
  // the auth-code sheet and Sign in pressed. Shared by that sheet (OnUseCode),
  // the browser sign-in bridge sheet and the urnetwork://auth?code= deep link
  // (AppController::HandleDeepLink, which has already refused the link when a
  // session exists). The code is a credential and is never logged.
  void SignInWithAuthCode(std::string code);

  // ---- XAML event handlers (forwarded from MainWindow) ----
  void OnGetStarted(winrt::Windows::Foundation::IInspectable const&,
                    winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnSignIn(winrt::Windows::Foundation::IInspectable const&,
                winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnPasswordKeyDown(winrt::Windows::Foundation::IInspectable const&,
                         winrt::Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const&);
  void OnLoginBack(winrt::Windows::Foundation::IInspectable const&,
                   winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnForgotPassword(winrt::Windows::Foundation::IInspectable const&,
                        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnSendResetLink(winrt::Windows::Foundation::IInspectable const&,
                       winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnCreateNameChanged(winrt::Windows::Foundation::IInspectable const&,
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
  winrt::fire_and_forget OnUseCode(winrt::Windows::Foundation::IInspectable const&,
                                   winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnSignInWithBittensor(winrt::Windows::Foundation::IInspectable const&,
                             winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  winrt::fire_and_forget OnSignInWithSolana(
      winrt::Windows::Foundation::IInspectable const&,
      winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  // gates Get started on a non-empty field (iOS/android parity)
  void OnUserAuthChanged(winrt::Windows::Foundation::IInspectable const&,
                         winrt::Microsoft::UI::Xaml::Controls::TextChangedEventArgs const&);
  // Google / Apple through the provider's web flow (SdkHost::SignInWithSso)
  void OnSignInWithGoogle(winrt::Windows::Foundation::IInspectable const&,
                          winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnSignInWithApple(winrt::Windows::Foundation::IInspectable const&,
                         winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  // seedphrase sign-in step
  void OnSignInWithSeedphrase(winrt::Windows::Foundation::IInspectable const&,
                              winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnSeedphraseChanged(winrt::Windows::Foundation::IInspectable const&,
                           winrt::Microsoft::UI::Xaml::Controls::TextChangedEventArgs const&);
  void OnSeedphraseSubmit(winrt::Windows::Foundation::IInspectable const&,
                          winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  // instant (seedphrase-only) account step
  void OnCreateInstantAccount(winrt::Windows::Foundation::IInspectable const&,
                              winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnInstantTermsChanged(winrt::Windows::Foundation::IInspectable const&,
                             winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnCreateInstantSubmit(winrt::Windows::Foundation::IInspectable const&,
                             winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  // the bottom-left "Change Network API" affordance, and the VLESS sheet its
  // VLESS button opens in its place
  winrt::fire_and_forget OnChangeNetworkServer(
      winrt::Windows::Foundation::IInspectable const&,
      winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  // the bottom-left "Sign in with browser" affordance: the bridge sheet
  winrt::fire_and_forget OnSignInWithBrowser(
      winrt::Windows::Foundation::IInspectable const&,
      winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  // the title-bar avatar
  void OnAccountMenu(winrt::Windows::Foundation::IInspectable const&,
                     winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);

 private:
  enum class LoginStep { Initial, Password, Create, Verify, Reset, Seedphrase, Instant };
  // What the create step submits: a fresh network with email + password, or one
  // with the retained wallet auth or SSO id token.
  enum class CreateMode { Password, Wallet, AuthJwt };

  void ShowLoginStep(LoginStep step);
  void ApplyLoginRouting(urnw::LoginRouting const& routing);
  void EnterCreateStep(std::string const& userAuth, CreateMode mode);
  void EnterVerifyStep(std::string const& userAuth);
  void ShowLoginErrorFor(LoginStep step, winrt::hstring const& message);
  // A code the server did not send, on the verify step; false when it was sent
  // (the caller says so, or not, as before).
  bool ShowVerifySendError(urnw::VerifySendNotice const& notice);
  // A reset link the server did not send, on the reset step; false when it was
  // sent.
  bool ShowPasswordResetError(urnw::VerifySendNotice const& notice);
  // Counts the rate-limit notices down and turns Resend / Send back on once
  // the retry time has passed (rateLimitTimer_ tick).
  void RefreshRateLimits();
  void StartRateLimitTimer();
  // the initial step's URInlineErrorText; empty message hides it
  void SetInitialLoginError(winrt::hstring const& message);
  // Get started is enabled only for a non-empty field with no discovery in
  // flight; several paths re-enable the sign-in affordances and all of them go
  // through here rather than writing `true`.
  void UpdateGetStartedEnabled();
  void CheckCreateNameNow();   // debounce elapsed: run the availability check
  void ApplyNameCheck(uint32_t generation, bool ok, bool available);
  void ShowNameCheck();        // the name's supporting line for the flow's state
  void ValidateBonusCodeNow();
  void ApplyBonusValidation(uint32_t generation, bool ok, bool valid, bool capped);
  void ValidateCreateForm();   // gates the Continue button
  void SubmitVerifyCode();
  void SetWalletSignInEnabled(bool enabled);
  void ApplyWalletSignInResult(urnw::AuthResult const& result);
  // The one place bridge error tokens become display copy. Google's web flow
  // answers error=not_configured while the production api vault lacks
  // sign_in_oauth; the raw token reads as a broken app on the login screen.
  // Every other error passes through as-is.
  winrt::hstring MapAuthErrorForDisplay(std::string const& error);
  // Google or Apple: open the provider's sign-in page and wait for the api's
  // urnetwork://oauth/<provider> answer; `provider` is "google" or "apple".
  void StartSsoSignIn(const char* provider);
  // seedphrase step: word count -> the warning line + the submit gate
  void ValidateSeedphrase();
  // Empty the seedphrase field. It is UIA-readable (and writable) by any
  // process for as long as it holds anything, so nothing may leave a phrase
  // sitting in it.
  void ClearSeedphraseField();
  // Give the sign-in affordances their height FIRST and the hero carousel
  // whatever is left, so Sign in with Seedphrase / Create Instant Account /
  // Change Network API are never pushed below the fold. Re-run on every size
  // change of the scroll viewport or the column.
  void ApplyLoginLayout();
  // run the carousel only when it is on the initial step, on screen, and its
  // slot has not been collapsed by the layout above
  void UpdateCarouselRunning();
  // Name the icon-plus-text pills and the icon tiles for UIA: their content is
  // a panel, not a string, so ContentControl derives no name from it, and a
  // tile's caption is a short word where the name should be the sentence.
  void ApplySignInAutomationNames();

  winrt::URnetwork::implementation::MainWindow& w_;

  // sign-in flow state (UI thread only)
  LoginStep loginStep_ = LoginStep::Initial;
  // guards ApplyLoginLayout against the SizeChanged its own writes raise
  bool inLayoutPass_ = false;
  std::string loginUserAuth_;      // the echoed user auth driving the current step
  bool discoveringLogin_ = false;  // authLogin discovery in flight
  CreateMode createMode_ = CreateMode::Password;  // what the create step submits
  bool creatingNetwork_ = false;
  bool verifying_ = false;
  bool newNetworkPending_ = false;  // see ConsumeNewNetwork
  bool verifyIsNewNetwork_ = false;  // the verify step follows a sign-up
  bool sendingReset_ = false;
  // Every SetWalletSignInEnabled(false) call arms this: those attempts (the
  // SSO / wallet browser round trips, the auth code) re-enable the affordances
  // only from their SDK callback, and a browser the user closed never delivers
  // one. OnWindowReactivated is the re-enable of last resort for exactly this
  // state, so no other path may set it.
  bool walletSignInInFlight_ = false;
  // create-network name availability (debounced; the generation drops stale checks)
  winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer nameCheckTimer_{nullptr};
  // re-runs a check that errored (see NetworkNameCheck.h)
  winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer nameRetryTimer_{nullptr};
  // the name state, debounce and retries; drives the two timers above
  std::unique_ptr<urnw::NetworkNameCheckFlow> nameCheck_;
  // bonus referral code validation (debounced)
  winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer bonusCheckTimer_{nullptr};
  uint32_t bonusCheckGeneration_ = 0;
  bool bonusValid_ = false;
  bool bonusCapped_ = false;
  // resend-code cooldown (15s, macOS parity)
  winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer resendCooldownTimer_{nullptr};
  // after the server refused a code / reset link for too many attempts:
  // Resend / Send stay off until the retry time, with a 1s tick counting the
  // notice down
  urnw::ResendCooldown verifyRateLimit_;
  urnw::ResendCooldown resetRateLimit_;
  std::string resetRateLimitUserAuth_;  // the account resetRateLimit_ is for
  // the rate-limit line last shown, so the tick only rewrites its own notice
  winrt::hstring verifyRateLimitText_;
  winrt::hstring resetRateLimitText_;
  winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer rateLimitTimer_{nullptr};

  // seedphrase / instant-account step state (UI thread only)
  bool seedphraseLoggingIn_ = false;
  bool creatingInstant_ = false;
  // what the title-bar account menu should offer (pushed by ApplyAccountIdentity)
  bool accountGuest_ = false;
  std::string accountNetworkName_;
  std::unique_ptr<urnw::LoginCarousel> carousel_;
  bool presentationActive_ = false;
  std::shared_ptr<urnw::SeedphraseDisplaySheet> seedphraseSheet_;
  std::shared_ptr<urnw::NetworkServerSheet> networkServerSheet_;
  std::shared_ptr<urnw::VlessSheet> vlessSheet_;
  std::shared_ptr<urnw::ControlDohSheet> controlDohSheet_;
  std::shared_ptr<urnw::GuestConversionSheet> guestConversionSheet_;
  // "Seedphrase copied" / "Referral link copied" acknowledgements
  std::unique_ptr<urnw::kit::Snackbar> snackbar_;
};

}  // namespace urnw
