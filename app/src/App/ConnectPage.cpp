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
#include <iterator>
#include <tuple>
#include <utility>
#include <vector>

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
  // ---- R3: the three pane headers -----------------------------------------
  // Every title here is a SHIPPED key. "Details" / "Session" / "Inspector",
  // which would have named the third pane more exactly, do not exist in the
  // store (945 keys, ~250 used) and are reported rather than invented.
  w_.PaneATitle().Text(Loc("connect"));
  w_.PaneBTitle().Text(Loc("activity"));
  w_.PaneCTitle().Text(Loc("client_statistics"));
  w_.ConnectionsLabel().Text(Loc("connections"));
  w_.DataUsageLabel().Text(Loc("data_usage"));
  w_.PeersGroupLabel().Text(Loc("network_peers"));
  // A pane is a landmark: without a name the three columns reach a screen
  // reader as three unlabelled groups in an arbitrary order.
  namespace pane_automation = winrt::Microsoft::UI::Xaml::Automation;
  pane_automation::AutomationProperties::SetName(w_.ConnectPaneA(), Loc("connect"));
  pane_automation::AutomationProperties::SetName(w_.ConnectPaneB(), Loc("activity"));
  pane_automation::AutomationProperties::SetName(w_.ConnectPaneC(),
                                                 Loc("client_statistics"));
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
  // R3: these three are the STATISTICS pane's group headers now, not three
  // cards. Their old card bodies became the rows under each group; the card
  // itself survives only as the group's trailing action, which still opens the
  // same sheet it always did.
  w_.ClientStatsLabel().Text(Loc("client_contracts"));
  w_.LocalStatsLabel().Text(Loc("split_rules"));
  w_.DnsCardLabel().Text(Loc("custom_dns"));
  // The activity pane's empty state: a centred line inside the full-height pane.
  // "Contracts appear here while connected." is the shipped string closest to
  // the spec's "session statistics will appear after you connect"; the exact
  // copy is a reported store addition.
  w_.SessionEmptyText().Text(Loc("contracts_appear_connected"));
  ApplySessionCardsVisibility(connected_);
  ApplySessionRows();
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
  automation::AutomationProperties::SetName(w_.ClientStatsCard(), Loc("client_contracts"));
  automation::AutomationProperties::SetName(w_.LocalStatsCard(), Loc("split_rules"));
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
  // The plan + usage card that used to sit in this rail is gone from Home
  // (spec §5); its strings now belong only to Account, which paints them from
  // MainWindow::ApplyBalance.
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
  if (PreviewSampleActive()) return;  // see ApplyStats
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

// The service-setup banner (beta spec §3). Renders MainWindow's one snapshot
// onto ServiceSetupBar, the InfoBar sitting under BalanceWarning in this pane
// — same bar shape, same one-writer discipline. Every label goes through
// Adv(): the store's 916 keys were searched and carry nothing for a Windows
// service surface (the only "Set up"/"Install" strings are the browser
// extension's), so these ids wait for the store the same way the inspector's
// do. The two shipped strings that DO fit are used: "Update" (the generic
// CTA) and "Setting up…" (site_ext_setting_up — its comment scopes it to the
// extension, but its value is exactly this moment).
void ConnectPage::ApplyServiceSetup(urnw::ServiceSetup::Snapshot const& snap) {
  using State = urnw::ServiceSetup::State;
  using Notice = urnw::ServiceSetup::Notice;
  auto bar = w_.ServiceSetupBar();
  const State state = snap.observation.state;
  const bool show = state == State::NotInstalled || state == State::Stopped ||
                    state == State::VersionMismatch;
  if (!show) {
    bar.IsOpen(false);
    return;
  }

  winrt::hstring title;
  winrt::hstring action;
  std::wstring message;
  switch (state) {
    case State::NotInstalled:
      title = Adv("svc_setup_title", L"Set up the VPN service");
      action = Adv("svc_setup_action", L"Set up");
      message = AdvW("svc_setup_message",
                     L"URnetwork uses a Windows service to carry traffic. One "
                     L"click — Windows will ask for administrator permission.");
      break;
    case State::Stopped:
      title = Adv("svc_start_title", L"Start the VPN service");
      action = Adv("svc_start_action", L"Start");
      message = AdvW("svc_start_message",
                     L"The service is installed but not running.");
      break;
    default:  // VersionMismatch — `show` admits nothing else
      title = Adv("svc_update_title", L"Update the VPN service");
      action = Loc("update");
      message = AdvW("svc_update_message",
                     L"The installed service is a different version than this "
                     L"app.");
      // The versions are DATA (release-grammar strings, never translated), so
      // appending them to the store line is composition, not a hidden literal.
      if (!snap.observation.installedVersion.empty() &&
          !snap.observation.siblingVersion.empty()) {
        message += L" (" + snap.observation.installedVersion + L" → " +
                   snap.observation.siblingVersion + L")";
      }
      break;
  }

  // The in-flight and aftermath lines REPLACE the pitch, not the title: the
  // banner keeps saying what it is for while the message says what is
  // happening to it right now.
  if (snap.busy) {
    message = std::wstring{Loc("site_ext_setting_up")};
  } else if (snap.notice == Notice::UacDeclined) {
    message = AdvW("svc_uac_declined",
                   L"Windows asked for permission and the prompt was closed. "
                   L"Click again whenever you're ready.");
  } else if (snap.notice == Notice::ActionFailed) {
    message = AdvW("svc_action_failed",
                   L"That didn't finish — the service is unchanged. Details "
                   L"are in the app log.");
  }

  bar.Title(title);
  bar.Message(winrt::hstring{message});
  if (auto button = bar.ActionButton()) {
    button.Content(winrt::box_value(action));
    button.IsEnabled(!snap.busy);
  }
  bar.IsOpen(true);
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
  const bool disconnectAction = ConnectActionIsDisconnect();
  w_.ConnectButton().Content(disconnectAction ? LocBox("disconnect") : LocBox("connect"));
  // ...and its WEIGHT. The state is carried on four channels — the word above,
  // the dot's colour, the hero, and now the button's FILL — so none of them is
  // carrying it alone and none of them is colour-alone. Filled blue while there
  // is something to do, outlined once the tunnel is up. The reasoning, and why
  // the full-bleed lime slab that used to be here is gone, is in App.xaml at
  // UrPaneActionPrimaryStyle.
  //
  // The whole STYLE is swapped rather than the Background brush: a style carries
  // its pointer-over, pressed and disabled states with it, and setting a brush
  // alone would leave a button that washes to the other state's hover colour
  // under the pointer.
  ApplyConnectButtonStyle(disconnectAction ? L"UrPaneActionSecondaryStyle"
                                           : L"UrPaneActionPrimaryStyle");

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

// Swap the connect action between its filled and outlined forms. Looked up by
// key rather than held, because App.xaml is where the two forms are DEFINED and
// a cached Style here would be a second place they could drift apart. The
// HasKey test is not defensive noise: a missing key throws out of Lookup, and
// this runs on every status push.
void ConnectPage::ApplyConnectButtonStyle(std::wstring_view key) {
  auto const resources = Application::Current().Resources();
  auto const boxed = winrt::box_value(hstring{key});
  if (!resources.HasKey(boxed)) return;
  if (auto style = resources.Lookup(boxed).try_as<winrt::Microsoft::UI::Xaml::Style>()) {
    if (w_.ConnectButton().Style() != style) w_.ConnectButton().Style(style);
  }
}

// ---- live stats (macOS parity) -------------------------------------------

void ConnectPage::ApplyStats(urnw::LiveStats const& stats) {
  // --preview-ui with a sample loaded: the process has no session, so every push
  // that reaches here is the empty one, and applying it wipes the synthetic rows
  // the operator asked for a few hundred milliseconds after they appear. Same
  // gate, same reason, as PreviewHeroActive on the canvas.
  if (PreviewSampleActive()) return;
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

  // Live throughput feed: down / up bit rate. This is the ACTIVITY PANE's own
  // header figure now — the pane whose chart and connections table it describes
  // — rather than a line inside a card two columns away.
  urnw::kit::SetTextOrCollapse(
      w_.ThroughputText(),
      stats.connected ? H("↓ " + urnw::FormatBitRate(stats.downBitsPerSecond) +
                          "   ↑ " + urnw::FormatBitRate(stats.upBitsPerSecond))
                      : hstring(L""));
  w_.LiveStatsGroup().Visibility(stats.connected ? Visibility::Visible
                                                 : Visibility::Collapsed);
  // R3: the statistics pane draws the session as key/value rows, so it needs the
  // figures rather than only the prose lines above.
  downBitsPerSecond_ = stats.downBitsPerSecond;
  upBitsPerSecond_ = stats.upBitsPerSecond;
  providerCount_ = stats.providerCount;
  statsConnected_ = stats.connected;
  ApplySessionRows();
  // the activity list vs its centred empty line, on the same connected signal
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
  // The ROW collapses, not just its text: a fixed-height row wrapped around a
  // collapsed TextBlock is still a blank row, which is the same "hole in the
  // middle of the panel" defect one level down.
  w_.ProvideStatsText().Text(provide);
  w_.ProvideStatsRow().Visibility(provide.empty() ? Visibility::Collapsed
                                                  : Visibility::Visible);

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

namespace {
// Keep a chart inside its pane.
//
// TransferChart draws into a Canvas, and a Canvas does not clip: its curves and
// its edge labels run a few pixels past the host and, in a pane layout, straight
// across the 1px rule into the NEXT pane. It did - the activity chart put a
// green sliver and a stray peak marker inside the statistics pane, right at the
// boundary. A Grid column does not clip its children either, so the clip has to
// be stated, and re-stated on every resize because Clip is a fixed rectangle.
void ClipToBounds(winrt::Microsoft::UI::Xaml::Controls::Grid const& host) {
  if (!host) return;
  auto apply = [](winrt::Microsoft::UI::Xaml::FrameworkElement const& element,
                  winrt::Windows::Foundation::Size const& size) {
    winrt::Microsoft::UI::Xaml::Media::RectangleGeometry clip;
    clip.Rect({0, 0, static_cast<float>(size.Width), static_cast<float>(size.Height)});
    element.Clip(clip);
  };
  host.SizeChanged([apply](winrt::Windows::Foundation::IInspectable const& sender,
                           SizeChangedEventArgs const& args) {
    if (auto element = sender.try_as<winrt::Microsoft::UI::Xaml::FrameworkElement>()) {
      apply(element, args.NewSize());
    }
  });
}
}  // namespace

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
  // R3: a chart is now full-bleed to its pane's edge, so anything it overdraws
  // lands in the pane next door.
  ClipToBounds(w_.RemoteChartHost());
  ClipToBounds(w_.BlockedChartHost());
  ClipToBounds(w_.LocalChartHost());
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
        page.ApplyContractsList();
        if (page.contractsSheet_) page.contractsSheet_->Update(page.contractRows_);
      }
    });
  });
  sdk.SetBlockActionsHandler([queue, weak](std::vector<urnw::BlockActionItem> actions) {
    queue.TryEnqueue([weak, actions = std::move(actions)] {
      if (auto self = weak.get()) {
        auto& page = self->connect();
        page.blockActions_ = actions;
        page.ApplyConnectionsList();
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
        page.ApplySessionRows();
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
  ApplySplitRuleCount();  // also rebuilds the split-rules list
  ApplyDnsCard(dnsSettings_);
  ApplyConnectionsList();
  ApplyContractsList();
  ApplySessionRows();
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

// R3: the activity pane's list against its empty state. Both occupy the SAME
// full-height area (they are the two children of one Grid), so an empty session
// is a centred sentence in a floor-to-ceiling pane rather than a short card
// floating in a page. The charts and the statistics pane stay put either way:
// they are structure, and structure that disappears when a session ends is how
// the window came to be half empty in the first place.
void ConnectPage::ApplySessionCardsVisibility(bool connected) {
  const bool hasRows = 0 < w_.ConnectionsHost().Children().Size();
  const bool showList = connected && hasRows;
  w_.ConnectionsScroll().Visibility(showList ? Visibility::Visible
                                             : Visibility::Collapsed);
  w_.SessionEmptyCard().Visibility(showList ? Visibility::Collapsed
                                            : Visibility::Visible);
}

// ---- R3: the pane lists ----------------------------------------------------
//
// Three lists, one row species (kit::MakePaneListRow), one height per list. The
// old page put each of these behind a card that opened a ContentDialog; the
// dialogs are all still there and still reachable from each group's trailing
// action, but the CONTENT is now on screen, which is the whole difference
// between a dashboard and a launcher for dialogs.

namespace {
// the first thing that identifies a routing decision: an override match, then a
// hostname, then an address. iOS BlockActionItem renders the same precedence.
std::string BlockActionTitle(urnw::BlockActionItem const& action) {
  if (!action.matchedHosts.empty()) return action.matchedHosts.front();
  if (!action.hosts.empty()) return action.hosts.front();
  if (!action.matchedIps.empty()) return action.matchedIps.front();
  if (!action.ips.empty()) return action.ips.front();
  return {};
}

// A peer client id is 36 characters of uuid and there is no room for it in a
// 380dip pane. The head is what distinguishes one peer from another on screen;
// the full value stays in the contracts sheet.
std::string ShortId(std::string const& id) {
  return id.size() <= 12 ? id : id.substr(0, 12) + "…";
}
}  // namespace

// The activity pane's table: every routing decision the device has made, newest
// first. Coral = blocked, green = allowed through the tunnel, amber = sent
// around it (a split rule matched). This is the pane's reason to exist and the
// list that has to FILL it.
void ConnectPage::ApplyConnectionsList() {
  auto host = w_.ConnectionsHost();
  host.Children().Clear();
  connectionRows_.clear();
  connectionRowIds_.clear();
  // A cap, not a scroll budget: the SDK's action feed is unbounded and every row
  // is a live XAML subtree. 200 rows is ~7000px of pane, well past any window.
  constexpr size_t kMaxRows = 200;
  const size_t count = std::min(blockActions_.size(), kMaxRows);
  for (size_t i = 0; i < count; ++i) {
    auto const& action = blockActions_[i];
    const std::string title = BlockActionTitle(action);
    const hstring titleText = title.empty() ? Loc("unknown") : H(title);
    const auto verdictColor = action.block   ? urnw::colors::kUrCoral
                              : action.local ? urnw::colors::kUrAmber
                                             : urnw::colors::kUrGreen;
    // The verdict in WORDS, for the row's accessible name. The dot is Raw, so
    // the name is the only place the colour's meaning exists for a screen
    // reader — and `local` is a THIRD verdict the old two-way name folded into
    // "allowed": traffic sent around the tunnel is allowed and unprotected, and
    // those are not the same thing to anyone reading this list.
    const hstring verdict = action.block  ? Loc("blocked")
                            : action.local ? Loc("local")
                                           : Loc("allowed");
    const hstring meta = H(urnw::FormatByteCountCompact(action.byteCount) + "   " +
                           urnw::FormatCountCompact(action.packetCount) + " pkt");

    if (!advancedMode_) {
      // NORMAL. Exactly what shipped: a static row, not focusable, not
      // selectable. A Normal user is being told what their VPN is doing, not
      // handed 200 tab stops on the way to the Connect button.
      auto row = urnw::kit::MakePaneListRow(36);
      row.dot.Fill(urnw::colors::MakeBrush(verdictColor));
      row.title.Text(titleText);
      row.meta.Text(meta);
      winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(
          row.root, hstring{std::wstring{titleText} + L", " + std::wstring{verdict}});
      host.Children().Append(row.root);
      continue;
    }

    // ADVANCED. The same row, selectable: clickable, in the tab order, and
    // invokable with Enter or Space because it is a real Button rather than a
    // Border with a pointer handler bolted on.
    auto row = urnw::kit::MakePaneListRowButton(36);
    row.dot.Fill(urnw::colors::MakeBrush(verdictColor));
    row.title.Text(titleText);
    row.meta.Text(meta);
    winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(
        row.root, hstring{std::wstring{titleText} + L", " + std::wstring{verdict}});
    // By ID, never by index — see SelectConnection. The id is captured by value
    // so the handler does not reach back into a vector that has been rebuilt.
    const std::string id = action.id;
    row.root.Click([weak = w_.get_weak(), id](auto const&, auto const&) {
      if (auto self = weak.get()) self->connect().SelectConnection(id);
    });
    host.Children().Append(row.root);
    connectionRows_.push_back(row);
    connectionRowIds_.push_back(id);
  }
  w_.ConnectionsCount().Text(
      hstring{urnw::Plural("host_count", static_cast<int64_t>(blockActions_.size()))});
  ApplySessionCardsVisibility(statsConnected_);
  // The list was just rebuilt underneath the selection. If what was selected is
  // no longer in the feed, the inspector must say so rather than keep printing a
  // connection that has aged out.
  ApplyConnectionSelectionVisuals();
  ApplyInspector();
}

// The session, as key/value rows on the statistics pane's grid. These were four
// prose lines inside the hero card; a figure belongs in a column beside its
// label, on the same rhythm as every other figure on the pane.
void ConnectPage::ApplySessionRows() {
  auto host = w_.SessionRowsHost();
  host.Children().Clear();
  auto add = [&host](winrt::hstring const& key, std::string const& value) {
    host.Children().Append(urnw::kit::MakePaneKeyValueRow(key, H(value)).root);
  };
  const std::string idle = "—";  // an em dash: "no session", not "zero"
  add(Loc("remote"), statsConnected_
                         ? "↓ " + urnw::FormatBitRate(downBitsPerSecond_)
                         : idle);
  add(Loc("local"), statsConnected_
                        ? "↑ " + urnw::FormatBitRate(upBitsPerSecond_)
                        : idle);
  add(Loc("allowed"), urnw::FormatCountCompact(allowedCount_));
  add(Loc("blocked"), urnw::FormatCountCompact(blockedCount_));
  add(Loc("connections"), urnw::FormatCountCompact(static_cast<int64_t>(blockActions_.size())));
  if (!advancedMode_) return;
  // ---- the Advanced reading of the same group (D5) --------------------------
  // Two rows a Normal user has no use for and an operator cannot work without.
  //
  // `raw` is the PRE-CLAMP connection status. LiveStats clamps connectionStatus
  // to the unrecognised "RPC_ONLY" in an rpc-only session precisely so no screen
  // can claim a tunnel that does not exist — and this is the one place in the
  // product that is supposed to see through that clamp, which is why
  // rawConnectionStatus was built. (It is populated ONLY in that session; in
  // every other one the clamped value IS the raw one.)
  //
  // `exits` is how many exits the reliability stack currently holds, which is
  // the denominator for every "via" line the inspector prints.
  const std::string raw = Sdk().CurrentStats().rpcOnly
                              ? Sdk().CurrentStats().rawConnectionStatus
                              : Sdk().CurrentStats().connectionStatus;
  host.Children().Append(
      urnw::kit::MakePaneKeyValueRow(Adv("adv_raw_status", L"Raw status"),
                                     raw.empty() ? Adv("adv_none", L"none") : H(raw))
          .root);
  host.Children().Append(
      urnw::kit::MakePaneKeyValueRow(
          Adv("adv_exits", L"Exits"),
          H(urnw::FormatCountCompact(static_cast<int64_t>(exits_.size()))))
          .root);
}

// ---- D5: the connection inspector ------------------------------------------
//
// THE PORTMASTER ASK. Advanced Mode makes the activity pane's rows selectable
// and turns the third pane into the detail for the selection.
//
// What it can honestly show, and where each field comes from, because the
// tempting thing here is to print a Portmaster screenshot's field list and fill
// the gaps with plausible values:
//
//   host / addresses     BlockActionItem::hosts / ips / matchedHosts / matchedIps
//   verdict              BlockActionItem::block, ::local  (THREE states, not two:
//                        blocked, tunnelled, and sent AROUND the tunnel)
//   reason               BlockActionItem::overrideId + hasBlockOverride /
//                        hasRouteOverride. The SDK has no free-text reason; an
//                        override id and which KIND it was is the whole of it.
//   packets / bytes      BlockActionItem::packetCount / ::byteCount. TOTALS.
//                        There is no per-direction split on a block action and
//                        the labels do not pretend there is — the directional
//                        counters in the SDK (ThroughputSample, PacketStats) are
//                        device-wide, so printing them per row would be a lie
//                        with the right shape.
//   tunnelled            derived from the same two verdict bits, which is what
//                        RouteOverride::Local means.
//   first seen           BlockActionItem::timeMillis
//   via exit             DestinationExit{DestinationIp -> ClientId}, joined on
//                        the action's recorded ips. This is the ONLY per-
//                        connection "which exit" the SDK has.
//   exit health          Exit{Tier, EffectiveTier, FlowCount, DialFailureCount,
//                        Quarantined, Warning, WarningCause, Proven,
//                        ProbeAgeSeconds}, joined on that client id.
//   exit country         LiveStats::countryName — and labelled as the SESSION's
//                        exit, because that is what it is. Per-exit geo exists
//                        in the SDK (ConnectedProviderLocation) and is not
//                        bridged; claiming it per connection would be inventing.
//
// What it does NOT show, deliberately: protocol, port, per-direction counters,
// ASN/org, per-connection duration and per-connection RTT. None of those exists
// on any feed this client can reach. They are in the report as bridging work.
//
// Every label here is an Adv() id — see pages::Adv. The store has 945 keys and
// not one of them names a field of a connection inspector.

void ConnectPage::SelectConnection(std::string const& id) {
  // A second click on the selected row clears it. The alternative is a selection
  // that can be moved but never removed, and the inspector then permanently
  // occupies the top of the pane over a connection the user stopped caring about.
  selectedConnectionId_ = (selectedConnectionId_ == id) ? std::string{} : id;
  ApplyConnectionSelectionVisuals();
  ApplyInspector();
}

void ConnectPage::OnInspectorClear(IInspectable const&, RoutedEventArgs const&) {
  selectedConnectionId_.clear();
  ApplyConnectionSelectionVisuals();
  ApplyInspector();
}

// Repaint, do not rebuild. Rebuilding the list on a click destroys the element
// that has keyboard focus, which drops focus to the top of the pane and makes
// the list unusable with Tab — the exact opposite of what making the rows
// focusable was for.
void ConnectPage::ApplyConnectionSelectionVisuals() {
  for (size_t i = 0; i < connectionRows_.size(); ++i) {
    const bool selected =
        !selectedConnectionId_.empty() && connectionRowIds_[i] == selectedConnectionId_;
    urnw::kit::SetPaneListRowSelected(connectionRows_[i], selected);
    // A screen reader is TOLD, not shown. Without this the fill and the accent
    // bar carry the selection to sighted users only.
    auto const& row = connectionRows_[i];
    auto name = winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::GetName(
        row.root);
    std::wstring base{name};
    const std::wstring suffix = L", " + AdvW("adv_selected", L"selected");
    const bool hasSuffix = base.size() >= suffix.size() &&
                           base.compare(base.size() - suffix.size(), suffix.size(),
                                        suffix) == 0;
    if (selected && !hasSuffix) {
      winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(
          row.root, hstring{base + suffix});
    } else if (!selected && hasSuffix) {
      winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(
          row.root, hstring{base.substr(0, base.size() - suffix.size())});
    }
  }
}

std::optional<ConnectPage::ExitRouting> ConnectPage::RoutingForAddresses(
    std::vector<std::string> const& addresses) const {
  for (auto const& ip : addresses) {
    for (auto const& dest : destinationExits_) {
      if (dest.DestinationIp != ip) continue;
      ExitRouting out;
      out.clientId = dest.ClientId ? *dest.ClientId : std::string{};
      out.flowCount = dest.FlowCount;
      for (auto const& exit : exits_) {
        if (!exit.ClientId || *exit.ClientId != out.clientId) continue;
        out.haveExit = true;
        out.tier = exit.Tier;
        out.effectiveTier = exit.EffectiveTier;
        out.exitFlowCount = exit.FlowCount;
        out.dialFailureCount = exit.DialFailureCount;
        out.quarantined = exit.Quarantined;
        out.warning = exit.Warning;
        out.warningCause = exit.WarningCause;
        out.proven = exit.Proven;
        out.probeAgeSeconds = exit.ProbeAgeSeconds;
        break;
      }
      return out;
    }
  }
  return std::nullopt;
}

void ConnectPage::ApplyInspector() {
  // Normal mode: the group is not merely empty, it is gone. The third pane is
  // the statistics pane it has always been, with no vestigial header.
  if (!advancedMode_) {
    w_.InspectorGroup().Visibility(Visibility::Collapsed);
    return;
  }
  w_.InspectorGroup().Visibility(Visibility::Visible);
  w_.InspectorLabel().Text(Adv("adv_inspector", L"Inspector"));
  // A landmark with no name is an unlabelled region, which is worse than no
  // landmark at all. Set here rather than in markup because the name is an Adv()
  // id, which markup cannot resolve.
  winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(
      w_.InspectorGroup(), Adv("adv_inspector", L"Inspector"));

  auto host = w_.InspectorRowsHost();
  host.Children().Clear();

  // Find the selection in the CURRENT feed. It may have aged out of the SDK's
  // window since it was picked, and if it has, saying so is the honest reading —
  // the alternative is a detail pane frozen on a connection that no longer
  // exists, which is indistinguishable from a hung inspector.
  const urnw::BlockActionItem* action = nullptr;
  if (!selectedConnectionId_.empty()) {
    for (auto const& candidate : blockActions_) {
      if (candidate.id == selectedConnectionId_) {
        action = &candidate;
        break;
      }
    }
  }

  w_.InspectorClearButton().Visibility(action ? Visibility::Visible
                                              : Visibility::Collapsed);
  winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(
      w_.InspectorClearButton(), Adv("adv_clear_selection", L"Clear selection"));

  if (!action) {
    // The empty reading. Not a hole and not an empty card: the headline row says
    // what to do, on the same rhythm as every other row in the pane.
    w_.InspectorHeadline().Visibility(Visibility::Visible);
    w_.InspectorTitle().Text(
        selectedConnectionId_.empty()
            ? Adv("adv_no_selection", L"No connection selected")
            : Adv("adv_selection_gone", L"That connection is no longer listed"));
    w_.InspectorDot().Fill(urnw::colors::MakeBrush(urnw::colors::kTextFaint));
    w_.InspectorVerdict().Text(
        Adv("adv_select_a_row", L"Select a row in Activity to inspect it"));
    return;
  }

  // ---- the headline: what it is, and the verdict --------------------------
  const std::string title = BlockActionTitle(*action);
  w_.InspectorHeadline().Visibility(Visibility::Visible);
  w_.InspectorTitle().Text(title.empty() ? Loc("unknown") : H(title));
  // THREE verdicts, not two. "Blocked" and "allowed" lose the one the user most
  // needs to see: traffic a split rule sent AROUND the tunnel is allowed and
  // unprotected, and a privacy tool that files that under "allowed" is hiding
  // the fact it exists to surface.
  const auto verdictColor = action->block   ? urnw::colors::kUrCoral
                            : action->local ? urnw::colors::kUrAmber
                                            : urnw::colors::kUrGreen;
  w_.InspectorDot().Fill(urnw::colors::MakeBrush(verdictColor));
  w_.InspectorVerdict().Text(
      action->block ? Adv("adv_verdict_blocked", L"Blocked — no packets sent")
      : action->local
          ? Adv("adv_verdict_local", L"Bypassed the tunnel — not protected")
          : Adv("adv_verdict_tunnelled", L"Tunnelled through URnetwork"));

  auto add = [&host](winrt::hstring const& key, winrt::hstring const& value) {
    host.Children().Append(urnw::kit::MakePaneKeyValueRow(key, value).root);
  };
  auto addText = [&add](winrt::hstring const& key, std::string const& value) {
    add(key, value.empty() ? Adv("adv_none", L"none") : H(value));
  };
  auto join = [](std::vector<std::string> const& parts) {
    std::string out;
    for (auto const& part : parts) {
      if (!out.empty()) out += ", ";
      out += part;
    }
    return out;
  };

  // ---- identity -----------------------------------------------------------
  addText(Adv("adv_host", L"Host"), join(action->hosts));
  addText(Adv("adv_addresses", L"Addresses"), join(action->ips));
  // What actually matched an override, which is disjoint from hosts/ips and is
  // the difference between "a rule named this" and "a rule named its parent".
  const std::string matched = join(action->matchedHosts).empty()
                                  ? join(action->matchedIps)
                                  : join(action->matchedHosts);
  if (!matched.empty()) addText(Adv("adv_matched", L"Matched"), matched);

  // ---- the decision -------------------------------------------------------
  add(Adv("adv_protected", L"Protected"),
      action->block ? Adv("adv_na", L"—")
      : action->local ? Loc("off")
                      : Loc("on"));
  // The SDK has no free-text reason. An override id, and WHICH KIND of override
  // it was, is the entirety of what it can say — so that is what this prints
  // rather than a sentence someone made up.
  if (action->overrideId.empty()) {
    add(Adv("adv_reason", L"Reason"), Adv("adv_reason_default", L"Default policy"));
  } else {
    add(Adv("adv_reason", L"Reason"),
        action->hasBlockOverride  ? Adv("adv_reason_block", L"Block override")
        : action->hasRouteOverride ? Adv("adv_reason_route", L"Route override")
                                   : Adv("adv_reason_override", L"Override"));
    addText(Adv("adv_override_id", L"Override"), action->overrideId);
  }

  // ---- volume -------------------------------------------------------------
  // TOTALS, and the labels say so. There is no per-direction split on a block
  // action; the SDK's directional counters (ThroughputSample, PacketStats) are
  // device-wide, so an "in / out" pair here would be the right shape around the
  // wrong number. Reported as bridging work instead.
  add(Adv("adv_packets_total", L"Packets (total)"),
      H(urnw::FormatCountCompact(action->packetCount)));
  add(Adv("adv_bytes_total", L"Bytes (total)"),
      H(urnw::FormatByteCountCompact(action->byteCount)));

  // ---- timing -------------------------------------------------------------
  // The action's own timestamp, as an age. NOT a duration: the SDK records when
  // a routing decision was MADE and nothing anywhere records when a connection
  // closed, so a "Duration" field would have to be invented.
  if (0 < action->timeMillis) {
    const int64_t nowMillis = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::system_clock::now().time_since_epoch())
                                  .count();
    add(Adv("adv_last_decision", L"Last decision"),
        H(urnw::RelativeTime(action->timeMillis, nowMillis)));
  }

  // ---- the exit it routed through -----------------------------------------
  // Joined out of the reliability snapshot. Absent rather than guessed when the
  // action recorded no addresses, or when none of them is in the snapshot: that
  // is the normal case for a host that resolved after the last refresh, and an
  // inspector that answers "which exit" with a plausible wrong exit is worse
  // than one that says it does not know.
  if (!action->block) {
    if (auto routing = RoutingForAddresses(action->ips)) {
      addText(Adv("adv_via_exit", L"Via exit"), routing->clientId);
      add(Adv("adv_exit_flows", L"Flows to this destination"),
          H(urnw::FormatCountCompact(routing->flowCount)));
      if (routing->haveExit) {
        add(Adv("adv_exit_tier", L"Exit tier"),
            H(std::to_string(routing->effectiveTier) + " / " +
              std::to_string(routing->tier)));
        add(Adv("adv_exit_flows_total", L"Exit flows"),
            H(urnw::FormatCountCompact(routing->exitFlowCount)));
        add(Adv("adv_exit_dial_failures", L"Dial failures"),
            H(urnw::FormatCountCompact(routing->dialFailureCount)));
        add(Adv("adv_exit_state", L"Exit state"),
            routing->quarantined ? Adv("adv_exit_quarantined", L"Quarantined")
            : routing->warning   ? Adv("adv_exit_warning", L"Warning")
            : routing->proven    ? Adv("adv_exit_proven", L"Proven")
                                 : Adv("adv_exit_ok", L"OK"));
        if (routing->warning && !routing->warningCause.empty()) {
          addText(Adv("adv_exit_warning_cause", L"Warning cause"), routing->warningCause);
        }
        if (0 < routing->probeAgeSeconds) {
          add(Adv("adv_probe_age", L"Probe age"),
              H(std::to_string(routing->probeAgeSeconds) + "s"));
        }
      }
    } else {
      add(Adv("adv_via_exit", L"Via exit"), Adv("adv_unknown_exit", L"Not in the routing table"));
    }
    // The SESSION's exit country, labelled as the session's. Per-exit geo exists
    // in the SDK (ConnectedProviderLocation: country, region, city, lat/lon,
    // connected-since) and is not bridged into this client at all; attaching the
    // session's country to a per-connection row as though it were that
    // connection's would be exactly the fabrication this pane must not do.
    if (!countryName_.empty()) {
      addText(Adv("adv_session_exit_country", L"Session exit country"), countryName_);
    }
  }

  // ---- identity, last, and copyable ---------------------------------------
  // Ids are the thing an operator pastes into a bug report. The entity ids on
  // this client were made copyable for that reason; a 36-character id you can
  // read but not copy is a screenshot.
  {
    auto row = urnw::kit::MakePaneKeyValueRow(Adv("adv_action_id", L"Action id"),
                                              H(ShortId(action->id)));
    row.value.IsTextSelectionEnabled(true);
    host.Children().Append(row.root);
  }
}

// ReadReliability() is several SYNCHRONOUS rpcs into the service. It must never
// run on the UI thread — the settled shape for that in this codebase is
// resume_background + the queue, because there is no resume_foreground overload
// for Microsoft.UI.Dispatching (see PostQuantumIdentitySheet).
winrt::fire_and_forget ConnectPage::RefreshExitRouting() {
  // Same gate, same reason, as ApplyStats: with a preview sample loaded the
  // process has NO session, so every read that reaches here is the empty one,
  // and applying it wipes the synthetic routing tables the operator asked for.
  // Measured: the inspector rendered "Via exit: not in the routing table" five
  // seconds after showing the join correctly.
  if (PreviewSampleActive()) co_return;
  if (exitRefreshInFlight_) co_return;
  exitRefreshInFlight_ = true;
  auto weak = w_.get_weak();
  auto queue = w_.DispatcherQueue();

  co_await winrt::resume_background();
  std::vector<urnet::Exit> exits;
  std::vector<urnet::DestinationExit> destinationExits;
  try {
    // ReadReliability already guards both list unwraps with ReadSdkList — the
    // Go side marshals a nil slice as the four-byte document `null`, which the
    // top-level vector unwrap turns into type_error.302. Seven of eleven list
    // getters were observed throwing that against a live session.
    auto snapshot = Sdk().ReadReliability();
    exits = std::move(snapshot.exits);
    destinationExits = std::move(snapshot.destinationExits);
  } catch (std::exception const& e) {
    urnw::LogWarn("connect: exit routing refresh failed: {}", e.what());
  } catch (...) {
    urnw::LogWarn("connect: exit routing refresh failed");
  }

  queue.TryEnqueue([weak, exits = std::move(exits),
                    destinationExits = std::move(destinationExits)]() mutable {
    auto self = weak.get();
    if (!self) return;
    auto& page = self->connect();
    page.exitRefreshInFlight_ = false;
    // Re-checked HERE, not only on the way in. The entry gate reads
    // MainWindow::previewUi(), which is false while the window is still in its
    // constructor — and ApplyAdvancedMode runs there — so a refresh started at
    // construction passes the gate, completes on a worker, and lands AFTER
    // EnterPreviewUi has filled the caches. Measured: the inspector joined
    // correctly and then read "Not in the routing table" a moment later. Every
    // push into this page's caches has to test the sample gate on arrival, the
    // way ApplyStats already does.
    if (page.PreviewSampleActive()) return;
    page.exits_ = std::move(exits);
    page.destinationExits_ = std::move(destinationExits);
    // Only the inspector reads these, and only when something is selected.
    if (page.advancedMode_ && !page.selectedConnectionId_.empty()) page.ApplyInspector();
  });
}

// The page's Advanced reading. One call, and every surface here has re-rendered
// itself in the new mode — the ApplyStrings() shape, for the same reason: a mode
// that each surface consults independently is a mode that half the surfaces
// forget to consult.
void ConnectPage::ApplyAdvancedMode(bool on) {
  if (advancedMode_ == on) return;
  advancedMode_ = on;
  if (!on) selectedConnectionId_.clear();
  // The activity rows change TYPE (Border <-> Button), so this one genuinely has
  // to rebuild rather than repaint.
  ApplyConnectionsList();
  ApplySessionRows();
  ApplyContractsList();
  ApplyInspector();
  // Seed the routing tables the moment the mode comes on, rather than waiting
  // for the first slow tick: the user who just enabled Advanced Mode is looking
  // at the pane now.
  if (on) RefreshExitRouting();
}

// One row per contract peer: which peer, and how much has moved each way.
void ConnectPage::ApplyContractsList() {
  auto host = w_.ContractsHost();
  host.Children().Clear();
  if (contractRows_.empty()) {
    // still a ROW, on the same grid — an empty group must not become a hole
    auto row = urnw::kit::MakePaneKeyValueRow(Loc("contracts_appear_connected"), {}, 34);
    host.Children().Append(row.root);
    return;
  }
  for (auto const& peer : contractRows_) {
    auto row = urnw::kit::MakePaneListRow(36);
    const bool active = 0 < peer.lastActivityMillis && !peer.closing;
    row.dot.Fill(urnw::colors::MakeBrush(active ? urnw::colors::kUrGreen
                                                : urnw::colors::kTextFaint));
    // D5: Advanced shows the WHOLE client id and lets it be selected. The
    // elision exists because a 36-character uuid does not fit a 380dip pane —
    // but the operator who turned Advanced Mode on is the one person who needs
    // the other 24 characters, and truncating them means opening a sheet to
    // read a value that is already on screen.
    row.title.Text(H(advancedMode_ ? peer.clientId : ShortId(peer.clientId)));
    if (advancedMode_) {
      row.title.IsTextSelectionEnabled(true);
      row.title.TextTrimming(winrt::Microsoft::UI::Xaml::TextTrimming::CharacterEllipsis);
    }
    row.meta.Text(H("↑ " + urnw::FormatByteCountCompact(peer.sendByteCount) + "   ↓ " +
                    urnw::FormatByteCountCompact(peer.receiveByteCount)));
    winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(
        row.root, hstring{std::wstring{Loc("contract")} + L", " + H(peer.clientId)});
    host.Children().Append(row.root);
  }
}

// The connect pane's list: the other devices on this network. This feed already
// arrived on every peers push and only its COUNT was ever drawn.
void ConnectPage::ApplyPeersList() {
  auto host = w_.PeersHost();
  host.Children().Clear();
  const int64_t count = peers_ ? static_cast<int64_t>(peers_->size()) : 0;
  w_.PeersGroupCount().Text(count == 0 ? hstring{L""}
                                       : H(urnw::FormatCountCompact(count)));
  if (!peers_ || peers_->empty()) {
    auto row = urnw::kit::MakePaneKeyValueRow(Loc("peer_discovery_disabled"), {}, 34);
    host.Children().Append(row.root);
    return;
  }
  for (auto const& peer : *peers_) {
    auto row = urnw::kit::MakePaneListRow(34);
    row.dot.Fill(urnw::colors::MakeBrush(peer.ProvideEnabled ? urnw::colors::kUrGreen
                                                             : urnw::colors::kTextFaint));
    row.title.Text(H(urnw::PeerDisplayName(peer)));
    // what the device IS, not what it is called: the spec is the only thing that
    // distinguishes two phones with the same default name
    row.meta.Text(H(peer.DeviceSpec));
    host.Children().Append(row.root);
  }
}

// One row per split rule: the host cluster it names, and where it sends it.
void ConnectPage::ApplySplitRulesList() {
  auto host = w_.SplitRulesHost();
  host.Children().Clear();
  if (splitRules_.empty()) {
    auto row = urnw::kit::MakePaneKeyValueRow(Loc("app_split_active_none"), {}, 34);
    host.Children().Append(row.root);
    return;
  }
  for (auto const& rule : splitRules_) {
    auto row = urnw::kit::MakePaneListRow(36);
    // routeLocal sends the cluster AROUND the tunnel; amber is the same "not
    // protected, on purpose" colour the connections table gives that decision.
    row.dot.Fill(urnw::colors::MakeBrush(rule.routeLocal ? urnw::colors::kUrAmber
                                                         : urnw::colors::kUrGreen));
    row.title.Text(rule.hosts.empty() ? Loc("unknown") : H(rule.hosts.front()));
    row.meta.Text(1 < rule.hosts.size()
                      ? hstring{urnw::Plural("host_count",
                                             static_cast<int64_t>(rule.hosts.size()))}
                      : Loc(rule.routeLocal ? "local" : "remote"));
    host.Children().Append(row.root);
  }
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
  ApplySplitRulesList();
}

void ConnectPage::ApplyDnsCard(std::optional<urnet::DnsResolverSettings> const& settings) {
  w_.DnsRowsPanel().Visibility(settings ? Visibility::Visible : Visibility::Collapsed);
  // the ROW, not the text inside it: see ProvideStatsRow
  w_.DnsUnavailableRow().Visibility(settings ? Visibility::Collapsed : Visibility::Visible);
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
  // the preview sample's 60s window scrolls off if it is only pushed once
  if (PreviewSampleActive() && chartTickCount_ % 20 == 0) PreviewSampleCharts();
  if (++chartTickCount_ % 10 == 0) {  // ~1s cadence
    if (splitRulesSheet_) splitRulesSheet_->RefreshTimes();  // "Ns ago" labels
    // D5: the inspector's exit-routing tables, every 5s. THREE gates, and all
    // three earn their place — the mode is on (nothing else reads these), the
    // window is presenting (this function already returned otherwise), and the
    // Home pane is the visible one. It is several synchronous rpcs into the
    // service; running it for a pane nobody is looking at is the whole cost of
    // the feature with none of its value.
    if (advancedMode_ && w_.ConnectView().Visibility() == Visibility::Visible &&
        ++exitRefreshTick_ % 5 == 0) {
      RefreshExitRouting();
    }
  }
  // the contract-details activity resort now lives in the SDK view controller;
  // the sheet just reports scroll and renders the ordered rows (no local tick)
}

// The entrance.
//
// R1 staggered a fade + slide-up across the six cards. R3 has no cards: the
// panes ARE the window, and sliding a full-height column up 16px on every visit
// to Home reads as the layout settling after a failure rather than as polish. So
// the whole shell fades in once, quickly, and nothing moves.
void ConnectPage::AnimateDrawerIn() {
  if (drawerAnimated_) return;
  drawerAnimated_ = true;
  namespace anim = winrt::Microsoft::UI::Xaml::Media::Animation;
  auto view = w_.ConnectView();
  view.Opacity(0);
  anim::CubicEase ease;
  ease.EasingMode(anim::EasingMode::EaseOut);
  anim::Storyboard storyboard;
  anim::DoubleAnimation fade;
  fade.From(0.0);
  fade.To(1.0);
  fade.Duration(Duration{std::chrono::duration_cast<winrt::Windows::Foundation::TimeSpan>(
                             std::chrono::milliseconds(180)),
                         DurationType::TimeSpan});
  fade.EasingFunction(ease);
  anim::Storyboard::SetTarget(fade, view);
  anim::Storyboard::SetTargetProperty(fade, L"Opacity");
  storyboard.Children().Append(fade);
  storyboard.Begin();
}

// --preview-ui + URNETWORK_PREVIEW_SAMPLE. Two gates, both required: the preview
// flag says there is no session, and the env var says the operator explicitly
// asked for synthetic content. Same contract as PreviewHeroActive.
bool ConnectPage::PreviewSampleActive() const {
  if (!w_.previewUi()) return false;
  wchar_t buffer[8]{};
  const DWORD n = ::GetEnvironmentVariableW(L"URNETWORK_PREVIEW_SAMPLE", buffer, 8);
  return 0 < n && n < 8;
}

// A minute of synthetic throughput ending NOW.
void ConnectPage::PreviewSampleCharts() {
  constexpr int64_t kWindowSeconds = 60;
  auto hash = [](uint32_t v) { return v * 2654435761u; };
  const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
  // the series has to MOVE between pushes or the chart reads as a still image;
  // one bucket per second of wall clock does it and stays deterministic
  const uint32_t phase = static_cast<uint32_t>(now / 1000);
  std::vector<urnet::ThroughputPoint> points;
  for (int64_t i = kWindowSeconds; 0 <= i; --i) {
    urnet::ThroughputPoint point;
    point.Time = now - i * 1000;
    auto sample = [&](uint32_t salt, int64_t scale) {
      urnet::ThroughputSample s;
      const uint32_t v = hash((phase - static_cast<uint32_t>(i)) * 131 + salt);
      s.EgressByteCount = static_cast<int64_t>(v % 100) * scale;
      s.IngressByteCount = static_cast<int64_t>((v >> 8) % 100) * scale * 3;
      s.EgressPacketCount = static_cast<int64_t>((v >> 16) % 90) + 4;
      s.IngressPacketCount = static_cast<int64_t>((v >> 20) % 140) + 6;
      s.EgressBitRate = s.EgressByteCount * 8;
      s.IngressBitRate = s.IngressByteCount * 8;
      return s;
    };
    point.Remote = sample(1, 40000);
    point.Local = sample(2, 6000);
    point.Block = sample(3, 3000);
    points.push_back(point);
  }
  if (remoteChart_) remoteChart_->SetPoints(points, kWindowSeconds);
  if (blockedChart_) blockedChart_->SetPoints(points, kWindowSeconds);
  if (localChart_) localChart_->SetPoints(points, kWindowSeconds);
}

// Fill the panes with obviously synthetic rows.
//
// This exists because an empty pane proves nothing. The entire claim of the R3
// layout is that the window is COVERED by dense uniform rows; a review build
// that renders three empty states is a screenshot of the chrome, not of the
// design. Nothing here touches Sdk(), the network, or any stored state - it
// writes this page's own caches and re-renders, exactly as a live feed would.
void ConnectPage::ApplyPreviewSample() {
  if (!PreviewSampleActive()) return;

  // deterministic, so two runs produce the same screenshot
  auto hash = [](uint32_t v) { return v * 2654435761u; };
  const int64_t nowMillis = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count();

  static const char* kHosts[] = {
      "api.urnetwork.com",   "cdn.cloudflare.net",     "telemetry.microsoft.com",
      "s3.amazonaws.com",    "graph.facebook.com",     "fonts.gstatic.com",
      "doubleclick.net",     "analytics.google.com",   "registry.npmjs.org",
      "github.com",          "ocsp.digicert.com",      "push.apple.com",
      "ads.adservice.net",   "mail.protonmail.ch",     "matrix.org",
      "steamcdn-a.akamaihd.net", "discord.gg",         "wikipedia.org",
      "192.168.1.1",         "1.1.1.1",                "tracker.example.net",
      "signal.org",          "update.mozilla.org",     "duckduckgo.com",
      "metrics.segment.io",  "cdn.jsdelivr.net",       "objects.githubusercontent.com",
      "pixel.quantserve.com", "static.doubleverify.com", "img.shields.io"};
  blockActions_.clear();
  for (uint32_t i = 0; i < static_cast<uint32_t>(std::size(kHosts)); ++i) {
    const uint32_t h = hash(i + 7);
    urnw::BlockActionItem action;
    action.id = "preview-" + std::to_string(i);
    action.hosts = {kHosts[i]};
    action.block = (h >> 5) % 5 == 0;
    action.local = !action.block && (h >> 9) % 7 == 0;
    action.byteCount = static_cast<int64_t>((h >> 11) % 900000) + 512;
    action.packetCount = static_cast<int64_t>((h >> 13) % 4000) + 3;
    // D5: the fields the Advanced-Mode inspector reads. Without them a preview
    // run can only ever screenshot the inspector's "nothing to join against"
    // reading, which is the one reading that does not exercise it. Deterministic
    // and obviously synthetic, like everything else in this function.
    action.ips = {"203.0.113." + std::to_string(1 + (h % 200))};
    // Ages relative to NOW, oldest last. An absolute constant here would be an
    // epoch offset and the inspector would print "496159h ago", which is what
    // the first version of this did.
    action.timeMillis = nowMillis - 1'000 * static_cast<int64_t>(11 * i + (h % 7));
    if ((h >> 17) % 6 == 0) {
      action.overrideId = "preview-override-" + std::to_string(i);
      action.hasBlockOverride = action.block;
      action.hasRouteOverride = action.local;
      action.matchedHosts = {kHosts[i]};
    }
    blockActions_.push_back(action);
  }
  // ...and the routing tables the inspector joins those addresses against. In a
  // real session these come off ReadReliability (DestinationExit -> Exit); here
  // they are generated so the join has something to find. RFC 5737 / TEST-NET-3
  // addresses, so nothing in this block can be mistaken for a real destination.
  exits_.clear();
  destinationExits_.clear();
  for (uint32_t i = 0; i < 4; ++i) {
    const uint32_t h = hash(i + 313);
    urnet::Exit exit;
    exit.ClientId = "preview-exit-" + std::to_string(i);
    exit.Tier = static_cast<int32_t>(1 + (h % 3));
    exit.EffectiveTier = exit.Tier;
    exit.FlowCount = static_cast<int32_t>(4 + (h % 40));
    exit.DialFailureCount = static_cast<int32_t>((h >> 5) % 3);
    exit.Quarantined = i == 3;
    exit.Warning = i == 2;
    exit.WarningCause = i == 2 ? "probe timeout" : "";
    exit.Proven = i < 2;
    exit.ProbeAgeSeconds = static_cast<int64_t>(5 + (h % 90));
    exits_.push_back(exit);
  }
  for (auto const& action : blockActions_) {
    if (action.block || action.ips.empty()) continue;
    urnet::DestinationExit dest;
    dest.DestinationIp = action.ips.front();
    dest.ClientId = exits_[hash(static_cast<uint32_t>(action.ips.front().size())) % 4]
                        .ClientId;
    dest.FlowCount = 1 + static_cast<int32_t>(action.packetCount % 5);
    destinationExits_.push_back(dest);
  }

  contractRows_.clear();
  for (uint32_t i = 0; i < 7; ++i) {
    const uint32_t h = hash(i + 101);
    urnw::ContractPeerRow peer;
    peer.clientId = "0f2a" + std::to_string(1000 + (h % 8999)) + "-c4e1-4b77-9a3d";
    peer.sendByteCount = static_cast<int64_t>(h % 40000000) + 40000;
    peer.receiveByteCount = static_cast<int64_t>((h >> 7) % 90000000) + 90000;
    peer.lastActivityMillis = (h % 4 == 0) ? 0 : 1;
    contractRows_.push_back(peer);
  }

  splitRules_.clear();
  for (auto const& [host, local] : std::initializer_list<std::pair<const char*, bool>>{
           {"printer.local", true},
           {"nas.home.arpa", true},
           {"corp.vpn.example", false},
           {"chat.internal", false},
           {"10.0.0.0/8", true}}) {
    urnw::SplitRule rule;
    rule.overrideId = host;
    rule.hosts = {host};
    rule.routeLocal = local;
    splitRules_.push_back(rule);
  }

  urnet::NetworkPeerList samplePeers;
  for (auto const& [name, spec, providing] :
       std::initializer_list<std::tuple<const char*, const char*, bool>>{
           {"workshop-desktop", "windows", true},
           {"kitchen-pi", "linux/arm64", true},
           {"pixel-8", "android", false},
           {"studio-mbp", "darwin/arm64", true},
           {"attic-nuc", "linux/amd64", false}}) {
    urnet::NetworkPeer peer;
    peer.ClientId = std::string("preview-") + name;
    peer.DeviceName = name;
    peer.DeviceSpec = spec;
    peer.ProvideEnabled = providing;
    samplePeers.push_back(peer);
  }
  peers_ = samplePeers;

  allowedCount_ = 18422;
  blockedCount_ = 1174;
  downBitsPerSecond_ = 24600000;
  upBitsPerSecond_ = 3100000;
  providerCount_ = 4;
  statsConnected_ = true;
  connectStatus_ = ConnectStatus::Connected;
  connected_ = true;

  PreviewSampleCharts();

  ApplyConnectStatus();
  urnw::kit::SetTextOrCollapse(
      w_.ThroughputText(),
      H("↓ " + urnw::FormatBitRate(downBitsPerSecond_) + "   ↑ " +
        urnw::FormatBitRate(upBitsPerSecond_)));
  w_.LiveStatsGroup().Visibility(Visibility::Visible);
  w_.ProviderCountText().Text(
      hstring{urnw::Plural("connected_provider_count", providerCount_)});
  // the provide block: ApplyStats normally paints these and is gated off here
  w_.DiscoverableText().Text(Loc("device_discoverable"));
  w_.ProvideModeDot().Fill(urnw::colors::MakeBrush(urnw::colors::kUrGreen));
  w_.ProvideModeRing().Stroke(urnw::colors::MakeBrush(urnw::colors::kUrGreen));
  w_.ProvideModeRing().Visibility(Visibility::Visible);
  w_.ProvideStatsText().Text(hstring{urnw::Plural("providing_client_count", 3)});
  w_.ProvideStatsRow().Visibility(Visibility::Visible);
  // the DNS rows, from the SDK's own defaults. A pure local lookup - it reads a
  // table compiled into the SDK and makes no request.
  if (auto defaults = urnet::getDefaultDnsResolverSettings()) {
    dnsSettings_ = defaults;
    ApplyDnsCard(dnsSettings_);
  }
  ApplyConnectionsList();
  ApplyContractsList();
  ApplySplitRuleCount();
  ApplySessionRows();
  ApplyPeersList();
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
  //
  // R3: the snapshot is KEPT now. Before the pane shell the only thing ever
  // drawn from it was a count, and a nullopt trigger (a remote attach/detach)
  // means "re-render what you have", not "there are no peers" - so the cache is
  // only replaced when a real list arrives.
  if (peers) peers_ = peers;
  ApplyPeersList();
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
