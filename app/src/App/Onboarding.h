// The post-sign-up onboarding flow: four pages over the home view, with the
// step-bubbles-and-Skip top bar, the walking members on page 1 and the
// connector mark that flies from the route line into the header. Built in
// code over the OnboardingOverlay grid in MainWindow.xaml; shown once, right
// after a network is created (never for an existing account signing in).
//
// The language and design follow the Android flow (ui/introduction/*) page for
// page; Windows has no cellular toggle and no widget page, so it is four steps.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Media.Animation.h>
#include <winrt/Microsoft.UI.Xaml.Shapes.h>

#include "PlanPicker.h"
#include "ReferralCard.h"
#include "SubscriptionBalance.h"

namespace urnw {

class Onboarding : public std::enable_shared_from_this<Onboarding> {
 public:
  struct Actions {
    std::function<void()> finish;                    // Get connected, and Skip
    std::function<void(bool yearly)> startCheckout;  // the Stripe trial checkout
    std::function<void()> redeemCode;                // the redeem balance code sheet
  };

  static std::shared_ptr<Onboarding> Create(winrt::Microsoft::UI::Xaml::Controls::Grid host,
                                            Actions actions);

  void Show();
  void Hide();
  bool Visible() const { return visible_; }

  // Balance-store push, forwarded by the window: page 2's numbers and page 4's
  // referral code follow the store.
  void OnBalance(BalanceSnapshot const& snapshot);

 private:
  Onboarding(winrt::Microsoft::UI::Xaml::Controls::Grid host, Actions actions)
      : host_(host), actions_(std::move(actions)) {}

  static constexpr int kStepCount = 4;

  void Build();
  void ShowStep(int step);
  void ApplyTopBar();
  void ApplyBubbles();

  winrt::Microsoft::UI::Xaml::Controls::StackPanel BuildWelcome();
  void ApplyPlanCta(bool yearly);
  winrt::Microsoft::UI::Xaml::Controls::StackPanel BuildBandwidth();
  winrt::Microsoft::UI::Xaml::Controls::StackPanel BuildProviding();
  winrt::Microsoft::UI::Xaml::Controls::StackPanel BuildReferral();

  // page 1: the route line and its walker
  winrt::Microsoft::UI::Xaml::FrameworkElement BuildRoute();
  void StartTrip();
  void StopTrip();
  void PlaceWalker(double fraction);

  // the connector's flight between the route slot and the header slot
  void FlyConnector(bool toHeader);
  void PlaceFlyer(winrt::Windows::Foundation::Rect const& rect);
  winrt::Windows::Foundation::Rect SlotRect(winrt::Microsoft::UI::Xaml::FrameworkElement const& slot);
  void ParkFlyer();

  // page 2 / page 4 live content
  void ApplyBandwidth();
  void ApplyReferral();

  winrt::Microsoft::UI::Xaml::Controls::Grid host_;
  Actions actions_;
  bool built_ = false;
  bool visible_ = false;
  bool animations_ = true;
  int step_ = 1;

  // top bar
  winrt::Microsoft::UI::Xaml::Controls::Button backButton_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Button skipButton_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Border headerSlot_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::StackPanel bubbles_{nullptr};

  // pages
  winrt::Microsoft::UI::Xaml::Controls::ScrollViewer scroll_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::ContentControl pageHost_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::StackPanel welcome_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::StackPanel bandwidth_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::StackPanel providing_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::StackPanel referral_{nullptr};

  // page 1: route line
  winrt::Microsoft::UI::Xaml::Controls::Canvas walkerCanvas_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Grid walker_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Image walkerImage_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Border heroSlot_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Image routeConnector_{nullptr};
  winrt::Microsoft::UI::Xaml::Media::Animation::Storyboard trip_{nullptr};
  std::vector<int> deck_;
  int person_ = 0;
  double travel_ = 0;

  // page 1: plans (the picker shared with the upgrade sheet)
  PlanPicker plans_;
  winrt::Microsoft::UI::Xaml::Controls::Button checkoutButton_{nullptr};
  winrt::Microsoft::UI::Xaml::Media::Animation::Storyboard haloStoryboard_{nullptr};

  // the flying connector
  winrt::Microsoft::UI::Xaml::Controls::Canvas flightCanvas_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Image flyer_{nullptr};
  winrt::Microsoft::UI::Xaml::Media::Animation::Storyboard flight_{nullptr};
  bool flying_ = false;

  // page 2
  std::unique_ptr<class UsageBar> usageBar_;
  winrt::Microsoft::UI::Xaml::Controls::TextBlock dailyValue_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock dailyLine_{nullptr};
  BalanceSnapshot balance_;

  // page 4
  ReferralCard referralCard_;  // the shared referral progress box + gold panel
};

}  // namespace urnw
