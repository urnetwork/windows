// Executable spec for the Manage Subscription row's visibility
// (App/ManageSubscription.h): the Stripe customer portal shows only for a
// Stripe subscription, never for a free network or a store subscription
// (UPGRADE.md D7); a gift code's year of Pro reads "A gift. It doesn't renew."
// instead.
//
//   c++ -std=c++20 -I ../src/App manage-subscription-tests.cpp -o /tmp/manage-subscription-tests && /tmp/manage-subscription-tests
//
// SPDX-License-Identifier: MPL-2.0

#include <iostream>
#include <string>
#include <vector>

#include "ManageSubscription.h"

using urnw::PlanStoreFamily;
using urnw::ShowsGiftDoesNotRenew;
using urnw::ShowsManageSubscription;

namespace {

int gFailures = 0;
int gCases = 0;

void Check(bool condition, const std::string& what) {
  ++gCases;
  if (!condition) {
    ++gFailures;
    std::cout << "  FAIL " << what << "\n";
  }
}

}  // namespace

int main() {
  // the families urnet::classifySubscriptionStore returns
  Check(ShowsManageSubscription("stripe"), "a Stripe subscription opens the portal");
  Check(!ShowsManageSubscription(""), "a free network has nothing to manage");
  Check(!ShowsManageSubscription("apple"), "an App Store subscription is managed in the App Store");
  Check(!ShowsManageSubscription("google"), "a Play subscription is managed in Play");
  Check(!ShowsManageSubscription("other"), "a store this app does not know has no portal");
  // a one-time USDC, Zcash or Monero year (urnet::SubscriptionStoreCrypto):
  // it never renews, so there is nothing to manage or cancel (crypto-pay §6.2)
  Check(!ShowsManageSubscription("crypto"), "a crypto year has no portal");
  // a gift code's year of Pro (urnet::SubscriptionStoreGift): someone else paid
  // for it and it never renews, so its own line says so instead
  Check(!ShowsManageSubscription("gift"), "a gift year has no portal");
  Check(ShowsGiftDoesNotRenew("gift"), "a gift year says it does not renew");
  for (const char* family : {"stripe", "apple", "google", "crypto", "other", ""}) {
    Check(!ShowsGiftDoesNotRenew(family), std::string("only a gift year: \"") + family + "\"");
  }

  // A subscriber who pays and redeems a gift keeps the paying store's
  // controls: the gift is the plan's store only while no other store is
  // active, otherwise the first other one in the server's order (android
  // planStore's rule). The families, as the balance's subscriptions list
  // classifies, in its order (by market: gift sorts before google, monero,
  // solana, stripe and zcash).
  using Families = std::vector<std::string>;
  Check(PlanStoreFamily("", Families{}).empty(), "a free network has no plan store");
  Check(PlanStoreFamily("stripe", Families{"gift", "stripe"}) == "stripe",
        "no gift current: the current store, whatever else is active");
  Check(PlanStoreFamily("apple", Families{"apple", "gift"}) == "apple",
        "a store before the gift stays the current one");
  Check(PlanStoreFamily("gift", Families{}) == "gift", "a gift, no list: the gift");
  Check(PlanStoreFamily("gift", Families{"gift"}) == "gift", "a gift alone: the gift");
  Check(PlanStoreFamily("gift", Families{"", "gift"}) == "gift", "an empty store is no store");
  Check(PlanStoreFamily("gift", Families{"gift", "stripe"}) == "stripe",
        "a card subscriber who redeems a gift keeps the card's store");
  Check(PlanStoreFamily("gift", Families{"gift", "google"}) == "google",
        "a Play subscriber who redeems a gift keeps Play's store");
  Check(PlanStoreFamily("gift", Families{"gift", "crypto", "stripe"}) == "crypto",
        "the first paying store in the server's order");
  Check(PlanStoreFamily("gift", Families{"gift", "other"}) == "other",
        "a store this app does not know still pays");
  const std::string card = PlanStoreFamily("gift", Families{"gift", "stripe"});
  Check(ShowsManageSubscription(card) && !ShowsGiftDoesNotRenew(card),
        "a card subscriber with a gift keeps Manage Subscription, and no gift line");
  std::cout << (gCases - gFailures) << "/" << gCases << " manage subscription checks passed\n";
  return gFailures == 0 ? 0 : 1;
}
