// SPDX-License-Identifier: MPL-2.0
#include "pch.h"

#include "WalletPage.h"

#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Automation.Peers.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.Shapes.h>
#include <winrt/Windows.UI.Text.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <iterator>
#include <string_view>

#include "Log.h"
#include "MainWindow.xaml.h"
#include "PageContext.h"
#include "StatsFormat.h"
#include "Strings.h"
#include "UrColors.h"
#include "UrComponents.h"  // kit::MakeEmptyStateCard
#include "WalletSheets.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::UI::Xaml::Media;
using namespace urnw::pages;

namespace urnw {

// winrt::implements makes IInspectable a member typedef of every C++/WinRT
// implementation type, which is why MainWindow could name it unqualified. A
// plain class outside that hierarchy has to bring it in.
using winrt::Windows::Foundation::IInspectable;

namespace {

// A stat tile's value, in the colour its state deserves. The dash is a
// PLACEHOLDER, not a number, and it was rendering in the full text colour at
// 26pt condensed - three big bright dashes across the top of Wallet that read
// as "the answer is nothing" rather than "no answer yet". Faint for the
// placeholder, text colour for a real figure.
void SetStatValue(TextBlock const& value, hstring const& text, bool loaded) {
  if (!value) return;
  value.Text(text);
  value.Foreground(loaded ? urnw::colors::TextBrush() : urnw::colors::FaintBrush());
}


using ShapeEllipse = winrt::Microsoft::UI::Xaml::Shapes::Ellipse;
using ShapePolyline = winrt::Microsoft::UI::Xaml::Shapes::Polyline;

// The four account-point events the server emits (iOS AccountPointEvent).
constexpr const char* kEventPayout = "payout";
constexpr const char* kEventReferral = "payout_linked_account";
constexpr const char* kEventMultiplier = "payout_multiplier";
constexpr const char* kEventReliability = "payout_reliability";

// A country multiplier at or above this reads as a standout (iOS
// CountryMultiplierList.highlightThreshold).
constexpr double kMultiplierHighlight = 2.0;

TextBlock MakeText(hstring const& text, double fontSize, Brush const& brush = nullptr,
                   bool wrap = false) {
  TextBlock tb;
  tb.Text(text);
  tb.FontSize(fontSize);
  if (brush) tb.Foreground(brush);
  if (wrap) tb.TextWrapping(TextWrapping::Wrap);
  return tb;
}

TextBlock MakeValue(hstring const& text, double fontSize = 22, Brush const& brush = nullptr) {
  auto tb = MakeText(text, fontSize, brush ? brush : colors::TextBrush());
  tb.FontFamily(FontFamily(L"ms-appx:///Assets/Fonts/abcgravity_extra_condensed.otf#ABC "
                           L"Gravity Extra Condensed"));
  return tb;
}

ColumnDefinition StarColumn(double weight = 1) {
  ColumnDefinition col;
  col.Width(GridLengthHelper::FromValueAndType(weight, GridUnitType::Star));
  return col;
}

ColumnDefinition AutoColumn() {
  ColumnDefinition col;
  col.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Auto));
  return col;
}

// A style from App.xaml by key. The component kit is markup (see App.xaml); a
// code-built control joins it by wearing the same style, not by re-setting the
// same properties.
Style KitStyle(std::wstring_view key) {
  return Application::Current()
      .Resources()
      .Lookup(winrt::box_value(hstring{key}))
      .as<Style>();
}

// The payment's timestamp for ordering and display: completion when it
// completed, otherwise when it was created.
std::string PaymentTime(urnet::AccountPayment const& p) {
  if (p.complete_time && !p.complete_time->empty()) return *p.complete_time;
  if (p.create_time && !p.create_time->empty()) return *p.create_time;
  return {};
}

bool PaymentCompleted(urnet::AccountPayment const& p) {
  return p.completed && *p.completed;
}

// "1.2 GiB" from a MiB float (the leaderboard's unit; iOS formatMiB).
std::wstring FormatMiB(float mib) {
  const double bytes = static_cast<double>(mib) * 1024.0 * 1024.0;
  return urnw::Widen(urnw::FormatByteCountCompact(static_cast<int64_t>(bytes)));
}

// A polyline over `values`, scaled to `host` and normalised against `scaleMax`.
// Returns an empty (invisible) line when there is nothing to draw, so the
// caller never has to special-case a missing series.
ShapePolyline MakeSeries(winrt::Windows::UI::Color color, double thickness, bool dashed) {
  ShapePolyline line;
  line.Stroke(colors::MakeBrush(color));
  line.StrokeThickness(thickness);
  if (dashed) {
    auto dashes = line.StrokeDashArray();
    dashes.Append(5);
    dashes.Append(3);
  }
  return line;
}

void PlotSeries(ShapePolyline const& line, std::vector<double> const& values, double scaleMax,
                double width, double height) {
  auto points = line.Points();
  points.Clear();
  if (values.size() < 2 || width <= 0 || height <= 0 || scaleMax <= 0) return;
  const double step = width / static_cast<double>(values.size() - 1);
  for (size_t i = 0; i < values.size(); ++i) {
    const double y = height - (std::clamp(values[i] / scaleMax, 0.0, 1.0) * height);
    points.Append(winrt::Windows::Foundation::Point{static_cast<float>(i * step),
                                                    static_cast<float>(y)});
  }
}

// One legend entry: a coloured dot and the series name.
StackPanel LegendItem(winrt::Windows::UI::Color color, hstring const& name) {
  StackPanel item;
  item.Orientation(Orientation::Horizontal);
  item.Spacing(6);
  ShapeEllipse dot;
  dot.Width(8);
  dot.Height(8);
  dot.Fill(colors::MakeBrush(color));
  dot.VerticalAlignment(VerticalAlignment::Center);
  item.Children().Append(dot);
  item.Children().Append(MakeText(name, 12, colors::MutedBrush()));
  return item;
}

// The reliability window's three series over one canvas: the per-bucket
// reliability weight, the total client count, and the flat mean. Weights and
// clients are on independent scales - they are different quantities and a
// single axis would flatten whichever is smaller into the floor - which is why
// each carries its own legend entry rather than a shared y-axis label.
UIElement BuildReliabilityChart(std::vector<double> weights, std::vector<double> clients,
                                double mean) {
  Grid host;
  host.Height(110);

  Canvas canvas;
  host.Children().Append(canvas);

  auto meanLine = MakeSeries(colors::kTextMuted, 1.5, /*dashed=*/true);
  auto weightLine = MakeSeries(colors::kUrPink, 2.0, false);
  auto clientLine = MakeSeries(colors::kUrGreen, 2.0, false);
  canvas.Children().Append(meanLine);
  canvas.Children().Append(clientLine);
  canvas.Children().Append(weightLine);

  // A Canvas has no layout size of its own, so the series can only be plotted
  // once the host has been measured - and again on every resize, which a
  // resizable desktop window does have.
  //
  // The three lines are held WEAKLY. Holding them by value made the handler own
  // strong references to three Polylines that are descendants of the very Grid
  // raising the event: host -> canvas -> line -> handler -> line, a cycle
  // neither end of which can ever be collected. LoadWallet() rebuilds this
  // panel on every visit to the destination and after every wallet change, so
  // it was one leaked chart per load for the life of the process.
  auto weakMean = winrt::make_weak(meanLine);
  auto weakWeight = winrt::make_weak(weightLine);
  auto weakClient = winrt::make_weak(clientLine);
  host.SizeChanged([weights, clients, mean, weakMean, weakWeight, weakClient](
                       IInspectable const&, SizeChangedEventArgs const& args) {
    auto meanShape = weakMean.get();
    auto weightShape = weakWeight.get();
    auto clientShape = weakClient.get();
    if (!meanShape || !weightShape || !clientShape) return;  // the chart is gone
    const double width = args.NewSize().Width;
    const double height = args.NewSize().Height;
    double weightMax = mean;
    for (double v : weights) weightMax = (std::max)(weightMax, v);
    double clientMax = 0;
    for (double v : clients) clientMax = (std::max)(clientMax, v);
    std::vector<double> meanSeries(weights.size(), mean);
    PlotSeries(meanShape, meanSeries, weightMax, width, height);
    PlotSeries(weightShape, weights, weightMax, width, height);
    PlotSeries(clientShape, clients, clientMax, width, height);
  });

  return host;
}

// ---- --preview-ui sample data ----------------------------------------------
//
// WHY THIS EXISTS. --preview-ui deliberately makes no API call (Startup.h): with
// no token every request would be an unauthenticated hit on the production API,
// so every panel on this destination renders EMPTY. That is worth looking at and
// it is not the populated one - and a wallet card, a payouts table, a points
// breakdown, a reliability chart and a leaderboard row are exactly the things
// where "reads correct" has never been evidence on this project. Without this
// they could ship having never been drawn.
//
// So: with --preview-ui ALREADY on, URNETWORK_PREVIEW_SAMPLE=1 pushes obviously
// synthetic rows through the SAME Apply* functions the API path uses. Two gates
// and a warning in the log every time, because data on screen that did not come
// from the server is the one thing a screenshot cannot show you.
//
// THIS COMMENT USED TO SAY "no network", AND THAT WAS FALSE. The sample makes
// the cards and rows interactive, and interactive meant a real sheet with a
// real Remove behind it: two clicks put an authenticated-looking removeWallet
// on the wire from a build with no account. The gates keep the api at arm's
// length from the LOAD paths only; what actually holds the line is
// WalletPage::CanCallApi(), which every action on this destination now passes
// through. See its comment in WalletPage.h.
//
// The values are deliberately not plausible as anybody's account: the wallet
// addresses spell what they are.
constexpr const char* kSampleOwnNetworkId = "sample-network-self";
// The addresses spell what they are, and their LAST SIX characters differ per
// chain on purpose: every surface in this destination shows a wallet as
// MaskAddress's "***" + last six, so three samples ending in the same word made
// all three cards, all three ledger rows and both sheets read "***SAMPLE" -
// which is exactly the thing the masked address exists to tell apart.
constexpr const char* kSampleSolAddress = "SAMPLEwa11etADDRESSnotREALsolana00SOLSMP";
constexpr const char* kSampleMaticAddress = "0xSAMPLEwa11etADDRESSnotREALpolygonMATSMP";
constexpr const char* kSampleTaoAddress = "5SAMPLEwa11etADDRESSnotREALbittensorTAOSMP";

bool PreviewSample() {
  static const bool on = [] {
    size_t len = 0;
    char value[16]{};
    if (getenv_s(&len, value, sizeof(value), "URNETWORK_PREVIEW_SAMPLE") != 0 || len == 0) {
      return false;
    }
    const bool enabled = std::string_view(value) == "1";
    if (enabled) {
      urnw::LogWarn(
          "preview-sample: rendering SYNTHETIC wallet/leaderboard rows - none of "
          "this came from the api");
    }
    return enabled;
  }();
  return on;
}

std::vector<urnet::AccountWallet> SampleWallets() {
  urnet::AccountWallet sol;
  sol.wallet_id = "5a3e0000-0000-4000-8000-00000000501a";
  sol.blockchain = urnet::SOL;
  sol.wallet_address = kSampleSolAddress;
  sol.default_token_type = "USDC";
  sol.active = true;
  sol.has_seeker_token = true;

  urnet::AccountWallet matic;
  matic.wallet_id = "5a3e0000-0000-4000-8000-0000000a71c0";
  matic.blockchain = urnet::MATIC;
  matic.wallet_address = kSampleMaticAddress;
  matic.default_token_type = "USDC";
  matic.active = true;

  urnet::AccountWallet tao;
  tao.wallet_id = "5a3e0000-0000-4000-8000-00000000007a";
  tao.blockchain = urnet::TAO;
  tao.wallet_address = kSampleTaoAddress;
  tao.default_token_type = "USDC";
  tao.active = true;
  return {sol, matic, tao};
}

std::vector<urnet::AccountPayment> SamplePayments() {
  // The address travels WITH the payment. It used to be hardcoded to the solana
  // one for all three, so the polygon payout rendered under a solana address -
  // in the ledger, in the wallet sheet and in the payout detail. A sample whose
  // whole job is to show what a populated screen looks like must not show a
  // wrong one.
  auto make = [](const char* id, const char* wallet, const char* chain, const char* address,
                 double amount, const char* when, const char* tx, bool completed) {
    urnet::AccountPayment p;
    p.payment_id = id;
    p.wallet_id = wallet;
    p.blockchain = chain;
    p.token_type = "USDC";
    p.token_amount = amount;
    p.complete_time = completed ? std::optional<std::string>(when) : std::nullopt;
    p.create_time = when;
    p.completed = completed;
    p.wallet_address = address;
    if (tx) p.tx_hash = tx;
    p.payout_byte_count = 8'123'456'789;
    return p;
  };
  return {
      make("5a3e0000-0000-4000-8000-0000000000a1", "5a3e0000-0000-4000-8000-00000000501a",
           urnet::SOL, kSampleSolAddress, 0, "2026-08-02T00:00:00Z", nullptr, false),
      make("5a3e0000-0000-4000-8000-0000000000a2", "5a3e0000-0000-4000-8000-00000000501a",
           urnet::SOL, kSampleSolAddress, 12.34, "2026-07-26T00:00:00Z",
           "SAMPLEtxHASHnotREAL2222222222222222", true),
      make("5a3e0000-0000-4000-8000-0000000000a3", "5a3e0000-0000-4000-8000-0000000a71c0",
           urnet::MATIC, kSampleMaticAddress, 8.90, "2026-07-19T00:00:00Z",
           "0xSAMPLEtxHASHnotREAL33333333333333", true),
  };
}

std::vector<urnet::AccountPoint> SamplePoints() {
  auto make = [](const char* event, int64_t nanoPoints, const char* paymentId) {
    urnet::AccountPoint p;
    p.event = event;
    p.point_value = nanoPoints;
    p.account_payment_id = paymentId;
    return p;
  };
  // urnet::nanoPointsToPoints divides by 1e6, not 1e9. At 1e9 this sample drew
  // 36,955,000 net points against a $21 lifetime payout - a thousand times the
  // real scale, on the one screen whose entire job is to show what the real
  // scale looks like.
  const int64_t nano = 1'000'000;
  return {
      make(kEventPayout, 12'340 * nano, "5a3e0000-0000-4000-8000-0000000000a2"),
      make(kEventReferral, 2'100 * nano, "5a3e0000-0000-4000-8000-0000000000a2"),
      make(kEventReliability, 860 * nano, "5a3e0000-0000-4000-8000-0000000000a2"),
      make(kEventMultiplier, 12'340 * nano, "5a3e0000-0000-4000-8000-0000000000a2"),
      make(kEventPayout, 8'900 * nano, "5a3e0000-0000-4000-8000-0000000000a3"),
      make(kEventReliability, 415 * nano, "5a3e0000-0000-4000-8000-0000000000a3"),
  };
}

urnet::ReliabilityWindow SampleReliability() {
  urnet::ReliabilityWindow rw;
  rw.mean_reliability_weight = 0.62;
  rw.max_total_client_count = 18;
  rw.max_client_count = 11;
  rw.bucket_duration_seconds = 3600;
  std::vector<double> weights;
  std::vector<int64_t> totals;
  for (int i = 0; i < 24; ++i) {
    weights.push_back(0.35 + 0.45 * std::sin(i * 0.5) * std::sin(i * 0.5) + 0.05 * (i % 3));
    totals.push_back(4 + (i * 7) % 15);
  }
  rw.reliability_weights = weights;
  rw.total_client_counts = totals;
  rw.client_counts = totals;

  urnet::CountryMultiplierList countries;
  auto country = [](const char* name, const char* code, double multiplier) {
    urnet::CountryMultiplier cm;
    cm.country_location_id = std::string("sample-") + code;
    cm.country = name;
    cm.country_code = code;
    cm.reliability_multiplier = multiplier;
    return cm;
  };
  countries.push_back(country("Sample Republic", "SR", 3.25));
  countries.push_back(country("Sampleland", "SL", 2.00));
  countries.push_back(country("Samplia", "SA", 1.40));
  countries.push_back(country("Not Multiplied", "NM", 1.00));  // filtered out
  rw.country_multipliers = countries;
  return rw;
}

urnet::LeaderboardEarnersList SampleEarners() {
  auto make = [](const char* id, const char* name, float mib, bool isPublic,
                 bool profanity = false) {
    urnet::LeaderboardEarner e;
    e.network_id = id;
    e.network_name = name;
    e.net_mib_count = mib;
    e.is_public = isPublic;
    e.contains_profanity = profanity;
    return e;
  };
  return {
      make("sample-net-1", "sample-alpha", 4'194'304.0f, true),
      make("sample-net-2", "hidden-should-not-render", 2'097'152.0f, /*isPublic=*/false),
      make(kSampleOwnNetworkId, "sample-my-network", 786'432.0f, true),
      make("sample-net-4", "profane-should-not-render", 524'288.0f, true, /*profanity=*/true),
      make("sample-net-5", "sample-epsilon", 131'072.0f, true),
  };
}

}  // namespace

WalletPage::WalletPage(winrt::URnetwork::implementation::MainWindow& window)
    : w_(window),
      snackbar_(window.WalletInfo(), window.DispatcherQueue()),
      leaderboardSnackbar_(window.LeaderboardInfo(), window.DispatcherQueue()) {}

WalletPage::~WalletPage() {
  if (walletValidateTimer_) walletValidateTimer_.Stop();
  if (seekerFlow_.timer) seekerFlow_.timer.Stop();
  if (connectFlow_.timer) connectFlow_.timer.Stop();
  if (rankingFlow_.timer) rankingFlow_.timer.Stop();
}

// See the long note on the declaration (WalletPage.h): guarding the LOAD paths
// left every ACTION on this destination able to reach the api with no session.
bool WalletPage::CanCallApi() const {
  return !w_.previewUi() && Sdk().apiReady() && Sdk().IsLoggedIn();
}

void WalletPage::RefuseNoSession() {
  urnw::LogWarn("wallet: refusing an api call - no session (preview={})",
                w_.previewUi());
  Notify(Loc("please_login_to_urnetwork"), InfoBarSeverity::Error);
}

// The bar the user can SEE. Each destination's InfoBar lives inside that
// destination's own ScrollViewer, and the other one is Collapsed.
void WalletPage::Notify(hstring const& message, InfoBarSeverity severity) {
  if (w_.LeaderboardView().Visibility() == Visibility::Visible) {
    leaderboardSnackbar_.Show(message, severity);
    return;
  }
  snackbar_.Show(message, severity);
}

uint32_t WalletPage::BeginFlow(Flow& flow, int timeoutMs, std::function<void()> onTimeout) {
  const uint32_t generation = ++flow.generation;
  if (!flow.timer) {
    flow.timer = w_.DispatcherQueue().CreateTimer();
    flow.timer.IsRepeating(false);
  }
  flow.timer.Stop();
  flow.timer.Interval(std::chrono::milliseconds(timeoutMs));
  // Bumping the generation is what makes the give-up final: the real answer,
  // whenever it turns up, no longer matches and is dropped. Without that a
  // watchdog that has already told the user it failed could be contradicted
  // minutes later by a success that re-enables and reloads under them.
  flow.timer.Tick([weak = w_.get_weak(), &flow, generation,
                   onTimeout = std::move(onTimeout)](auto const&, auto const&) {
    auto self = weak.get();
    if (!self || flow.generation != generation) return;
    ++flow.generation;
    urnw::LogError("wallet: a request never answered - giving up on it");
    onTimeout();
  });
  flow.timer.Start();
  return generation;
}

bool WalletPage::SettleFlow(Flow& flow, uint32_t generation) {
  if (flow.generation != generation) return false;  // timed out, or superseded
  if (flow.timer) flow.timer.Stop();
  return true;
}

void WalletPage::Initialize() {
  // debounce the connect-wallet address validation while typing (apple parity):
  // each keystroke restarts the window, and only the pause validates.
  walletValidateTimer_ = w_.DispatcherQueue().CreateTimer();
  walletValidateTimer_.Interval(std::chrono::milliseconds(300));
  walletValidateTimer_.IsRepeating(false);
  walletValidateTimer_.Tick([weak = w_.get_weak()](auto const&, auto const&) {
    if (auto self = weak.get()) self->wallet().ValidateWalletAddress();
  });
}

void WalletPage::ApplyStrings() {
  // wallet
  w_.PlanHeading().Text(Loc("plan"));
  w_.UpgradeButton().Content(LocBox("upgrade_with_stripe"));
  w_.WalletUnpaidLabel().Text(Loc("unpaid_data_provided"));
  w_.WalletPendingLabel().Text(Loc("pending_payout"));
  w_.WalletReferralsLabel().Text(Loc("total_referrals"));
  w_.PayoutsThresholdText().Text(Loc("payouts_amount_threshold"));
  w_.PayoutWalletsHeading().Text(Loc("payout_wallets"));
  w_.WalletsEmptyText().Text(Loc("to_start_earning_connect_your_solana_wallet_to"));
  w_.WalletsEmptyNote().Text(Loc("these_wallets_are_not_affiliated_or_controlled"));
  w_.ConnectWalletHeading().Text(Loc("connect_a_wallet"));
  // supported chains, then the bittensor caveat: two store sentences rather than a
  // third near-duplicate of both
  w_.ConnectWalletChainsText().Text(
      hstring{urnw::Localized("connect_external_wallet_supported_chains") + L" " +
              urnw::Localized("bittensor_wallet_future_use")});
  w_.WalletAddressBox().PlaceholderText(Loc("enter_wallet_address"));
  w_.ConnectWalletButton().Content(LocBox("connect"));
  w_.AccountPointsHeading().Text(Loc("account_points"));
  w_.EarningMultipliersHeading().Text(Loc("earning_multipliers"));
  w_.VerifySeekerButton().Content(LocBox("verify_seeker_token_btn"));
  w_.NetworkReliabilityHeading().Text(Loc("site_app_network_reliability"));
  w_.PayoutsHeading().Text(Loc("payouts"));

  // Every panel starts in the state its fetch has not left yet. Without this
  // an unloaded destination is blank, which reads as "there is nothing" rather
  // than "nothing has been asked for".
  const hstring loading = Loc("loading");
  w_.WalletsStatusText().Text(loading);
  w_.AccountPointsStatusText().Text(loading);
  w_.ReliabilityStatusText().Text(loading);
  w_.PayoutsStatusText().Text(loading);
  const hstring dash{L"-"};
  SetStatValue(w_.WalletUnpaidValue(), dash, false);
  SetStatValue(w_.WalletPendingValue(), dash, false);
  SetStatValue(w_.WalletReferralsValue(), dash, false);
  ApplySeekerState();

  // leaderboard (its title comes from the NavigationView header)
  w_.LeaderboardRankLabel().Text(Loc("current_ranking"));
  w_.LeaderboardNetProvidedLabel().Text(Loc("net_provided"));
  w_.LeaderboardPublicLabel().Text(Loc("display_network_on_leaderboard"));
  SetStatValue(w_.LeaderboardRankValue(), dash, false);
  SetStatValue(w_.LeaderboardNetProvidedValue(), dash, false);
  w_.LeaderboardDescription().Text(Loc("leaderboard_description"));
  w_.LeaderboardStatusText().Text(loading);
}

// ---- wallet --------------------------------------------------------------

// Eight independent requests, each settling its own panel. They are NOT chained:
// a destination that blanks every list because one endpoint 500'd tells the user
// less than one that shows six panels and one failure.
void WalletPage::LoadWallet() {
  if (!Sdk().IsLoggedIn()) return;  // the caller's guard is not the only one
  auto queue = w_.DispatcherQueue();
  auto weak = w_.get_weak();

  if (auto jwt = Sdk().ParsedJwt(); jwt && jwt->NetworkId) ownNetworkId_ = *jwt->NetworkId;

  w_.WalletsStatusText().Text(Loc("loading"));
  w_.WalletsStatusText().Visibility(Visibility::Visible);
  Sdk().api().getAccountWallets(
      [queue, weak](std::optional<urnet::GetAccountWalletsResult> result,
                    std::optional<std::string> err) {
        const bool ok = result && result->wallets && !err;
        std::vector<urnet::AccountWallet> wallets;
        if (ok) wallets = *result->wallets;
        if (!ok) {
          urnw::LogError("wallet: getAccountWallets failed{}",
                         err ? (": " + *err) : std::string());
        }
        queue.TryEnqueue([weak, wallets = std::move(wallets), ok] {
          if (auto self = weak.get())
            self->wallet().ApplyWallets(wallets, ok ? Fetch::Ready : Fetch::Failed);
        });
      });

  Sdk().api().getPayoutWallet([queue, weak](std::optional<urnet::GetPayoutWalletIdResult> result,
                                            std::optional<std::string> err) {
    // A miss is not an error: an account with no payout wallet set answers with
    // no id. Only log the transport failure.
    if (err) urnw::LogError("wallet: getPayoutWallet failed: {}", *err);
    std::string walletId;
    if (result && result->wallet_id) walletId = *result->wallet_id;
    queue.TryEnqueue([weak, walletId] {
      if (auto self = weak.get()) self->wallet().ApplyPayoutWalletId(walletId);
    });
  });

  Sdk().api().getTransferStats([queue, weak](std::optional<urnet::TransferStatsResult> result,
                                             std::optional<std::string> err) {
    const bool ok = result && !err;
    const int64_t unpaid = ok ? result->unpaid_bytes_provided : 0;
    if (!ok) {
      urnw::LogError("wallet: getTransferStats failed{}", err ? (": " + *err) : std::string());
    }
    queue.TryEnqueue([weak, unpaid, ok] {
      if (auto self = weak.get()) self->wallet().ApplyTransferStats(unpaid, ok);
    });
  });

  Sdk().api().walletBalance([queue, weak](std::optional<urnet::WalletBalanceResult> result,
                                          std::optional<std::string> err) {
    const bool ok = result && result->wallet_info && !err;
    const int64_t nanoCents = ok ? result->wallet_info->balance_usdc_nano_cents : 0;
    if (!ok) {
      urnw::LogError("wallet: walletBalance failed{}", err ? (": " + *err) : std::string());
    }
    queue.TryEnqueue([weak, nanoCents, ok] {
      if (auto self = weak.get()) self->wallet().ApplyWalletBalance(nanoCents, ok);
    });
  });

  Sdk().api().getNetworkReferralCode(
      [queue, weak](std::optional<urnet::GetNetworkReferralCodeResult> result,
                    std::optional<std::string> err) {
        const bool ok = result && !err;
        const int64_t total = ok ? result->total_referrals : 0;
        queue.TryEnqueue([weak, total, ok] {
          if (auto self = weak.get()) self->wallet().ApplyReferrals(total, ok);
        });
      });

  w_.AccountPointsStatusText().Text(Loc("loading"));
  w_.AccountPointsStatusText().Visibility(Visibility::Visible);
  Sdk().api().getAccountPoints([queue, weak](std::optional<urnet::AccountPointsResult> result,
                                             std::optional<std::string> err) {
    const bool ok = result && !err;
    std::vector<urnet::AccountPoint> points;
    if (ok && result->network_points) points = *result->network_points;
    if (!ok) {
      urnw::LogError("wallet: getAccountPoints failed{}", err ? (": " + *err) : std::string());
    }
    queue.TryEnqueue([weak, points = std::move(points), ok] {
      if (auto self = weak.get())
        self->wallet().ApplyPoints(points, ok ? Fetch::Ready : Fetch::Failed);
    });
  });

  w_.ReliabilityStatusText().Text(Loc("loading"));
  w_.ReliabilityStatusText().Visibility(Visibility::Visible);
  Sdk().api().getNetworkReliability(
      [queue, weak](std::optional<urnet::GetNetworkReliabilityResult> result,
                    std::optional<std::string> err) {
        std::string error = err ? *err : std::string();
        if (error.empty() && result && result->error) error = result->error->message;
        const bool ok = result && error.empty();
        std::optional<urnet::ReliabilityWindow> window;
        if (ok && result->reliability_window) window = *result->reliability_window;
        if (!ok) urnw::LogError("wallet: getNetworkReliability failed: {}", error);
        queue.TryEnqueue([weak, window, ok] {
          if (auto self = weak.get())
            self->wallet().ApplyReliability(window, ok ? Fetch::Ready : Fetch::Failed);
        });
      });

  w_.PayoutsStatusText().Text(Loc("loading"));
  w_.PayoutsStatusText().Visibility(Visibility::Visible);
  Sdk().api().getAccountPayments(
      [queue, weak](std::optional<urnet::GetNetworkAccountPaymentsResult> result,
                    std::optional<std::string> err) {
        std::string error = err ? *err : std::string();
        if (error.empty() && result && result->error) error = result->error->message;
        const bool ok = result && error.empty();
        std::vector<urnet::AccountPayment> payments;
        if (ok && result->account_payments) payments = *result->account_payments;
        if (!ok) urnw::LogError("wallet: getAccountPayments failed: {}", error);
        queue.TryEnqueue([weak, payments = std::move(payments), ok] {
          if (auto self = weak.get())
            self->wallet().ApplyPayments(payments, ok ? Fetch::Ready : Fetch::Failed);
        });
      });
}

void WalletPage::RefreshAfterWalletChange() { LoadWallet(); }

void WalletPage::ApplyWallets(std::vector<urnet::AccountWallet> const& wallets, Fetch state) {
  wallets_ = wallets;
  seekerHolder_ = false;
  for (auto const& wallet : wallets_) {
    if (wallet.has_seeker_token) seekerHolder_ = true;
  }

  if (state == Fetch::Failed) {
    w_.WalletsStatusText().Text(Loc("something_went_wrong"));
    w_.WalletsStatusText().Visibility(Visibility::Visible);
    w_.WalletCardsScroller().Visibility(Visibility::Collapsed);
    w_.WalletsEmptyPanel().Visibility(Visibility::Collapsed);
    ApplySeekerState();
    return;
  }
  // The empty state is onboarding copy, not a status line: it says what
  // connecting a wallet is FOR, which "None" does not (iOS EmptyWalletsView).
  w_.WalletsStatusText().Visibility(Visibility::Collapsed);
  const bool empty = wallets_.empty();
  w_.WalletsEmptyPanel().Visibility(empty ? Visibility::Visible : Visibility::Collapsed);
  w_.WalletCardsScroller().Visibility(empty ? Visibility::Collapsed : Visibility::Visible);
  RebuildWalletCards();
  ApplySeekerState();
}

void WalletPage::ApplyPayoutWalletId(std::string const& walletId) {
  // Preserve the current selection when the server answers with no id (iOS
  // PayoutWalletViewModel): dropping the marker on a transient nil would make
  // the default wallet look unset.
  if (walletId.empty()) return;
  payoutWalletId_ = walletId;
  RebuildWalletCards();
}

void WalletPage::RebuildWalletCards() {
  w_.WalletCardsPanel().Children().Clear();
  for (auto const& wallet : wallets_) {
    w_.WalletCardsPanel().Children().Append(BuildWalletCard(wallet));
  }
}

UIElement WalletPage::BuildWalletCard(urnet::AccountWallet const& wallet) {
  const bool isPayout =
      wallet.wallet_id && !wallet.wallet_id->empty() && *wallet.wallet_id == payoutWalletId_;

  // A card the user can activate is a Button, not a Border with a Tapped
  // handler (App.xaml UrCardButtonStyle): the platform then gives it the tab
  // order, the focus rectangle, hover/press states and an automation peer.
  Button card;
  card.Style(KitStyle(L"UrCardButtonStyle"));
  card.Width(248);
  card.Height(132);
  card.HorizontalContentAlignment(HorizontalAlignment::Stretch);
  card.VerticalContentAlignment(VerticalAlignment::Stretch);

  Grid body;
  body.RowDefinitions().Append(RowDefinition());
  RowDefinition bottom;
  bottom.Height(GridLengthHelper::FromValueAndType(1, GridUnitType::Auto));
  body.RowDefinitions().Append(bottom);
  body.ColumnDefinitions().Append(AutoColumn());
  body.ColumnDefinitions().Append(StarColumn());

  auto icon = WalletIconElement(wallet.blockchain);
  icon.VerticalAlignment(VerticalAlignment::Top);
  Grid::SetRow(icon, 0);
  Grid::SetColumn(icon, 0);
  body.Children().Append(icon);

  StackPanel totals;
  totals.HorizontalAlignment(HorizontalAlignment::Right);
  auto tag = PayoutWalletTag(isPayout);
  tag.HorizontalAlignment(HorizontalAlignment::Right);
  totals.Children().Append(tag);
  auto paid = MakeValue(hstring{urnw::Format("amount_usdc",
                                             FormatUsdcAmount(TotalPaidToWallet(
                                                 wallet.wallet_id.value_or(std::string()))))},
                        24);
  paid.HorizontalAlignment(HorizontalAlignment::Right);
  totals.Children().Append(paid);
  auto caption = MakeText(Loc("total_payouts"), 12, colors::MutedBrush());
  caption.HorizontalAlignment(HorizontalAlignment::Right);
  totals.Children().Append(caption);
  Grid::SetRow(totals, 0);
  Grid::SetColumn(totals, 1);
  body.Children().Append(totals);

  Grid footer;
  footer.ColumnDefinitions().Append(StarColumn());
  footer.ColumnDefinitions().Append(AutoColumn());
  auto chain = MakeText(hstring{ChainDisplayName(wallet.blockchain)}, 12, colors::MutedBrush());
  chain.VerticalAlignment(VerticalAlignment::Center);
  Grid::SetColumn(chain, 0);
  footer.Children().Append(chain);
  auto address = MakeText(hstring{MaskAddress(wallet.wallet_address)}, 18, colors::TextBrush());
  address.FontFamily(FontFamily(L"ms-appx:///Assets/Fonts/pp_neue_bit_bold.ttf#PP NeueBit"));
  Grid::SetColumn(address, 1);
  footer.Children().Append(address);
  Grid::SetRow(footer, 1);
  Grid::SetColumnSpan(footer, 2);
  body.Children().Append(footer);

  // A Button whose Content is a Panel gets NO automatic name (6fdacf8, checked
  // against the UIA tree, and checked again here: all three cards came back as
  // `[Button] id='' name=''`). Name it after what the card IS - the chain and
  // the wallet it identifies - exactly as LocationRow names itself from its
  // label plus its one datum. The two children that name now carries are marked
  // Raw so a reader does not hear them twice; the payout total, its caption and
  // the DEFAULT chip stay in the content view, because those are data.
  namespace automation = winrt::Microsoft::UI::Xaml::Automation;
  automation::AutomationProperties::SetName(
      card, hstring{urnw::Format("wallet_provider", ChainDisplayName(wallet.blockchain)) +
                    L", " + MaskAddress(wallet.wallet_address)});
  automation::AutomationProperties::SetAccessibilityView(
      chain, automation::Peers::AccessibilityView::Raw);
  automation::AutomationProperties::SetAccessibilityView(
      address, automation::Peers::AccessibilityView::Raw);

  card.Content(body);
  card.Click([weak = w_.get_weak(), wallet](auto const&, auto const&) {
    if (auto self = weak.get()) self->wallet().ShowWalletDetail(wallet);
  });
  return card;
}

winrt::fire_and_forget WalletPage::ShowWalletDetail(urnet::AccountWallet wallet) {
  if (w_.sheetOpen()) co_return;  // only one ContentDialog can show at a time
  auto self = w_.get_strong();
  // The sheet is READABLE with no session (that is the point of the preview)
  // but not ACTABLE: its two buttons write. This is the flag that stops a
  // sample card's Remove reaching the api.
  const bool allowActions = CanCallApi();
  const bool isPayout =
      wallet.wallet_id && !wallet.wallet_id->empty() && *wallet.wallet_id == payoutWalletId_;

  // only the payments that landed in THIS wallet (iOS WalletView)
  std::vector<urnet::AccountPayment> mine;
  for (auto const& payment : payments_) {
    if (payment.wallet_id && wallet.wallet_id && *payment.wallet_id == *wallet.wallet_id) {
      mine.push_back(payment);
    }
  }

  auto weak = w_.get_weak();
  self->SetSheetOpen(true);
  try {
    walletSheet_ = urnw::WalletDetailSheet::Create(
        self->Content().XamlRoot(), Sdk(), wallet, isPayout, mine, allowActions,
        [weak] {
          if (auto w = weak.get()) w->wallet().RefreshAfterWalletChange();
        },
        [weak](hstring message) {
          // success only: the sheet is gone by the time this runs
          if (auto w = weak.get()) {
            w->wallet().Notify(message, InfoBarSeverity::Success);
          }
        });
    co_await self->wallet().walletSheet_->Dialog().ShowAsync();
  } catch (winrt::hresult_error const& e) {
    // Silence here is how a click that opens nothing stays a mystery.
    urnw::LogError("wallet: the wallet sheet failed to open: {}",
                   urnw::Narrow(std::wstring{e.message()}));
  } catch (...) {
    urnw::LogError("wallet: the wallet sheet failed to open");
  }
  self->wallet().walletSheet_.reset();
  self->SetSheetOpen(false);
}

// ---- header stats --------------------------------------------------------

void WalletPage::ApplyTransferStats(int64_t unpaidBytes, bool ok) {
  SetStatValue(w_.WalletUnpaidValue(),
               ok ? hstring{urnw::Widen(urnw::FormatByteCountCompact(unpaidBytes))}
                  : hstring{L"-"},
               ok);
}

void WalletPage::ApplyWalletBalance(int64_t balanceUsdcNanoCents, bool ok) {
  // The pending balance the payout threshold is measured against. The threshold
  // sentence itself sits under the card (payouts_amount_threshold): the server
  // does not report the figure, so naming a number here would be an invention.
  SetStatValue(
      w_.WalletPendingValue(),
      ok ? hstring{urnw::Format("amount_usdc",
                                FormatUsdcAmount(urnet::nanoCentsToUsd(balanceUsdcNanoCents)))}
         : hstring{L"-"},
      ok);
}

void WalletPage::ApplyReferrals(int64_t totalReferrals, bool ok) {
  SetStatValue(w_.WalletReferralsValue(),
               ok ? hstring{std::to_wstring(totalReferrals)} : hstring{L"-"}, ok);
}

// ---- account points ------------------------------------------------------

void WalletPage::ApplyPoints(std::vector<urnet::AccountPoint> const& points, Fetch state) {
  points_ = points;
  accountPoints_ = {};
  for (auto const& point : points_) {
    const double value = urnet::nanoPointsToPoints(point.point_value);
    accountPoints_.net += value;
    if (point.event == kEventPayout) accountPoints_.payout += value;
    else if (point.event == kEventReferral) accountPoints_.referral += value;
    else if (point.event == kEventMultiplier) accountPoints_.multiplier += value;
    else if (point.event == kEventReliability) accountPoints_.reliability += value;
  }

  if (state == Fetch::Failed) {
    w_.AccountPointsStatusText().Text(Loc("something_went_wrong"));
    w_.AccountPointsStatusText().Visibility(Visibility::Visible);
    w_.AccountPointsPanel().Children().Clear();
    w_.AccountPointsCard().Visibility(Visibility::Collapsed);
    return;
  }
  w_.AccountPointsStatusText().Visibility(Visibility::Collapsed);
  w_.AccountPointsCard().Visibility(Visibility::Visible);
  RebuildPointsCard();
  RebuildWalletCards();  // the per-wallet totals do not move, but the seeker row does
}

void WalletPage::RebuildPointsCard() {
  w_.AccountPointsPanel().Children().Clear();
  w_.AccountPointsPanel().Children().Append(
      urnw::BuildPointsBreakdown(accountPoints_, seekerHolder_));
}

PointsBreakdown WalletPage::BreakdownForPayment(std::string const& paymentId) const {
  PointsBreakdown out;
  if (paymentId.empty()) return out;
  for (auto const& point : points_) {
    if (!point.account_payment_id || *point.account_payment_id != paymentId) continue;
    const double value = urnet::nanoPointsToPoints(point.point_value);
    out.net += value;
    if (point.event == kEventPayout) out.payout += value;
    else if (point.event == kEventReferral) out.referral += value;
    else if (point.event == kEventMultiplier) out.multiplier += value;
    else if (point.event == kEventReliability) out.reliability += value;
  }
  return out;
}

double WalletPage::TotalPaidToWallet(std::string const& walletId) const {
  if (walletId.empty()) return 0;
  double total = 0;
  for (auto const& payment : payments_) {
    if (!PaymentCompleted(payment)) continue;
    if (!payment.wallet_id || *payment.wallet_id != walletId) continue;
    total += payment.token_amount.value_or(0.0);
  }
  return total;
}

// ---- payouts -------------------------------------------------------------

void WalletPage::ApplyPayments(std::vector<urnet::AccountPayment> const& payments, Fetch state) {
  payments_ = payments;
  // newest first; the SDK does not promise an order
  std::sort(payments_.begin(), payments_.end(),
            [](urnet::AccountPayment const& a, urnet::AccountPayment const& b) {
              return PaymentTime(a) > PaymentTime(b);
            });

  // RebuildWalletCards() on EVERY path, not just the happy one. The per-wallet
  // "total payouts" figure is computed from payments_, so a failed or empty
  // fetch that cleared the ledger while leaving the cards alone left last
  // load's totals on screen underneath a "Something went wrong" - stale numbers
  // presented as current, which is worse than no numbers.
  if (state == Fetch::Failed) {
    w_.PayoutsStatusText().Text(Loc("something_went_wrong"));
    w_.PayoutsStatusText().Visibility(Visibility::Visible);
    w_.PayoutsPanel().Children().Clear();
    RebuildWalletCards();
    return;
  }
  if (payments_.empty()) {
    w_.PayoutsStatusText().Visibility(Visibility::Collapsed);
    w_.PayoutsPanel().Children().Clear();
    w_.PayoutsPanel().Children().Append(
        urnw::kit::MakeEmptyStateCard(L"", Loc("site_app_no_payouts")));
    RebuildWalletCards();
    return;
  }
  w_.PayoutsStatusText().Visibility(Visibility::Collapsed);
  RebuildPayouts();
  RebuildWalletCards();  // per-wallet totals come from the payments
}

// The desktop advantage over the phone: the ledger is a table with column
// headers, not a stack of two-line cells. Every row is a Button so it is
// keyboard reachable and opens the payout detail.
void WalletPage::RebuildPayouts() {
  auto panel = w_.PayoutsPanel();
  panel.Children().Clear();

  auto columns = [](Grid const& grid) {
    grid.ColumnSpacing(12);
    grid.ColumnDefinitions().Append(StarColumn(2));  // date
    grid.ColumnDefinitions().Append(StarColumn(2));  // amount
    grid.ColumnDefinitions().Append(StarColumn(3));  // wallet
    grid.ColumnDefinitions().Append(StarColumn(3));  // transaction
  };

  Grid header;
  columns(header);
  header.Padding(Thickness{12, 4, 12, 4});
  const hstring headings[] = {Loc("payout"), Loc("amount"), Loc("site_app_wallet"),
                              Loc("transaction")};
  for (int i = 0; i < 4; ++i) {
    auto cell = MakeText(headings[i], 12, colors::MutedBrush());
    Grid::SetColumn(cell, i);
    header.Children().Append(cell);
  }
  panel.Children().Append(header);

  Border rule;
  rule.Height(1);
  rule.Background(colors::BorderBrush());
  panel.Children().Append(rule);

  for (auto const& payment : payments_) {
    // UrCardRowButtonStyle: the kit's row-sized card button, so each row is
    // keyboard reachable and gets the platform's hover/press states.
    Button row;
    row.Style(KitStyle(L"UrCardRowButtonStyle"));
    row.Margin(Thickness{0, 2, 0, 0});
    row.HorizontalContentAlignment(HorizontalAlignment::Stretch);

    Grid cells;
    columns(cells);
    const bool completed = PaymentCompleted(payment);

    auto date = MakeText(hstring{ShortDate(PaymentTime(payment))}, 13, colors::TextBrush());
    Grid::SetColumn(date, 0);
    cells.Children().Append(date);

    const hstring amount =
        completed ? hstring{urnw::Format("plus_amount_usdc",
                                         FormatUsdcAmount(payment.token_amount.value_or(0.0)))}
                  : Loc("pending_payout");
    auto amountCell = MakeText(amount, 13,
                               completed ? colors::MakeBrush(colors::kUrGreen)
                                         : colors::MutedBrush());
    Grid::SetColumn(amountCell, 1);
    cells.Children().Append(amountCell);

    auto wallet = MakeText(hstring{MaskAddress(payment.wallet_address)}, 13,
                           colors::MutedBrush());
    Grid::SetColumn(wallet, 2);
    cells.Children().Append(wallet);

    const std::string hash = payment.tx_hash.value_or(std::string());
    auto tx = MakeText(hash.empty() ? Loc("none") : hstring{MaskAddress(hash)}, 13,
                       colors::MutedBrush());
    Grid::SetColumn(tx, 3);
    cells.Children().Append(tx);

    // Same as the wallet cards: a Button over a Grid has no automatic name, so
    // all three ledger rows were unnamed buttons. The row is named after the
    // payout it opens - the same title its detail sheet carries, so the two
    // cannot drift - and the date cell alone goes Raw, because the name now
    // says it. Amount, wallet and transaction stay readable: they are data.
    //
    // By DATE for every row, pending included. Naming a pending row "Pending
    // payout" duplicated its own amount cell, which already says exactly that -
    // the double read this pass exists to avoid - and it left the row without
    // the one thing that tells it apart from the other pending rows.
    namespace automation = winrt::Microsoft::UI::Xaml::Automation;
    automation::AutomationProperties::SetName(
        row, hstring{urnw::Format("date_payout", ShortDate(PaymentTime(payment)))});
    automation::AutomationProperties::SetAccessibilityView(
        date, automation::Peers::AccessibilityView::Raw);

    row.Content(cells);
    row.Click([weak = w_.get_weak(), payment](auto const&, auto const&) {
      if (auto self = weak.get()) self->wallet().ShowPayoutDetail(payment);
    });
    panel.Children().Append(row);
  }
}

winrt::fire_and_forget WalletPage::ShowPayoutDetail(urnet::AccountPayment payment) {
  if (w_.sheetOpen()) co_return;
  auto self = w_.get_strong();
  const auto breakdown = BreakdownForPayment(payment.payment_id.value_or(std::string()));
  const bool seeker = seekerHolder_;
  self->SetSheetOpen(true);
  try {
    payoutSheet_ = urnw::PayoutDetailSheet::Create(self->Content().XamlRoot(), payment,
                                                   breakdown, seeker);
    co_await self->wallet().payoutSheet_->Dialog().ShowAsync();
  } catch (winrt::hresult_error const& e) {
    urnw::LogError("payout: the payout sheet failed to open: {}",
                   urnw::Narrow(std::wstring{e.message()}));
  } catch (...) {
    urnw::LogError("payout: the payout sheet failed to open");
  }
  self->wallet().payoutSheet_.reset();
  self->SetSheetOpen(false);
}

// ---- network reliability -------------------------------------------------

void WalletPage::ApplyReliability(std::optional<urnet::ReliabilityWindow> window, Fetch state) {
  reliability_ = window;
  auto panel = w_.ReliabilityPanel();
  panel.Children().Clear();
  // an empty card is a bar of nothing sitting under its own status line
  w_.ReliabilityCard().Visibility(Visibility::Collapsed);

  if (state == Fetch::Failed) {
    w_.ReliabilityStatusText().Text(Loc("something_went_wrong"));
    w_.ReliabilityStatusText().Visibility(Visibility::Visible);
    return;
  }
  if (!reliability_) {
    w_.ReliabilityStatusText().Text(Loc("site_app_no_reliability"));
    w_.ReliabilityStatusText().Visibility(Visibility::Visible);
    return;
  }
  w_.ReliabilityStatusText().Visibility(Visibility::Collapsed);
  w_.ReliabilityCard().Visibility(Visibility::Visible);
  auto const& rw = *reliability_;

  // the two figures the window is summarised by
  Grid stats;
  stats.ColumnSpacing(24);
  stats.ColumnDefinitions().Append(StarColumn());
  stats.ColumnDefinitions().Append(StarColumn());
  wchar_t meanBuf[32];
  std::swprintf(meanBuf, std::size(meanBuf), L"%.2f", rw.mean_reliability_weight);
  StackPanel meanCell;
  meanCell.Children().Append(MakeText(Loc("average_reliability"), 12, colors::MutedBrush()));
  meanCell.Children().Append(MakeValue(hstring{meanBuf}));
  Grid::SetColumn(meanCell, 0);
  stats.Children().Append(meanCell);
  StackPanel clientCell;
  clientCell.Children().Append(MakeText(Loc("total_clients"), 12, colors::MutedBrush()));
  clientCell.Children().Append(
      MakeValue(hstring{std::to_wstring(rw.max_total_client_count)}));
  Grid::SetColumn(clientCell, 1);
  stats.Children().Append(clientCell);
  panel.Children().Append(stats);

  std::vector<double> weights;
  if (rw.reliability_weights) weights = *rw.reliability_weights;
  std::vector<double> clients;
  if (rw.total_client_counts) {
    for (int64_t v : *rw.total_client_counts) clients.push_back(static_cast<double>(v));
  }
  if (weights.size() >= 2 || clients.size() >= 2) {
    panel.Children().Append(BuildReliabilityChart(weights, clients, rw.mean_reliability_weight));
    StackPanel legend;
    legend.Orientation(Orientation::Horizontal);
    legend.Spacing(16);
    legend.Children().Append(LegendItem(colors::kUrPink, Loc("reliability_weight")));
    legend.Children().Append(LegendItem(colors::kUrGreen, Loc("total_clients")));
    legend.Children().Append(LegendItem(colors::kTextMuted, Loc("average_reliability_2")));
    panel.Children().Append(legend);
  }

  // Country multipliers, above 1.0 only (iOS NetworkReliabilityView): a
  // multiplier of exactly 1 is "no multiplier" and would pad the table with
  // rows that say nothing.
  std::vector<urnet::CountryMultiplier> multipliers;
  if (rw.country_multipliers) {
    for (auto const& cm : *rw.country_multipliers) {
      if (cm.reliability_multiplier > 1.0) multipliers.push_back(cm);
    }
  }
  if (multipliers.empty()) return;
  std::sort(multipliers.begin(), multipliers.end(),
            [](urnet::CountryMultiplier const& a, urnet::CountryMultiplier const& b) {
              return a.reliability_multiplier > b.reliability_multiplier;
            });

  Border rule;
  rule.Height(1);
  rule.Background(colors::BorderBrush());
  rule.Margin(Thickness{0, 4, 0, 4});
  panel.Children().Append(rule);
  panel.Children().Append(MakeText(Loc("country_multipliers"), 15, colors::TextBrush()));

  Grid head;
  head.ColumnDefinitions().Append(StarColumn());
  head.ColumnDefinitions().Append(AutoColumn());
  auto countryHead = MakeText(Loc("country"), 12, colors::MutedBrush());
  Grid::SetColumn(countryHead, 0);
  head.Children().Append(countryHead);
  auto multiplierHead = MakeText(Loc("multiplier"), 12, colors::MutedBrush());
  Grid::SetColumn(multiplierHead, 1);
  head.Children().Append(multiplierHead);
  panel.Children().Append(head);

  for (auto const& cm : multipliers) {
    const bool highlight = cm.reliability_multiplier >= kMultiplierHighlight;
    auto brush = highlight ? colors::MakeBrush(colors::kUrGreen) : colors::TextBrush();
    Grid row;
    row.ColumnDefinitions().Append(StarColumn());
    row.ColumnDefinitions().Append(AutoColumn());
    auto country = MakeText(hstring{urnw::Widen(cm.country)}, 13, brush);
    Grid::SetColumn(country, 0);
    row.Children().Append(country);
    wchar_t buf[32];
    std::swprintf(buf, std::size(buf), L"x%.2f", cm.reliability_multiplier);
    auto value = MakeText(hstring{buf}, 13, brush);
    Grid::SetColumn(value, 1);
    row.Children().Append(value);
    panel.Children().Append(row);
  }
}

// ---- earning multiplier (Seeker) -----------------------------------------

void WalletPage::ApplySeekerState() {
  if (seekerHolder_) {
    w_.SeekerStatusText().Text(
        hstring{urnw::Localized("seeker_token_verified") + L" " +
                urnw::Localized("you_re_earning_2x_points")});
    w_.VerifySeekerButton().Visibility(Visibility::Collapsed);
    return;
  }
  // "Waiting" is a state the user must be able to SEE. The button greyed itself
  // out for the length of the bridge round trip and said nothing, which is
  // indistinguishable from broken - and, before the watchdog below, it was
  // permanent whenever the browser tab was simply closed.
  w_.SeekerStatusText().Text(verifyingSeeker_ ? Loc("opening_wallet_in_browser")
                                              : Loc("connect_seeker_wallet"));
  w_.VerifySeekerButton().Visibility(Visibility::Visible);
  w_.VerifySeekerButton().IsEnabled(!verifyingSeeker_);
}

// Claim the 2x multiplier by proving the wallet holds the Seeker / Saga token
// (android SettingsScreen.signAndVerifySeekerHolder). The wallet signs a
// timestamped challenge through the ur.io/wallet-connect browser bridge and the
// signed triple goes to Api.verifySeekerHolder â€” the address alone proves
// nothing, so there is no shortcut past the signature.
//
// android puts up its wallet picker through Mobile Wallet Adapter; the browser
// bridge needs the provider baked into the url it opens, so the picker is a
// dialog here, exactly as the Solana sign-in does it (LoginPage).
winrt::fire_and_forget WalletPage::OnVerifySeeker(IInspectable const&, RoutedEventArgs const&) {
  if (w_.sheetOpen() || verifyingSeeker_) co_return;
  // Before the wallet picker, not after: with no session this ends in
  // verifySeekerHolder, and it opens a BROWSER on the way there.
  if (!CanCallApi()) {
    RefuseNoSession();
    co_return;
  }
  auto self = w_.get_strong();

  ContentDialog dialog;
  dialog.XamlRoot(self->Content().XamlRoot());
  dialog.Title(winrt::box_value(Loc("confirm_seeker_token")));
  dialog.Content(winrt::box_value(Loc("connect_seeker_wallet")));
  dialog.PrimaryButtonText(Loc("phantom"));
  dialog.SecondaryButtonText(Loc("solflare"));
  dialog.CloseButtonText(Loc("cancel"));
  dialog.DefaultButton(ContentDialogButton::Primary);
  dialog.Background(colors::SheetBrush());

  self->SetSheetOpen(true);
  ContentDialogResult result{ContentDialogResult::None};
  try {
    result = co_await dialog.ShowAsync();
  } catch (...) {
  }
  self->SetSheetOpen(false);
  if (result == ContentDialogResult::None) co_return;

  const auto provider = (result == ContentDialogResult::Secondary)
                            ? urnw::WalletConnect::Provider::Solflare
                            : urnw::WalletConnect::Provider::Phantom;

  self->wallet().verifyingSeeker_ = true;
  self->wallet().ApplySeekerState();

  // WalletConnect has no timeout, and its on_error only fires when the deep
  // link comes BACK carrying an error. A closed browser tab produces nothing at
  // all - so this flag stayed true forever and Verify Seeker was dead until the
  // app was restarted, with nothing on screen to say why.
  const uint32_t generation = self->wallet().BeginFlow(
      self->wallet().seekerFlow_, kBridgeTimeoutMs, [weak = self->get_weak()] {
        if (auto w = weak.get()) {
          w->wallet().verifyingSeeker_ = false;
          w->wallet().ApplySeekerState();
          w->wallet().Notify(Loc("error_claiming_multiplier"), InfoBarSeverity::Error);
        }
      });

  // android's challenge shape, timestamp and all: a fixed string would be
  // replayable
  const std::string message =
      "Verify Seeker Token Holder - " +
      std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count());

  auto queue = self->DispatcherQueue();
  auto weak = self->get_weak();
  Sdk().SignWithSolanaWallet(
      provider, message,
      [queue, weak, message, generation](bool ok, std::string address, std::string signature,
                                         std::string error) {
        if (!ok) {
          urnw::LogError("seeker: wallet signature failed: {}", error);
          queue.TryEnqueue([weak, error, generation] {
            if (auto w = weak.get()) w->wallet().ApplySeekerResult(generation, false, error);
          });
          return;
        }
        urnet::VerifySeekerNftHolderArgs args;
        args.wallet_address = address;
        args.wallet_signature = signature;
        args.wallet_message = message;
        Sdk().api().verifySeekerHolder(
            args, [queue, weak, generation](
                      std::optional<urnet::VerifySeekerNftHolderResult> result,
                      std::optional<std::string> err) {
              std::string failure = err ? *err : std::string();
              if (failure.empty() && result && result->error) failure = result->error->message;
              const bool verified = result && result->success && failure.empty();
              if (!verified) urnw::LogError("seeker: verifySeekerHolder failed: {}", failure);
              queue.TryEnqueue([weak, verified, failure, generation] {
                if (auto w = weak.get())
                  w->wallet().ApplySeekerResult(generation, verified, failure);
              });
            });
      });
}

void WalletPage::ApplySeekerResult(uint32_t generation, bool ok,
                                   std::string const& serverError) {
  // The watchdog already gave up on this one and said so: do not now contradict
  // it by reporting the outcome of a request the user was told had failed.
  if (!SettleFlow(seekerFlow_, generation)) {
    urnw::LogWarn("seeker: dropping a result for an abandoned verification (ok={})", ok);
    return;
  }
  verifyingSeeker_ = false;
  Notify(ok ? Loc("successfully_claimed_multiplier")
            : (serverError.empty()
                   ? Loc("error_claiming_multiplier")
                   : hstring{urnw::Format("error_claiming_multiplier_with_reason",
                                          urnw::Widen(serverError))}),
         ok ? InfoBarSeverity::Success : InfoBarSeverity::Error);
  if (ok) {
    LoadWallet();  // has_seeker_token now reads true on the verified wallet
    return;
  }
  ApplySeekerState();
}

// ---- connect wallet (external wallet, by address) -------------------------
// Paste an address; the server validates it per chain and the first chain that
// accepts it wins. Bittensor connects by address only (no signature), matching
// apple/android â€” the signed bridge flow is sign-in, not wallet connect.

void WalletPage::OnWalletAddressChanged(IInspectable const&, TextChangedEventArgs const&) {
  walletValidation_ = {};
  walletChain_.clear();
  ++walletValidateGeneration_;  // drop any validation still in flight
  w_.ConnectWalletButton().IsEnabled(false);
  w_.WalletChainText().Text(L"");
  if (walletValidateTimer_) {
    walletValidateTimer_.Stop();  // restart the debounce window on every keystroke
    walletValidateTimer_.Start();
  }
}

void WalletPage::ValidateWalletAddress() {
  const std::string address = urnw::Narrow(w_.WalletAddressBox().Text().c_str());
  // the shortest supported address (solana base58) is 32 characters
  //
  // apiReady() was the only gate here and it is not a session check - it is
  // api_.has_value(), true from SDK init - so typing 32 characters under
  // --preview-ui put THREE walletValidateAddress calls on the wire, one per
  // chain, with no token. This is a question asked of the server; it goes
  // through the same guard as every other one. Silently: the user did not ask
  // for anything, they typed.
  if (address.size() < 32 || !CanCallApi()) return;
  const uint32_t generation = ++walletValidateGeneration_;

  auto queue = w_.DispatcherQueue();
  auto weak = w_.get_weak();
  const std::string chains[] = {urnet::SOL, urnet::MATIC, urnet::TAO};
  for (std::string const& chain : chains) {
    urnet::WalletValidateAddressArgs args;
    args.address = address;
    args.chain = chain;
    Sdk().api().walletValidateAddress(
        args, [queue, weak, chain, generation](
                  std::optional<urnet::WalletValidateAddressResult> result,
                  std::optional<std::string> err) {
          // The error was DISCARDED here, and it is the only difference between
          // "the server says this is not a $chain address" and "the question
          // never got an answer". Both drew an empty verdict line and a dead
          // Connect button, so a validator that was failing outright looked
          // exactly like a user typo. Found the hard way: three genuinely valid
          // addresses in a row came back with nothing to say.
          if (err) {
            urnw::LogError("wallet: walletValidateAddress({}) failed: {}", chain, *err);
          }
          const bool valid = result && result->valid && *result->valid;
          queue.TryEnqueue([weak, chain, generation, valid] {
            if (auto self = weak.get())
              self->wallet().ApplyWalletValidation(chain, generation, valid);
          });
        });
  }
}

void WalletPage::ApplyWalletValidation(std::string const& chain, uint32_t generation,
                                       bool valid) {
  if (generation != walletValidateGeneration_) return;  // a later edit superseded this
  if (chain == urnet::SOL) walletValidation_.sol = valid;
  else if (chain == urnet::MATIC) walletValidation_.matic = valid;
  else if (chain == urnet::TAO) walletValidation_.tao = valid;

  if (walletValidation_.sol) walletChain_ = urnet::SOL;
  else if (walletValidation_.matic) walletChain_ = urnet::MATIC;
  else if (walletValidation_.tao) walletChain_ = urnet::TAO;
  else walletChain_.clear();

  w_.ConnectWalletButton().IsEnabled(!walletChain_.empty() && !connectingWallet_);
  if (walletChain_ == urnet::TAO) {
    w_.WalletChainText().Text(Loc("bittensor_wallet_future_use"));
  } else if (!walletChain_.empty()) {
    w_.WalletChainText().Text(
        hstring{urnw::Format("wallet_provider_lower", ChainDisplayName(walletChain_))});
  } else {
    w_.WalletChainText().Text(L"");
  }
}

void WalletPage::OnConnectWallet(IInspectable const&, RoutedEventArgs const&) {
  const std::string address = urnw::Narrow(w_.WalletAddressBox().Text().c_str());
  if (address.empty() || walletChain_.empty() || connectingWallet_) return;
  if (!CanCallApi()) {
    RefuseNoSession();
    return;
  }
  connectingWallet_ = true;
  w_.ConnectWalletButton().IsEnabled(false);

  // createAccountWallet with no answer left connectingWallet_ true forever and
  // the Connect button greyed out for the life of the process.
  const uint32_t generation =
      BeginFlow(connectFlow_, kApiTimeoutMs, [weak = w_.get_weak()] {
        if (auto self = weak.get()) {
          self->wallet().connectingWallet_ = false;
          self->wallet().w_.ConnectWalletButton().IsEnabled(
              !self->wallet().walletChain_.empty());
          self->wallet().Notify(Loc("wallet_connect_failed"), InfoBarSeverity::Error);
        }
      });

  // WalletViewController::addExternalWallet parity: the account wallet is created
  // on the chain the server validated, with the USDC token type.
  urnet::CreateAccountWalletArgs args;
  args.blockchain = walletChain_;
  args.wallet_address = address;
  args.default_token_type = "USDC";

  auto queue = w_.DispatcherQueue();
  auto weak = w_.get_weak();
  Sdk().api().createAccountWallet(
      args, [queue, weak, generation](std::optional<urnet::CreateAccountWalletResult> result,
                                      std::optional<std::string> err) {
        const bool ok = result && result->wallet_id && !result->wallet_id->empty();
        // this runs on an sdk thread; hand the raw outcome to the ui thread and
        // let it do the lookup (the store is read from the ui thread throughout)
        const std::string error = ok ? std::string() : (err ? *err : std::string());
        queue.TryEnqueue([weak, ok, error, generation] {
          if (auto self = weak.get())
            self->wallet().ApplyWalletConnectResult(generation, ok, error);
        });
      });
}

void WalletPage::ApplyWalletConnectResult(uint32_t generation, bool ok,
                                          std::string const& serverError) {
  if (!SettleFlow(connectFlow_, generation)) {
    urnw::LogWarn("wallet: dropping a connect result for an abandoned request (ok={})", ok);
    return;
  }
  connectingWallet_ = false;
  // a server error is not localizable; show it when there is one
  Notify(ok ? Loc("wallet_connected")
            : (serverError.empty() ? Loc("wallet_connect_failed") : H(serverError)),
         ok ? InfoBarSeverity::Success : InfoBarSeverity::Error);
  if (!ok) {
    w_.ConnectWalletButton().IsEnabled(!walletChain_.empty());
    return;
  }
  w_.WalletAddressBox().Text(L"");  // clears the verdict through OnWalletAddressChanged
  LoadWallet();                     // refresh the list with the new wallet
}

void WalletPage::ShowPreviewSnackbar() {
  snackbar_.Show(Loc("wallet_connect_failed"), InfoBarSeverity::Error);
}

// --preview-ui: with no session the API loads are skipped, so every panel would
// otherwise sit on "Loading..." forever - which is exactly what a hang looks
// like. Settle them all on their empty state instead.
void WalletPage::ShowPreviewWalletState() {
  if (PreviewSample()) {
    ApplyWallets(SampleWallets(), Fetch::Ready);
    ApplyPayoutWalletId("5a3e0000-0000-4000-8000-00000000501a");
    ApplyTransferStats(41'237'481'984, /*ok=*/true);
    ApplyWalletBalance(1'234'500'000'000, /*ok=*/true);
    ApplyReferrals(7, /*ok=*/true);
    ApplyPayments(SamplePayments(), Fetch::Ready);
    ApplyPoints(SamplePoints(), Fetch::Ready);
    ApplyReliability(SampleReliability(), Fetch::Ready);
    return;
  }
  ApplyWallets({}, Fetch::Ready);
  ApplyTransferStats(0, /*ok=*/false);
  ApplyWalletBalance(0, /*ok=*/false);
  ApplyReferrals(0, /*ok=*/false);
  ApplyPoints({}, Fetch::Ready);
  ApplyPayments({}, Fetch::Ready);
  ApplyReliability(std::nullopt, Fetch::Ready);
}

// ---- leaderboard ---------------------------------------------------------

void WalletPage::ShowPreviewLeaderboardState() {
  if (PreviewSample()) {
    ownNetworkId_ = kSampleOwnNetworkId;
    urnet::NetworkRanking ranking;
    ranking.leaderboard_rank = 42;
    ranking.net_mib_count = 786432.0f;  // 768 GiB
    ranking.leaderboard_public = true;
    ApplyRanking(ranking, /*ok=*/true);
    ApplyLeaderboard(SampleEarners(), Fetch::Ready);
    return;
  }
  ApplyRanking({}, /*ok=*/false);
  ApplyLeaderboard({}, Fetch::Ready);
}

// The panel used to draw one heading and nothing else whether the fetch was in
// flight, had come back empty, or had failed - and a failure was silent in the
// log too, so there was no way at all to tell "this screen is broken" from
// "there is no data". All three now say which they are.
void WalletPage::ApplyLeaderboard(urnet::LeaderboardEarnersList const& earners, Fetch state) {
  auto rows = w_.LeaderboardRows();
  rows.Children().Clear();
  if (state == Fetch::Failed) {
    w_.LeaderboardStatusText().Text(Loc("something_went_wrong"));
    w_.LeaderboardStatusText().Visibility(Visibility::Visible);
    return;
  }
  if (earners.empty()) {
    // A glyph and a sentence on a card, not a lone grey line on the page. This
    // destination measured 60% blank rows: below the header card there was one
    // muted sentence and ~500px of nothing, which reads as a screen that failed
    // to draw rather than a leaderboard with no entries yet.
    w_.LeaderboardStatusText().Visibility(Visibility::Collapsed);
    rows.Children().Append(
        urnw::kit::MakeEmptyStateCard(L"", Loc("site_app_leaderboard_empty")));
    return;
  }
  w_.LeaderboardStatusText().Visibility(Visibility::Collapsed);

  int rank = 0;
  for (auto const& earner : earners) {
    ++rank;
    const bool isOwn = !ownNetworkId_.empty() && earner.network_id == ownNetworkId_;
    // A network that has not opted in is on the board by its numbers only; the
    // name is never rendered. Profanity is masked the same way - the server
    // flags it and the client is what decides not to draw it.
    const bool masked = !isOwn && (!earner.is_public || earner.contains_profanity);

    Border rule;
    rule.Height(1);
    rule.Background(colors::BorderBrush());
    rows.Children().Append(rule);

    Grid row;
    row.ColumnSpacing(12);
    row.Padding(Thickness{0, 8, 0, 8});
    row.ColumnDefinitions().Append(AutoColumn());
    row.ColumnDefinitions().Append(StarColumn());
    row.ColumnDefinitions().Append(AutoColumn());

    auto brush = isOwn ? colors::MakeBrush(colors::kUrGreen)
                       : (masked ? colors::MutedBrush() : colors::TextBrush());

    auto rankCell = MakeText(hstring{L"#" + std::to_wstring(rank)}, 13,
                             isOwn ? brush : colors::MutedBrush());
    rankCell.MinWidth(42);
    Grid::SetColumn(rankCell, 0);
    row.Children().Append(rankCell);

    auto name = MakeText(masked ? Loc("private_network")
                                : hstring{urnw::Widen(earner.network_name)},
                         13, brush);
    Grid::SetColumn(name, 1);
    row.Children().Append(name);

    auto provided = MakeText(hstring{FormatMiB(earner.net_mib_count)}, 13, brush);
    Grid::SetColumn(provided, 2);
    row.Children().Append(provided);

    rows.Children().Append(row);
  }
}

void WalletPage::ApplyRanking(urnet::NetworkRanking const& ranking, bool ok) {
  if (!ok) {
    // The card keeps its "-" placeholders; the list's own status line carries
    // the failure, and two failure messages for one screen is noise.
    SetStatValue(w_.LeaderboardRankValue(), L"-", false);
    SetStatValue(w_.LeaderboardNetProvidedValue(), L"-", false);
    return;
  }
  leaderboardRank_ = ranking.leaderboard_rank;
  const bool ranked = ranking.leaderboard_rank > 0;
  SetStatValue(w_.LeaderboardRankValue(),
               ranked ? hstring{L"#" + std::to_wstring(ranking.leaderboard_rank)}
                      : hstring{L"-"},
               ranked);
  SetStatValue(w_.LeaderboardNetProvidedValue(), hstring{FormatMiB(ranking.net_mib_count)},
               true);
  rankingPublic_ = ranking.leaderboard_public;
  SetRankingToggle(rankingPublic_);
}

void WalletPage::SetRankingToggle(bool isPublic) {
  applyingRankingToggle_ = true;
  w_.LeaderboardPublicToggle().IsOn(isPublic);
  applyingRankingToggle_ = false;
}

void WalletPage::LoadLeaderboard() {
  if (!Sdk().IsLoggedIn()) return;  // the caller's guard is not the only one
  w_.LeaderboardStatusText().Text(Loc("loading"));
  w_.LeaderboardStatusText().Visibility(Visibility::Visible);

  if (auto jwt = Sdk().ParsedJwt(); jwt && jwt->NetworkId) ownNetworkId_ = *jwt->NetworkId;

  // [queue, weak], like every other SDK callback in this file. This one used to
  // capture a raw `this` and call w_.DispatcherQueue() from the SDK thread -
  // the only two places in the split that did (the other is OnSendFeedback).
  auto queue = w_.DispatcherQueue();
  auto weak = w_.get_weak();

  Sdk().api().getNetworkLeaderboardRanking(
      [queue, weak](std::optional<urnet::GetNetworkRankingResult> result,
                    std::optional<std::string> err) {
        std::string error = err ? *err : std::string();
        if (error.empty() && result && result->error) error = result->error->message;
        const bool ok = result && result->network_ranking && error.empty();
        urnet::NetworkRanking ranking;
        if (ok) ranking = *result->network_ranking;
        if (!ok) urnw::LogError("leaderboard: ranking fetch failed: {}", error);
        queue.TryEnqueue([weak, ranking, ok] {
          if (auto self = weak.get()) self->wallet().ApplyRanking(ranking, ok);
        });
      });

  urnet::GetLeaderboardArgs args;
  Sdk().api().getLeaderboard(
      args, [queue, weak](std::optional<urnet::LeaderboardResult> result,
                          std::optional<std::string> err) {
        const bool ok = result && result->earners && !err;
        urnet::LeaderboardEarnersList earners;
        if (ok) earners = *result->earners;
        if (!ok) {
          urnw::LogError("leaderboard: fetch failed{}",
                         err ? (": " + *err) : std::string());
        }
        queue.TryEnqueue([weak, earners = std::move(earners), ok] {
          if (auto self = weak.get())
            self->wallet().ApplyLeaderboard(earners, ok ? Fetch::Ready : Fetch::Failed);
        });
      });
}

void WalletPage::OnLeaderboardPublicToggled(IInspectable const&, RoutedEventArgs const&) {
  // The handler cannot tell a user flip from the programmatic write that
  // renders the server's answer, so the write sets a flag and this returns.
  if (applyingRankingToggle_) return;
  const bool requested = w_.LeaderboardPublicToggle().IsOn();
  if (requested == rankingPublic_) return;
  if (settingRankingPublic_) {
    SetRankingToggle(rankingPublic_);  // one in flight: snap back
    return;
  }
  // This switch is live under a plain `--preview-ui=leaderboard`, with no env
  // var and no sample data in sight: one click used to be one
  // setNetworkLeaderboardPublic at the api. Put the switch back where the
  // server has it and say why.
  if (!CanCallApi()) {
    SetRankingToggle(rankingPublic_);
    RefuseNoSession();
    return;
  }
  settingRankingPublic_ = true;
  w_.LeaderboardPublicToggle().IsEnabled(false);

  const uint32_t generation =
      BeginFlow(rankingFlow_, kApiTimeoutMs, [weak = w_.get_weak()] {
        if (auto self = weak.get()) {
          self->wallet().settingRankingPublic_ = false;
          self->LeaderboardPublicToggle().IsEnabled(true);
          self->wallet().SetRankingToggle(self->wallet().rankingPublic_);
          self->wallet().Notify(Loc("something_went_wrong"), InfoBarSeverity::Error);
        }
      });

  urnet::SetNetworkRankingPublicArgs args;
  args.is_public = requested;
  auto queue = w_.DispatcherQueue();
  auto weak = w_.get_weak();
  Sdk().api().setNetworkLeaderboardPublic(
      args, [queue, weak, requested,
             generation](std::optional<urnet::SetNetworkRankingPublicResult> result,
                         std::optional<std::string> err) {
        std::string error = err ? *err : std::string();
        if (error.empty() && result && result->error) error = result->error->message;
        const bool ok = result && error.empty();
        if (!ok) urnw::LogError("leaderboard: setNetworkLeaderboardPublic failed: {}", error);
        queue.TryEnqueue([weak, ok, requested, error, generation] {
          if (auto self = weak.get())
            self->wallet().ApplyRankingPublicResult(generation, ok, requested, error);
        });
      });
}

void WalletPage::ApplyRankingPublicResult(uint32_t generation, bool ok, bool requested,
                                          std::string const& serverError) {
  if (!SettleFlow(rankingFlow_, generation)) {
    urnw::LogWarn("leaderboard: dropping a ranking result for an abandoned request (ok={})",
                  ok);
    return;
  }
  settingRankingPublic_ = false;
  w_.LeaderboardPublicToggle().IsEnabled(true);
  if (!ok) {
    // The switch must not keep showing a state the server refused - and the
    // message has to land on the LEADERBOARD's bar, which is the one on screen.
    SetRankingToggle(rankingPublic_);
    Notify(serverError.empty() ? Loc("something_went_wrong") : H(serverError),
           InfoBarSeverity::Error);
    return;
  }
  rankingPublic_ = requested;
  LoadLeaderboard();  // the board itself changes: our row masks or unmasks
}

}  // namespace urnw
