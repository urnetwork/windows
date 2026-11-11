// The sdk's crypto-pay surface (crypto-pay CONTRACT §4.2-4.5, mmm/BTCPAY.md
// §17.6), read into this app's views (CryptoPayPresentation.h). The one place
// that names the fields of urnet::CryptoInvoiceState (its steps among them), of
// urnet::CryptoPaymentTab, of urnet::CryptoAssetLines and of
// SubscriptionBalanceResult's crypto_assets / crypto_unavailable_assets /
// crypto_term, and the one place that calls the sdk's pure crypto-pay rules:
// urnet::cryptoPaymentTabs, urnet::cryptoPaymentTabId,
// urnet::cryptoPaymentTabCoin, urnet::cryptoAssetLabelKey,
// urnet::cryptoAssetLabel, urnet::cryptoAssetLinesFor,
// urnet::cryptoAssetMarket, urnet::cryptoPaymentEventProduct,
// urnet::paymentMethodAllowsPlan, urnet::cryptoPaymentUriScheme,
// urnet::checkCryptoPaymentUri, urnet::cryptoPaymentShowsMemo,
// urnet::cryptoWalletSuggestions and urnet::cryptoTermNoticeDue. Everything
// else in the app reads the views, so a change in the generated wrapper is a
// change here alone.
//
// Header-only and WinRT-free. It needs the generated urnetwork_sdk.hpp (and
// nlohmann/json), so tools/crypto-pay-tests.cpp builds it against a header
// that has the crypto-pay surface, wherever one is (the vendored
// app/third_party/urnetwork-sdk/<arch>, URNETWORK_SDK_INCLUDE, or the sdk
// checkout beside this repository): that pins every field name and type read
// below to the wrapper. The spec runs the readers that call nothing (the
// *FromSdk and *ToSdk functions); nothing here calls the C ABI unless the app
// calls it, so the spec links without the sdk library.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <exception>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <urnetwork_sdk.hpp>

#include "CryptoPayPresentation.h"

namespace urnw::cryptopay {

// The state's status steps (urnet::CryptoInvoiceState.steps, each a
// urnet::CryptoPaymentStep), step for step.
inline std::vector<PaymentStepView> StepsFromSdk(
    const std::optional<urnet::CryptoPaymentStepList>& sdkSteps) {
  std::vector<PaymentStepView> steps;
  if (!sdkSteps) return steps;
  for (const urnet::CryptoPaymentStep& sdkStep : *sdkSteps) {
    PaymentStepView step;
    step.status = sdkStep.status;
    step.progress = sdkStep.progress;
    step.confirmations = sdkStep.confirmations;
    step.confirmationsRequired = sdkStep.confirmations_required;
    steps.push_back(std::move(step));
  }
  return steps;
}

// The controller's state (sdk crypto_invoice_view_controller.go
// CryptoInvoiceState, json-tagged, so the wrapper's members are the json
// names), field for field, its status steps among them.
inline CryptoInvoiceView StateFromSdk(const urnet::CryptoInvoiceState& state) {
  CryptoInvoiceView view;
  view.phase = state.phase;
  view.intentId = state.intent_id;
  view.asset = state.asset;
  view.plan = state.plan;
  view.itemId = state.item_id;
  view.address = state.address;
  view.amount = state.amount;
  view.ticker = state.ticker;
  view.uri = state.uri;
  view.qrText = state.qr_text;
  view.memo = state.memo;
  view.priceUsd = state.price_usd;
  view.expiresAt = state.expires_at;
  view.secondsLeft = state.seconds_left;
  view.status = state.status;
  view.reviewReason = state.review_reason;
  view.received = state.received;
  view.remaining = state.remaining;
  view.confirmations = state.confirmations;
  view.confirmationsRequired = state.confirmations_required;
  view.steps = StepsFromSdk(state.steps);
  view.refreshing = state.refreshing;
  view.errorCode = state.error_code;
  view.errorMessage = state.error_message;
  return view;
}

// A coin's lines (urnet::CryptoAssetLines), each its key with its English.
inline CoinLines LinesFromSdk(const urnet::CryptoAssetLines& sdkLines) {
  CoinLines lines;
  lines.networkLine = CoinLine{sdkLines.network_line_key, sdkLines.network_line};
  lines.networkWarning = CoinLine{sdkLines.network_warning_key, sdkLines.network_warning};
  lines.noPaymentLinkNote =
      CoinLine{sdkLines.no_payment_link_note_key, sdkLines.no_payment_link_note};
  return lines;
}

// A coin's lines (urnet::cryptoAssetLinesFor): the network it is sent on, the
// warning beside its address, and TAO's note under its address-only QR. None
// for anything else, and none when the sdk refuses.
inline CoinLines LinesFor(const std::string& asset) {
  if (!IsCoin(asset)) return CoinLines{};
  try {
    if (const std::optional<urnet::CryptoAssetLines> lines = urnet::cryptoAssetLinesFor(asset)) {
      return LinesFromSdk(*lines);
    }
  } catch (const std::exception&) {
  }
  return CoinLines{};
}

// A coin with no payment link (urnet::cryptoPaymentUriScheme is empty: TAO):
// its invoice is paid by hand, the address and the exact amount, and its QR
// holds the address alone. False for anything else, and when the sdk refuses.
inline bool NoPaymentLink(const std::string& asset) {
  if (!IsCoin(asset)) return false;
  try {
    return urnet::cryptoPaymentUriScheme(asset).empty();
  } catch (const std::exception&) {
    return false;
  }
}

// Whether the invoice shows its memo, with Copy (urnet::cryptoPaymentShowsMemo):
// only when the server sent one, which it does for USDC on Solana alone.
inline bool ShowsMemo(const std::string& asset, const std::string& memo) {
  try {
    return urnet::cryptoPaymentShowsMemo(asset, memo);
  } catch (const std::exception&) {
    return false;
  }
}

// The invoice as the surfaces read it: the controller's state (StateFromSdk)
// with the sdk's answers for its coin beside it, its lines (LinesFor), whether
// it has no payment link (NoPaymentLink) and whether its memo shows
// (ShowsMemo).
inline CryptoInvoiceView ViewFromSdk(const urnet::CryptoInvoiceState& state) {
  CryptoInvoiceView view = StateFromSdk(state);
  view.lines = LinesFor(view.asset);
  view.noPaymentLink = NoPaymentLink(view.asset);
  view.showsMemo = ShowsMemo(view.asset, view.memo);
  return view;
}

// SubscriptionBalanceResult.crypto_term (§2.2); none from an older server.
// (Its asset is not read: CryptoTermView.)
inline CryptoTermView TermFromSdk(const urnet::SubscriptionBalanceResult& result) {
  CryptoTermView view;
  if (!result.crypto_term) return view;
  view.market = result.crypto_term->market;
  view.termEndTime = result.crypto_term->term_end_time;
  view.graceEndTime = result.crypto_term->grace_end_time;
  return view;
}

// SubscriptionBalanceResult.crypto_assets (§2.2): the coins the server sells
// now; empty from an older server and with every coin switched off.
inline urnet::CryptoPaymentAssetList AssetsFromSdk(const urnet::SubscriptionBalanceResult& result) {
  return result.crypto_assets.value_or(urnet::CryptoPaymentAssetList{});
}

// SubscriptionBalanceResult.crypto_unavailable_assets: the coins that are on
// but cannot be quoted now (a wallet scanner lagging, a coin with no address
// yet), which the selector shows greyed, "Temporarily unavailable", instead of
// leaving out. Empty from a server before 2026-11-10, and while every coin
// that is on can be quoted.
inline urnet::CryptoPaymentAssetList UnavailableAssetsFromSdk(
    const urnet::SubscriptionBalanceResult& result) {
  return result.crypto_unavailable_assets.value_or(urnet::CryptoPaymentAssetList{});
}

// The sdk's selector (urnet::CryptoPaymentTab), tab for tab and coin for coin.
// The coins' network lines are the sdk's too, added by PaymentTabsFor.
inline std::vector<PaymentTabView> TabsFromSdk(const urnet::CryptoPaymentTabList& sdkTabs) {
  std::vector<PaymentTabView> tabs;
  for (const urnet::CryptoPaymentTab& sdkTab : sdkTabs) {
    PaymentTabView tab;
    tab.id = sdkTab.id;
    tab.label = sdkTab.label;
    tab.labelKey = sdkTab.label_key;
    if (sdkTab.icon_keys) tab.iconKeys = *sdkTab.icon_keys;
    if (sdkTab.coins) {
      for (const urnet::CryptoPaymentCoin& sdkCoin : *sdkTab.coins) {
        PaymentCoinView coin;
        coin.asset = sdkCoin.asset;
        coin.label = sdkCoin.label;
        coin.labelKey = sdkCoin.label_key;
        coin.iconKey = sdkCoin.icon_key;
        coin.available = sdkCoin.available;
        tab.coins.push_back(std::move(coin));
      }
    }
    tab.available = sdkTab.available;
    tabs.push_back(std::move(tab));
  }
  return tabs;
}

// A tab the other way, as the sdk lays it out (urnet::CryptoPaymentTab): what
// urnet::cryptoPaymentTabCoin reads. The coins' network lines are no part of
// the sdk's tab and stay behind.
inline urnet::CryptoPaymentTab TabToSdk(const PaymentTabView& tab) {
  urnet::CryptoPaymentTab sdkTab;
  sdkTab.id = tab.id;
  sdkTab.label = tab.label;
  sdkTab.label_key = tab.labelKey;
  sdkTab.icon_keys = tab.iconKeys;
  urnet::CryptoPaymentCoinList sdkCoins;
  for (const PaymentCoinView& coin : tab.coins) {
    urnet::CryptoPaymentCoin sdkCoin;
    sdkCoin.asset = coin.asset;
    sdkCoin.label = coin.label;
    sdkCoin.label_key = coin.labelKey;
    sdkCoin.icon_key = coin.iconKey;
    sdkCoin.available = coin.available;
    sdkCoins.push_back(std::move(sdkCoin));
  }
  sdkTab.coins = std::move(sdkCoins);
  sdkTab.available = tab.available;
  return sdkTab;
}

// The method row's coin selector on a plan surface: urnet::cryptoPaymentTabs
// over the balance's two lists, the coins it sells now and the coins that are
// on but cannot be quoted now (shown greyed, "Temporarily unavailable",
// instead of left out), and the plan a coin buys there; each coin with its
// network line (LinesFor). USDC on Solana, Zcash, Monero, then More coins with
// TAO, Bitcoin, ETH and SOL; each coin says whether it can be paid with now.
// Whether there is a selector at all is the sdk's to say: none when no coin of
// either list sells the plan (an older server lists none), and none when the
// sdk refuses. Without one the row hides, and the surface shows Card alone.
inline std::vector<PaymentTabView> PaymentTabsFor(const urnet::CryptoPaymentAssetList& assets,
                                                  const urnet::CryptoPaymentAssetList& unavailableAssets,
                                                  bool offerActive) {
  try {
    const urnet::StringList items = PlanSurfaceItems(offerActive);
    if (const std::optional<urnet::CryptoPaymentTabList> sdkTabs =
            urnet::cryptoPaymentTabs(assets, unavailableAssets, items)) {
      std::vector<PaymentTabView> tabs = TabsFromSdk(*sdkTabs);
      for (PaymentTabView& tab : tabs) {
        for (PaymentCoinView& coin : tab.coins) coin.networkLine = LinesFor(coin.asset).networkLine;
      }
      return tabs;
    }
  } catch (const std::exception&) {
    // a malformed list: Card alone, as an older server
  }
  return {};
}

// The tab a method sits in (urnet::cryptoPaymentTabId): the asset itself for
// USDC on Solana, Zcash and Monero, kTabMoreCoins for TAO, Bitcoin, ETH and
// SOL, "" for Card. The remembered method selects this tab.
inline std::string TabIdFor(const std::string& method) {
  if (!IsCoin(method)) return std::string();
  try {
    return urnet::cryptoPaymentTabId(method);
  } catch (const std::exception&) {
    return std::string();
  }
}

// The coin a tap on `tab` chooses (urnet::cryptoPaymentTabCoin): `lastChosen`,
// the coin chosen last in that tab (LastChosenCoins), while it can be paid
// with now; else `remembered`, the method remembered on this device, while it
// is one of the tab's coins that can; else the tab's first that can. "" when
// none can (the tab cannot be chosen), for a coin this app does not take, and
// when the sdk refuses.
inline std::string TabCoinFor(const PaymentTabView& tab, const std::string& lastChosen,
                              const std::string& remembered) {
  try {
    const std::string coin = urnet::cryptoPaymentTabCoin(TabToSdk(tab), lastChosen, remembered);
    return IsCoin(coin) ? coin : std::string();
  } catch (const std::exception&) {
    return std::string();
  }
}

// A method's label (CONTRACT §5): the store key, and the English it falls back
// to while the catalog lacks the key.
struct MethodLabelText {
  std::string key;
  std::string english;
};

// Card's own, or a coin's from the sdk (urnet::cryptoAssetLabelKey and
// urnet::cryptoAssetLabel: "USDC on Solana", "Zcash", "Monero", "TAO",
// "Bitcoin", "ETH", "SOL"), for "Pay with {method}" and the invoice's lines.
inline MethodLabelText MethodLabel(const std::string& method) {
  if (!IsCoin(method)) return MethodLabelText{"pay_method_card", "Card"};
  try {
    return MethodLabelText{urnet::cryptoAssetLabelKey(method), urnet::cryptoAssetLabel(method)};
  } catch (const std::exception&) {
    return MethodLabelText{std::string(), method};
  }
}

// The store a method's offer and purchase events name (§6.1 Events): a coin's
// market (urnet::cryptoAssetMarket: solana for USDC and SOL, zcash, monero,
// bittensor, bitcoin, ethereum), stripe for Card.
inline std::string MethodEventStore(const std::string& method) {
  if (!IsCoin(method)) return urnet::EventStoreStripe;
  try {
    return urnet::cryptoAssetMarket(method);
  } catch (const std::exception&) {
    return std::string();
  }
}

// The product a coin's purchase events name (urnet::cryptoPaymentEventProduct:
// the year of Pro by the coin's ticker, pro_yearly_usdc to pro_yearly_sol, the
// welcome offer's year included): USDC and SOL share the solana store, and the
// product tells them apart. "" for Card, whose product is its plan's
// (PlanProduct), and when the sdk refuses.
inline std::string MethodEventProduct(const std::string& method) {
  if (!IsCoin(method)) return std::string();
  try {
    return urnet::cryptoPaymentEventProduct(method);
  } catch (const std::exception&) {
    return std::string();
  }
}

// Picking a coin greys out monthly ("Monthly is card only") and moves the
// selection to yearly: no coin sells monthly (urnet::paymentMethodAllowsPlan).
inline bool MonthlyLocked(const std::string& method) {
  if (!IsCoin(method)) return false;
  try {
    return !urnet::paymentMethodAllowsPlan(method, kPlanMonthly);
  } catch (const std::exception&) {
    return true;
  }
}

// CryptoUriAllowed with the sdk's own check, the whole gate: whether this
// view's uri may be drawn as a QR and opened.
inline bool UriAllowed(const CryptoInvoiceView& view) {
  return CryptoUriAllowed(view.asset, view.uri, view.address,
                          [](const std::string& asset, const std::string& uri,
                             const std::string& address) {
                            return urnet::checkCryptoPaymentUri(asset, uri, address);
                          });
}

// "Wallets that work" for an asset (urnet::cryptoWalletSuggestions), the sdk's
// names in its order, untranslated: every coin has its list, Bitcoin's and
// ETH's among them; with none the line goes.
inline std::vector<std::string> WalletSuggestions(const std::string& asset) {
  try {
    if (const std::optional<urnet::StringList> names = urnet::cryptoWalletSuggestions(asset)) {
      return *names;
    }
  } catch (const std::exception&) {
  }
  return {};
}

// The renewal notice is due (urnet::cryptoTermNoticeDue): from the paid
// term's end until its grace ends, never before.
inline bool TermNoticeDue(const CryptoTermView& term) {
  if (term.termEndTime.empty() || term.graceEndTime.empty()) return false;
  urnet::CryptoTerm sdkTerm;
  sdkTerm.market = term.market;
  sdkTerm.term_end_time = term.termEndTime;
  sdkTerm.grace_end_time = term.graceEndTime;
  try {
    return urnet::cryptoTermNoticeDue(sdkTerm);
  } catch (const std::exception&) {
    return false;
  }
}

}  // namespace urnw::cryptopay
