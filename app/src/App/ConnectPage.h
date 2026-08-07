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

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>

#include "LocationSheets.h"
#include "SdkHost.h"
#include "StatsSheets.h"
#include "TransferChart.h"

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

  // ---- window-level relays ----
  void ApplyStats(urnw::LiveStats const& stats);
  void SetConnectedUi(bool connected);
  // network name off the stored jwt, for the idle "{name} is ready to connect"
  // copy; re-renders the status line
  void SetNetworkIdentity(std::string const& networkName, bool guestMode);
  void ResyncDrawer();     // seed the caches/cards from SdkHost snapshots
  void AnimateDrawerIn();  // fade + slide-up entrance, staggered across cards

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

 private:
  // The SDK's own connection status (ConnectViewController.getConnectionStatus,
  // surfaced as LiveStats.connectionStatus). Mirrors android's
  // ui/shared/models/ConnectStatus. This is a DIFFERENT signal from the service
  // tunnel state that drives connected_: the tunnel says whether packets can
  // flow, this says what the connect controller is doing about it, and only this
  // one has a "connecting" value to show.
  enum class ConnectStatus { Disconnected, Connecting, DestinationSet, Connected };

  // Status line + status dot + connect button, from connectStatus_ (the SDK) and
  // connected_ (the service tunnel). The single place any of the three is written.
  void ApplyConnectStatus();
  static ConnectStatus ParseConnectStatus(std::string const& value);
  // What the connect button does right now, from the SDK status only (NOT the
  // tunnel — see the definition): anything other than a settled disconnected
  // state means the press disconnects, and in a transition aborts.
  bool ConnectActionIsDisconnect() const;

  void BuildCharts();
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
  void ApplySplitRuleCount();
  void ApplyBlockerUi(bool on);
  void OnChartTick();
  winrt::fire_and_forget ShowClientContractsSheet();
  winrt::fire_and_forget ShowSplitRulesSheet();
  winrt::fire_and_forget ShowDnsSheet();
  winrt::fire_and_forget ShowLocationChooserSheet();
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
  // network name off the stored jwt, for the idle "{name} is ready to connect"
  // copy. Read once per auth change, not per stats push (ParsedJwt re-parses).
  std::string networkName_;
  bool guestMode_ = false;

  // drawer state (UI thread only)
  std::unique_ptr<urnw::TransferChart> remoteChart_;
  std::unique_ptr<urnw::TransferChart> blockedChart_;
  std::unique_ptr<urnw::TransferChart> localChart_;
  winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer chartTimer_{nullptr};
  uint32_t chartTickCount_ = 0;
  std::vector<urnw::ContractPeerRow> contractRows_;
  std::vector<urnw::BlockActionItem> blockActions_;
  std::vector<urnw::SplitRule> splitRules_;
  int64_t allowedCount_ = 0;
  int64_t blockedCount_ = 0;
  std::optional<urnet::DnsResolverSettings> dnsSettings_;
  std::string countryCode_;  // selected location country (dns recommendations)
  std::string countryName_;
  bool updatingControls_ = false;  // guards programmatic toggle/segment updates
  bool drawerAnimated_ = false;    // entrance plays once per window
  std::shared_ptr<urnw::ClientContractsSheet> contractsSheet_;
  std::shared_ptr<urnw::SplitRulesSheet> splitRulesSheet_;
  std::shared_ptr<urnw::DnsEditorSheet> dnsSheet_;
  std::shared_ptr<urnw::LocationChooserSheet> locationSheet_;
};

}  // namespace urnw
