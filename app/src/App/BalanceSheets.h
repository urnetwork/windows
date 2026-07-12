// Balance and upgrade sheets, as ContentDialogs (macOS parity:
// RedeemBalanceCodeSheet, UpgradeSubscriptionSheet + PurchaseSuccessView).
// Plain C++ helpers like StatsSheets; all methods run on the UI thread, and
// the window forwards live balance-store pushes into the open sheet.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <functional>
#include <memory>
#include <string>

#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Shapes.h>

#include "SdkHost.h"
#include "SubscriptionBalance.h"

namespace urnw {

// Set a TextBlock's inlines from a store string carrying markdown links
// ("...[Terms and Services](https://ur.io/terms)...") — the link text becomes a
// tappable Hyperlink run.
void SetMarkdownLinkText(winrt::Microsoft::UI::Xaml::Controls::TextBlock const& text,
                         std::wstring const& value, double fontSize);

// Set a TextBlock's inlines from the terms_checkbox store string, whose
// {terms_start}/{terms_end} and {privacy_start}/{privacy_end} markers wrap the
// linkable ranges.
void SetTermsMarkerText(winrt::Microsoft::UI::Xaml::Controls::TextBlock const& text,
                        std::wstring const& value, double fontSize);

// ---- Redeem balance code ----------------------------------------------------
// 26-character code entry with inline validation, a where-to-get-codes note,
// and a success state. On success the owner re-polls the balance and refreshes
// the redeemed-codes list.
class RedeemCodeSheet : public std::enable_shared_from_this<RedeemCodeSheet> {
 public:
  static std::shared_ptr<RedeemCodeSheet> Create(
      winrt::Microsoft::UI::Xaml::XamlRoot const& root, SdkHost& sdk,
      std::function<void()> onRedeemed);

  winrt::Microsoft::UI::Xaml::Controls::ContentDialog Dialog() const { return dialog_; }

 private:
  explicit RedeemCodeSheet(SdkHost& sdk, std::function<void()> onRedeemed)
      : sdk_(sdk), onRedeemed_(std::move(onRedeemed)) {}

  void Build(winrt::Microsoft::UI::Xaml::XamlRoot const& root);
  void Submit();
  void ApplyResult(bool ok, bool rejected, std::string const& serverError,
                   int64_t balanceByteCount);

  SdkHost& sdk_;
  std::function<void()> onRedeemed_;
  winrt::Microsoft::UI::Xaml::Controls::ContentDialog dialog_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::StackPanel formPanel_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::StackPanel successPanel_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBox codeBox_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock errorText_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock successAmountText_{nullptr};
  bool redeeming_ = false;
};

// ---- Upgrade to UR Pro -------------------------------------------------------
// Product cards (yearly / monthly) → Stripe checkout → waiting state while the
// balance store's confirmation poll runs → purchase-success (or timeout) state.
// macOS UpgradeSubscriptionSheet parity, with the checkout leg swapped from
// StoreKit to a Stripe session (linux UpgradeSheet parity):
//   - embedded first: the ur.io/checkout bridge page in a WebView2, which
//     mounts Stripe's Embedded Checkout inline and hands control back over the
//     urnetwork:// scheme;
//   - hosted fallback: the system browser, whenever the WebView2 runtime is
//     missing or any embedded step fails before the form rendered. A payment
//     path never hard-fails for want of a webview.
class UpgradeSheet : public std::enable_shared_from_this<UpgradeSheet> {
 public:
  static std::shared_ptr<UpgradeSheet> Create(
      winrt::Microsoft::UI::Xaml::XamlRoot const& root, SdkHost& sdk,
      SubscriptionBalanceStore& balance);

  winrt::Microsoft::UI::Xaml::Controls::ContentDialog Dialog() const { return dialog_; }

  // Balance-store push, forwarded by the window (already on the UI thread):
  // flips the waiting state to success when Pro lands, or to the timeout
  // message when the confirmation poll gives up.
  void OnBalance(BalanceSnapshot const& snapshot, BalancePollState const& poll);

 private:
  UpgradeSheet(SdkHost& sdk, SubscriptionBalanceStore& balance)
      : sdk_(sdk), balance_(balance) {}

  enum class Page { Products, Checkout, Waiting, Success, TimedOut };

  void Build(winrt::Microsoft::UI::Xaml::XamlRoot const& root);
  winrt::Microsoft::UI::Xaml::Controls::Border BuildProductCard(bool yearly);
  void ApplySelection();
  void BeginCheckout();
  // Create a Stripe session in the given ui mode and route the result: embedded
  // → OpenEmbedded (or retry once as hosted), hosted → the system browser.
  void RequestSession(bool embedded);
  // Embedded checkout: swap in the WebView2 page and navigate it to the ur.io
  // bridge for this session's client_secret. Async because WebView2 init is.
  winrt::fire_and_forget OpenEmbedded(std::string clientSecret);
  // A urnetwork:// callback intercepted in the webview (status=complete or
  // errorCode/errorMessage). Runs deferred, on the UI thread.
  void HandleCheckoutCallback(std::string const& uri);
  // The embedded leg failed before Stripe's form rendered: retry as hosted.
  void FallBackToHosted();
  void TeardownWebView();
  void ShowPage(Page page);
  void ShowCheckoutError(winrt::hstring const& message);

  SdkHost& sdk_;
  SubscriptionBalanceStore& balance_;
  winrt::Microsoft::UI::Xaml::Controls::ContentDialog dialog_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::StackPanel productsPanel_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::StackPanel checkoutPanel_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::StackPanel waitingPanel_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::StackPanel successPanel_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::StackPanel timeoutPanel_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock checkoutErrorText_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock waitingBodyText_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Button subscribeButton_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::ProgressRing subscribeRing_{nullptr};
  // embedded checkout: the webview lives in this slot, under a loading ring;
  // it is created per attempt (a closed WebView2 cannot be revived)
  winrt::Microsoft::UI::Xaml::Controls::Grid webviewSlot_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::WebView2 webview_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::ProgressRing checkoutRing_{nullptr};
  // the two product cards + their selection dots
  winrt::Microsoft::UI::Xaml::Controls::Border yearlyCard_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Border monthlyCard_{nullptr};
  winrt::Microsoft::UI::Xaml::Shapes::Ellipse yearlyDot_{nullptr};
  winrt::Microsoft::UI::Xaml::Shapes::Ellipse monthlyDot_{nullptr};

  Page page_ = Page::Products;
  bool yearlySelected_ = true;  // macOS default: yearly, "Most Popular"
  bool checkingOut_ = false;
  bool closed_ = false;  // the dialog was dismissed; drop in-flight checkout legs
  // embedded-checkout attempt state (linux UpgradeSheet parity)
  bool checkoutPageLoaded_ = false;   // the ur.io page rendered at least once
  bool hostedFallbackTried_ = false;  // one embedded→hosted rescue per attempt
  uint32_t webviewGeneration_ = 0;    // drops async results of a torn-down webview
};

}  // namespace urnw
