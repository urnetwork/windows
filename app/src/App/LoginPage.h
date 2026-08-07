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

#include <memory>
#include <string>

#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>

#include "AuthSheets.h"
#include "SdkHost.h"
#include "UrComponents.h"

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
  // The title-bar account menu's identity: avatar initials, the Pro ring, and
  // whether the menu offers "Create account". Pushed from the window's auth
  // relay, which already parses the jwt.
  void ApplyAccountIdentity(std::string const& networkName, bool guest, bool pro,
                            bool signedIn);
  // surfaces an auth error on whichever step the user is looking at
  void ShowErrorOnCurrentStep(winrt::hstring const& message);
  bool IsGuestUpgrade() const;
  void ClearGuestUpgrade();
  // The plan card's create-account affordance for a guest: the create step in
  // guest-upgrade mode, shown over the login flow while the session stays live.
  void BeginGuestUpgrade();

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
  winrt::fire_and_forget OnUseCode(winrt::Windows::Foundation::IInspectable const&,
                                   winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnTryGuestMode(winrt::Windows::Foundation::IInspectable const&,
                      winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnSignInWithBittensor(winrt::Windows::Foundation::IInspectable const&,
                             winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  winrt::fire_and_forget OnSignInWithSolana(
      winrt::Windows::Foundation::IInspectable const&,
      winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  // gates Get started on a non-empty field (iOS/android parity)
  void OnUserAuthChanged(winrt::Windows::Foundation::IInspectable const&,
                         winrt::Microsoft::UI::Xaml::Controls::TextChangedEventArgs const&);
  void OnSignInWithGoogle(winrt::Windows::Foundation::IInspectable const&,
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
  // the bottom-left "Change Network API" affordance
  winrt::fire_and_forget OnChangeNetworkServer(
      winrt::Windows::Foundation::IInspectable const&,
      winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  // the title-bar avatar
  void OnAccountMenu(winrt::Windows::Foundation::IInspectable const&,
                     winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);

 private:
  enum class LoginStep { Initial, Password, Create, Verify, Reset, Seedphrase, Instant };
  // What the create step submits: a fresh network with email + password, one
  // with the retained wallet auth or SSO id token, or the guest network's
  // upgrade to a full account (Api::upgradeGuest; linux CreateNetworkPage::Mode
  // parity).
  enum class CreateMode { Password, Wallet, AuthJwt, GuestUpgrade };

  void ShowLoginStep(LoginStep step);
  void ApplyLoginRouting(urnw::LoginRouting const& routing);
  void EnterCreateStep(std::string const& userAuth, CreateMode mode);
  void EnterVerifyStep(std::string const& userAuth);
  void ShowLoginErrorFor(LoginStep step, winrt::hstring const& message);
  // the initial step's URInlineErrorText; empty message hides it
  void SetInitialLoginError(winrt::hstring const& message);
  // Get started is enabled only for a non-empty field with no discovery in
  // flight; several paths re-enable the sign-in affordances and all of them go
  // through here rather than writing `true`.
  void UpdateGetStartedEnabled();
  void CheckCreateNameNow();   // debounce elapsed: run the availability check
  void ApplyNameCheck(uint32_t generation, bool ok, bool available);
  void ValidateBonusCodeNow();
  void ApplyBonusValidation(uint32_t generation, bool ok, bool valid, bool capped);
  void ValidateCreateForm();   // gates the Continue button
  void SubmitVerifyCode();
  winrt::fire_and_forget ShowGuestModeSheet();  // terms consent -> LoginAsGuest
  void SetWalletSignInEnabled(bool enabled);
  void ApplyWalletSignInResult(urnw::AuthResult const& result);
  // seedphrase step: word count -> the warning line + the submit gate
  void ValidateSeedphrase();
  // the newly minted phrase, shown once, gating the device registration
  winrt::fire_and_forget ShowSeedphraseSheet(std::string seedphrase);

  winrt::URnetwork::implementation::MainWindow& w_;

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

  // seedphrase / instant-account step state (UI thread only)
  bool seedphraseLoggingIn_ = false;
  bool creatingInstant_ = false;
  // what the title-bar account menu should offer (pushed by ApplyAccountIdentity)
  bool accountGuest_ = false;
  std::string accountNetworkName_;
  std::shared_ptr<urnw::GuestModeSheet> guestSheet_;
  std::shared_ptr<urnw::SeedphraseDisplaySheet> seedphraseSheet_;
  std::shared_ptr<urnw::NetworkServerSheet> networkServerSheet_;
  // "Seedphrase copied" / "Referral link copied" acknowledgements
  std::unique_ptr<urnw::kit::Snackbar> snackbar_;
};

}  // namespace urnw
