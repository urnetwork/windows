// Price presentation rules (mmm/onboarding/PLAN.md "price presentation"):
// what the plan cards print from the server's price tier, the SDK's price
// equivalent and the network's welcome offer. Pure: no GTK, no SDK, no
// catalog — the callers pass the SDK's numbers in and wrap the results in
// the store's strings, and the unit tests pin the rules.
//
//   * the billed amount is the number ("$40/year"); the per-month equivalent
//     is a sub-line only, ceiling-rounded by the SDK, and only when the SDK
//     says so and the tier is the standard one — the regional $4 tier never
//     shows one
//   * "Save N%" from the SDK's saving percent, only when there is one
//   * the welcome offer: the first year at percent_off, then the regular
//     price; the deadline is a static local date and time, never a countdown
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace urnw {

inline constexpr const char* kPriceTierStandard = "standard";
inline constexpr const char* kPriceTierRegional = "regional";

// The server's price tier, as the plan cards need it (SubscriptionBalance
// price_tier). Defaults are the standard tier so the cards never print
// nothing before the first fetch.
struct PriceTierView {
  std::string name = kPriceTierStandard;
  double yearly = 40.0;
  double monthly = 5.0;
  std::string currency = "USD";
};

// urnet_compute_price_equivalent's answer, as the cards need it.
struct PriceEquivalentView {
  double monthlyEquivalent = 0;
  bool showEquivalent = false;
  int64_t savingPercent = 0;
};

// The network's welcome offer, as the cards need it (SubscriptionBalance
// onboarding_offer / POST /onboarding/offer/issue).
struct OfferView {
  bool active = false;
  int64_t percentOff = 25;
  int64_t monthsFree = 3;
  double firstYear = 30.0;
  double regularYear = 40.0;
  std::string currency = "USD";
  std::string expiresAt;  // RFC 3339, as the server sent it
};

// A money amount in the tier's currency: whole amounts without decimals
// ("$40"), anything else with two ("$3.34", "$0.50"). USD is the default
// presentation everywhere (Stripe charges in USD; the store apps let the
// stores convert), so only the dollar sign is special-cased; any other code
// prints after the amount ("4 EUR").
inline std::string FormatMoney(double amount, const std::string& currency) {
  const double rounded = std::round(amount * 100.0) / 100.0;
  char buf[64];
  const bool whole = std::fabs(rounded - std::round(rounded)) < 0.005;
  if (whole) {
    std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(std::llround(rounded)));
  } else {
    std::snprintf(buf, sizeof(buf), "%.2f", rounded);
  }
  if (currency.empty() || currency == "USD") return std::string("$") + buf;
  return std::string(buf) + " " + currency;
}

// The per-month sub-line under the yearly price is shown only when the SDK
// computed one worth showing and the tier is the standard one.
inline bool ShowMonthlyEquivalent(const PriceTierView& tier, const PriceEquivalentView& eq) {
  return eq.showEquivalent && tier.name != kPriceTierRegional && 0 < eq.monthlyEquivalent;
}

inline bool ShowSaving(const PriceEquivalentView& eq) { return 0 < eq.savingPercent; }

// The offer's first-year amount from the tier when the server did not send
// one (the picker before the issue call answers): percent_off of the yearly
// price, to the cent.
inline double OfferFirstYear(double yearly, int64_t percentOff) {
  return std::round(yearly * (100.0 - static_cast<double>(percentOff))) / 100.0;
}

// The store's plan ids and the event vocabulary for a picker selection.
inline const char* PlanName(bool yearly) { return yearly ? "yearly" : "monthly"; }
inline const char* PlanProduct(bool yearly) { return yearly ? "pro_yearly" : "pro_monthly"; }

}  // namespace urnw
