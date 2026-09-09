// The welcome offer's supporting lines (mmm/onboarding/PLAN.md "in-app offer
// screen"), shared by every surface that prints the offer — the onboarding
// welcome page under the plan picker, the final offer page, and the upgrade
// sheet — so the deadline, the trial timeline and the terms cannot drift
// (the Linux OfferCard, ported):
//
//   Available until <local date, time>        (static: never a countdown)
//   Today     Free trial starts
//   Day 12    Reminder before the charge
//   Day 14    $30 for the year, cancel anytime before
//   14 days free, then $30 for your first year, then $40/year. Cancel anytime.
//
// The compact form (the upgrade sheet) is the deadline line and the terms
// only. All texts come from the store; the numbers from the offer and tier.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <string>

#include <winrt/Microsoft.UI.Xaml.Controls.h>

#include "PricePresentation.h"

namespace urnw {

// The plan cards' texts for a tier and (optionally) the offer, composed from
// the store's strings. One composer, so the picker and the offer page agree.
struct PlanCardTexts {
  winrt::hstring yearlyPrice;       // "$40/year" | "$30 for your first year"
  winrt::hstring yearlySecondary;   // "Save 33%" | "then $40/year"
  winrt::hstring yearlyEquivalent;  // "≈ $3.34/month · billed once a year" or ""
  winrt::hstring yearlyTrial;       // "Includes 14 day free trial"
  winrt::hstring monthlyPrice;      // "$5/month"
  winrt::hstring monthlyLine;       // "Billed monthly · cancel anytime"
};
PlanCardTexts ComposePlanCardTexts(const PriceTierView& tier, const OfferView& offer,
                                   int64_t trialDays);

// The offer's deadline as the store's sentence, in the user's locale and
// local time zone ("" when the offer has no usable expiry).
winrt::hstring OfferDeadlineText(const OfferView& offer);
// Seconds until the offer expires (0 when unknown or past).
int64_t OfferExpiresInSeconds(const OfferView& offer);

// The offer lines as a block the host appends; Update reprints them.
class OfferLines {
 public:
  winrt::Microsoft::UI::Xaml::Controls::StackPanel Build(bool compact);
  void Update(const OfferView& offer, const PriceTierView& tier, int64_t trialDays);
  winrt::Microsoft::UI::Xaml::Controls::StackPanel Root() const { return root_; }

 private:
  winrt::Microsoft::UI::Xaml::Controls::StackPanel root_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock deadline_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock chargeLine_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock terms_{nullptr};
};

}  // namespace urnw
