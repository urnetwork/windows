// The "Refer and earn" page (ReferralsView in MainWindow.xaml), reached from
// the Account destination's Referrals row. android/apple Referrals screen and
// linux ReferralsPage parity:
//
//   1. the SHARED referral card (ReferralCard.h): the gold king-frog panel the
//      onboarding "Refer friends" step shows, so the wording, the bonus figure
//      and the crowned state cannot drift between the two surfaces. The code
//      with copy and share, and the referral progress bar with its "joined /
//      cap" count, are part of the panel (android ReferralsScreen shows the
//      panel alone; only the onboarding step adds the progress box above it).
//   2. the figures: total referrals and the referral points earned
//      (payout_linked_account).
//   3. the referral network (who referred THIS network), built into
//      ReferralsNetworkHost by SettingsPage, which still owns that sheet.
//
// Every referral number comes from the balance store's ReferralTerms (the
// server's cap and bonus) — nothing here hardcodes the bonus. The card follows
// the store on every balance change; the points row is this page's one API
// read and terminates in a rows::FieldState like every other async field.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <winrt/Microsoft.UI.Xaml.Controls.h>

#include "ReferralCard.h"
#include "SettingsSheets.h"  // rows::FieldState + the row kit

namespace winrt::URnetwork::implementation {
struct MainWindow;
}

namespace urnw {

class ReferralsPage {
 public:
  explicit ReferralsPage(winrt::URnetwork::implementation::MainWindow& window);

  void ApplyStrings();  // the pane title + the back affordance; builds once
  // Opening the page (and the auth relay while it is up): the card and the
  // total from the store, the points from the API. With no session every
  // field settles on NoSession without a request.
  void Load();
  // The balance store published: repaint the card and the total.
  void OnBalance();
  void ResetForSignOut();

 private:
  void Build();  // idempotent
  void ApplyTotal();
  void ApplyPoints(rows::FieldState state, double points);

  winrt::URnetwork::implementation::MainWindow& w_;
  bool built_ = false;
  ReferralCard card_;
  winrt::Microsoft::UI::Xaml::Controls::TextBlock totalValue_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock pointsValue_{nullptr};
};

}  // namespace urnw
