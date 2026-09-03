// SPDX-License-Identifier: MPL-2.0
#include "pch.h"

#include "WalletPage.h"
#include "ProvideModeVisual.h"

#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Automation.Peers.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.Shapes.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Text.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <iterator>
#include <string_view>

#include "EarningsSheets.h"
#include "EmojiKeyboard.h"
#include "Log.h"
#include "MainWindow.xaml.h"
#include "PageContext.h"
#include "SettingsSheets.h"
#include "StatsFormat.h"
#include "Strings.h"
#include "UrColors.h"
#include "UrComponents.h"

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

// The wallet bridge opens a browser and the user may take a while in it; a
// plain api call does not.
constexpr int kBridgeTimeoutMs = 180'000;
constexpr int kApiTimeoutMs = 20'000;

// The bridge's purpose for a signature that attaches a coldkey to the provider
// (the sign-in leaves it empty).
constexpr const char* kConnectPurpose = "connect";

// Where the claim and head-spot routes live on the web app.
constexpr const char* kUrXyzUrl = "https://ur.xyz";
constexpr const char* kTop200Path = "/app/account/top200";

// A head score this close to the eviction floor is worth a warning.
constexpr double kDemotionWarningRatio = 1.15;

// A stat tile's value, in the colour its state deserves. The dash is a
// PLACEHOLDER, not a number: faint for the placeholder, text colour for a real
// figure.
void SetStatValue(TextBlock const& value, hstring const& text, bool loaded) {
  if (!value) return;
  value.Text(text);
  value.Foreground(loaded ? urnw::colors::TextBrush() : urnw::colors::FaintBrush());
}

using ShapeEllipse = winrt::Microsoft::UI::Xaml::Shapes::Ellipse;
using ShapePolyline = winrt::Microsoft::UI::Xaml::Shapes::Polyline;

// The SDK's stable error codes (SnErrorCode*), in the store's words. A code
// the store has no sentence for shows the SDK's message as it came, so a new
// server state is visible rather than blank.
hstring SnErrorText(std::optional<urnet::SnError> const& error, int64_t epoch = 0) {
  if (!error) return Loc("something_went_wrong");
  const std::string code = error->code.value_or(std::string());
  if (code == "invalid_ss58_address") return Loc("invalid_ss58_address");
  if (code == "wallet_blocked") return Loc("wallet_blocked");
  if (code == "connect_wallet_first") return Loc("connect_wallet_first");
  if (code == "chain_rpc_unreachable" || code == "chain_rpc_error") {
    return Loc("chain_rpc_unreachable");
  }
  if (code == "needs_gas") return Loc("add_tao_for_gas");
  if (code == "claims_for_epoch_expired") {
    return hstring{urnw::Format("claims_for_epoch_expired", epoch)};
  }
  if (code == "already_claimed") return Loc("claim_confirmed");
  if (code == "claim_failed" && error->message.empty()) return Loc("claim_failed");
  if (!error->message.empty()) return H(error->message);
  return code.empty() ? Loc("something_went_wrong") : H(code);
}

// A transport failure as an SnError, so one path renders both.
urnet::SnError TransportError(std::string const& message) {
  urnet::SnError error;
  error.message = message;
  return error;
}

// SnSetWallet predates the common coded SnError result shape. Adapt its
// message explicitly instead of assigning between two unrelated optionals.
urnet::SnError SetWalletError(urnet::SnSetWalletError const& source) {
  urnet::SnError error;
  error.message = source.message;
  return error;
}

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
  // once the host has been measured - and again on every resize. The three
  // lines are held WEAKLY: holding them by value made the handler own strong
  // references to descendants of the very Grid raising the event, a cycle
  // that leaked one chart per load for the life of the process.
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
// --preview-ui deliberately makes no API call (Startup.h): with no token every
// request would be an unauthenticated hit on the production API, so every
// panel on this destination renders EMPTY. With --preview-ui ALREADY on,
// URNETWORK_PREVIEW_SAMPLE=1 pushes obviously synthetic rows through the SAME
// Apply* functions the API path uses. Two gates and a warning in the log every
// time, because data on screen that did not come from the server is the one
// thing a screenshot cannot show you. What holds the line for the ACTIONS is
// WalletPage::CanCallApi(), which every one of them passes through.
constexpr const char* kSampleOwnNetworkId = "sample-network-self";
constexpr const char* kSampleColdkey = "5SAMPLEcoldkeyADDRESSnotREALbittensorSMPL";
constexpr const char* kSampleGasAddress = "0xSAMPLEgasKEYaddressNOTreal000000000000";
constexpr const char* kSampleGasMirror = "5SAMPLEgasMIRRORss58notREALbittensorMIRR";

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
          "preview-sample: rendering SYNTHETIC earnings/leaderboard rows - none of "
          "this came from the api");
    }
    return enabled;
  }();
  return on;
}

std::vector<urnet::AccountPoint> SamplePoints() {
  auto make = [](const char* event, int64_t nanoPoints) {
    urnet::AccountPoint p;
    p.event = event;
    p.point_value = nanoPoints;
    return p;
  };
  // urnet::nanoPointsToPoints divides by 1e6
  const int64_t nano = 1'000'000;
  return {
      make(kEventPayout, 12'340 * nano),      make(kEventReferral, 2'100 * nano),
      make(kEventReliability, 860 * nano),    make(kEventMultiplier, 12'340 * nano),
      make(kEventPayout, 8'900 * nano),       make(kEventReliability, 415 * nano),
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
  *alive_ = false;  // the controller's listener and the sheet's completions stop here
  if (walletValidateTimer_) walletValidateTimer_.Stop();
  if (seekerFlow_.timer) seekerFlow_.timer.Stop();
  if (connectFlow_.timer) connectFlow_.timer.Stop();
  if (rankingFlow_.timer) rankingFlow_.timer.Stop();
  if (pointsPublicFlow_.timer) pointsPublicFlow_.timer.Stop();
  try {
    ClosePointsBoard(/*deviceAlive=*/true);
  } catch (...) {
    // the host may already be gone at teardown; nothing left to close on
  }
}

// See the long note on the declaration (WalletPage.h): guarding the LOAD paths
// left every ACTION on this destination able to reach the api with no session.
bool WalletPage::CanCallApi() const {
  return !w_.previewUi() && Sdk().apiReady() && Sdk().IsLoggedIn();
}

bool WalletPage::CanClaim() const { return CanCallApi() && Sdk().hasDevice(); }

void WalletPage::RefuseNoSession() {
  urnw::LogWarn("earnings: refusing an api call - no session (preview={})", w_.previewUi());
  Notify(Loc("please_login_to_urnetwork"), InfoBarSeverity::Error);
}

// The bar the user can SEE: the leaderboard's lives in pane C beside its own
// ranking rows, the earnings pane's in pane A beside the form that raises it.
void WalletPage::Notify(hstring const& message, InfoBarSeverity severity) {
  if (w_.LeaderboardHost().Visibility() == Visibility::Visible) {
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
  // whenever it turns up, no longer matches and is dropped.
  flow.timer.Tick([weak = w_.get_weak(), &flow, generation,
                   onTimeout = std::move(onTimeout)](auto const&, auto const&) {
    auto self = weak.get();
    if (!self || flow.generation != generation) return;
    ++flow.generation;
    urnw::LogError("earnings: a request never answered - giving up on it");
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
  // debounce the manual-address validation while typing: each keystroke
  // restarts the window, and only the pause validates.
  walletValidateTimer_ = w_.DispatcherQueue().CreateTimer();
  walletValidateTimer_.Interval(std::chrono::milliseconds(300));
  walletValidateTimer_.IsRepeating(false);
  walletValidateTimer_.Tick([weak = w_.get_weak()](auto const&, auto const&) {
    if (auto self = weak.get()) self->wallet().ValidateWalletAddress();
  });
  InitializePointsBoard();
}

void WalletPage::OpenUrl(std::string const& url) {
  try {
    winrt::Windows::System::Launcher::LaunchUriAsync(winrt::Windows::Foundation::Uri(H(url)));
  } catch (...) {
    urnw::LogWarn("earnings: could not open {}", url);
  }
}

void WalletPage::ApplyStrings() {
  // the three pane headers, and a landmark name each
  w_.WalletPaneATitle().Text(Loc("earnings"));
  w_.WalletPaneCTitle().Text(Loc("earnings_network_pane_title"));
  namespace automation = winrt::Microsoft::UI::Xaml::Automation;
  automation::AutomationProperties::SetName(w_.WalletPaneA(), Loc("earnings"));
  automation::AutomationProperties::SetName(w_.WalletPaneB(), Loc("epoch_history"));
  automation::AutomationProperties::SetName(w_.WalletPaneC(), Loc("earnings_network_pane_title"));

  // the ledger pane's switch
  w_.HistoryTabItem().Text(Loc("epoch_history"));
  w_.LeaderboardTabItem().Text(Loc("leaderboard"));
  if (!w_.EarningsTableBar().SelectedItem()) {
    w_.EarningsTableBar().SelectedItem(w_.HistoryTabItem());
  }
  ApplyPointsBoardStrings();

  // pane A
  w_.PointsHeadlineLabel().Text(Loc("points_earned"));
  w_.BittensorWalletHeading().Text(Loc("bittensor_wallet"));
  w_.WalletConnectedNote().Text(Loc("wallet_connected_to_protocol"));
  w_.ChangeWalletButton().Content(LocBox("earnings_change_wallet"));
  w_.WalletNotRetroactiveRun().Text(Loc("wallet_not_retroactive"));
  w_.WalletLearnMoreRun().Text(Loc("learn_more"));
  w_.ConnectWalletButton().Content(LocBox("connect_bittensor_wallet"));
  w_.EnterAddressManuallyButton().Content(LocBox("enter_address_manually"));
  w_.WalletAddressBox().PlaceholderText(Loc("earnings_address_placeholder"));
  automation::AutomationProperties::SetName(w_.WalletAddressBox(),
                                            Loc("earnings_address_placeholder"));
  w_.ConnectAddressButton().Content(LocBox("connect"));
  w_.UnclaimedHeading().Text(Loc("unclaimed"));
  w_.ClaimButton().Content(LocBox("claim"));
  w_.Top200Heading().Text(Loc("top200"));
  w_.Top200Button().Content(LocBox("claim_your_spot"));
  w_.Top200Warning().Text(Loc("top200_demotion_warning"));
  w_.UpgradeButton().Content(LocBox("upgrade_with_stripe"));

  // pane C
  w_.EarningMultipliersHeading().Text(Loc("earning_multipliers"));
  w_.SeekerPointsOnlyText().Text(Loc("seeker_points_only"));
  w_.VerifySeekerButton().Content(LocBox("verify_seeker"));
  w_.NetworkReliabilityHeading().Text(Loc("site_app_network_reliability"));
  w_.WalletProvideModeLabel().Text(Loc("provide_mode"));
  w_.WalletProvideModeValue().Text(Loc(Sdk().CurrentProvideControlMode().c_str()));
  w_.LeaderboardRankLabel().Text(Loc("current_ranking"));
  w_.LeaderboardNetProvidedLabel().Text(Loc("net_provided"));
  w_.LeaderboardPublicLabel().Text(Loc("display_network_on_leaderboard"));
  w_.LeaderboardDescription().Text(Loc("leaderboard_description"));

  // Every panel starts in the state its fetch has not left yet. Without this
  // an unloaded destination is blank, which reads as "there is nothing" rather
  // than "nothing has been asked for".
  const hstring loading = Loc("loading");
  w_.AccountPointsStatusText().Text(loading);
  w_.WalletStatusText().Text(loading);
  w_.ClaimsStatusText().Text(loading);
  w_.ReliabilityStatusText().Text(loading);
  w_.HistoryStatusText().Text(loading);
  w_.HistoryStatusText().Visibility(Visibility::Visible);
  w_.LeaderboardStatusText().Text(loading);
  w_.LeaderboardStatusText().Visibility(Visibility::Visible);
  const hstring dash{L"-"};
  SetStatValue(w_.PointsHeadlineValue(), dash, false);
  SetStatValue(w_.UnclaimedValue(), dash, false);
  SetStatValue(w_.LeaderboardRankValue(), dash, false);
  SetStatValue(w_.LeaderboardNetProvidedValue(), dash, false);
  ApplySeekerState();
  ShowManualPanel(manualPanelOpen_);
}

// The ledger pane shows ONE table at a time; this is the switch in its header.
void WalletPage::OnEarningsTableChanged(SelectorBar const& bar,
                                        SelectorBarSelectionChangedEventArgs const&) {
  const bool leaderboard = bar.SelectedItem() == w_.LeaderboardTabItem();
  w_.HistoryHost().Visibility(leaderboard ? Visibility::Collapsed : Visibility::Visible);
  w_.LeaderboardHost().Visibility(leaderboard ? Visibility::Visible : Visibility::Collapsed);
  ApplyLedgerMeta();
  if (leaderboard && pointsBoardShowing_) EnsurePointsBoard();
}

// ---- loading ---------------------------------------------------------------

void WalletPage::LoadWallet() {
  if (!Sdk().IsLoggedIn()) return;  // the caller's guard is not the only one
  if (auto jwt = Sdk().ParsedJwt(); jwt && jwt->NetworkId) ownNetworkId_ = *jwt->NetworkId;
  LoadPoints();
  LoadSeeker();
  LoadReliability();
  LoadEpochs();
  LoadSnWallet();  // continues into LoadClaims/LoadGas once the coldkey is known
  LoadHead();
}

void WalletPage::RefreshAfterWalletChange() { LoadWallet(); }

void WalletPage::LoadPoints() {
  auto queue = w_.DispatcherQueue();
  auto weak = w_.get_weak();
  w_.AccountPointsStatusText().Text(Loc("loading"));
  w_.AccountPointsStatusText().Visibility(Visibility::Visible);
  Sdk().api().getAccountPoints([queue, weak](std::optional<urnet::AccountPointsResult> result,
                                             std::optional<std::string> err) {
    const bool ok = result && !err;
    std::vector<urnet::AccountPoint> points;
    if (ok && result->network_points) points = *result->network_points;
    if (!ok) {
      urnw::LogError("earnings: getAccountPoints failed{}", err ? (": " + *err) : std::string());
    }
    queue.TryEnqueue([weak, points = std::move(points), ok] {
      if (auto self = weak.get())
        self->wallet().ApplyPoints(points, ok ? Fetch::Ready : Fetch::Failed);
    });
  });
}

// The Seeker flag still lives on the account's wallets (has_seeker_token on
// the verified Solana wallet). Nothing else about those wallets is shown.
void WalletPage::LoadSeeker() {
  auto queue = w_.DispatcherQueue();
  auto weak = w_.get_weak();
  Sdk().api().getAccountWallets(
      [queue, weak](std::optional<urnet::GetAccountWalletsResult> result,
                    std::optional<std::string> err) {
        bool holder = false;
        if (result && result->wallets && !err) {
          for (auto const& wallet : *result->wallets) {
            if (wallet.has_seeker_token) holder = true;
          }
        } else {
          urnw::LogError("earnings: getAccountWallets failed{}",
                         err ? (": " + *err) : std::string());
        }
        queue.TryEnqueue([weak, holder] {
          if (auto self = weak.get()) {
            self->wallet().seekerHolder_ = holder;
            self->wallet().ApplySeekerState();
            self->wallet().RebuildPointsRows();
          }
        });
      });
}

void WalletPage::LoadReliability() {
  auto queue = w_.DispatcherQueue();
  auto weak = w_.get_weak();
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
        if (!ok) urnw::LogError("earnings: getNetworkReliability failed: {}", error);
        queue.TryEnqueue([weak, window, ok] {
          if (auto self = weak.get())
            self->wallet().ApplyReliability(window, ok ? Fetch::Ready : Fetch::Failed);
        });
      });
}

// GET /account/epochs: one row per finalized epoch, points and the network's
// share of the block.
void WalletPage::LoadEpochs() {
  auto queue = w_.DispatcherQueue();
  auto weak = w_.get_weak();
  w_.HistoryStatusText().Text(Loc("loading"));
  w_.HistoryStatusText().Visibility(Visibility::Visible);
  Sdk().api().accountEpochs([queue, weak](std::optional<urnet::AccountEpochsResult> result,
                                          std::optional<std::string> err) {
    std::string error = err ? *err : std::string();
    if (error.empty() && result && result->error) error = result->error->message;
    const bool ok = result && error.empty();
    std::vector<EpochRow> rows;
    if (ok && result->epochs) {
      for (auto const& e : *result->epochs) {
        EpochRow row;
        row.epoch = e.epoch;
        row.startMillis = e.start_millis;
        row.endMillis = e.end_millis;
        row.points = e.points;
        row.shareBps = e.share_bps;
        rows.push_back(row);
      }
    }
    if (!ok) urnw::LogError("earnings: accountEpochs failed: {}", error);
    queue.TryEnqueue([weak, rows = std::move(rows), ok] {
      if (auto self = weak.get())
        self->wallet().ApplyEpochs(rows, ok ? Fetch::Ready : Fetch::Failed);
    });
  });
}

// The coldkey attached to this provider. The device caches it (local state
// ".sn_wallet") and knows its own client id, so it is the first source; with
// no device session (before the first connect, or after Disconnect tore the
// DeviceRemote down) the account setting answers instead.
void WalletPage::LoadSnWallet() {
  auto queue = w_.DispatcherQueue();
  auto weak = w_.get_weak();
  w_.WalletStatusText().Text(Loc("loading"));
  if (Sdk().hasDevice()) {
    std::optional<SnWalletInfo> info;
    try {
      if (auto wallet = Sdk().device().getSnWallet()) {
        info = SnWalletInfo{wallet->coldkey_ss58, wallet->client_id.value_or(std::string()),
                            wallet->set_at_millis};
      }
    } catch (const std::exception& e) {
      urnw::LogError("earnings: getSnWallet failed: {}", e.what());
    }
    ApplySnWallet(info, Fetch::Ready);
    return;
  }
  Sdk().api().snGetWallet([queue, weak](std::optional<urnet::SnGetWalletResult> result,
                                        std::optional<std::string> err) {
    std::string error = err ? *err : std::string();
    if (error.empty() && result && result->error) error = result->error->message;
    const bool ok = result && error.empty();
    std::optional<SnWalletInfo> info;
    if (ok && result->wallet && !result->wallet->coldkey_ss58.empty()) {
      info = SnWalletInfo{result->wallet->coldkey_ss58,
                          result->wallet->client_id.value_or(std::string()),
                          result->wallet->set_at_millis};
    }
    if (!ok) urnw::LogError("earnings: snGetWallet failed: {}", error);
    queue.TryEnqueue([weak, info, ok] {
      if (auto self = weak.get())
        self->wallet().ApplySnWallet(info, ok ? Fetch::Ready : Fetch::Failed);
    });
  });
}

// GET /sn/head: whether this network qualifies for (or holds) a head mining
// spot, per the validators' consensus as the server currently estimates it.
void WalletPage::LoadHead() {
  auto queue = w_.DispatcherQueue();
  auto weak = w_.get_weak();
  Sdk().api().snHead([queue, weak](std::optional<urnet::SnHeadResult> result,
                                   std::optional<std::string> err) {
    std::string error = err ? *err : std::string();
    if (error.empty() && result && result->error) error = result->error->message;
    const bool ok = result && error.empty();
    std::optional<HeadInfo> head;
    if (ok) {
      HeadInfo h;
      h.eligible = result->eligible;
      h.score = result->score;
      h.floor = result->floor;
      h.rankEstimate = result->rank_estimate;
      h.cutoff = result->cutoff > 0 ? result->cutoff : 200;
      h.bound = result->bound;
      h.hotkey = result->hotkey.value_or(std::string());
      h.uid = result->uid.value_or(0);
      h.rank = result->rank.value_or(0);
      head = h;
    }
    if (!ok) urnw::LogError("earnings: snHead failed: {}", error);
    queue.TryEnqueue([weak, head, ok] {
      if (auto self = weak.get())
        self->wallet().ApplyHead(head, ok ? Fetch::Ready : Fetch::Failed);
    });
  });
}

// The vault, read by the SDK on this device: entitlements, leafClaimed and the
// published payout artifact, verified against the vault's root. Nothing from a
// URnetwork API.
void WalletPage::LoadClaims() {
  if (!snWallet_) return;
  if (!Sdk().hasDevice()) {
    ApplyClaims({}, 0, Fetch::Failed, std::nullopt);
    return;
  }
  auto queue = w_.DispatcherQueue();
  auto weak = w_.get_weak();
  w_.ClaimsStatusText().Text(Loc("loading"));
  w_.ClaimsStatusText().Visibility(Visibility::Visible);
  Sdk().device().snClaims([queue, weak](std::optional<urnet::SnClaimsResult> result,
                                        std::optional<std::string> err) {
    std::optional<urnet::SnError> error;
    if (err) error = TransportError(*err);
    else if (result && result->error) error = result->error;
    else if (!result) error = TransportError("snClaims returned no result");
    const bool ok = !error.has_value();
    std::vector<EpochClaim> claims;
    int64_t total = 0;
    if (ok) {
      total = result->total_claimable_rao;
      if (result->claims) {
        for (auto const& c : *result->claims) {
          EpochClaim claim;
          claim.epoch = c.epoch;
          claim.shareBps = c.share_bps;
          claim.amountRao = c.amount_rao;
          claim.status = c.status;
          claim.claimOpenBlock = c.claim_open_block;
          claim.expiryBlock = c.expiry_block;
          claim.txHash = c.tx_hash.value_or(std::string());
          claim.message = c.message.value_or(std::string());
          claims.push_back(std::move(claim));
        }
      }
    }
    if (!ok) {
      urnw::LogError("earnings: snClaims failed: {} {}", error->code.value_or(std::string()),
                     error->message);
    }
    queue.TryEnqueue([weak, claims = std::move(claims), total, ok, error] {
      if (auto self = weak.get())
        self->wallet().ApplyClaims(claims, total, ok ? Fetch::Ready : Fetch::Failed, error);
    });
  });
}

void WalletPage::EnsureChainSettings(std::function<void()> then) {
  if (!Sdk().hasDevice()) {
    then();
    return;
  }
  bool configured = chainSynced_;
  if (!configured) {
    try {
      if (auto settings = Sdk().device().getSnChainSettings()) {
        configured = !settings->vault_address.empty() &&
                     !settings->coordinator_address.empty() && !settings->no_id.empty();
      }
    } catch (const std::exception& e) {
      urnw::LogWarn("earnings: getSnChainSettings failed: {}", e.what());
    }
  }
  if (configured) {
    chainSynced_ = true;
    then();
    return;
  }
  // GET /sn/epoch through the device: it stores the vault, coordinator,
  // operator id, netuid and rpc url it answers with. A failure still runs
  // `then`, so the tile shows the SDK's chain_not_configured reason rather
  // than "Loading" forever.
  auto queue = w_.DispatcherQueue();
  auto weak = w_.get_weak();
  Sdk().device().syncSnChainSettings(
      [queue, weak, then](std::optional<urnet::SnEpochResult> result,
                          std::optional<std::string> err) {
        const bool ok = result && !err;
        if (!ok) {
          urnw::LogError("earnings: syncSnChainSettings failed{}",
                         err ? (": " + *err) : std::string());
        }
        queue.TryEnqueue([weak, then, ok] {
          if (auto self = weak.get()) {
            if (ok) self->wallet().chainSynced_ = true;
            then();
          }
        });
      });
}

// The SDK-held gas key (created on first use, never exported) and its TAO
// balance on the subtensor EVM.
void WalletPage::LoadGas() {
  if (!snWallet_ || !Sdk().hasDevice()) {
    ApplyGas(std::nullopt);
    return;
  }
  GasKeyInfo info;
  try {
    if (auto key = Sdk().device().getSnGasKey()) {
      info.address = key->address;
      info.mirrorSs58 = key->mirror_ss58;
    }
  } catch (const std::exception& e) {
    urnw::LogError("earnings: getSnGasKey failed: {}", e.what());
  }
  if (info.address.empty()) {
    ApplyGas(std::nullopt);
    return;
  }
  ApplyGas(info);  // the address first; the balance follows
  auto queue = w_.DispatcherQueue();
  auto weak = w_.get_weak();
  Sdk().device().snGasBalance([queue, weak, info](std::optional<urnet::SnGasBalanceResult> result,
                                                  std::optional<std::string> err) mutable {
    std::string error = err ? *err : std::string();
    if (error.empty() && result && result->error) error = result->error->message;
    if (result && error.empty()) {
      info.tao = result->tao;
    } else {
      urnw::LogError("earnings: snGasBalance failed: {}", error);
    }
    queue.TryEnqueue([weak, info] {
      if (auto self = weak.get()) self->wallet().ApplyGas(info);
    });
  });
}

// ---- points --------------------------------------------------------------

void WalletPage::ApplyPoints(std::vector<urnet::AccountPoint> const& points, Fetch state) {
  accountPoints_ = {};
  for (auto const& point : points) {
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
    SetStatValue(w_.PointsHeadlineValue(), L"-", false);
    w_.AccountPointsPanel().Children().Clear();
    return;
  }
  w_.AccountPointsStatusText().Visibility(Visibility::Collapsed);
  SetStatValue(w_.PointsHeadlineValue(), hstring{FormatPointsValue(accountPoints_.net)}, true);
  RebuildPointsRows();
}

// The breakdown as rows on the pane's grid: providing, referral, reliability,
// and the Seeker 2x only for a holder (it is points only, and it is the one
// row that says so).
void WalletPage::RebuildPointsRows() {
  auto panel = w_.AccountPointsPanel();
  panel.Children().Clear();
  auto add = [&panel](hstring const& key, double value) {
    auto row = kit::MakePaneKeyValueRow(key, hstring{FormatPointsValue(value)});
    panel.Children().Append(row.root);
  };
  add(Loc("providing"), accountPoints_.payout);
  add(Loc("referral"), accountPoints_.referral);
  add(Loc("reliability"), accountPoints_.reliability);
  if (seekerHolder_) {
    auto row = kit::MakePaneKeyValueRow(
        Loc("seeker_token_verified"),
        hstring{urnw::Format("plus_amount", FormatPointsValue(accountPoints_.multiplier))});
    row.value.Foreground(colors::MakeBrush(colors::kUrGreen));
    panel.Children().Append(row.root);
  }
}

// ---- the coldkey -----------------------------------------------------------

void WalletPage::ApplySnWallet(std::optional<SnWalletInfo> wallet, Fetch state) {
  snWallet_ = std::move(wallet);
  walletState_ = state;
  const bool connected = snWallet_.has_value();

  if (state == Fetch::Failed && !connected) {
    w_.WalletStatusText().Text(Loc("something_went_wrong"));
    w_.WalletStatusText().Visibility(Visibility::Visible);
  } else {
    kit::SetTextOrCollapse(w_.WalletStatusText(),
                           connected ? hstring{ShortAddress(snWallet_->coldkeySs58)} : hstring{});
  }
  w_.WalletConnectedPanel().Visibility(connected ? Visibility::Visible : Visibility::Collapsed);
  w_.WalletDisconnectedPanel().Visibility(connected ? Visibility::Collapsed : Visibility::Visible);
  if (connected) {
    w_.WalletAddressText().Text(hstring{urnw::Widen(snWallet_->coldkeySs58)});
    w_.ManualAddressPanel().Visibility(Visibility::Collapsed);
  }

  // THE SUBNET LAYER: the unclaimed tile exists only once a coldkey is
  // attached, and the history's alpha column with it. Not retroactive: earlier
  // epochs stay points only.
  w_.UnclaimedPanel().Visibility(connected ? Visibility::Visible : Visibility::Collapsed);
  if (!connected) {
    claims_.clear();
    totalClaimableRao_ = 0;
    gas_.reset();
  }
  RebuildHistory();
  if (connected && !w_.previewUi()) {
    EnsureChainSettings([weak = w_.get_weak()] {
      if (auto self = weak.get()) {
        self->wallet().LoadClaims();
        self->wallet().LoadGas();
      }
    });
  }
}

void WalletPage::SetConnectingWallet(bool connecting) {
  connectingWallet_ = connecting;
  w_.ConnectWalletButton().IsEnabled(!connecting);
  w_.ChangeWalletButton().IsEnabled(!connecting);
  w_.ConnectAddressButton().IsEnabled(!connecting && manualAddressOk_);
  // "Waiting" is a state the user must be able to SEE: the browser is open
  // and the app is waiting for it to come back.
  kit::SetTextOrCollapse(w_.WalletConnectStatusText(),
                         connecting ? Loc("opening_wallet_in_browser") : hstring{});
}

// Both doors lead here. `pinnedAddress` is the pasted address (the challenge
// is fetched for it and the bridge has to answer with it), or empty for the
// bridge's own pick.
void WalletPage::StartWalletConnect(std::string const& pinnedAddress) {
  if (connectingWallet_ || w_.sheetOpen()) return;
  // Before the browser opens, not after: with no session this ends in a
  // server write, and it opens a BROWSER on the way there.
  if (!CanCallApi()) {
    RefuseNoSession();
    return;
  }
  SetConnectingWallet(true);

  // WalletConnect has no timeout, and its on_error only fires when the deep
  // link comes BACK carrying an error. A closed browser tab produces nothing
  // at all - so the watchdog is what re-enables the buttons.
  const uint32_t generation =
      BeginFlow(connectFlow_, kBridgeTimeoutMs, [weak = w_.get_weak()] {
        if (auto self = weak.get()) {
          self->wallet().SetConnectingWallet(false);
          self->wallet().Notify(Loc("wallet_connect_failed"), InfoBarSeverity::Error);
        }
      });

  auto queue = w_.DispatcherQueue();
  auto weak = w_.get_weak();
  Sdk().SignWithBittensorWallet(
      pinnedAddress, kConnectPurpose,
      [queue, weak, generation, pinnedAddress](bool ok, std::string address,
                                               std::string signature, std::string message,
                                               std::string error) {
        // on whichever thread delivered the deep link
        queue.TryEnqueue([weak, generation, ok, address, signature, message, error,
                          pinnedAddress] {
          if (auto self = weak.get()) {
            self->wallet().ApplyWalletSigned(generation, ok, address, signature, message, error,
                                             pinnedAddress);
          }
        });
      });
}

void WalletPage::OnConnectWallet(IInspectable const&, RoutedEventArgs const&) {
  StartWalletConnect(std::string());
}

void WalletPage::OnChangeWallet(IInspectable const&, RoutedEventArgs const&) {
  StartWalletConnect(std::string());
}

// The signature is back. The address is validated BEFORE anything is sent to
// the account: a syntax failure is rejected outright, a banned address is
// blocked and goes nowhere, an address with no chain activity is allowed with
// a warning (the pasted path already showed it while typing).
void WalletPage::ApplyWalletSigned(uint32_t generation, bool ok, std::string const& address,
                                   std::string const& signature, std::string const& message,
                                   std::string const& error, std::string const& expectedAddress) {
  if (connectFlow_.generation != generation) {
    urnw::LogWarn("earnings: dropping a wallet signature for an abandoned request (ok={})", ok);
    return;
  }
  if (!ok) {
    SettleFlow(connectFlow_, generation);
    SetConnectingWallet(false);
    urnw::LogError("earnings: wallet signature failed: {}", error);
    Notify(error.empty() ? Loc("wallet_connect_failed") : H(error), InfoBarSeverity::Error);
    return;
  }
  if (!expectedAddress.empty() && address != expectedAddress) {
    SettleFlow(connectFlow_, generation);
    SetConnectingWallet(false);
    Notify(Loc("earnings_wallet_mismatch"), InfoBarSeverity::Error);
    return;
  }
  if (!urnet::validateSs58(address)) {
    SettleFlow(connectFlow_, generation);
    SetConnectingWallet(false);
    Notify(Loc("invalid_ss58_address"), InfoBarSeverity::Error);
    return;
  }
  // The device validates the address itself before anything is sent (a
  // banned address goes nowhere; a never-seen one comes back as a warning),
  // and the pasted address was already checked while typing. Only the api
  // fallback needs the check here.
  const bool manualChecked =
      !expectedAddress.empty() && manualAddressOk_ && expectedAddress == manualAddress_;
  if (Sdk().hasDevice() || manualChecked) {
    SubmitWalletConnect(generation, address, signature, message);
    return;
  }
  kit::SetTextOrCollapse(w_.WalletConnectStatusText(), Loc("checking_wallet_address"));
  auto weak = w_.get_weak();
  ValidateAddressRemote(
      address, [weak, generation, address, signature, message](std::optional<AddressVerdict> v) {
        auto self = weak.get();
        if (!self) return;
        auto& page = self->wallet();
        if (page.connectFlow_.generation != generation) return;
        if (!v) {
          page.SettleFlow(page.connectFlow_, generation);
          page.SetConnectingWallet(false);
          page.Notify(Loc("something_went_wrong"), InfoBarSeverity::Error);
          return;
        }
        if (v->banned) {
          page.SettleFlow(page.connectFlow_, generation);
          page.SetConnectingWallet(false);
          page.Notify(Loc("wallet_blocked"), InfoBarSeverity::Error);
          return;
        }
        if (!v->validSyntax) {
          page.SettleFlow(page.connectFlow_, generation);
          page.SetConnectingWallet(false);
          page.Notify(Loc("invalid_ss58_address"), InfoBarSeverity::Error);
          return;
        }
        if (!v->existsOnChain) {
          page.Notify(Loc("wallet_looks_new_warning"), InfoBarSeverity::Warning);
        }
        page.SubmitWalletConnect(generation, address, signature, message);
      });
}

// Attach the signed coldkey to THIS device's provider (Device.connectSnWallet,
// which knows its client id and caches the answer), or to the account when
// there is no device session to route through.
void WalletPage::SubmitWalletConnect(uint32_t generation, std::string const& address,
                                     std::string const& signature, std::string const& message) {
  auto queue = w_.DispatcherQueue();
  auto weak = w_.get_weak();
  // the bridge round trip is over; from here it is one api call
  BeginFlow(connectFlow_, kApiTimeoutMs, [weak] {
    if (auto self = weak.get()) {
      self->wallet().SetConnectingWallet(false);
      self->wallet().Notify(Loc("wallet_connect_failed"), InfoBarSeverity::Error);
    }
  });
  const uint32_t submitGeneration = connectFlow_.generation;
  (void)generation;

  auto deliver = [queue, weak, submitGeneration](std::optional<urnet::SnError> error,
                                                 std::string warning) {
    queue.TryEnqueue([weak, submitGeneration, error, warning] {
      if (auto self = weak.get()) {
        self->wallet().ApplyWalletConnectResult(submitGeneration, !error.has_value(), error,
                                                warning);
      }
    });
  };

  if (Sdk().hasDevice()) {
    Sdk().device().connectSnWallet(
        address, signature, message,
        [deliver](std::optional<urnet::SnConnectWalletResult> result,
                  std::optional<std::string> err) {
          std::optional<urnet::SnError> error;
          if (err) error = TransportError(*err);
          else if (result && result->error) error = result->error;
          else if (!result) error = TransportError("connectSnWallet returned no result");
          if (error) {
            urnw::LogError("earnings: connectSnWallet failed: {} {}",
                           error->code.value_or(std::string()), error->message);
          }
          deliver(error, (result && !error) ? result->warning.value_or(std::string())
                                            : std::string());
        });
    return;
  }
  urnet::SnSetWalletArgs args;
  args.coldkey_ss58 = address;
  args.signature = signature;
  args.message = message;
  Sdk().api().snSetWallet(args, [deliver](std::optional<urnet::SnSetWalletResult> result,
                                          std::optional<std::string> err) {
    std::optional<urnet::SnError> error;
    if (err) error = TransportError(*err);
    else if (result && result->error) error = SetWalletError(*result->error);
    else if (!result) error = TransportError("snSetWallet returned no result");
    if (error) {
      urnw::LogError("earnings: snSetWallet failed: {} {}", error->code.value_or(std::string()),
                     error->message);
    }
    deliver(error, std::string());
  });
}

void WalletPage::ApplyWalletConnectResult(uint32_t generation, bool ok,
                                          std::optional<urnet::SnError> const& error,
                                          std::string const& warning) {
  if (!SettleFlow(connectFlow_, generation)) {
    urnw::LogWarn("earnings: dropping a connect result for an abandoned request (ok={})", ok);
    return;
  }
  SetConnectingWallet(false);
  if (!ok) {
    Notify(error ? SnErrorText(error) : Loc("wallet_connect_failed"), InfoBarSeverity::Error);
    return;
  }
  // The SDK's warning is a store key (wallet_looks_new_warning): the wallet is
  // connected, and the one thing worth saying is the warning.
  if (!warning.empty()) {
    const std::wstring text = urnw::Localized(warning);
    Notify(text == urnw::Widen(warning) ? Loc("wallet_connected") : hstring{text},
           InfoBarSeverity::Warning);
  } else {
    Notify(Loc("wallet_connected"), InfoBarSeverity::Success);
  }
  w_.WalletAddressBox().Text(L"");  // clears the verdict through OnWalletAddressChanged
  ShowManualPanel(false);
  LoadSnWallet();  // the coldkey, then the claims and the gas key behind it
}

// ---- the manual address (still signed) --------------------------------------

void WalletPage::ShowManualPanel(bool show) {
  manualPanelOpen_ = show;
  w_.ManualAddressPanel().Visibility(show ? Visibility::Visible : Visibility::Collapsed);
}

void WalletPage::OnEnterAddressManually(IInspectable const&, RoutedEventArgs const&) {
  ShowManualPanel(!manualPanelOpen_);
  if (manualPanelOpen_) w_.WalletAddressBox().Focus(FocusState::Programmatic);
}

void WalletPage::OnWalletAddressChanged(IInspectable const&, TextChangedEventArgs const&) {
  manualAddressOk_ = false;
  manualAddress_.clear();
  ++walletValidateGeneration_;  // drop any validation still in flight
  w_.ConnectAddressButton().IsEnabled(false);
  w_.WalletAddressVerdictText().Text(L"");
  w_.WalletAddressVerdictText().Foreground(colors::MutedBrush());
  if (walletValidateTimer_) {
    walletValidateTimer_.Stop();  // restart the debounce window on every keystroke
    walletValidateTimer_.Start();
  }
}

// Syntax first, locally, through the SDK's own ss58 check: a typo never
// reaches the network. Then the unauthenticated validate call.
void WalletPage::ValidateWalletAddress() {
  const std::string address =
      TrimWhitespace(urnw::Narrow(w_.WalletAddressBox().Text().c_str()));
  if (address.empty()) return;
  const uint32_t generation = ++walletValidateGeneration_;
  if (!urnet::validateSs58(address)) {
    w_.WalletAddressVerdictText().Text(Loc("invalid_ss58_address"));
    w_.WalletAddressVerdictText().Foreground(colors::DangerBrush());
    return;
  }
  // The validate call needs no token, but --preview-ui asks the network for
  // nothing and a build with no api has nothing to ask.
  if (w_.previewUi() || !Sdk().apiReady()) return;
  w_.WalletAddressVerdictText().Text(Loc("checking_wallet_address"));
  w_.WalletAddressVerdictText().Foreground(colors::MutedBrush());
  auto weak = w_.get_weak();
  ValidateAddressRemote(address, [weak, generation, address](std::optional<AddressVerdict> v) {
    if (auto self = weak.get()) {
      self->wallet().manualAddress_ = address;
      self->wallet().ApplyManualVerdict(generation, v);
    }
  });
}

void WalletPage::ValidateAddressRemote(std::string const& address,
                                       std::function<void(std::optional<AddressVerdict>)> done) {
  auto queue = w_.DispatcherQueue();
  Sdk().api().snValidateWallet(
      address, [queue, done = std::move(done)](std::optional<urnet::SnValidateWalletResult> result,
                                               std::optional<std::string> err) {
        std::string error = err ? *err : std::string();
        if (error.empty() && result && result->error) error = result->error->message;
        std::optional<AddressVerdict> verdict;
        if (result && error.empty()) {
          AddressVerdict v;
          v.validSyntax = result->valid_syntax;
          v.existsOnChain = result->exists_on_chain;
          v.banned = result->banned;
          v.message = result->message.value_or(std::string());
          verdict = v;
        } else {
          urnw::LogError("earnings: snValidateWallet failed: {}", error);
        }
        queue.TryEnqueue([done, verdict] { done(verdict); });
      });
}

void WalletPage::ApplyManualVerdict(uint32_t generation, std::optional<AddressVerdict> verdict) {
  if (generation != walletValidateGeneration_) return;  // a later edit superseded this
  manualAddressOk_ = false;
  auto text = w_.WalletAddressVerdictText();
  if (!verdict) {
    text.Text(Loc("something_went_wrong"));
    text.Foreground(colors::DangerBrush());
  } else if (verdict->banned) {
    // blocked: the address goes nowhere
    text.Text(Loc("wallet_blocked"));
    text.Foreground(colors::DangerBrush());
  } else if (!verdict->validSyntax) {
    text.Text(Loc("invalid_ss58_address"));
    text.Foreground(colors::DangerBrush());
  } else if (!verdict->existsOnChain) {
    // a warning, and the user may continue
    text.Text(Loc("wallet_looks_new_warning"));
    text.Foreground(colors::MakeBrush(colors::kUrAmber));
    manualAddressOk_ = true;
  } else {
    text.Text(L"");
    manualAddressOk_ = true;
  }
  w_.ConnectAddressButton().IsEnabled(manualAddressOk_ && !connectingWallet_);
}

// The pasted address is only ever attached SIGNED: the bridge signs the
// challenge issued for it, and the answer has to come from that address.
void WalletPage::OnConnectWalletAddress(IInspectable const&, RoutedEventArgs const&) {
  if (!manualAddressOk_ || manualAddress_.empty()) return;
  StartWalletConnect(manualAddress_);
}

// ---- history ---------------------------------------------------------------

void WalletPage::ApplyEpochs(std::vector<EpochRow> const& epochs, Fetch state) {
  epochs_ = epochs;
  epochsState_ = state;
  // newest first; the server does not promise an order
  std::sort(epochs_.begin(), epochs_.end(),
            [](EpochRow const& a, EpochRow const& b) { return a.epoch > b.epoch; });
  RebuildHistory();
}

const EpochClaim* WalletPage::ClaimForEpoch(int64_t epoch) const {
  for (auto const& claim : claims_) {
    if (claim.epoch == epoch) return &claim;
  }
  return nullptr;
}

// The desktop advantage over the phone: the history is a table with column
// headers. Points, always; the SN25a and status columns only with a wallet,
// filled from the vault for the epochs since it was attached and "-" before.
void WalletPage::RebuildHistory() {
  auto panel = w_.HistoryPanel();
  panel.Children().Clear();
  if (epochsState_ == Fetch::Failed) {
    w_.HistoryStatusText().Text(Loc("something_went_wrong"));
    w_.HistoryStatusText().Visibility(Visibility::Visible);
    ApplyLedgerMeta();
    return;
  }
  if (epochsState_ == Fetch::Loading) {
    ApplyLedgerMeta();
    return;
  }
  if (epochs_.empty()) {
    w_.HistoryStatusText().Text(Loc("no_points_yet"));
    w_.HistoryStatusText().Visibility(Visibility::Visible);
    ApplyLedgerMeta();
    return;
  }
  w_.HistoryStatusText().Visibility(Visibility::Collapsed);

  const bool withAlpha = snWallet_.has_value();
  std::vector<double> weights{2, 2, 2, 2};
  std::vector<hstring> titles{Loc("earnings_epoch_column"), Loc("earnings_date_column"),
                              Loc("earnings_points_column"), Loc("earnings_share_column")};
  if (withAlpha) {
    weights.push_back(2);
    weights.push_back(2);
    titles.push_back(Loc("sn_alpha_symbol"));
    titles.push_back(Loc("earnings_status_column"));
  }
  // two leading text columns (epoch, date); every column after them is a figure
  panel.Children().Append(kit::MakePaneTableHeader(weights, titles, /*textColumns=*/2));

  namespace automation = winrt::Microsoft::UI::Xaml::Automation;
  for (auto const& epoch : epochs_) {
    auto row = kit::MakePaneTableRow(weights, 36, /*textColumns=*/2);
    row.cells[0].Text(hstring{urnw::Format("epoch_row_title", epoch.epoch)});
    row.cells[1].Text(hstring{DateFromMillis(epoch.endMillis)});
    row.cells[2].Text(hstring{FormatPointsValue(epoch.points)});
    row.cells[3].Text(hstring{FormatShareBpsValue(epoch.shareBps)});
    if (withAlpha) {
      const EpochClaim* claim = ClaimForEpoch(epoch.epoch);
      if (claim) {
        row.cells[4].Text(hstring{FormatAlphaRao(claim->amountRao)});
        row.cells[5].Text(ClaimStatusText(claim->status));
        // Lime is the earnings accent: alpha that can be claimed now, or was.
        if (claim->status == "claimable" || claim->status == "claimed") {
          row.cells[4].Foreground(colors::MakeBrush(colors::kUrGreen));
        }
        if (claim->status == "expired") row.cells[5].Foreground(colors::DangerBrush());
      } else {
        // before the wallet, or not finalized: points only
        row.cells[4].Text(L"-");
        row.cells[4].Foreground(colors::FaintBrush());
        row.cells[5].Text(L"");
      }
    }
    automation::AutomationProperties::SetName(
        row.root, hstring{urnw::Format("epoch_row_title", epoch.epoch) + L", " +
                          urnw::Format("points_short", FormatPointsValue(epoch.points))});
    panel.Children().Append(row.root);
  }
  ApplyLedgerMeta();
}

// The ledger pane's header figure belongs to whichever table is showing.
void WalletPage::ApplyLedgerMeta() {
  const bool history = w_.LeaderboardHost().Visibility() != Visibility::Visible;
  const int64_t boardCount =
      pointsBoardShowing_ ? static_cast<int64_t>(pointsRows_.size()) : leaderboardCount_;
  const int64_t count = history ? static_cast<int64_t>(epochs_.size()) : boardCount;
  kit::SetTextOrCollapse(w_.WalletPaneBMeta(),
                         count <= 0 ? hstring{} : hstring{std::to_wstring(count)});
}

// ---- claims, gas, head -----------------------------------------------------

void WalletPage::ApplyClaims(std::vector<EpochClaim> const& claims, int64_t totalClaimableRao,
                             Fetch state, std::optional<urnet::SnError> const& error) {
  claims_ = claims;
  totalClaimableRao_ = totalClaimableRao;
  claimsState_ = state;
  size_t claimable = 0;
  for (auto const& claim : claims_) {
    if (claim.status == "claimable") ++claimable;
  }

  if (state == Fetch::Failed) {
    const bool noDevice = !Sdk().hasDevice();
    w_.ClaimsStatusText().Text(noDevice ? hstring{} : Loc("something_went_wrong"));
    w_.ClaimsStatusText().Visibility(noDevice ? Visibility::Collapsed : Visibility::Visible);
    SetStatValue(w_.UnclaimedValue(), L"-", false);
    w_.UnclaimedNote().Text(noDevice ? Loc("earnings_claims_need_session") : SnErrorText(error));
    w_.ClaimButton().IsEnabled(false);
    RebuildHistory();
    return;
  }
  w_.ClaimsStatusText().Visibility(Visibility::Collapsed);
  SetStatValue(w_.UnclaimedValue(), hstring{FormatAlphaRao(totalClaimableRao_)}, true);
  if (totalClaimableRao_ > 0) w_.UnclaimedValue().Foreground(colors::MakeBrush(colors::kUrGreen));
  w_.UnclaimedNote().Text(
      claimable > 0
          ? hstring{urnw::Format("claim_across_epochs", static_cast<int64_t>(claimable))}
          : Loc("wallet_connected_to_protocol"));
  // Live: only with something to claim and a device to send from. Under the
  // preview's sample the dialog still OPENS (read-only, its action disabled)
  // so it can be looked at.
  const bool previewSample = w_.previewUi() && PreviewSample() && !claims_.empty();
  w_.ClaimButton().IsEnabled((totalClaimableRao_ > 0 && CanClaim()) || previewSample);
  RebuildHistory();
}

void WalletPage::ApplyGas(std::optional<GasKeyInfo> gas) { gas_ = std::move(gas); }

void WalletPage::ApplyHead(std::optional<HeadInfo> head, Fetch state) {
  head_ = std::move(head);
  const bool show = state == Fetch::Ready && head_ && (head_->eligible || head_->bound);
  w_.Top200Panel().Visibility(show ? Visibility::Visible : Visibility::Collapsed);
  if (!show) return;
  if (head_->bound) {
    w_.Top200Status().Text(hstring{urnw::Format("top200_bound_status", head_->uid, head_->rank)});
    w_.Top200Detail().Text(Loc("top200_bound_detail"));
    w_.Top200Button().Visibility(Visibility::Collapsed);
  } else {
    w_.Top200Status().Text(Loc("top200_you_qualify"));
    w_.Top200Detail().Text(
        hstring{urnw::Format("top200_detail", head_->rankEstimate, head_->cutoff)});
    w_.Top200Button().Visibility(Visibility::Visible);
  }
  const bool nearFloor =
      head_->floor > 0 && head_->score > 0 && head_->score < head_->floor * kDemotionWarningRatio;
  w_.Top200Warning().Visibility(nearFloor ? Visibility::Visible : Visibility::Collapsed);
}

std::string WalletPage::ExplorerTxUrl() const {
  try {
    // the device's merged view first (synced from /sn/epoch), the defaults
    // otherwise
    if (Sdk().hasDevice()) {
      if (auto settings = Sdk().device().getSnChainSettings();
          settings && !settings->explorer_tx_url.empty()) {
        return settings->explorer_tx_url;
      }
    }
    if (auto settings = urnet::defaultSnChainSettings()) return settings->explorer_tx_url;
  } catch (const std::exception& e) {
    urnw::LogWarn("earnings: chain settings unavailable: {}", e.what());
  }
  return {};
}

// Device.snClaim, as the dialog's Claimer: the SDK builds the claim calldata
// from the published artifact, signs with the gas key and sends it to the
// vault, reporting per epoch. Nothing here touches a URnetwork API.
Claimer WalletPage::MakeClaimer() {
  return [](std::vector<int64_t> epochs, ClaimEvents events) {
    if (!Sdk().hasDevice()) {
      if (events.failed) {
        for (int64_t epoch : epochs) events.failed(epoch, "no device");
      }
      if (events.done) events.done();
      return;
    }
    urnet::SnClaimCallback callback;
    callback.sent = [events](int64_t epoch, const std::string& txHash) {
      if (events.sent) events.sent(epoch, txHash);
    };
    callback.confirmed = [events](int64_t epoch, const std::string& txHash, int64_t amountRao) {
      if (events.confirmed) events.confirmed(epoch, txHash, amountRao);
    };
    callback.failed = [events](int64_t epoch, const std::string& message) {
      if (events.failed) events.failed(epoch, message);
    };
    callback.done = [events]() {
      if (events.done) events.done();
    };
    Sdk().device().snClaim(epochs, callback);
  };
}

winrt::fire_and_forget WalletPage::OnClaimAlpha(IInspectable const&, RoutedEventArgs const&) {
  if (w_.sheetOpen()) co_return;
  if (!snWallet_) {
    Notify(Loc("connect_wallet_first"), InfoBarSeverity::Error);
    co_return;
  }
  auto self = w_.get_strong();
  // Readable with no session (that is the point of the preview) but not
  // ACTABLE: its one button sends a transaction.
  const bool allowActions = CanClaim();
  GasKeyInfo gas = gas_.value_or(GasKeyInfo{});
  auto weak = w_.get_weak();
  self->SetSheetOpen(true);
  try {
    claimSheet_ = urnw::ClaimAlphaSheet::Create(
        self->Content().XamlRoot(), self->DispatcherQueue(), claims_, gas, ExplorerTxUrl(),
        allowActions, MakeClaimer(), [weak] {
          // at least one epoch confirmed: the tile and the history move
          if (auto w = weak.get()) {
            w->wallet().LoadClaims();
            w->wallet().LoadGas();
          }
        });
    co_await self->wallet().claimSheet_->Dialog().ShowAsync();
  } catch (winrt::hresult_error const& e) {
    urnw::LogError("earnings: the claim sheet failed to open: {}",
                   urnw::Narrow(std::wstring{e.message()}));
  } catch (...) {
    urnw::LogError("earnings: the claim sheet failed to open");
  }
  self->wallet().claimSheet_.reset();
  self->SetSheetOpen(false);
}

void WalletPage::OnClaimTop200(IInspectable const&, RoutedEventArgs const&) {
  OpenUrl("https://" + Sdk().linkHostName() + kTop200Path);
}

void WalletPage::OpenProtocolSite() { OpenUrl(kUrXyzUrl); }

// ---- network reliability -------------------------------------------------

void WalletPage::ApplyReliability(std::optional<urnet::ReliabilityWindow> window, Fetch state) {
  if (!providingEnabled_) {
    // providing is off: the chart hides and the group says so, the same gate
    // and message as the stats widget
    w_.ReliabilityStatusText().Text(Loc("providing_disabled"));
    w_.ReliabilityStatusText().Visibility(Visibility::Visible);
    w_.ReliabilityCard().Visibility(Visibility::Collapsed);
    return;
  }
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

// ---- the Seeker multiplier (points only) ------------------------------------

void WalletPage::ApplySeekerState() {
  if (seekerHolder_) {
    w_.SeekerStatusText().Text(
        hstring{urnw::Localized("seeker_token_verified") + L" " +
                urnw::Localized("you_re_earning_2x_points")});
    w_.VerifySeekerButton().Visibility(Visibility::Collapsed);
    return;
  }
  // "Waiting" is a state the user must be able to SEE.
  w_.SeekerStatusText().Text(verifyingSeeker_ ? Loc("opening_wallet_in_browser")
                                              : Loc("connect_seeker_wallet"));
  w_.VerifySeekerButton().Visibility(Visibility::Visible);
  w_.VerifySeekerButton().IsEnabled(!verifyingSeeker_);
}

// Claim the 2x multiplier by proving a Solana wallet holds the Seeker token
// (android SettingsScreen.signAndVerifySeekerHolder). The wallet signs a
// timestamped challenge through the ur.io/wallet-connect browser bridge and
// the signed triple goes to Api.verifySeekerHolder. Points only: the Seeker
// wallet has no bearing on SN25a, which settles on the Bittensor coldkey.
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

  // A closed browser tab produces nothing at all, so the watchdog is what
  // brings the button back.
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
    LoadSeeker();  // has_seeker_token now reads true on the verified wallet
    return;
  }
  ApplySeekerState();
}

void WalletPage::ShowPreviewSnackbar() {
  snackbar_.Show(Loc("wallet_connect_failed"), InfoBarSeverity::Error);
}

// --preview-ui: with no session the API loads are skipped, so every panel would
// otherwise sit on "Loading..." forever - which is exactly what a hang looks
// like. Settle them all on their empty state instead.
void WalletPage::ShowPreviewWalletState() {
  if (PreviewSample()) {
    seekerHolder_ = true;
    ApplyPoints(SamplePoints(), Fetch::Ready);
    ApplyReliability(SampleReliability(), Fetch::Ready);
    std::vector<EpochRow> epochs;
    for (int i = 0; i < 4; ++i) {
      EpochRow row;
      row.epoch = 120 - i;
      row.endMillis = 1'785'000'000'000LL - i * 7LL * 86'400'000LL;
      row.startMillis = row.endMillis - 7LL * 86'400'000LL;
      row.points = 3'120.0 - 410.0 * i;
      row.shareBps = 71 - 9 * i;
      epochs.push_back(row);
    }
    ApplyEpochs(epochs, Fetch::Ready);
    ApplySnWallet(SnWalletInfo{kSampleColdkey, "sample-client", 1'783'000'000'000LL},
                  Fetch::Ready);
    std::vector<EpochClaim> claims;
    auto claim = [](int64_t epoch, int64_t rao, const char* status, const char* tx) {
      EpochClaim c;
      c.epoch = epoch;
      c.shareBps = 71;
      c.amountRao = rao;
      c.status = status;
      if (tx) c.txHash = tx;
      return c;
    };
    claims.push_back(claim(120, 3'241'000'000, "claimable", nullptr));
    claims.push_back(claim(119, 2'980'500'000, "claimed", "0xSAMPLEtxHASHnotREAL111111"));
    claims.push_back(claim(118, 2'700'000'000, "expired", nullptr));
    ApplyClaims(claims, 3'241'000'000, Fetch::Ready, std::nullopt);
    GasKeyInfo gas;
    gas.address = kSampleGasAddress;
    gas.mirrorSs58 = kSampleGasMirror;
    gas.tao = 0.0;  // the funding hint renders
    ApplyGas(gas);
    HeadInfo head;
    head.eligible = true;
    head.score = 41.5;
    head.floor = 38.0;
    head.rankEstimate = 172;
    head.cutoff = 200;
    ApplyHead(head, Fetch::Ready);
    return;
  }
  ApplyPoints({}, Fetch::Ready);
  ApplyReliability(std::nullopt, Fetch::Ready);
  ApplyEpochs({}, Fetch::Ready);
  ApplySnWallet(std::nullopt, Fetch::Ready);
  ApplyGas(std::nullopt);
  ApplyHead(std::nullopt, Fetch::Ready);
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
    SettlePointsBoardPreview();
    return;
  }
  ApplyRanking({}, /*ok=*/false);
  ApplyLeaderboard({}, Fetch::Ready);
  SettlePointsBoardPreview();
}

void WalletPage::ApplyLeaderboard(urnet::LeaderboardEarnersList const& earners, Fetch state) {
  auto rows = w_.LeaderboardRows();
  rows.Children().Clear();
  if (state == Fetch::Failed) {
    w_.LeaderboardStatusText().Text(Loc("something_went_wrong"));
    w_.LeaderboardStatusText().Visibility(Visibility::Visible);
    return;
  }
  if (earners.empty()) {
    w_.LeaderboardStatusText().Text(Loc("site_app_leaderboard_empty"));
    w_.LeaderboardStatusText().Visibility(Visibility::Visible);
    return;
  }
  w_.LeaderboardStatusText().Visibility(Visibility::Collapsed);

  // The same table builder the history table uses.
  const std::vector<double> weights{1, 5, 2};
  rows.Children().Append(
      kit::MakePaneTableHeader(weights,
                               {Loc("current_ranking"), Loc("network"), Loc("net_provided")},
                               /*textColumns=*/2));

  int rank = 0;
  for (auto const& earner : earners) {
    ++rank;
    const bool isOwn = !ownNetworkId_.empty() && earner.network_id == ownNetworkId_;
    // A network that has not opted in is on the board by its numbers only; the
    // name is never rendered. Profanity is masked the same way.
    const bool masked = !isOwn && (!earner.is_public || earner.contains_profanity);

    auto row = kit::MakePaneTableRow(weights, 36, /*textColumns=*/2);
    row.cells[0].Text(hstring{L"#" + std::to_wstring(rank)});
    row.cells[1].Text(masked ? Loc("private_network")
                             : hstring{urnw::Widen(earner.network_name)});
    row.cells[2].Text(hstring{FormatMiB(earner.net_mib_count)});

    // The account's own row is the point of the table, so it is marked - in
    // colour AND with the pane's fill step, because colour alone is never the
    // only signal.
    if (isOwn) {
      auto own = colors::MakeBrush(colors::kUrGreen);
      for (auto const& cell : row.cells) cell.Foreground(own);
      row.root.Background(colors::CardBrush());
    } else if (masked) {
      for (auto const& cell : row.cells) cell.Foreground(colors::MutedBrush());
    }
    rows.Children().Append(row.root);
  }
  leaderboardCount_ = static_cast<int64_t>(earners.size());
  ApplyLedgerMeta();
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
  if (pointsBoardShowing_) EnsurePointsBoard();
  w_.LeaderboardStatusText().Text(Loc("loading"));
  w_.LeaderboardStatusText().Visibility(Visibility::Visible);

  if (auto jwt = Sdk().ParsedJwt(); jwt && jwt->NetworkId) ownNetworkId_ = *jwt->NetworkId;

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

// ---- the points board -----------------------------------------------------
//
// The all-time points leaderboard (android/POINTSLEADERBOARD.md), the Android
// screen's structure on the ledger pane: a Data | Points switch above the
// board, sort chips above the rows, rows paged in by the SDK controller as the
// list nears its end, and this network's own block beside it on pane C. The
// controller (PointsLeaderboardViewController) is the ONLY source of rows,
// ranks, sort and pages; this file only mirrors its state and forwards the
// sort, load-more and refresh intents.

namespace {

constexpr double kPointsRowHeight = 36;
constexpr double kPointsTableHeaderHeight = 28;  // the column-name strip above the rows

hstring Utf8(std::string const& s) { return hstring{urnw::Widen(s)}; }

// a Segoe Fluent glyph as a button's content
FontIcon PointsGlyph(wchar_t const* glyph) {
  FontIcon icon;
  icon.Glyph(glyph);
  icon.FontSize(14);
  return icon;
}

ColumnDefinition PointsAutoColumn() {
  ColumnDefinition col;
  col.Width(GridLengthHelper::Auto());
  return col;
}

// a pane row with the pane's hairline, whose height is its content
Border PointsPaneRow(double padY) {
  Border row;
  row.BorderBrush(colors::BorderBrush());
  row.BorderThickness(ThicknessHelper::FromLengths(0, 0, 0, 1));
  row.Padding(ThicknessHelper::FromLengths(12, padY, 12, padY));
  return row;
}

}  // namespace

namespace {
WalletPage::PointsRow ToPointsRow(urnet::PointsLeaderboardRow const& row) {
  WalletPage::PointsRow out;
  out.networkId = row.network_id.value_or(std::string());
  out.displayName = row.display_name.value_or(std::string());
  out.emojiTag = row.emoji_tag.value_or(std::string());
  out.anonymous = row.anonymous;
  out.totalPointsText = row.total_points_text.value_or(std::string());
  out.blocksText = row.blocks_with_points_text.value_or(std::string());
  out.streakText = row.streak_text.value_or(std::string());
  out.longestStreakText = row.longest_streak_text.value_or(std::string());
  out.rankPointsText = row.rank_points_text.value_or(std::string());
  out.rankBlocksText = row.rank_blocks_text.value_or(std::string());
  out.rankStreakText = row.rank_streak_text.value_or(std::string());
  return out;
}
}  // namespace

void WalletPage::InitializePointsBoard() {
  auto weak = w_.get_weak();
  auto alive = alive_;
  w_.LeaderboardBoardBar().SelectionChanged(
      [weak, alive](SelectorBar const& bar, SelectorBarSelectionChangedEventArgs const&) {
        if (!*alive) return;
        if (auto self = weak.get()) {
          self->wallet().ShowPointsBoard(bar.SelectedItem() == self->PointsBoardItem());
        }
      });
  w_.PointsSortBar().SelectionChanged(
      [weak, alive](SelectorBar const& bar, SelectorBarSelectionChangedEventArgs const&) {
        if (!*alive) return;
        auto self = weak.get();
        if (!self) return;
        auto const item = bar.SelectedItem();
        std::string sort = urnet::PointsLeaderboardSortPoints;
        if (item == self->PointsSortBlocksItem()) {
          sort = urnet::PointsLeaderboardSortBlocks;
        } else if (item == self->PointsSortStreakItem()) {
          sort = urnet::PointsLeaderboardSortStreak;
        }
        self->wallet().OnPointsSortChanged(sort);
      });
  w_.PointsScroll().ViewChanged(
      [weak, alive](IInspectable const&, ScrollViewerViewChangedEventArgs const&) {
        if (!*alive) return;
        if (auto self = weak.get()) self->wallet().OnPointsScroll();
      });
  w_.PointsRetryButton().Click([weak, alive](IInspectable const&, RoutedEventArgs const&) {
    if (!*alive) return;
    if (auto self = weak.get()) self->wallet().OnPointsRetry();
  });
}

void WalletPage::ApplyPointsBoardStrings() {
  w_.DataBoardItem().Text(Loc("data"));
  w_.PointsBoardItem().Text(Loc("points"));
  if (!w_.LeaderboardBoardBar().SelectedItem()) {
    w_.LeaderboardBoardBar().SelectedItem(w_.DataBoardItem());
  }
  w_.PointsSortPointsItem().Text(Loc("points"));
  w_.PointsSortBlocksItem().Text(Loc("blocks"));
  w_.PointsSortStreakItem().Text(Loc("streak"));
  if (!w_.PointsSortBar().SelectedItem()) {
    w_.PointsSortBar().SelectedItem(w_.PointsSortPointsItem());
  }
  w_.PointsRetryButton().Content(LocBox("try_again"));
  w_.PointsStatusText().Text(Loc("loading"));
  w_.PointsStatusText().Visibility(Visibility::Visible);
  BuildPointsNetworkHost();
  RenderPointsHeader();
  RenderPointsFooter();
}

// Pane C's block for the Points board: the group strip with the ranked count,
// the identity line (emoji tag, own name, the pencil), the three dimensions
// each with its rank chip, the longest streak, the opt-in switch with its
// hint, and what the board measures. The Android header card, on the pane's
// row rhythm.
void WalletPage::BuildPointsNetworkHost() {
  auto host = w_.PointsNetworkHost();
  host.Children().Clear();
  auto weak = w_.get_weak();
  auto alive = alive_;
  namespace automation = winrt::Microsoft::UI::Xaml::Automation;

  auto group = kit::MakePaneGroupHeader(Loc("points"));
  pointsGroupMeta_ = group.meta;
  host.Children().Append(group.root);

  // identity
  {
    auto row = PointsPaneRow(10);
    Grid grid;
    grid.ColumnSpacing(10);
    grid.ColumnDefinitions().Append(PointsAutoColumn());
    grid.ColumnDefinitions().Append(StarColumn());
    grid.ColumnDefinitions().Append(PointsAutoColumn());

    pointsEmojiText_ = MakeText(hstring{}, 26, colors::TextBrush());
    pointsEmojiText_.VerticalAlignment(VerticalAlignment::Center);
    Grid::SetColumn(pointsEmojiText_, 0);
    grid.Children().Append(pointsEmojiText_);

    pointsNameText_ = MakeText(hstring{L"-"}, 14, colors::TextBrush());
    pointsNameText_.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
    pointsNameText_.TextTrimming(TextTrimming::CharacterEllipsis);
    pointsNameText_.VerticalAlignment(VerticalAlignment::Center);
    Grid::SetColumn(pointsNameText_, 1);
    grid.Children().Append(pointsNameText_);

    editEmojiButton_ = Button();
    editEmojiButton_.Content(PointsGlyph(L""));
    editEmojiButton_.Width(36);
    editEmojiButton_.Height(36);
    editEmojiButton_.Padding(ThicknessHelper::FromUniformLength(0));
    editEmojiButton_.Click([weak, alive](IInspectable const&, RoutedEventArgs const&) {
      if (!*alive) return;
      if (auto self = weak.get()) self->wallet().OnEditEmoji();
    });
    Grid::SetColumn(editEmojiButton_, 2);
    grid.Children().Append(editEmojiButton_);

    row.Child(grid);
    host.Children().Append(row);
  }

  // the three dimensions, each with its own rank
  {
    auto row = PointsPaneRow(10);
    Grid grid;
    grid.ColumnSpacing(8);
    const hstring labels[3] = {Loc("points"), Loc("blocks"), Loc("streak")};
    for (int i = 0; i < 3; ++i) {
      grid.ColumnDefinitions().Append(StarColumn());
      StackPanel tile;
      tile.Spacing(2);
      tile.Children().Append(MakeText(labels[i], 12, colors::MutedBrush()));
      pointsTiles_[i].value = MakeValue(hstring{L"-"}, 22, colors::FaintBrush());
      tile.Children().Append(pointsTiles_[i].value);
      Border chip;
      chip.CornerRadius(CornerRadiusHelper::FromUniformRadius(4));
      chip.Padding(ThicknessHelper::FromLengths(6, 1, 6, 1));
      chip.HorizontalAlignment(HorizontalAlignment::Left);
      chip.Background(colors::CardBrush());
      pointsTiles_[i].rank = MakeText(hstring{L"-"}, 11, colors::MutedBrush());
      pointsTiles_[i].rank.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
      chip.Child(pointsTiles_[i].rank);
      pointsTiles_[i].chip = chip;
      tile.Children().Append(chip);
      Grid::SetColumn(tile, i);
      grid.Children().Append(tile);
    }
    StackPanel column;
    column.Spacing(6);
    column.Children().Append(grid);
    pointsLongestText_ = MakeText(hstring{}, 12, colors::MutedBrush(), true);
    column.Children().Append(pointsLongestText_);
    row.Child(column);
    host.Children().Append(row);
  }

  // the opt-in switch
  {
    auto row = kit::MakePaneTwoLineRow(Loc("show_on_points_leaderboard"), {}, 44);
    pointsPublicToggle_ = ToggleSwitch();
    pointsPublicToggle_.Style(rows::Lookup(L"UrSwitchToggleStyle"));
    pointsPublicToggle_.Width(44);
    automation::AutomationProperties::SetLabeledBy(pointsPublicToggle_, row.title);
    pointsPublicToggle_.Toggled([weak, alive](IInspectable const&, RoutedEventArgs const&) {
      if (!*alive) return;
      if (auto self = weak.get()) self->wallet().OnPointsPublicToggled();
    });
    row.trailing.Children().Append(pointsPublicToggle_);
    host.Children().Append(row.root);
  }
  {
    auto row = PointsPaneRow(8);
    pointsPrivateHint_ =
        MakeText(Loc("points_leaderboard_private_hint"), 12, colors::MutedBrush(), true);
    row.Child(pointsPrivateHint_);
    host.Children().Append(row);
  }

  // what the board measures
  {
    auto row = PointsPaneRow(8);
    row.Child(MakeText(Loc("points_leaderboard_description"), 12, colors::MutedBrush(), true));
    host.Children().Append(row);
  }
}

void WalletPage::ShowPointsBoard(bool points) {
  pointsBoardShowing_ = points;
  w_.LeaderboardDataHost().Visibility(points ? Visibility::Collapsed : Visibility::Visible);
  w_.PointsHost().Visibility(points ? Visibility::Visible : Visibility::Collapsed);
  w_.DataRankingHost().Visibility(points ? Visibility::Collapsed : Visibility::Visible);
  w_.PointsNetworkHost().Visibility(points ? Visibility::Visible : Visibility::Collapsed);
  ApplyLedgerMeta();
  if (points) EnsurePointsBoard();
}

void WalletPage::EnsurePointsBoard() {
  if (w_.previewUi()) {
    SettlePointsBoardPreview();
    return;
  }
  // the controller lives on the device: no session or no device, no board
  if (!Sdk().IsLoggedIn() || !Sdk().hasDevice()) {
    ClosePointsBoard(/*deviceAlive=*/false);
    w_.PointsStatusText().Text(Loc("please_login_to_urnetwork"));
    w_.PointsStatusText().Visibility(Visibility::Visible);
    return;
  }
  const uint64_t device = Sdk().device().handle();
  if (pointsVc_ && pointsVcDevice_ == device) return;  // still the device it was opened on
  ClosePointsBoard(pointsVcDevice_ == device);
  pointsVcDevice_ = device;
  try {
    pointsVc_.emplace(Sdk().device().openPointsLeaderboardViewController());
  } catch (std::exception const& e) {
    urnw::LogError("points board: could not open the controller: {}", e.what());
    pointsVc_.reset();
    pointsVcDevice_ = 0;
    w_.PointsStatusText().Text(Loc("something_went_wrong"));
    w_.PointsStatusText().Visibility(Visibility::Visible);
    return;
  }
  auto queue = w_.DispatcherQueue();
  auto weak = w_.get_weak();
  auto alive = alive_;
  // the SDK calls from its own thread; the state is read on the UI thread
  pointsSub_.emplace(pointsVc_->addPointsLeaderboardListener([queue, weak, alive] {
    queue.TryEnqueue([weak, alive] {
      if (!*alive) return;
      if (auto self = weak.get()) self->wallet().ReadPointsBoard();
    });
  }));
  pointsVc_->start();
  // a sort picked before the controller existed is applied now
  if (pointsVc_->getSort() != pointsSort_) pointsVc_->setSort(pointsSort_);
  w_.PointsStatusText().Text(Loc("loading"));
  w_.PointsStatusText().Visibility(Visibility::Visible);
  ReadPointsBoard();
}

void WalletPage::ClosePointsBoard(bool deviceAlive) {
  pointsSub_.reset();  // unsubscribes
  if (pointsVc_) {
    // the controller must be closed on the device that opened it; a device
    // that is gone took its controllers with it
    if (deviceAlive && Sdk().hasDevice() && Sdk().device().handle() == pointsVcDevice_) {
      Sdk().device().closePointsLeaderboardViewController(*pointsVc_);
    }
    pointsVc_.reset();
  }
  pointsVcDevice_ = 0;
  pointsRows_.clear();
  pointsHasLoaded_ = false;
  pointsLoading_ = false;
  pointsEnd_ = false;
  pointsError_.clear();
  pointsMe_.reset();
  RenderPointsRows();
  RenderPointsHeader();
  RenderPointsFooter();
}

void WalletPage::ReadPointsBoard() {
  if (!pointsVc_) return;
  std::vector<PointsRow> next;
  try {
    if (auto list = pointsVc_->getRows()) {
      next.reserve(list->size());
      for (auto const& row : *list) next.push_back(ToPointsRow(row));
    }
    pointsSort_ = pointsVc_->getSort();
    if (pointsSort_.empty()) pointsSort_ = urnet::PointsLeaderboardSortPoints;
    pointsLoading_ = pointsVc_->isLoading();
    pointsEnd_ = pointsVc_->isEndReached();
    pointsError_ = pointsVc_->getErrorMessage();
    pointsTotalRanked_ = pointsVc_->getTotalRanked();
    if (auto me = pointsVc_->getMe()) {
      pointsMe_ = me->Row ? std::optional<PointsRow>(ToPointsRow(*me->Row)) : std::nullopt;
      if (ownFlagsAppliedAt_ >= ownFlagsEditedAt_) {
        pointsPublic_ = me->PointsLeaderboardPublic;
        emojiTag_ = pointsMe_ ? pointsMe_->emojiTag : std::string();
      }
    }
  } catch (std::exception const& e) {
    // a malformed document must never take the page down
    urnw::LogError("points board: reading the controller failed: {}", e.what());
    return;
  } catch (...) {
    urnw::LogError("points board: reading the controller failed");
    return;
  }
  const bool rowsChanged = next != pointsRows_ || pointsRenderedSort_ != pointsSort_;
  if (next != pointsRows_) pointsRows_ = std::move(next);
  if (!pointsLoading_ && (!pointsRows_.empty() || pointsEnd_ || !pointsError_.empty())) {
    pointsHasLoaded_ = true;
  }

  // the sort bar follows the controller (a same-sort reselect is a no-op below)
  SelectorBarItem item = w_.PointsSortPointsItem();
  if (pointsSort_ == urnet::PointsLeaderboardSortBlocks) {
    item = w_.PointsSortBlocksItem();
  } else if (pointsSort_ == urnet::PointsLeaderboardSortStreak) {
    item = w_.PointsSortStreakItem();
  }
  if (w_.PointsSortBar().SelectedItem() != item) w_.PointsSortBar().SelectedItem(item);

  if (rowsChanged) RenderPointsRows();
  RenderPointsHeader();
  RenderPointsFooter();
  if (pointsBoardShowing_) ApplyLedgerMeta();

  // a page that does not fill the pane can never be scrolled to its end, so
  // the next one is asked for once layout has run (the controller refuses a
  // second in-flight page and a page past the end)
  if (!pointsLoading_ && !pointsEnd_ && !pointsRows_.empty()) {
    auto weak = w_.get_weak();
    auto alive = alive_;
    w_.DispatcherQueue().TryEnqueue(
        winrt::Microsoft::UI::Dispatching::DispatcherQueuePriority::Low, [weak, alive] {
          if (!*alive) return;
          auto self = weak.get();
          if (!self) return;
          auto& page = self->wallet();
          if (page.pointsVc_ && !page.pointsLoading_ && !page.pointsEnd_ &&
              self->PointsScroll().ScrollableHeight() <= 0) {
            page.pointsVc_->loadMore();
          }
        });
  }
}

void WalletPage::RenderPointsRows() {
  auto rows = w_.PointsRows();
  rows.Children().Clear();
  pointsRenderedSort_ = pointsSort_;
  if (pointsRows_.empty()) return;

  // The same table builder the data board uses; rank and network read as
  // text, the three figures read right.
  const std::vector<double> weights{1, 5, 2, 1, 1};
  rows.Children().Append(kit::MakePaneTableHeader(
      weights, {Loc("current_ranking"), Loc("network"), Loc("points"), Loc("blocks"), Loc("streak")},
      /*textColumns=*/2));

  const bool byBlocks = pointsSort_ == urnet::PointsLeaderboardSortBlocks;
  const bool byStreak = pointsSort_ == urnet::PointsLeaderboardSortStreak;
  const size_t activeColumn = byBlocks ? 3 : (byStreak ? 4 : 2);
  const std::string ownId = pointsMe_ ? pointsMe_->networkId : std::string();
  const hstring anonymous = Loc("anonymous");

  for (auto const& r : pointsRows_) {
    const bool isOwn = !ownId.empty() && r.networkId == ownId;
    auto row = kit::MakePaneTableRow(weights, kPointsRowHeight, /*textColumns=*/2);
    row.cells[0].Text(Utf8(byBlocks ? r.rankBlocksText : (byStreak ? r.rankStreakText : r.rankPointsText)));
    // the emoji tag shows either way; the name only when the network is not anonymous
    const bool anon = r.anonymous || r.displayName.empty();
    std::wstring name = anon ? std::wstring{anonymous} : std::wstring{Utf8(r.displayName)};
    if (!r.emojiTag.empty()) name = std::wstring{Utf8(r.emojiTag)} + L"  " + name;
    row.cells[1].Text(hstring{name});
    row.cells[2].Text(Utf8(r.totalPointsText));
    row.cells[3].Text(Utf8(r.blocksText));
    row.cells[4].Text(Utf8(r.streakText));
    // the sorted figure reads in the text voice; the other two step back
    for (size_t i = 2; i < row.cells.size(); ++i) {
      row.cells[i].Foreground(i == activeColumn ? colors::TextBrush() : colors::MutedBrush());
      if (i == activeColumn) {
        row.cells[i].FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
      }
    }
    if (anon) row.cells[1].Foreground(colors::MutedBrush());
    // the account's own row is the point of the table: colour AND the pane's
    // fill step, because colour alone is never the only signal
    if (isOwn) {
      auto own = colors::MakeBrush(colors::kUrGreen);
      for (auto const& cell : row.cells) cell.Foreground(own);
      row.root.Background(colors::CardBrush());
    }
    rows.Children().Append(row.root);
  }
}

void WalletPage::RenderPointsHeader() {
  if (!pointsNameText_) return;  // not built yet
  namespace automation = winrt::Microsoft::UI::Xaml::Automation;
  const bool hasMe = pointsMe_.has_value();

  kit::SetTextOrCollapse(pointsEmojiText_, Utf8(emojiTag_));
  pointsNameText_.Text(hasMe && !pointsMe_->displayName.empty() ? Utf8(pointsMe_->displayName)
                                                                : hstring{L"-"});
  const hstring editName = Loc(emojiTag_.empty() ? "add_emoji" : "edit_emoji");
  automation::AutomationProperties::SetName(editEmojiButton_, editName);
  ToolTipService::SetToolTip(editEmojiButton_, winrt::box_value(editName));
  kit::SetTextOrCollapse(
      pointsGroupMeta_,
      pointsTotalRanked_ > 0
          ? hstring{urnw::Format("ranked_networks_count",
                                 urnw::Widen(urnet::formatPoints(static_cast<double>(pointsTotalRanked_))))}
          : hstring{});

  const std::string values[3] = {hasMe ? pointsMe_->totalPointsText : std::string(),
                                 hasMe ? pointsMe_->blocksText : std::string(),
                                 hasMe ? pointsMe_->streakText : std::string()};
  const std::string ranks[3] = {hasMe ? pointsMe_->rankPointsText : std::string(),
                                hasMe ? pointsMe_->rankBlocksText : std::string(),
                                hasMe ? pointsMe_->rankStreakText : std::string()};
  const bool emphasized[3] = {pointsSort_ == urnet::PointsLeaderboardSortPoints,
                              pointsSort_ == urnet::PointsLeaderboardSortBlocks,
                              pointsSort_ == urnet::PointsLeaderboardSortStreak};
  winrt::Windows::UI::Color tint = colors::kUrGreen;
  tint.A = 46;  // the chip's green wash behind the sorted dimension
  for (int i = 0; i < 3; ++i) {
    SetStatValue(pointsTiles_[i].value, values[i].empty() ? hstring{L"-"} : Utf8(values[i]),
                 !values[i].empty());
    pointsTiles_[i].rank.Text(ranks[i].empty() ? hstring{L"-"} : Utf8(ranks[i]));
    pointsTiles_[i].rank.Foreground(emphasized[i] ? colors::MakeBrush(colors::kUrGreen)
                                                  : colors::MutedBrush());
    pointsTiles_[i].chip.Background(emphasized[i] ? colors::MakeBrush(tint) : colors::CardBrush());
  }
  kit::SetTextOrCollapse(pointsLongestText_,
                         hasMe ? hstring{std::wstring{Loc("longest_streak")} + L": " +
                                         std::wstring{Utf8(pointsMe_->longestStreakText)}}
                               : hstring{});

  SetPointsToggle(pointsPublic_);
  pointsPublicToggle_.IsEnabled(!settingPointsPublic_);
  pointsPrivateHint_.Visibility(pointsPublic_ ? Visibility::Collapsed : Visibility::Visible);
}

void WalletPage::RenderPointsFooter() {
  const bool showError = !pointsLoading_ && !pointsError_.empty();
  // the page spinner only once there are rows to page after; before the first
  // page the centred status line says "Loading..." on its own
  const bool paging = pointsLoading_ && !pointsRows_.empty();
  w_.PointsFooterRing().IsActive(paging);
  w_.PointsFooterRing().Visibility(paging ? Visibility::Visible : Visibility::Collapsed);
  kit::SetTextOrCollapse(w_.PointsFooterText(), showError ? Utf8(pointsError_) : hstring{});
  w_.PointsRetryButton().Visibility(showError ? Visibility::Visible : Visibility::Collapsed);
  if (!pointsVc_) return;  // the status line already says why there is no board
  if (pointsRows_.empty() && !showError) {
    w_.PointsStatusText().Text(pointsHasLoaded_ ? Loc("points_leaderboard_empty")
                                                : Loc("loading"));
    w_.PointsStatusText().Visibility(Visibility::Visible);
  } else {
    w_.PointsStatusText().Visibility(Visibility::Collapsed);
  }
}

// Switches the sort; the controller clears its rows and reloads.
void WalletPage::OnPointsSortChanged(std::string const& sort) {
  if (sort == pointsSort_ || !urnet::isPointsLeaderboardSort(sort)) return;
  // reflect the chip immediately; the controller confirms on its event
  pointsSort_ = sort;
  if (pointsVc_) pointsVc_->setSort(sort);
  RenderPointsRows();
  RenderPointsHeader();
}

// Asks for the next page when the last visible row is within reach of the end.
void WalletPage::OnPointsScroll() {
  if (!pointsVc_) return;
  auto const scroll = w_.PointsScroll();
  const int64_t rowCount = static_cast<int64_t>(pointsRows_.size());
  const int64_t last = emoji::LastVisibleRow(scroll.VerticalOffset(), scroll.ViewportHeight(),
                                             kPointsTableHeaderHeight, kPointsRowHeight, rowCount);
  if (emoji::ShouldLoadMore(last, rowCount, pointsLoading_, pointsEnd_)) pointsVc_->loadMore();
}

// Retries after an error: the controller re-requests the same page.
void WalletPage::OnPointsRetry() {
  if (!pointsVc_) {
    EnsurePointsBoard();
    return;
  }
  if (pointsRows_.empty()) {
    ownFlagsAppliedAt_ = ++ownFlagsClock_;  // the next `me` is newer than any local edit
    pointsVc_->refresh();
  } else {
    pointsVc_->loadMore();
  }
}

void WalletPage::SetPointsToggle(bool isPublic) {
  if (!pointsPublicToggle_) return;
  applyingPointsToggle_ = true;
  pointsPublicToggle_.IsOn(isPublic);
  applyingPointsToggle_ = false;
}

void WalletPage::OnPointsPublicToggled() {
  // the handler cannot tell a user flip from the programmatic write that
  // renders the answer, so the write sets a flag and this returns
  if (applyingPointsToggle_) return;
  const bool requested = pointsPublicToggle_.IsOn();
  if (requested == pointsPublic_) return;
  if (settingPointsPublic_) {
    SetPointsToggle(pointsPublic_);  // one in flight: snap back
    return;
  }
  if (!CanCallApi()) {
    SetPointsToggle(pointsPublic_);
    RefuseNoSession();
    return;
  }
  settingPointsPublic_ = true;
  pointsPublicToggle_.IsEnabled(false);

  auto alive = alive_;
  const uint32_t generation =
      BeginFlow(pointsPublicFlow_, kApiTimeoutMs, [weak = w_.get_weak(), alive] {
        if (!*alive) return;
        if (auto self = weak.get()) {
          auto& page = self->wallet();
          page.settingPointsPublic_ = false;
          page.pointsPublicToggle_.IsEnabled(true);
          page.SetPointsToggle(page.pointsPublic_);
          page.Notify(Loc("something_went_wrong"), InfoBarSeverity::Error);
        }
      });

  urnet::SetPointsLeaderboardPublicArgs args;
  args.public_ = requested;
  auto queue = w_.DispatcherQueue();
  auto weak = w_.get_weak();
  Sdk().api().setPointsLeaderboardPublic(
      args, [queue, weak, alive, requested,
             generation](std::optional<urnet::SetPointsLeaderboardPublicResult> result,
                         std::optional<std::string> err) {
        std::string error = err ? *err : std::string();
        if (error.empty() && result && result->error) error = result->error->message;
        const bool ok = result && error.empty();
        if (!ok) urnw::LogError("points board: setPointsLeaderboardPublic failed: {}", error);
        queue.TryEnqueue([weak, alive, ok, requested, error, generation] {
          if (!*alive) return;
          if (auto self = weak.get()) {
            self->wallet().ApplyPointsPublicResult(generation, ok, requested, error);
          }
        });
      });
}

void WalletPage::ApplyPointsPublicResult(uint32_t generation, bool ok, bool requested,
                                         std::string const& serverError) {
  if (!SettleFlow(pointsPublicFlow_, generation)) return;
  settingPointsPublic_ = false;
  pointsPublicToggle_.IsEnabled(true);
  if (ok) {
    // the local value wins until a `me` newer than this edit lands
    ownFlagsEditedAt_ = ++ownFlagsClock_;
    pointsPublic_ = requested;
    RenderPointsHeader();
    // the list shows or hides the own row; `me` is re-read too
    if (pointsVc_) {
      ownFlagsAppliedAt_ = ++ownFlagsClock_;
      pointsVc_->refresh();
    }
    return;
  }
  SetPointsToggle(pointsPublic_);
  Notify(serverError.empty() ? Loc("something_went_wrong") : H(serverError),
         InfoBarSeverity::Error);
}

// Stores the tag (already normalized by the SDK), or an empty string to clear
// it; `done` gets the server's message on failure, on the UI thread.
void WalletPage::SaveEmojiTag(std::string tag, std::function<void(std::string)> done) {
  if (savingEmojiTag_) return;
  if (!CanCallApi()) {
    RefuseNoSession();
    done(urnw::Narrow(std::wstring{Loc("please_login_to_urnetwork")}));
    return;
  }
  savingEmojiTag_ = true;
  urnet::SetEmojiTagArgs args;
  args.emoji_tag = tag;
  auto queue = w_.DispatcherQueue();
  auto weak = w_.get_weak();
  auto alive = alive_;
  Sdk().api().setEmojiTag(
      args, [queue, weak, alive, done](std::optional<urnet::SetEmojiTagResult> result,
                                       std::optional<std::string> err) {
        std::string error = err ? *err : std::string();
        if (error.empty() && result && result->error) error = result->error->message;
        if (error.empty() && !result) error = "set emoji tag: no result";
        const std::string stored =
            error.empty() && result && result->emoji_tag ? *result->emoji_tag : std::string();
        if (!error.empty()) urnw::LogError("points board: setEmojiTag failed: {}", error);
        queue.TryEnqueue([weak, alive, done, error, stored] {
          if (!*alive) return;
          auto self = weak.get();
          if (!self) return;
          auto& page = self->wallet();
          page.savingEmojiTag_ = false;
          if (error.empty()) {
            page.ownFlagsEditedAt_ = ++page.ownFlagsClock_;
            page.emojiTag_ = stored;
            page.RenderPointsHeader();
            if (page.pointsVc_) {
              page.ownFlagsAppliedAt_ = ++page.ownFlagsClock_;
              page.pointsVc_->refresh();
            }
          }
          if (done) done(error);
        });
      });
}

winrt::fire_and_forget WalletPage::OnEditEmoji() {
  if (w_.sheetOpen()) co_return;
  if (!CanCallApi()) {
    RefuseNoSession();
    co_return;
  }
  auto self = w_.get_strong();
  auto weak = w_.get_weak();
  auto alive = alive_;
  self->SetSheetOpen(true);
  try {
    emojiSheet_ = urnw::EmojiTagSheet::Create(
        self->Content().XamlRoot(), emojiTag_,
        [weak, alive](std::string tag, std::function<void(std::string)> done) {
          if (!*alive) return;
          if (auto w = weak.get()) w->wallet().SaveEmojiTag(std::move(tag), std::move(done));
        });
    co_await self->wallet().emojiSheet_->Dialog().ShowAsync();
  } catch (winrt::hresult_error const& e) {
    urnw::LogError("points board: the emoji sheet failed to open: {}",
                   urnw::Narrow(std::wstring{e.message()}));
  } catch (...) {
    urnw::LogError("points board: the emoji sheet failed to open");
  }
  self->wallet().emojiSheet_.reset();
  self->SetSheetOpen(false);
}

// --preview-ui: the board on its real empty state, with no controller
void WalletPage::SettlePointsBoardPreview() {
  ClosePointsBoard(/*deviceAlive=*/false);
  pointsHasLoaded_ = true;
  w_.PointsStatusText().Text(Loc("points_leaderboard_empty"));
  w_.PointsStatusText().Visibility(Visibility::Visible);
}

void WalletPage::ApplyProvideState(urnw::LiveStats const& stats) {
  const auto visual = urnw::ProvideModeVisualFor(stats.provideMode, stats.providePaused);
  w_.WalletProvideModeDot().Fill(urnw::colors::MakeBrush(visual.color));
  w_.WalletProvideModeRing().Stroke(urnw::colors::MakeBrush(visual.color));
  w_.WalletProvideModeRing().Visibility(visual.ring ? Visibility::Visible : Visibility::Collapsed);
  // the control mode strings are the store keys of their labels
  w_.WalletProvideModeValue().Text(Loc(Sdk().CurrentProvideControlMode().c_str()));
  // the gate reads the same value the row shows: the provide mode the user
  // picked. Never hides every provider plot behind the disabled message,
  // whatever the device's live provide state says.
  const bool enabled = Sdk().CurrentProvideControlMode() != "never";
  if (enabled == providingEnabled_) return;
  providingEnabled_ = enabled;
  if (enabled) {
    LoadReliability();  // repaint the chart the gate was hiding
  } else {
    ApplyReliability(std::nullopt, Fetch::Ready);  // the gate paints the message
  }
}

}  // namespace urnw
