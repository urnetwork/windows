// The plan picker the onboarding welcome page and the upgrade sheet share:
// the yearly plan in the Pro-gold dress (price, saving, the free trial line,
// the Best value pill, the halo) selected by default, the monthly plan plain
// below it with no trial. ONE implementation, so the two surfaces cannot
// drift in order, default, copy or the CTA label rule (yearly -> "Start free
// trial", monthly -> "Subscribe": only the yearly plan carries the trial).
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <functional>
#include <memory>

#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Shapes.h>

namespace urnw {

// The free trial the yearly plan starts with, in days, as printed on the plan
// card. The trial itself is the server's Stripe checkout session setting
// (subscription_stripe_controller: trial_period_days) and must match this.
inline constexpr int64_t kFreeTrialDays = 14;

class PlanPicker {
 public:
  // Builds the plans block (the halo, the yearly card with the Best value
  // pill, the monthly card) and returns it for the host to place.
  winrt::Microsoft::UI::Xaml::Controls::Grid Build();
  bool Yearly() const { return state_->yearly; }
  // Select a plan programmatically: repaints, does NOT fire onSelect (the
  // host sets its own CTA label from CtaLabel).
  void Select(bool yearly);
  void SetEnabled(bool enabled);
  // The CTA label for a selection: only the yearly plan carries the trial.
  static winrt::hstring CtaLabel(bool yearly);
  // The halo behind the yearly card, for a host that pulses it (onboarding).
  winrt::Microsoft::UI::Xaml::Shapes::Rectangle Halo() const { return state_->halo; }
  // The halo's pulse (opacity 0.6 <-> 1.0 over 2.2 s, forever): one definition,
  // so every host that shows the picker breathes the same way. The host owns
  // the storyboard: Begin() once the halo is loaded, Stop() when it hides.
  winrt::Microsoft::UI::Xaml::Media::Animation::Storyboard HaloPulse() const;

  // Fired on a tap, after the selection changed.
  std::function<void(bool yearly)> onSelect;

 private:
  // The XAML handlers capture this weakly: a card tapped after its host is
  // gone must not touch freed memory.
  struct State {
    bool yearly = true;
    winrt::Microsoft::UI::Xaml::Controls::Border yearlyCard{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Border monthlyCard{nullptr};
    winrt::Microsoft::UI::Xaml::Shapes::Ellipse yearlyDot{nullptr};
    winrt::Microsoft::UI::Xaml::Shapes::Ellipse monthlyDot{nullptr};
    winrt::Microsoft::UI::Xaml::Shapes::Rectangle halo{nullptr};
    // the selection pink laid over the gold halo and ground while the yearly
    // card is selected, so the selection language survives the gold dress
    winrt::Microsoft::UI::Xaml::Shapes::Rectangle haloSelected{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Border yearlyTint{nullptr};
    std::function<void(bool yearly)>* onSelect = nullptr;
  };
  static winrt::Microsoft::UI::Xaml::Controls::Border BuildCard(std::shared_ptr<State> const& state,
                                                                bool yearly);
  static void Apply(State const& state);

  std::shared_ptr<State> state_ = std::make_shared<State>();
};

}  // namespace urnw
