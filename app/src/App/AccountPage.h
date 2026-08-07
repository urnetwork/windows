// The Account destination: profile (network name + auth + password), the
// redeemed balance-code list, and the referral summary. macOS AccountRootView
// and ProfileView parity.
//
// The plan + usage card that sits above these is NOT here: it is written by the
// SubscriptionBalanceStore relay in MainWindow, which paints the account panel
// and the connect drawer from one snapshot.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>

#include "SettingsSheets.h"  // rows::FieldState + the row kit
#include "UrComponents.h"

namespace winrt::URnetwork::implementation {
struct MainWindow;
}

namespace urnw {

class AccountPage {
 public:
  explicit AccountPage(winrt::URnetwork::implementation::MainWindow& window);

  void ApplyStrings();

  void LoadAccount();
  void LoadReferralInfo();   // referral code + totals (usage-bar rows)
  void LoadBalanceCodes();   // redeemed-codes list (account panel)

  // read by MainWindow::ApplyBalance for the "Total Referrals" / bonus rows on
  // both plan cards
  int64_t totalReferrals() const { return totalReferrals_; }

  void OnSaveNetworkName(winrt::Windows::Foundation::IInspectable const&,
                         winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);

 private:
  // The controls the markup cannot carry: the name status line and the
  // password-reset affordance, built into the AccountProfileExtra host panel.
  void BuildProfileExtra();
  void SendPasswordReset();
  // Every async field on this surface reaches one of these, for the same reason
  // the settings page does: before it, a 401 and an empty account looked
  // identical (the redeemed-codes list rendered "No balance codes found" for
  // both).
  void ApplyAccountState(rows::FieldState state);

  winrt::URnetwork::implementation::MainWindow& w_;

  int64_t totalReferrals_ = 0;
  std::string referralCode_;
  // the auth this account signs in with, needed by the password-reset call
  std::string userAuth_;
  // apple AccountNavStackView: a name must be CLAIMED (no reclaim cooldown on
  // the auto-generated one) rather than CHANGED (24h cooldown protects the old
  // name) exactly when the account carries none of these identity methods.
  // Seedphrase deliberately does not count - a seedphrase-only account still
  // has an auto-generated name to claim.
  bool needsNameClaim_ = false;

  winrt::Microsoft::UI::Xaml::Controls::TextBlock nameStatus_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Button changePasswordButton_{nullptr};
  bool built_ = false;
  // one-shot: the "nothing has been requested yet" states, applied by
  // ApplyStrings so a load is not needed to make the card readable
  bool initialStatesApplied_ = false;
  bool savingName_ = false;
  bool sendingReset_ = false;
};

}  // namespace urnw
