// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"strings"
	"testing"
)

// The network peers while disconnected (owner report 2026-11-02: Peer
// Discovery showed no devices on Windows while an iPhone on the same account
// listed its own devices). With no session the app holds no DeviceRemote and
// so no PeerViewController, and get_provider_stats carried only the count of
// the peers the service's provider-only device discovers. It carries the peers
// now (ProviderStats::network_peers), and the app fills the lists a session's
// controller fills: the Connect page's network peers, the chooser's and the
// Network destination's. The pure parts run in provide-protocol-tests.cpp and
// peer-location-tests.cpp; these read the service and app sources, which need
// Windows and WinRT, with comments stripped so prose cannot satisfy a
// contract.

// The service answers with the peers its listener pushed last and the count of
// that same list; the listener and the statistics' opening keep the list, and
// the retire drops it.
func TestProviderOnlyPeersServiceWiring(t *testing.T) {
	source := tunnelControllerSource(t)
	stats := definitionBody(t, "TunnelController.cpp", source,
		"proto::ProviderStats TunnelController::ProviderStats()")
	provideRequireOrder(t, "ProviderStats", stats,
		"const urnet::NetworkPeerList connected = providerPeers_->Load();",
		"stats.client_count = static_cast<int64_t>(connected.size());")
	provideRequireOrder(t, "ProviderStats", stats,
		"stats.client_count = static_cast<int64_t>(connected.size());",
		"stats.network_peers = connected;")

	open := definitionBody(t, "TunnelController.cpp", source,
		"void TunnelController::OpenProviderStatsLocked()")
	listener := handlerSource(t, "TunnelController.cpp", open,
		"providerDevice_->addNetworkPeersChangeListener(")
	provideRequire(t, "the peers listener", listener,
		"peers->Store(ConnectedPeers(std::move(networkPeers)));")
	provideRequireOrder(t, "OpenProviderStatsLocked", open,
		"peers->Store(ConnectedPeers(providerDevice_->getNetworkPeers()));",
		"providerPeers_ = std::move(peers);")

	retire := definitionBody(t, "TunnelController.cpp", source,
		"void TunnelController::RetireProviderDeviceLocked()")
	provideRequire(t, "RetireProviderDeviceLocked", retire, "providerPeers_.reset();")

	// every signed-in user of the machine can ask the control pipe: a peer goes
	// out with what the rows read and connect with, and nothing else
	rows := definitionBody(t, "TunnelController.cpp", source,
		"urnet::NetworkPeerList ConnectedPeers(std::optional<urnet::NetworkPeers> peers)")
	provideRequire(t, "ConnectedPeers", rows, "row.ClientId = ", "row.ProvideEnabled = ",
		"row.DeviceName = ", "row.DeviceSpec = ")
	requireNone(t, "ConnectedPeers", rows, "Principal", "Roles", "rows = std::move(*peers->Connected)",
		"return std::move(*peers->Connected)")
}

// With no session the app pushes the device's connectable peers to the peers
// feed, only when the rows change, and hands the feed back to a session's
// controller; a provider that stops takes its rows with it, and an emptied feed
// makes the next answer push again. The line under the connect button counts
// the provider-only device's peers instead of saying discovery is disabled.
func TestProviderOnlyPeersAppWiring(t *testing.T) {
	host := sdkHostSource(t)
	show := definitionBody(t, "SdkHost.cpp", host,
		"void SdkHost::ShowProviderOnlyStatsLocked(const proto::ProviderStats& stats)")
	provideRequire(t, "ShowProviderOnlyStatsLocked", show,
		"proto::NetworkPeersOf<urnet::NetworkPeerList>(stats)",
		"PublishProviderOnlyPeersLocked(ConnectablePeers(std::move(*peers)));")

	publish := definitionBody(t, "SdkHost.cpp", host,
		"void SdkHost::PublishProviderOnlyPeersLocked(urnet::NetworkPeerList peers)")
	provideRequireOrder(t, "PublishProviderOnlyPeersLocked", publish,
		"SamePeerRows(*lastProviderOnlyPeers_, peers)) return;", "lastProviderOnlyPeers_ = peers;")
	// both consumers, as a session's controller pushes them, and after the
	// drawer lock is released: the handlers marshal to the UI thread
	provideRequireOrder(t, "PublishProviderOnlyPeersLocked", publish,
		"lastProviderOnlyPeers_ = peers;\n  }", "if (onPeersObserver_) onPeersObserver_(peers);")
	provideRequireOrder(t, "PublishProviderOnlyPeersLocked", publish,
		"if (onPeersObserver_) onPeersObserver_(peers);", "if (onPeers_) onPeers_(std::move(peers));")

	loop := definitionBody(t, "SdkHost.cpp", host, "void SdkHost::ProviderOnlyStatsLoop()")
	at := strings.Index(loop, "if (device_) {")
	if at < 0 {
		t.Fatal("ProviderOnlyStatsLoop no longer hands a session the feeds; update this contract")
	}
	session := loop[at:]
	if end := strings.Index(session, "continue;"); end >= 0 {
		session = session[:end]
	}
	provideRequire(t, "ProviderOnlyStatsLoop's session branch", session, "ForgetProviderOnlyPeers();")
	requireNone(t, "ProviderOnlyStatsLoop's session branch", session, "PublishProviderOnlyPeersLocked(")

	clear := definitionBody(t, "SdkHost.cpp", host,
		"void SdkHost::ClearProviderOnlyStatsLocked(bool providerGone)")
	provideRequireOrder(t, "ClearProviderOnlyStatsLocked", clear, "if (providerGone) {",
		"if (pushed) PublishProviderOnlyPeersLocked({});")
	provideRequireOrder(t, "ClearProviderOnlyStatsLocked", clear,
		"if (pushed) PublishProviderOnlyPeersLocked({});", "ForgetProviderOnlyPeers();")

	drawer := definitionBody(t, "SdkHost.cpp", host, "void SdkHost::ClearDrawer(bool sessionEnding)")
	provideRequireOrder(t, "ClearDrawer", drawer, "lastProviderOnlyPeers_.reset();",
		"if (onPeers_) onPeers_(std::nullopt);")

	fill := definitionBody(t, "SdkHost.cpp", host,
		"void SdkHost::FillProviderOnlyStats(LiveStats& stats) const")
	provideRequireOrder(t, "FillProviderOnlyStats", fill, "if (0 <= clients) {",
		"stats.connectedPeerCount = clients;")
	provideRequire(t, "FillProviderOnlyStats", fill, "stats.providerOnlyPeers = true;")

	page := stripComments(readAppSource(t, "ConnectPage.cpp"))
	count := definitionBody(t, "ConnectPage.cpp", page, "void ConnectPage::ApplyPeerCount(")
	provideRequire(t, "ApplyPeerCount", count, "if (!PeerDiscoveryLive(peerLine_)) {")
	requireNone(t, "ApplyPeerCount", count, "if (!peerLine_.remoteConnected)")
	list := definitionBody(t, "ConnectPage.cpp", page, "void ConnectPage::ApplyPeersList()")
	provideRequire(t, "ApplyPeersList", list, "PeerDiscoveryLive(peerLine_)",
		"urnw::Plural(\"network_peer_count\", peerLine_.connectedPeerCount)")
	header := stripComments(readAppSource(t, "ConnectPage.h"))
	provideRequire(t, "ConnectPage.h", header,
		"return reading.remoteConnected || reading.providerOnlyPeers;")
}

// A reply that loses the peers, on either side of the wire, leaves the lists
// empty while disconnected; an older service's silence read as "no peers
// connected" would empty lists it has no reading for.
func TestProvideProtocolRejectsDroppedNetworkPeers(t *testing.T) {
	extra := provideJsonFlags(t)
	requireProvideFailure(t, "provide-protocol-tests.cpp", map[string]func(string) string{
		"Protocol.h": func(source string) string {
			return strings.Replace(source,
				"  if (auto it = j.find(\"network_peers\"); it != j.end() && it->is_array())\n    v.network_peers = *it;\n",
				"", 1)
		},
	}, "peers: the connected peers come back", extra...)
	requireProvideFailure(t, "provide-protocol-tests.cpp", map[string]func(string) string{
		"Protocol.h": func(source string) string {
			return strings.Replace(source, "      {\"network_peers\", v.network_peers},\n", "", 1)
		},
	}, "peers: the connected peers come back", extra...)
	requireProvideFailure(t, "provide-protocol-tests.cpp", map[string]func(string) string{
		"Protocol.h": func(source string) string {
			return strings.Replace(source,
				"  if (!stats.network_peers.is_array()) return std::nullopt;\n",
				"  if (stats.network_peers.is_null()) return PeerList{};\n", 1)
		},
	}, "peers: no list reads as none, not as no peers", extra...)
}

// The lists hold what a session's PeerViewController holds: a peer that does
// not provide is no row. And a peer whose name changed is pushed.
func TestProviderOnlyPeersRejectsUnfilteredOrStaleRows(t *testing.T) {
	requirePeerLocationFailure(t, func(source string) string {
		return replaceStatsPublishOnce(t, source,
			"    if (peer.ProvideEnabled) connectable.push_back(std::move(peer));\n",
			"    connectable.push_back(std::move(peer));\n")
	}, "a peer that does not provide is listed")
	requirePeerLocationFailure(t, func(source string) string {
		return replaceStatsPublishOnce(t, source,
			"        a[i].DeviceName != b[i].DeviceName || a[i].DeviceSpec != b[i].DeviceSpec) {",
			"        a[i].DeviceSpec != b[i].DeviceSpec) {")
	}, "a renamed peer is not pushed")
}
