// Where a window opened by the upgrade sheet's payment pages goes (the pay
// page's and the checkout bridge's WebView2, BalanceSheets.cpp RouteNewWindow
// on CoreWebView2::NewWindowRequested).
//
// Every such window used to go to the system browser. That is right for a
// link (Stripe's terms and privacy are target=_blank), but it breaks a payment
// step: a window Stripe opens to authenticate the card (3D Secure) hands its
// answer back to the payment page through window.opener, which a system
// browser does not have. A window on one of Stripe's payment hosts now stays
// in the app: left unhandled, WebView2 opens it in a window of its own, on the
// sheet's profile, with window.opener kept, and the bank's pages it moves on
// to stay in that window. Stripe's payment hosts:
//
//   hooks.stripe.com     3D Secure and redirect authentication
//   js.stripe.com        Stripe.js's frames, the 3D Secure challenge's among them
//   checkout.stripe.com  Stripe Checkout
//
// Anything else (Stripe's own site at stripe.com, where the terms and privacy
// links go; ur.io; a bank's own links) goes to the system browser as before.
// A bank's pages cannot be listed in advance, since any issuer can run them,
// so a window that a bank's page opens by itself still goes to the system
// browser. Stripe shows its 3D Secure challenge in a frame of the payment page,
// and a redirect to the bank moves the web view itself (only urnetwork:// is
// intercepted), so neither of those needs a window.
//
// Only https, the host exactly as listed (in any letter case), with no user
// info and no port. Anything that does not read cleanly goes to the system
// browser.
//
// WinRT-free so tools/payment-window-tests.cpp runs it on any host.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <array>
#include <string>
#include <string_view>

namespace urnw {

enum class PaymentWindowRoute {
  SystemBrowser,  // the default browser, as every window went before
  InApp,          // left to WebView2, which keeps it in the app with its opener
};

inline constexpr std::array<std::string_view, 3> kStripePaymentWindowHosts = {
    "hooks.stripe.com",
    "js.stripe.com",
    "checkout.stripe.com",
};

inline PaymentWindowRoute PaymentWindowRouteFor(std::string_view uri) {
  const auto lower = [](char c) { return 'A' <= c && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
  constexpr std::string_view kHttps = "https://";
  if (uri.size() <= kHttps.size()) return PaymentWindowRoute::SystemBrowser;
  for (size_t i = 0; i < kHttps.size(); ++i) {
    if (lower(uri[i]) != kHttps[i]) return PaymentWindowRoute::SystemBrowser;
  }
  const std::string_view rest = uri.substr(kHttps.size());
  // the authority ends at the path, the query or the fragment
  const std::string_view authority = rest.substr(0, rest.find_first_of("/?#"));
  // user info could put another host after it, and Stripe names no port
  if (authority.find_first_of("@:") != std::string_view::npos) {
    return PaymentWindowRoute::SystemBrowser;
  }
  std::string host;
  host.reserve(authority.size());
  for (const char c : authority) host += lower(c);
  for (const std::string_view stripeHost : kStripePaymentWindowHosts) {
    if (host == stripeHost) return PaymentWindowRoute::InApp;
  }
  return PaymentWindowRoute::SystemBrowser;
}

}  // namespace urnw
