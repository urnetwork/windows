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
#include <winrt/Microsoft.UI.Xaml.Shapes.h>  // ConnectionRowEntry's Ellipse

#include "ConnectCanvas.h"
#include "ExtenderPanel.h"
#include "FreeRefreshTicker.h"
#include "IpFamilyStatusRow.h"
#include "LocationSheets.h"
#include "ProviderLocationsSheet.h"
#include "SdkHost.h"
#include "ServiceSetup.h"
#include "StatsSheets.h"
#include "TapSequenceGate.h"
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

  // The statistics pane's fold state, pushed by MainWindow::ApplyBreakpoint
  // (its connectThree gate, inverted - the window computes the one gate, this
  // page renders it, the same push shape as ApplyAdvancedMode above). While
  // pane C is folded its sheet doors would fold with it, so the fold-gated
  // section in pane A (BuildFoldDoors) shows instead: the fold rule bars a
  // foldable pane from owning content with no second door.
  void ApplyPaneCFolded(bool folded);

  // ---- window-level relays ----
  void ApplyStats(urnw::LiveStats const& stats);
  // The provider extender row under the provide group (connect/EXTENDER.md N7).
  // MainWindow hands every pushed status here and to the Earnings page, as it
  // hands the live stats to both.
  void ApplyExtenderProvideState(urnw::ExtenderProvideStatusView const& view);
  // The provider transport policy in force, as the settings listener last pushed
  // it (nullopt: unknown, and the editor opens on the SDK default). The Earnings
  // provider transport sheet opens on it rather than reading the device on the
  // UI thread.
  std::optional<urnet::TransportSettings> const& ProviderTransportSettings() const {
    return providerTransportSettings_;
  }
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
  // Phase::None closes the bar, unless no check has worked for 72 hours, which
  // it says; everything else renders one of the five standing states (offer /
  // in-flight / manual finish / failure / the update helper's report).
  void ApplyUpdateChecker(urnw::UpdateChecker::Snapshot const& snap);
  // The report's title, message and severity.
  static winrt::Microsoft::UI::Xaml::Controls::InfoBarSeverity ApplyUpdateResult(
      urnw::UpdateChecker::Snapshot const& snap, winrt::hstring& title, std::wstring& message);

  // Status line + status dot + hero canvas + connect button, from connectStatus_
  // (the SDK), connected_ (the service tunnel) and the window's balance state.
  // The single place any of them is written. Public because the balance inputs
  // live on MainWindow: UpdateBalanceWarning calls this so the hero's error and
  // processing states and the InfoBar can never disagree.
  void ApplyConnectStatus();
  // the out-of-balance banner's message (ApplyConnectStatus, then once per
  // displayed minute of the free refresh countdown while it is open)
  void ApplyBalanceWarningMessage();

  // ---- XAML event handlers (forwarded from MainWindow) ----
  void OnConnectToggle(winrt::Windows::Foundation::IInspectable const&,
                       winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnConnectionModeChanged(
      winrt::Microsoft::UI::Xaml::Controls::SelectorBar const&,
      winrt::Microsoft::UI::Xaml::Controls::SelectorBarSelectionChangedEventArgs const&);
  void OnProvideModeChanged(
      winrt::Microsoft::UI::Xaml::Controls::SelectorBar const&,
      winrt::Microsoft::UI::Xaml::Controls::SelectorBarSelectionChangedEventArgs const&);
  // the extender switch: writes the setting through the device and paints the
  // row's guess until the next pushed status (N7)
  void OnExtenderToggled(winrt::Windows::Foundation::IInspectable const&,
                         winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
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
  // The fold-gated sheet doors at the foot of pane A (the fold rule; see
  // ApplyPaneCFolded above). Idempotent - ApplyStrings calls it on every pass
  // and then re-strings the rows like every other label.
  void BuildFoldDoors();
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
  // The extender row, its description and its switch, from extenderProvideView_:
  // the one writer of all three (N7).
  void ApplyExtenderProvideRow();
  // R3: the activity pane's list vs its empty state. The empty state is a
  // centred line INSIDE the full-height pane, not a card, so this only swaps
  // which of the two is drawn in that same area.
  void ApplySessionCardsVisibility(bool connected);

  // ---- R3: the pane lists ---------------------------------------------------
  // The three dense, uniform-row lists the pane shell put where the cards were.
  // Each renders its host StackPanel from this page's cached feed, and every
  // row in a list is the same height as every other row in it.
  //
  //   activity pane     ApplyConnectionsList  the routing decisions (block
  //                                           actions): verdict, host, bytes.
  //                                           INCREMENTAL: rows are keyed by
  //                                           BlockActionItem::id and updated
  //                                           in place - a feed push inserts
  //                                           the new rows at the top, rewrites
  //                                           the rest, trims past the 200-row
  //                                           cap, and never Clear()s, so the
  //                                           scroller's offset survives every
  //                                           push. resetScroll is for the
  //                                           filter controls: a changed filter
  //                                           is a new result set and reads
  //                                           from the top. Group-by-host folds
  //                                           the same filtered feed into one
  //                                           row per display host, keyed by
  //                                           the host - a second payload over
  //                                           the SAME pass, not a second list.
  //   statistics pane   ApplySessionRows      the session figures, key/value
  //                     ApplyContractsList    one row per contract peer
  //                     ApplySplitRulesList   one row per split rule
  void ApplyConnectionsList(bool resetScroll = false);
  void ApplySessionRows();

  // ---- the connections filter row + relative time ---------------------------
  // The verdict filter: the same three-way verdict the rows print (blocked /
  // tunnelled / sent AROUND the tunnel) plus All. Applied view-side over the
  // cached blockActions_ - no Sdk() read - so the filter and the push path can
  // never disagree about the feed, and a filter change re-evaluates membership
  // through the same incremental pass a push uses.
  enum class ConnectionVerdictFilter { All, Blocked, Tunnelled, Bypassed };
  static bool VerdictPassesFilter(ConnectionVerdictFilter filter,
                                  urnw::BlockActionItem const& action);
  // The host/IP substring: case-insensitive over every identity field the row
  // or the inspector can print (hosts, ips, and the override-matched variants).
  static bool ConnectionQueryPasses(std::string const& query,
                                    urnw::BlockActionItem const& action);
  // One host's aggregate of the FILTERED feed (group-by-host). The fold key is
  // the display host (BlockActionTitle's precedence: first matched host, else
  // host, else ip), the verdict is the precedence the row dots already print -
  // blocked if ANY decision in the group blocked, else bypassed if any went
  // around the tunnel, else tunnelled - the counters are the sums, and the time
  // is the LATEST decision's, which is also the group's sort order.
  struct ConnectionGroup {
    std::string host;
    int64_t connections = 0;
    int64_t byteCount = 0;
    int64_t packetCount = 0;
    int64_t latestMillis = 0;
    bool anyBlocked = false;
    bool anyLocal = false;
    // the newest decision in the group: the row menu's quick actions read its
    // override id for QuickActionFor's `decided` fallback, the way a flat row
    // reads its own action.
    urnw::BlockActionItem const* latest = nullptr;
  };
  // The filtered feed folded by host. The ONE fold: ApplyConnectionsList
  // renders it and a group row's menu re-reads it, so the two can never
  // disagree about what a group holds.
  std::vector<ConnectionGroup> FoldConnectionGroups() const;
  // The reconcile's unit: ONE routing decision, or one host group. Exactly one
  // of the two pointers is set, and that pointer IS the row's kind.
  struct ConnectionViewItem {
    urnw::BlockActionItem const* action = nullptr;
    ConnectionGroup const* group = nullptr;
  };
  // One row of the activity list as it stands on screen: the elements both
  // modes share (root, dot, title, meta) plus, in Advanced Mode, the selectable
  // row kept whole for SetPaneListRowSelected. The cached counters let the 1s
  // clock re-render the relative-time prefix without a feed push. `group`
  // marks the entry model's second kind: a host aggregate (id = the host)
  // rather than one routing decision (id = BlockActionItem::id).
  struct ConnectionRowEntry {
    std::string id;
    bool group = false;       // a host aggregate row, keyed by host
    bool selectable = false;  // root is a Button (Advanced Mode), else a Border
    winrt::Microsoft::UI::Xaml::UIElement root{nullptr};
    winrt::Microsoft::UI::Xaml::Shapes::Ellipse dot{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock title{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock meta{nullptr};
    urnw::kit::PaneListRowButton button{};  // valid only when selectable
    int64_t timeMillis = 0;
    int64_t byteCount = 0;
    int64_t packetCount = 0;
    int64_t groupConnections = 0;  // the fold count; group rows only
  };
  ConnectionRowEntry BuildConnectionRow(ConnectionViewItem const& item);
  void UpdateConnectionRow(ConnectionRowEntry& entry, ConnectionViewItem const& item);
  // the search row (kit::MakePaneSearchRow, NetworkPage::Build parity), once
  void BuildConnectionsFilter();
  // verdict bar change -> verdictFilter_ -> incremental re-evaluation
  void OnConnectionsVerdictChanged();
  // group-by-host switch change -> connectionsGrouped_ -> the same pass
  void OnConnectionsGroupToggled();
  // The clear-filters button (visible only while any of the three controls is
  // off its default): one click puts the verdict bar, the search text and the
  // group fold back to their defaults and re-runs the ONE incremental pass -
  // a filter change is a new result set, and three at once are no different.
  void OnConnectionsClearFilters();
  // The verdict ratio bar under the connections group header (markup's
  // ConnectionsVerdictRatio): the session's allowed / blocked proportions from
  // BlockStats plus the bypassed-local third counted over the cached
  // blockActions_ window. Recomputed only on the feed pushes that already
  // rebuild the header count - three star-weight writes, never a rebuild, so
  // the strip costs nothing per frame.
  void ApplyVerdictRatioBar();
  // A group row's click, and the whole of its interaction: fill the search box
  // with the group's host and leave group mode, so the group expands IN PLACE
  // through the filter that already exists. Not a selection (a group has no
  // one connection to inspect) and not a nested list.
  void DrillIntoConnectionGroup(std::string const& host);
  // Re-render every row's relative-time prefix on the 1s divider: a repaint of
  // one TextBlock per row, never a rebuild.
  void RefreshConnectionRowTimes();
  // The small-height scroll escape for pane B (the markup comment on
  // ActivityBodyScroll has the rule): pin the body under the filter rows at
  // its scroller's viewport height, floored at kActivityBodyMinHeight, so the
  // outer scroller engages only when the window is too short for the fixed
  // blocks AND the list together. The same viewport drives the Remote chart's
  // flex (kRemoteChartMinHeight..kRemoteChartMaxHeight).
  void ApplyActivityBodyHeight();
  // pane C's chart flex (the markup comment on PaneCBody has the rule): pin
  // the body grid's MinHeight to its scroller's viewport, so the two star
  // chart rows have leftover to share exactly while the content is shorter
  // than the pane.
  void ApplyPaneCBodyHeight();

  // ---- D5: the connection inspector -----------------------------------------
  //
  // Advanced Mode turns the activity pane's rows into a SELECTION and the third
  // pane into the detail for it. Everything the inspector prints comes from a
  // feed this page or SdkHost already holds — nothing here fabricates a field
  // the SDK cannot supply, and the fields it CANNOT supply (protocol, port,
  // per-direction counters, ASN, per-connection duration) are absent rather than
  // guessed. See the report; they need bridging or upstream SDK work.
  //
  // Selection is held by the block action's ID, not by its index. The feed is
  // live and rows move on every push; an index selection follows the POSITION
  // and quietly starts inspecting a different connection, which is the worst
  // failure available to a tool whose whole job is to tell you what a given
  // connection is doing.
  void SelectConnection(std::string const& id);
  void ApplyInspector();
  // The selection in the CURRENT feed, or nullptr (nothing selected, or the
  // action aged out of the SDK's window). ApplyInspector and the three quick
  // action handlers all need exactly this lookup.
  const urnw::BlockActionItem* SelectedConnectionAction() const;
  // ---- the per-connection quick actions (Portmaster's observe-decide-rule) ---
  // One button pair (block/allow, bypass/tunnel) plus copy-details, rendered by
  // ApplyInspector from the LIVE overrides list (SdkHost::CurrentHostRules) -
  // the block action itself is a snapshot of the decision as made and does not
  // change when a rule lands afterwards, so it cannot carry the on/off state.
  //
  // Toggle semantics, not duplicates: when a rule covers the connection the
  // button is ON and the click removes that rule; otherwise the click creates
  // the rule whose polarity is the inverse of the current verdict (a blocked
  // connection gets an allow override, a tunnelled one a block override, and
  // the same for bypass/tunnel).
  struct InspectorQuickAction {
    bool enabled = false;     // there is something to rule on (or a rule to remove)
    bool active = false;      // on-state: a rule is in force; click removes overrideId
    std::string overrideId;   // the rule the click removes (when active)
    // the rule's value (block kind: true=blocking; route kind: true=bypass),
    // which the label reads rather than the action's verdict - the verdict is
    // a STALE snapshot here (the rule landed after the decision), and the
    // label names what the click leaves behind.
    bool polarity = false;
    std::vector<std::string> hosts;  // the host values a click would rule on (when !active)
  };
  // kind: true walks block overrides, false route overrides. Host matching is
  // exact string equality over every identity field the row prints, and falls
  // back to the action's own deciding override id, which covers the SDK's
  // suffix matching (an override for the parent named this connection without
  // naming its exact host).
  InspectorQuickAction QuickActionFor(urnw::BlockActionItem const& action,
                                      bool blockKind) const;
  void OnInspectorBlockToggle();
  void OnInspectorRouteToggle();
  void OnInspectorCopyDetails();
  // Execute a quick action's click against the LIVE rule state: the toggle's
  // off half removes the rule in force by id, the on half creates the rule
  // whose polarity is the inverse of the verdict, and both confirm through the
  // snackbar (Undo armed on a creation). Factored out of the inspector's
  // button handlers so the row context menu runs the SAME logic - the two
  // surfaces can never disagree about what a click does. `action` supplies the
  // verdict the create-half inverts (null tolerated; the create half no-ops).
  void RunBlockQuickAction(InspectorQuickAction const& state,
                           urnw::BlockActionItem const* action);
  void RunRouteQuickAction(InspectorQuickAction const& state,
                           urnw::BlockActionItem const* action);
  // The copy-details third of the same trio: the inspector's fields, in the
  // inspector's words, onto the clipboard.
  void CopyConnectionDetails(urnw::BlockActionItem const& action);
  // The row's right-tap menu (Advanced Mode, where rows are Buttons): the two
  // rule toggles and copy-details above, on the row under the pointer instead
  // of the selection. `key` is the row's reconcile key - the decision id, or
  // the host when `group` is set, in which case the menu rules on that host.
  void ShowConnectionRowMenu(winrt::Microsoft::UI::Xaml::FrameworkElement const& anchor,
                             std::string const& key, bool group);
  // the snackbar's Undo: deletes the just-created override by id
  void OnInspectorUndo();
  // Confirmation after a rule write. undoOverrideId non-empty arms the Undo
  // action (a creation); empty shows the acknowledgement alone (a removal).
  void ShowInspectorRuleSnackbar(winrt::hstring const& message,
                                 std::string undoOverrideId);
  // Paint a quick-action button for its state: outlined (the style's rest) when
  // a click would CREATE a rule, action-blue fill when one is in force - the
  // toggle on-state the switches already paint, on the button whose label
  // names the other side (the label changes word; the fill is a second
  // channel, never the only one).
  void ApplyQuickActionButton(winrt::Microsoft::UI::Xaml::Controls::Button const& button,
                              InspectorQuickAction const& state,
                              winrt::hstring const& label);
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
  // the easter egg's five-tap count on the status dot (Initialize wires it)
  TapSequenceGate connectedIconTaps_;
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
  // DESIGNSTYLE "Placeholders, not pop-in": the sections whose data arrives
  // after first paint (the dns readings, the transport legend) hold a skeleton
  // of their settled box until it lands. `dnsSettled_` is "a reading has been
  // taken" — absent settings after that are the unavailable row, before it
  // they are still loading. The chart clock closes both after
  // kPlaceholderCeilingMillis (a service that never answers settles on its
  // empty states, not a shimmer).
  bool dnsSettled_ = false;
  int64_t placeholdersSinceMillis_ = 0;
  void BeginPlaceholders();
  void SettlePlaceholders();
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
  // the IP-family status row directly under the transport bar (IPV6.md D2):
  // the Dualstack / IPv4 / IPv6 columns with their connected and connecting
  // counts, fed from the same grid push as the hero
  std::unique_ptr<urnw::IpFamilyStatusRow> ipFamilyStatusRow_;
  // The extender panel (EXTENDER.md K4), the row under the status row. Fed from
  // SdkHost's extender status feed through the same dispatcher hop as the other
  // drawer feeds; no click, no sheet.
  std::unique_ptr<urnw::ExtenderPanel> extenderPanel_;
  // the client / provider transport policies in force, from SdkHost's change
  // listeners (nullopt = unknown -> the editor opens on the SDK default). Cached
  // here so the sheet opens on the last push, like dnsSettings_.
  std::optional<urnet::TransportSettings> clientTransportSettings_;
  std::optional<urnet::TransportSettings> providerTransportSettings_;
  winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer chartTimer_{nullptr};
  urnw::FreeRefreshTicker balanceRefreshTicker_;  // the banner's countdown
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
  // Re-collected from connectionRowEntries_ after each incremental reconcile.
  std::vector<urnw::kit::PaneListRowButton> connectionRows_;
  std::vector<std::string> connectionRowIds_;
  // The activity rows as they stand on screen, in display order and parallel
  // to ConnectionsHost.Children() - the structure the incremental
  // ApplyConnectionsList reconciles against the filtered feed.
  std::vector<ConnectionRowEntry> connectionRowEntries_;
  // The mode the on-screen rows were built for. A flip changes the row TYPE
  // (static Border <-> selectable Button), which an incremental pass cannot
  // morph - it is the one ApplyConnectionsList path that still clears
  // (ApplyAdvancedMode, a user gesture and never a push).
  bool connectionRowsSelectable_ = false;
  ConnectionVerdictFilter verdictFilter_ = ConnectionVerdictFilter::All;
  // group-by-host (the filter strip's second control): the filtered feed folds
  // into one row per display host. Reconciled through the SAME incremental
  // pass - the entry carries a kind, there is no second list.
  bool connectionsGrouped_ = false;
  // the host/IP substring, lowercased and trimmed; empty = no text filter
  std::string connectionsQuery_;
  winrt::Microsoft::UI::Xaml::Controls::TextBox connectionsSearch_{nullptr};
  bool connectionsFilterBuilt_ = false;
  // The reliability snapshot's routing tables, refreshed off-thread. Exits are
  // keyed by client id; destination exits map a destination ip to the exit
  // carrying its flows. This is the ONLY per-connection "which exit" the SDK has.
  std::vector<urnet::Exit> exits_;
  std::vector<urnet::DestinationExit> destinationExits_;
  bool exitRefreshInFlight_ = false;
  uint32_t exitRefreshTick_ = 0;
  // The quick actions' current state, derived on every ApplyInspector. The
  // click handlers consume the stored state rather than re-deriving it, so
  // the click acts on exactly the button the user saw rendered.
  InspectorQuickAction blockQuickAction_, routeQuickAction_;
  // The rule confirmation + its Undo (kit::Snackbar on InspectorSnackbar).
  // inspectorUndoOverrideId_ is the just-created override the Undo action
  // deletes; empty while the bar shows a plain acknowledgement. The undo
  // button is built once and swapped onto the InfoBar's ActionButton per
  // Show (null hides the action slot; a collapsed button would leave its
  // padding behind).
  std::unique_ptr<urnw::kit::Snackbar> inspectorSnackbar_;
  winrt::Microsoft::UI::Xaml::Controls::Button inspectorUndoButton_{nullptr};
  std::string inspectorUndoOverrideId_;

  bool updatingControls_ = false;  // guards programmatic toggle/segment updates
  // ---- the provider extender row (N7) ----
  // The last pushed status, or the switch's guess painted over it until the
  // next push. The default is "no session": hidden, and never written.
  urnw::ExtenderProvideStatusView extenderProvideView_;
  // whether the device is providing (LiveStats), which the switch's guess reads
  bool provideEnabled_ = false;
  bool drawerAnimated_ = false;    // entrance plays once per window
  // ---- the fold-gated sheet doors (the fold rule; BuildFoldDoors) -----------
  // One row of the pane A section, kept for re-stringing: the rows are built
  // once, but ApplyStrings runs on every language change and the automation
  // name IS the title, so both move together.
  struct FoldDoor {
    std::string_view key;
    winrt::Microsoft::UI::Xaml::Controls::Button root{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock title{nullptr};
  };
  bool foldDoorsBuilt_ = false;
  // The last fold state ApplyPaneCFolded pushed, read at build time so a gate
  // that landed before the section existed is replayed (bind-then-replay, the
  // same shape as every other pushed value on this page).
  bool paneCFolded_ = false;
  winrt::Microsoft::UI::Xaml::Controls::StackPanel foldDoorHost_{nullptr};
  urnw::kit::PaneGroupHeader foldDoorHeader_{};
  std::vector<FoldDoor> foldDoors_;
  // The provider-locations door, tracked separately because it follows pane
  // C's ProviderCountLine rule: hidden while there is no session to draw
  // (ApplyStats writes it next to LiveStatsGroup).
  winrt::Microsoft::UI::Xaml::Controls::Button foldDoorGlobeRow_{nullptr};
  std::shared_ptr<urnw::ClientContractsSheet> contractsSheet_;
  std::shared_ptr<urnw::SplitRulesSheet> splitRulesSheet_;
  std::shared_ptr<urnw::DnsEditorSheet> dnsSheet_;
  std::shared_ptr<urnw::TransportSettingsSheet> transportSheet_;
  std::shared_ptr<urnw::LocationChooserSheet> locationSheet_;
  std::shared_ptr<urnw::ProviderLocationsSheet> providerLocationsSheet_;
};

}  // namespace urnw
