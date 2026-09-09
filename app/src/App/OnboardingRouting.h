// The onboarding flow's page order and what Skip does, as pure rules
// (mmm/onboarding/PLAN.md "in-app offer screen"):
//
//   welcome(1) -> bandwidth(2) -> provide(3) -> referral(4) -> offer(5)
//
// The offer page is the final page everyone in the in-app offer experiment
// reaches: Get connected on the referral page goes there, and Skip from any
// earlier page lands on it once — Skip is not a way around the offer, only
// "Continue with the free plan" on the offer page itself finishes the flow.
// The holdout variant never sees the page: the flow is the four pages it was,
// and Skip finishes immediately.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <string>

namespace urnw {

inline constexpr int kOnboardingStepWelcome = 1;
inline constexpr int kOnboardingStepBandwidth = 2;
inline constexpr int kOnboardingStepProvide = 3;
inline constexpr int kOnboardingStepReferral = 4;
inline constexpr int kOnboardingStepOffer = 5;

inline constexpr const char* kOfferExperimentSurface = "offer.in_app";
inline constexpr const char* kExperimentHoldout = "holdout";

// The in-app offer shows unless the network is in the experiment's holdout.
// An unassigned surface (an old server, no experiments in the balance) shows
// it too: the offer is the product, the holdout is the exception.
inline bool OfferPageEnabled(const std::string& variant) { return variant != kExperimentHoldout; }

inline int OnboardingStepCount(bool offerEnabled) {
  return offerEnabled ? kOnboardingStepOffer : kOnboardingStepReferral;
}

// The page Skip goes to from `current`, or 0 to finish the flow.
inline int OnboardingSkipTarget(int current, bool offerEnabled) {
  if (!offerEnabled) return 0;
  if (kOnboardingStepOffer <= current) return 0;
  return kOnboardingStepOffer;
}

// The page the referral page's primary button goes to, or 0 to finish.
inline int OnboardingReferralNext(bool offerEnabled) {
  return offerEnabled ? kOnboardingStepOffer : 0;
}

// Skip is not offered on the offer page: the page's own link is the way out.
inline bool OnboardingShowsSkip(int step, bool offerEnabled) {
  return !(offerEnabled && step == kOnboardingStepOffer);
}

inline const char* OnboardingStepName(int step) {
  switch (step) {
    case kOnboardingStepWelcome: return "welcome";
    case kOnboardingStepBandwidth: return "bandwidth";
    case kOnboardingStepProvide: return "provide";
    case kOnboardingStepReferral: return "referral";
    case kOnboardingStepOffer: return "offer";
    default: return "";
  }
}

// The deep link destinations under urnetwork://onboarding/<step>.
enum class OnboardingLink { None, Connect, Widgets, Offer, Feedback };

inline OnboardingLink ParseOnboardingLink(const std::string& url) {
  const std::string prefix = "urnetwork://onboarding/";
  if (url.compare(0, prefix.size(), prefix) != 0) return OnboardingLink::None;
  std::string step = url.substr(prefix.size());
  for (const char stop : {'?', '#', '/'}) {
    if (const auto at = step.find(stop); at != std::string::npos) step.erase(at);
  }
  if (step == "connect") return OnboardingLink::Connect;
  if (step == "widgets") return OnboardingLink::Widgets;
  if (step == "offer") return OnboardingLink::Offer;
  if (step == "feedback") return OnboardingLink::Feedback;
  return OnboardingLink::None;
}

}  // namespace urnw
