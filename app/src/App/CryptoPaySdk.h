// The sdk's crypto-pay surface (crypto-pay CONTRACT §4.2-4.5, mmm/BTCPAY.md
// §17.6), read into this app's views (CryptoPayPresentation.h). The one place
// that names the fields of urnet::CryptoInvoiceState, of urnet::CryptoPaymentTab
// and of SubscriptionBalanceResult's crypto_assets / crypto_term, and the one
// place that calls the sdk's pure crypto-pay rules: urnet::cryptoPaymentTabs,
// urnet::cryptoPaymentTabId, urnet::cryptoAssetLabelKey, urnet::cryptoAssetLabel,
// urnet::cryptoAssetMarket, urnet::paymentMethodAllowsPlan,
// urnet::checkCryptoPaymentUri, urnet::cryptoWalletSuggestions and
// urnet::cryptoTermNoticeDue. Everything else in the app reads the views, so a
// change in the generated wrapper is a change here alone.
//
// Header-only and WinRT-free. It needs the generated urnetwork_sdk.hpp (and
// nlohmann/json), so tools/crypto-pay-tests.cpp builds it against a header
// that has the crypto-pay surface, wherever one is (the vendored
// app/third_party/urnetwork-sdk/<arch>, URNETWORK_SDK_INCLUDE, or the sdk
// checkout beside this repository): that pins every field name and type read
// below to the wrapper. Nothing here calls the C ABI unless the app calls it,
// so the spec links without the sdk library.
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

// The controller's state (sdk crypto_invoice_view_controller.go
// CryptoInvoiceState, json-tagged, so the wrapper's members are the json
// names), field for field.
inline CryptoInvoiceView ViewFromSdk(const urnet::CryptoInvoiceState& state) {
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
  view.refreshing = state.refreshing;
  view.errorCode = state.error_code;
  view.errorMessage = state.error_message;
  return view;
}

// SubscriptionBalanceResult.crypto_term (§2.2); none from an older server.
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

// The sdk's selector (urnet::CryptoPaymentTab), tab for tab and coin for coin.
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

// The method row's coin selector on a plan surface: urnet::cryptoPaymentTabs
// over the balance's coins, no list of coins that cannot be quoted (the
// balance names only the coins it can sell now), and the plan a coin buys
// there. USDC on Solana, Zcash, Monero, then More coins with TAO, Bitcoin, ETH
// and SOL; each coin says whether it can be paid with now. Empty when no coin
// sells the plan, when the server lists none, or when the sdk refuses: the row
// hides, and the surface shows Card alone.
inline std::vector<PaymentTabView> PaymentTabsFor(const urnet::CryptoPaymentAssetList& assets,
                                                  bool offerActive) {
  if (assets.empty()) return {};
  try {
    const urnet::StringList items = PlanSurfaceItems(offerActive);
    if (const std::optional<urnet::CryptoPaymentTabList> tabs =
            urnet::cryptoPaymentTabs(assets, urnet::CryptoPaymentAssetList{}, items)) {
      return TabsFromSdk(*tabs);
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

// CryptoUriAllowed with the sdk's own check: whether this view's uri may be
// drawn as a QR and opened.
inline bool UriAllowed(const CryptoInvoiceView& view) {
  return CryptoUriAllowed(view.asset, view.uri, view.address,
                          [](const std::string& asset, const std::string& uri,
                             const std::string& address) {
                            return urnet::checkCryptoPaymentUri(asset, uri, address);
                          });
}

// "Wallets that work" for an asset (urnet::cryptoWalletSuggestions): none for
// Bitcoin and ETH, which most wallets send, and the line goes.
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
