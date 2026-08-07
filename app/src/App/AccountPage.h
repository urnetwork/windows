// The Account destination: profile (network name + auth), the redeemed
// balance-code list, and the referral summary. macOS AccountRootView parity.
//
// The plan + usage card that sits above these is NOT here: it is written by the
// SubscriptionBalanceStore relay in MainWindow, which paints the account panel
// and the connect drawer from one snapshot.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <string>

#include <winrt/Microsoft.UI.Xaml.h>

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
  winrt::URnetwork::implementation::MainWindow& w_;

  int64_t totalReferrals_ = 0;
  std::string referralCode_;
};

}  // namespace urnw
