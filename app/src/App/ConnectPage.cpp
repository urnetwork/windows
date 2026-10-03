// SPDX-License-Identifier: MPL-2.0
#include "pch.h"

#include "ConnectPage.h"
#include "ProvideModeVisual.h"

#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Media.Animation.h>
#include <winrt/Microsoft.UI.Xaml.Shapes.h>  // PeerDot Ellipse.Fill
#include <winrt/Windows.ApplicationModel.DataTransfer.h>  // copy-details DataPackage

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

  // The connections verdict filter. Wired here rather than in markup for the
  // same reason the status-dot taps are: XAML event handlers live on
  // MainWindow, which this page does not own. The selection is seeded BEFORE
  // the handler attaches so the seed itself cannot echo into the handler.
  w_.ConnectionsVerdictBar().SelectedItem(w_.VerdictAllItem());
  w_.ConnectionsVerdictBar().SelectionChanged([weak = w_.get_weak()](auto const&, auto const&) {
    if (auto self = weak.get()) self->connect().OnConnectionsVerdictChanged();
  });
  // The group-by-host toggle rides the same filter row and is wired here for
  // the same reason: XAML event handlers live on MainWindow, which this page
  // does not own.
  w_.ConnectionsGroupToggle().Toggled([weak = w_.get_weak()](auto const&, auto const&) {
    if (auto self = weak.get()) self->connect().OnConnectionsGroupToggled();
  });
  // The clear-filters button at the verdict row's right end - same wiring
  // reason as the bar and the toggle above.
  w_.ConnectionsClearFilters().Click([weak = w_.get_weak()](auto const&, auto const&) {
    if (auto self = weak.get()) self->connect().OnConnectionsClearFilters();
  });
  // The verdict ratio bar's segments wear the SAME three verdict colours the
  // row dots print (UpdateConnectionRow), painted once here: green tunnelled,
  // coral blocked, amber bypassed. colors:: is the source for all three -
  // markup ships UrGreenBrush/UrCoralBrush but no UrAmberBrush, and three code
  // fills keep the bar and the dots on one palette statement. After this the
  // bar's only per-push work is ApplyVerdictRatioBar's star weights.
  auto ratioSegment = [this](uint32_t i) {
    return w_.ConnectionsVerdictRatio().Children().GetAt(i).as<Controls::Border>();
  };
  ratioSegment(0).Background(urnw::colors::MakeBrush(urnw::colors::kUrGreen));
  ratioSegment(1).Background(urnw::colors::MakeBrush(urnw::colors::kUrCoral));
  ratioSegment(2).Background(urnw::colors::MakeBrush(urnw::colors::kUrAmber));
  // the small-height scroll escape for pane B: keep the body under the filter
  // rows viewport-sized, with the floor that engages the outer scroller only
  // when height is scarce (ApplyActivityBodyHeight has the rule)
  w_.ActivityBodyScroll().SizeChanged([weak = w_.get_weak()](auto const&, auto const&) {
    if (auto self = weak.get()) self->connect().ApplyActivityBodyHeight();
  });
  // pane C's chart flex: the body grid's star rows can only share LEFTOVER
  // height, which a scroller never offers (it measures content unbounded), so
  // the body is pinned to the viewport on every pane resize - content shorter
  // than the pane gets the flex, content taller overrides the pin and scrolls
  // (ApplyPaneCBodyHeight has the rule)
  w_.ConnectPaneC().SizeChanged([weak = w_.get_weak()](auto const&, auto const&) {
    if (auto self = weak.get()) self->connect().ApplyPaneCBodyHeight();
  });

  // The inspector's quick actions (D5). Wired here rather than in markup for
  // the same reason as the verdict bar above: XAML Click handlers live on
  // MainWindow, which this page does not own.
  w_.InspectorBlockButton().Click([weak = w_.get_weak()](auto const&, auto const&) {
    if (auto self = weak.get()) self->connect().OnInspectorBlockToggle();
  });
  w_.InspectorRouteButton().Click([weak = w_.get_weak()](auto const&, auto const&) {
    if (auto self = weak.get()) self->connect().OnInspectorRouteToggle();
  });
  w_.InspectorCopyButton().Click([weak = w_.get_weak()](auto const&, auto const&) {
    if (auto self = weak.get()) self->connect().OnInspectorCopyDetails();
  });
  // the rule confirmation and its Undo action. The button is built once and
  // swapped onto the InfoBar per Show - a creation arms it, a removal shows
  // the bare acknowledgement.
  inspectorSnackbar_ =
      std::make_unique<urnw::kit::Snackbar>(w_.InspectorSnackbar(), w_.DispatcherQueue());
  inspectorUndoButton_ = Controls::Button();
  inspectorUndoButton_.Content(winrt::box_value(Adv("adv_undo", L"Undo")));
  inspectorUndoButton_.Click([weak = w_.get_weak()](auto const&, auto const&) {
    if (auto self = weak.get()) self->connect().OnInspectorUndo();
  });

  // the easter egg: five taps on the status dot while connected, each within
  // two seconds of the previous, play the Pro celebration; silent otherwise
  w_.StatusDot().Tapped([weak = w_.get_weak()](auto const&, auto const&) {
    auto self = weak.get();
    if (!self) return;
    ConnectPage& page = self->connect();
    if (page.health_ != urnw::health::State::Connected) {
      page.connectedIconTaps_.Reset();
      return;
    }
    if (page.connectedIconTaps_.Tap(static_cast<int64_t>(GetTickCount64()))) {
      self->LaunchProCelebration();
    }
  });

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
  BuildFoldDoors();  // idempotent: the fold rule's pane A sheet doors
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
  // The connections filter row. The verdict choices reuse the store's own
  // verdict words where they exist ("blocked" ships; the store has no short
  // "All"/"Tunnelled"/"Bypassed" - those are Adv ids, reported like every
  // other). The search field is built once and re-strung on every pass, the
  // NetworkPage::ApplyStrings pattern.
  BuildConnectionsFilter();
  w_.VerdictAllItem().Text(Adv("adv_filter_all", L"All"));
  w_.VerdictBlockedItem().Text(Loc("blocked"));
  w_.VerdictTunnelledItem().Text(Adv("adv_filter_tunnelled", L"Tunnelled"));
  w_.VerdictBypassedItem().Text(Adv("adv_filter_bypassed", L"Bypassed"));
  // The clear-filters reset. "clear" is the shipped key (the store's own
  // comment: "a button that clears a field or setting"), so no Adv id - the
  // one-string-fits rule is exactly what the key exists for. Text content
  // gives the button its automation name for free.
  w_.ConnectionsClearFilters().Content(LocBox("clear"));
  // The group-by-host switch's caption. The switch itself is labelled BY this
  // TextBlock (markup's AutomationProperties.LabeledBy), so there is no second
  // name to string.
  w_.ConnectionsGroupLabel().Text(Adv("adv_group_by_host", L"Group by host"));
  if (connectionsSearch_) {
    connectionsSearch_.PlaceholderText(Adv("adv_search_connections", L"Search hosts or IPs"));
    // a TextBox's placeholder is NOT its accessible name (MakePaneSearchRow
    // sets both, so a re-string sets both)
    pane_automation::AutomationProperties::SetName(
        connectionsSearch_, Adv("adv_search_connections", L"Search hosts or IPs"));
  }
  // the provider extender row (N7): its title, the switch's name, the
  // description under it, and the state line again in the new language
  w_.ExtenderLabel().Text(Loc("extender"));
  winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(w_.ExtenderToggle(),
                                                                       Loc("extender"));
  w_.ExtenderDescription().Text(Loc("extender_setting_description"));
  ApplyExtenderProvideRow();
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
  // The fold-gated sheet doors (BuildFoldDoors built them on the first pass):
  // re-strung here with every other label, and the automation name IS the
  // row's title, so the two are written together.
  for (auto const& d : foldDoors_) {
    const hstring doorText = Loc(d.key);
    d.title.Text(doorText);
    automation::AutomationProperties::SetName(d.root, doorText);
  }
  if (foldDoorHeader_.title) foldDoorHeader_.title.Text(Loc("client_statistics"));
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
  // The status row's labels and lines, and the extender panel's own fixed
  // labels (title, the two automation names); guarded because ApplyStrings
  // also runs before BuildCharts has made them.
  if (ipFamilyStatusRow_) ipFamilyStatusRow_->ApplyStrings();
  if (extenderPanel_) extenderPanel_->ApplyStrings();
  // The snackbar's Undo is built once in Initialize, so its label is re-strung
  // here like every other fixed label; the action-row buttons' labels
  // re-render from ApplyInspector with the rest of the inspector.
  if (inspectorUndoButton_) inspectorUndoButton_.Content(winrt::box_value(Adv("adv_undo", L"Undo")));
  // The plan + usage card that used to sit in this rail is gone from Home
  // (spec §5); its strings now belong only to Account, which paints them from
  // MainWindow::ApplyBalance.
}

// ---- connect -------------------------------------------------------------

void ConnectPage::OnConnectToggle(IInspectable const&, RoutedEventArgs const&) {
  // The failure state's one action is Retry (track 2): rebuild the session.
  // It must win over the disconnect rule below — in the failed state the SDK
  // still holds a destination and the machine is still captured, so the
  // gesture predicate reads Disconnect, but the button SAYS Retry and a press
  // must do what the button says.
  const bool retry = RenderHealth() == urnw::health::State::Failed;
  if (!retry && ConnectActionIsDisconnect()) {
    Sdk().Disconnect();
    return;
  }
  // Connect to what the user PICKED. This button used to call
  // ConnectBestAvailable() unconditionally, while the chooser's own rows
  // connect to their location directly -- so choosing Japan and then pressing
  // Connect silently sent you somewhere else, and the two controls
  // contradicted each other with no way to tell from the UI. Android connects
  // to connectViewModel.selectedLocation; do the same, and fall back to
  // best-available only when there genuinely is no selection (the SDK flags
  // best-available on the selection itself -- IsBestAvailableSelected, the
  // same test the chooser's check glyphs use).
  //
  // Deliberately the IMMEDIATE entry points, not the coalesced row variants
  // (SdkHost::ConnectFromRow): this is a single explicit press of THE connect
  // button, not a hunt through a list, and it also supersedes any row intent
  // still settling.
  const auto selected = Sdk().SelectedLocation();
  const bool bestAvailable = IsBestAvailableSelected(selected);
  if (retry) {
    // The Retry contract: the disconnect tears down the failed session (multi
    // client, routes, window) and the connect below builds a fresh one — the
    // exact manual sequence that reliably recovered the field hangs. Both
    // calls are explicit user-gesture entry points (D8: a retry press IS a
    // connect gesture).
    Sdk().Disconnect();
  }
  // Say "connecting" NOW rather than waiting for the SDK to push it back. On a
  // client that has never run, a Connect press that produces no visible change
  // is indistinguishable from a hang; the next status push corrects this if the
  // SDK disagrees.
  connectStatus_ = ConnectStatus::Connecting;
  // #27: the aggregate must move with the optimistic status, or the render
  // reconciliation in ApplyConnectStatus is the only thing masking a stale
  // health value — keep the two telling the same story from the same instant.
  health_ = urnw::health::State::Connecting;
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
  // the window honesty layer's terminal outcome (track 2). Recognised so the
  // button can offer Retry over a session that still exists; an old SDK never
  // sends it and the fallthrough below stays the fail-safe it always was.
  if (upper == "CONNECT_FAILED") return ConnectStatus::Failed;
  return ConnectStatus::Disconnected;
}

urnw::gesture::ServiceFacts ConnectPage::CurrentServiceFacts() const {
  urnw::gesture::ServiceFacts f;
  f.pipeUp = health_ != urnw::health::State::NoService;
  f.known = f.pipeUp;
  f.routesInstalled = w_.statusRoutesInstalled();
  f.mode = w_.statusSessionMode();
  f.wfpState = w_.statusWfpState();
  f.stopReason = w_.statusStopReason();
  f.state = connected_ ? urnw::proto::TunnelState::Up
                       : urnw::proto::TunnelState::Stopped;
  return f;
}

bool ConnectPage::ConnectActionIsDisconnect() const {
  // It used to be `connectStatus_ != Disconnected` — the SDK status ALONE, and
  // deliberately so: connected_ is also fed by the service pipe's TunnelStatus,
  // so the tunnel can be up with no destination selected, and testing that
  // alone offered "Disconnect" while the line above read "Ready to connect".
  //
  // The SDK status alone was still not the whole question, and the gap is the
  // owner's bug A. Press Disconnect: the SDK settles to DISCONNECTED, this
  // returned false, the button flipped back to "Connect" — over a machine whose
  // capture routes were still installed and which therefore had no internet at
  // all. The button offering to CONNECT was the only control on screen, and it
  // was the wrong one. Both halves are asked now, through the rule the tray
  // shares: anything the SDK is doing, OR a machine still captured.
  return urnw::gesture::ActionIsDisconnect(CurrentServiceFacts(), RenderHealth());
}

urnw::health::State ConnectPage::RenderHealth() const {
  using Health = urnw::health::State;
  // The optimistic local Connecting (a press writes connectStatus_ ahead of
  // any SDK push) and the SDK's own transitional status outrank a health value
  // from an older snapshot — but never outrank the sharper evidence states
  // (Evaluating/Degraded), which are already about a live transition.
  Health render = health_;
  if ((connectStatus_ == ConnectStatus::Connecting ||
       connectStatus_ == ConnectStatus::DestinationSet) &&
      (render == Health::Disconnected || render == Health::NoService ||
       render == Health::Connected)) {
    render = Health::Connecting;
  }
  // Failed is deliberately NOT overridden by a transitional SDK status: during
  // the old-DLL/new-service transition the in-process controller never learns
  // the CONNECT_FAILED word and keeps saying CONNECTING forever, which is the
  // exact infinite yellow this state exists to end. The Retry press does not
  // need the override either — it writes BOTH optimistic values (connectStatus_
  // and health_), so the failure stops rendering at the press itself.
  // ...and the settled idle status outranks a stale ACTIVE health: an unknown/
  // clamped status must never leave the line claiming a connection.
  if (connectStatus_ == ConnectStatus::Disconnected &&
      render != Health::NoService && render != Health::Disconnected) {
    render = Health::Disconnected;
  }
  return render;
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
        // LAYOUT, not content: XAML line-breaking treats an ASCII hyphen as a
        // break opportunity, and the banner's message column is narrow, so
        // "(0.0.0-dev → …)" used to wrap MID-token and orphan "dev)" onto its
        // own line. Non-breaking hyphens (U+2011) inside each version keep a
        // version on one line; the spaces around them stay REGULAR so the
        // parenthetical still wraps BETWEEN tokens. (Making the spaces
        // non-breaking too fused "app. (…)" into one 48-char word, and the
        // InfoBar message's WrapWholeWords CLIPS an overlong word at the
        // column edge instead of wrapping it - the tail vanished.) The
        // snapshot strings themselves are untouched.
        const auto nonBreaking = [](std::wstring version) {
          std::replace(version.begin(), version.end(), L'-', L'\u2011');
          return version;
        };
        message += L" (" + nonBreaking(snap.observation.installedVersion) +
                   L" → " + nonBreaking(snap.observation.siblingVersion) + L")";
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

// The update banner (beta spec §5). Renders MainWindow's snapshot copy onto
// UpdateBar, directly under the service bar — same shape, same one-writer
// rule. Labels go through Adv() with `upd_` ids for the same reason the
// service bar's use `svc_`: the store carries nothing for an update surface,
// and the version string itself is DATA (release grammar, never translated),
// so appending it is composition, not a hidden literal.
void ConnectPage::ApplyUpdateChecker(urnw::UpdateChecker::Snapshot const& snap) {
  using Phase = urnw::UpdateChecker::Phase;
  using Stage = urnw::UpdateChecker::Stage;
  using Failure = urnw::UpdateChecker::Failure;
  auto bar = w_.UpdateBar();
  if (snap.phase == Phase::None) {
    bar.IsOpen(false);
    return;
  }

  // The headline is the spec's wording in every phase — the banner keeps
  // saying what it is FOR while the message says what is happening to it.
  const winrt::hstring title{
      AdvW("upd_available_title", L"Update available:") + L" v" + snap.version};
  winrt::hstring action = Loc("update");
  bool enabled = true;
  std::wstring message;
  auto severity = winrt::Microsoft::UI::Xaml::Controls::InfoBarSeverity::Informational;

  switch (snap.phase) {
    case Phase::Available:
      message = AdvW("upd_available_msi_message",
                     L"One click downloads the release, verifies it and runs "
                     L"its installer, which updates the app and the VPN "
                     L"service. The app closes while it installs.");
      break;
    case Phase::Applying:
      enabled = false;
      switch (snap.stage) {
        case Stage::Downloading:
          message = AdvW("upd_stage_downloading", L"Downloading the update…");
          break;
        case Stage::Verifying:
          message = AdvW("upd_stage_verifying", L"Verifying the download…");
          break;
        default:  // Installing — Idle never renders under Applying
          message = AdvW("upd_stage_installing", L"Starting the installer…");
          break;
      }
      break;
    case Phase::ManualInstall:
      // The one phase whose action is not the apply: the installer could not
      // be started, the verified MSI is downloaded, and the click re-reveals it.
      action = Adv("upd_show_file", L"Show file");
      message = AdvW("upd_manual_install_message",
                     L"The installer didn't start (it needs administrator "
                     L"approval). The verified download was shown in Explorer "
                     L"— quit the app and run it.");
      if (!snap.installerPath.empty())
        message += L" (" + snap.installerPath + L")";
      break;
    default: {  // Failed — Phase::None returned above
      severity = winrt::Microsoft::UI::Xaml::Controls::InfoBarSeverity::Error;
      switch (snap.failure) {
        case Failure::Download:
          message = AdvW("upd_failed_download",
                         L"The download didn't finish. Check the connection "
                         L"and click to try again.");
          break;
        default:  // Checksum
          message = AdvW("upd_failed_checksum",
                         L"The download didn't match the release's checksums, "
                         L"so it was discarded. Click to try again.");
          break;
      }
      break;
    }
  }

  bar.Severity(severity);
  bar.Title(title);
  bar.Message(winrt::hstring{message});
  if (auto button = bar.ActionButton()) {
    button.Content(winrt::box_value(action));
    button.IsEnabled(enabled);
  }
  bar.IsOpen(true);
}

// The connect status line, its dot, and the button label — android
// ConnectStatusIndicator parity. Until now StatusText read the SERVICE tunnel
// state and said only "Connected"/"Disconnected", while the SDK's four-state
// connectionStatus was fetched into LiveStats and dropped on the floor: there
// was no connecting state anywhere in the client.
//
// #27: the line now renders the AGGREGATE health (health_), not the SDK status
// alone. The SDK status says what the connect controller is doing; health says
// whether anything is actually proven to work, which is the claim the word
// "Connected" makes to a user. The button/watchdog still read connectStatus_ —
// what the press DOES is the controller's business — and the two are
// reconciled below for the one instant they can lag each other.
void ConnectPage::ApplyConnectStatus() {
  using Health = urnw::health::State;
  // One reading of this instant, shared with the button label — see
  // RenderHealth, which used to be inlined here and therefore could not be.
  const Health render = RenderHealth();
  // THE SECOND AXIS. Health is a pure function of what the SDK is CARRYING, and
  // that is right; what no surface consulted was the service fact sitting one
  // field away. The rule this adds is one line: no surface may print the word
  // for Disconnected while this machine's routes are still installed.
  const bool captured = w_.statusRoutesInstalled();

  hstring text;
  winrt::Windows::UI::Color dot = urnw::colors::kStatusIdle;
  auto heroConnection = urnw::ConnectCanvas::State::Disconnected;
  // The two balance states iOS's ConnectButtonView layers OVER the connection
  // state, read from the same two fields MainWindow::UpdateBalanceWarning gates
  // the InfoBar on, so the hero and the InfoBar cannot disagree. A running
  // post-checkout confirmation poll wins over an out-of-balance account: the
  // balance is mid-flight, and showing a warning for it would be wrong.
  const bool processing = w_.balanceConfirming();
  const bool outOfBalance = !processing && w_.balanceBlocked();
  switch (render) {
    case Health::Connected:
      // the provider count lives in its own line below (ProviderCountText),
      // where android folds it into this string — desktop has room for both
      text = Loc("connected");
      dot = urnw::colors::kUrGreen;
      heroConnection = urnw::ConnectCanvas::State::Connected;
      break;
    case Health::Evaluating:
      // The honest name for "all yellow": the window has providers and none is
      // proven yet. The hero stays in its Connecting state ON PURPOSE — that is
      // the state in which SetGrid keeps taking updates, so the per-provider
      // dots keep telling the same story instead of freezing mid-evaluation
      // under a green headline.
      text = Adv("conn_finding_providers", L"Finding providers…");
      dot = urnw::colors::kStatusConnecting;
      heroConnection = urnw::ConnectCanvas::State::Connecting;
      break;
    case Health::Degraded:
      // Was connected, proven dropped and stayed dropped past the hold. The
      // SDK reconnects on its own; the copy says so rather than asking for a
      // click the recovery does not need. (Disconnect IS one click away — the
      // button below reads connectStatus_, which is still active here.)
      text = Adv("conn_degraded", L"Connection degraded — reconnecting");
      dot = urnw::colors::kUrCoral;
      heroConnection = urnw::ConnectCanvas::State::Connecting;
      break;
    case Health::Connecting:
      // android maps DESTINATION_SET and CONNECTING to one connecting state
      text = Loc("connecting_status_indicator");
      dot = urnw::colors::kStatusConnecting;
      heroConnection = urnw::ConnectCanvas::State::Connecting;
      break;
    case Health::Failed:
      // The window honesty layer's terminal outcome: zero providers Added
      // past both outcome deadlines, one automatic rebuild already spent.
      // The reason line below says WHY; the button says Retry. The hero shows
      // its error form — this is a settled failure, not a transition, and the
      // climbing-dots animation would contradict the words.
      text = Adv("conn_failed", L"Couldn't connect");
      dot = urnw::colors::kUrCoral;
      heroConnection = urnw::ConnectCanvas::State::Error;
      break;
    case Health::NoService:
    case Health::Disconnected:
      // R1: PROTECTION STATE, not readiness. The owner reconciliation is explicit
      // that "{network} is ready to connect" reads as backend readiness, not
      // protection, and must go. The spec's headline is "Not connected" with a
      // "Your internet traffic is not protected" supporting line; neither ships,
      // so the closest shipped protection word - "Disconnected" - leads instead,
      // and the two missing lines are reported for the store. The network name is
      // no longer folded into the hero headline; it lives in the status strip.
      // (NoService deliberately shares the word: the service-setup banner above
      // this line is the surface that explains WHY there is no service.)
      //
      // ...UNLESS THE MACHINE IS STILL CAPTURED. This is the row that did not
      // exist, and its absence is the whole of the owner's bug A: the SDK had
      // stopped, so the page printed "Disconnected" — while 31 capture routes
      // and a firewall policy were still in force and the machine had no
      // internet at all. After the fix this state is the fraction of a second
      // in which the stop is in flight, so the word is a transient one; if it
      // persists, the tray's recovery item is the escape and the button below
      // still reads Disconnect, which is the control that clears it.
      if (captured) {
        text = Adv("conn_disconnecting",
                   L"Disconnecting — your traffic is still going through the "
                   L"tunnel");
        dot = urnw::colors::kStatusConnecting;
        heroConnection = urnw::ConnectCanvas::State::Connecting;
        break;
      }
      // ...and the OTHER honest word this line never had. Routes are gone but
      // the armed policy is still holding this machine, which is the kill
      // switch doing exactly what it says — and "Disconnected" describes it
      // about as well as it described the captured state above. It is also the
      // headline the tray tooltip has always used; the two now agree.
      if (w_.statusWfpState() == "armed") {
        text = Adv("conn_blocked_kill_switch", L"Blocked — kill switch on");
        dot = urnw::colors::kUrCoral;
        heroConnection = urnw::ConnectCanvas::State::Disconnected;
        break;
      }
      text = Loc("disconnected");
      dot = urnw::colors::kStatusIdle;
      heroConnection = urnw::ConnectCanvas::State::Disconnected;
      break;
  }
  w_.StatusText().Text(text);

  // ---- soft-kill-switch honesty (#27; RECOVERY.md's "says Connected but
  // nothing works" gap) ------------------------------------------------------
  // With the tunnel up and nothing proven, traffic is being routed into the
  // tunnel and going nowhere — that is the soft kill switch DOING ITS JOB, and
  // the one thing worse than the stall is not saying it. Two variants because
  // the fails-closed guarantee is only true while the service reports a
  // firewall policy in force; wfp_state == "off" over a live tunnel (a failed
  // or unelevated install) must not be described as protection.
  {
    std::wstring held;
    if (connected_ &&
        (render == Health::Evaluating || render == Health::Degraded ||
         render == Health::Failed)) {
      if (w_.statusWfpState() != "off") {
        held = AdvW("conn_traffic_blocked",
                    L"No working provider right now — your traffic is blocked, "
                    L"not exposed. Disconnect to go back to your normal "
                    L"connection.");
      } else {
        held = AdvW("conn_traffic_blocked_unprotected",
                    L"No working provider right now — traffic sent into the "
                    L"tunnel is going nowhere, and leak protection is off, so "
                    L"some traffic may bypass it. Disconnect to go back to "
                    L"your normal connection.");
      }
      // #41 THE PRE-EMPTIVE HALF. The service will turn this tunnel off by
      // itself if nothing gets through, and a teardown nobody was warned about
      // reads as a crash however good the explanation afterwards is. Appended to
      // the existing line rather than shown as a second surface, because it is
      // the same subject: this is what happens next if the state above does not
      // change.
      if (w_.statusFailsafeArmed()) {
        held += L" ";
        held += AdvW("conn_failsafe_armed",
                     L"If nothing gets through shortly, URnetwork will turn the "
                     L"tunnel off automatically so you keep your internet.");
      }
    } else if (!connected_ && proto::IsFailsafeStop(w_.statusStopReason())) {
      // #41 THE EXPLANATION. Rendered while DISCONNECTED, which is the one state
      // this line has never had anything to say in — and the exact state a user
      // lands in when the service tore the tunnel down without being asked.
      //
      // Two variants, and the difference is not decoration: with the kill switch
      // on the machine is STILL BLOCKED, and telling that user "your traffic is
      // going out normally" would be false in the direction that matters.
      held = w_.statusWfpState() != "off"
                 ? AdvW("conn_failsafe_blocked",
                        L"The tunnel could not carry traffic, so URnetwork shut "
                        L"it down. The kill switch is on, so nothing leaves "
                        L"this machine until you connect again or turn the kill "
                        L"switch off — nothing is leaking.")
                 : AdvW("conn_failsafe_restored",
                        L"URnetwork disconnected you to keep you online: the "
                        L"tunnel was up but nothing was getting through. Your "
                        L"traffic is going out normally now and is NOT "
                        L"protected. Press Connect to try again.");
    }
    urnw::kit::SetTextOrCollapse(w_.TrafficHeldText(), winrt::hstring{held});
  }

  // ---- the stall reason line (track 2 window honesty) ----------------------
  // One writer, under the status line: while the attempt is yellow the SDK's
  // machine-readable diagnosis becomes a human sentence, and in the failure
  // state it explains what failed. Empty (idle, connected, old service, or
  // plain "evaluating" — the headline already says "Finding providers…")
  // collapses the line. Store-first ids with english fallbacks, the Adv()
  // convention every not-yet-in-store string on this page uses.
  {
    std::wstring reason;
    const bool stalled = render == Health::Connecting ||
                         render == Health::Evaluating ||
                         render == Health::Degraded ||
                         render == Health::Failed;
    if (stalled) {
      if (windowStallReason_ == "platform-unreachable") {
        reason = AdvW("conn_reason_platform", L"Contacting the platform…");
      } else if (windowStallReason_ == "providers-unresponsive") {
        reason = AdvW("conn_reason_providers",
                      L"Providers not responding — retrying…");
      } else if (windowStallReason_ == "rate-limited") {
        reason = AdvW("conn_reason_rate_limited", L"Rate limited — waiting…");
      } else if (windowStallReason_ == "auth-failing") {
        reason = AdvW("conn_reason_auth",
                      L"Signing in to the platform is failing…");
      }
    }
    if (render == Health::Failed && reason.empty()) {
      reason = AdvW("conn_failed_detail",
                    L"No providers could be reached. Retry rebuilds the "
                    L"connection from scratch.");
    }
    urnw::kit::SetTextOrCollapse(w_.StatusReasonText(), winrt::hstring{reason});
  }
  w_.StatusDot().Fill(urnw::colors::MakeBrush(dot));
  // The window's status strip shows this same state on every OTHER destination.
  // It reads the derivation rather than repeating it, for the reason the hero
  // and the balance InfoBar already share one: two switches over
  // connectionStatus in two files is two places for them to disagree, and the
  // strip is visible while the connect screen is not.
  w_.ApplyStatusStripConnection(text, dot);
  // In the failed state the one action is Retry — the gesture predicate reads
  // Disconnect (the SDK still holds a destination, the machine is captured),
  // but a failure whose only offered control is "Disconnect" strands the user
  // one manual step from the retry that usually works. OnConnectToggle keeps
  // the two in agreement: a press in this state disconnects AND reconnects.
  const bool failedAction = render == Health::Failed;
  const bool disconnectAction = !failedAction && ConnectActionIsDisconnect();
  w_.ConnectButton().Content(failedAction
                                 ? LocBox("retry")
                                 : (disconnectAction ? LocBox("disconnect")
                                                     : LocBox("connect")));
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
  // the line above it. The connection half (heroConnection) was derived in the
  // health switch above so it CANNOT disagree with the words; the two balance
  // states still layer over it, exactly as before.
  auto heroState = heroConnection;
  if (processing) {
    heroState = urnw::ConnectCanvas::State::Processing;
  } else if (outOfBalance) {
    heroState = urnw::ConnectCanvas::State::Error;
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
  // The provider name is an external string: filtered where it enters the
  // page, so the row AND the automation name built from it stay shapeable.
  std::string locationName = urnw::kit::SanitizeExternalDisplayText(stats.locationName);
  const auto peers = Sdk().ConnectedProvidePeers();
  if (auto selected = Sdk().SelectedLocation();
      peers && selected && selected->connect_location_id &&
      selected->connect_location_id->client_id &&
      !selected->connect_location_id->client_id->empty()) {
    const auto& clientId = *selected->connect_location_id->client_id;
    for (const auto& peer : *peers) {
      if (peer.ClientId && *peer.ClientId == clientId) {
        // a peer's device name is off the wire too: same filter, same reason
        locationName = urnw::kit::SanitizeExternalDisplayText(urnw::PeerDisplayName(peer));
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
  // the stall diagnosis rides the same snapshot (track 2); rendered by
  // ApplyConnectStatus as the reason line under the hero
  windowStallReason_ = stats.windowStallReason;
  // #27: the aggregate health, derived in the SAME ReadStats as every other
  // field of this snapshot — the status line renders THIS, not the raw status.
  health_ = stats.health;
  healthReevalAtMillis_ = stats.healthReevalAtMillis;
  // The provider grid, into the hero. This is the first consumer
  // getProviderGridPointList() has ever had in this client. An empty list is
  // normal (no session, rpc-only, or a connection that has not placed a
  // provider yet) and the canvas renders it as its bare lattice, so it is fed
  // through unconditionally rather than gated on non-empty.
  if (canvas_ && !PreviewHeroActive()) {
    canvas_->SetGrid(stats.gridPoints, stats.gridWidth, stats.gridHeight);
  }
  // The same grid, counted by proven address family for the drawer. Unlike
  // the hero this never freezes on connect: which exits can carry v6 is live
  // information for as long as the window is.
  if (ipFamilyStatusRow_ && !PreviewHeroActive()) {
    ipFamilyStatusRow_->SetGrid(stats.gridPoints);
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
  // #27: gated on the AGGREGATE, not the raw SDK bit — "Connected to N
  // providers" under a headline reading "Finding providers…" is the exact
  // contradiction the aggregate exists to remove.
  // While connecting the same row reads "Connecting to providers" and still
  // opens the sheet (it lists the providers known so far, or its own
  // "Connecting to providers" empty line) — android/apple parity, where the
  // status label is the tap target in both states.
  const bool providersConnecting = stats.health == urnw::health::State::Connecting;
  urnw::kit::SetTextOrCollapse(
      w_.ProviderCountText(),
      stats.connected && stats.health == urnw::health::State::Connected
          ? hstring{urnw::Plural("connected_provider_count", stats.providerCount)}
          : providersConnecting ? hstring{Loc("connecting_status_indicator")}
                                : hstring{L""});

  // Live throughput feed: down / up bit rate. This is the ACTIVITY PANE's own
  // header figure now — the pane whose chart and connections table it describes
  // — rather than a line inside a card two columns away.
  urnw::kit::SetTextOrCollapse(
      w_.ThroughputText(),
      stats.connected ? H("↓ " + urnw::FormatBitRate(stats.downBitsPerSecond) +
                          "   ↑ " + urnw::FormatBitRate(stats.upBitsPerSecond))
                      : hstring(L""));
  w_.LiveStatsGroup().Visibility(stats.connected || providersConnecting
                                     ? Visibility::Visible
                                     : Visibility::Collapsed);
  // The fold-gated globe door follows pane C's ProviderCountLine rule exactly
  // (it sits inside LiveStatsGroup): hidden while there is no session to draw.
  if (foldDoorGlobeRow_) {
    foldDoorGlobeRow_.Visibility(stats.connected || providersConnecting
                                     ? Visibility::Visible
                                     : Visibility::Collapsed);
  }
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
  // the extender switch's guess needs whether the device is providing, the
  // same fact the SDK's not_providing state reports (N3)
  provideEnabled_ = stats.provideEnabled;

  // provide indicator (apple parity). The effective provide mode is a bit set
  // (0 none, 1 network, 2 friends-and-family, 3 public) — per-case only.
  // Solid dot = Network tier; dot + outer ring = Public tier (amber while
  // paused — pause stops public only); coral = not providing.
  const auto provideVisual = urnw::ProvideModeVisualFor(stats.provideMode, stats.providePaused);
  const auto provideColor = provideVisual.color;
  const bool provideRing = provideVisual.ring;
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
      // a synthetic family too, so the drawer's status row fills in the
      // preview in the design's expected proportions (mostly dualstack)
      switch ((h >> 12) % 6) {
        case 0: p.IpFamily = "v4-only"; break;
        case 1: p.IpFamily = "v6-only"; break;
        default: p.IpFamily = "dualstack"; break;
      }
      p.Active = true;
      // A few of the preview's providers are reached through an extender, so
      // the rings of EXTENDER.md K2 are visible in --preview-ui at one, two and
      // four addresses (four is the collapsed dashed third ring). The colours
      // are the SDK's own pinned values for these addresses.
      switch ((h >> 18) % 16) {
        case 0:
          p.ExtenderIps = "192.0.2.1";
          p.ExtenderColorHexes = "3cdd67";
          break;
        case 1:
          p.ExtenderIps = "192.0.2.1,2001:db8::1";
          p.ExtenderColorHexes = "3cdd67,dd4f3c";
          break;
        case 2:
          p.ExtenderIps = "192.0.2.1,2001:db8::1,198.51.100.7,203.0.113.42";
          p.ExtenderColorHexes = "3cdd67,dd4f3c,8fd0e8,e8c23c";
          break;
        default: break;  // most providers are reached directly
      }
      points.push_back(p);
    }
  }
  canvas_->SetGrid(points, kCols, kCols);
  if (ipFamilyStatusRow_) ipFamilyStatusRow_->SetGrid(points);
  if (extenderPanel_) {
    // the panel has no feed in preview (there is no device), so give it a
    // plausible one rather than leaving the row saying 0 of 0 forever
    urnw::ExtenderStatusView preview;
    preview.gossipState = (seed % 12) < 8   ? urnw::kGossipStateConnected
                          : (seed % 12) < 10 ? urnw::kGossipStateConnecting
                                             : urnw::kGossipStateDisconnected;
    preview.activeCount = 2;
    preview.reserveCount = 7;
    preview.eventCountLastMinute = static_cast<int64_t>(seed % 5);
    preview.extenders = {
        urnw::ExtenderInfoView{"192.0.2.1", "3cdd67", 1},
        urnw::ExtenderInfoView{"2001:db8::1", "dd4f3c", 1},
        urnw::ExtenderInfoView{"198.51.100.7", "8fd0e8", 0},
    };
    extenderPanel_->SetStatus(preview);
  }
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
  // R3: a chart is now full-bleed to its pane's edge, so anything it overdraws
  // lands in the pane next door.
  urnw::kit::ClipToBounds(w_.RemoteChartHost());
  urnw::kit::ClipToBounds(w_.BlockedChartHost());
  urnw::kit::ClipToBounds(w_.LocalChartHost());
  // The transport distribution bar (TRANSPORTSTATS): the window's remote traffic
  // by transport, full width directly under the Remote plot in the activity
  // pane. Its click opens the client transport settings editor; the bar is its
  // own row (a pane-row Button), so there is no surrounding card click to win
  // over here. The window may be gone by the time a click lands, so the
  // callback resolves the weak window ref like every other XAML handler.
  transportBar_ = std::make_unique<urnw::TransportBar>(
      w_.TransportBarHost(), [weak = w_.get_weak()] {
        if (auto self = weak.get()) {
          self->connect().ShowTransportSettingsSheet(urnw::TransportSettingsKind::Client);
        }
      });
  // The IP-family status row (IPV6.md D2), directly under the transport bar in
  // its own host row: the Dualstack / IPv4 / IPv6 columns with their connected
  // and connecting counts. Fed by ApplyStats from the same grid push the hero
  // reads.
  ipFamilyStatusRow_ = std::make_unique<urnw::IpFamilyStatusRow>(w_.IpFamilyStatusRowHost());
  // The extender panel (EXTENDER.md K4), its own host row directly under the
  // status row: the extenders carrying live connections, the usable count, and
  // the gossip network's state. Fed by the SDK's once-a-second extender status
  // listener rather than by the stats tick -- it is a property of the network,
  // not of this window's traffic.
  extenderPanel_ = std::make_unique<urnw::ExtenderPanel>(w_.ExtenderPanelHost());
}

// ---- the fold-gated sheet doors (the fold rule) -----------------------------
// Pane C folds below 1042dip of nav content (MainWindow::ApplyBreakpoint's
// connectThree gate), and pane C owns the only doors to five sheets: split
// rules (its group-header action AND the Advanced inspector's Reason-row link),
// the DNS editor, client contracts, the provider-locations globe - and the
// transport settings editor, whose bar rides pane B, gone below 640dip of
// window. The fold rule bars a foldable pane from owning content with no
// second door, so the doors are built TWICE, the Support-contact pattern:
// once in pane C and once here, and ApplyPaneCFolded shows exactly one set.
void ConnectPage::BuildFoldDoors() {
  if (foldDoorsBuilt_) return;
  foldDoorsBuilt_ = true;
  // Pane A's scroller content has no x:Name (a new one is a stale-object
  // startup crash without a full .obj wipe), so the host is reached, not named:
  // markup's PaneAScroll ScrollViewer has exactly one child, the pane's
  // StackPanel. The section inserts ahead of the peers GROUP HEADER - the child
  // right before PeersHost - so it closes the pane's fixed controls rather than
  // appending under the pane's one list; if that shape ever changes the
  // fallback appends and the section lands at the foot, still correct.
  auto panel = w_.PaneAScroll().Content().try_as<Controls::StackPanel>();
  if (!panel) return;
  foldDoorHost_ = Controls::StackPanel();
  foldDoorHost_.Visibility(paneCFolded_ ? Visibility::Visible : Visibility::Collapsed);
  uint32_t peersIndex = 0;
  if (panel.Children().IndexOf(w_.PeersHost(), peersIndex) && 0 < peersIndex) {
    panel.Children().InsertAt(peersIndex - 1, foldDoorHost_);
  } else {
    panel.Children().Append(foldDoorHost_);
  }

  // The section IS the folded statistics pane's doors, so it wears that pane's
  // title. Every label below is a shipped store key - no Adv() fallbacks.
  foldDoorHeader_ = urnw::kit::MakePaneGroupHeader(Loc("client_statistics"));
  foldDoorHost_.Children().Append(foldDoorHeader_.root);
  auto door = [this](std::string_view key, auto const& open) {
    auto row = urnw::kit::MakePaneTwoLineRowButton(Loc(key));
    row.root.Click([weak = w_.get_weak(), open](auto const&, auto const&) {
      if (auto self = weak.get()) open(self->connect());
    });
    foldDoors_.push_back({key, row.root, row.title});
    foldDoorHost_.Children().Append(row.root);
    return row;
  };
  door("client_contracts", [](ConnectPage& page) { page.ShowClientContractsSheet(); });
  door("split_rules", [](ConnectPage& page) { page.ShowSplitRulesSheet(); });
  door("custom_dns", [](ConnectPage& page) { page.ShowDnsSheet(); });
  door("transports", [](ConnectPage& page) {
    page.ShowTransportSettingsSheet(urnw::TransportSettingsKind::Client);
  });
  // The globe door goes through OnProviderCountClick so the two rows can never
  // disagree about when the sheet opens (the guard is the handler's; the args
  // are unused). Its VISIBILITY follows the session like pane C's own row -
  // ApplyStats writes it next to LiveStatsGroup, and it starts hidden the way
  // LiveStatsGroup starts collapsed: before the first push there is no session
  // to draw either.
  foldDoorGlobeRow_ = door("provider_locations_title", [](ConnectPage& page) {
                           page.OnProviderCountClick(nullptr, nullptr);
                         }).root;
  foldDoorGlobeRow_.Visibility(Visibility::Collapsed);
}

void ConnectPage::ApplyPaneCFolded(bool folded) {
  paneCFolded_ = folded;
  if (!foldDoorHost_) return;  // not built yet; BuildFoldDoors replays the state
  foldDoorHost_.Visibility(folded ? Visibility::Visible : Visibility::Collapsed);
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
        // The stats pair is two of the ratio bar's three inputs (the local
        // third rides the block-actions push into ApplyConnectionsList), so
        // this handler rebuilds the bar the way it rebuilds the header count.
        page.ApplyVerdictRatioBar();
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
        // The quick actions read the SAME publish (through CurrentHostRules),
        // so a rule written from any surface - the sheet, not only the
        // inspector's own buttons - re-renders the on/off state here.
        page.ApplyInspector();
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
        page.dnsSettled_ = true;  // a push is a reading, present or not
        page.dnsSettings_ = settings;
        page.ApplyDnsCard(settings);
      }
    });
  });
  // the transport distribution: SdkHost reads it on the same throughput tick as
  // the points and pushes only when it changed, so this can apply every push
  sdk.SetTransportDistributionHandler([queue, weak](urnw::TransportDistributionSnapshot d) {
    queue.TryEnqueue([weak, d = std::move(d)] {
      if (auto self = weak.get()) {
        auto& page = self->connect();
        if (page.transportBar_) page.transportBar_->SetDistribution(d);
      }
    });
  });
  // the extender network (K4, K5): SdkHost maps the SDK status to the plain
  // view the panel draws and pushes only when it changed, so this can apply
  // every push
  sdk.SetExtenderStatusHandler([queue, weak](urnw::ExtenderStatusView status) {
    queue.TryEnqueue([weak, status = std::move(status)] {
      if (auto self = weak.get()) {
        auto& page = self->connect();
        if (page.extenderPanel_) page.extenderPanel_->SetStatus(status);
      }
    });
  });
  // the transport policies in force: cached for the editor (an open editor is
  // not reset by a push, dns parity); the bar's unused footer follows the policy
  // through the SDK view controller's enabled flags, not through this
  sdk.SetTransportSettingsHandler([queue, weak](urnw::TransportSettingsKind kind,
                                                std::optional<urnet::TransportSettings> settings) {
    queue.TryEnqueue([weak, kind, settings = std::move(settings)] {
      if (auto self = weak.get()) {
        auto& page = self->connect();
        if (kind == urnw::TransportSettingsKind::Provider) {
          page.providerTransportSettings_ = settings;
        } else {
          page.clientTransportSettings_ = settings;
        }
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
  // the connected providers and where they are: SdkHost re-reads the getter on
  // the signal-only change listener and only pushes when the rows differ by
  // value, so this can apply every push directly
  sdk.SetProviderLocationsHandler([queue, weak](std::vector<urnw::ProviderLocationRow> rows) {
    queue.TryEnqueue([weak, rows = std::move(rows)] {
      if (auto self = weak.get()) {
        auto& page = self->connect();
        page.providerLocations_ = rows;
        if (page.providerLocationsSheet_) {
          page.providerLocationsSheet_->Update(rows, Sdk().RemoteConnected());
        }
      }
    });
  });
  // the verified-e2e identity set behind the locations badge: signal-only feed,
  // pushed the same way and applied to the sheet if it is open
  sdk.SetProviderIdentitiesHandler(
      [queue, weak](std::vector<urnw::ProviderIdentityRow> identities) {
        queue.TryEnqueue([weak, identities = std::move(identities)] {
          if (auto self = weak.get()) {
            auto& page = self->connect();
            page.providerIdentities_ = identities;
            if (page.providerLocationsSheet_) {
              page.providerLocationsSheet_->UpdateIdentities(identities);
            }
          }
        });
      });
  // the globe's selection, owned by the SDK view controller (a wheel step lands
  // here). Signal-only and fired under the host's lock, so the selection is
  // read back on the UI thread, never in the callback.
  sdk.SetProviderSelectionHandler([queue, weak] {
    queue.TryEnqueue([weak] {
      if (auto self = weak.get()) {
        auto& page = self->connect();
        if (page.providerLocationsSheet_) page.providerLocationsSheet_->RefreshSelection();
      }
    });
  });
  sdk.SetRemoteChangedHandler([queue, weak](bool) {
    queue.TryEnqueue([weak] {
      if (auto self = weak.get()) {
        auto& page = self->connect();
        // remote attach/detach flips the peers line's disabled state; the
        // nullopt trigger leaves the chooser's peer rows untouched
        page.ApplyPeerCount(std::nullopt);
        // the same fact grays the provider-locations list: while the rpc is
        // down an empty window is unavailable, not "no providers"
        if (page.providerLocationsSheet_) {
          page.providerLocationsSheet_->Update(page.providerLocations_,
                                               Sdk().RemoteConnected());
        }
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
  if (transportBar_) transportBar_->SetDistribution(sdk.CurrentTransportDistribution());
  if (extenderPanel_) extenderPanel_->SetStatus(sdk.CurrentExtenderStatus());
  ApplyExtenderProvideState(sdk.CurrentExtenderProvideStatus());
  clientTransportSettings_ = sdk.CurrentTransportSettings(urnw::TransportSettingsKind::Client);
  providerTransportSettings_ =
      sdk.CurrentTransportSettings(urnw::TransportSettingsKind::Provider);
  ApplySplitRuleCount();  // also rebuilds the split-rules list
  ApplyDnsCard(dnsSettings_);
  ApplyConnectionsList();
  ApplyContractsList();
  ApplySessionRows();
  SeedConnectControls();
  BeginPlaceholders();
}

// ---- DESIGNSTYLE "Placeholders, not pop-in" -----------------------------------

// How long a loading skeleton may stand before the section settles on its
// empty reading (DESIGNSTYLE: a placeholder must resolve). The device is up
// well inside this after a sign-in; past it, the honest reading is the one the
// feeds gave — the unavailable row, an empty transport track.
constexpr int64_t kPlaceholderCeilingMillis = 6000;

namespace {
int64_t SteadyMillis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
}  // namespace

// Arm the skeletons for whatever has not been read yet: on every ResyncDrawer
// (login / re-show) — no shares on a signed-in page means the device is still
// coming up, and the reading is worth waiting for.
void ConnectPage::BeginPlaceholders() {
  placeholdersSinceMillis_ = SteadyMillis();
  if (!dnsSettings_) {
    dnsSettled_ = false;
    ApplyDnsCard(dnsSettings_);
  }
  if (transportBar_ && Sdk().CurrentTransportDistribution().shares.empty()) {
    transportBar_->BeginLoading();
  }
}

// The ceiling: whatever is still a skeleton becomes its empty reading, in the
// same box. Idempotent — a section that settled on real data is untouched.
void ConnectPage::SettlePlaceholders() {
  placeholdersSinceMillis_ = 0;
  if (!dnsSettled_) {
    dnsSettled_ = true;
    ApplyDnsCard(dnsSettings_);
  }
  if (transportBar_ && transportBar_->IsLoading()) transportBar_->SettleEmpty();
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

// ---- the provider extender row (connect/EXTENDER.md N7) -----------------------

void ConnectPage::ApplyExtenderProvideState(urnw::ExtenderProvideStatusView const& view) {
  // a pushed status always replaces the switch's guess
  extenderProvideView_ = view;
  ApplyExtenderProvideRow();
}

void ConnectPage::OnExtenderToggled(IInspectable const&, RoutedEventArgs const&) {
  if (updatingControls_) return;
  // Never written while the row is hidden (N1): a device that reports the role
  // unsupported may be a daemon that cannot take the setting at all.
  if (!extenderProvideView_.supported) return;
  const bool on = w_.ExtenderToggle().IsOn();
  Sdk().SetProvideExtender(on);
  // Repaint now rather than a device epoch later: grey Off, yellow Setting up
  // while providing, grey Not providing while not. The next pushed status
  // replaces the guess.
  extenderProvideView_ = urnw::ExtenderProvideGuessFor(extenderProvideView_, on, provideEnabled_);
  ApplyExtenderProvideRow();
}

void ConnectPage::ApplyExtenderProvideRow() {
  const urnw::ExtenderProvideRowModel model =
      urnw::ExtenderProvideRowModelFor(extenderProvideView_);
  // Hidden, never disabled (N1): a device without the role shows the provide
  // group exactly as before, and the description goes with the row.
  const Visibility shown = model.visible ? Visibility::Visible : Visibility::Collapsed;
  w_.ExtenderRow().Visibility(shown);
  w_.ExtenderDescriptionRow().Visibility(shown);
  const hstring text{urnw::ExtenderProvideText(model)};
  // the provide dot's drawing; a new state repaints it at once, with no motion
  w_.ExtenderDot().Fill(urnw::colors::MakeBrush(urnw::ExtenderProvideToneColor(model.tone)));
  w_.ExtenderNote().Text(text);
  w_.ExtenderNote().Foreground(urnw::ExtenderProvideNoteBrush(model.tone));
  // the note style cuts the line at the row's width, so the whole line rides
  // the tooltip: a listen failure names every carrier
  ToolTipService::SetToolTip(w_.ExtenderNote(),
                             text.empty() ? IInspectable{nullptr} : winrt::box_value(text));
  // the switch is named "Extender" (ApplyStrings) and carries the state as its
  // help text; the dot is decorative (markup)
  winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::SetHelpText(
      w_.ExtenderToggle(), text);
  // the switch is the setting, read beside the status; the echo guard keeps
  // the repaint from writing it back
  if (w_.ExtenderToggle().IsOn() != model.on) {
    updatingControls_ = true;
    w_.ExtenderToggle().IsOn(model.on);
    updatingControls_ = false;
  }
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

// A connection row's meta line: when the routing decision was made, then the
// volume totals the line has always carried. The age re-renders on the 1s
// clock (RefreshConnectionRowTimes) from the row's cached fields, so it stays
// honest without a feed push. timeMillis is unix-ms; 0 means the feed predates
// the field, and the prefix is simply absent rather than a 56-year age.
std::string BlockActionMeta(int64_t timeMillis, int64_t byteCount, int64_t packetCount,
                            int64_t nowMillis) {
  std::string meta;
  if (0 < timeMillis) {
    meta = urnw::RelativeTime(timeMillis, nowMillis);
    meta += "   ";
  }
  meta += urnw::FormatByteCountCompact(byteCount) + "   " +
          urnw::FormatCountCompact(packetCount) + " pkt";
  return meta;
}

// The fold count as words, for a group row's meta and its accessible name.
// "1 connection" / "N connections": no store key plurals "connection" today
// (host_count is the nearest and names the wrong thing), so the Adv pair is
// reported with the rest of this surface.
std::string GroupConnectionsWord(int64_t connections) {
  return std::to_string(connections) + " " +
         (connections == 1 ? Narrow(Adv("adv_connection_count_one", L"connection"))
                           : Narrow(Adv("adv_connection_count", L"connections")));
}

// A GROUP row's meta line: the fold count first - it is what makes the row a
// group - then the same age / bytes / packets figures every connection row
// prints, summed over the group with the LATEST decision's age. The age
// re-renders on the 1s clock like any other row's.
std::string GroupConnectionsMeta(int64_t connections, int64_t timeMillis,
                                 int64_t byteCount, int64_t packetCount,
                                 int64_t nowMillis) {
  std::string meta = GroupConnectionsWord(connections);
  meta += "   ";
  if (0 < timeMillis) {
    meta += urnw::RelativeTime(timeMillis, nowMillis);
    meta += "   ";
  }
  meta += urnw::FormatByteCountCompact(byteCount) + "   " +
          urnw::FormatCountCompact(packetCount) + " pkt";
  return meta;
}
}  // namespace

// The verdict filter's membership test. "Tunnelled" and "Bypassed" split the
// old two-way "allowed": both pass traffic, but only one of them protects it -
// the same three-way reading the row dots and the inspector already print.
bool ConnectPage::VerdictPassesFilter(ConnectionVerdictFilter filter,
                                      urnw::BlockActionItem const& action) {
  switch (filter) {
    case ConnectionVerdictFilter::Blocked:
      return action.block;
    case ConnectionVerdictFilter::Tunnelled:
      return !action.block && !action.local;
    case ConnectionVerdictFilter::Bypassed:
      return !action.block && action.local;
    default:
      return true;
  }
}

// Case-insensitive substring over every identity field the row or the
// inspector can print. The query arrives lowercased and trimmed; the search
// field's TextChanged is its only writer.
bool ConnectPage::ConnectionQueryPasses(std::string const& query,
                                        urnw::BlockActionItem const& action) {
  if (query.empty()) return true;
  auto anyMatch = [&query](std::vector<std::string> const& values) {
    for (auto const& value : values) {
      if (ToLower(value).find(query) != std::string::npos) return true;
    }
    return false;
  };
  return anyMatch(action.hosts) || anyMatch(action.ips) ||
         anyMatch(action.matchedHosts) || anyMatch(action.matchedIps);
}

// The activity row, built ONCE per reconcile key. NORMAL: a static row, not
// focusable, not selectable - a Normal user is being told what their VPN is
// doing, not handed 200 tab stops on the way to the Connect button. ADVANCED:
// the same row, selectable - clickable, in the tab order, and invokable with
// Enter or Space because it is a real Button rather than a Border with a
// pointer handler bolted on. A GROUP row (group-by-host) is the same row over
// a host's aggregate; its click is the drill-in, not a selection.
ConnectPage::ConnectionRowEntry ConnectPage::BuildConnectionRow(
    ConnectionViewItem const& item) {
  ConnectionRowEntry entry;
  entry.group = item.group != nullptr;
  entry.id = entry.group ? item.group->host : item.action->id;
  entry.selectable = advancedMode_;
  if (!advancedMode_) {
    auto row = urnw::kit::MakePaneListRow(36);
    entry.root = row.root;
    entry.dot = row.dot;
    entry.title = row.title;
    entry.meta = row.meta;
  } else {
    auto row = urnw::kit::MakePaneListRowButton(36);
    entry.button = row;
    entry.root = row.root;
    entry.dot = row.dot;
    entry.title = row.title;
    entry.meta = row.meta;
    // By KEY, never by index - see SelectConnection. The key is captured by
    // value so the handler does not reach back into a vector that has been
    // rebuilt. A group row's click drills into the host; a decision row's
    // click selects the connection for the inspector.
    const std::string key = entry.id;
    const bool group = entry.group;
    if (group) {
      row.root.Click([weak = w_.get_weak(), key](auto const&, auto const&) {
        if (auto self = weak.get()) self->connect().DrillIntoConnectionGroup(key);
      });
    } else {
      row.root.Click([weak = w_.get_weak(), key](auto const&, auto const&) {
        if (auto self = weak.get()) self->connect().SelectConnection(key);
      });
    }
    // Right-tap (or the keyboard's menu key): the row's rule toggles and
    // copy-details. Advanced Mode only - Normal's static rows get no menu, the
    // same division the click has.
    const Controls::Button anchor = row.root;
    row.root.ContextRequested(
        [weak = w_.get_weak(), key, group, anchor](
            auto const&,
            winrt::Microsoft::UI::Xaml::Input::ContextRequestedEventArgs const& args) {
          args.Handled(true);
          if (auto self = weak.get()) {
            self->connect().ShowConnectionRowMenu(anchor, key, group);
          }
        });
  }
  UpdateConnectionRow(entry, item);
  return entry;
}

// The in-place rewrite: everything a push can change about a row that is
// already on screen - the counters (and with them the meta line), the title,
// the verdict's colour and word - without touching the row's identity, focus
// or the scroller's offset. A group row rewrites from the aggregate: the
// verdict by precedence (blocked if any blocked, else bypassed if any local,
// else tunnelled), the counters summed, the age from the latest decision.
void ConnectPage::UpdateConnectionRow(ConnectionRowEntry& entry,
                                      ConnectionViewItem const& item) {
  const bool group = item.group != nullptr;
  entry.timeMillis = group ? item.group->latestMillis : item.action->timeMillis;
  entry.byteCount = group ? item.group->byteCount : item.action->byteCount;
  entry.packetCount = group ? item.group->packetCount : item.action->packetCount;
  entry.groupConnections = group ? item.group->connections : 0;
  const std::string title = group ? item.group->host : BlockActionTitle(*item.action);
  const bool blocked = group ? item.group->anyBlocked : item.action->block;
  const bool local = group ? !item.group->anyBlocked && item.group->anyLocal
                           : item.action->local;
  const hstring titleText = title.empty() ? Loc("unknown") : H(title);
  const auto verdictColor = blocked   ? urnw::colors::kUrCoral
                            : local   ? urnw::colors::kUrAmber
                                      : urnw::colors::kUrGreen;
  // The verdict in WORDS, for the row's accessible name. The dot is Raw, so
  // the name is the only place the colour's meaning exists for a screen
  // reader - and `local` is a THIRD verdict the old two-way name folded into
  // "allowed": traffic sent around the tunnel is allowed and unprotected, and
  // those are not the same thing to anyone reading this list.
  const hstring verdict = blocked ? Loc("blocked")
                          : local ? Loc("local")
                                  : Loc("allowed");
  const int64_t nowMillis = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count();
  entry.dot.Fill(urnw::colors::MakeBrush(verdictColor));
  entry.title.Text(titleText);
  entry.meta.Text(
      H(group ? GroupConnectionsMeta(entry.groupConnections, entry.timeMillis,
                                     entry.byteCount, entry.packetCount, nowMillis)
              : BlockActionMeta(entry.timeMillis, entry.byteCount,
                                entry.packetCount, nowMillis)));
  // A group row's name carries the fold count too: "host, N connections,
  // verdict" - the aggregate fact the sighted row shows and the bare title
  // would not say.
  std::wstring name{titleText};
  if (group) name += L", " + urnw::Widen(GroupConnectionsWord(entry.groupConnections));
  name += L", " + std::wstring{verdict};
  winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(
      entry.root, hstring{name});
}

// The activity pane's table: every routing decision the device has made, newest
// first. Coral = blocked, green = allowed through the tunnel, amber = sent
// around it (a split rule matched). This is the pane's reason to exist and the
// list that has to FILL it.
//
// INCREMENTAL, because the feed pushes several times a second and a
// Clear()+rebuild on every push reset the scroller to the top, which made the
// list unreadable while it moved: rows are keyed by BlockActionItem::id (or, in
// group-by-host mode, by display host - see ConnectionRowEntry's kind), new
// decisions insert at the top, living rows are rewritten in place, rows past
// the cap or filtered out are removed, and the offset is restored afterwards.
// The verdict filter, the host/IP search and the group fold re-evaluate
// membership through this same pass, so a filter change is a diff, not a
// rebuild.
void ConnectPage::ApplyConnectionsList(bool resetScroll) {
  auto host = w_.ConnectionsHost();
  auto scroll = w_.ConnectionsScroll();

  // The Advanced-Mode flip changes the row TYPE (static Border <-> selectable
  // Button), which an incremental pass cannot morph: it is the ONE path that
  // still clears, and it is a user gesture, never a push. (The group flip only
  // changes the row's KEY - the reconcile replaces the rows below.)
  if (connectionRowsSelectable_ != advancedMode_ && !connectionRowEntries_.empty()) {
    host.Children().Clear();
    connectionRowEntries_.clear();
  }
  connectionRowsSelectable_ = advancedMode_;

  // The visible slice: verdict filter and host/IP substring over the CACHED
  // feed (no Sdk() read, so the filter and the push path cannot disagree),
  // under the cap the full rebuild had. A cap, not a scroll budget: the SDK's
  // action feed is unbounded and every row is a live XAML subtree - 200 rows
  // is ~7000px of pane, well past any window. Group mode folds the SAME
  // filtered feed first and the cap counts groups.
  constexpr size_t kMaxRows = 200;
  int64_t filteredCount = 0;
  std::vector<ConnectionGroup> groups;
  std::vector<ConnectionViewItem> visible;
  if (connectionsGrouped_) {
    groups = FoldConnectionGroups();
    for (auto const& group : groups) filteredCount += group.connections;
    visible.reserve(std::min(groups.size(), kMaxRows));
    for (auto const& group : groups) {
      if (visible.size() >= kMaxRows) break;
      visible.push_back(ConnectionViewItem{nullptr, &group});
    }
  } else {
    visible.reserve(std::min(blockActions_.size(), kMaxRows));
    for (auto const& action : blockActions_) {
      if (!VerdictPassesFilter(verdictFilter_, action) ||
          !ConnectionQueryPasses(connectionsQuery_, action)) {
        continue;
      }
      ++filteredCount;
      if (visible.size() < kMaxRows) {
        visible.push_back(ConnectionViewItem{&action, nullptr});
      }
    }
  }

  // Read the offset BEFORE the mutations; it is restored after them. A filter
  // change is a new result set and reads from the top instead.
  const double offset = resetScroll ? 0.0 : scroll.VerticalOffset();

  // The reconcile key + kind check: the decision id for a flat row, the host
  // for a group row. The kind has to match too - the id namespace and the
  // hostname namespace share the entry's one string, so a group flip must
  // REPLACE a row, never rewrite a decision row into a group that happens to
  // spell the same.
  auto matches = [](ConnectionViewItem const& item, ConnectionRowEntry const& entry) {
    const bool group = item.group != nullptr;
    if (group != entry.group) return false;
    return (group ? item.group->host : item.action->id) == entry.id;
  };

  // Rows that left the visible set - aged out of the feed, trimmed past the
  // cap, or filtered out - walk back to front so the Children() indices stay
  // valid as they come out.
  for (size_t i = connectionRowEntries_.size(); 0 < i--;) {
    bool stays = false;
    for (auto const& item : visible) {
      if (matches(item, connectionRowEntries_[i])) {
        stays = true;
        break;
      }
    }
    if (stays) continue;
    host.Children().RemoveAt(static_cast<uint32_t>(i));
    connectionRowEntries_.erase(connectionRowEntries_.begin() +
                                static_cast<ptrdiff_t>(i));
  }

  // The visible order, top = newest: update in place where the row already
  // stands, reseat it if the feed moved it, insert it if it is new.
  for (size_t i = 0; i < visible.size(); ++i) {
    auto const& item = visible[i];
    size_t at = connectionRowEntries_.size();
    for (size_t k = i; k < connectionRowEntries_.size(); ++k) {
      if (matches(item, connectionRowEntries_[k])) {
        at = k;
        break;
      }
    }
    if (at < connectionRowEntries_.size()) {
      UpdateConnectionRow(connectionRowEntries_[at], item);
      if (at != i) {
        ConnectionRowEntry entry = std::move(connectionRowEntries_[at]);
        connectionRowEntries_.erase(connectionRowEntries_.begin() +
                                    static_cast<ptrdiff_t>(at));
        host.Children().RemoveAt(static_cast<uint32_t>(at));
        host.Children().InsertAt(static_cast<uint32_t>(i), entry.root);
        connectionRowEntries_.insert(connectionRowEntries_.begin() +
                                         static_cast<ptrdiff_t>(i),
                                     std::move(entry));
      }
    } else {
      ConnectionRowEntry entry = BuildConnectionRow(item);
      host.Children().InsertAt(static_cast<uint32_t>(i), entry.root);
      connectionRowEntries_.insert(connectionRowEntries_.begin() +
                                       static_cast<ptrdiff_t>(i),
                                   std::move(entry));
    }
  }

  // The selectable rows, re-collected so the selection path keeps repainting
  // instead of rebuilding (see ApplyConnectionSelectionVisuals).
  connectionRows_.clear();
  connectionRowIds_.clear();
  for (auto const& entry : connectionRowEntries_) {
    if (!entry.selectable) continue;
    connectionRows_.push_back(entry.button);
    connectionRowIds_.push_back(entry.id);
  }

  // Restore what a rebuild would have lost. ChangeView applies against the new
  // extent once layout settles; the animation is disabled because this is a
  // correction, not a transition.
  if (resetScroll) {
    scroll.ChangeView(nullptr, winrt::Windows::Foundation::IReference<double>{0.0}, nullptr,
                      true);
  } else if (0 < offset) {
    scroll.ChangeView(nullptr, winrt::Windows::Foundation::IReference<double>{offset},
                      nullptr, true);
  }

  // The group-header count: "N hosts" with no filter, "N hosts of M" while a
  // filter is holding rows back - of_total is the shipped "of {}" key, so the
  // pair stays plural-correct in every language the store covers. Group mode
  // always reads "N hosts of M": the fold over the filtered feed it folded -
  // "8 hosts of 30".
  const bool filterActive =
      verdictFilter_ != ConnectionVerdictFilter::All || !connectionsQuery_.empty();
  std::wstring count =
      urnw::Plural("host_count",
                   connectionsGrouped_ ? static_cast<int64_t>(groups.size())
                   : filterActive      ? filteredCount
                                       : static_cast<int64_t>(blockActions_.size()));
  if (connectionsGrouped_ || filterActive) {
    count += L" ";
    count += urnw::Format("of_total", connectionsGrouped_
                                          ? filteredCount
                                          : static_cast<int64_t>(blockActions_.size()));
  }
  w_.ConnectionsCount().Text(hstring{count});
  // The clear-filters affordance rides on ANY of the three controls being off
  // its default. The group fold counts here though the header count above
  // deliberately does not treat it as a filter: it changes what the list
  // shows, which is exactly what the one-click reset is for. Every path that
  // can change any of the three lands in this pass, so this is the one place
  // the button's visibility is written.
  w_.ConnectionsClearFilters().Visibility(
      filterActive || connectionsGrouped_ ? Visibility::Visible
                                          : Visibility::Collapsed);
  // The verdict ratio bar under the header, rebuilt on the same pass that
  // rebuilds the count (and from the block-stats handler for its session
  // inputs): three star weights, never a rebuild.
  ApplyVerdictRatioBar();

  ApplySessionCardsVisibility(statsConnected_);
  // A filter that matches nothing in a session that HAS rows must not read as
  // an empty session: the blank list under the active filter controls says
  // exactly what happened, which the session-empty line would contradict.
  if (filterActive && filteredCount == 0 && !blockActions_.empty() && statsConnected_) {
    w_.ConnectionsScroll().Visibility(Visibility::Visible);
    w_.SessionEmptyCard().Visibility(Visibility::Collapsed);
  }
  // The selection survived the reconcile by id. If what was selected is no
  // longer in the feed, the inspector must say so rather than keep printing a
  // connection that has aged out.
  ApplyConnectionSelectionVisuals();
  ApplyInspector();
}

// Group-by-host's fold: the FILTERED feed (the same verdict + query membership
// the flat list renders) collapsed by display host - BlockActionTitle's rule,
// so a group is named exactly the way its members' rows would be. The feed is
// newest-first, so the first sight of a host is its latest decision; the
// explicit sort afterwards states the row order (latest first) rather than
// trusting that property through ties and zeroed times.
std::vector<ConnectPage::ConnectionGroup> ConnectPage::FoldConnectionGroups() const {
  std::vector<ConnectionGroup> groups;
  for (auto const& action : blockActions_) {
    if (!VerdictPassesFilter(verdictFilter_, action) ||
        !ConnectionQueryPasses(connectionsQuery_, action)) {
      continue;
    }
    const std::string host = BlockActionTitle(action);
    ConnectionGroup* group = nullptr;
    for (auto& candidate : groups) {
      if (candidate.host == host) {
        group = &candidate;
        break;
      }
    }
    if (!group) {
      groups.push_back(ConnectionGroup{});
      group = &groups.back();
      group->host = host;
      group->latest = &action;  // newest-first feed: first seen is the latest
    }
    ++group->connections;
    group->byteCount += action.byteCount;
    group->packetCount += action.packetCount;
    group->latestMillis = std::max(group->latestMillis, action.timeMillis);
    group->anyBlocked = group->anyBlocked || action.block;
    group->anyLocal = group->anyLocal || action.local;
  }
  std::stable_sort(groups.begin(), groups.end(),
                   [](ConnectionGroup const& a, ConnectionGroup const& b) {
                     return a.latestMillis > b.latestMillis;
                   });
  return groups;
}

// NetworkPage::Build parity: the search field and its filter live in one
// place, built once into the markup's host. The locations search is owned by
// the SDK; the connections feed's reading is NOT - this filter is view-side
// over the cached feed, so TextChanged just re-runs the incremental pass.
void ConnectPage::BuildConnectionsFilter() {
  if (connectionsFilterBuilt_) return;
  connectionsFilterBuilt_ = true;
  auto row =
      urnw::kit::MakePaneSearchRow(Adv("adv_search_connections", L"Search hosts or IPs"));
  connectionsSearch_ = row.box;
  connectionsSearch_.TextChanged([weak = w_.get_weak()](IInspectable const&, auto const&) {
    if (auto self = weak.get()) {
      auto& page = self->connect();
      page.connectionsQuery_ =
          ToLower(TrimWhitespace(Narrow(page.connectionsSearch_.Text())));
      page.ApplyConnectionsList(true);
    }
  });
  w_.ConnectionsSearchHost().Children().Append(row.root);
}

// The verdict filter, read off the bar the way SelectedMode reads the
// connection mode (same non-const accessor note). A changed filter is a new
// result set: re-evaluate membership through the ordinary incremental pass
// and read it from the top.
void ConnectPage::OnConnectionsVerdictChanged() {
  if (updatingControls_) return;
  auto selected = w_.ConnectionsVerdictBar().SelectedItem();
  ConnectionVerdictFilter filter = ConnectionVerdictFilter::All;
  if (selected == w_.VerdictBlockedItem()) {
    filter = ConnectionVerdictFilter::Blocked;
  } else if (selected == w_.VerdictTunnelledItem()) {
    filter = ConnectionVerdictFilter::Tunnelled;
  } else if (selected == w_.VerdictBypassedItem()) {
    filter = ConnectionVerdictFilter::Bypassed;
  }
  if (filter == verdictFilter_) return;
  verdictFilter_ = filter;
  ApplyConnectionsList(true);
}

// The group-by-host switch, read off the control the way the verdict bar is. A
// changed fold is a new result set, like a changed verdict: re-evaluate
// through the ordinary incremental pass and read it from the top.
void ConnectPage::OnConnectionsGroupToggled() {
  if (updatingControls_) return;
  const bool grouped = w_.ConnectionsGroupToggle().IsOn();
  if (grouped == connectionsGrouped_) return;
  connectionsGrouped_ = grouped;
  ApplyConnectionsList(true);
}

// The one-click reset. The state fields move first, the controls follow behind
// updatingControls_ so the programmatic writes cannot echo back through their
// own handlers, and the pass runs ONCE at the end: clearing the search text
// fires its TextChanged (the same path the user's own typing takes), and when
// the box was already empty no TextChanged is coming, so the pass runs here
// instead - the DrillIntoConnectionGroup pattern.
void ConnectPage::OnConnectionsClearFilters() {
  verdictFilter_ = ConnectionVerdictFilter::All;
  connectionsGrouped_ = false;
  updatingControls_ = true;
  w_.ConnectionsVerdictBar().SelectedItem(w_.VerdictAllItem());
  w_.ConnectionsGroupToggle().IsOn(false);
  updatingControls_ = false;
  if (connectionsSearch_ && !connectionsSearch_.Text().empty()) {
    connectionsSearch_.Text(L"");
  } else {
    connectionsQuery_.clear();
    ApplyConnectionsList(true);
  }
}

// The verdict ratio bar under the connections group header: the session's
// allowed (green) / blocked (coral) split, plus the bypassed-local third
// (amber). The BlockStats pair is SESSION-scoped; the local count is read off
// the cached blockActions_ window, so the amber share is WINDOW-scoped (the
// SDK has no session-scoped bypass counter) - the two scopes sit on one bar
// because the window is the only place a bypass reading exists at all.
//
// The work per call is deliberately trivial: one pass over the cached window
// for the local count, then three star-weight writes on the columns that
// already exist. No element is built here - the strip updates only on feed
// pushes (the block-actions pass and the block-stats handler), so a live
// session pays nothing per frame.
void ConnectPage::ApplyVerdictRatioBar() {
  auto bar = w_.ConnectionsVerdictRatio();
  int64_t localCount = 0;
  for (auto const& action : blockActions_) {
    if (!action.block && action.local) ++localCount;
  }
  const int64_t total = allowedCount_ + blockedCount_ + localCount;
  if (total <= 0) {
    // Nothing to proportion: the strip collapses rather than drawing an
    // empty track, which would read as chrome instead of data.
    bar.Visibility(Visibility::Collapsed);
    return;
  }
  bar.Visibility(Visibility::Visible);
  const double weights[3] = {static_cast<double>(allowedCount_),
                             static_cast<double>(blockedCount_),
                             static_cast<double>(localCount)};
  for (uint32_t i = 0; i < 3; ++i) {
    bar.ColumnDefinitions().GetAt(i).Width(GridLength{weights[i], GridUnitType::Star});
  }
}

// The drill-in. The search box takes the host (its TextChanged folds the text
// into connectionsQuery_ and re-runs the pass - the same path the user's own
// typing takes), and the switch goes off behind updatingControls_ so the
// programmatic flip does not echo back through its handler. When the box
// already held exactly this host no TextChanged is coming, so the pass runs
// here instead: the fold flip alone still re-renders.
void ConnectPage::DrillIntoConnectionGroup(std::string const& host) {
  if (host.empty()) return;  // an unnamed group has nothing to search for
  if (connectionsGrouped_) {
    connectionsGrouped_ = false;
    updatingControls_ = true;
    w_.ConnectionsGroupToggle().IsOn(false);
    updatingControls_ = false;
  }
  if (connectionsSearch_) {
    if (Narrow(connectionsSearch_.Text()) == host) {
      ApplyConnectionsList(true);
    } else {
      connectionsSearch_.Text(H(host));
    }
  } else {
    connectionsQuery_ = ToLower(TrimWhitespace(host));
    ApplyConnectionsList(true);
  }
}

// The 1s reading of the meta line's age prefix. Every row keeps the counters
// its meta was built from, so this is one string re-render per row with no
// feed read and no rebuild - and a row whose age text has not changed is not
// touched, so a quiet minute costs no layout.
void ConnectPage::RefreshConnectionRowTimes() {
  if (connectionRowEntries_.empty()) return;
  const int64_t nowMillis = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count();
  for (auto& entry : connectionRowEntries_) {
    if (entry.timeMillis <= 0) continue;
    // a group row re-renders its own meta shape (fold count + the trio)
    const hstring meta =
        H(entry.group ? GroupConnectionsMeta(entry.groupConnections, entry.timeMillis,
                                             entry.byteCount, entry.packetCount, nowMillis)
                      : BlockActionMeta(entry.timeMillis, entry.byteCount,
                                        entry.packetCount, nowMillis));
    if (entry.meta.Text() != meta) entry.meta.Text(meta);
  }
}

// The pane-B body floor: the Remote chart at its 120 floor + the transport bar
// + the ip-family row + the extender panel + the 28px group header + the 3px
// ratio bar + a usable sliver of the list. Below it the fixed blocks would
// leave the star-sized list under ~3 rows, so the body stops shrinking and the
// pane's own scroller takes over (the markup comment on ActivityBodyScroll
// states the rule). The floor's list guarantee survived the chart flex because
// the chart yields its flex FIRST: at the floor the chart sits at 120, not its
// old fixed 150, so the list's sliver here is 27px LARGER than the fixed-150
// layout's was (the chart's 30 less the ratio bar's 3).
constexpr double kActivityBodyMinHeight = 520;
// The Remote chart's flex bounds. The floor is what the chart may shrink to
// when height is scarce (pane-model rule: fixed chrome never starves the
// pane's list - the chart is the one fixed block here that can give); the cap
// keeps a tall window's chart a chart rather than a second list.
constexpr double kRemoteChartMinHeight = 120;
constexpr double kRemoteChartMaxHeight = 240;

void ConnectPage::ApplyActivityBodyHeight() {
  // Pin the body at the viewport while the window is tall enough - the body IS
  // the viewport there, so the layout is pixel-identical to the fixed rows it
  // replaced - and at the floor below it, which is the one thing a star row
  // cannot express: "shrink with the pane, but no further than this".
  const double viewport = w_.ActivityBodyScroll().ViewportHeight();
  w_.ActivityBody().Height(std::max(viewport, kActivityBodyMinHeight));
  // The Remote chart's share of that math, derived from the SAME viewport so
  // the chart and the body can never disagree: today's 150 at the floor (the
  // default layout is unchanged), a THIRD of each viewport pixel past it - the
  // list keeps the other two thirds, because the list is the pane's reason to
  // exist - floored at 120 when height is scarce and capped at 240. The chart
  // re-renders and re-stamps its clip on the SizeChanged this causes, the same
  // path any resize already took.
  const double chartFlex = 150.0 + (viewport - kActivityBodyMinHeight) / 3.0;
  w_.RemoteChartHost().Height(
      std::clamp(chartFlex, kRemoteChartMinHeight, kRemoteChartMaxHeight));
}

// The pane header's fixed height (UrPaneHeaderHeight in App.xaml; keep in
// step). The pane's rows are Auto-header + star-scroller, so the scroller's
// viewport is exactly the pane's height minus this.
constexpr double kPaneHeaderHeight = 40;

void ConnectPage::ApplyPaneCBodyHeight() {
  // Pin the body grid's MinHeight to the viewport. MINHeight, not Height:
  // content taller than the pane overrides the pin and scrolls exactly as the
  // StackPanel did, while content shorter than the pane gets a bounded grid
  // whose star chart rows can share the leftover (132 floor, 220 cap - the
  // markup comment on PaneCBody has the rule). Without the pin the scroller's
  // unbounded measure would leave the star rows at their floor forever.
  const double viewport = w_.ConnectPaneC().ActualHeight() - kPaneHeaderHeight;
  if (viewport <= 0) return;  // not laid out yet; the first SizeChanged re-runs
  w_.PaneCBody().MinHeight(viewport);
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

// The selection in the CURRENT feed, or nullptr. The action may have aged out
// of the SDK's window since it was picked, and if it has, saying so is the
// honest reading - the alternative is a detail pane frozen on a connection
// that no longer exists, which is indistinguishable from a hung inspector.
const urnw::BlockActionItem* ConnectPage::SelectedConnectionAction() const {
  if (selectedConnectionId_.empty()) return nullptr;
  for (auto const& candidate : blockActions_) {
    if (candidate.id == selectedConnectionId_) return &candidate;
  }
  return nullptr;
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

  const urnw::BlockActionItem* action = SelectedConnectionAction();

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
    // No selection, no actions: the row goes WITH the empty reading, and a
    // disabled row left behind would offer rules on a connection that is gone.
    w_.InspectorActionsRow().Visibility(Visibility::Collapsed);
    blockQuickAction_ = {};
    routeQuickAction_ = {};
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

  // ---- the quick actions (observe -> decide -> rule) ----------------------
  // Derived on EVERY render, from the live overrides list: the click handlers
  // consume the stored state, so the state and the buttons can never disagree.
  blockQuickAction_ = QuickActionFor(*action, true);
  routeQuickAction_ = QuickActionFor(*action, false);
  w_.InspectorActionsRow().Visibility(Visibility::Visible);
  // The label names what the click leaves behind: "Allow this host" over a
  // blocking rule (or a blocked verdict when no rule is in force), "Block
  // this host" over an allowing one - and the bypass/tunnel pair the same
  // way. The ACTIVE rule's polarity answers, never the action's verdict: the
  // verdict is a stale snapshot once a rule has landed after the decision,
  // and a label read from it would name the click backwards exactly while
  // the undo snackbar is up. The fill says create vs remove
  // (ApplyQuickActionButton).
  ApplyQuickActionButton(
      w_.InspectorBlockButton(), blockQuickAction_,
      (blockQuickAction_.active ? blockQuickAction_.polarity : action->block)
          ? Adv("adv_allow_host", L"Allow this host")
          : Adv("adv_block_host", L"Block this host"));
  ApplyQuickActionButton(
      w_.InspectorRouteButton(), routeQuickAction_,
      (routeQuickAction_.active ? routeQuickAction_.polarity : action->local)
          ? Adv("adv_always_tunnel", L"Always tunnel")
          : Adv("adv_bypass_tunnel", L"Bypass the tunnel"));
  w_.InspectorCopyButton().Content(
      winrt::box_value(Adv("adv_copy_details", L"Copy details")));

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
    const winrt::hstring reason =
        action->hasBlockOverride  ? Adv("adv_reason_block", L"Block override")
        : action->hasRouteOverride ? Adv("adv_reason_route", L"Route override")
                                   : Adv("adv_reason_override", L"Override");
    // The reason names a rule, and the rule lives on the split-rules surface -
    // so "why did this happen" is one click away: the value is a button that
    // opens that surface (the trailing caret says so, the way
    // ProviderCountLine's does) rather than a fact the user hunts down.
    auto reasonRow = urnw::kit::MakePaneKeyValueRow(Adv("adv_reason", L"Reason"), reason);
    auto reasonGrid = reasonRow.root.Child().as<Controls::Grid>();
    uint32_t reasonValueIndex = 0;
    if (reasonGrid.Children().IndexOf(reasonRow.value, reasonValueIndex)) {
      reasonGrid.Children().RemoveAt(reasonValueIndex);
      Controls::Button link;
      link.Padding(ThicknessHelper::FromLengths(0, 0, 0, 0));
      link.Background(urnw::colors::MakeBrush(winrt::Windows::UI::Color{0, 0, 0, 0}));
      link.BorderThickness(ThicknessHelper::FromLengths(0, 0, 0, 0));
      link.HorizontalContentAlignment(HorizontalAlignment::Right);
      Controls::StackPanel linkContent;
      linkContent.Orientation(Controls::Orientation::Horizontal);
      linkContent.Spacing(4);
      Controls::TextBlock linkText;
      linkText.Text(reason);
      linkText.FontSize(13);
      linkText.Foreground(urnw::colors::TextBrush());
      linkText.VerticalAlignment(VerticalAlignment::Center);
      linkContent.Children().Append(linkText);
      Controls::FontIcon caret;
      caret.Glyph(L"\uE76C");
      caret.FontSize(12);
      caret.Foreground(urnw::colors::FaintBrush());
      caret.VerticalAlignment(VerticalAlignment::Center);
      linkContent.Children().Append(caret);
      link.Content(linkContent);
      // A Button whose Content is a Panel gets NO automatic name (the kit's
      // PaneListRowButton note): name it with the fact and where it goes, or
      // a screen reader hears "button" and nothing else.
      winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(
          link, winrt::hstring{std::wstring{Adv("adv_reason", L"Reason")} + L", " +
                               std::wstring{reason} + L", " +
                               AdvW("adv_open_split_rules", L"open split rules")});
      link.Click([weak = w_.get_weak()](auto const&, auto const&) {
        if (auto self = weak.get()) self->connect().ShowSplitRulesSheet();
      });
      Controls::Grid::SetColumn(link, 1);
      reasonGrid.Children().Append(link);
    }
    host.Children().Append(reasonRow.root);
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

// The quick action's whole state, derived from the LIVE overrides list. The
// action itself cannot answer "is a rule in force NOW": it snapshots the
// decision as made and does not change when a rule is added afterwards, which
// is exactly when the user reaches for these buttons.
ConnectPage::InspectorQuickAction ConnectPage::QuickActionFor(
    urnw::BlockActionItem const& action, bool blockKind) const {
  InspectorQuickAction out;
  // The host values a click would rule on: the matched names first, then the
  // bare hosts, then the addresses - the same values, in the same order, the
  // split-rule editor offers for this same action (OpenEditorForAction).
  out.hosts = action.matchedHosts;
  out.hosts.insert(out.hosts.end(), action.hosts.begin(), action.hosts.end());
  out.hosts.insert(out.hosts.end(), action.matchedIps.begin(), action.matchedIps.end());
  out.hosts.insert(out.hosts.end(), action.ips.begin(), action.ips.end());
  out.enabled = !out.hosts.empty();

  for (auto const& rule : Sdk().CurrentHostRules()) {
    if (blockKind ? !rule.hasBlockOverride : !rule.hasRouteOverride) continue;
    const bool covers = std::any_of(rule.hosts.begin(), rule.hosts.end(),
                                    [&out](std::string const& host) {
                                      return std::find(out.hosts.begin(), out.hosts.end(),
                                                       host) != out.hosts.end();
                                    });
    // The flattened action carries ONE override id and the block decision wins
    // the slot, so the id is safe to remove for the route kind only when no
    // block override shared the decision (device_local
    // blockActionFromConnectWithLock). The id match also catches the SDK's
    // suffix matching: an override for the parent names this connection
    // without spelling any of its exact hosts.
    const bool idNamesKind = blockKind ? action.hasBlockOverride
                                       : (action.hasRouteOverride && !action.hasBlockOverride);
    const bool decided = idNamesKind && rule.overrideId == action.overrideId;
    if (!covers && !decided) continue;
    out.active = true;
    out.overrideId = rule.overrideId;
    out.polarity = blockKind ? rule.block : rule.routeLocal;
    out.enabled = true;
    return out;
  }
  return out;
}

// Filled action-blue when a rule is in force (the click removes it), the
// style's own outlined rest when the click creates one - the toggle on-state
// the switches already paint, on the button whose label names the outcome.
// The label changes word with the verdict, so the fill is a second channel,
// never the only one.
void ConnectPage::ApplyQuickActionButton(Button const& button,
                                         InspectorQuickAction const& state,
                                         winrt::hstring const& label) {
  button.Content(winrt::box_value(label));
  button.IsEnabled(state.enabled);
  if (state.active) {
    button.Background(urnw::colors::MakeBrush(urnw::colors::kToggleAccent));
    button.Foreground(urnw::colors::MakeBrush(urnw::colors::kInverseText));
    button.BorderBrush(urnw::colors::MakeBrush(urnw::colors::kToggleAccent));
  } else {
    // CLEAR the on-state's local values so the style's rest shows through -
    // re-applying "transparent" by hand would be a second definition of the
    // style's own colors.
    button.ClearValue(Controls::Control::BackgroundProperty());
    button.ClearValue(Controls::Control::ForegroundProperty());
    button.ClearValue(Controls::Control::BorderBrushProperty());
  }
  // The on/off state is sighted-only otherwise; the name carries it, the same
  // treatment ApplyConnectionSelectionVisuals gives the selected row.
  winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(
      button,
      state.active
          ? winrt::hstring{std::wstring{label} + L", " +
                           AdvW("adv_rule_active", L"rule active, click to remove")}
          : label);
}

void ConnectPage::OnInspectorBlockToggle() {
  RunBlockQuickAction(blockQuickAction_, SelectedConnectionAction());
}

void ConnectPage::RunBlockQuickAction(InspectorQuickAction const& state,
                                      urnw::BlockActionItem const* action) {
  if (!state.enabled) return;
  if (state.active) {
    // the toggle's off half: remove the rule in force, by id
    Sdk().RemoveBlockRule(state.overrideId);
    ShowInspectorRuleSnackbar(Adv("adv_rule_removed", L"Rule removed"), {});
  } else {
    if (!action) return;
    // the inverse of the verdict: a tunnelled host gets a blocking rule; a
    // host the default policy blocked gets an allowing one (the countermand)
    const bool block = !action->block;
    const std::string overrideId = Sdk().CreateBlockRule(state.hosts, block);
    if (overrideId.empty()) return;
    ShowInspectorRuleSnackbar(
        block ? Adv("adv_host_blocked", L"This host will be blocked")
              : Adv("adv_host_allowed", L"This host will be allowed"),
        overrideId);
  }
  // Re-derive now: CreateBlockRule/RemoveBlockRule republish the overrides
  // synchronously (the handler push lands after this click returns), so the
  // buttons flip with the click rather than a beat later.
  ApplyInspector();
}

void ConnectPage::OnInspectorRouteToggle() {
  RunRouteQuickAction(routeQuickAction_, SelectedConnectionAction());
}

void ConnectPage::RunRouteQuickAction(InspectorQuickAction const& state,
                                      urnw::BlockActionItem const* action) {
  if (!state.enabled) return;
  if (state.active) {
    Sdk().RemoveSplitRule(state.overrideId);
    ShowInspectorRuleSnackbar(Adv("adv_rule_removed", L"Rule removed"), {});
  } else {
    if (!action) return;
    // the inverse of the verdict, as with the block pair: bypassed gets a
    // tunnel rule (Local=false), tunnelled gets a bypass rule (Local=true)
    const std::string overrideId = action->local
                                       ? Sdk().CreateTunnelRule(state.hosts)
                                       : Sdk().CreateSplitRule(state.hosts);
    if (overrideId.empty()) return;
    ShowInspectorRuleSnackbar(
        action->local ? Adv("adv_host_tunnelled", L"This host will use the tunnel")
                      : Adv("adv_host_bypassed", L"This host will bypass the tunnel"),
        overrideId);
  }
  ApplyInspector();
}

void ConnectPage::OnInspectorCopyDetails() {
  const urnw::BlockActionItem* action = SelectedConnectionAction();
  if (!action) return;
  CopyConnectionDetails(*action);
}

void ConnectPage::CopyConnectionDetails(urnw::BlockActionItem const& action) {
  auto join = [](std::vector<std::string> const& parts) {
    std::string out;
    for (auto const& part : parts) {
      if (!out.empty()) out += ", ";
      out += part;
    }
    return out;
  };
  // The inspector's own fields, in the inspector's own words and order: host,
  // addresses, verdict, reason, totals, last decision. A copy that invents a
  // second phrasing of the same facts is a second place to be wrong.
  std::wstring text;
  auto line = [&text](winrt::hstring const& key, winrt::hstring const& value) {
    if (!text.empty()) text += L"\r\n";
    text += std::wstring{key} + L": " + std::wstring{value};
  };
  const std::string hostsJoined = join(action.hosts);
  const std::string ipsJoined = join(action.ips);
  line(Adv("adv_host", L"Host"),
       hostsJoined.empty() ? Adv("adv_none", L"none") : H(hostsJoined));
  line(Adv("adv_addresses", L"Addresses"),
       ipsJoined.empty() ? Adv("adv_none", L"none") : H(ipsJoined));
  line(Adv("adv_verdict", L"Verdict"),
       action.block ? Adv("adv_verdict_blocked", L"Blocked — no packets sent")
       : action.local
           ? Adv("adv_verdict_local", L"Bypassed the tunnel — not protected")
           : Adv("adv_verdict_tunnelled", L"Tunnelled through URnetwork"));
  if (action.overrideId.empty()) {
    line(Adv("adv_reason", L"Reason"), Adv("adv_reason_default", L"Default policy"));
  } else {
    line(Adv("adv_reason", L"Reason"),
         action.hasBlockOverride  ? Adv("adv_reason_block", L"Block override")
         : action.hasRouteOverride ? Adv("adv_reason_route", L"Route override")
                                    : Adv("adv_reason_override", L"Override"));
    line(Adv("adv_override_id", L"Override"), H(action.overrideId));
  }
  line(Adv("adv_packets_total", L"Packets (total)"),
       H(urnw::FormatCountCompact(action.packetCount)));
  line(Adv("adv_bytes_total", L"Bytes (total)"),
       H(urnw::FormatByteCountCompact(action.byteCount)));
  if (0 < action.timeMillis) {
    const int64_t nowMillis = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::system_clock::now().time_since_epoch())
                                  .count();
    line(Adv("adv_last_decision", L"Last decision"),
         H(urnw::RelativeTime(action.timeMillis, nowMillis)));
  }
  winrt::Windows::ApplicationModel::DataTransfer::DataPackage package;
  package.SetText(winrt::hstring{text});
  winrt::Windows::ApplicationModel::DataTransfer::Clipboard::SetContent(package);
  ShowInspectorRuleSnackbar(Adv("adv_details_copied", L"Connection details copied"), {});
}

// The row menu is built at OPEN, never cached: the quick-action state reads
// the live overrides list, and a menu that remembered a rule state from when
// its row was built would offer to remove rules that no longer exist. The
// state and the target are then captured INTO the items, so a click does
// exactly what the open menu showed - the same stored-state rule the
// inspector's buttons follow.
void ConnectPage::ShowConnectionRowMenu(
    winrt::Microsoft::UI::Xaml::FrameworkElement const& anchor,
    std::string const& key, bool group) {
  // Resolve the target NOW, and own it: the feed is live, and neither the menu
  // nor its clicks may point into vectors the next push replaces. A group
  // row's target is a synthesized action over the group's HOST (the menu rules
  // on the host, the way the row aggregates it), folded back out of the same
  // filtered feed the list rendered so menu and row cannot disagree.
  urnw::BlockActionItem target;
  if (!group) {
    const urnw::BlockActionItem* action = nullptr;
    for (auto const& candidate : blockActions_) {
      if (candidate.id == key) {
        action = &candidate;
        break;
      }
    }
    if (!action) return;  // aged out of the feed: nothing honest to offer
    target = *action;
  } else {
    bool found = false;
    for (auto const& fold : FoldConnectionGroups()) {
      if (fold.host != key) continue;
      found = true;
      target.id = fold.host;
      target.hosts = {fold.host};
      target.block = fold.anyBlocked;
      target.local = !fold.anyBlocked && fold.anyLocal;
      target.timeMillis = fold.latestMillis;
      target.byteCount = fold.byteCount;
      target.packetCount = fold.packetCount;
      if (fold.latest) {
        target.overrideId = fold.latest->overrideId;
        target.hasBlockOverride = fold.latest->hasBlockOverride;
        target.hasRouteOverride = fold.latest->hasRouteOverride;
      }
      break;
    }
    if (!found) return;
  }

  const InspectorQuickAction blockState = QuickActionFor(target, true);
  const InspectorQuickAction routeState = QuickActionFor(target, false);

  MenuFlyout flyout;
  MenuFlyoutItem blockItem;
  // The label names what the click leaves behind - the active rule's polarity,
  // else the verdict - the exact reading the inspector's buttons print
  // (ApplyInspector), so the menu and the buttons never name the same click
  // two ways.
  blockItem.Text((blockState.active ? blockState.polarity : target.block)
                     ? Adv("adv_allow_host", L"Allow this host")
                     : Adv("adv_block_host", L"Block this host"));
  blockItem.IsEnabled(blockState.enabled);
  blockItem.Click([weak = w_.get_weak(), blockState, target](auto const&, auto const&) {
    if (auto self = weak.get()) {
      self->connect().RunBlockQuickAction(blockState, &target);
    }
  });
  flyout.Items().Append(blockItem);
  MenuFlyoutItem routeItem;
  routeItem.Text((routeState.active ? routeState.polarity : target.local)
                     ? Adv("adv_always_tunnel", L"Always tunnel")
                     : Adv("adv_bypass_tunnel", L"Bypass the tunnel"));
  routeItem.IsEnabled(routeState.enabled);
  routeItem.Click([weak = w_.get_weak(), routeState, target](auto const&, auto const&) {
    if (auto self = weak.get()) {
      self->connect().RunRouteQuickAction(routeState, &target);
    }
  });
  flyout.Items().Append(routeItem);
  flyout.Items().Append(MenuFlyoutSeparator());
  MenuFlyoutItem copyItem;
  copyItem.Text(Adv("adv_copy_details", L"Copy details"));
  copyItem.Click([weak = w_.get_weak(), target](auto const&, auto const&) {
    if (auto self = weak.get()) self->connect().CopyConnectionDetails(target);
  });
  flyout.Items().Append(copyItem);
  flyout.ShowAt(anchor);
}

void ConnectPage::OnInspectorUndo() {
  if (inspectorUndoOverrideId_.empty()) return;
  const std::string overrideId = inspectorUndoOverrideId_;
  inspectorUndoOverrideId_.clear();
  if (inspectorSnackbar_) inspectorSnackbar_->Hide();
  // Kind-agnostic by construction: split, tunnel and block rules share the one
  // overrides store, and removal from it is by id (SdkHost::RemoveBlockRule).
  Sdk().RemoveBlockRule(overrideId);
  ApplyInspector();
}

void ConnectPage::ShowInspectorRuleSnackbar(winrt::hstring const& message,
                                            std::string undoOverrideId) {
  inspectorUndoOverrideId_ = std::move(undoOverrideId);
  if (!inspectorSnackbar_) return;
  // The action slot exists only while there is something to undo: a creation
  // arms Undo (which deletes the just-created override by id), a removal is
  // the plain acknowledgement. One bar, so one message at a time - a second
  // Show restarts the auto-dismiss window (kit::Snackbar), and Success is one
  // of the severities that dismiss themselves.
  if (inspectorUndoOverrideId_.empty()) {
    w_.InspectorSnackbar().ActionButton(nullptr);
  } else {
    w_.InspectorSnackbar().ActionButton(inspectorUndoButton_);
  }
  inspectorSnackbar_->Show(message, InfoBarSeverity::Success);
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
  if (settings) dnsSettled_ = true;
  // DESIGNSTYLE "Placeholders, not pop-in": before the first reading the four
  // rows are up with their labels (the labels are static) and a skeleton where
  // the On/Off value goes, so the group opens at its settled 4x34 and the
  // values are replaced in place. Only a reading that comes back empty swaps
  // to the unavailable row — the error state, in the same group.
  const bool loading = !settings && !dnsSettled_;
  w_.DnsRowsPanel().Visibility(settings || loading ? Visibility::Visible : Visibility::Collapsed);
  // the ROW, not the text inside it: see ProvideStatsRow
  w_.DnsUnavailableRow().Visibility(settings || loading ? Visibility::Collapsed
                                                        : Visibility::Visible);
  // the applied settings just changed: re-evaluate the recommendation pill (it
  // reads dnsSettings_, already updated to `settings` by the caller). Runs in
  // the unavailable path too so the pill collapses with the rows.
  ApplyDnsRecommendationPill();
  {
    // the value's skeleton (shimmer begun once, on the first loading pass)
    const hstring loadingText = Loc("loading");
    auto skeleton = [loading, &loadingText](Microsoft::UI::Xaml::Shapes::Ellipse const& dot,
                                            TextBlock const& label, TextBlock const& state,
                                            Border const& bar) {
      bar.Visibility(loading ? Visibility::Visible : Visibility::Collapsed);
      if (!loading) return;
      dot.Fill(urnw::colors::MakeBrush(urnw::colors::WithAlpha(urnw::colors::kTextFaint, 102)));
      state.Text(L"");
      winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(
          state, hstring{std::wstring{label.Text()} + L", " + std::wstring{loadingText}});
      if (!winrt::unbox_value_or<bool>(bar.Tag(), false)) {
        bar.Tag(winrt::box_value(true));
        kit::StartSkeletonShimmer(bar);
      }
    };
    skeleton(w_.DohDot(), w_.DohLabel(), w_.DohState(), w_.DohSkeleton());
    skeleton(w_.UdnsDot(), w_.UdnsLabel(), w_.UdnsState(), w_.UdnsSkeleton());
    skeleton(w_.LdnsDot(), w_.LdnsLabel(), w_.LdnsState(), w_.LdnsSkeleton());
    skeleton(w_.FallbackDot(), w_.FallbackLabel(), w_.FallbackState(), w_.FallbackSkeleton());
  }
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
  // #27: a pending degrade hold expires on the CLOCK, not on an SDK event —
  // grid pushes stop arriving exactly when the window is stuck, so waiting for
  // one would hold "Connected" over a dead window forever. BEFORE the
  // per-pane gate below on purpose: the status strip renders health on every
  // destination, not only Home. One nudge per deadline (the push re-arms it if
  // the hold is still running), through the ordinary stats path so the window
  // AND the tray both hear the answer.
  if (healthReevalAtMillis_ > 0) {
    const int64_t nowMillis =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();
    if (nowMillis >= healthReevalAtMillis_) {
      healthReevalAtMillis_ = 0;
      Sdk().RepublishStats();
    }
  }
  // the loading skeletons' ceiling is clock-driven for the same reason
  if (placeholdersSinceMillis_ != 0 &&
      SteadyMillis() - placeholdersSinceMillis_ > kPlaceholderCeilingMillis) {
    SettlePlaceholders();
  }
  if (w_.ConnectView().Visibility() != Visibility::Visible && !w_.sheetOpen()) return;
  if (w_.ConnectView().Visibility() == Visibility::Visible) {
    remoteChart_->Tick();
    blockedChart_->Tick();
    localChart_->Tick();
    // the transport bar's boundary tween / empty fade; returns immediately
    // unless one is in flight (TransferChart's rule)
    if (transportBar_) transportBar_->Tick();
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
  // globe recenter animation + the 1s connected-duration retick (the sheet
  // owns its own 1s divider off this 100ms clock)
  if (providerLocationsSheet_) providerLocationsSheet_->Tick();
  // the preview sample's 60s window scrolls off if it is only pushed once
  if (PreviewSampleActive() && chartTickCount_ % 20 == 0) PreviewSampleCharts();
  if (++chartTickCount_ % 10 == 0) {  // ~1s cadence
    if (splitRulesSheet_) splitRulesSheet_->RefreshTimes();  // "Ns ago" labels
    // the activity rows' age prefix rides the same 1s cadence as the sheet's;
    // gated on the pane being on screen, like every other repaint here
    if (w_.ConnectView().Visibility() == Visibility::Visible) RefreshConnectionRowTimes();
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
    // Every fourth row repeats an earlier host: group-by-host needs repeated
    // hosts in the sample, or the preview demonstrates 30 one-member groups,
    // which is the mode showing nothing. Deterministic, like everything here.
    const char* host = (i % 4 == 3) ? kHosts[(i / 4) % 8] : kHosts[i];
    action.hosts = {host};
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
      action.matchedHosts = {host};
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
  // The mode bar itself is seeded from the SDK's stored mode (SeedConnectControls,
  // run by ResyncDrawer just before this), and with no session that lands on
  // Never - "Never" beside discoverable + 3 clients is a contradiction. The
  // sample PROVIDES, so pin Auto with it, behind the guard so the selection
  // change does not push a setting into the session-less SDK.
  updatingControls_ = true;
  w_.ProvideModeBar().SelectedItem(w_.ProvideAutoItem());
  updatingControls_ = false;
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

void ConnectPage::OnProviderCountClick(IInspectable const&, RoutedEventArgs const&) {
  // LiveStatsGroup is already collapsed while disconnected, so this guard is
  // belt-and-braces — the sheet has nothing to draw without a session. While
  // connecting it opens too, listing the providers known so far (its empty
  // state reads "Connecting to providers").
  const bool connecting = health_ == urnw::health::State::Connecting;
  if (!connected_ && !connecting) return;
  ShowProviderLocationsSheet();
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

winrt::fire_and_forget ConnectPage::ShowTransportSettingsSheet(
    urnw::TransportSettingsKind kind) {
  if (w_.sheetOpen()) co_return;
  auto self = w_.get_strong();
  w_.SetSheetOpen(true);
  try {
    // the draft opens on the policy in force (the last change-listener push, or
    // the SDK default when none is known) and applies together on Update; live
    // pushes don't reset the open editor (dns parity)
    transportSheet_ = urnw::TransportSettingsSheet::Create(
        self->Content().XamlRoot(), Sdk(), kind,
        kind == urnw::TransportSettingsKind::Provider ? providerTransportSettings_
                                                      : clientTransportSettings_);
    co_await transportSheet_->Dialog().ShowAsync();
  } catch (...) {
  }
  transportSheet_.reset();
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

// The connected providers and where they are: globe + list, opened from the
// "Connected to N providers" row (android ProviderLocationsScreen parity).
winrt::fire_and_forget ConnectPage::ShowProviderLocationsSheet() {
  if (w_.sheetOpen()) co_return;  // only one ContentDialog can show at a time
  auto self = w_.get_strong();
  w_.SetSheetOpen(true);
  try {
    providerLocationsSheet_ =
        urnw::ProviderLocationsSheet::Create(self->Content().XamlRoot(), Sdk());
    // seeded from the CACHE, not a getter: the locations feed is signal-only, so
    // the rows the sheet should open on are the ones the last push left here
    providerLocationsSheet_->Update(providerLocations_, Sdk().RemoteConnected());
    // seed the badge set from the current snapshot so open-while-connected shows
    // badges immediately, not only after the next identity change
    providerIdentities_ = Sdk().CurrentProviderIdentities();
    providerLocationsSheet_->UpdateIdentities(providerIdentities_);
    co_await providerLocationsSheet_->Dialog().ShowAsync();
  } catch (...) {
  }
  providerLocationsSheet_.reset();
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
