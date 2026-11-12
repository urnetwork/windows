// Balance and upgrade sheets, as ContentDialogs (macOS parity:
// RedeemBalanceCodeSheet, UpgradeSubscriptionSheet + PurchaseSuccessView).
// Plain C++ helpers like StatsSheets; all methods run on the UI thread, and
// the window forwards live balance-store pushes into the open sheet.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Shapes.h>

#include "SdkHost.h"
#include "PlanPicker.h"
#include "OfferCard.h"
#include "SubscriptionBalance.h"
#include "FreeRefreshTicker.h"
#include "CryptoInvoice.h"
#include "CryptoPayViews.h"

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

// Pair a terms CHECKBOX with the sentence beside it, for UIA, without the
// sentence being announced twice.
//
// AutomationProperties.LabeledBy gives the checkbox its name — it has no
// content of its own and without it the control is nameless — but the label
// TextBlock ALSO stays in the control view carrying the identical string, so
// a screen reader read the whole terms sentence, then read it again. This
// takes the label out of the control view and puts its Hyperlinks BACK in
// individually, because the links are the only way to reach the terms and the
// privacy policy from the keyboard and a subtree hide would take them with it.
//
// Call AFTER SetTermsMarkerText: it works on the inlines that call produces.
void PairTermsLabel(winrt::Microsoft::UI::Xaml::Controls::CheckBox const& box,
                    winrt::Microsoft::UI::Xaml::Controls::TextBlock const& label);

// ---- Redeem balance code ----------------------------------------------------
// 26-character code entry (urnet::isBalanceCodeFormatValid) with inline
// validation, a where-to-get-codes note, and a success state. The answer comes
// from the SDK's classification (BalanceCodeRedeem.h). The success state is
// the data the code added (a code is data only, never a plan). onRedeemed
// fires when this call credited the code (credited=true) or when the network
// already had it (credited=false); either way the owner reads the balance once
// and refreshes the redeemed-codes list.
class RedeemCodeSheet : public std::enable_shared_from_this<RedeemCodeSheet> {
 public:
  static std::shared_ptr<RedeemCodeSheet> Create(
      winrt::Microsoft::UI::Xaml::XamlRoot const& root, SdkHost& sdk,
      std::function<void(bool credited)> onRedeemed);

  winrt::Microsoft::UI::Xaml::Controls::ContentDialog Dialog() const { return dialog_; }

 private:
  explicit RedeemCodeSheet(SdkHost& sdk, std::function<void(bool)> onRedeemed)
      : sdk_(sdk), onRedeemed_(std::move(onRedeemed)) {}

  void Build(winrt::Microsoft::UI::Xaml::XamlRoot const& root);
  void Submit();
  // urnet::classifyBalanceCodeRedeem on the redeem answer (nullopt for a
  // transport failure); anything but "redeemed" is classified again with the
  // network's redeemed-code list before ApplyResult shows it.
  void Classify(std::string const& secret,
                std::optional<urnet::RedeemBalanceCodeResult> const& result);
  void ApplyResult(std::string const& outcome,
                   std::optional<urnet::RedeemBalanceCodeResult> const& result);

  SdkHost& sdk_;
  std::function<void(bool)> onRedeemed_;
  winrt::Microsoft::UI::Xaml::Controls::ContentDialog dialog_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::StackPanel formPanel_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::StackPanel successPanel_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBox codeBox_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock errorText_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock successAmountText_{nullptr};
  bool redeeming_ = false;
};

// ---- About your data --------------------------------------------------------
// What Used, Pending and Available mean, the daily balance the server reports
// and when the free data refreshes (DataInfo.h), from the window's balance
// snapshot. Opened from the info button on the Account usage card and from
// Why? in the out-of-balance banner.
class DataInfoSheet {
 public:
  static std::shared_ptr<DataInfoSheet> Create(
      winrt::Microsoft::UI::Xaml::XamlRoot const& root, BalanceSnapshot const& balance);

  winrt::Microsoft::UI::Xaml::Controls::ContentDialog Dialog() const { return dialog_; }

 private:
  DataInfoSheet() = default;

  void Build(winrt::Microsoft::UI::Xaml::XamlRoot const& root, BalanceSnapshot const& balance);

  winrt::Microsoft::UI::Xaml::Controls::ContentDialog dialog_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock refreshText_{nullptr};
  FreeRefreshTicker refreshTicker_;
};

// ---- Upgrade to UR Pro -------------------------------------------------------
// Product cards (yearly / monthly) → Stripe checkout → waiting state while the
// balance store's confirmation poll runs → purchase-success (or timeout) state.
// The finished card payment is also reported to the server by its Stripe id
// (ReportCardPayment), so a late webhook no longer keeps a paid plan on Free.
// macOS UpgradeSubscriptionSheet parity, with the checkout leg swapped from
// StoreKit to a Stripe session (linux UpgradeSheet parity):
//   - embedded first: the ur.io/checkout bridge page in a WebView2, which
//     mounts Stripe's Embedded Checkout inline and hands control back over the
//     urnetwork:// scheme;
//   - hosted fallback: the system browser, whenever the WebView2 runtime is
//     missing or any embedded step fails before the form rendered. A payment
//     path never hard-fails for want of a webview.
//
// Crypto (crypto-pay CONTRACT §6): while the server sells a coin, the method
// row Card · USDC on Solana · Zcash · Monero · More coins (TAO, Bitcoin, ETH,
// SOL) sits directly above the buy button. Card is
// all of the above. A coin buys the yearly plan (the welcome offer while it is
// active) through the sdk's crypto invoice, which this sheet shows natively on
// its Invoice page, above whatever surface opened it; once the invoice is paid
// the sheet takes the card's success path (the confirmation poll, then the
// success page when Pro lands). One invoice at a time (§6.2.1): while a saved
// invoice is in progress, a coin shows it instead of starting another. Close
// only hides the invoice (it stays saved, and the Account page's "Payment in
// progress" brings it back); Pay another way is the one way to give it up.
class UpgradeSheet : public std::enable_shared_from_this<UpgradeSheet> {
 public:
  // The sheet with the purchase already started for the plan and the payment
  // method an onboarding page picked (the sheet's own products page would only
  // ask again): Card starts the checkout for `yearly`; a coin the server lists
  // opens its invoice instead (crypto-pay §6.3), for the yearly plan, or the
  // welcome offer while it is active. The products page stands behind either
  // for a refusal or Pay another way.
  static std::shared_ptr<UpgradeSheet> CreateForCheckout(
      winrt::Microsoft::UI::Xaml::XamlRoot const& root, SdkHost& sdk,
      SubscriptionBalanceStore& balance, CryptoInvoiceStore& invoice, bool yearly,
      std::string const& method = cryptopay::kMethodCard);

  // The sheet on the crypto invoice the store holds (the Account page's
  // "Payment in progress" row, after a resume), its products page behind it.
  static std::shared_ptr<UpgradeSheet> CreateForInvoice(
      winrt::Microsoft::UI::Xaml::XamlRoot const& root, SdkHost& sdk,
      SubscriptionBalanceStore& balance, CryptoInvoiceStore& invoice);

  // `freeRefresh`: a start connect refused for the balance opened the sheet
  // (datainfo::UpgradeShowsFreeRefresh), so it says when the free data
  // refreshes and offers Wait for refresh.
  static std::shared_ptr<UpgradeSheet> Create(
      winrt::Microsoft::UI::Xaml::XamlRoot const& root, SdkHost& sdk,
      SubscriptionBalanceStore& balance, CryptoInvoiceStore& invoice, bool freeRefresh = false);
  // The invoice stops with the sheet, whatever closed it (the Closed handler
  // does it first; this is the backstop).
  ~UpgradeSheet();

  winrt::Microsoft::UI::Xaml::Controls::ContentDialog Dialog() const { return dialog_; }

  // Balance-store push, forwarded by the window (already on the UI thread):
  // flips the waiting state to success when Pro lands, or to the timeout
  // message when the confirmation poll gives up. The timeout page is not
  // terminal: a later Pro-confirming snapshot (background poll, activation
  // refresh) flips it to success too. A paid crypto invoice waits the same way.
  void OnBalance(BalanceSnapshot const& snapshot, BalancePollState const& poll);

  // Crypto-invoice store push, forwarded by the window (UI thread): the
  // answer to this sheet's start, and every change of the invoice it shows.
  void OnInvoice(cryptopay::CryptoInvoiceView const& view);

  // The server refused the checkout because the network is a legacy guest
  // (guest_sign_in_required): the sheet closed itself, and its opener sends
  // the guest to the conversion once the dialog is gone.
  bool GuestSignInRequired() const { return guestSignInRequired_; }

 private:
  UpgradeSheet(SdkHost& sdk, SubscriptionBalanceStore& balance, CryptoInvoiceStore& invoice)
      : sdk_(sdk), balance_(balance), invoice_(invoice) {}

  enum class Page { Products, Checkout, Waiting, Success, TimedOut, Invoice };

  void Build(winrt::Microsoft::UI::Xaml::XamlRoot const& root);
  void BeginCheckout();
  // ---- crypto (crypto-pay CONTRACT §6) ----
  // The method row from the balance's coins (cryptopay::PaymentTabsFor),
  // keeping the selection, or the device's remembered method until one is
  // made here. Products page only, and never mid-purchase.
  void ApplyMethods();
  // The selection's consequences: monthly locked to Card for a coin, the
  // button's words and the fine print.
  void ApplyMethod(std::string const& method);
  // The button's words for the method and plan: Card's trial or subscribe,
  // a coin's "Pay with {method}".
  winrt::hstring CtaLabel() const;
  // One invoice at a time (§6.2.1): the saved invoice in progress (open, its
  // status not unknown), of any coin, shown on the Invoice page. False, and
  // nothing changed, when there is none.
  bool ShowInvoiceInProgress();
  // "Pay with {coin}": the invoice for the yearly plan, or the welcome offer
  // while it is active. The products page waits with its ring for the answer
  // (OnInvoice). An invoice already in progress is shown instead
  // (ShowInvoiceInProgress), and nothing is started.
  void BeginCryptoInvoice();
  // The start failed: "Temporarily unavailable" under the row (the coin stays
  // selectable for a retry), or the surface's own refusal handling.
  void RefuseCryptoStart(cryptopay::CryptoInvoiceView const& view);
  // The invoice page, painted from the store's state.
  void ShowInvoice();
  void RenderInvoice(cryptopay::CryptoInvoiceView const& view);
  // "Pay another way": the invoice dismissed, back to the products with Card.
  void PayAnotherWay();
  // "Open in wallet": the server's uri to the OS, only once the sdk's check
  // passed; when no wallet takes it the QR stays, and nothing alarming is said.
  winrt::fire_and_forget OpenInWallet();
  // The purchase affordances back after an attempt ended on the products page.
  void ResetPurchaseAffordances();
  // The payment sheet path (mmm/onboarding/PLAN.md: inline Stripe, never an
  // external browser while a web view exists): Api::stripePaymentSheet{plan}
  // -> the ur.io embedded pay page https://ur.io/app/pay-sheet?cs=&pk=&plan=
  // &return= in the WebView2. The page mounts Stripe's Payment Element,
  // confirms the SetupIntent (yearly: the trial, the welcome offer's coupon
  // applied server-side) or the PaymentIntent (monthly), and hands control
  // back by navigating to the return url urnetwork://pay/done or posting
  // {type: "ur-pay", status} (WebMessageReceived). A pay-sheet failure before
  // anything rendered continues with the embedded checkout session.
  void RequestPaymentSheet();
  winrt::fire_and_forget OpenPaySheet(std::string clientSecret, std::string publishableKey);
  void HandlePayMessage(std::string const& json);
  // The balance store's tier/offer: reprint the plan cards and the read-only
  // offer line (shown while the offer state is active).
  void ApplyPrices();
  void EmitPurchase(const char* outcome, std::string const& errorClass = std::string());
  // Create a Stripe session in the given ui mode and route the result: embedded
  // → OpenEmbedded (or retry once as hosted), hosted → LaunchHosted, or the
  // server's refusal in this app's words (PaymentRefusal.h).
  void RequestSession(bool embedded);
  // Open the hosted checkout url in the system browser, OBSERVING the launch:
  // Waiting (+ confirmation poll) only on success; on failure an inline error
  // with the url appended so the user can copy it into a browser by hand.
  winrt::fire_and_forget LaunchHosted(std::string url);
  // Embedded checkout: swap in the WebView2 page and navigate it to the ur.io
  // bridge for this session's client_secret. Async because WebView2 init is.
  winrt::fire_and_forget OpenEmbedded(std::string clientSecret);
  // A urnetwork:// callback intercepted in the webview (status=complete or
  // errorCode/errorMessage). Runs deferred, on the UI thread.
  void HandleCheckoutCallback(std::string const& uri);
  // The embedded leg failed before Stripe's form rendered: retry as hosted.
  void FallBackToHosted();
  // Hands the server the id this attempt's card payment started with
  // (Api::reportStripePurchase), so it reads the payment back from Stripe
  // instead of waiting for the webhook: the payment sheet's subscription, or
  // the checkout session's. Called where the confirmation poll starts, once
  // per attempt; the sdk asks again while the answer is pending. A credited
  // answer reads the balance at once; the page still follows the balance.
  void ReportCardPayment(bool paySheet);
  void TeardownWebView();
  void ShowPage(Page page);
  void ShowCheckoutError(winrt::hstring const& message);
  // The refusal for a guest network (PurchaseRefusalFor): no fallback, no
  // error line; the sheet hides and GuestSignInRequired() says why.
  void RefuseForGuest();

  SdkHost& sdk_;
  SubscriptionBalanceStore& balance_;
  CryptoInvoiceStore& invoice_;
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
  // the plan cards: the picker the onboarding welcome page shows (yearly in
  // the gold dress with the trial, selected by default; monthly plain, no trial)
  PlanPicker plans_;
  // the yearly card's halo pulse, running while the sheet is open (onboarding parity)
  winrt::Microsoft::UI::Xaml::Media::Animation::Storyboard haloStoryboard_{nullptr};
  OfferLines offerLine_;            // the active welcome offer, read-only
  bool purchaseEmitted_ = false;    // purchase.completed once per checkout
  bool paySheetActive_ = false;     // the web view shows the pay page (not Checkout)
  bool freeRefresh_ = false;        // opened by a blocked connect (Create)
  winrt::Microsoft::UI::Xaml::Controls::TextBlock freeRefreshText_{nullptr};
  FreeRefreshTicker freeRefreshTicker_;

  Page page_ = Page::Products;
  bool checkingOut_ = false;
  bool closed_ = false;  // the dialog was dismissed; drop in-flight checkout legs
  bool guestSignInRequired_ = false;  // RefuseForGuest closed the sheet
  // embedded-checkout attempt state (linux UpgradeSheet parity)
  bool checkoutPageLoaded_ = false;   // the ur.io page rendered at least once
  bool hostedFallbackTried_ = false;  // one embedded→hosted rescue per attempt
  uint32_t webviewGeneration_ = 0;    // drops async results of a torn-down webview
  // the Stripe ids the server answered for this attempt, which
  // ReportCardPayment reports: the payment sheet's subscription and the
  // checkout session (embedded or hosted) of the latest leg
  std::string paySheetSubscriptionId_;
  std::string checkoutSessionId_;
  bool cardPaymentReported_ = false;  // ReportCardPayment ran for this attempt

  // ---- crypto (crypto-pay CONTRACT §6) ----
  PaymentMethodRow methods_;  // Card, then the sdk's coin tabs, above the button
  // the method picked here or by the page that opened the sheet; "" until
  // then, when the row starts on the device's remembered method
  std::string wantedMethod_;
  winrt::Microsoft::UI::Xaml::Controls::TextBlock cryptoFinePrint_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::StackPanel invoicePanel_{nullptr};
  CryptoInvoicePanel invoiceView_;  // the Invoice page's content
  // the sdk's answers for the invoice on screen, asked again only when the
  // uri, its address or the asset change (the state changes every second)
  std::string checkedUri_;
  std::string checkedAddress_;
  std::string checkedAsset_;
  bool uriAllowed_ = false;
  std::string walletsAsset_;
  std::vector<std::string> wallets_;
  // the method the purchase events describe: the last attempt's (Card, or the
  // coin of the invoice on screen)
  std::string purchaseMethod_ = cryptopay::kMethodCard;
  double purchasePriceUsd_ = 0;  // a coin's quoted price, once the invoice says it
  bool cryptoStarting_ = false;  // this sheet asked for an invoice; its answer is awaited
  bool invoicePaid_ = false;     // the invoice is paid: the confirmation poll decides Success
  bool invoiceResumed_ = false;  // shows a saved invoice: its purchase began earlier
};

}  // namespace urnw
