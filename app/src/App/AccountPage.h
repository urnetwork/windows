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
  // R4: the profile name's explicit edit mode (spec, Profile). The row shows the
  // saved name; the pencil opens a field; Save and Cancel exist only while it is
  // open, and Cancel restores the saved name rather than whatever was typed.
  void OnEditNetworkName(winrt::Windows::Foundation::IInspectable const&,
                         winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnCancelNetworkName(winrt::Windows::Foundation::IInspectable const&,
                           winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);

  // The redeemed-code table and every state it can be in, in one place: three
  // scattered writes to a panel and an empty line used to disagree about which
  // was showing. Called by LoadBalanceCodes, ApplyStrings and ResetForSignOut.
  void RenderBalanceCodes(urnet::RedeemedBalanceCodeList const& codes,
                          rows::FieldState state);

  // Drop the signed-out account's identity. userAuth_ is the dangerous one:
  // SendPasswordReset mails a link to it, so a leftover value mails the
  // PREVIOUS account's owner. needsNameClaim_ would likewise pick the previous
  // account's branch on the next save.
  void ResetForSignOut();

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

  void SetEditingName(bool editing);
  // The auth line and its ROW: an empty fixed-height row is still a hole.
  void SetAuthText(winrt::hstring const& text);
  // Write the saved name into the view row AND keep the copy the editor seeds
  // from. The TextBox is never the source of truth for the name.
  void ApplyNetworkName(std::string const& name);

  winrt::Microsoft::UI::Xaml::Controls::TextBlock nameStatus_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Button changePasswordButton_{nullptr};
  bool built_ = false;
  // one-shot: the "nothing has been requested yet" states, applied by
  // ApplyStrings so a load is not needed to make the card readable
  bool initialStatesApplied_ = false;
  bool savingName_ = false;
  bool editingName_ = false;
  // the last name the SERVER acknowledged; what the editor seeds from and what
  // Cancel restores
  std::string networkName_;
  bool sendingReset_ = false;
};

}  // namespace urnw
