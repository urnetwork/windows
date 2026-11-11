// Crypto payments on the billing surfaces (crypto-pay CONTRACT §1, §4.3, §4.4,
// §6; mmm/BTCPAY.md §17.6): the payment-method row's rules over the sdk's coin
// selector, what the native invoice shows for each state of the sdk's
// CryptoInvoiceViewController, the quote countdown, the gate in front of every
// payment uri, and a crypto year's plan rows.
//
// The sdk owns the invoice (quote, countdown, requote, status, persistence)
// and builds every payment uri and the text its QR code holds; this app
// renders CryptoInvoiceState and calls the controller's actions.
// CryptoInvoiceView mirrors that state field for field (CryptoPaySdk.h
// ViewFromSdk is the one place that reads the sdk's struct), and everything a
// surface decides from it is decided here, so the upgrade sheet, the
// onboarding pages and the Account rows cannot disagree. The sdk also lays the
// coins out (urnet::cryptoPaymentTabs: the tabs, their order, their coins,
// their labels and which can be paid with now), read into PaymentTabView.
//
// The literals are the sdk's constants (crypto_pay.go, client_events.go),
// named beside each, in the ManageSubscription.h / PaymentRefusal.h manner.
// The store keys are the localization store's (CONTRACT §5).
//
// WinRT/SDK-free so tools/crypto-pay-tests.cpp runs it on any host.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace urnw::cryptopay {

// ---- vocabulary (CONTRACT §1) ------------------------------------------------

// A method is what the row offers: Card (Stripe), or a coin, named by its asset.
inline constexpr const char* kMethodCard = "card";              // urnet::PaymentMethodCard
inline constexpr const char* kAssetUsdcSolana = "usdc_solana";  // urnet::CryptoAssetUsdcSolana
inline constexpr const char* kAssetZec = "zec";                 // urnet::CryptoAssetZec
inline constexpr const char* kAssetXmr = "xmr";                 // urnet::CryptoAssetXmr
inline constexpr const char* kAssetTao = "tao";                 // urnet::CryptoAssetTao
inline constexpr const char* kAssetBtc = "btc";                 // urnet::CryptoAssetBtc
inline constexpr const char* kAssetEth = "eth";                 // urnet::CryptoAssetEth
inline constexpr const char* kAssetSol = "sol";                 // urnet::CryptoAssetSol

// The selector's tab of the coins beyond the main three, which opens a
// sub-selector of them (urnet::CryptoPaymentTabMoreCoins). A tab of one coin
// is named by its asset.
inline constexpr const char* kTabMoreCoins = "more_coins";

// What this app shows of a coin beyond the sdk's labels: the scheme of its
// payment uri for the gate below (urnet::cryptoPaymentUriScheme, with its
// colon; "" for TAO, which has no payment link), the line naming the network
// it is sent on (the invoice's first line, and the line under the coin in the
// More coins list), the warning beside its address ("" for none), and for a
// coin with no payment link the note under its QR code, which holds the
// address alone. Store keys (CONTRACT §5). A coin missing here is no coin of
// this app's (IsCoin); tests/crypto_pay_test.go holds the schemes to the
// sdk's.
struct CoinWords {
  const char* asset;
  const char* uriScheme;
  const char* networkLineKey;
  const char* networkWarningKey;
  const char* noPaymentLinkKey;
};

inline constexpr std::array<CoinWords, 7> kCoinWords = {{
    {kAssetUsdcSolana, "solana:", "crypto_network_usdc", "usdc_solana_network_only", ""},
    {kAssetZec, "zcash:", "crypto_network_zcash", "", ""},
    {kAssetXmr, "monero:", "crypto_network_monero", "", ""},
    {kAssetTao, "", "crypto_network_tao", "crypto_invoice_tao_network_only",
     "crypto_invoice_tao_no_payment_link"},
    {kAssetBtc, "bitcoin:", "crypto_network_bitcoin", "crypto_invoice_bitcoin_network_only", ""},
    {kAssetEth, "ethereum:", "crypto_network_eth", "crypto_invoice_eth_network_only", ""},
    {kAssetSol, "solana:", "crypto_network_sol", "crypto_invoice_sol_network_only", ""},
}};

// The words of `asset`, or nullptr for anything else.
inline const CoinWords* WordsFor(std::string_view asset) {
  for (const CoinWords& words : kCoinWords) {
    if (asset == words.asset) return &words;
  }
  return nullptr;
}

// The plans a surface sells. No coin sells monthly (decision 6).
inline constexpr const char* kPlanYearly = "yearly";                       // urnet::PlanYearly
inline constexpr const char* kPlanMonthly = "monthly";                     // urnet::PlanMonthly
inline constexpr const char* kPlanYearlyOnboarding = "yearly_onboarding";  // urnet::PlanYearlyOnboarding

// CryptoInvoiceState.Phase (§4.4)
inline constexpr const char* kPhaseStarting = "starting";
inline constexpr const char* kPhaseOpen = "open";
inline constexpr const char* kPhasePaid = "paid";
inline constexpr const char* kPhaseFailed = "failed";

// CryptoInvoiceState.Status, the server's (§2.5): urnet::CryptoPaymentStatus*
inline constexpr const char* kStatusPending = "pending";
inline constexpr const char* kStatusSeen = "seen";
inline constexpr const char* kStatusConfirming = "confirming";
inline constexpr const char* kStatusPaid = "paid";
inline constexpr const char* kStatusExpired = "expired";
inline constexpr const char* kStatusReview = "review";
inline constexpr const char* kStatusUnknown = "unknown";

// The refusal codes a start or a requote reports (§2.7; PurchaseErrorCode*)
inline constexpr const char* kErrorAssetUnavailable = "asset_unavailable";
inline constexpr const char* kErrorQuoteExpired = "quote_expired";
inline constexpr const char* kErrorGuestSignInRequired = "guest_sign_in_required";

// ---- the method row (§6.1, mmm/BTCPAY.md §17.6) -------------------------------
// Card, then the sdk's coin selector: the tabs USDC on Solana, Zcash, Monero
// and More coins, which opens a sub-selector of TAO, Bitcoin, ETH and SOL. The
// sdk decides the tabs, their order, their coins, their labels and which coins
// can be paid with now (urnet::cryptoPaymentTabs, read by CryptoPaySdk.h
// PaymentTabsFor); the rules below only choose among them.

// A coin of this app's (kCoinWords): not Card, nothing, or a ticker.
inline bool IsCoin(std::string_view method) { return WordsFor(method) != nullptr; }

// A coin of the selector (urnet::CryptoPaymentCoin).
struct PaymentCoinView {
  std::string asset;
  std::string label;     // in English: the fallback when the catalog lacks labelKey
  std::string labelKey;  // the store key of its label
  std::string iconKey;   // its mark: the bundled Assets\<iconKey>.svg
  // payable now. A coin that is not stays in its place and cannot be chosen:
  // its label is greyed and says "Temporarily unavailable", and its mark stays
  // at full contrast (Solana's and Bittensor's brand rules).
  bool available = false;

  bool operator==(const PaymentCoinView&) const = default;
};

// A tab of the selector (urnet::CryptoPaymentTab).
struct PaymentTabView {
  std::string id;  // the asset of a tab of one coin, or kTabMoreCoins
  std::string label;
  std::string labelKey;
  // the marks the tab shows, in order: for More coins each coin's, as separate
  // marks with space between them, never one badge
  std::vector<std::string> iconKeys;
  std::vector<PaymentCoinView> coins;  // in the sub-selector's order
  bool available = false;              // one of its coins is

  bool operator==(const PaymentTabView&) const = default;
};

// The row shows only while the sdk lays a selector out: its tabs are empty
// when no coin the server lists sells the surface's item, and the surface then
// looks exactly as it did before crypto.
inline bool MethodRowShows(const std::vector<PaymentTabView>& tabs) { return !tabs.empty(); }

// The coin `asset` among the tabs, or nullptr.
inline const PaymentCoinView* FindCoin(const std::vector<PaymentTabView>& tabs, std::string_view asset) {
  for (const PaymentTabView& tab : tabs) {
    for (const PaymentCoinView& coin : tab.coins) {
      if (coin.asset == asset) return &coin;
    }
  }
  return nullptr;
}

// Whether the row offers `method` now: Card always, a coin of this app's while
// the selector has it available.
inline bool MethodOffered(const std::vector<PaymentTabView>& tabs, std::string_view method) {
  if (method == kMethodCard) return true;
  if (!IsCoin(method)) return false;
  const PaymentCoinView* coin = FindCoin(tabs, method);
  return coin != nullptr && coin->available;
}

// The method the row selects: `wanted` (the device's remembered choice, or
// the selection before the tabs changed) while it is offered, else Card. A
// coin that cannot be paid with now, or that the server stopped selling,
// falls back to Card.
inline std::string MethodToSelect(const std::vector<PaymentTabView>& tabs, std::string_view wanted) {
  return MethodOffered(tabs, wanted) ? std::string(wanted) : std::string(kMethodCard);
}

// A tab opens a sub-selector of its coins (More coins); a tab of one coin is
// that coin.
inline bool TabOpensCoins(const PaymentTabView& tab) { return tab.id == kTabMoreCoins; }

// The coin choosing `tab` selects: `wanted` (the coin chosen there last, or
// remembered) while it is one of the tab's available coins, else the tab's
// first available coin; "" when none is, and the tab cannot be chosen.
inline std::string TabCoinToSelect(const PaymentTabView& tab, std::string_view wanted) {
  for (const PaymentCoinView& coin : tab.coins) {
    if (coin.available && coin.asset == wanted && IsCoin(coin.asset)) return coin.asset;
  }
  for (const PaymentCoinView& coin : tab.coins) {
    if (coin.available && IsCoin(coin.asset)) return coin.asset;
  }
  return std::string();
}

// The plan a coin buys on a plan surface: the welcome offer while it is
// active (the card path keeps its own trial and coupon handling), else yearly.
inline const char* CryptoPlanFor(bool offerActive) {
  return offerActive ? kPlanYearlyOnboarding : kPlanYearly;
}

// The items a plan surface asks urnet::paymentMethods about: the plan a coin
// would buy there, so a coin that does not sell the welcome offer is not
// offered while the offer is what a coin buys.
inline std::vector<std::string> PlanSurfaceItems(bool offerActive) {
  return {CryptoPlanFor(offerActive)};
}

// A method's label and the store its events name are the sdk's
// (CryptoPaySdk.h MethodLabel, MethodEventStore).

// Only the card's yearly plan starts with a free trial.
inline bool PurchaseHasTrial(std::string_view method, bool yearly) {
  return yearly && !IsCoin(method);
}

// ---- no renewal or trial wording while a coin is selected (§6.1) -------------
// A coin buys one year once, with no trial and no renewal. While one is
// selected every trial or renewal line on the plan and offer cards goes: the
// renewal price under the offer's first-year price (offer_then_regular_price),
// the offer's terms (offer_terms_first_year), "Includes N day free trial"
// (includes_free_trial_days) and the offer's trial timeline. The price stays
// what the coin pays (the offer's first-year price while the offer is
// active), the offer's deadline stays, and the coin's fine print under the
// button (crypto_fine_print_yearly) replaces the hidden lines. Card keeps
// every line. (Events still name the plan yearly, the welcome-offer year too:
// the server's event schema takes only yearly and monthly, and
// CryptoPlanFor's yearly_onboarding is only ever asked of the sdk.)
inline bool CardTermsShown(std::string_view method) {
  return !IsCoin(method);
}

// The yearly plan card's lines (PlanPicker).
enum class PlanCardLine { Price, Secondary, Equivalent, Trial };

struct PlanCardSlot {
  PlanCardLine line = PlanCardLine::Price;
  // false: printed transparent and kept from screen readers, holding the room
  // the line takes for Card
  bool shown = true;
};

// What the yearly card's four text blocks print, top to bottom: block 0 the
// price; blocks 1 and 2, alike in style, the secondary line (the saving, or
// the offer's renewal price: `secondaryRenews`) and the monthly equivalent;
// block 3 the trial line. While a coin is selected (`cardTermsShown` false)
// the trial and a renewal price go, transparent, so the card keeps its height
// and nothing under it moves as the method changes; a renewal price moves
// below the equivalent first, so no gap opens under the price.
inline std::array<PlanCardSlot, 4> YearlyCardSlots(bool cardTermsShown, bool secondaryRenews) {
  const bool renewalGoes = !cardTermsShown && secondaryRenews;
  return {{
      {PlanCardLine::Price, true},
      renewalGoes ? PlanCardSlot{PlanCardLine::Equivalent, true}
                  : PlanCardSlot{PlanCardLine::Secondary, true},
      renewalGoes ? PlanCardSlot{PlanCardLine::Secondary, false}
                  : PlanCardSlot{PlanCardLine::Equivalent, true},
      {PlanCardLine::Trial, cardTermsShown},
  }};
}

// ---- the invoice (§6.2) -------------------------------------------------------

// The invoice's first line: the network the coin is sent on. The More coins
// list names it under each coin too.
inline const char* NetworkLineKey(std::string_view asset) {
  const CoinWords* words = WordsFor(asset);
  return words ? words->networkLineKey : "";
}

// The warning beside the address: the network to send on, and that anything
// sent on another is not credited. "" for a coin without one.
inline const char* NetworkWarningKey(std::string_view asset) {
  const CoinWords* words = WordsFor(asset);
  return words ? words->networkWarningKey : "";
}

// The payment uri scheme of each asset (§3.5, §4.3; mmm/BTCPAY.md §17.7), ""
// for a coin with no payment link.
inline const char* UriSchemeFor(std::string_view asset) {
  const CoinWords* words = WordsFor(asset);
  return words ? words->uriScheme : "";
}

// A coin of this app's with no payment link (TAO): its invoice is paid by
// hand, the address and the exact amount, and its QR code holds the address
// alone.
inline bool HasNoPaymentLink(std::string_view asset) {
  return IsCoin(asset) && std::string_view(UriSchemeFor(asset)).empty();
}

// The note under such a coin's QR code, which says it holds only the address.
inline const char* NoPaymentLinkKey(std::string_view asset) {
  const CoinWords* words = WordsFor(asset);
  return words ? words->noPaymentLinkKey : "";
}

// The gate in front of the QR and Open in wallet. The server builds the uri
// and the sdk's urnet::checkCryptoPaymentUri(asset, uri, address) checks that
// its scheme is the asset's and that it pays exactly `address`; this app never
// draws or opens a uri that check did not pass. `check` is that sdk call (a
// fake in the spec); a check that throws refuses. Before it runs, the gate
// also refuses what it can see itself: an empty field, a scheme other than the
// asset's, and a space, a control character or a byte outside ASCII (a uri
// is ASCII; the server percent-encodes), none of which is handed to the OS.
template <typename Check>
bool CryptoUriAllowed(const std::string& asset, const std::string& uri,
                      const std::string& address, Check&& check) {
  const std::string_view scheme = UriSchemeFor(asset);
  if (scheme.empty() || uri.size() <= scheme.size() || address.empty()) return false;
  for (size_t at = 0; at < scheme.size(); ++at) {
    char c = uri[at];
    if ('A' <= c && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    if (c != scheme[at]) return false;
  }
  for (const char c : uri) {
    const auto byte = static_cast<unsigned char>(c);
    if (byte <= 0x20 || 0x7F <= byte) return false;
  }
  try {
    return static_cast<bool>(check(asset, uri, address));
  } catch (...) {
    return false;
  }
}

// The QR of the payment uri, drawn as rectangles on a white frame: a whole
// number of dip per module (a fraction leaves seams between the rectangles and
// makes a camera work harder) and the spec's quiet zone of four modules on
// every side, all inside `maxFrameSide`. A payment uri carries a long address
// (a Zcash unified address is over 200 characters), so the code runs to 60-80
// modules and the frame is as large as the sheet allows.
inline constexpr int kQrQuietModules = 4;

struct QrLayout {
  int moduleCount = 0;    // the code's side, in modules
  double moduleSize = 0;  // dip per module
  double codeSide = 0;    // moduleCount * moduleSize
  double quietZone = 0;   // the white margin around the code
  double frameSide = 0;   // codeSide + 2 * quietZone
};

// A moduleCount of zero (no code) or a frame of no size draws nothing.
inline QrLayout QrLayoutFor(int moduleCount, double maxFrameSide) {
  QrLayout layout;
  if (moduleCount <= 0 || maxFrameSide <= 0) return layout;
  layout.moduleCount = moduleCount;
  layout.moduleSize = std::floor(maxFrameSide / (moduleCount + 2 * kQrQuietModules));
  if (layout.moduleSize < 1) layout.moduleSize = 1;
  layout.codeSide = layout.moduleSize * moduleCount;
  layout.quietZone = layout.moduleSize * kQrQuietModules;
  layout.frameSide = layout.codeSide + 2 * layout.quietZone;
  return layout;
}

// "m:ss" for the quote's countdown ("Quote expires in {time}"): whole minutes,
// then two-digit seconds. A USDC quote runs an hour, so minutes go past 59
// ("60:00"); nothing goes below "0:00".
inline std::string FormatCountdown(int64_t seconds) {
  if (seconds < 0) seconds = 0;
  const int64_t minutes = seconds / 60;
  const int64_t rest = seconds % 60;
  return std::to_string(minutes) + (rest < 10 ? ":0" : ":") + std::to_string(rest);
}

// Whether a decimal amount string from the server ("0.0125", "0") is above
// zero. Only digits and one point are a number here; anything else is not.
inline bool DecimalIsPositive(std::string_view value) {
  bool point = false;
  bool digit = false;
  bool nonzero = false;
  for (const char c : value) {
    if (c == '.') {
      if (point) return false;
      point = true;
    } else if ('0' <= c && c <= '9') {
      digit = true;
      if (c != '0') nonzero = true;
    } else {
      return false;
    }
  }
  return digit && nonzero;
}

// "0.072387 XMR": the server's exact amount with the asset's ticker.
inline std::string AmountWithTicker(const std::string& amount, const std::string& ticker) {
  if (amount.empty()) return std::string();
  if (ticker.empty()) return amount;
  return amount + " " + ticker;
}

// "Cake, Feather, Monerujo" for crypto_invoice_wallets.
inline std::string JoinNames(const std::vector<std::string>& names) {
  std::string joined;
  for (const std::string& name : names) {
    if (name.empty()) continue;
    if (!joined.empty()) joined += ", ";
    joined += name;
  }
  return joined;
}

// The sdk's CryptoInvoiceState (§4.4), as this app reads it.
struct CryptoInvoiceView {
  std::string phase;  // none | starting | open | paid | failed ("" before any state)
  std::string intentId;
  std::string asset;
  std::string plan;
  std::string itemId;
  std::string address;
  std::string amount;
  std::string ticker;  // urnet::cryptoAssetTicker(asset), set by the sdk
  // the server's uri, for Open in wallet; the sdk already empties one that
  // fails its check (and TAO has none), and this app checks again before
  // drawing or opening it (CryptoUriAllowed)
  std::string uri;
  // what the QR code holds (urnet::cryptoPaymentQrText): the uri once the
  // sdk's check passed it, or for a coin with no payment link the address
  // alone; "" for no QR code
  std::string qrText;
  std::string memo;  // usdc_solana and sol: the memo for a transfer sent by hand
  double priceUsd = 0;
  std::string expiresAt;
  int64_t secondsLeft = 0;  // 0 once expired
  std::string status;       // §2.5
  std::string reviewReason;
  std::string received;
  std::string remaining;
  int64_t confirmations = 0;
  int64_t confirmationsRequired = 0;
  bool refreshing = false;  // a requote is in flight
  // the last start or requote failure: a refusal code, or "" with a message
  // for a failure without an answer from the server
  std::string errorCode;
  std::string errorMessage;
};

// What the hosting surface does with a state.
enum class InvoiceRoute {
  None,         // no invoice: never started, or dismissed
  Starting,     // a start is in flight
  Open,         // an invoice in progress
  Paid,         // credited: the surface's success path
  Unavailable,  // the start failed with asset_unavailable: "Temporarily unavailable"
  Refused,      // the start failed otherwise: the surface's own refusal handling
  Unknown,      // the server no longer knows the invoice (status unknown); the sdk forgot it
};

inline InvoiceRoute RouteFor(const CryptoInvoiceView& view) {
  if (view.status == kStatusUnknown) return InvoiceRoute::Unknown;
  if (view.phase == kPhaseStarting) return InvoiceRoute::Starting;
  if (view.phase == kPhasePaid) return InvoiceRoute::Paid;
  if (view.phase == kPhaseOpen) {
    return view.status == kStatusPaid ? InvoiceRoute::Paid : InvoiceRoute::Open;
  }
  if (view.phase == kPhaseFailed) {
    return view.errorCode == kErrorAssetUnavailable ? InvoiceRoute::Unavailable
                                                    : InvoiceRoute::Refused;
  }
  return InvoiceRoute::None;
}

// An invoice in progress (§6.2.1): open, and known to the server. One at a
// time: while one is, the coin path shows it instead of starting another
// (Pay another way is the way out), and the Account page offers it back as
// "Payment in progress".
inline bool InvoiceInProgress(const CryptoInvoiceView& view) {
  return RouteFor(view) == InvoiceRoute::Open;
}

// Nothing was received, as the server said: pending or expired. The status of
// a resumed invoice before its first poll ("") is not that: money may already
// be on its way for it (§6.2.1).
inline bool NothingReceived(const CryptoInvoiceView& view) {
  return view.status == kStatusPending || view.status == kStatusExpired;
}

// The quote ran out with nothing received: the server's expired, or a pending
// quote whose countdown reached zero before the next poll said so.
inline bool QuoteExpired(const CryptoInvoiceView& view) {
  return view.status == kStatusExpired ||
         (view.status == kStatusPending && view.secondsLeft <= 0);
}

// The status line's key: waiting -> payment seen -> confirming n of N -> paid,
// and the review copy instead of any of them.
inline const char* StatusKey(const CryptoInvoiceView& view) {
  if (view.status == kStatusReview) return "crypto_status_review";
  if (view.phase == kPhasePaid || view.status == kStatusPaid) return "crypto_status_paid";
  if (view.status == kStatusConfirming) return "crypto_status_confirming";
  if (view.status == kStatusSeen) return "crypto_status_seen";
  return "crypto_status_waiting";
}

// The step the status has reached: 0 waiting, 1 payment seen, 2 confirming,
// 3 paid; -1 in review, which shows its copy instead of the steps.
inline int StatusStep(const CryptoInvoiceView& view) {
  if (view.status == kStatusReview) return -1;
  if (view.phase == kPhasePaid || view.status == kStatusPaid) return 3;
  if (view.status == kStatusConfirming) return 2;
  if (view.status == kStatusSeen) return 1;
  return 0;
}

enum class QuoteLine {
  Hidden,
  Counting,  // "Quote expires in m:ss"
  Expired,   // crypto_invoice_quote_expired
};

// Everything the invoice shows for one state, top to bottom (§6.2), by the
// rules of §6.2.1:
//   payable    open, pending and time left: the amount, the QR (the sdk's qr
//              text: the uri the gate passed, or the address alone for a coin
//              with no payment link), Open in wallet (a non-empty uri the gate
//              passed), the address with the coin's network warning, the memo
//              of a coin on Solana and the countdown. A coin with no payment
//              link (TAO) says to send exactly this amount to this address,
//              its Copy buttons say what they copy, and its QR says it holds
//              only the address
//   not polled a resumed invoice before its first status (""): a brief
//              loading line in place of the payment details, nothing payable
//   expired    expired, or pending at zero: the expired line and Refresh
//              quote, no amount, no QR, no Open in wallet
//   seen       the address with Copy for the top-up, and "{remaining} more to
//              send"; never the QR or the full amount (scanning again overpays)
//   confirming the status steps
//   review     crypto_status_review, nothing else to do
// Refresh quote only while nothing was received (pending, expired), disabled
// while a requote is in flight. Pay another way only while nothing was
// received, or when the invoice failed or is unknown; otherwise only Close,
// which hides the invoice and leaves "Payment in progress".
struct CryptoInvoiceDisplay {
  std::string networkLineKey;  // 1. the network, first
  // a start in flight, or a resumed invoice before its first status: a brief
  // loading line in place of the payment details
  bool busy = false;
  // 2-3. what to send, where
  bool showAmount = false;  // "Send exactly" and the amount with its ticker
  std::string amountText;
  bool showQr = false;
  std::string qrText;  // what the QR encodes while it shows
  bool showOpenWallet = false;
  // a coin with no payment link (TAO): "Send exactly this amount to this
  // address." while payable, Copy amount and Copy address on the buttons, and
  // under its QR (the address alone) the note saying so
  bool noPaymentLink = false;
  bool showByHandLine = false;
  std::string noPaymentLinkKey;  // "" while no such QR shows
  bool showAddress = false;
  std::string address;
  bool showMemo = false;  // a coin on Solana: the memo, for a transfer sent by hand
  std::string memo;
  // the coin's network warning, with the address ("" for a coin without one)
  bool showNetworkWarning = false;
  std::string networkWarningKey;
  // 4. the quote
  QuoteLine quote = QuoteLine::Hidden;
  std::string countdown;
  bool showRefresh = false;
  bool refreshEnabled = false;
  // 5. the status
  bool showSteps = false;
  int step = 0;
  std::string statusKey;
  bool statusCounts = false;  // crypto_status_confirming takes the counts
  int64_t confirmations = 0;
  int64_t confirmationsRequired = 0;
  bool showRemaining = false;  // crypto_invoice_remaining
  std::string remainingText;
  bool review = false;  // crypto_status_review, and nothing else to do
  bool paid = false;
  // 6. help, while a payment is asked for
  bool showWallets = false;
  bool showZcashNote = false;
  // 7. Pay another way (else only Close)
  bool showPayAnotherWay = false;
  // a failure: a requote the coin cannot be quoted for now
  // (crypto_invoice_unavailable), or anything else in the surface's refusal
  // words (a requote refused, an invoice the server no longer knows)
  bool showUnavailable = false;
  bool showRefusal = false;
};

// `uriAllowed` is CryptoUriAllowed's answer for the view's asset, uri and
// address.
inline CryptoInvoiceDisplay DisplayFor(const CryptoInvoiceView& view, bool uriAllowed) {
  CryptoInvoiceDisplay display;
  display.networkLineKey = NetworkLineKey(view.asset);
  const InvoiceRoute route = RouteFor(view);
  const bool open = route == InvoiceRoute::Open;
  const std::string& status = view.status;
  const bool unpolled = open && status.empty();
  display.busy = route == InvoiceRoute::Starting || unpolled;
  display.review = open && status == kStatusReview;
  display.paid = route == InvoiceRoute::Paid;
  const bool expired = open && QuoteExpired(view);
  const bool payable = open && status == kStatusPending && 0 < view.secondsLeft;
  const bool partial = open && status == kStatusSeen;
  const std::string& ticker = view.ticker;

  display.showAmount = payable && !view.amount.empty();
  display.amountText = AmountWithTicker(view.amount, ticker);
  // The QR draws the sdk's qr text: the uri, once the sdk's check and this
  // app's gate passed it, or for a coin with no payment link the bare address.
  // An empty uri (another device requoted the invoice, or the sdk's check
  // failed and it blanked the uri) leaves a coin with a payment link no QR and
  // no Open in wallet: the address and the exact amount, sent by hand
  // (CONTRACT §4.4.1).
  const bool qrIsUri = uriAllowed && !view.uri.empty() && view.qrText == view.uri;
  const bool qrIsAddress = HasNoPaymentLink(view.asset) && view.uri.empty() &&
                           !view.address.empty() && view.qrText == view.address;
  display.showQr = payable && (qrIsUri || qrIsAddress);
  display.qrText = display.showQr ? view.qrText : std::string();
  display.showOpenWallet = payable && uriAllowed && !view.uri.empty();
  display.noPaymentLink = HasNoPaymentLink(view.asset);
  display.showByHandLine = display.noPaymentLink && display.showAmount;
  display.noPaymentLinkKey = display.showQr && qrIsAddress ? NoPaymentLinkKey(view.asset) : "";
  display.showAddress = (payable || partial) && !view.address.empty();
  display.address = view.address;
  display.showMemo = payable && (view.asset == kAssetUsdcSolana || view.asset == kAssetSol) &&
                     !view.memo.empty();
  display.memo = view.memo;
  display.networkWarningKey = NetworkWarningKey(view.asset);
  display.showNetworkWarning = display.showAddress && !display.networkWarningKey.empty();

  if (expired) {
    display.quote = QuoteLine::Expired;
  } else if (payable) {
    display.quote = QuoteLine::Counting;
    display.countdown = FormatCountdown(view.secondsLeft);
  }
  display.showRefresh = open && NothingReceived(view);
  display.refreshEnabled = display.showRefresh && !view.refreshing;

  const bool stepped =
      status == kStatusPending || status == kStatusSeen || status == kStatusConfirming;
  display.showSteps = (open && stepped && !expired) || display.paid;
  display.step = StatusStep(view);
  display.statusKey = StatusKey(view);
  display.statusCounts = status == kStatusConfirming && !display.paid;
  display.confirmations = view.confirmations;
  display.confirmationsRequired = view.confirmationsRequired;
  display.showRemaining = partial && DecimalIsPositive(view.remaining);
  display.remainingText = AmountWithTicker(view.remaining, ticker);

  display.showWallets = payable || partial;
  display.showZcashNote = (payable || partial) && view.asset == kAssetZec;

  const bool failed = route == InvoiceRoute::Unavailable || route == InvoiceRoute::Refused;
  const bool unknown = route == InvoiceRoute::Unknown;
  display.showPayAnotherWay = (open && NothingReceived(view)) || failed || unknown;

  const bool quiet = display.paid || display.review;
  display.showUnavailable = !quiet && view.errorCode == kErrorAssetUnavailable;
  display.showRefusal =
      !quiet && !display.showUnavailable &&
      (unknown || failed ||
       (!view.errorCode.empty() && view.errorCode != kErrorQuoteExpired) ||
       (view.errorCode.empty() && !view.errorMessage.empty()));
  return display;
}

// The Account page's "Payment in progress" row (crypto_payment_in_progress):
// an invoice in progress (Resume brings it back at launch and when the page
// appears); it reopens the invoice. Its second line is the status line, and
// the network line names the coin.
inline bool ShowsPaymentInProgress(const CryptoInvoiceView& view) {
  return InvoiceInProgress(view);
}

// That row's second line: none before the first status (money may be on its
// way), the expired line once the quote ran out, else the status line
// (crypto_status_confirming takes the counts).
inline const char* InProgressNoteKey(const CryptoInvoiceView& view) {
  if (!InvoiceInProgress(view) || view.status.empty()) return "";
  if (QuoteExpired(view)) return "crypto_invoice_quote_expired";
  return StatusKey(view);
}

// The status steps an asset passes through: waiting -> payment seen ->
// confirming n of N -> paid. USDC only ever reports pending, paid or expired
// (§2.5: the server sees confirmed transfers only), so its invoice shows
// waiting -> paid; and the confirming step needs the confirmations the asset
// requires, which the quote carries.
inline std::vector<int> StepsFor(std::string_view asset, int64_t confirmationsRequired) {
  if (asset == kAssetUsdcSolana) return {0, 3};
  if (confirmationsRequired <= 0) return {0, 1, 3};
  return {0, 1, 2, 3};
}

// ---- a crypto year's plan rows (§2.2, §6.2) -------------------------------------
// When the renewal notice shows is the sdk's rule (urnet::cryptoTermNoticeDue:
// from term_end_time until grace_end_time, never before); what is decided here
// is its date and the "Does not renew" line.

// RFC 3339 ("2026-10-20T18:30:00Z", "...+02:00", fractional seconds allowed)
// to Unix seconds; nullopt for anything else. The notice's "Renew by {date}"
// is the grace's end in the user's own calendar.
inline std::optional<int64_t> ParseRfc3339(std::string_view text) {
  size_t at = 0;
  auto digits = [&](size_t count, int64_t& out) {
    if (text.size() < at + count) return false;
    int64_t value = 0;
    for (size_t i = 0; i < count; ++i) {
      const char c = text[at + i];
      if (c < '0' || '9' < c) return false;
      value = value * 10 + (c - '0');
    }
    at += count;
    out = value;
    return true;
  };
  auto expect = [&](char wanted) {
    if (text.size() <= at) return false;
    const char c = text[at];
    const bool same = c == wanted || (wanted == 'T' && c == 't');
    if (same) ++at;
    return same;
  };
  int64_t year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
  if (!digits(4, year) || !expect('-') || !digits(2, month) || !expect('-') || !digits(2, day) ||
      !expect('T') || !digits(2, hour) || !expect(':') || !digits(2, minute) || !expect(':') ||
      !digits(2, second)) {
    return std::nullopt;
  }
  if (at < text.size() && text[at] == '.') {
    ++at;
    const size_t fraction = at;
    while (at < text.size() && '0' <= text[at] && text[at] <= '9') ++at;
    if (at == fraction) return std::nullopt;
  }
  int64_t offsetSeconds = 0;
  if (at < text.size() && (text[at] == 'Z' || text[at] == 'z')) {
    ++at;
  } else if (at < text.size() && (text[at] == '+' || text[at] == '-')) {
    const bool west = text[at] == '-';
    ++at;
    int64_t offsetHours = 0, offsetMinutes = 0;
    if (!digits(2, offsetHours) || !expect(':') || !digits(2, offsetMinutes) || 23 < offsetHours ||
        59 < offsetMinutes) {
      return std::nullopt;
    }
    offsetSeconds = (offsetHours * 60 + offsetMinutes) * 60 * (west ? -1 : 1);
  } else {
    return std::nullopt;
  }
  if (at != text.size()) return std::nullopt;
  const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
  static constexpr int64_t kDaysInMonth[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month < 1 || 12 < month || day < 1 || 23 < hour || 59 < minute || 60 < second) {
    return std::nullopt;
  }
  if (kDaysInMonth[month - 1] + (month == 2 && leap ? 1 : 0) < day) return std::nullopt;
  // days from 1970-01-01 (the civil-calendar count)
  const int64_t y = month <= 2 ? year - 1 : year;
  const int64_t era = (0 <= y ? y : y - 399) / 400;
  const int64_t yearOfEra = y - era * 400;
  const int64_t dayOfYear = (153 * (month <= 2 ? month + 9 : month - 3) + 2) / 5 + day - 1;
  const int64_t dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
  const int64_t days = era * 146097 + dayOfEra - 719468;
  return days * 86400 + hour * 3600 + minute * 60 + second - offsetSeconds;
}

// The network's latest one-time crypto credit whose grace has not ended
// (SubscriptionBalanceResult.crypto_term). Empty fields: none.
struct CryptoTermView {
  std::string market;
  std::string termEndTime;
  std::string graceEndTime;
};

// A subscription in the "crypto" store family (urnet::classifySubscriptionStore:
// solana, zcash, monero, bittensor, bitcoin and ethereum) is one-time: it
// never renews, so nothing manages or cancels it, and the term notice is its
// renewal reminder.
inline constexpr const char* kStoreFamilyCrypto = "crypto";  // urnet::SubscriptionStoreCrypto

inline bool StoreFamilyRenews(std::string_view storeFamily) {
  return storeFamily != kStoreFamilyCrypto;
}

// "Does not renew. Pay again when it ends." under a plan paid once in crypto,
// until its term has ended and the renewal notice says the rest.
inline bool DoesNotRenewShows(std::string_view storeFamily, bool termNoticeShows) {
  return storeFamily == kStoreFamilyCrypto && !termNoticeShows;
}

// ---- the coins' marks (mmm/BTCPAY.md §17.6) ---------------------------------------
// A coin's mark is the bundled Assets\<icon key>.svg (App.vcxproj copies the
// four of More coins beside the exe), one path filled nonzero on a 24 x 24
// box, drawn as a PathIcon (CryptoPayViews.cpp CoinMark).

inline constexpr double kMarkViewBox = 24;

// An icon key that names a bundled file: lowercase letters, digits and
// underscores (the sdk's CryptoIconKey* values), never a path.
inline bool IsMarkKey(std::string_view key) {
  if (key.empty()) return false;
  for (const char c : key) {
    if (!(('a' <= c && c <= 'z') || ('0' <= c && c <= '9') || c == '_')) return false;
  }
  return true;
}

// The d attribute of an svg's one <path> element (comments skipped), or ""
// when it has none or several, or when the data holds anything but letters,
// digits, spaces, points, commas and signs: only path data ever reaches the
// PathIcon's markup.
inline std::string SvgPathData(std::string_view svg) {
  const auto startsAt = [&](size_t pos, std::string_view what) {
    return pos <= svg.size() && svg.substr(pos, what.size()) == what;
  };
  const auto isSpace = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
  std::string_view data;
  int paths = 0;
  size_t pos = 0;
  while (pos < svg.size()) {
    if (startsAt(pos, "<!--")) {
      const size_t end = svg.find("-->", pos + 4);
      if (end == std::string_view::npos) return std::string();
      pos = end + 3;
      continue;
    }
    const bool element = startsAt(pos, "<path") && pos + 5 < svg.size() &&
                         (isSpace(svg[pos + 5]) || svg[pos + 5] == '/' || svg[pos + 5] == '>');
    if (!element) {
      ++pos;
      continue;
    }
    const size_t close = svg.find('>', pos + 5);
    if (close == std::string_view::npos) return std::string();
    ++paths;
    const std::string_view tag = svg.substr(pos + 5, close - (pos + 5));
    for (size_t at = 0; at + 4 <= tag.size(); ++at) {
      if (!isSpace(tag[at]) || tag.substr(at + 1, 3) != "d=\"") continue;
      const size_t end = tag.find('"', at + 4);
      if (end == std::string_view::npos) return std::string();
      data = tag.substr(at + 4, end - (at + 4));
      break;
    }
    pos = close + 1;
  }
  if (paths != 1 || data.empty()) return std::string();
  for (const char c : data) {
    const bool allowed = ('0' <= c && c <= '9') || ('A' <= c && c <= 'Z') || ('a' <= c && c <= 'z') ||
                         c == ' ' || c == '.' || c == ',' || c == '-' || c == '+';
    if (!allowed) return std::string();
  }
  return std::string(data);
}

}  // namespace urnw::cryptopay
