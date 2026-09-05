// The referral pieces the onboarding "Refer friends" step and the Account
// "Refer and earn" page share: the referral progress box (referrals earned
// out of the ones that pay, in referral gold) and the gold king-frog panel
// (android ReferralGoldPanel: heading, the bonus sentence, the code with copy
// and share, the crowned congratulations). ONE implementation, so the two
// surfaces cannot drift in wording, numbers or the crowned state.
//
// Every figure comes from the balance store's ReferralTerms (the server's cap
// and bonus GiB/day); nothing here hardcodes the bonus.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <string>

#include <winrt/Microsoft.UI.Xaml.Controls.h>

namespace urnw {

class ReferralCard {
 public:
  // Appends the panel host to `host`. The panel carries the only referral
  // progress bar (the 6px bar with "joined / cap" inside it, as on android):
  // the onboarding step and the Referrals page both show it alone.
  // `animations` gates the halo pulse (UISettings().AnimationsEnabled()).
  void Build(winrt::Microsoft::UI::Xaml::Controls::Panel const& host, bool animations);
  // Repaints from the balance store (terms, total referrals, code). The panel
  // is rebuilt only when the code, the count or the cap changed.
  void Apply();
  bool built() const { return referralPanelHost_ != nullptr; }

 private:
  bool animations_ = true;
  winrt::Microsoft::UI::Xaml::Controls::StackPanel referralPanelHost_{nullptr};
  std::string shownReferralCode_;
  int64_t shownReferralTotal_ = -1;
  int64_t shownReferralMax_ = -1;
};

}  // namespace urnw
