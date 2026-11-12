// Executable spec for what the Account plan card shows for the sdk's plan
// decision (App/PlanView.h; support inbox 4074). In December 2025 the balance
// read failed under database load and the apps showed paying networks Free,
// with the upgrade: the card read Free whenever the plan was not Pro, before
// the server had answered and after a failed read alike. Now Pro and Free are
// the server's answer (Pro also by the stored jwt's claim before it answers),
// and an unknown plan reads Loading, or Something went wrong with Try again
// once a read failed, with no upgrade. The decision itself
// (SubscriptionProTracker) is the sdk's, tested there. Run on any host with a
// C++20 compiler:
//
//   c++ -std=c++20 -Wall -Wextra -Werror -I ../src/App plan-view-tests.cpp -o /tmp/plan-view-tests && /tmp/plan-view-tests
//
// SPDX-License-Identifier: MPL-2.0

#include <iostream>
#include <string>
#include <string_view>

#include "PlanView.h"

using namespace urnw::planview;

namespace {

int gFailures = 0;
int gCases = 0;

void Check(bool condition, const std::string& what) {
  ++gCases;
  if (!condition) {
    ++gFailures;
    std::cout << "  FAIL " << what << std::endl;
  }
}

// The card before this change: Pro, else Free, with the upgrade.
std::string_view ValueKeyBefore(bool isPro, bool guest) {
  return guest ? "guest" : isPro ? "supporter" : "free";
}

void TestTheSdksPlanValues() {
  Check(PlanViewFor("pro", false) == PlanView::Pro, "the server's Pro, or a Pro claim");
  Check(PlanViewFor("pro", true) == PlanView::Pro, "a Pro claim stays Pro through a failed read");
  Check(PlanViewFor("free", false) == PlanView::Free, "the server's Free");
  Check(PlanViewFor("free", true) == PlanView::Free,
        "the server's Free stands through a later failed read");
  Check(PlanViewFor("unknown", false) == PlanView::Loading, "the first read on its way");
  Check(PlanViewFor("unknown", true) == PlanView::Failed, "a failed read with no answer since");
  Check(PlanViewFor("", false) == PlanView::Loading, "no plan yet is unknown, not Free");
  Check(PlanViewFor("supporter", true) == PlanView::Failed, "a value it does not know is unknown");
}

void TestAPayingNetworkWhoseReadFailsIsNeverFree() {
  // 2025-12-04: the stored jwt still says Free after the purchase, and every
  // balance read fails
  const PlanView plan = PlanViewFor("unknown", true);
  // control: the card before this change read Free and offered the upgrade
  Check(ValueKeyBefore(false, false) == "free", "control: the old card said Free");
  Check(PlanValueKey(plan, false) != "free", "a failed read is never shown as Free");
  Check(PlanValueKey(plan, false) == "something_went_wrong", "it says the read failed");
  Check(OffersRetry(plan, false), "with Try again");
  Check(!OffersUpgrade(plan, false), "and no upgrade");

  const PlanView loading = PlanViewFor("unknown", false);
  Check(PlanValueKey(loading, false) == "loading", "a read on its way says Loading");
  Check(!OffersRetry(loading, false), "with nothing to retry yet");
  Check(!OffersUpgrade(loading, false), "and no upgrade");
}

void TestTheServersAnswer() {
  Check(PlanValueKey(PlanView::Pro, false) == "supporter", "Pro");
  Check(!OffersUpgrade(PlanView::Pro, false), "Pro has nothing to buy");
  Check(!OffersRetry(PlanView::Pro, false), "Pro has nothing to retry");
  Check(PlanValueKey(PlanView::Free, false) == "free", "Free, once the server says so");
  Check(OffersUpgrade(PlanView::Free, false), "Free offers the upgrade");
  Check(!OffersRetry(PlanView::Free, false), "Free has nothing to retry");
}

void TestAGuest() {
  for (const PlanView plan : {PlanView::Pro, PlanView::Free, PlanView::Loading, PlanView::Failed}) {
    Check(PlanValueKey(plan, true) == "guest", "a guest reads Guest whatever the plan");
    Check(!OffersUpgrade(plan, true), "a guest creates an account first");
    Check(!OffersRetry(plan, true), "a guest's card has no Try again");
  }
}

}  // namespace

int main() {
  TestTheSdksPlanValues();
  TestAPayingNetworkWhoseReadFailsIsNeverFree();
  TestTheServersAnswer();
  TestAGuest();
  std::cout << (gCases - gFailures) << "/" << gCases << " plan view checks passed\n";
  return gFailures == 0 ? 0 : 1;
}
