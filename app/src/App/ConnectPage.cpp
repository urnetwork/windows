// SPDX-License-Identifier: MPL-2.0
#include "pch.h"

#include "ConnectPage.h"

#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Media.Animation.h>
#include <winrt/Microsoft.UI.Xaml.Shapes.h>  // PeerDot Ellipse.Fill

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>

#include "Log.h"
#include "MainWindow.xaml.h"
#include "PageContext.h"
#include "Strings.h"
#include "StatsFormat.h"
#include "UrColors.h"
#include "UrComponents.h"  // kit::SetTextOrCollapse

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace urnw::pages;

namespace urnw {

// winrt::implements makes IInspectable a member typedef of every C++/WinRT
// implementation type, which is why MainWindow could name it unqualified. A
// plain class outside that hierarchy has to bring it in.
using winrt::Windows::Foundation::IInspectable;

namespace {
// country-code case folding for the dns pill: the sdk recommendation/color
// lookups are keyed on the lowercase code; ToUpper is the display fallback when
// the connected location has no country name (StatsSheets parity).
std::string ToLower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}
std::string ToUpper(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  return s;
}

// "AABBCC" / "#AABBCC" / "AARRGGBB" -> Color (fallback muted gray). Mirrors the
// StatsSheets helper; used to fill the dns pill's country-color dot.
winrt::Windows::UI::Color ColorFromHex(std::string hex) {
  if (!hex.empty() && hex[0] == '#') hex = hex.substr(1);
  auto parse = [&](size_t offset) {
    return static_cast<uint8_t>(std::stoul(hex.substr(offset, 2), nullptr, 16));
  };
  try {
    if (hex.size() == 6) return {255, parse(0), parse(2), parse(4)};
    if (hex.size() == 8) return {parse(0), parse(2), parse(4), parse(6)};
  } catch (...) {
  }
  return urnw::colors::kTextMuted;
}

// Two applied dns snapshots are equivalent when every resolver flag and every
// server list matches; an absent list and an empty list are the same. This is
// the same field-for-field comparison DnsEditorSheet makes on its Draft and the
// iOS DnsSettings ==, so the pill agrees with the editor's recommendation panel.
bool DnsSettingsEquivalent(urnet::DnsResolverSettings const& a,
                           urnet::DnsResolverSettings const& b) {
  auto list = [](std::optional<urnet::StringList> const& v) {
    return v ? *v : urnet::StringList{};
  };
  return a.EnableRemoteDoh == b.EnableRemoteDoh && a.EnableLocalDoh == b.EnableLocalDoh &&
         a.EnableRemoteDns == b.EnableRemoteDns && a.EnableLocalDns == b.EnableLocalDns &&
         a.EnableFallback == b.EnableFallback &&
         list(a.RemoteDohUrlsIpv4) == list(b.RemoteDohUrlsIpv4) &&
         list(a.RemoteDohUrlsIpv6) == list(b.RemoteDohUrlsIpv6) &&
         list(a.LocalDohUrlsIpv4) == list(b.LocalDohUrlsIpv4) &&
         list(a.LocalDohUrlsIpv6) == list(b.LocalDohUrlsIpv6) &&
         list(a.RemoteDnsIpv4) == list(b.RemoteDnsIpv4) &&
         list(a.RemoteDnsIpv6) == list(b.RemoteDnsIpv6) &&
         list(a.LocalDnsIpv4) == list(b.LocalDnsIpv4) &&
         list(a.LocalDnsIpv6) == list(b.LocalDnsIpv6);
}
}  // namespace

ConnectPage::ConnectPage(winrt::URnetwork::implementation::MainWindow& window)
    : w_(window) {}

ConnectPage::~ConnectPage() {
  if (chartTimer_) chartTimer_.Stop();
}

void ConnectPage::Initialize() {
  BuildCharts();
  BuildHero();
  WireDrawerFeeds();

  // shared drawer clock: ~10 fps chart redraw, plus 1s relative-time refresh
  chartTimer_ = w_.DispatcherQueue().CreateTimer();
  chartTimer_.Interval(std::chrono::milliseconds(100));
  chartTimer_.Tick([weak = w_.get_weak()](auto const&, auto const&) {
    if (auto self = weak.get()) self->connect().OnChartTick();
  });
}

void ConnectPage::SetPresentationActive(bool active) {
  // the hero's repeating storyboards run on the compositor, so they keep
  // presenting frames for a window nobody is looking at unless they are stopped
  // here as well as the per-frame clock
  if (canvas_) canvas_->SetPresentationActive(active);
  if (!chartTimer_) return;
  if (active) {
    if (!chartTimer_.IsRunning()) chartTimer_.Start();
  } else {
    chartTimer_.Stop();
  }
}

void ConnectPage::ApplyStrings() {
  // status line, dot and button label all come from ApplyConnectStatus, which
  // is the single writer of the three (seeded here: idle, blue dot, "Connect")
  ApplyConnectStatus();
  w_.SelectedProviderLabel().Text(Loc("selected_provider"));
  w_.LocationText().Text(Loc("best_available_provider"));
  ApplyPeerCount(std::nullopt);   // seed the peers status line ("0 peers" + dot)
  w_.BalanceWarning().Title(Loc("insufficient_balance"));
  w_.BalanceWarning().Message(Loc("insufficient_balance_message"));
  w_.ConnectOptionsLabel().Text(Loc("connect_options"));
  w_.ModeAutoItem().Text(Loc("window_type_auto"));
  w_.ModeWebItem().Text(Loc("window_type_quality"));
  w_.ModeStreamingItem().Text(Loc("window_type_speed"));
  w_.ProvideModeLabel().Text(Loc("provide_mode"));
  w_.ProvideAutoItem().Text(Loc("auto"));
  w_.ProvideAlwaysItem().Text(Loc("always"));
  w_.ProvideNetworkItem().Text(Loc("network"));
  w_.ProvideNeverItem().Text(Loc("never"));
  w_.FixedIpLabel().Text(Loc("fixed_ip"));
  w_.StrongAnonLabel().Text(Loc("strong_anonymization"));
  w_.PostQuantumLabel().Text(Loc("post_quantum_encryption"));
  w_.ClientStatsLabel().Text(Loc("client_statistics"));
  w_.LocalStatsLabel().Text(Loc("local_statistics"));
  w_.DnsCardLabel().Text(Loc("custom_dns"));
  // R1: the compact session empty state (shown while disconnected in place of
  // the two zero-value chart cards). "Contracts appear here while connected." is
  // the shipped string closest to the spec's "session statistics will appear
  // after you connect"; the exact copy is a reported store addition.
  w_.SessionEmptyText().Text(Loc("contracts_appear_connected"));
  ApplySessionCardsVisibility(connected_);
  // Each tappable card is a Button now, so it has an automation peer — but with
  // no explicit name UIA falls back to concatenating the entire content
  // subtree, which for the DNS card is nine TextBlocks read as one run-on
  // "name". Name them from the same store keys as their visible labels.
  //
  // The card's LABEL child is marked AccessibilityView="Raw" in the markup so
  // it is not then announced a second time straight after the button's name.
  // Only the label: AccessibilityView is per-element, and the cards' other
  // children are DATA — throughput figures, per-resolver on/off states — which
  // must stay readable. (Blanket Raw on the template's presenter, which
  // URButton does use, would have hidden those too.)
  //
  // PeersLine IS named, from ApplyPeerCount, because its text changes with the
  // count. It looked like it could be dropped - one text child, so surely the
  // automatic name is that text - but dumping the UIA tree said otherwise: a
  // Button whose Content is a Panel gets NO automatic name, and the row came
  // back as an unnamed button. Assumption checked, assumption wrong.
  namespace automation = winrt::Microsoft::UI::Xaml::Automation;
  automation::AutomationProperties::SetName(w_.ClientStatsCard(), Loc("client_statistics"));
  automation::AutomationProperties::SetName(w_.LocalStatsCard(), Loc("local_statistics"));
  automation::AutomationProperties::SetName(w_.DnsCard(), Loc("custom_dns"));
  ApplyLocationRowName();
  w_.DohLabel().Text(Loc("dns_over_https"));
  w_.UdnsLabel().Text(Loc("unencrypted_dns"));
  w_.LdnsLabel().Text(Loc("local_dns"));
  w_.FallbackLabel().Text(Loc("local_dns_fallback"));
  w_.DohState().Text(Loc("off"));
  w_.UdnsState().Text(Loc("off"));
  w_.LdnsState().Text(Loc("off"));
  w_.FallbackState().Text(Loc("off"));
  w_.DnsUnavailableText().Text(Loc("dns_settings_unavailable"));
  w_.BlockerLabel().Text(Loc("block_ads_and_trackers"));
  // drawer plan + usage card (macOS ConnectActions parity)
  w_.DrawerPlanLabel().Text(Loc("plan"));
  w_.DrawerPlanValueText().Text(Loc("free"));
  w_.DrawerGetProButton().Content(LocBox("get_pro"));
  w_.DrawerDailyLabel().Text(Loc("daily_data_balance_label"));
}

// ---- connect -------------------------------------------------------------

void ConnectPage::OnConnectToggle(IInspectable const&, RoutedEventArgs const&) {
  if (ConnectActionIsDisconnect()) {
    Sdk().Disconnect();
    return;
  }
  // Connect to what the user PICKED. This button used to call
  // ConnectBestAvailable() unconditionally, while the chooser's own rows call
  // Connect(location) directly -- so choosing Japan and then pressing Connect
  // silently sent you somewhere else, and the two controls contradicted each
  // other with no way to tell from the UI. Android connects to
  // connectViewModel.selectedLocation; do the same, and fall back to
  // best-available only when there genuinely is no selection (the SDK flags
  // best-available on the selection itself -- LocationSheets
  // IsBestAvailableSelected uses the same test).
  const auto selected = Sdk().SelectedLocation();
  const bool bestAvailable =
      !selected || (selected->connect_location_id &&
                    selected->connect_location_id->best_available.value_or(false));
  // Say "connecting" NOW rather than waiting for the SDK to push it back. On a
  // client that has never run, a Connect press that produces no visible change
  // is indistinguishable from a hang; the next status push corrects this if the
  // SDK disagrees.
  connectStatus_ = ConnectStatus::Connecting;
  ApplyConnectStatus();
  if (bestAvailable)
    Sdk().ConnectBestAvailable();
  else
    Sdk().Connect(*selected);
}

// ---- state relay ---------------------------------------------------------

void ConnectPage::SetNetworkIdentity(std::string const& networkName, bool guestMode) {
  networkName_ = networkName;
  guestMode_ = guestMode;
  ApplyConnectStatus();
}

// The SERVICE tunnel came up or went down. This no longer writes the status
// line: it records the signal and re-renders, so the invariant "the status is
// re-rendered whenever any of its inputs changes" stays true even though the
// current rendering does not read connected_.
//
// connected_ is kept because it is the signal android's displayReconnectTunnel
// needs -- SDK connected but tunnel down, "VPN tunnel disconnected" -- which is
// a separate work item (parity audit §3 item 7). It is deliberately NOT built
// here: the service has never run, so a tunnel that simply never reports Up
// would show a permanent false alarm, which is worse than the omission.
void ConnectPage::SetConnectedUi(bool connected) {
  connected_ = connected;
  ApplyConnectStatus();
}

// "CONNECTED" / "CONNECTING" / "DESTINATION_SET" / "DISCONNECTED", the four
// values getConnectionStatus() emits (android ConnectStatus.fromString folds
// case the same way). Anything unrecognised reads as disconnected: an unknown
// status must not leave the button claiming a connection the SDK never made.
ConnectPage::ConnectStatus ConnectPage::ParseConnectStatus(std::string const& value) {
  const std::string upper = ToUpper(value);
  if (upper == "CONNECTED") return ConnectStatus::Connected;
  if (upper == "CONNECTING") return ConnectStatus::Connecting;
  if (upper == "DESTINATION_SET") return ConnectStatus::DestinationSet;
  return ConnectStatus::Disconnected;
}

bool ConnectPage::ConnectActionIsDisconnect() const {
  // The SDK status ALONE, deliberately -- not connected_. The two are not
  // interchangeable: connected_ is also fed by the service pipe's TunnelStatus
  // (SdkHost wires service_.SetStateHandler straight to onTunnel_), so the
  // tunnel can be up with no destination selected, and the old `if (connected_)`
  // test then offered "Disconnect" while the status line read "Ready to
  // connect" -- leaving no way to connect at all. The button and the line above
  // it must never disagree; both now read the same signal.
  return connectStatus_ != ConnectStatus::Disconnected;
}

// The connect status line, its dot, and the button label — android
// ConnectStatusIndicator parity. Until now StatusText read the SERVICE tunnel
// state and said only "Connected"/"Disconnected", while the SDK's four-state
// connectionStatus was fetched into LiveStats and dropped on the floor: there
// was no connecting state anywhere in the client.
void ConnectPage::ApplyConnectStatus() {
  hstring text;
  winrt::Windows::UI::Color dot = urnw::colors::kStatusIdle;
  // The two balance states iOS's ConnectButtonView layers OVER the connection
  // state, read from the same two fields MainWindow::UpdateBalanceWarning gates
  // the InfoBar on, so the hero and the InfoBar cannot disagree. A running
  // post-checkout confirmation poll wins over an out-of-balance account: the
  // balance is mid-flight, and showing a warning for it would be wrong.
  const bool processing = w_.balanceConfirming();
  const bool outOfBalance = !processing && w_.balanceBlocked();
  switch (connectStatus_) {
    case ConnectStatus::Connected:
      // the provider count lives in its own line below (ProviderCountText),
      // where android folds it into this string — desktop has room for both
      text = Loc("connected");
      dot = urnw::colors::kUrGreen;
      break;
    case ConnectStatus::Connecting:
    case ConnectStatus::DestinationSet:
      // android maps DESTINATION_SET and CONNECTING to one connecting state
      text = Loc("connecting_status_indicator");
      dot = urnw::colors::kStatusConnecting;
      break;
    case ConnectStatus::Disconnected:
      // R1: PROTECTION STATE, not readiness. The owner reconciliation is explicit
      // that "{network} is ready to connect" reads as backend readiness, not
      // protection, and must go. The spec's headline is "Not connected" with a
      // "Your internet traffic is not protected" supporting line; neither ships,
      // so the closest shipped protection word - "Disconnected" - leads instead,
      // and the two missing lines are reported for the store. The network name is
      // no longer folded into the hero headline; it lives in the status strip.
      text = Loc("disconnected");
      dot = urnw::colors::kStatusIdle;
      break;
  }
  w_.StatusText().Text(text);
  w_.StatusDot().Fill(urnw::colors::MakeBrush(dot));
  // The window's status strip shows this same state on every OTHER destination.
  // It reads the derivation rather than repeating it, for the reason the hero
  // and the balance InfoBar already share one: two switches over
  // connectionStatus in two files is two places for them to disagree, and the
  // strip is visible while the connect screen is not.
  w_.ApplyStatusStripConnection(text, dot);
  w_.ConnectButton().Content(ConnectActionIsDisconnect() ? LocBox("disconnect")
                                                         : LocBox("connect"));

  // ---- the hero -----------------------------------------------------------
  // Same inputs, same instant, one function: the canvas is not allowed to lag
  // the line above it.
  auto heroState = urnw::ConnectCanvas::State::Disconnected;
  if (processing) {
    heroState = urnw::ConnectCanvas::State::Processing;
  } else if (outOfBalance) {
    heroState = urnw::ConnectCanvas::State::Error;
  } else {
    switch (connectStatus_) {
      case ConnectStatus::Connected:
        heroState = urnw::ConnectCanvas::State::Connected;
        break;
      case ConnectStatus::Connecting:
      case ConnectStatus::DestinationSet:
        heroState = urnw::ConnectCanvas::State::Connecting;
        break;
      case ConnectStatus::Disconnected:
        heroState = urnw::ConnectCanvas::State::Disconnected;
        break;
    }
  }
  // --preview-ui drives the walk itself; letting the real status overwrite it
  // would pin the preview to Disconnected forever (there is no session).
  if (canvas_ && !PreviewHeroActive()) canvas_->SetState(heroState);

  // A Button whose Content is a Panel gets NO automatic name (this project
  // already paid for that lesson on PeersLine), and the hero's content is a
  // decorative canvas marked Raw. Name it after the state it is showing, which
  // is the one thing it is for.
  winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(w_.ConnectHero(),
                                                                       text);

  // ---- the transition -----------------------------------------------------
  // The connect action is disabled while the SDK reports CONNECTING: the press
  // has been accepted, and a second one fires a duplicate connect (android
  // gates on DISCONNECTED for the same reason). DESTINATION_SET and CONNECTED
  // are settled — Disconnect is a legitimate action in both — so only the
  // genuinely transitional state disables.
  //
  // It is a WATCHDOG, not a latch. The earlier code kept the button enabled
  // throughout on the grounds that a connect which hangs must not leave a dead
  // control, and that reasoning is right; it is preserved here by re-enabling
  // after kConnectWatchdog rather than by never disabling at all.
  constexpr auto kConnectWatchdog = std::chrono::seconds(8);
  const bool transitional = connectStatus_ == ConnectStatus::Connecting;
  if (!transitional) {
    connectingSince_ = {};
    connectWatchdogFired_ = false;
  } else if (connectingSince_ == std::chrono::steady_clock::time_point{}) {
    connectingSince_ = std::chrono::steady_clock::now();
    connectWatchdogFired_ = false;
  } else if (kConnectWatchdog < std::chrono::steady_clock::now() - connectingSince_) {
    connectWatchdogFired_ = true;
  }
  // out of balance / mid-poll: there is nothing a connect press can do, and iOS
  // blocks the tap in exactly these two cases
  const bool blocked = processing || outOfBalance;
  const bool enabled = !blocked && (!transitional || connectWatchdogFired_);
  w_.ConnectButton().IsEnabled(enabled);
  w_.ConnectHero().IsEnabled(enabled);
}

// ---- live stats (macOS parity) -------------------------------------------

void ConnectPage::ApplyStats(urnw::LiveStats const& stats) {
  // Selected provider row. When the selected location is a connected network
  // peer, show its device name instead of the raw client id (req4): resolve it
  // from the live peer list by client id, like the linux drawer does.
  std::string locationName = stats.locationName;
  const auto peers = Sdk().ConnectedProvidePeers();
  if (auto selected = Sdk().SelectedLocation();
      peers && selected && selected->connect_location_id &&
      selected->connect_location_id->client_id &&
      !selected->connect_location_id->client_id->empty()) {
    const auto& clientId = *selected->connect_location_id->client_id;
    for (const auto& peer : *peers) {
      if (peer.ClientId && *peer.ClientId == clientId) {
        locationName = urnw::PeerDisplayName(peer);
        break;
      }
    }
  }
  w_.LocationText().Text(locationName.empty() ? Loc("best_available_provider")
                                              : H(locationName));
  ApplyLocationRowName();  // the row's name carries the provider, not just the label
  // The SDK's connection status: the only signal in the client that carries a
  // CONNECTING state. It was read into LiveStats and never used.
  connectStatus_ = ParseConnectStatus(stats.connectionStatus);
  // The provider grid, into the hero. This is the first consumer
  // getProviderGridPointList() has ever had in this client. An empty list is
  // normal (no session, rpc-only, or a connection that has not placed a
  // provider yet) and the canvas renders it as its bare lattice, so it is fed
  // through unconditionally rather than gated on non-empty.
  if (canvas_ && !PreviewHeroActive()) {
    canvas_->SetGrid(stats.gridPoints, stats.gridWidth, stats.gridHeight);
  }
  ApplyConnectStatus();
  ApplyPeerCount(peers);  // the peers status line below the connect button (req1)
  // the connected country drives the dns-card recommendation pill; only refresh
  // it when the country actually changes (stats push on every throughput tick).
  const bool countryChanged =
      countryCode_ != stats.countryCode || countryName_ != stats.countryName;
  countryCode_ = stats.countryCode;
  countryName_ = stats.countryName;
  if (countryChanged) ApplyDnsRecommendationPill();

  // Provider window size ("Connected to N providers"), like macOS. The count is a
  // CLDR plural in the store: select the form, never inflect here.
  //
  // SetTextOrCollapse, not Text: both of these lines are empty while
  // disconnected, and an empty TextBlock in a StackPanel still spends the
  // panel's Spacing. Together with the two below they left ~120px of blank card
  // in the middle of the screen the app opens on. The whole group carries a
  // divider, so it is hidden as a unit - a rule with nothing under it is the
  // same defect, one pixel tall.
  urnw::kit::SetTextOrCollapse(
      w_.ProviderCountText(),
      stats.connected
          ? hstring{urnw::Plural("connected_provider_count", stats.providerCount)}
          : hstring{L""});

  // Live throughput feed: down / up bit rate. Arrows + rates, no prose.
  urnw::kit::SetTextOrCollapse(
      w_.ThroughputText(),
      stats.connected ? H("↓ " + urnw::FormatBitRate(stats.downBitsPerSecond) +
                          "    ↑ " + urnw::FormatBitRate(stats.upBitsPerSecond))
                      : hstring(L""));
  w_.LiveStatsGroup().Visibility(stats.connected ? Visibility::Visible
                                                 : Visibility::Collapsed);
  // R1: the chart cards vs the compact empty state, on the same connected signal.
  ApplySessionCardsVisibility(stats.connected);

  // Insufficient-balance warning (auto-disconnect happens in the SDK). The
  // action button opens the upgrade flow; Pro / a running confirmation poll
  // suppress it (MainWindow::UpdateBalanceWarning).
  w_.SetInsufficientBalance(stats.insufficientBalance);

  // Provide stats.
  hstring provide{L""};
  if (stats.provideEnabled) {
    provide = stats.providePaused
                  ? Loc("providing_paused")
                  : hstring{urnw::Plural("providing_client_count", stats.provideClients)};
  }
  urnw::kit::SetTextOrCollapse(w_.ProvideStatsText(), provide);

  // provide indicator (apple parity). The effective provide mode is a bit set
  // (0 none, 1 network, 2 friends-and-family, 3 public) — per-case only.
  // Solid dot = Network tier; dot + outer ring = Public tier (amber while
  // paused — pause stops public only); coral = not providing.
  auto provideColor = urnw::colors::kUrCoral;
  bool provideRing = false;
  switch (stats.provideMode) {
    case 3:  // public
      provideColor = stats.providePaused ? urnw::colors::kUrAmber : urnw::colors::kUrGreen;
      provideRing = true;
      break;
    case 1:  // network (also Auto while idle)
    case 2:  // friends-and-family
      provideColor = urnw::colors::kUrGreen;
      break;
    default:
      break;
  }
  // discoverability line (apple/android parity): a paused device stays
  // discoverable — pause stops public provide only
  w_.DiscoverableText().Text(Loc(stats.provideEnabled && stats.provideHasNetworkKey
                                     ? "device_discoverable"
                                     : "device_not_discoverable"));
  w_.ProvideModeDot().Fill(urnw::colors::MakeBrush(provideColor));
  w_.ProvideModeRing().Stroke(urnw::colors::MakeBrush(provideColor));
  w_.ProvideModeRing().Visibility(provideRing ? Visibility::Visible
                                              : Visibility::Collapsed);
}

// ---- connect drawer --------------------------------------------------------
// macOS ConnectActions parity: three stats cards over live SDK feeds, the
// blocker toggle, the connect options (performance profile), and the plan +
// usage card.

// ---- hero canvas ----------------------------------------------------------

void ConnectPage::BuildHero() {
  // The hero is decorative. If building it throws, the connect page must still
  // come up: an exception escaping here takes Initialize() with it, and the
  // drawer feeds and the chart clock are wired AFTER this call — so a broken
  // hero would silently cost the whole page its live data. (That is exactly
  // what happened once during development, and the symptom was not "no hero",
  // it was "no hero and nothing updates".)
  try {
    canvas_ = std::make_unique<urnw::ConnectCanvas>(w_.ConnectCanvasHost());
  } catch (winrt::hresult_error const& e) {
    urnw::LogError("connect: hero canvas failed to build (hresult 0x{:08x}): {}",
                   static_cast<uint32_t>(e.code()), urnw::Narrow(e.message().c_str()));
    canvas_.reset();
    return;
  } catch (std::exception const& e) {
    urnw::LogError("connect: hero canvas failed to build: {}", e.what());
    canvas_.reset();
    return;
  }
  urnw::LogInfo("connect: hero canvas built");

  // Desktop affordances, wired here rather than in the markup so the hero adds
  // no new MainWindow handler surface. `this` outlives these handlers: the page
  // is owned by the window that owns the button, and the whole tree goes at
  // once.
  auto hero = w_.ConnectHero();
  hero.PointerEntered([this](IInspectable const&, auto const&) {
    if (canvas_) canvas_->SetHovered(true);
  });
  hero.PointerExited([this](IInspectable const&, auto const&) {
    if (canvas_) canvas_->SetHovered(false);
  });
  hero.GotFocus([this](IInspectable const&, RoutedEventArgs const&) {
    // keyboard focus only. A focus ring drawn on a mouse press is noise; the
    // platform draws its own focus visuals the same way.
    if (canvas_) {
      canvas_->SetFocusRingVisible(w_.ConnectHero().FocusState() == FocusState::Keyboard);
    }
  });
  hero.LostFocus([this](IInspectable const&, RoutedEventArgs const&) {
    if (canvas_) canvas_->SetFocusRingVisible(false);
  });
}

// --preview-ui only, and only with URNETWORK_PREVIEW_HERO set. Two gates, both
// required: the preview flag says there is no session, and the env var says the
// operator explicitly asked for synthetic content. Nothing below touches Sdk(),
// the network, or any stored state — it generates points in this process.
bool ConnectPage::PreviewHeroActive() const {
  if (!w_.previewUi()) return false;
  wchar_t buffer[8]{};
  const DWORD n = ::GetEnvironmentVariableW(L"URNETWORK_PREVIEW_HERO", buffer, 8);
  return 0 < n && n < 8;
}

void ConnectPage::PreviewHeroTick() {
  if (!canvas_) return;
  // Walk the five states on a 4s cadence and churn a synthetic grid underneath
  // them, because a still frame cannot show whether the motion is right.
  static uint32_t frame = 0;
  ++frame;
  const uint32_t phase = (frame / 40) % 5;
  const std::array<urnw::ConnectCanvas::State, 5> walk = {
      urnw::ConnectCanvas::State::Disconnected, urnw::ConnectCanvas::State::Connecting,
      urnw::ConnectCanvas::State::Connected, urnw::ConnectCanvas::State::Error,
      urnw::ConnectCanvas::State::Processing};
  canvas_->SetState(walk[phase]);

  // one synthetic grid push per second, so the point transitions are visible
  if (frame % 10 != 0) return;
  constexpr int32_t kCols = 14;
  std::vector<urnet::ProviderGridPoint> points;
  // a cheap deterministic hash, so the walk is reproducible across runs
  auto hash = [](uint32_t v) { return v * 2654435761u; };
  const uint32_t seed = frame / 10;
  for (int32_t y = 0; y < kCols; ++y) {
    for (int32_t x = 0; x < kCols; ++x) {
      const uint32_t h = hash(static_cast<uint32_t>(x * 131 + y * 17) ^ hash(seed));
      if ((h >> 8) % 100 < 42) continue;  // not every cell is occupied
      urnet::ProviderGridPoint p;
      p.X = x;
      p.Y = y;
      p.ClientId = "preview-" + std::to_string(x) + "-" + std::to_string(y);
      switch ((h >> 3) % 8) {
        case 0: p.State = "InEvaluation"; break;
        case 1: p.State = "EvaluationFailed"; break;
        case 2: p.State = "NotAdded"; break;
        default: p.State = "Added"; break;
      }
      p.Active = true;
      points.push_back(p);
    }
  }
  canvas_->SetGrid(points, kCols, kCols);
}

void ConnectPage::BuildCharts() {
  remoteChart_ = std::make_unique<urnw::TransferChart>(
      w_.RemoteChartHost(), urnw::Localized("remote"), urnw::ThroughputRoute::Remote,
      urnw::colors::kUrGreen, urnw::colors::kUrPink);
  blockedChart_ = std::make_unique<urnw::TransferChart>(
      w_.BlockedChartHost(), urnw::Localized("blocked"), urnw::ThroughputRoute::Block,
      urnw::colors::kUrCoral, urnw::colors::kUrMutedCoral);
  localChart_ = std::make_unique<urnw::TransferChart>(
      w_.LocalChartHost(), urnw::Localized("local"), urnw::ThroughputRoute::Local,
      urnw::colors::kUrGreen, urnw::colors::kUrPink);
}

void ConnectPage::WireDrawerFeeds() {
  // SdkHost handlers fire on SDK callback threads. Capture the (agile)
  // DispatcherQueue here on the UI thread and hop through it, resolving the
  // weak window ref only on the UI side.
  auto queue = w_.DispatcherQueue();
  auto weak = w_.get_weak();
  auto& sdk = Sdk();

  sdk.SetThroughputHandler([queue, weak](std::vector<urnet::ThroughputPoint> points,
                                         int64_t windowSeconds) {
    queue.TryEnqueue([weak, points = std::move(points), windowSeconds] {
      if (auto self = weak.get()) {
        auto& page = self->connect();
        page.remoteChart_->SetPoints(points, windowSeconds);
        page.blockedChart_->SetPoints(points, windowSeconds);
        page.localChart_->SetPoints(points, windowSeconds);
      }
    });
  });
  // The ContractDetailsViewController already coalesces the egress + ingress
  // change streams into one settled ContractRowsChanged (no intermediate
  // one-list-updated aggregate reaches us), so the UI can apply each push
  // directly -- re-reading the settled snapshot on the UI thread (macOS
  // ContractDetailsStore.update parity).
  sdk.SetContractRowsHandler([queue, weak](std::vector<urnw::ContractPeerRow>) {
    queue.TryEnqueue([weak] {
      if (auto self = weak.get()) {
        auto& page = self->connect();
        page.contractRows_ = Sdk().CurrentContractRows();
        if (page.contractsSheet_) page.contractsSheet_->Update(page.contractRows_);
      }
    });
  });
  sdk.SetBlockActionsHandler([queue, weak](std::vector<urnw::BlockActionItem> actions) {
    queue.TryEnqueue([weak, actions = std::move(actions)] {
      if (auto self = weak.get()) {
        auto& page = self->connect();
        page.blockActions_ = actions;
        if (page.splitRulesSheet_) {
          page.splitRulesSheet_->Update(page.splitRules_, page.blockActions_,
                                        page.allowedCount_, page.blockedCount_);
        }
      }
    });
  });
  sdk.SetBlockStatsHandler([queue, weak](int64_t allowed, int64_t blocked) {
    queue.TryEnqueue([weak, allowed, blocked] {
      if (auto self = weak.get()) {
        auto& page = self->connect();
        page.allowedCount_ = allowed;
        page.blockedCount_ = blocked;
        if (page.splitRulesSheet_) {
          page.splitRulesSheet_->Update(page.splitRules_, page.blockActions_, allowed,
                                        blocked);
        }
      }
    });
  });
  sdk.SetSplitRulesHandler([queue, weak](std::vector<urnw::SplitRule> rules) {
    queue.TryEnqueue([weak, rules = std::move(rules)] {
      if (auto self = weak.get()) {
        auto& page = self->connect();
        page.splitRules_ = rules;
        page.ApplySplitRuleCount();
        if (page.splitRulesSheet_) {
          page.splitRulesSheet_->Update(page.splitRules_, page.blockActions_,
                                        page.allowedCount_, page.blockedCount_);
        }
      }
    });
  });
  sdk.SetDnsSettingsHandler([queue, weak](std::optional<urnet::DnsResolverSettings> settings) {
    queue.TryEnqueue([weak, settings = std::move(settings)] {
      if (auto self = weak.get()) {
        auto& page = self->connect();
        page.dnsSettings_ = settings;
        page.ApplyDnsCard(settings);
      }
    });
  });
  sdk.SetBlockerEnabledHandler([queue, weak](bool on) {
    queue.TryEnqueue([weak, on] {
      if (auto self = weak.get()) self->connect().ApplyBlockerUi(on);
    });
  });
  // location/provider chooser: the bucketed locations feed the open sheet; the
  // peers feed both the sheet's pinned section and the drawer's peer-count label
  sdk.SetLocationsHandler([queue, weak](std::optional<urnet::FilteredLocations> locations,
                                        std::string) {
    queue.TryEnqueue([weak, locations = std::move(locations)] {
      if (auto self = weak.get(); self && self->connect().locationSheet_) {
        self->connect().locationSheet_->Update(locations, Sdk().ConnectedProvidePeers());
      }
    });
  });
  sdk.SetPeersHandler([queue, weak](std::optional<urnet::NetworkPeerList> peers) {
    queue.TryEnqueue([weak, peers = std::move(peers)] {
      if (auto self = weak.get()) {
        auto& page = self->connect();
        page.ApplyPeerCount(peers);
        if (page.locationSheet_) {
          page.locationSheet_->Update(Sdk().CurrentFilteredLocations(), peers);
        }
      }
    });
  });
  sdk.SetRemoteChangedHandler([queue, weak](bool) {
    queue.TryEnqueue([weak] {
      if (auto self = weak.get()) {
        // remote attach/detach flips the peers line's disabled state; the
        // nullopt trigger leaves the chooser's peer rows untouched
        self->connect().ApplyPeerCount(std::nullopt);
      }
    });
  });
}

void ConnectPage::ResyncDrawer() {
  auto& sdk = Sdk();
  // open the locations + peers feeds so the drawer's "N network peers" label is
  // live from login, not only after the chooser is first opened (idempotent and
  // session-guarded; one provider fetch per session, matching the Linux app).
  sdk.EnsureLocations();
  int64_t windowSeconds = 60;
  auto points = sdk.CurrentThroughputPoints(windowSeconds);
  remoteChart_->SetPoints(points, windowSeconds);
  blockedChart_->SetPoints(points, windowSeconds);
  localChart_->SetPoints(points, windowSeconds);
  contractRows_ = sdk.CurrentContractRows();
  blockActions_ = sdk.CurrentBlockActions();
  sdk.CurrentBlockCounts(allowedCount_, blockedCount_);
  splitRules_ = sdk.CurrentSplitRules();
  dnsSettings_ = sdk.CurrentDnsSettings();
  ApplySplitRuleCount();
  ApplyDnsCard(dnsSettings_);
  SeedConnectControls();
}

void ConnectPage::SeedConnectControls() {
  updatingControls_ = true;
  const urnw::PerformanceSettings settings = Sdk().CurrentPerformanceSettings();
  switch (settings.mode) {
    case urnw::ConnectionMode::Auto:
      w_.ConnectionModeBar().SelectedItem(w_.ModeAutoItem());
      break;
    case urnw::ConnectionMode::Web:
      w_.ConnectionModeBar().SelectedItem(w_.ModeWebItem());
      break;
    case urnw::ConnectionMode::Streaming:
      w_.ConnectionModeBar().SelectedItem(w_.ModeStreamingItem());
      break;
  }
  w_.FixedIpToggle().IsOn(settings.fixedIp);
  w_.FixedIpToggle().IsEnabled(settings.mode != urnw::ConnectionMode::Auto);
  // "Strong Anonymization" is the inverse of allowDirect
  w_.StrongAnonToggle().IsOn(!settings.allowDirect);
  w_.PostQuantumToggle().IsOn(settings.postQuantum);
  w_.BlockerToggle().IsOn(Sdk().CurrentBlockerEnabled());
  // provide control mode ("manual"/unknown land on Never, the SDK's
  // conservative default case)
  const std::string provideMode = Sdk().CurrentProvideControlMode();
  if (provideMode == "auto") {
    w_.ProvideModeBar().SelectedItem(w_.ProvideAutoItem());
  } else if (provideMode == "always") {
    w_.ProvideModeBar().SelectedItem(w_.ProvideAlwaysItem());
  } else if (provideMode == "network") {
    w_.ProvideModeBar().SelectedItem(w_.ProvideNetworkItem());
  } else {
    w_.ProvideModeBar().SelectedItem(w_.ProvideNeverItem());
  }
  updatingControls_ = false;
}

urnw::ConnectionMode ConnectPage::SelectedMode() {
  auto selected = w_.ConnectionModeBar().SelectedItem();
  if (selected == w_.ModeWebItem()) return urnw::ConnectionMode::Web;
  if (selected == w_.ModeStreamingItem()) return urnw::ConnectionMode::Streaming;
  return urnw::ConnectionMode::Auto;
}

void ConnectPage::PushPerformanceSettings() {
  urnw::PerformanceSettings settings;
  settings.mode = SelectedMode();
  settings.fixedIp = w_.FixedIpToggle().IsOn();
  settings.allowDirect = !w_.StrongAnonToggle().IsOn();
  settings.postQuantum = w_.PostQuantumToggle().IsOn();
  Sdk().SetPerformanceSettings(settings);
}

void ConnectPage::OnConnectionModeChanged(SelectorBar const&,
                                          SelectorBarSelectionChangedEventArgs const&) {
  if (updatingControls_) return;
  const urnw::ConnectionMode mode = SelectedMode();
  if (mode == urnw::ConnectionMode::Auto && w_.FixedIpToggle().IsOn()) {
    // Auto forces Fixed IP off (macOS parity); update quietly, push once below
    updatingControls_ = true;
    w_.FixedIpToggle().IsOn(false);
    updatingControls_ = false;
  }
  w_.FixedIpToggle().IsEnabled(mode != urnw::ConnectionMode::Auto);
  PushPerformanceSettings();
}

std::string ConnectPage::SelectedProvideMode() {
  auto selected = w_.ProvideModeBar().SelectedItem();
  if (selected == w_.ProvideAutoItem()) return "auto";
  if (selected == w_.ProvideAlwaysItem()) return "always";
  if (selected == w_.ProvideNetworkItem()) return "network";
  return "never";
}

void ConnectPage::OnProvideModeChanged(SelectorBar const&,
                                       SelectorBarSelectionChangedEventArgs const&) {
  if (updatingControls_) return;
  Sdk().SetProvideControlMode(SelectedProvideMode());
}

void ConnectPage::OnFixedIpToggled(IInspectable const&, RoutedEventArgs const&) {
  if (updatingControls_) return;
  PushPerformanceSettings();
}

void ConnectPage::OnStrongAnonToggled(IInspectable const&, RoutedEventArgs const&) {
  if (updatingControls_) return;
  PushPerformanceSettings();
}

void ConnectPage::OnPostQuantumToggled(IInspectable const&, RoutedEventArgs const&) {
  if (updatingControls_) return;
  PushPerformanceSettings();
}

void ConnectPage::OnBlockerToggled(IInspectable const&, RoutedEventArgs const&) {
  if (updatingControls_) return;
  // the device applies and persists the blocker; the app stores nothing
  Sdk().SetBlockerEnabled(w_.BlockerToggle().IsOn());
}

void ConnectPage::ApplyBlockerUi(bool on) {
  if (w_.BlockerToggle().IsOn() == on) return;
  updatingControls_ = true;
  w_.BlockerToggle().IsOn(on);
  updatingControls_ = false;
}

// R1: the two live-chart cards read as zero-value graphs while disconnected -
// the spec's oversized empty cards - so they yield to one compact empty state
// until there is a session to chart. The DNS card stays either way: it shows a
// device setting, not session data. One writer, from ApplyStats and ApplyStrings.
void ConnectPage::ApplySessionCardsVisibility(bool connected) {
  const auto cards = connected ? Visibility::Visible : Visibility::Collapsed;
  const auto empty = connected ? Visibility::Collapsed : Visibility::Visible;
  w_.ClientStatsCard().Visibility(cards);
  w_.LocalStatsCard().Visibility(cards);
  w_.SessionEmptyCard().Visibility(empty);
}

// "Selected provider, Berlin". Naming the row after its LABEL alone left a
// screen reader announcing "Selected provider, button" — the label is already
// on screen and marked Raw, and the one thing the row is actually for, which
// provider is selected, was the part it omitted.
void ConnectPage::ApplyLocationRowName() {
  winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(
      w_.LocationRow(),
      hstring{urnw::Localized("selected_provider") + L", " +
              std::wstring{w_.LocationText().Text()}});
}

void ConnectPage::ApplySplitRuleCount() {
  w_.SplitRuleCountText().Text(
      hstring{urnw::Plural("split_rule_count", static_cast<int64_t>(splitRules_.size()))});
}

void ConnectPage::ApplyDnsCard(std::optional<urnet::DnsResolverSettings> const& settings) {
  w_.DnsRowsPanel().Visibility(settings ? Visibility::Visible : Visibility::Collapsed);
  w_.DnsUnavailableText().Visibility(settings ? Visibility::Collapsed : Visibility::Visible);
  // the applied settings just changed: re-evaluate the recommendation pill (it
  // reads dnsSettings_, already updated to `settings` by the caller). Runs in
  // the unavailable path too so the pill collapses with the rows.
  ApplyDnsRecommendationPill();
  if (!settings) return;

  // looked up once for the four rows; the lambda runs here, so capturing by
  // reference is safe
  const hstring onText = Loc("on");
  const hstring offText = Loc("off");
  auto applyRow = [&onText, &offText](Microsoft::UI::Xaml::Shapes::Ellipse const& dot,
                                      TextBlock const& state, bool on) {
    dot.Fill(urnw::colors::MakeBrush(
        on ? urnw::colors::kUrGreen
           : urnw::colors::WithAlpha(urnw::colors::kTextFaint, 102)));
    state.Text(on ? onText : offText);
    state.Foreground(on ? urnw::colors::MakeBrush(urnw::colors::kUrGreen)
                        : urnw::colors::MutedBrush());
  };
  applyRow(w_.DohDot(), w_.DohState(), settings->EnableRemoteDoh || settings->EnableLocalDoh);
  applyRow(w_.UdnsDot(), w_.UdnsState(), settings->EnableRemoteDns || settings->EnableLocalDns);
  applyRow(w_.LdnsDot(), w_.LdnsState(), settings->EnableLocalDoh || settings->EnableLocalDns);
  applyRow(w_.FallbackDot(), w_.FallbackState(), settings->EnableFallback);
}

// The unapplied-recommendation pill atop the dns card (iOS DnsRecommendationPill
// parity). Priority, matching the iOS computed `recommendation`:
//   1. no applied settings -> hidden (nothing to compare; the card shows
//      "unavailable").
//   2. a connected country whose regional recommendation differs from the
//      applied settings -> pill "...recommended settings for {country}" with the
//      country-color dot. If that recommendation IS already applied, hide and do
//      NOT fall through to the default nudge.
//   3. otherwise (no country, or the country has no regional recommendation) and
//      the safe defaults are not applied -> pill "default safe settings are not
//      applied", no dot.
//   4. hidden otherwise.
void ConnectPage::ApplyDnsRecommendationPill() {
  const auto& current = dnsSettings_;
  if (!current) {
    w_.DnsRecPill().Visibility(Visibility::Collapsed);
    return;
  }
  if (!countryCode_.empty()) {
    const std::string code = ToLower(countryCode_);
    if (auto rec = urnet::getRecommendedDnsResolverSettings(code)) {
      if (!DnsSettingsEquivalent(*current, *rec)) {
        const std::wstring name = countryName_.empty() ? urnw::Widen(ToUpper(countryCode_))
                                                       : urnw::Widen(countryName_);
        w_.DnsRecText().Text(hstring{urnw::Format("dns_pill_recommended", name)});
        w_.DnsRecDot().Fill(urnw::colors::MakeBrush(ColorFromHex(urnet::getColorHex(code))));
        w_.DnsRecDot().Visibility(Visibility::Visible);
        w_.DnsRecPill().Visibility(Visibility::Visible);
      } else {
        w_.DnsRecPill().Visibility(Visibility::Collapsed);
      }
      return;  // the country has a recommendation: never fall through to defaults
    }
  }
  if (auto def = urnet::getDefaultDnsResolverSettings();
      def && !DnsSettingsEquivalent(*current, *def)) {
    w_.DnsRecText().Text(Loc("dns_pill_default"));
    w_.DnsRecDot().Visibility(Visibility::Collapsed);
    w_.DnsRecPill().Visibility(Visibility::Visible);
    return;
  }
  w_.DnsRecPill().Visibility(Visibility::Collapsed);
}

void ConnectPage::OnChartTick() {
  // skip the redraw work while the window is hidden (tray) or on another tab
  if (!w_.Visible()) return;
  if (w_.ConnectView().Visibility() != Visibility::Visible && !w_.sheetOpen()) return;
  if (w_.ConnectView().Visibility() == Visibility::Visible) {
    remoteChart_->Tick();
    blockedChart_->Tick();
    localChart_->Tick();
    // the hero's only per-frame path; it returns immediately unless a point
    // transition is in flight
    if (canvas_) canvas_->Tick();
    if (PreviewHeroActive()) PreviewHeroTick();
    // the connect watchdog: re-render so a transition that has outlived
    // kConnectWatchdog gives the control back
    if (connectStatus_ == ConnectStatus::Connecting && !connectWatchdogFired_) {
      ApplyConnectStatus();
    }
  }
  if (contractsSheet_) contractsSheet_->Tick();  // ring/disc easing + slide animations
  if (++chartTickCount_ % 10 == 0) {  // ~1s cadence
    if (splitRulesSheet_) splitRulesSheet_->RefreshTimes();  // "Ns ago" labels
  }
  // the contract-details activity resort now lives in the SDK view controller;
  // the sheet just reports scroll and renders the ordered rows (no local tick)
}

void ConnectPage::AnimateDrawerIn() {
  // fade + slight slide-up, 300ms ease-out, staggered across the cards. Played
  // once per window: replaying would fight the finished animations' hold
  // values and flash the cards.
  if (drawerAnimated_) return;
  drawerAnimated_ = true;
  namespace anim = winrt::Microsoft::UI::Xaml::Media::Animation;
  // The drawer's modules, in the order they appear. BlockerCard is gone - the
  // ad/tracker switch is a row in ConnectOptionsCard now, with the other three
  // per-connection switches, instead of being a card of its own with one line
  // in it.
  const std::array<FrameworkElement, 7> cards = {
      w_.ControlsCard(),        w_.ProvideCard(),     w_.ConnectOptionsCard(),
      w_.ClientStatsCard(),     w_.LocalStatsCard(),  w_.DnsCard(),
      w_.DrawerPlanCard()};
  const auto duration = Duration{std::chrono::duration_cast<winrt::Windows::Foundation::TimeSpan>(
                                     std::chrono::milliseconds(300)),
                                 DurationType::TimeSpan};
  int index = 0;
  for (auto const& card : cards) {
    auto shift = Media::TranslateTransform();
    shift.Y(16);
    card.RenderTransform(shift);
    card.Opacity(0);

    anim::CubicEase ease;
    ease.EasingMode(anim::EasingMode::EaseOut);
    const auto beginTime = std::chrono::duration_cast<winrt::Windows::Foundation::TimeSpan>(
        std::chrono::milliseconds(50 * index));

    anim::Storyboard storyboard;
    anim::DoubleAnimation fade;
    fade.From(0.0);
    fade.To(1.0);
    fade.Duration(duration);
    fade.BeginTime(beginTime);
    fade.EasingFunction(ease);
    anim::Storyboard::SetTarget(fade, card);
    anim::Storyboard::SetTargetProperty(fade, L"Opacity");
    storyboard.Children().Append(fade);

    anim::DoubleAnimation slide;
    slide.From(16.0);
    slide.To(0.0);
    slide.Duration(duration);
    slide.BeginTime(beginTime);
    slide.EasingFunction(ease);
    anim::Storyboard::SetTarget(slide, shift);
    anim::Storyboard::SetTargetProperty(slide, L"Y");
    storyboard.Children().Append(slide);

    storyboard.Begin();
    ++index;
  }
}

// ---- drawer sheets (ContentDialogs) ----------------------------------------

void ConnectPage::OnClientStatsCardClick(IInspectable const&,
                                          RoutedEventArgs const&) {
  ShowClientContractsSheet();
}

void ConnectPage::OnLocalStatsCardClick(IInspectable const&,
                                         RoutedEventArgs const&) {
  ShowSplitRulesSheet();
}

void ConnectPage::OnDnsCardClick(IInspectable const&, RoutedEventArgs const&) {
  ShowDnsSheet();
}

void ConnectPage::OnLocationRowClick(IInspectable const&, RoutedEventArgs const&) {
  ShowLocationChooserSheet();
}

void ConnectPage::OnPeersLineClick(IInspectable const&, RoutedEventArgs const&) {
  ShowLocationChooserSheet();
}

winrt::fire_and_forget ConnectPage::ShowClientContractsSheet() {
  if (w_.sheetOpen()) co_return;  // only one ContentDialog can show at a time
  auto self = w_.get_strong();
  w_.SetSheetOpen(true);
  try {
    contractsSheet_ = urnw::ClientContractsSheet::Create(self->Content().XamlRoot(), Sdk());
    contractsSheet_->Update(contractRows_);
    co_await contractsSheet_->Dialog().ShowAsync();
  } catch (...) {
  }
  contractsSheet_.reset();
  w_.SetSheetOpen(false);
  // the sheet drove the VC's at-top state; leave it at the top on close so the
  // controller isn't stuck frozen (collecting a pending count) with nobody viewing
  Sdk().SetContractsAtTop(true);
}

winrt::fire_and_forget ConnectPage::ShowSplitRulesSheet() {
  if (w_.sheetOpen()) co_return;
  auto self = w_.get_strong();
  w_.SetSheetOpen(true);
  try {
    splitRulesSheet_ = urnw::SplitRulesSheet::Create(self->Content().XamlRoot(), Sdk());
    splitRulesSheet_->Update(splitRules_, blockActions_, allowedCount_, blockedCount_);
    co_await splitRulesSheet_->Dialog().ShowAsync();
  } catch (...) {
  }
  splitRulesSheet_.reset();
  w_.SetSheetOpen(false);
}

winrt::fire_and_forget ConnectPage::ShowDnsSheet() {
  if (w_.sheetOpen()) co_return;
  auto self = w_.get_strong();
  w_.SetSheetOpen(true);
  try {
    // draft edits apply together on Update; live store pushes don't reset the
    // open editor (macOS parity)
    dnsSheet_ = urnw::DnsEditorSheet::Create(self->Content().XamlRoot(), Sdk(),
                                             dnsSettings_, countryCode_, countryName_);
    co_await dnsSheet_->Dialog().ShowAsync();
  } catch (...) {
  }
  dnsSheet_.reset();
  w_.SetSheetOpen(false);
}

winrt::fire_and_forget ConnectPage::ShowLocationChooserSheet() {
  if (w_.sheetOpen()) co_return;  // only one ContentDialog can show at a time
  auto self = w_.get_strong();
  w_.SetSheetOpen(true);
  // open the locations + peers view controllers (idempotent) and push an initial
  // snapshot before seeding the sheet from the current values
  Sdk().EnsureLocations();
  try {
    locationSheet_ = urnw::LocationChooserSheet::Create(self->Content().XamlRoot(), Sdk());
    locationSheet_->Update(Sdk().CurrentFilteredLocations(), Sdk().ConnectedProvidePeers());
    co_await locationSheet_->Dialog().ShowAsync();
  } catch (...) {
  }
  locationSheet_.reset();
  w_.SetSheetOpen(false);
}

void ConnectPage::ApplyPeerCount(std::optional<urnet::NetworkPeerList> const& peers) {
  // ALL connected devices (online, provide or not); the chooser's peers
  // section stays provide-filtered (connectable only). The list argument is
  // the update trigger; the count reads the unfiltered value.
  (void)peers;
  // The peers state lives in the service's device: while the rpc is down
  // (service not running) a zero here would be a stale claim presented as
  // fact, so the line goes gray and says discovery is disabled (apple
  // ConnectActions parity).
  if (!Sdk().RemoteConnected()) {
    const hstring disabled = Loc("peer_discovery_disabled");
    w_.PeerCountText().Text(disabled);
    // the row's automation name IS its text, and its text changes: set both
    // together so they can never disagree
    winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(w_.PeersLine(),
                                                                         disabled);
    w_.PeerDot().Fill(urnw::colors::MutedBrush());
    return;
  }
  const int64_t count = Sdk().ConnectedPeerCount();
  // the standalone peers status line below the connect button, always shown:
  // "{n} peers" + a filled dot, green when providing peers are online and amber
  // at zero (apple ConnectActions parity)
  const hstring peerText{urnw::Plural("network_peer_count", count)};
  w_.PeerCountText().Text(peerText);
  winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(w_.PeersLine(),
                                                                       peerText);
  w_.PeerDot().Fill(urnw::colors::MakeBrush(0 < count ? urnw::colors::kUrGreen
                                                      : urnw::colors::kUrAmber));
}

}  // namespace urnw
