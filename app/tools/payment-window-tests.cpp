// Executable spec for where a window opened by the upgrade sheet's payment
// pages goes (App/PaymentWindow.h): a window on one of Stripe's payment hosts
// (3D Secure authentication) stays in the app with its opener, and every other
// window (Stripe's terms and privacy, ur.io, a bank's own links, anything that
// does not read cleanly) goes to the system browser as before.
//
//   c++ -std=c++20 -Wall -Wextra -Werror -I ../src/App payment-window-tests.cpp -o /tmp/payment-window-tests && /tmp/payment-window-tests
//
// SPDX-License-Identifier: MPL-2.0

#include <iostream>
#include <string>

#include "PaymentWindow.h"

using urnw::PaymentWindowRoute;
using urnw::PaymentWindowRouteFor;

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

void CheckInApp(const std::string& uri) {
  Check(PaymentWindowRouteFor(uri) == PaymentWindowRoute::InApp, "stays in the app: " + uri);
}

void CheckSystemBrowser(const std::string& uri) {
  Check(PaymentWindowRouteFor(uri) == PaymentWindowRoute::SystemBrowser,
        "goes to the system browser: \"" + uri + "\"");
}

}  // namespace

int main() {
  // Stripe's payment hosts: the windows of a payment step
  CheckInApp("https://hooks.stripe.com/3d_secure_2/hosted?merchant=acct_1&payment_intent=pi_1");
  CheckInApp("https://hooks.stripe.com/redirect/authenticate/src_1?client_secret=src_client_secret_1");
  CheckInApp("https://js.stripe.com/v3/three-ds-2-challenge-1.html#intentId=seti_1");
  CheckInApp("https://checkout.stripe.com/c/pay/cs_test_1");
  // the host reads in any letter case, and with nothing after it
  CheckInApp("https://HOOKS.Stripe.COM/redirect/authenticate/src_1");
  CheckInApp("HTTPS://hooks.stripe.com/");
  CheckInApp("https://hooks.stripe.com");
  CheckInApp("https://hooks.stripe.com?x=1");
  CheckInApp("https://hooks.stripe.com#x");

  // Stripe's own site: the terms and privacy links stay links
  CheckSystemBrowser("https://stripe.com/legal/end-users");
  CheckSystemBrowser("https://stripe.com/privacy");
  CheckSystemBrowser("https://www.stripe.com/privacy");
  CheckSystemBrowser("https://support.stripe.com/");
  CheckSystemBrowser("https://link.com/terms");
  // the app's site and a bank's own pages
  CheckSystemBrowser("https://ur.io/terms");
  CheckSystemBrowser("https://ur.io/privacy");
  CheckSystemBrowser("https://acs.examplebank.com/challenge?id=1");
  // only https
  CheckSystemBrowser("http://hooks.stripe.com/redirect/authenticate/src_1");
  CheckSystemBrowser("urnetwork://pay/done");
  CheckSystemBrowser("javascript:alert(1)");
  CheckSystemBrowser("about:blank");
  CheckSystemBrowser("");
  CheckSystemBrowser("https://");
  // another host made to look like Stripe's
  CheckSystemBrowser("https://hooks.stripe.com.example.com/redirect");
  CheckSystemBrowser("https://hooks.stripe.com@example.com/redirect");
  CheckSystemBrowser("https://user:pass@hooks.stripe.com/redirect");
  CheckSystemBrowser("https://example.com/https://hooks.stripe.com/");
  CheckSystemBrowser("https://example.com?next=https://hooks.stripe.com/");
  CheckSystemBrowser("https://evilhooks.stripe.com/");
  CheckSystemBrowser("https://stripe.com.hooks.example/");
  CheckSystemBrowser("https://hooks.stripe.co/");
  // anything that does not read cleanly
  CheckSystemBrowser("https://hooks.stripe.com:443/redirect");
  CheckSystemBrowser("https://hooks.stripe.com:8443/redirect");
  CheckSystemBrowser("https://hooks.stripe.com./redirect");
  CheckSystemBrowser("https://hooks%2Estripe.com/redirect");
  CheckSystemBrowser("https://hooks.stripe.com\\@example.com/");
  CheckSystemBrowser(" https://hooks.stripe.com/");
  CheckSystemBrowser("https:hooks.stripe.com/");
  CheckSystemBrowser("https:/hooks.stripe.com/");

  std::cout << (gCases - gFailures) << "/" << gCases << " payment window checks passed\n";
  return gFailures == 0 ? 0 : 1;
}
