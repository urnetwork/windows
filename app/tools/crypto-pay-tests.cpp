// Executable spec for the crypto payment rules (App/CryptoPayPresentation.h,
// crypto-pay CONTRACT §4.3, §4.4, §6, mmm/BTCPAY.md §17.6): the payment-method
// row over the sdk's coin selector (when it shows, the remembered method and
// its fallback to Card, the coin chosen last in each tab that a tap hands the
// sdk, the plan a coin buys), the marks' svg reading, the invoice's display
// for every state of the sdk's controller and for every coin, with the sdk's
// answers for the coin as the view carries them (its lines, its payment link,
// its memo, its steps), the quote countdown, the gate in front of every
// payment uri, the QR's layout for real payment uris, the "Payment in
// progress" row, and a crypto year's plan rows (the notice's date, "Does not
// renew"). The rules the sdk owns (urnet::cryptoPaymentTabs,
// cryptoPaymentTabCoin, paymentMethodAllowsPlan, checkCryptoPaymentUri,
// cryptoPaymentQrText, cryptoAssetLinesFor, cryptoPaymentShowsMemo,
// cryptoPaymentSteps, cryptoPaymentEventProduct, cryptoTermNoticeDue, the
// labels and the event stores) are the sdk's own tests', which replay the
// shared vectors; the gate's use of the uri check is pinned here with a fake.
//
//   c++ -std=c++20 -Wall -Wextra -Werror -I ../src/App -I ../third_party/qrcodegen crypto-pay-tests.cpp ../third_party/qrcodegen/qrcodegen.cpp -o /tmp/crypto-pay-tests && /tmp/crypto-pay-tests
//
// With URNW_CRYPTO_PAY_TESTS_SDK it also builds App/CryptoPaySdk.h against the
// generated header and reads the sdk's own structs through it, which pins the
// field names and types the app reads to the wrapper. The header needs
// nlohmann/json; both are system includes because the generated code does not
// build with -Wextra -Werror:
//
//   ... -DURNW_CRYPTO_PAY_TESTS_SDK -isystem <dir of urnetwork_sdk.hpp> -isystem <dir of nlohmann/> ...
//
// SPDX-License-Identifier: MPL-2.0

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <qrcodegen.hpp>

#include "CryptoPayPresentation.h"

#if defined(URNW_CRYPTO_PAY_TESTS_SDK)
#include "CryptoPaySdk.h"
#endif

using namespace urnw::cryptopay;

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

void CheckEq(const std::string& want, const std::string& got, const std::string& what) {
  Check(want == got, what + ": want \"" + want + "\", got \"" + got + "\"");
}

void CheckEq(int64_t want, int64_t got, const std::string& what) {
  Check(want == got, what + ": want " + std::to_string(want) + ", got " + std::to_string(got));
}

std::string Join(const std::vector<std::string>& values) {
  std::string joined;
  for (const std::string& value : values) joined += (joined.empty() ? "" : ",") + value;
  return joined;
}

// A status step as the sdk hands it over (urnet::CryptoPaymentStep).
PaymentStepView MakeStep(const char* status, const char* progress, int64_t confirmations = 0,
                         int64_t confirmationsRequired = 0) {
  PaymentStepView step;
  step.status = status;
  step.progress = progress;
  step.confirmations = confirmations;
  step.confirmationsRequired = confirmationsRequired;
  return step;
}

// "pending=done,confirming=current 3/6,paid=upcoming": each step's status, how
// it is drawn, and the confirming step's counts.
std::string Steps(const std::vector<PaymentStepView>& steps) {
  std::string joined;
  for (const PaymentStepView& step : steps) {
    joined += (joined.empty() ? "" : ",") + step.status + "=" + step.progress;
    if (step.status == kStatusConfirming) {
      joined += " " + std::to_string(step.confirmations) + "/" + std::to_string(step.confirmationsRequired);
    }
  }
  return joined;
}

std::string Quote(QuoteLine line) {
  switch (line) {
    case QuoteLine::Hidden: return "hidden";
    case QuoteLine::Counting: return "counting";
    case QuoteLine::Expired: return "expired";
  }
  return "?";
}

std::string Route(InvoiceRoute route) {
  switch (route) {
    case InvoiceRoute::None: return "none";
    case InvoiceRoute::Starting: return "starting";
    case InvoiceRoute::Open: return "open";
    case InvoiceRoute::Paid: return "paid";
    case InvoiceRoute::Unavailable: return "unavailable";
    case InvoiceRoute::Refused: return "refused";
    case InvoiceRoute::Unknown: return "unknown";
  }
  return "?";
}

// a fake of urnet::checkCryptoPaymentUri that records what it was asked
struct FakeCheck {
  bool answer = true;
  bool throws = false;
  int calls = 0;
  std::string asset, uri, address;
  bool operator()(const std::string& a, const std::string& u, const std::string& d) {
    ++calls;
    asset = a;
    uri = u;
    address = d;
    if (throws) throw std::runtime_error("urnet: the check failed");
    return answer;
  }
};

// A text of `count` characters from a base58-looking alphabet: the shape of a
// real address, not one (the spec never names a real receive address).
std::string Fill(char first, size_t count) {
  static const std::string alphabet = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
  std::string text(1, first);
  for (size_t at = 1; at < count; ++at) text += alphabet[(at * 7 + 3) % alphabet.size()];
  return text;
}

std::string Bech32Fill(const std::string& prefix, size_t count) {
  static const std::string alphabet = "qpzry9x8gf2tvdw0s3jn54khce6mua7l";
  std::string text = prefix;
  for (size_t at = prefix.size(); at < count; ++at) text += alphabet[(at * 11 + 5) % alphabet.size()];
  return text;
}

// The invoice of a Monero yearly plan, open and waiting, with the sdk's
// answers for Monero beside it as CryptoPaySdk.h ViewFromSdk carries them: its
// network line (no warning, no payment-link note), a payment link, no memo,
// and its four steps at pending.
CryptoInvoiceView MoneroInvoice() {
  CryptoInvoiceView view;
  view.phase = kPhaseOpen;
  view.intentId = "0b6f";
  view.asset = kAssetXmr;
  view.plan = kPlanYearly;
  view.address = "8sample";
  view.amount = "0.072387";
  view.ticker = "XMR";
  view.uri = "monero:8sample?tx_amount=0.072387&tx_description=URnetwork%20Pro%20yearly";
  view.qrText = view.uri;  // urnet::cryptoPaymentQrText: the uri its check passed
  view.priceUsd = 40;
  view.expiresAt = "2026-10-20T18:30:00Z";
  view.secondsLeft = 1799;
  view.status = kStatusPending;
  view.remaining = "0.072387";
  view.confirmationsRequired = 1;
  view.steps = {MakeStep(kStatusPending, kStepCurrent), MakeStep(kStatusSeen, kStepUpcoming),
                MakeStep(kStatusConfirming, kStepUpcoming, 0, 1), MakeStep(kStatusPaid, kStepUpcoming)};
  view.lines.networkLine = {"crypto_network_monero", "Monero"};
  return view;
}

// A coin of the selector as the sdk lays it out.
PaymentCoinView MakeCoin(const char* asset, const char* label, const char* labelKey, const char* iconKey,
                         bool available) {
  PaymentCoinView coin;
  coin.asset = asset;
  coin.label = label;
  coin.labelKey = labelKey;
  coin.iconKey = iconKey;
  coin.available = available;
  return coin;
}

// The sdk's selector (urnet::cryptoPaymentTabs) for a balance that sells
// `listed`, with no unavailable list: none at all without a coin; else the
// three main tabs always, and More coins while one of its coins is listed,
// each coin available while it is.
std::vector<PaymentTabView> Tabs(const std::vector<std::string>& listed) {
  std::vector<PaymentTabView> tabs;
  if (listed.empty()) return tabs;
  const auto has = [&listed](const char* asset) {
    return std::find(listed.begin(), listed.end(), asset) != listed.end();
  };
  const auto single = [&](const char* asset, const char* label, const char* labelKey, const char* iconKey) {
    PaymentTabView tab;
    tab.id = asset;
    tab.label = label;
    tab.labelKey = labelKey;
    tab.iconKeys = {iconKey};
    tab.coins = {MakeCoin(asset, label, labelKey, iconKey, has(asset))};
    tab.available = has(asset);
    tabs.push_back(tab);
  };
  single(kAssetUsdcSolana, "USDC on Solana", "pay_method_usdc_solana", "coin_usdc");
  single(kAssetZec, "Zcash", "pay_method_zcash", "coin_zec");
  single(kAssetXmr, "Monero", "pay_method_monero", "coin_xmr");
  if (has(kAssetTao) || has(kAssetBtc) || has(kAssetEth) || has(kAssetSol)) {
    PaymentTabView more;
    more.id = kTabMoreCoins;
    more.label = "More coins";
    more.labelKey = "pay_method_more_coins";
    more.iconKeys = {"coin_tao", "coin_btc", "coin_eth", "coin_sol"};
    more.coins = {MakeCoin(kAssetTao, "TAO", "pay_method_tao", "coin_tao", has(kAssetTao)),
                  MakeCoin(kAssetBtc, "Bitcoin", "pay_method_bitcoin", "coin_btc", has(kAssetBtc)),
                  MakeCoin(kAssetEth, "ETH", "pay_method_eth", "coin_eth", has(kAssetEth)),
                  MakeCoin(kAssetSol, "SOL", "pay_method_sol", "coin_sol", has(kAssetSol))};
    more.available = true;
    tabs.push_back(more);
  }
  return tabs;
}

void MethodRow() {
  const std::vector<PaymentTabView> none = Tabs({});
  const std::vector<PaymentTabView> monero = Tabs({kAssetXmr});
  const std::vector<PaymentTabView> all =
      Tabs({kAssetUsdcSolana, kAssetZec, kAssetXmr, kAssetTao, kAssetBtc, kAssetEth, kAssetSol});
  // Alchemy's credentials missing: Bitcoin and ETH cannot be quoted
  const std::vector<PaymentTabView> taoAndSol = Tabs({kAssetTao, kAssetSol});

  Check(!MethodRowShows(none), "no tabs (no coin sells the item): no row, the surface as before");
  Check(MethodRowShows(monero), "a listed coin shows the row");
  CheckEq(3, static_cast<int64_t>(monero.size()), "the main three tabs while no More coins coin is listed");
  CheckEq(4, static_cast<int64_t>(all.size()), "and More coins while one of its coins is");

  Check(IsCoin(kAssetUsdcSolana) && IsCoin(kAssetZec) && IsCoin(kAssetXmr), "the main three coins");
  Check(IsCoin(kAssetTao) && IsCoin(kAssetBtc) && IsCoin(kAssetEth) && IsCoin(kAssetSol), "More coins' four");
  Check(!IsCoin(kMethodCard) && !IsCoin("") && !IsCoin("XMR") && !IsCoin("doge") && !IsCoin(kTabMoreCoins),
        "Card, nothing, a ticker, a coin this app does not know and a tab are no coin");

  // the remembered method while it can be paid with now, else Card
  CheckEq(kAssetXmr, MethodToSelect(monero, kAssetXmr), "a remembered coin that is listed");
  CheckEq(kMethodCard, MethodToSelect(monero, kAssetZec), "a remembered coin the server does not list (greyed)");
  CheckEq(kMethodCard, MethodToSelect(none, kAssetXmr), "a remembered coin with every coin off");
  CheckEq(kMethodCard, MethodToSelect(monero, ""), "nothing remembered");
  CheckEq(kMethodCard, MethodToSelect(monero, kMethodCard), "Card remembered");
  CheckEq(kMethodCard, MethodToSelect(all, "doge"), "a remembered method this app does not know");
  CheckEq(kAssetUsdcSolana, MethodToSelect(all, kAssetUsdcSolana), "USDC remembered and listed");
  CheckEq(kAssetSol, MethodToSelect(taoAndSol, kAssetSol), "a More coins coin remembered and listed");
  CheckEq(kMethodCard, MethodToSelect(taoAndSol, kAssetBtc), "Bitcoin remembered while it cannot be quoted");
  CheckEq(kMethodCard, MethodToSelect(all, kTabMoreCoins), "a tab is never a method");
  Check(MethodOffered(none, kMethodCard), "Card is always offered");
  Check(!MethodOffered(taoAndSol, kAssetEth) && MethodOffered(taoAndSol, kAssetTao),
        "a greyed coin is not offered, an available one is");

  // the order: USDC on Solana, Zcash, Monero, More coins with TAO, Bitcoin, ETH, SOL
  std::vector<std::string> ids;
  std::vector<std::string> assets;
  for (const PaymentTabView& tab : all) {
    ids.push_back(tab.id);
    for (const PaymentCoinView& coin : tab.coins) assets.push_back(coin.asset);
  }
  CheckEq("usdc_solana,zec,xmr,more_coins", Join(ids), "the tabs in order");
  CheckEq("usdc_solana,zec,xmr,tao,btc,eth,sol", Join(assets), "the coins in order");
  CheckEq("coin_tao,coin_btc,coin_eth,coin_sol", Join(all[3].iconKeys), "More coins' four marks, each its own");
  const PaymentCoinView* eth = FindCoin(all, kAssetEth);
  Check(eth != nullptr && eth->labelKey == "pay_method_eth", "a coin found by its asset");
  Check(FindCoin(all, "doge") == nullptr, "and no coin the selector does not lay out");

  // a tab chosen: the sdk's coin for the tap (urnet::cryptoPaymentTabCoin,
  // CryptoPaySdk.h TabCoinFor), asked with the coin chosen last in that tab
  const PaymentTabView& more = taoAndSol[3];
  CheckEq(kTabMoreCoins, more.id, "More coins is the fourth tab");
  Check(TabOpensCoins(more) && !TabOpensCoins(taoAndSol[0]), "only More coins opens a list");
  {
    const std::vector<PaymentTabView> solOnly = Tabs({kAssetSol});
    Check(!solOnly[0].available && solOnly[3].available, "USDC greyed, More coins not");
  }

  // the coin chosen last in each tab, which a tap on it hands the sdk: kept
  // per tab, so a choice in another tab never replaces it (one "chosen last
  // anywhere" sent USDC to More coins, which then fell back to TAO)
  {
    LastChosenCoins chosen;
    CheckEq("", chosen.In(kTabMoreCoins), "nothing chosen in More coins yet");
    chosen.Choose(all, kAssetSol);
    CheckEq(kAssetSol, chosen.In(kTabMoreCoins), "SOL, chosen in the More coins list");
    chosen.Choose(all, kAssetUsdcSolana);
    CheckEq(kAssetUsdcSolana, chosen.In(kAssetUsdcSolana), "then USDC on Solana, chosen in its own tab");
    CheckEq(kAssetSol, chosen.In(kTabMoreCoins), "More coins still has SOL: USDC's tab is another");
    chosen.Choose(all, kMethodCard);
    chosen.Choose(all, "");
    chosen.Choose(all, "doge");
    chosen.Choose(all, kTabMoreCoins);
    chosen.Choose(all, "XMR");
    CheckEq(2, static_cast<int64_t>(chosen.byTab.size()),
            "Card, nothing, a coin no tab holds, a tab and a ticker change nothing");
    CheckEq(kAssetSol, chosen.In(kTabMoreCoins), "and More coins keeps SOL through them");
    chosen.Choose(all, kAssetBtc);
    CheckEq(kAssetBtc, chosen.In(kTabMoreCoins), "a later choice in More coins replaces SOL");
    chosen.Choose(monero, kAssetEth);
    CheckEq(kAssetBtc, chosen.In(kTabMoreCoins), "a coin of a tab the row does not lay out changes nothing");
    chosen.Choose(monero, kAssetXmr);
    CheckEq(kAssetXmr, chosen.In(kAssetXmr), "a tab of one coin keeps its coin");
    CheckEq("", chosen.In(kAssetZec), "and a tab never chosen has none");
  }
  // the row rebuilds only for a selector that changed
  Check(Tabs({kAssetXmr}) == monero && !(taoAndSol == all), "the selector compares by value");

  // the plan a coin buys, and what the row asks the sdk about
  CheckEq(kPlanYearlyOnboarding, CryptoPlanFor(true), "an active welcome offer: the offer");
  CheckEq(kPlanYearly, CryptoPlanFor(false), "no offer: yearly");
  CheckEq("yearly_onboarding", Join(PlanSurfaceItems(true)), "the items with the offer");
  CheckEq("yearly", Join(PlanSurfaceItems(false)), "the items without the offer");

  // (a method's label and its events' store are the sdk's: CryptoPaySdk.h)
  Check(PurchaseHasTrial(kMethodCard, true), "the card's yearly plan has the trial");
  Check(!PurchaseHasTrial(kMethodCard, false), "the card's monthly plan has none");
  Check(!PurchaseHasTrial(kAssetXmr, true) && !PurchaseHasTrial(kAssetTao, true), "a coin's year has no trial");
}

void InvoiceWords() {
  // (each coin's network line, its warning, TAO's note and whether it has a
  // payment link are the sdk's: CryptoPaySdk.h, pinned below against the
  // generated header)

  // a status step's line, whatever coin it is for
  CheckEq("crypto_status_waiting", StepKey(kStatusPending), "pending: waiting for your payment");
  CheckEq("crypto_status_seen", StepKey(kStatusSeen), "seen: payment seen");
  CheckEq("crypto_status_confirming", StepKey(kStatusConfirming), "confirming n of N");
  CheckEq("crypto_status_paid", StepKey(kStatusPaid), "paid");
  CheckEq("", StepKey(kStatusExpired), "expired is no step");
  CheckEq("", StepKey(kStatusReview), "nor review");
  CheckEq("", StepKey(""), "nor nothing");

  // the step a screen reader hears when the payment moves on
  CheckEq(1, AnnouncedStep({MakeStep(kStatusPending, kStepDone), MakeStep(kStatusSeen, kStepCurrent),
                            MakeStep(kStatusConfirming, kStepUpcoming, 0, 6), MakeStep(kStatusPaid, kStepUpcoming)}),
          "the current step");
  CheckEq(2, AnnouncedStep({MakeStep(kStatusPending, kStepDone), MakeStep(kStatusSeen, kStepDone),
                            MakeStep(kStatusConfirming, kStepCurrent, 3, 6), MakeStep(kStatusPaid, kStepUpcoming)}),
          "confirming, the seen step skipped and drawn done");
  CheckEq(1, AnnouncedStep({MakeStep(kStatusPending, kStepDone), MakeStep(kStatusPaid, kStepDone)}),
          "paid, every step done: the last");
  CheckEq(-1, AnnouncedStep({}), "no steps: none");

  CheckEq("0:00", FormatCountdown(0), "zero");
  CheckEq("0:05", FormatCountdown(5), "seconds");
  CheckEq("1:05", FormatCountdown(65), "a minute");
  CheckEq("29:59", FormatCountdown(1799), "a fresh Monero quote");
  CheckEq("30:00", FormatCountdown(1800), "thirty minutes");
  CheckEq("60:00", FormatCountdown(3600), "a USDC quote's hour stays in minutes");
  CheckEq("0:00", FormatCountdown(-3), "never below zero");

  Check(!DecimalIsPositive(""), "nothing is not positive");
  Check(!DecimalIsPositive("0"), "zero");
  Check(!DecimalIsPositive("0.000000"), "zero with decimals");
  Check(DecimalIsPositive("0.000001"), "one micro");
  Check(DecimalIsPositive("12"), "a whole amount");
  Check(DecimalIsPositive(".5") && DecimalIsPositive("5."), "a bare point");
  Check(!DecimalIsPositive("-1"), "a sign is not this server's");
  Check(!DecimalIsPositive("1.2.3") && !DecimalIsPositive("1e3") && !DecimalIsPositive("0x1"),
        "anything but digits and one point");

  CheckEq("0.072387 XMR", AmountWithTicker("0.072387", "XMR"), "the amount with its ticker");
  CheckEq("0.072387", AmountWithTicker("0.072387", ""), "no ticker");
  CheckEq("", AmountWithTicker("", "XMR"), "no amount");
  CheckEq("btcli, Talisman, Nova", JoinNames({"btcli", "Talisman", "", "Nova"}), "the wallets");
  CheckEq("", JoinNames({}), "no wallets");
}

void UriGate() {
  const std::string address = "8sample";
  const std::string uri = "monero:8sample?tx_amount=0.072387&tx_description=URnetwork%20Pro%20yearly";
  {
    FakeCheck check;
    Check(CryptoUriAllowed(kAssetXmr, uri, address, check), "the sdk's check passes: drawn");
    CheckEq(1, check.calls, "the sdk is asked once");
    CheckEq(kAssetXmr, check.asset, "the sdk is asked about the asset");
    CheckEq(uri, check.uri, "the sdk is asked about the uri itself");
    CheckEq(address, check.address, "the sdk is asked about the invoice's address");
  }
  {
    FakeCheck check;
    check.answer = false;
    Check(!CryptoUriAllowed(kAssetXmr, uri, address, check), "the sdk's check refuses: never drawn");
  }
  {
    FakeCheck check;
    check.throws = true;
    Check(!CryptoUriAllowed(kAssetXmr, uri, address, check), "a check that throws refuses");
  }
  {
    // a scheme in capitals is the same scheme to the sdk (RFC 3986), and the
    // gate adds no judgement of its own
    FakeCheck check;
    Check(CryptoUriAllowed(kAssetXmr, "MONERO:8sample?tx_amount=1", address, check),
          "what the sdk's check passes, the gate passes");
    CheckEq("MONERO:8sample?tx_amount=1", check.uri, "the uri reaches the sdk as it is");
  }
  // the links of Bitcoin, ETH and native SOL (mmm/BTCPAY.md §17.7) reach the
  // sdk's check, which alone reads their parameters
  struct Link {
    std::string asset;
    std::string uri;
  };
  for (const Link& c : {
           Link{kAssetBtc, "bitcoin:bc1qsample?amount=0.00061234&label=URnetwork"},
           Link{kAssetEth, "ethereum:0xsample@1?value=12000000000000000"},
           Link{kAssetSol, "solana:Ssample?amount=0.251234&reference=Rsample&label=URnetwork"},
       }) {
    FakeCheck check;
    Check(CryptoUriAllowed(c.asset, c.uri, "sample", check), "the sdk is asked about " + c.asset + "'s link");
    CheckEq(1, check.calls, c.asset + ": asked once");
  }
  // The sdk's check is the whole gate (urnet::checkCryptoPaymentUri refuses an
  // empty field, another coin's scheme, a byte outside printable ASCII, a
  // fragment, and every uri of TAO): this app keeps no scheme list or
  // pre-check of its own. Each of these reaches the check as it is, and the
  // check's refusal (the fake's here) stands.
  struct Refused {
    const char* why;
    std::string asset;
    std::string uri;
    std::string address;
  };
  for (const Refused& c : {
           Refused{"another asset's scheme", kAssetXmr, "zcash:u1sample?amount=1", address},
           Refused{"Monero's scheme on a Zcash invoice", kAssetZec, uri, "u1sample"},
           Refused{"a web link", kAssetUsdcSolana, "https://pay.example/solana", address},
           Refused{"no uri", kAssetXmr, "", address},
           Refused{"the scheme alone", kAssetXmr, "monero:", address},
           Refused{"no address", kAssetXmr, uri, ""},
           Refused{"no asset", "", uri, address},
           Refused{"an asset this app does not know", "doge", "doge:Dsample", address},
           Refused{"TAO, which has no payment link", kAssetTao, "tao:5sample?amount=1", "5sample"},
           Refused{"Bitcoin's scheme on an ETH invoice", kAssetEth, "bitcoin:0xsample?amount=1", "0xsample"},
           Refused{"ETH's scheme on a native SOL invoice", kAssetSol, "ethereum:Ssample@1", "Ssample"},
           Refused{"a space", kAssetXmr, "monero:8sample?tx_description=URnetwork Pro", address},
           Refused{"a line break", kAssetXmr, "monero:8sample\n?tx_amount=1", address},
           Refused{"a tab", kAssetXmr, "monero:8sample\t", address},
           Refused{"a control character", kAssetXmr, std::string("monero:8sample") + '\x01', address},
           Refused{"DEL", kAssetXmr, std::string("monero:8sample") + '\x7F', address},
           Refused{"a byte outside ASCII", kAssetXmr, "monero:8sample?tx_description=caf\xC3\xA9", address},
           Refused{"a fragment", kAssetXmr, "monero:8sample?tx_amount=1#x", address},
       }) {
    FakeCheck check;
    check.answer = false;
    Check(!CryptoUriAllowed(c.asset, c.uri, c.address, check), std::string("refused by the sdk: ") + c.why);
    CheckEq(1, check.calls, std::string("the sdk's check is asked: ") + c.why);
    Check(check.asset == c.asset && check.uri == c.uri && check.address == c.address,
          std::string("with the asset, the uri and the address as they are: ") + c.why);
  }
}

void Routes() {
  CryptoInvoiceView view;
  CheckEq("none", Route(RouteFor(view)), "no invoice");
  view.phase = "none";
  CheckEq("none", Route(RouteFor(view)), "the sdk's none");
  view.phase = kPhaseStarting;
  CheckEq("starting", Route(RouteFor(view)), "a start in flight");
  view = MoneroInvoice();
  CheckEq("open", Route(RouteFor(view)), "an open invoice");
  view.status = kStatusPaid;
  CheckEq("paid", Route(RouteFor(view)), "an open invoice the server credited");
  view.phase = kPhasePaid;
  CheckEq("paid", Route(RouteFor(view)), "paid");
  view = MoneroInvoice();
  view.phase = kPhaseFailed;
  view.status = kStatusUnknown;
  CheckEq("unknown", Route(RouteFor(view)), "an invoice the server no longer knows");
  view = CryptoInvoiceView{};
  view.phase = kPhaseFailed;
  view.errorCode = kErrorAssetUnavailable;
  CheckEq("unavailable", Route(RouteFor(view)), "a coin that cannot be quoted now");
  view.errorCode = "rate_limited";
  CheckEq("refused", Route(RouteFor(view)), "any other refusal");
  view.errorCode = kErrorGuestSignInRequired;
  CheckEq("refused", Route(RouteFor(view)), "a guest's refusal is the surface's");
  view.errorCode = "";
  CheckEq("refused", Route(RouteFor(view)), "a failure without a code (no answer)");
}

// The rules of CONTRACT §6.2.1, state by state. The coin's lines, its payment
// link, whether its memo shows and its steps are the sdk's, carried in the
// view as CryptoPaySdk.h ViewFromSdk fills it; when each shows is this app's.
void Display() {
  // payable: open, pending, time left
  CryptoInvoiceView view = MoneroInvoice();
  CryptoInvoiceDisplay display = DisplayFor(view, true);
  CheckEq("crypto_network_monero", display.networkLine.key, "the network line comes first");
  CheckEq("Monero", display.networkLine.english, "with the sdk's English");
  Check(!display.busy, "an open, polled invoice is not loading");
  Check(display.showAmount && display.showQr && display.showAddress, "payable: the amount, the QR, the address");
  CheckEq(view.uri, display.qrText, "the QR holds the uri the gate passed (the sdk's qr text)");
  Check(display.showOpenWallet, "and Open in wallet opens it");
  Check(!display.noPaymentLink && !display.showByHandLine && display.noPaymentLinkNote.key.empty(),
        "a payment link fills the amount in: no by-hand line, no note under the QR");
  CheckEq("0.072387 XMR", display.amountText, "send exactly the amount with its ticker");
  CheckEq("8sample", display.address, "to this address");
  Check(!display.showMemo && !display.showNetworkWarning && !display.showZcashNote,
        "Monero has no memo, no network warning and no Zcash note");
  CheckEq("counting", Quote(display.quote), "the countdown");
  CheckEq("29:59", display.countdown, "m:ss");
  Check(display.showRefresh && display.refreshEnabled, "Refresh quote while nothing was received");
  Check(display.showSteps, "the steps");
  CheckEq("pending=current,seen=upcoming,confirming=upcoming 0/1,paid=upcoming", Steps(display.steps),
          "the sdk's steps, as it draws them");
  CheckEq("crypto_status_waiting", display.statusKey, "waiting for your payment");
  Check(!display.showRemaining && !display.review && !display.paid, "nothing else to say while waiting");
  Check(display.showWallets, "the wallets that work");
  Check(display.showPayAnotherWay, "another way while nothing was received");
  Check(!display.showUnavailable && !display.showRefusal, "no failure");

  // the sdk's check refused the uri: no QR (and no Open in wallet), the rest stays
  display = DisplayFor(view, false);
  Check(display.showAmount && display.showAddress && !display.showQr && !display.showOpenWallet,
        "a refused uri is never drawn or opened; the amount and the address are sent by hand");

  // a coin with a payment link draws its uri and nothing else: a qr text that
  // is not the uri the gate passed (the bare address) is no QR
  {
    CryptoInvoiceView bare = view;
    bare.qrText = bare.address;
    display = DisplayFor(bare, true);
    Check(!display.showQr && display.qrText.empty(), "Monero's QR is its uri, never its bare address");
    bare.qrText = "";
    display = DisplayFor(bare, true);
    Check(!display.showQr, "no qr text, no QR");
  }

  // requoted on another device (the sdk blanks the uri when a status brings
  // another amount): no QR and no Open in wallet, the new amount and the
  // address to send by hand, whatever the caller says of the uri
  {
    CryptoInvoiceView requoted = view;
    requoted.uri = "";
    requoted.qrText = "";  // the sdk's qr text follows the uri
    requoted.amount = "0.071111";
    display = DisplayFor(requoted, true);
    Check(display.showAmount && display.showAddress && !display.showQr && !display.showOpenWallet,
          "an empty uri is never drawn or opened");
    CheckEq("0.071111 XMR", display.amountText, "the new amount, to send by hand");
    FakeCheck check;
    check.answer = false;  // as urnet::checkCryptoPaymentUri answers an empty uri
    Check(!CryptoUriAllowed(kAssetXmr, requoted.uri, requoted.address, check),
          "the gate refuses an empty uri: the sdk's check does");
    CheckEq(1, check.calls, "asked of the sdk, which keeps the whole gate");
  }

  // a requote in flight: the button stays, disabled
  view.refreshing = true;
  display = DisplayFor(view, true);
  Check(display.showRefresh && !display.refreshEnabled, "one requote at a time");
  view.refreshing = false;

  // expired: the countdown reached zero before the next poll said so. The
  // sdk still lists the pending steps; this app does not show them then
  view.secondsLeft = 0;
  display = DisplayFor(view, true);
  CheckEq("expired", Quote(display.quote), "a pending quote at zero reads expired");
  Check(!display.showAmount && !display.showQr && !display.showAddress && !display.showMemo,
        "expired: no amount, no QR, no Open in wallet");
  Check(display.showRefresh && display.refreshEnabled, "and Refresh quote");
  Check(!display.showSteps && display.steps.empty() && !display.showWallets, "nothing else to pay with");
  Check(display.showPayAnotherWay, "another way stays open");

  // the server's expired (the sdk's steps are at the first step, as for pending)
  view = MoneroInvoice();
  view.status = kStatusExpired;
  view.secondsLeft = 0;
  display = DisplayFor(view, true);
  CheckEq("expired", Quote(display.quote), "expired");
  Check(!display.showAmount && !display.showQr && display.showRefresh && display.showPayAnotherWay,
        "expired: refresh or another way, nothing to send");
  Check(!display.showSteps, "and no steps while the quote is gone");
  view.secondsLeft = 120;
  display = DisplayFor(view, true);
  CheckEq("expired", Quote(display.quote), "the server's expired whatever the clock says");
  Check(!display.showAmount, "and still nothing to send");

  // not polled yet: a resumed invoice before its first status (the sdk lists
  // no steps then; none would show if it did)
  view = MoneroInvoice();
  view.status = "";
  display = DisplayFor(view, true);
  Check(display.busy, "a brief loading line");
  Check(!display.showAmount && !display.showQr && !display.showAddress && !display.showMemo,
        "nothing payable until the first status");
  CheckEq("hidden", Quote(display.quote), "no countdown");
  Check(!display.showRefresh && !display.showPayAnotherWay, "money may be on its way: no requote, no other way");
  Check(!display.showSteps && !display.showWallets, "nothing else either");
  view.secondsLeft = 0;
  display = DisplayFor(view, true);
  CheckEq("hidden", Quote(display.quote), "an unpolled invoice at zero is not expired");

  // seen, short of the amount: the address for the top-up, never the QR or
  // the full amount (scanning again would overpay)
  view = MoneroInvoice();
  view.status = kStatusSeen;
  view.received = "0.06";
  view.remaining = "0.012387";
  view.secondsLeft = 600;
  view.steps = {MakeStep(kStatusPending, kStepDone), MakeStep(kStatusSeen, kStepCurrent),
                MakeStep(kStatusConfirming, kStepUpcoming, 0, 1), MakeStep(kStatusPaid, kStepUpcoming)};
  display = DisplayFor(view, true);
  Check(display.showAddress && !display.showQr && !display.showAmount, "seen: the address, no QR, no full amount");
  Check(display.showRemaining, "how much more to send");
  CheckEq("0.012387 XMR", display.remainingText, "the remainder with its ticker");
  CheckEq("hidden", Quote(display.quote), "no countdown once money arrived");
  Check(!display.showRefresh && !display.showPayAnotherWay, "no requote, no other way once money arrived");
  CheckEq("pending=done,seen=current,confirming=upcoming 0/1,paid=upcoming", Steps(display.steps),
          "payment seen");
  CheckEq("crypto_status_seen", display.statusKey, "payment seen");
  Check(display.showSteps && display.showWallets, "the steps and the wallets");
  view.remaining = "0";
  display = DisplayFor(view, true);
  Check(!display.showRemaining, "nothing more to send");

  // confirming: the steps, the confirming one with the sdk's counts (a whole
  // payment never reports seen: that step is drawn done all the same)
  view = MoneroInvoice();
  view.status = kStatusConfirming;
  view.received = "0.072387";
  view.remaining = "0";
  view.confirmations = 0;
  view.confirmationsRequired = 1;
  view.steps = {MakeStep(kStatusPending, kStepDone), MakeStep(kStatusSeen, kStepDone),
                MakeStep(kStatusConfirming, kStepCurrent, 0, 1), MakeStep(kStatusPaid, kStepUpcoming)};
  display = DisplayFor(view, true);
  Check(!display.showAmount && !display.showQr && !display.showAddress && !display.showWallets,
        "confirming: nothing to send");
  CheckEq("hidden", Quote(display.quote), "no countdown while confirming");
  Check(!display.showRefresh && !display.showPayAnotherWay, "only Close");
  Check(display.showSteps, "the steps");
  CheckEq("pending=done,seen=done,confirming=current 0/1,paid=upcoming", Steps(display.steps),
          "confirming n of N, the step's own counts");
  CheckEq(2, AnnouncedStep(display.steps), "the confirming step is the one announced");
  CheckEq("crypto_status_confirming", display.statusKey, "confirming n of N");

  // paid: every step done, the confirming one at its full count
  view.phase = kPhasePaid;
  view.status = kStatusPaid;
  view.steps = {MakeStep(kStatusPending, kStepDone), MakeStep(kStatusSeen, kStepDone),
                MakeStep(kStatusConfirming, kStepDone, 1, 1), MakeStep(kStatusPaid, kStepDone)};
  display = DisplayFor(view, true);
  Check(display.paid && display.showSteps, "paid: the steps, done");
  CheckEq("pending=done,seen=done,confirming=done 1/1,paid=done", Steps(display.steps), "every step done");
  CheckEq(3, AnnouncedStep(display.steps), "paid is the one announced");
  CheckEq("crypto_status_paid", display.statusKey, "paid, thank you");
  Check(!display.showAmount && !display.showRefresh && !display.showPayAnotherWay, "nothing left to do");
  view.phase = kPhaseOpen;
  display = DisplayFor(view, true);
  Check(display.paid && display.showSteps, "an open invoice whose status is paid is paid");

  // review: the copy, and nothing else to do (the sdk lists no steps then;
  // none would show if it did)
  view = MoneroInvoice();
  view.status = kStatusReview;
  view.reviewReason = "underpaid";
  view.errorCode = "underpaid";
  display = DisplayFor(view, true);
  Check(display.review, "review");
  CheckEq("crypto_status_review", display.statusKey, "the review copy");
  Check(!display.showSteps && !display.showAmount && !display.showQr && !display.showAddress &&
            !display.showRefresh && !display.showWallets && !display.showPayAnotherWay &&
            !display.showUnavailable && !display.showRefusal,
        "review: nothing else to do");
  Check(display.steps.empty(), "no step in review");

  // no steps from the sdk (an asset it does not know): no steps block at all
  view = MoneroInvoice();
  view.steps.clear();
  display = DisplayFor(view, true);
  Check(!display.showSteps && display.steps.empty(), "no steps listed, no steps shown");

  // USDC: the memo with the amount, the network warning with the address, and
  // two steps (its server sees confirmed transfers only)
  view = MoneroInvoice();
  view.asset = kAssetUsdcSolana;
  view.ticker = "USDC";
  view.amount = "40.012345";
  view.memo = "Rsample";
  view.showsMemo = true;  // urnet::cryptoPaymentShowsMemo: the server sent one
  view.uri = "solana:Msample?amount=40.012345";
  view.qrText = view.uri;
  view.address = "Msample";
  view.lines.networkLine = {"crypto_network_usdc", "USDC on Solana"};
  view.lines.networkWarning = {
      "usdc_solana_network_only",
      "Send USDC on the Solana network only. USDC sent on any other network will not arrive."};
  view.steps = {MakeStep(kStatusPending, kStepCurrent), MakeStep(kStatusPaid, kStepUpcoming)};
  display = DisplayFor(view, true);
  CheckEq("crypto_network_usdc", display.networkLine.key, "USDC on Solana");
  CheckEq("40.012345 USDC", display.amountText, "USDC's amount");
  Check(display.showMemo && display.memo == "Rsample", "the memo, for a transfer sent by hand");
  Check(display.showNetworkWarning, "the Solana-only warning, with the address");
  CheckEq("usdc_solana_network_only", display.networkWarning.key, "USDC's own words");
  CheckEq("pending=current,paid=upcoming", Steps(display.steps), "USDC's two steps");
  view.memo = "";
  view.showsMemo = false;
  display = DisplayFor(view, true);
  Check(!display.showMemo && display.showNetworkWarning, "no memo to show; the warning stays");
  view.secondsLeft = 0;
  display = DisplayFor(view, true);
  Check(!display.showNetworkWarning, "no address, no warning, once the quote expired");
  view = MoneroInvoice();
  view.memo = "Rsample";
  view.showsMemo = true;
  view.secondsLeft = 0;
  display = DisplayFor(view, true);
  Check(!display.showMemo, "a memo the sdk would show waits for a payable quote");

  // Zcash: the exchange note while a payment is asked for
  view = MoneroInvoice();
  view.asset = kAssetZec;
  view.ticker = "ZEC";
  view.lines.networkLine = {"crypto_network_zcash", "Zcash, shielded"};
  display = DisplayFor(view, true);
  CheckEq("crypto_network_zcash", display.networkLine.key, "Zcash, shielded");
  Check(display.showZcashNote && !display.showNetworkWarning, "the shielded-address note, and no warning");
  view.status = kStatusSeen;
  display = DisplayFor(view, true);
  Check(display.showZcashNote, "and for the top-up");
  view.status = kStatusConfirming;
  display = DisplayFor(view, true);
  Check(!display.showZcashNote, "not once the payment is complete");

  // TAO: no payment link (mmm/BTCPAY.md §17.6). The sdk keeps no uri and its
  // qr text is the address alone: the QR holds the address with the note
  // saying so, there is nothing to open in a wallet, and the invoice says to
  // send exactly this amount to this address, Copy amount and Copy address,
  // with the Bittensor network's warning
  {
    CryptoInvoiceView tao = MoneroInvoice();
    tao.asset = kAssetTao;
    tao.ticker = "TAO";
    tao.amount = "0.123456";
    tao.address = "5Tsample";
    tao.uri = "";
    tao.qrText = tao.address;
    tao.noPaymentLink = true;  // urnet::cryptoPaymentUriScheme("tao") is ""
    tao.lines.networkLine = {"crypto_network_tao", "Bittensor network"};
    tao.lines.networkWarning = {"crypto_invoice_tao_network_only",
                                "Send only TAO on the Bittensor network to this address. TAO sent on "
                                "any other network won't be credited."};
    tao.lines.noPaymentLinkNote = {"crypto_invoice_tao_no_payment_link",
                                   "TAO has no payment link, so this QR code holds only the address. "
                                   "Enter the exact amount in your wallet."};
    display = DisplayFor(tao, false);  // the gate refuses an empty uri
    CheckEq("crypto_network_tao", display.networkLine.key, "TAO: the Bittensor network first");
    Check(display.showQr, "TAO: a QR code");
    CheckEq("5Tsample", display.qrText, "of the address alone");
    CheckEq("crypto_invoice_tao_no_payment_link", display.noPaymentLinkNote.key, "with the note saying so");
    Check(!display.showOpenWallet, "TAO: nothing to open in a wallet");
    Check(display.noPaymentLink && display.showByHandLine, "TAO: send exactly this amount to this address");
    Check(display.showAmount && display.showAddress, "the exact amount and the address, each with its Copy");
    CheckEq("0.123456 TAO", display.amountText, "TAO's amount with its ticker");
    Check(display.showNetworkWarning, "the Bittensor network's warning, with the address");
    CheckEq("crypto_invoice_tao_network_only", display.networkWarning.key, "TAO's words");
    Check(!display.showMemo && !display.showZcashNote, "no memo, no Zcash note");
    CheckEq("counting", Quote(display.quote), "the countdown, as for any quote");
    display = DisplayFor(tao, true);
    Check(display.showQr && !display.showOpenWallet, "whatever the gate says of an empty uri");

    // the QR holds the address, or nothing
    tao.qrText = "tao:5Tsample";
    display = DisplayFor(tao, false);
    Check(!display.showQr && display.noPaymentLinkNote.key.empty(),
          "a qr text that is not the address: no QR, no note");
    Check(display.showAmount && display.showAddress && display.showByHandLine, "the address and the amount, by hand");
    tao.qrText = "";
    display = DisplayFor(tao, false);
    Check(!display.showQr && display.showAmount && display.showAddress, "no qr text: no QR, the rest by hand");

    // a coin the sdk says has a payment link never draws its bare address
    tao.qrText = tao.address;
    tao.noPaymentLink = false;
    display = DisplayFor(tao, false);
    Check(!display.showQr && display.noPaymentLinkNote.key.empty() && !display.showByHandLine,
          "the address-only QR is for a coin without a payment link alone");
    tao.noPaymentLink = true;

    // a top-up: the address for the rest, never the QR or the full amount
    tao.status = kStatusSeen;
    tao.received = "0.12";
    tao.remaining = "0.003456";
    display = DisplayFor(tao, false);
    Check(display.showAddress && display.showNetworkWarning && !display.showQr && !display.showAmount &&
              !display.showByHandLine,
          "seen: the address and its warning, no QR, no full amount");
    CheckEq("0.003456 TAO", display.remainingText, "the rest to send");

    // expired: nothing to send
    tao.status = kStatusPending;
    tao.secondsLeft = 0;
    display = DisplayFor(tao, false);
    Check(!display.showQr && !display.showAmount && !display.showByHandLine &&
              display.noPaymentLinkNote.key.empty(),
          "expired: no QR, no amount, no instruction");
  }

  // Bitcoin, ETH and native SOL: each its link's QR and Open in wallet, as
  // Monero's, its network's warning with the address, and its steps as the
  // sdk lists them (SOL, TAO and ETH usually jump from pending to paid, the
  // steps between drawn done)
  {
    struct LinkedCoin {
      const char* asset;
      const char* ticker;
      std::string uri;
      CoinLine networkLine;
      CoinLine networkWarning;
    };
    for (const LinkedCoin& c : {
             LinkedCoin{kAssetBtc, "BTC", "bitcoin:bc1qsample?amount=0.00061234",
                        {"crypto_network_bitcoin", "Bitcoin network"},
                        {"crypto_invoice_bitcoin_network_only", "Send only Bitcoin on the Bitcoin network."}},
             LinkedCoin{kAssetEth, "ETH", "ethereum:0xsample@1?value=12000000000000000",
                        {"crypto_network_eth", "Ethereum mainnet"},
                        {"crypto_invoice_eth_network_only", "Send only ETH on Ethereum mainnet."}},
             LinkedCoin{kAssetSol, "SOL", "solana:Ssample?amount=0.251234",
                        {"crypto_network_sol", "Solana (native SOL)"},
                        {"crypto_invoice_sol_network_only", "Send only SOL on the Solana network."}},
         }) {
      CryptoInvoiceView coin = MoneroInvoice();
      coin.asset = c.asset;
      coin.ticker = c.ticker;
      coin.uri = c.uri;
      coin.qrText = c.uri;
      coin.lines.networkLine = c.networkLine;
      coin.lines.networkWarning = c.networkWarning;
      display = DisplayFor(coin, true);
      const std::string name = c.asset;
      Check(display.networkLine == c.networkLine, name + ": its network line first, key and English");
      Check(display.showQr && display.qrText == c.uri && display.showOpenWallet,
            name + ": the QR of its link, and Open in wallet");
      Check(!display.noPaymentLink && !display.showByHandLine && display.noPaymentLinkNote.key.empty(),
            name + ": its link fills the amount in");
      Check(display.showNetworkWarning, name + ": its network's warning with the address");
      Check(display.networkWarning == c.networkWarning, name + ": in its own words");
      Check(!display.showMemo && !display.showZcashNote, name + ": no memo given, no Zcash note");
      coin.phase = kPhasePaid;
      coin.status = kStatusPaid;
      coin.steps = {MakeStep(kStatusPending, kStepDone), MakeStep(kStatusSeen, kStepDone),
                    MakeStep(kStatusConfirming, kStepDone, 1, 1), MakeStep(kStatusPaid, kStepDone)};
      display = DisplayFor(coin, true);
      CheckEq("pending=done,seen=done,confirming=done 1/1,paid=done", Steps(display.steps),
              name + ": straight to paid, every step done");
    }
    // a memo shows exactly when the sdk says so (only a memo the server sent;
    // it sends one for USDC alone, SOL included in the "")
    CryptoInvoiceView sol = MoneroInvoice();
    sol.asset = kAssetSol;
    sol.memo = "";
    sol.showsMemo = false;
    display = DisplayFor(sol, true);
    Check(!display.showMemo, "SOL: no memo row, the server sends none");
    CryptoInvoiceView btc = MoneroInvoice();
    btc.asset = kAssetBtc;
    btc.memo = "Rsample";
    btc.showsMemo = false;
    display = DisplayFor(btc, true);
    Check(!display.showMemo, "a memo the sdk does not show stays hidden, whatever the state says");
  }

  // starting: busy
  view = CryptoInvoiceView{};
  view.phase = kPhaseStarting;
  view.asset = kAssetXmr;
  view.lines.networkLine = {"crypto_network_monero", "Monero"};
  display = DisplayFor(view, true);
  Check(display.busy && !display.showAmount && !display.showSteps && !display.showRefresh &&
            !display.showPayAnotherWay,
        "starting: busy, nothing else");
  CheckEq("crypto_network_monero", display.networkLine.key, "the network line while starting");

  // the last requote failed
  view = MoneroInvoice();
  view.status = kStatusExpired;
  view.secondsLeft = 0;
  view.errorCode = kErrorAssetUnavailable;
  display = DisplayFor(view, true);
  Check(display.showUnavailable && !display.showRefusal, "a coin that cannot be quoted now");
  Check(display.showRefresh && display.showPayAnotherWay, "a retry, or another way");
  view.errorCode = kErrorQuoteExpired;
  display = DisplayFor(view, true);
  Check(!display.showUnavailable && !display.showRefusal, "an expired quote is the quote line's");
  view.errorCode = "rate_limited";
  display = DisplayFor(view, true);
  Check(!display.showUnavailable && display.showRefusal, "any other code is a refusal");
  view.errorCode = "";
  view.errorMessage = "Post \"https://api.example/pay/crypto/requote\": EOF";
  display = DisplayFor(view, true);
  Check(display.showRefusal, "a requote with no answer from the server");

  // the server no longer knows the invoice: the failure, and another way
  view = MoneroInvoice();
  view.phase = kPhaseFailed;
  view.status = kStatusUnknown;
  display = DisplayFor(view, true);
  Check(display.showRefusal && display.showPayAnotherWay, "unknown: the failure, and another way");
  Check(!display.showAmount && !display.showQr && !display.showAddress && !display.showSteps &&
            !display.showRefresh,
        "unknown: nothing to pay");
}

void PaymentInProgress() {
  CryptoInvoiceView view = MoneroInvoice();
  Check(ShowsPaymentInProgress(view) && InvoiceInProgress(view), "a saved invoice waiting for its payment");
  view.status = "";
  Check(InvoiceInProgress(view), "a resumed invoice before its first status");
  view.status = kStatusSeen;
  Check(InvoiceInProgress(view), "money seen");
  view.status = kStatusConfirming;
  Check(InvoiceInProgress(view), "confirming");
  view.status = kStatusReview;
  Check(InvoiceInProgress(view), "in review: the row opens the review copy");
  view.status = kStatusExpired;
  Check(InvoiceInProgress(view), "an expired quote can still be refreshed");
  view.status = kStatusPaid;
  Check(!InvoiceInProgress(view), "paid: the success path, not a row");
  view.phase = kPhaseFailed;
  view.status = kStatusUnknown;
  Check(!InvoiceInProgress(view), "an invoice the server no longer knows");
  Check(!InvoiceInProgress(CryptoInvoiceView{}), "no invoice");
  view = CryptoInvoiceView{};
  view.phase = kPhaseStarting;
  Check(!InvoiceInProgress(view), "a start in flight is the sheet's");
  view.phase = kPhaseFailed;
  view.errorCode = kErrorAssetUnavailable;
  Check(!InvoiceInProgress(view), "a failed start");

  // the row's second line
  view = MoneroInvoice();
  CheckEq("crypto_status_waiting", InProgressNoteKey(view), "waiting for the payment");
  view.status = "";
  CheckEq("", InProgressNoteKey(view), "nothing before the first status: money may be on its way");
  view.status = kStatusPending;
  view.secondsLeft = 0;
  CheckEq("crypto_invoice_quote_expired", InProgressNoteKey(view), "a quote that ran out");
  view.status = kStatusExpired;
  CheckEq("crypto_invoice_quote_expired", InProgressNoteKey(view), "the server's expired");
  view.status = kStatusSeen;
  CheckEq("crypto_status_seen", InProgressNoteKey(view), "money seen, whatever the clock says");
  view.status = kStatusConfirming;
  CheckEq("crypto_status_confirming", InProgressNoteKey(view), "confirming, with its counts");
  view.status = kStatusReview;
  CheckEq("crypto_status_review", InProgressNoteKey(view), "in review");
  view.status = kStatusPaid;
  CheckEq("", InProgressNoteKey(view), "no row once paid");
}

void Times() {
  struct Parsed {
    const char* text;
    int64_t seconds;
  };
  for (const Parsed& c : {
           Parsed{"1970-01-01T00:00:00Z", 0},
           Parsed{"2026-10-20T18:30:00Z", 1792521000},
           Parsed{"2026-10-20T20:30:00+02:00", 1792521000},
           Parsed{"2026-10-20T13:30:00-05:00", 1792521000},
           Parsed{"2026-10-20T18:30:00.123456Z", 1792521000},
           Parsed{"2026-10-20t18:30:00z", 1792521000},
           Parsed{"2028-02-29T00:00:00Z", 1835395200},
           Parsed{"2000-03-01T12:00:00Z", 951912000},
           Parsed{"1969-12-31T23:59:59Z", -1},
       }) {
    const std::optional<int64_t> parsed = ParseRfc3339(c.text);
    Check(parsed.has_value(), std::string("parses: ") + c.text);
    if (parsed) CheckEq(c.seconds, *parsed, c.text);
  }
  for (const char* text : {"", "2026-10-20", "2026-10-20T18:30:00", "2026-10-20 18:30:00Z",
                           "2026-02-29T00:00:00Z", "2026-13-01T00:00:00Z", "2026-04-31T00:00:00Z",
                           "2026-10-20T24:00:00Z", "2026-10-20T18:60:00Z", "2026-10-20T18:30:00.Z",
                           "2026-10-20T18:30:00+0200", "2026-10-20T18:30:00Zjunk", "26-10-20T18:30:00Z",
                           "2026-1O-20T18:30:00Z"}) {
    Check(!ParseRfc3339(text).has_value(), std::string("refused: \"") + text + "\"");
  }

  Check(!StoreFamilyRenews("crypto"), "a crypto year never renews");
  Check(!StoreFamilyRenews("gift"), "a gift year never renews");
  for (const char* family : {"stripe", "apple", "google", "other", ""}) {
    Check(StoreFamilyRenews(family), std::string("renews or is not crypto: \"") + family + "\"");
  }
  Check(DoesNotRenewShows("crypto", false), "a crypto year: does not renew");
  Check(!DoesNotRenewShows("crypto", true), "the term notice says the rest");
  Check(!DoesNotRenewShows("stripe", false) && !DoesNotRenewShows("", false) &&
            !DoesNotRenewShows("other", false),
        "only a crypto year");
  // a gift year has its own line (ShowsGiftDoesNotRenew), never the crypto one,
  // which asks to pay again
  Check(!DoesNotRenewShows("gift", false), "a gift year is not told to pay again");
}

void Qr() {
  QrLayout empty = QrLayoutFor(0, 320);
  Check(empty.moduleCount == 0 && empty.frameSide == 0, "no code draws nothing");
  empty = QrLayoutFor(25, 0);
  Check(empty.moduleCount == 0 && empty.frameSide == 0, "no room draws nothing");
  const QrLayout small = QrLayoutFor(21, 320);
  CheckEq(11, static_cast<int64_t>(small.moduleSize), "whole dip per module");
  CheckEq(231, static_cast<int64_t>(small.codeSide), "the code's side");
  CheckEq(44, static_cast<int64_t>(small.quietZone), "four modules of quiet zone");
  CheckEq(319, static_cast<int64_t>(small.frameSide), "inside the frame");
  const QrLayout tiny = QrLayoutFor(21, 10);
  CheckEq(1, static_cast<int64_t>(tiny.moduleSize), "never below one dip");

  // the server's uris (§3.5) at real lengths: each encodes, and each draws at
  // four dip a module or more inside the invoice's 320-dip frame
  const std::string description = "URnetwork%20Pro%20yearly%20%28welcome%20offer%29";
  struct Sample {
    const char* name;
    std::string uri;
  };
  for (const Sample& sample : {
           Sample{"Monero", "monero:" + Fill('8', 95) + "?tx_amount=0.072387&tx_description=" + description},
           Sample{"Zcash", "zcash:" + Bech32Fill("u1", 213) + "?amount=0.123456&message=" + description},
           Sample{"USDC", "solana:" + Fill('M', 44) + "?amount=30.000001&spl-token=" + Fill('E', 44) +
                              "&reference=" + Fill('R', 44) + "&label=URnetwork&message=" + description},
           // mmm/BTCPAY.md §17.7: BIP 21, EIP-681, Solana Pay without
           // spl-token, and TAO's address alone (an ss58 address)
           Sample{"Bitcoin", "bitcoin:" + Bech32Fill("bc1q", 42) + "?amount=0.00061234&label=URnetwork&message=" +
                                 description},
           Sample{"ETH", "ethereum:0x" + std::string(40, 'a') + "@1?value=12000000000000000"},
           Sample{"SOL", "solana:" + Fill('S', 44) + "?amount=0.251234&reference=" + Fill('R', 44) +
                             "&label=URnetwork&message=" + description},
           Sample{"TAO", Fill('5', 48)},
       }) {
    try {
      const qrcodegen::QrCode code =
          qrcodegen::QrCode::encodeText(sample.uri.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
      const QrLayout layout = QrLayoutFor(code.getSize(), 320);
      Check(4 <= layout.moduleSize, std::string(sample.name) + ": four dip a module or more (" +
                                        std::to_string(code.getSize()) + " modules)");
      Check(layout.frameSide <= 320, std::string(sample.name) + ": inside the frame");
    } catch (const std::exception& e) {
      Check(false, std::string(sample.name) + ": does not encode: " + e.what());
    }
  }
}

// The marks' files (CryptoPayViews.cpp CoinMark): an icon key names a bundled
// file and nothing else, and SvgPathData reads one plain path out of it.
void Marks() {
  Check(IsMarkKey("coin_tao") && IsMarkKey("coin_btc") && IsMarkKey("coin_eth") && IsMarkKey("coin_sol"),
        "the sdk's icon keys name files");
  for (const char* key : {"", "../coin_tao", "coin_tao.svg", "Coin_Tao", "coin tao", "coin\\tao"}) {
    Check(!IsMarkKey(key), std::string("not a mark's name: \"") + key + "\"");
  }
  const std::string svg =
      "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"24\" height=\"24\" viewBox=\"0 0 24 24\">\n"
      "<!-- a comment that names <path d=\"M9 9Z\"/> -->\n"
      "<path fill=\"currentColor\" d=\"M14.38 18.415V8.577C14.38 6.119 12.351 4.116 9.893 4.116Z\"/>\n"
      "</svg>\n";
  CheckEq("M14.38 18.415V8.577C14.38 6.119 12.351 4.116 9.893 4.116Z", SvgPathData(svg),
          "the one path's data, past a comment");
  CheckEq("", SvgPathData("<svg><path d=\"M0 0Z\"/><path d=\"M1 1Z\"/></svg>"), "two paths: not one mark");
  CheckEq("", SvgPathData("<svg></svg>"), "no path");
  CheckEq("", SvgPathData("<svg><path fill=\"x\"/></svg>"), "a path without data");
  CheckEq("", SvgPathData("<svg><path d=\"M0 0'/><Button/>\"/></svg>"),
          "markup in the data never reaches the PathIcon");
  CheckEq("", SvgPathData("<svg><path d=\"M0 0&amp;Z\"/></svg>"), "an entity is not path data");
  CheckEq("", SvgPathData("<svg><path d=\"M0 0Z"), "an unclosed element");
  CheckEq("", SvgPathData("<!-- <path d=\"M0 0Z\"/>"), "an unclosed comment");
  CheckEq("M0 0Z", SvgPathData("<svg><pathway/><path\nd=\"M0 0Z\"/></svg>"), "only a path element is a path");
  CheckEq(24, static_cast<int64_t>(kMarkViewBox), "the marks' 24 x 24 box");
}

#if defined(URNW_CRYPTO_PAY_TESTS_SDK)
// The generated wrapper's own structs, read (and, for a tap on a tab, written
// back) through CryptoPaySdk.h. Its calls into the sdk (ViewFromSdk's answers
// for the coin, PaymentTabsFor, TabCoinFor, MethodEventProduct, ...) need the
// sdk library and are not run here; what they read and write is.
void Sdk() {
  urnet::CryptoInvoiceState state;
  state.phase = "open";
  state.intent_id = "0b6f";
  state.asset = "xmr";
  state.plan = "yearly";
  state.item_id = "";
  state.address = "8sample";
  state.amount = "0.072387";
  state.ticker = "XMR";
  state.uri = "monero:8sample?tx_amount=0.072387";
  state.qr_text = "monero:8sample?tx_amount=0.072387";
  state.memo = "";
  state.price_usd = 40;
  state.expires_at = "2026-10-20T18:30:00Z";
  state.seconds_left = 1799;
  state.status = "seen";
  state.review_reason = "";
  state.received = "0.06";
  state.remaining = "0.012387";
  state.confirmations = 0;
  state.confirmations_required = 1;
  state.steps = nlohmann::json::parse(R"([
    {"status": "pending", "progress": "done", "confirmations": 0, "confirmations_required": 0},
    {"status": "seen", "progress": "current", "confirmations": 0, "confirmations_required": 0},
    {"status": "confirming", "progress": "upcoming", "confirmations": 0, "confirmations_required": 1},
    {"status": "paid", "progress": "upcoming", "confirmations": 0, "confirmations_required": 0}
  ])").get<urnet::CryptoPaymentStepList>();
  state.refreshing = true;
  state.error_code = "asset_unavailable";
  state.error_message = "Temporarily unavailable";
  const CryptoInvoiceView view = StateFromSdk(state);
  CheckEq("open", view.phase, "sdk: the phase");
  CheckEq("0b6f", view.intentId, "sdk: the intent");
  CheckEq("xmr", view.asset, "sdk: the asset");
  CheckEq("yearly", view.plan, "sdk: the plan");
  CheckEq("8sample", view.address, "sdk: the address");
  CheckEq("0.072387", view.amount, "sdk: the amount");
  CheckEq("XMR", view.ticker, "sdk: the ticker");
  CheckEq("monero:8sample?tx_amount=0.072387", view.uri, "sdk: the uri");
  CheckEq("monero:8sample?tx_amount=0.072387", view.qrText, "sdk: the qr text");
  Check(view.priceUsd == 40, "sdk: the price");
  CheckEq("2026-10-20T18:30:00Z", view.expiresAt, "sdk: the expiry");
  CheckEq(1799, view.secondsLeft, "sdk: the seconds left");
  CheckEq("seen", view.status, "sdk: the status");
  CheckEq("0.06", view.received, "sdk: received");
  CheckEq("0.012387", view.remaining, "sdk: remaining");
  CheckEq(1, view.confirmationsRequired, "sdk: confirmations required");
  CheckEq("pending=done,seen=current,confirming=upcoming 0/1,paid=upcoming", Steps(view.steps),
          "sdk: the state's steps, step for step");
  Check(view.refreshing, "sdk: refreshing");
  CheckEq("asset_unavailable", view.errorCode, "sdk: the error code");
  CheckEq("Temporarily unavailable", view.errorMessage, "sdk: the error message");
  Check(view.lines == CoinLines{} && !view.noPaymentLink && !view.showsMemo,
        "sdk: the state alone carries none of the sdk's answers (ViewFromSdk adds them)");

  // the state as the controller hands it over (json-tagged): SOL paid at once,
  // the confirming step not listed (no count required), every step done
  const urnet::CryptoInvoiceState paid = nlohmann::json::parse(R"({
    "phase": "paid", "asset": "sol", "status": "paid", "confirmations": 0, "confirmations_required": 0,
    "steps": [{"status": "pending", "progress": "done", "confirmations": 0, "confirmations_required": 0},
              {"status": "seen", "progress": "done", "confirmations": 0, "confirmations_required": 0},
              {"status": "paid", "progress": "done", "confirmations": 0, "confirmations_required": 0}]
  })").get<urnet::CryptoInvoiceState>();
  CheckEq("pending=done,seen=done,paid=done", Steps(StateFromSdk(paid).steps), "sdk: the steps from the json");
  const urnet::CryptoInvoiceState unpolled = nlohmann::json::parse(R"({
    "phase": "open", "asset": "xmr", "status": "", "steps": []
  })").get<urnet::CryptoInvoiceState>();
  Check(StateFromSdk(unpolled).steps.empty(), "sdk: no steps before the first status");

  // a coin's lines (urnet::cryptoAssetLinesFor's answer)
  const urnet::CryptoAssetLines taoLines = nlohmann::json::parse(R"({
    "network_line": "Bittensor network", "network_line_key": "crypto_network_tao",
    "network_warning": "Send only TAO on the Bittensor network to this address.",
    "network_warning_key": "crypto_invoice_tao_network_only",
    "no_payment_link_note": "TAO has no payment link.",
    "no_payment_link_note_key": "crypto_invoice_tao_no_payment_link"
  })").get<urnet::CryptoAssetLines>();
  const CoinLines tao = LinesFromSdk(taoLines);
  CheckEq("crypto_network_tao", tao.networkLine.key, "sdk: a coin's network line, its key");
  CheckEq("Bittensor network", tao.networkLine.english, "sdk: and its English");
  CheckEq("crypto_invoice_tao_network_only", tao.networkWarning.key, "sdk: its warning");
  CheckEq("Send only TAO on the Bittensor network to this address.", tao.networkWarning.english,
          "sdk: the warning's English");
  CheckEq("crypto_invoice_tao_no_payment_link", tao.noPaymentLinkNote.key, "sdk: TAO's note");
  CheckEq("TAO has no payment link.", tao.noPaymentLinkNote.english, "sdk: the note's English");
  const urnet::CryptoAssetLines zecLines = nlohmann::json::parse(R"({
    "network_line": "Zcash, shielded", "network_line_key": "crypto_network_zcash",
    "network_warning": "", "network_warning_key": "", "no_payment_link_note": "", "no_payment_link_note_key": ""
  })").get<urnet::CryptoAssetLines>();
  const CoinLines zec = LinesFromSdk(zecLines);
  Check(zec.networkLine.key == "crypto_network_zcash" && zec.networkWarning.key.empty() &&
            zec.noPaymentLinkNote.key.empty(),
        "sdk: a line the coin does not have has no key");

  // the balance's coins, the ones it cannot quote now, and its term, from the
  // server's json
  const urnet::SubscriptionBalanceResult result = nlohmann::json::parse(R"({
    "start_balance_byte_count": 0, "balance_byte_count": 0, "open_transfer_byte_count": 0,
    "crypto_assets": [
      {"asset": "usdc_solana", "plans": ["yearly", "yearly_onboarding"], "items": ["data_1tib"],
       "confirmations_required": 1, "quote_ttl_seconds": 3600},
      {"asset": "xmr", "plans": ["yearly"], "confirmations_required": 1, "quote_ttl_seconds": 1800}
    ],
    "crypto_unavailable_assets": [
      {"asset": "btc", "plans": ["yearly", "yearly_onboarding"], "confirmations_required": 1,
       "quote_ttl_seconds": 1800}
    ],
    "crypto_term": {"market": "solana", "asset": "sol", "term_end_time": "2026-11-19T00:00:00Z",
                    "grace_end_time": "2026-12-19T00:00:00Z"}
  })").get<urnet::SubscriptionBalanceResult>();
  const urnet::CryptoPaymentAssetList assets = AssetsFromSdk(result);
  CheckEq(2, static_cast<int64_t>(assets.size()), "sdk: the balance's coins");
  if (assets.size() == 2) CheckEq("xmr", assets[1].asset, "sdk: a coin's asset");
  const urnet::CryptoPaymentAssetList unavailable = UnavailableAssetsFromSdk(result);
  CheckEq(1, static_cast<int64_t>(unavailable.size()), "sdk: the coins it cannot quote now");
  if (unavailable.size() == 1) CheckEq("btc", unavailable[0].asset, "sdk: Bitcoin, greyed in its place");
  const CryptoTermView term = TermFromSdk(result);
  CheckEq("solana", term.market, "sdk: the term's market");
  CheckEq("2026-11-19T00:00:00Z", term.termEndTime, "sdk: the term's end");
  CheckEq("2026-12-19T00:00:00Z", term.graceEndTime, "sdk: the grace's end");
  // the term's coin decodes (USDC and SOL share solana) but nothing here names
  // the coin a year was paid with
  CheckEq("sol", result.crypto_term ? result.crypto_term->asset : std::string(), "sdk: the term's coin");
  const urnet::SubscriptionBalanceResult older = nlohmann::json::parse(R"({
    "start_balance_byte_count": 0, "balance_byte_count": 0, "open_transfer_byte_count": 0
  })").get<urnet::SubscriptionBalanceResult>();
  Check(AssetsFromSdk(older).empty(), "sdk: an older server lists no coin");
  Check(UnavailableAssetsFromSdk(older).empty(), "sdk: and none it cannot quote");
  Check(TermFromSdk(older).termEndTime.empty(), "sdk: and has no term");

  // the selector as urnet::cryptoPaymentTabs hands it over (json-tagged)
  const urnet::CryptoPaymentTabList sdkTabs = nlohmann::json::parse(R"([
    {"id": "usdc_solana", "label": "USDC on Solana", "label_key": "pay_method_usdc_solana",
     "icon_keys": ["coin_usdc"],
     "coins": [{"asset": "usdc_solana", "label": "USDC on Solana", "label_key": "pay_method_usdc_solana",
                "icon_key": "coin_usdc", "available": false}],
     "available": false},
    {"id": "more_coins", "label": "More coins", "label_key": "pay_method_more_coins",
     "icon_keys": ["coin_tao", "coin_btc", "coin_eth", "coin_sol"],
     "coins": [{"asset": "tao", "label": "TAO", "label_key": "pay_method_tao", "icon_key": "coin_tao",
                "available": true},
               {"asset": "btc", "label": "Bitcoin", "label_key": "pay_method_bitcoin", "icon_key": "coin_btc",
                "available": false}],
     "available": true}
  ])").get<urnet::CryptoPaymentTabList>();
  const std::vector<PaymentTabView> tabs = TabsFromSdk(sdkTabs);
  CheckEq(2, static_cast<int64_t>(tabs.size()), "sdk: the tabs");
  if (tabs.size() == 2) {
    CheckEq("usdc_solana", tabs[0].id, "sdk: a tab's id");
    CheckEq("pay_method_usdc_solana", tabs[0].labelKey, "sdk: a tab's label key");
    Check(!tabs[0].available && !tabs[0].coins.at(0).available, "sdk: an unavailable tab and coin");
    CheckEq("more_coins", tabs[1].id, "sdk: More coins");
    CheckEq("More coins", tabs[1].label, "sdk: its English");
    CheckEq("coin_tao,coin_btc,coin_eth,coin_sol", Join(tabs[1].iconKeys), "sdk: its four marks");
    CheckEq(2, static_cast<int64_t>(tabs[1].coins.size()), "sdk: its coins");
    if (tabs[1].coins.size() == 2) {
      const PaymentCoinView& btc = tabs[1].coins[1];
      Check(btc.asset == "btc" && btc.label == "Bitcoin" && btc.labelKey == "pay_method_bitcoin" &&
                btc.iconKey == "coin_btc" && !btc.available,
            "sdk: a coin, field for field");
      Check(btc.networkLine.key.empty(), "sdk: its network line is no part of the tab (PaymentTabsFor adds it)");
    }
    Check(tabs[1].available, "sdk: a tab with a coin that can be paid with");

    // and back, as a tap hands the tab to urnet::cryptoPaymentTabCoin
    const urnet::CryptoPaymentTab back = TabToSdk(tabs[1]);
    CheckEq("more_coins", back.id, "sdk: a tab back, its id");
    CheckEq("pay_method_more_coins", back.label_key, "sdk: its label key");
    Check(back.icon_keys && back.icon_keys->size() == 4, "sdk: its marks");
    Check(back.coins && back.coins->size() == 2 && (*back.coins)[0].asset == "tao" &&
              (*back.coins)[0].available && (*back.coins)[1].asset == "btc" && !(*back.coins)[1].available,
          "sdk: its coins, each with whether it can be paid with now");
    Check(back.available, "sdk: and whether the tab can be chosen");
    const urnet::CryptoPaymentTab sent = nlohmann::json(back).get<urnet::CryptoPaymentTab>();
    Check(TabsFromSdk({sent}) == std::vector<PaymentTabView>{tabs[1]},
          "sdk: a tab survives the trip to the sdk as json, field for field");
  }
}
#endif

}  // namespace

const char* LineName(PlanCardLine line) {
  switch (line) {
    case PlanCardLine::Price: return "price";
    case PlanCardLine::Secondary: return "secondary";
    case PlanCardLine::Equivalent: return "equivalent";
    case PlanCardLine::Trial: return "trial";
  }
  return "?";
}

// "price,secondary,-equivalent,..." with a minus for a line kept transparent
std::string Slots(const std::array<PlanCardSlot, 4>& slots) {
  std::string out;
  for (const PlanCardSlot& slot : slots) {
    if (!out.empty()) out += ",";
    if (!slot.shown) out += "-";
    out += LineName(slot.line);
  }
  return out;
}

// No renewal or trial wording while a coin is selected (CONTRACT §6.1).
void PlanCardCopy() {
  Check(CardTermsShown(kMethodCard), "Card keeps its trial and renewal lines");
  Check(!CardTermsShown(kAssetUsdcSolana) && !CardTermsShown(kAssetZec) && !CardTermsShown(kAssetXmr),
        "a coin buys one year once: no trial or renewal lines");
  CheckEq("price,secondary,equivalent,trial", Slots(YearlyCardSlots(true, true)),
          "Card with the offer: the first-year price, then the renewal price, the equivalent, the trial");
  CheckEq("price,secondary,equivalent,trial", Slots(YearlyCardSlots(true, false)),
          "Card without the offer: the price, the saving, the equivalent, the trial");
  CheckEq("price,equivalent,-secondary,-trial", Slots(YearlyCardSlots(false, true)),
          "a coin with the offer: no renewal price and no trial, their room kept below the equivalent");
  CheckEq("price,secondary,equivalent,-trial", Slots(YearlyCardSlots(false, false)),
          "a coin without the offer: the saving stays, the trial goes");
  // every line keeps a block, whatever the method: the card keeps its height
  for (const bool card : {true, false}) {
    for (const bool renews : {true, false}) {
      const auto slots = YearlyCardSlots(card, renews);
      int seen[4] = {0, 0, 0, 0};
      for (const PlanCardSlot& slot : slots) ++seen[static_cast<int>(slot.line)];
      Check(seen[0] == 1 && seen[1] == 1 && seen[2] == 1 && seen[3] == 1, "each line in one block");
      CheckEq("price", LineName(slots[0].line), "the price first, always");
      Check(slots[0].shown, "the price is what the coin pays: it stays");
      CheckEq("trial", LineName(slots[3].line), "the trial last, in its gold block");
      for (const PlanCardSlot& slot : slots) {
        const bool trialOrRenewal = slot.line == PlanCardLine::Trial ||
                                    (slot.line == PlanCardLine::Secondary && renews);
        if (!card && trialOrRenewal) Check(!slot.shown, "no trial or renewal wording for a coin");
        if (card) Check(slot.shown, "Card shows every line");
      }
    }
  }
  // the welcome-offer year is yearly to the events, whatever the method buys
  CheckEq(kPlanYearlyOnboarding, CryptoPlanFor(true), "the sdk is asked for the offer's year");
}

int main() {
  MethodRow();
  PlanCardCopy();
  InvoiceWords();
  UriGate();
  Routes();
  Display();
  PaymentInProgress();
  Times();
  Qr();
  Marks();
#if defined(URNW_CRYPTO_PAY_TESTS_SDK)
  Sdk();
#endif
  std::cout << (gCases - gFailures) << "/" << gCases << " crypto pay checks passed\n";
  return gFailures == 0 ? 0 : 1;
}
