// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"regexp"
	"strings"
	"testing"
)

// The provider-only device's follow-up wiring (support inbox 1521): what the
// app shows for it while disconnected (get_provider_stats, and its provider
// status read on the api), the network changes it is told about, and the
// network-space saves that rebuild it. The pure parts run in
// provide-protocol-tests.cpp, provider-status-tests.cpp and
// network-change-notify-tests.cpp; these read the service and app sources,
// which need Windows and WinRT, with comments stripped so prose cannot satisfy
// a contract.

// Each needle must be absent from text.
func requireNone(t *testing.T, where, text string, needles ...string) {
	t.Helper()
	for _, needle := range needles {
		if strings.Contains(text, needle) {
			t.Errorf("%s must not contain %q", where, needle)
		}
	}
}

// get_provider_stats is answered from what the build and the peers listener
// left and the device's own ContractViewController, under a lock of its own:
// never the session lock, never a call into the device, never a state push.
func TestProviderOnlyStatsServiceWiring(t *testing.T) {
	source := tunnelControllerSource(t)
	stats := definitionBody(t, "TunnelController.cpp", source,
		"proto::ProviderStats TunnelController::ProviderStats()")
	provideRequire(t, "ProviderStats", stats,
		"std::scoped_lock lock(providerStatsMutex_);",
		"if (!providerStatsVc_) return stats;",
		"stats.available = true;",
		"stats.client_id = providerClientId_;",
		"providerPeers_->Load()",
		"providerStatsVc_->getProviderThroughputPoints()",
		"providerStatsVc_->getProviderTransportDistribution()",
		"providerStatsVc_->getProviderPacketStats()",
		"providerStatsVc_->getWindowDurationSeconds()")
	requireNone(t, "ProviderStats", stats, "lock(mutex_", "mutex_, std::defer_lock",
		"providerDevice_", "StatusLocked(")

	open := definitionBody(t, "TunnelController.cpp", source,
		"void TunnelController::OpenProviderStatsLocked()")
	provideRequire(t, "OpenProviderStatsLocked", open,
		"providerDevice_->getClientId()",
		"providerDevice_->openContractViewController()",
		"std::scoped_lock lock(providerStatsMutex_);",
		"providerStatsVc_ = std::move(vc);",
		"providerPeersSub_ = std::move(peersSub);")
	// subscribed before the first read, so no change is lost in between
	provideRequireOrder(t, "OpenProviderStatsLocked", open,
		"providerDevice_->addNetworkPeersChangeListener(", "providerDevice_->getNetworkPeers()")
	// the listener holds a share of the peers, never the controller
	listener := open[strings.Index(open, "addNetworkPeersChangeListener("):]
	if end := strings.Index(listener, "});"); end >= 0 {
		listener = listener[:end]
	}
	provideRequire(t, "the peers listener", listener, "[peers]")
	requireNone(t, "the peers listener", listener, "this")

	start := definitionBody(t, "TunnelController.cpp", source,
		"bool TunnelController::StartProvider(")
	built := start[strings.Index(start, "RetireProviderDeviceLocked();"):]
	provideRequireOrder(t, "StartProvider", built,
		"providerDevice_->setProvideControlMode(request.provide_mode);", "OpenProviderStatsLocked();")
	provideRequireOrder(t, "StartProvider", built, "OpenProviderStatsLocked();",
		"WatchProviderNetworkLocked();")
	provideRequireOrder(t, "StartProvider", built, "WatchProviderNetworkLocked();", "return true;")

	compose := definitionBody(t, "TunnelController.cpp", source,
		"proto::TunnelStatus TunnelController::ComposeStatusLocked()")
	requireNone(t, "ComposeStatusLocked", compose, "providerStatsVc_", "providerPeers_")

	server := stripComments(readServiceSource(t, "ControlServer.cpp"))
	handle := definitionBody(t, "ControlServer.cpp", server, "nlohmann::json ControlServer::Handle(")
	at := strings.Index(handle, "proto::msg::kGetProviderStats")
	if at < 0 {
		t.Fatal("ControlServer::Handle does not answer get_provider_stats")
	}
	branch := handle[at:]
	if end := strings.Index(branch, "} else {"); end >= 0 {
		branch = branch[:end]
	}
	provideRequire(t, "the get_provider_stats branch", branch,
		"reply.ok = true;", "reply.provider_stats = tunnel_.ProviderStats();")
	requireNone(t, "the get_provider_stats branch", branch, "PushState()", "tunnel_.Status()")
}

// Every retire takes the statistics and the network watch with the device:
// the statistics leave the lock that get_provider_stats reads under, the
// watch's handlers are dropped on the calling thread, and the bounded worker
// ends the watch and the notifier before it closes the controller and then the
// device, which must outlive the notifier's calls into it.
func TestProviderOnlyRetireTakesStatsAndWatch(t *testing.T) {
	source := tunnelControllerSource(t)
	retire := definitionBody(t, "TunnelController.cpp", source,
		"void TunnelController::RetireProviderDeviceLocked()")
	provideRequire(t, "RetireProviderDeviceLocked", retire,
		"std::scoped_lock lock(providerStatsMutex_);",
		"statsVc = std::move(providerStatsVc_);",
		"peersSub = std::move(providerPeersSub_);",
		"providerEgress_->SetOnNetworkEvent(nullptr);",
		"providerEgress_->SetOnNetworkQualityEvent(nullptr);",
		"auto network = std::move(providerNetwork_);")
	provideRequireOrder(t, "RetireProviderDeviceLocked", retire,
		"statsVc = std::move(providerStatsVc_);", "RunBounded(")
	worker := retire[strings.Index(retire, "RunBounded("):]
	for _, pair := range [][2]string{
		{"egress->Stop();", "network.reset();"},
		{"network.reset();", "peersSub = urnet::Sub{};"},
		{"peersSub = urnet::Sub{};", "device->closeContractViewController(*statsVc);"},
		{"device->closeContractViewController(*statsVc);", "device->close();"},
		{"device->close();", "device.reset();"},
	} {
		provideRequireOrder(t, "the retire worker", worker, pair[0], pair[1])
	}
	// Sub::reset() releases the handle without unsubscribing (PacketPump.cpp)
	requireNone(t, "the retire worker", worker, "peersSub.reset()")
	header := stripComments(readServiceSource(t, "TunnelController.h"))
	provideRequire(t, "TunnelController.h", header,
		"std::unique_ptr<urnet::DeviceLocal> providerDevice_;",
		"std::unique_ptr<EgressMonitor> providerEgress_;",
		"std::unique_ptr<NetworkChangeNotifier> providerNetwork_;",
		"std::mutex providerStatsMutex_;",
		"proto::ProviderStats ProviderStats();")
	// the device outlives the notifier: members die in reverse declaration order
	provideRequireOrder(t, "TunnelController.h", header,
		"providerDevice_;", "providerNetwork_;")
}

// The provider-only device hears about network changes the way the tunnel
// session's device does — an EgressMonitor's events, only for what moved the
// network (EgressChangeFilter), coalesced, then networkChanged() — through a
// monitor that binds nothing, so the device's sockets still follow the route
// table.
func TestProviderOnlyNetworkWatchWiring(t *testing.T) {
	source := tunnelControllerSource(t)
	watch := definitionBody(t, "TunnelController.cpp", source,
		"void TunnelController::WatchProviderNetworkLocked()")
	provideRequire(t, "WatchProviderNetworkLocked", watch,
		"urnet::DeviceLocal* device = providerDevice_.get();",
		"providerNetwork_ = std::make_unique<NetworkChangeNotifier>(",
		"device->networkChanged();",
		"device->networkQualityChanged();",
		"std::make_unique<EgressMonitor>(NET_LUID{}, EgressMonitor::Binding::ObserveOnly)",
		"providerEgress_->SetOnNetworkEvent(providerNetwork_->NetworkEventSink());",
		"providerEgress_->SetOnNetworkQualityEvent(providerNetwork_->NetworkQualitySink());",
		"providerEgress_->Start();")
	requireNone(t, "WatchProviderNetworkLocked", watch,
		"setEgressInterfaceIndex(", "EgressMonitor::Binding::Bind", "SetOnChange(",
		"deadTunnelWatchdog_")
	// the tunnel's own monitor still binds
	startLocked := definitionBody(t, "TunnelController.cpp", source,
		"proto::TunnelStatus TunnelController::StartLocked(")
	provideRequire(t, "StartLocked", startLocked,
		"egress_ = std::make_unique<EgressMonitor>(egressExcludeLuid);")

	monitor := stripComments(readServiceSource(t, "EgressMonitor.cpp"))
	update := definitionBody(t, "EgressMonitor.cpp", monitor, "void EgressMonitor::Update(Reading reading)")
	observe := strings.Index(update, "if (binding_ == Binding::ObserveOnly) {")
	bind := strings.Index(update, "urnet::setEgressInterfaceIndex(")
	if observe < 0 || bind < 0 || bind < observe {
		t.Fatal("EgressMonitor::Update must handle an observe-only monitor before it binds")
	}
	// the branch, through the brace that closes it
	branch := update[observe:]
	if end := strings.Index(branch, "\n  }\n"); end >= 0 {
		branch = branch[:end]
	}
	// it reads the election only to tell a network that moved from churn, and
	// reports only that; it never binds
	if !strings.Contains(branch, "return;") || !strings.Contains(branch, "onNetworkEvent_") ||
		!strings.Contains(branch, "filter_.Seed(signature);") ||
		!strings.Contains(branch, "moved = filter_.Observe(signature).network;") ||
		!strings.Contains(branch, "if (moved && observed) observed();") ||
		strings.Contains(branch, "setEgressInterfaceIndex(") || strings.Contains(branch, "onChange_") {
		t.Error("an observe-only EgressMonitor must report only a network that moved, and return without binding")
	}
	startBody := definitionBody(t, "EgressMonitor.cpp", monitor, "bool EgressMonitor::Start()")
	// the baseline, in both modes, before any notification can arrive
	provideRequireOrder(t, "EgressMonitor::Start", startBody, "Update(Reading::Baseline);",
		"::NotifyIpInterfaceChange(")
	requireNone(t, "EgressMonitor::Start", startBody, "Refresh();", "Reading::Observation")

	// one copy of the coalescing rule, which the tunnel watchdog shares
	watchdog := stripComments(readServiceSource(t, "TunnelWatchdog.h"))
	provideRequire(t, "TunnelWatchdog.h", watchdog, `#include "NetworkChangeNotify.h"`)
	requireNone(t, "TunnelWatchdog.h", watchdog, "class NotifyCoalescer",
		"struct NetworkNotifySettings", "windowMillis = ")
	notify := readServiceSource(t, "NetworkChangeNotify.h")
	// the window is a setting with its default, never a constant
	provideRequire(t, "NetworkChangeNotify.h", notify, "class NotifyCoalescer",
		"class NetworkChangeNotifier", "struct NetworkNotifySettings {", "int64_t windowMillis = 750;",
		"deadlineMillis_ = nowMillis + windowMillis_;")
	requireNone(t, "NetworkChangeNotify.h", notify, "kNetworkNotifyDebounceMillis")
	if regexp.MustCompile(`#include\s*<(windows|winsock2)\.h>|#include\s*"(Sdk|Log)\.h"`).MatchString(notify) {
		t.Error("NetworkChangeNotify.h must stay portable: its harness runs off Windows")
	}
	project := readServiceSource(t, "Service.vcxproj")
	provideRequire(t, "Service.vcxproj", project, `<ClInclude Include="NetworkChangeNotify.h" />`)
}

// The app asks get_provider_stats off the UI thread, outside the session lock,
// only with no session and a window to show it in, and feeds what a session's
// DeviceRemote feeds: the Earnings provider plots and their gate, the "no
// traffic yet" line's window bytes, and the Connect page's count.
func TestProviderOnlyStatsAppWiring(t *testing.T) {
	client := stripComments(readAppSource(t, "ServiceClient.cpp"))
	get := definitionBody(t, "ServiceClient.cpp", client, "bool ServiceClient::GetProviderStats(")
	provideRequire(t, "ServiceClient::GetProviderStats", get,
		"proto::msg::kGetProviderStats", "if (!r.ok || !r.provider_stats) return false;")

	source := sdkHostSource(t)
	loop := definitionBody(t, "SdkHost.cpp", source, "void SdkHost::ProviderOnlyStatsLoop()")
	provideRequire(t, "ProviderOnlyStatsLoop", loop,
		"presenting && !HasSession() && serviceProviderRunning_.load() &&",
		"service_.GetProviderStats(stats) && stats.available",
		"if (device_) {",
		"ShowProviderOnlyStatsLocked(stats);",
		"ClearProviderOnlyStatsLocked(providerGone);",
		"FetchProviderOnlyStatusLocked(stats.client_id);",
		"ProviderOnlyStatusUnavailable();",
		"PublishStats();")
	// the pipe call waits behind a start_tunnel: never under the session lock
	provideRequireOrder(t, "ProviderOnlyStatsLoop", loop,
		"service_.GetProviderStats(stats)", "std::scoped_lock lock(mutex_);")
	if strings.Count(loop, "service_.GetProviderStats(") != 1 {
		t.Error("ProviderOnlyStatsLoop must ask the service exactly once per pass")
	}

	show := definitionBody(t, "SdkHost.cpp", source,
		"void SdkHost::ShowProviderOnlyStatsLocked(const proto::ProviderStats& stats)")
	provideRequire(t, "ShowProviderOnlyStatsLocked", show,
		"serviceProviderClients_.store(stats.client_count);",
		"proto::ProviderPointsOf<urnet::ThroughputPoint>(stats)",
		"proto::ProviderDistributionOf<urnet::TransportDistribution>(stats)",
		"provider.hasProviderStats = stats.has_provider_stats;",
		"lastProviderPoints_ = provider.providerPoints;",
		"onProviderThroughput_(std::move(provider));")
	clear := definitionBody(t, "SdkHost.cpp", source,
		"void SdkHost::ClearProviderOnlyStatsLocked(bool providerGone)")
	provideRequire(t, "ClearProviderOnlyStatsLocked", clear,
		"if (providerGone) serviceProviderClients_.store(-1);",
		"lastProviderPoints_.clear();",
		"if (providerGone) empty.hasProviderStats = false;",
		"onProviderThroughput_(std::move(empty));")

	initialize := definitionBody(t, "SdkHost.cpp", source, "bool SdkHost::Initialize()")
	provideRequire(t, "Initialize", initialize,
		"providerOnlyThread_ = std::thread([this] { ProviderOnlyStatsLoop(); });")
	destructor := definitionBody(t, "SdkHost.cpp", source, "SdkHost::~SdkHost()")
	provideRequireOrder(t, "~SdkHost", destructor, "StopProviderOnlyStats();",
		"std::scoped_lock lock(mutex_);")
	presentation := definitionBody(t, "SdkHost.cpp", source,
		"void SdkHost::SetPresentationActive(bool active)")
	provideRequireOrder(t, "SetPresentationActive", presentation,
		"presentationDesired_ = active;", "KickProviderOnlyStats();")
	// no way out between recording the change and the kick
	if recorded, kick := strings.Index(presentation, "presentationDesired_ = active;"),
		strings.Index(presentation, "KickProviderOnlyStats();"); recorded >= 0 && kick > recorded &&
		strings.Contains(presentation[recorded:kick], "return") {
		t.Error("SetPresentationActive must kick the statistics on every recorded change")
	}
	adopt := definitionBody(t, "SdkHost.cpp", source,
		"void SdkHost::AdoptServiceFacts(const proto::TunnelStatus& st)")
	provideRequire(t, "AdoptServiceFacts", adopt,
		"if (!st.provider_running) serviceProviderClients_.store(-1);",
		"if (providerWasRunning != st.provider_running) KickProviderOnlyStats();")
	for _, name := range []string{"void SdkHost::OnServiceDisconnected()", "void SdkHost::SignOutTailLocked()"} {
		provideRequire(t, name, definitionBody(t, "SdkHost.cpp", source, name),
			"serviceProviderClients_.store(-1);")
	}
}

// With no session the Earnings provider status comes from the same
// GET /network/provider-status the controller polls, read on the api for the
// client the service names, applied as the controller applies a poll, and
// polled on the controller's terms.
func TestProviderOnlyStatusWiring(t *testing.T) {
	source := sdkHostSource(t)
	fetch := definitionBody(t, "SdkHost.cpp", source,
		"void SdkHost::FetchProviderOnlyStatusLocked(const std::string& clientId)")
	provideRequire(t, "FetchProviderOnlyStatusLocked", fetch,
		"api_->getProviderStatus(",
		"if (generation != providerOnlyStatusGeneration_) return;",
		"providerOnlyStatus_.Fetched(result, error, clientId);",
		"onProviderOnlyStatus_(std::move(status));")
	wanted := definitionBody(t, "SdkHost.cpp", source,
		"void SdkHost::SetProviderOnlyStatusWanted(bool wanted)")
	provideRequire(t, "SetProviderOnlyStatusWanted", wanted,
		"if (!wanted) ++providerOnlyStatusGeneration_;", "providerOnlyKick_ = true;")
	requireNone(t, "SetProviderOnlyStatusWanted", wanted, "mutex_)")
	header := stripComments(readAppSource(t, "SdkHost.h"))
	provideRequire(t, "SdkHost.h", header,
		"using ProviderOnlyStatus = providerstatus::Readings<urnet::ProviderStatus>;",
		"kProviderOnlyStatusInterval{60}")

	wallet := stripLineComments(readAppSource(t, "WalletPage.cpp"))
	reconcile := definitionBody(t, "WalletPage.cpp", wallet, "void WalletPage::ReconcileProviderStatus() {")
	provideRequire(t, "WalletPage::ReconcileProviderStatus", reconcile,
		"const bool providerOnly = !device && CanCallApi() && provideStateKnown_ && providingEnabled_;",
		"TakeProviderOnlyReadings(Sdk().CurrentProviderOnlyStatus());",
		"Sdk().SetProviderOnlyStatusWanted(providerOnly && selected_ && presentationActive_);")
	apply := definitionBody(t, "WalletPage.cpp", wallet,
		"void WalletPage::ApplyProviderOnlyStatus(urnw::ProviderOnlyStatus const& status) {")
	provideRequireOrder(t, "WalletPage::ApplyProviderOnlyStatus", apply,
		"if (!providerOnlySource_) return;", "ApplyProviderStatus();")
	take := definitionBody(t, "WalletPage.cpp", wallet,
		"void WalletPage::TakeProviderOnlyReadings(urnw::ProviderOnlyStatus const& status) {")
	provideRequire(t, "WalletPage::TakeProviderOnlyReadings", take,
		"providerStatusLoaded_ = status.loaded;", "providerStatusError_ = status.error;",
		"providerStatus_ = status.status;")
	initialize := definitionBody(t, "WalletPage.cpp", wallet, "void WalletPage::Initialize() {")
	provideRequire(t, "WalletPage::Initialize", initialize,
		"Sdk().SetProviderOnlyStatusHandler(", "queue.TryEnqueue(",
		"self->wallet().ApplyProviderOnlyStatus(status);")
	provideRequire(t, "WalletPage::~WalletPage",
		definitionBody(t, "WalletPage.cpp", wallet, "WalletPage::~WalletPage() {"),
		"Sdk().SetProviderOnlyStatusWanted(false);")
}

// Saving a value of the network space the provider-only device was built from
// rebuilds it: with no session every writer queues the reconcile, whose
// start_provider carries the saved space, and the service builds a new device
// for a changed request. A writer added later without it fails here.
func TestProviderOnlyRebuiltOnNetworkSpaceSave(t *testing.T) {
	source := sdkHostSource(t)
	for _, writer := range []struct{ signature, write, reason string }{
		{signature: "std::optional<std::string> SdkHost::SetControlDohUrls(", write: "networkSpace_->setControlDohUrls(",
			reason: `if (!device_) RequestProviderReconcile("bootstrap doh servers saved");`},
		{signature: "std::optional<std::string> SdkHost::SetVlessSettings(", write: "networkSpace_->setVlessSettings(",
			reason: `if (!device_) RequestProviderReconcile("vless settings saved");`},
		{signature: "bool SdkHost::SetNetExtender(", write: "spaceManager_->updateNetworkSpaceValues(key, values);",
			reason: `if (!device_) RequestProviderReconcile("private extender saved");`},
	} {
		body := definitionBody(t, "SdkHost.cpp", source, writer.signature)
		provideRequireOrder(t, writer.signature, body, writer.write, writer.reason)
	}
	// a save that the setter refused changed nothing, and rebuilds nothing
	for _, signature := range []string{"std::optional<std::string> SdkHost::SetControlDohUrls(",
		"std::optional<std::string> SdkHost::SetVlessSettings("} {
		body := definitionBody(t, "SdkHost.cpp", source, signature)
		provideRequireOrder(t, signature, body, "if (errorId.empty()) {", "RequestProviderReconcile(")
	}

	// Every function that writes the space reconciles, or is the launch's
	// writer, which Initialize follows with the reconcile itself.
	definitions := regexp.MustCompile(`(?m)^[^\s#/][^\n;]*\bSdkHost::(\w+)\([^;{]*\{\s*$`)
	matches := definitions.FindAllStringSubmatchIndex(source, -1)
	writers := 0
	for i, match := range matches {
		end := len(source)
		if i+1 < len(matches) {
			end = matches[i+1][0]
		}
		body := source[match[0]:end]
		name := source[match[2]:match[3]]
		if !regexp.MustCompile(`->setControlDohUrls\(|->setVlessSettings\(|updateNetworkSpaceValues\(`).MatchString(body) {
			continue
		}
		writers++
		if name == "BuildNetworkSpace" {
			continue
		}
		if !strings.Contains(body, "RequestProviderReconcile(") && !strings.Contains(body, "EnsureSession(") {
			t.Errorf("SdkHost::%s writes the network space but never rebuilds the provider-only device "+
				"(RequestProviderReconcile)", name)
		}
	}
	if writers < 5 {
		t.Fatalf("found only %d network-space writers in SdkHost.cpp; update this contract", writers)
	}
	initialize := definitionBody(t, "SdkHost.cpp", source, "bool SdkHost::Initialize()")
	provideRequireOrder(t, "Initialize", initialize, "BuildNetworkSpace()", `EnsureSession("resume");`)
	provideRequireOrder(t, "Initialize", initialize, "BuildNetworkSpace()",
		`RequestProviderReconcile("launch, signed out");`)
}
