// Whether the Manage Subscription row (the Stripe customer portal) shows, from
// the current subscription's store family (urnet::classifySubscriptionStore,
// sdk payment_catalog.go). The portal can only manage a Stripe subscription:
// an App Store or Play subscription is managed in that store, a one-time
// crypto year (the "crypto" family: every coin's market) never renews and has
// nothing to cancel, and a free network has nothing to manage — for those the
// row only produced the portal's raw error (UPGRADE.md D7).
//
// Dependency-free so tools/manage-subscription-tests.cpp runs it without the SDK.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <string>

namespace urnw {

// storeFamily: urnet::classifySubscriptionStore(current_subscription.store),
// or "" without a current subscription
inline bool ShowsManageSubscription(std::string const& storeFamily) {
  return storeFamily == "stripe";  // urnet::SubscriptionStoreStripe
}

}  // namespace urnw
