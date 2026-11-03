// The one-tap Bittensor key's sheets (apple/BTEASY.md, the approved mocks'
// flows 1 to 4):
//
//   BittensorKeySheet   the Key screen (1d to 1h, 1j, 1k, 2b to 2g, 2i): the words
//                       hidden by default, Tap to reveal behind Windows Hello for a
//                       stored key, Hide and Copy once they show, Keep beside the
//                       one destructive action, what deleting means, and the two
//                       words. Its steps replace each other rather than stacking,
//                       so the funds disclaimer shows once.
//   ClaimRelaySheet     Claim now from the settlement v2 tile (4b, 4c, 4e).
//   the replace warning (3c), and the sign-out warning while the key isn't
//   backed up (3f).
//
// The sdk's flow decides everything the Key screen shows and does
// (urnet::BittensorLocalKeyScreen); the sheet draws it and adds what only
// Windows has: Windows Hello, the window kept out of screen capture, the
// clipboard for an explicit Copy, and the words hiding when the window loses
// focus. The words are in the visual tree only while they show, as plain text
// nothing can select. Nothing here logs or keeps them.
//
// Plain C++ helpers like EarningsSheets: every method runs on the UI thread,
// and control handlers capture the sheet weakly.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <winrt/Windows.Foundation.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>

#include "BittensorKeyPlatform.h"
#include "BittensorKeyPresentation.h"
#include "Sdk.h"

namespace winrt::URnetwork::implementation {
struct MainWindow;
}

namespace urnw {

class BittensorKeySheet : public std::enable_shared_from_this<BittensorKeySheet> {
 public:
  // `flow` is the Earnings page's, with its screen open; `window` is the main
  // window's, for Windows Hello and the capture exclusion.
  static std::shared_ptr<BittensorKeySheet> Create(
      winrt::Microsoft::UI::Xaml::XamlRoot const& root,
      winrt::Microsoft::UI::Dispatching::DispatcherQueue const& queue, HWND window,
      std::shared_ptr<urnet::BittensorLocalKeyFlow> flow);

  winrt::Microsoft::UI::Xaml::Controls::ContentDialog Dialog() const { return dialog_; }

  // The flow changed: the screen is read again. A finished screen, or one the
  // flow closed, closes the sheet.
  void Refresh();
  // The window lost focus, or is hiding: the words hide (the sdk's Hide).
  void HideWords();
  // The sheet closes now, from whatever step: the window is hiding to the tray,
  // or the account is signing out.
  void CloseNow();
  // Closed, or closing: the page shows it no more.
  bool Finished() const { return forceClose_ || closed_; }

 private:
  BittensorKeySheet(winrt::Microsoft::UI::Dispatching::DispatcherQueue const& queue, HWND window,
                    std::shared_ptr<urnet::BittensorLocalKeyFlow> flow);

  void Build(winrt::Microsoft::UI::Xaml::XamlRoot const& root);
  void BuildMain();
  void BuildDeleteExplained();
  void BuildConfirm();
  void UpdateConfirm();
  winrt::Microsoft::UI::Xaml::UIElement BuildWords();
  winrt::fire_and_forget TapToReveal();
  void Copy();
  void ExplainDelete();
  void OnWordsTyped();
  winrt::fire_and_forget Submit();
  void UpdateCountdown();

  winrt::Microsoft::UI::Dispatching::DispatcherQueue queue_;
  HWND window_ = nullptr;
  std::shared_ptr<urnet::BittensorLocalKeyFlow> flow_;
  std::unique_ptr<btkeyplatform::CaptureExclusion> capture_;

  urnet::BittensorLocalKeyScreen screen_;
  btkey::KeyStep step_ = btkey::KeyStep::Main;
  bool built_ = false;
  // Delete was tapped: what deleting means shows before the two words
  bool deleteExplained_ = false;
  // Windows Hello is up: a second tap asks nothing
  bool authenticating_ = false;
  // a close that no step holds back (CloseNow)
  bool forceClose_ = false;
  bool closed_ = false;
  // Copy was tapped: the button says Copied for a moment
  bool copied_ = false;

  winrt::Microsoft::UI::Xaml::Controls::ContentDialog dialog_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::StackPanel content_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock countdown_{nullptr};
  winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer countdownTimer_{nullptr};
  winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer copiedTimer_{nullptr};
  // the two words' step, built once and updated as the user types
  winrt::Microsoft::UI::Xaml::Controls::TextBox firstBox_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBox secondBox_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock firstWrong_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock secondWrong_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock rejected_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock confirmFailure_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Button confirmButton_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::ProgressRing confirmRing_{nullptr};
  bool wordsReady_ = false;
};

// Claim now through URnetwork's relay, from the settlement v2 tile: the
// balance, where it lands, why it waits, and Claim now or why it can't act,
// with the device's own claim as the fallback (the self-claim link closes the
// sheet; the page opens the gas-key claim after it).
class ClaimRelaySheet : public std::enable_shared_from_this<ClaimRelaySheet> {
 public:
  // The answer to a Claim now: the sdk's result, or the error that stopped it.
  using ClaimNowDone = std::function<void(std::optional<urnet::SnClaimNowResult> result, std::string error)>;
  // Asks the sdk (Device.snClaimNow) for `coldkey`; `done` runs on the UI thread.
  using ClaimNow = std::function<void(std::string coldkey, ClaimNowDone done)>;

  // `status` as the page last read it; `disclaimer` the sdk's for the claim
  // sheet (FundsDisclaimerAt("claim_sheet")).
  static std::shared_ptr<ClaimRelaySheet> Create(winrt::Microsoft::UI::Xaml::XamlRoot const& root,
                                                 std::optional<urnet::SnClaimStatusResult> status,
                                                 std::string disclaimer, ClaimNow claimNow);

  winrt::Microsoft::UI::Xaml::Controls::ContentDialog Dialog() const { return dialog_; }
  // The self-claim link was chosen: the page opens the gas-key claim.
  bool SelfClaimChosen() const { return selfClaimChosen_; }
  // A Claim now was answered: the page reads the claim status again.
  bool ClaimNowAnswered() const { return answered_; }

 private:
  enum class Outcome { None, Requested, Refused, Failed };

  ClaimRelaySheet(std::optional<urnet::SnClaimStatusResult> status, std::string disclaimer, ClaimNow claimNow);
  void Build(winrt::Microsoft::UI::Xaml::XamlRoot const& root);
  void ApplyState();
  void StartClaimNow();

  std::optional<urnet::SnClaimStatusResult> status_;
  std::string disclaimer_;
  ClaimNow claimNow_;
  Outcome outcome_ = Outcome::None;
  std::string refusedReason_;
  int64_t refusedNextAtMillis_ = 0;
  std::string failure_;
  bool claiming_ = false;
  bool answered_ = false;
  bool selfClaimChosen_ = false;

  winrt::Microsoft::UI::Xaml::Controls::ContentDialog dialog_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::StackPanel actionHost_{nullptr};
};

namespace btkeysheets {

// The warning before a new key replaces the payout wallet in force (3c):
// earnings go to a new key on this device from the next epoch, earlier ones
// stay on `oldAddress`, and the network admin gets an email. True once the
// user chose Replace payout wallet. False, asking nothing, while another sheet
// is open.
winrt::Windows::Foundation::IAsyncOperation<bool> ConfirmReplacePayoutWallet(
    winrt::com_ptr<winrt::URnetwork::implementation::MainWindow> window, std::string oldAddress);

// A sign-out the user asked for (3e to 3g). With a key on this device whose
// phrase isn't known to be saved, it asks first: Back up first opens Earnings
// with the Key screen, Sign out signs out, Cancel does nothing. With none it
// signs out at once, as it always did. The key itself stays on this device
// either way.
winrt::fire_and_forget RequestSignOut(winrt::com_ptr<winrt::URnetwork::implementation::MainWindow> window,
                                      std::function<void()> signOut);

}  // namespace btkeysheets
}  // namespace urnw
