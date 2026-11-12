// What the Account plan card shows for the plan the sdk decides
// (urnet::SubscriptionProTracker: getShownPlan, getLastReadFailed). Pro and
// Free are the server's answer once a balance has loaded (Pro also by the
// stored jwt's claim before it answers). Until the server answers the plan is
// unknown, and the card says so instead of "Free": Loading while the first
// balance read is on its way, Something went wrong with Try again once a read
// failed. Only a plan the server said is Free offers the upgrade. In December
// 2025 the balance read failed under database load and the apps showed paying
// networks Free with the upgrade (support inbox 4074).
//
// Header-only and WinRT-free: tools/plan-view-tests.cpp runs it.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <string_view>

namespace urnw::planview {

enum class PlanView {
  Pro,
  Free,
  // unknown: the first balance read of the session is on its way
  Loading,
  // unknown: a read failed and none has loaded since
  Failed,
};

// The sdk's plan values (urnet::ShownPlanPro, ShownPlanFree,
// ShownPlanUnknown; SubscriptionBalance.cpp asserts they match).
inline constexpr std::string_view kShownPlanPro = "pro";
inline constexpr std::string_view kShownPlanFree = "free";
inline constexpr std::string_view kShownPlanUnknown = "unknown";

// `shownPlan` is the sdk's getShownPlan; any value but Pro and Free is unknown.
constexpr PlanView PlanViewFor(std::string_view shownPlan, bool lastReadFailed) {
  if (shownPlan == kShownPlanPro) return PlanView::Pro;
  if (shownPlan == kShownPlanFree) return PlanView::Free;
  return lastReadFailed ? PlanView::Failed : PlanView::Loading;
}

// The plan value's string key. A guest network reads "guest" whatever the
// plan (its card offers creating an account instead).
constexpr std::string_view PlanValueKey(PlanView plan, bool guest) {
  if (guest) return "guest";
  switch (plan) {
    case PlanView::Pro:
      return "supporter";
    case PlanView::Free:
      return "free";
    case PlanView::Failed:
      return "something_went_wrong";
    case PlanView::Loading:
      break;
  }
  return "loading";
}

// Try again beside the plan value: a read failed and the server has not
// answered since.
constexpr bool OffersRetry(PlanView plan, bool guest) {
  return !guest && plan == PlanView::Failed;
}

// The plan card's Upgrade and the wallet's checkout: a signed-in account the
// server said is Free. Never while the plan is unknown.
constexpr bool OffersUpgrade(PlanView plan, bool guest) {
  return !guest && plan == PlanView::Free;
}

}  // namespace urnw::planview
