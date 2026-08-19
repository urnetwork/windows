// The connect drawer (macOS ConnectActions parity): status line and connect
// button, the selected-provider row, live stats, the performance / provide
// controls, the three stats cards over live SDK feeds, the DNS summary and the
// ad/tracker blocker — plus the ContentDialog sheets those cards open.
//
// Split out of MainWindow.xaml.cpp. SDK listener callbacks are marshalled onto
// the UI thread through a DispatcherQueue captured here plus the window's weak
// reference; nothing in this page holds a strong ref across a callback, and no
// UI is touched off the UI thread.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>

#include "ConnectCanvas.h"
#include "LocationSheets.h"
#include "ProviderLocationsSheet.h"
#include "SdkHost.h"
#include "ServiceSetup.h"
#include "StatsSheets.h"
#include "TransferChart.h"
#include "TransportBar.h"
#include "UpdateChecker.h"
#include "UrComponents.h"  // kit::PaneListRowButton (the selectable activity row)

namespace winrt::URnetwork::implementation {
struct MainWindow;
}

namespace urnw {

class ConnectPage {
 public:
  explicit ConnectPage(winrt::URnetwork::implementation::MainWindow& window);
  ~ConnectPage();

  // charts, SDK feed subscriptions and the drawer clock
  void Initialize();
  void SetPresentationActive(bool active);
  void ApplyStrings();
  // Advanced Mode changed (D5). The sibling of ApplyStrings: one call, after
  // which every surface on this page has re-read itself in the new mode. Home's
  // two readings are
  //
  //   Normal    the activity rows are static, the third pane is the statistics
  //             pane, and the session figures are the five a user cares about.
  //   Advanced  every activity row is SELECTABLE and the third pane leads with a
  //             connection inspector for the selection; the session figures gain
  //             the raw pre-clamp status and the session mode; contract rows show
  //             full client ids.
  //
  // Cheap and idempotent: it re-renders from the caches this page already holds
  // and issues no RPC of its own.
  void ApplyAdvancedMode(bool on);

  // ---- window-level relays ----
  void ApplyStats(urnw::LiveStats const& stats);
  void SetConnectedUi(bool connected);
  // network name off the stored jwt, for the idle "{name} is ready to connect"
  // copy; re-renders the status line
  void SetNetworkIdentity(std::string const& networkName, bool guestMode);
  void ResyncDrawer();     // seed the caches/panes from SdkHost snapshots
  void AnimateDrawerIn();  // the entrance; see the definition
  // --preview-ui + URNETWORK_PREVIEW_SAMPLE only: synthetic rows for the panes,
  // so a review build shows the layout FULL rather than four empty states. Two
  // gates, like PreviewHeroActive, and it writes only this page's own caches —
  // no Sdk() call, no network, no stored state. Called from MainWindow's
  // preview entry beside PreviewSampleStatusStrip.
  void ApplyPreviewSample();

  // The service-setup banner (beta spec §3): the one writer of ServiceSetupBar.
  // MainWindow owns the snapshot and pushes every change through here, the way
  // UpdateBalanceWarning pushes the balance InfoBar's gate — this only renders.
  // Running / ConsoleMode / Unknown all close the bar: the healthy state needs
  // no banner, a developer console must not be interfered with, and no
  // evidence means no claim.
  void ApplyServiceSetup(urnw::ServiceSetup::Snapshot const& snap);

  // The update banner (beta spec §5): the one writer of UpdateBar, stacked
  // directly under ServiceSetupBar with the same one-writer discipline —
  // MainWindow owns the snapshot copy and pushes every change through here.
  // Phase::None closes the bar; everything else renders one of the four
  // standing states (offer / in-flight / manual finish / failure).
  void ApplyUpdateChecker(urnw::UpdateChecker::Snapshot const& snap);

  // Status line + status dot + hero canvas + connect button, from connectStatus_
  // (the SDK), connected_ (the service tunnel) and the window's balance state.
  // The single place any of them is written. Public because the balance inputs
  // live on MainWindow: UpdateBalanceWarning calls this so the hero's error and
  // processing states and the InfoBar can never disagree.
  void ApplyConnectStatus();

  // ---- XAML event handlers (forwarded from MainWindow) ----
  void OnConnectToggle(winrt::Windows::Foundation::IInspectable const&,
                       winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnConnectionModeChanged(
      winrt::Microsoft::UI::Xaml::Controls::SelectorBar const&,
      winrt::Microsoft::UI::Xaml::Controls::SelectorBarSelectionChangedEventArgs const&);
  void OnProvideModeChanged(
      winrt::Microsoft::UI::Xaml::Controls::SelectorBar const&,
      winrt::Microsoft::UI::Xaml::Controls::SelectorBarSelectionChangedEventArgs const&);
  void OnFixedIpToggled(winrt::Windows::Foundation::IInspectable const&,
                        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnStrongAnonToggled(winrt::Windows::Foundation::IInspectable const&,
                           winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnPostQuantumToggled(winrt::Windows::Foundation::IInspectable const&,
                            winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnBlockerToggled(winrt::Windows::Foundation::IInspectable const&,
                        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnClientStatsCardClick(winrt::Windows::Foundation::IInspectable const&,
                               winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnLocalStatsCardClick(winrt::Windows::Foundation::IInspectable const&,
                              winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnDnsCardClick(winrt::Windows::Foundation::IInspectable const&,
                       winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnLocationRowClick(winrt::Windows::Foundation::IInspectable const&,
                           winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnPeersLineClick(winrt::Windows::Foundation::IInspectable const&,
                         winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  // "Connected to N providers" -> the globe sheet.
  void OnProviderCountClick(winrt::Windows::Foundation::IInspectable const&,
                             winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  // the inspector's "clear the selection" action (Advanced Mode)
  void OnInspectorClear(winrt::Windows::Foundation::IInspectable const&,
                        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);

 private:
  // The SDK's own connection status (ConnectViewController.getConnectionStatus,
  // surfaced as LiveStats.connectionStatus). Mirrors android's
  // ui/shared/models/ConnectStatus. This is a DIFFERENT signal from the service
  // tunnel state that drives connected_: the tunnel says whether packets can
  // flow, this says what the connect controller is doing about it, and only this
  // one has a "connecting" value to show.
  // Failed mirrors the SDK's CONNECT_FAILED (the window honesty layer's
  // terminal outcome); an old SDK never emits it and the parse falls through
  // to Disconnected exactly as before.
  enum class ConnectStatus { Disconnected, Connecting, DestinationSet, Connected, Failed };

  static ConnectStatus ParseConnectStatus(std::string const& value);
  // What the connect button does right now, from the ONE shared rule in
  // Common/ConnectAction.h: the action is Disconnect whenever there is anything
  // for a disconnect to DO — the SDK is driving something, OR this machine is
  // captured (routes installed, or a firewall policy in force). The tray asks
  // the same question of the same predicate.
  bool ConnectActionIsDisconnect() const;
  // The service facts the shared rule needs, from the window's one cache. The
  // string_view points into that cache, which outlives every call here.
  urnw::gesture::ServiceFacts CurrentServiceFacts() const;
  // The health state this page would RENDER right now: health_ reconciled
  // against the SDK's own connect status. Factored out of ApplyConnectStatus so
  // the button label and the status line cannot be computed from two different
  // readings of the same instant.
  urnw::health::State RenderHealth() const;
  // The connect action's two forms (App.xaml UrPaneActionPrimaryStyle /
  // ...SecondaryStyle): filled blue while there is something to do, outlined
  // once the tunnel is up. Called only from ApplyConnectStatus, which is the
  // single place the connect state is rendered.
  void ApplyConnectButtonStyle(std::wstring_view key);

  void BuildCharts();
  void BuildHero();        // the ConnectCanvas plus the hero's desktop affordances
  // --preview-ui + URNETWORK_PREVIEW_HERO only: a locally generated grid and a
  // state walk, so the hero's populated states can be looked at without a
  // session. Makes no network request of any kind — nothing here touches Sdk().
  bool PreviewHeroActive() const;
  void PreviewHeroTick();
  void WireDrawerFeeds();  // SdkHost push handlers -> UI thread -> caches/cards
  void SeedConnectControls();  // performance profile + blocker toggle state
  void PushPerformanceSettings();
  // Non-const: reads the ConnectionModeBar / ModeWebItem / ModeStreamingItem x:Name
  // accessors, which C++/WinRT generates as non-const members of the .xaml.g.h base.
  urnw::ConnectionMode SelectedMode();
  // provide control mode picker <-> the SDK's mode string (same non-const note)
  std::string SelectedProvideMode();
  void ApplyDnsCard(std::optional<urnet::DnsResolverSettings> const& settings);
  // The unapplied-recommendation pill atop the dns card: compares the applied dns
  // settings against the connected country's regional recommendation (else the
  // safe defaults) and shows/collapses the pill. Recomputed on dns-setting changes
  // (ApplyDnsCard) and connected-country changes (ApplyStats).
  void ApplyDnsRecommendationPill();
  // the row's automation name: the label plus the provider it names
  void ApplyLocationRowName();
  void ApplySplitRuleCount();
  void ApplyBlockerUi(bool on);
  // R3: the activity pane's list vs its empty state. The empty state is a
  // centred line INSIDE the full-height pane, not a card, so this only swaps
  // which of the two is drawn in that same area.
  void ApplySessionCardsVisibility(bool connected);

  // ---- R3: the pane lists ---------------------------------------------------
  // The three dense, uniform-row lists the pane shell put where the cards were.
  // Each rebuilds its host StackPanel from this page's cached feed, and every
  // row in a list is the same height as every other row in it.
  //
  //   activity pane     ApplyConnectionsList  the routing decisions (block
  //                                           actions): verdict, host, bytes
  //   statistics pane   ApplySessionRows      the session figures, key/value
  //                     ApplyContractsList    one row per contract peer
  //                     ApplySplitRulesList   one row per split rule
  void ApplyConnectionsList();
  void ApplySessionRows();

  // ---- D5: the connection inspector -----------------------------------------
  //
  // Advanced Mode turns the activity pane's rows into a SELECTION and the third
  // pane into the detail for it. Everything the inspector prints comes from a
  // feed this page or SdkHost already holds — nothing here fabricates a field
  // the SDK cannot supply, and the fields it CANNOT supply (protocol, port,
  // per-direction counters, ASN, per-connection duration) are absent rather than
  // guessed. See the report; they need bridging or upstream SDK work.
  //
  // Selection is held by the block action's ID, not by its index. The feed is a
  // live rebuild on every push and rows move; an index selection follows the
  // POSITION and quietly starts inspecting a different connection, which is the
  // worst failure available to a tool whose whole job is to tell you what a
  // given connection is doing.
  void SelectConnection(std::string const& id);
  void ApplyInspector();
  // Paint the selected/unselected state across the rows already on screen,
  // without rebuilding them: a rebuild on every click loses focus mid-keyboard-
  // navigation, which makes the list unusable from the keyboard.
  void ApplyConnectionSelectionVisuals();
  // The exit a destination ip routed through, and that exit's health, joined out
  // of the reliability snapshot (DestinationExit.DestinationIp -> ClientId ->
  // Exit). Returns nullopt when the ip is not in the snapshot, which is the
  // normal case for a host whose addresses the block action never recorded.
  struct ExitRouting {
    std::string clientId;
    int32_t flowCount = 0;
    bool haveExit = false;  // the clientId was also found in the exits list
    int32_t tier = 0;
    int32_t effectiveTier = 0;
    int32_t exitFlowCount = 0;
    int32_t dialFailureCount = 0;
    bool quarantined = false;
    bool warning = false;
    std::string warningCause;
    bool proven = false;
    int64_t probeAgeSeconds = 0;
  };
  std::optional<ExitRouting> RoutingForAddresses(
      std::vector<std::string> const& addresses) const;
  // Refresh the exits / destination-exits cache the inspector joins against.
  //
  // ReadReliability() is several SYNCHRONOUS rpcs into the service, so it runs on
  // a background thread and marshals back — never on the UI thread. Driven from
  // the drawer clock at a low cadence and ONLY while Advanced Mode is on and the
  // window is presenting, because a background poll for a pane nobody is looking
  // at is the cost of the feature with none of the value.
  winrt::fire_and_forget RefreshExitRouting();
  void ApplyContractsList();
  void ApplySplitRulesList();
  //   connect pane   ApplyPeersList  the other devices on this network. It is
  //                                  the only feed that belongs to the connect
  //                                  column, and without it that column is a
  //                                  fixed block of controls that ends two
  //                                  thirds of the way down a tall window.
  void ApplyPeersList();

  bool PreviewSampleActive() const;
  // A minute of synthetic throughput ending NOW. Regenerated on a cadence from
  // OnChartTick, not pushed once: the charts hold a 60s window, so a single push
  // at startup has scrolled off the left edge by the time anyone looks at the
  // build, and the review screenshot is of three flat lines.
  void PreviewSampleCharts();
  void OnChartTick();
  winrt::fire_and_forget ShowClientContractsSheet();
  winrt::fire_and_forget ShowSplitRulesSheet();
  winrt::fire_and_forget ShowDnsSheet();
  // The transport settings editor (TRANSPORTSTATS), opened from the transport
  // distribution bar under the Remote chart with the CLIENT policy. The
  // provider policy has no surface on windows yet (there is no provider stats
  // pane), but the sheet and the SdkHost path are parameterized for it.
  winrt::fire_and_forget ShowTransportSettingsSheet(urnw::TransportSettingsKind kind);
  winrt::fire_and_forget ShowLocationChooserSheet();
  // The connected providers and where they are: globe + list, opened from the
  // "Connected to N providers" row (android ProviderLocationsScreen parity).
  winrt::fire_and_forget ShowProviderLocationsSheet();
  // drawer "N network peers" sub-label (req1); space-preserved (blank + Opacity
  // 0 when there are none) so the location row never jumps
  void ApplyPeerCount(std::optional<urnet::NetworkPeerList> const& peers);

  winrt::URnetwork::implementation::MainWindow& w_;

  // the SERVICE tunnel is up (OnTunnelStateChanged, fed by both the SDK's
  // connect-location listener and the service pipe). Held for the
  // reconnect-tunnel affordance; NOT what the connect button reads -- see
  // ConnectActionIsDisconnect.
  bool connected_ = false;
  // the SDK connect controller's own status (ApplyStats); see ConnectStatus
  ConnectStatus connectStatus_ = ConnectStatus::Disconnected;
  // ---- #27: the aggregate connection health ----
  // What the status line/dot/strip/hero actually render now. Derived in
  // SdkHost::ReadStats (ConnectionHealth.h owns the transition table) and
  // carried on the same LiveStats as connectStatus_, so the two can only
  // disagree across the OPTIMISTIC local write a connect press makes — which
  // ApplyConnectStatus reconciles explicitly. NoService claims least.
  urnw::health::State health_ = urnw::health::State::NoService;
  // Non-zero while a degrade hold is pending: the steady-millis deadline at
  // which the clock alone changes health_. OnChartTick asks SdkHost to
  // republish then — no SDK event is coming; see LiveStats::healthReevalAtMillis.
  int64_t healthReevalAtMillis_ = 0;
  // network name off the stored jwt, for the idle "{name} is ready to connect"
  // copy. Read once per auth change, not per stats push (ParsedJwt re-parses).
  std::string networkName_;
  bool guestMode_ = false;
  // the last peer snapshot, for the connect pane's list (ApplyPeerCount already
  // receives it; before R3 only its COUNT was ever drawn)
  std::optional<urnet::NetworkPeerList> peers_;

  // the hero canvas (UI thread only; stepped from chartTimer_)
  std::unique_ptr<urnw::ConnectCanvas> canvas_;
  // When the SDK reports CONNECTING the connect action is disabled: the press
  // has been accepted and a second one would fire a duplicate connect. It is a
  // WATCHDOG, not a latch — see ApplyConnectStatus — so a connect that hangs
  // re-enables the control instead of trapping the user in a dead screen.
  std::chrono::steady_clock::time_point connectingSince_{};
  bool connectWatchdogFired_ = false;

  // drawer state (UI thread only)
  std::unique_ptr<urnw::TransferChart> remoteChart_;
  std::unique_ptr<urnw::TransferChart> blockedChart_;
  std::unique_ptr<urnw::TransferChart> localChart_;
  // the transport distribution bar directly under the Remote chart (TRANSPORTSTATS)
  std::unique_ptr<urnw::TransportBar> transportBar_;
  // the client / provider transport policies in force, from SdkHost's change
  // listeners (nullopt = unknown -> the editor opens on the SDK default). Cached
  // here so the sheet opens on the last push, like dnsSettings_.
  std::optional<urnet::TransportSettings> clientTransportSettings_;
  std::optional<urnet::TransportSettings> providerTransportSettings_;
  winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer chartTimer_{nullptr};
  uint32_t chartTickCount_ = 0;
  std::vector<urnw::ContractPeerRow> contractRows_;
  std::vector<urnw::BlockActionItem> blockActions_;
  // The globe sheet's two feeds, cached here rather than read on open: both are
  // signal-only pushes from SdkHost, so the sheet has to be able to come up on
  // whatever the last push left, not on a fresh getter read.
  std::vector<urnw::ProviderLocationRow> providerLocations_;
  std::vector<urnw::ProviderIdentityRow> providerIdentities_;
  std::vector<urnw::SplitRule> splitRules_;
  int64_t allowedCount_ = 0;
  int64_t blockedCount_ = 0;
  // R3: the statistics pane draws the session as key/value ROWS, which means it
  // needs the last figures rather than only the two prose lines the card used to
  // print. Written by ApplyStats, read by ApplySessionRows.
  int64_t downBitsPerSecond_ = 0;
  int64_t upBitsPerSecond_ = 0;
  int64_t providerCount_ = 0;
  bool statsConnected_ = false;
  std::optional<urnet::DnsResolverSettings> dnsSettings_;
  std::string countryCode_;  // selected location country (dns recommendations)
  std::string countryName_;
  // The SDK's stall diagnosis for a still-forming window (track 2), cached
  // off LiveStats for the reason line ApplyConnectStatus renders. Empty means
  // nothing to say (idle, or a service that predates the field).
  std::string windowStallReason_;
  // ---- D5: Advanced Mode ----
  // Pushed from MainWindow::ApplyAdvancedMode, which is itself driven by
  // SdkHost's standing value — this page never reads the preference itself, so
  // there is one authority and one apply path.
  bool advancedMode_ = false;
  // The selected connection, by BlockActionItem::id. Empty means "nothing
  // selected", which in Advanced Mode is a real state with its own inspector
  // reading, not an error.
  std::string selectedConnectionId_;
  // The rows currently on screen, parallel to the visible slice of
  // blockActions_, so a selection change repaints instead of rebuilding.
  std::vector<urnw::kit::PaneListRowButton> connectionRows_;
  std::vector<std::string> connectionRowIds_;
  // The reliability snapshot's routing tables, refreshed off-thread. Exits are
  // keyed by client id; destination exits map a destination ip to the exit
  // carrying its flows. This is the ONLY per-connection "which exit" the SDK has.
  std::vector<urnet::Exit> exits_;
  std::vector<urnet::DestinationExit> destinationExits_;
  bool exitRefreshInFlight_ = false;
  uint32_t exitRefreshTick_ = 0;

  bool updatingControls_ = false;  // guards programmatic toggle/segment updates
  bool drawerAnimated_ = false;    // entrance plays once per window
  std::shared_ptr<urnw::ClientContractsSheet> contractsSheet_;
  std::shared_ptr<urnw::SplitRulesSheet> splitRulesSheet_;
  std::shared_ptr<urnw::DnsEditorSheet> dnsSheet_;
  std::shared_ptr<urnw::TransportSettingsSheet> transportSheet_;
  std::shared_ptr<urnw::LocationChooserSheet> locationSheet_;
  std::shared_ptr<urnw::ProviderLocationsSheet> providerLocationsSheet_;
};

}  // namespace urnw
