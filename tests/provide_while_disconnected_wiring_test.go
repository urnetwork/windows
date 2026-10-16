// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"strings"
	"testing"
)

// The call sites of the provider-only device (support inbox 1521). The
// lifecycle is pure and runs in provide_lifecycle_test.go; what keeps a
// Windows provider earning while disconnected, and keeps its device from ever
// touching this machine's routes, DNS or firewall, lives in the service and in
// SdkHost, which need Windows and WinRT. So these read their sources, comments
// stripped so prose cannot satisfy a contract: a decision with no caller is the
// defect this repo keeps relearning.

// Each needle must occur in text.
func provideRequire(t *testing.T, where, text string, needles ...string) {
	t.Helper()
	for _, needle := range needles {
		if !strings.Contains(text, needle) {
			t.Errorf("%s is missing %q", where, needle)
		}
	}
}

// first must occur in text, and before second, which must occur too.
func provideRequireOrder(t *testing.T, where, text, first, second string) {
	t.Helper()
	a := strings.Index(text, first)
	b := strings.Index(text, second)
	if a < 0 || b < 0 || a >= b {
		t.Errorf("%s must have %q before %q", where, first, second)
	}
}

// The service's TunnelController.cpp, comments stripped.
func tunnelControllerSource(t *testing.T) string {
	return stripComments(readServiceSource(t, "TunnelController.cpp"))
}

// The app's SdkHost.cpp, comments stripped.
func sdkHostSource(t *testing.T) string {
	return stripComments(readAppSource(t, "SdkHost.cpp"))
}

// The provider-only device is steps 3 and 4 of a bring-up and nothing after
// them: no adapter, routes, DNS, firewall, marker, pump, split tunnel, egress
// binding or device RPC listener — and its own slot, so device_ keeps meaning
// the tunnel session's device everywhere else.
func TestProvideWiringProviderTouchesNoMachineState(t *testing.T) {
	source := tunnelControllerSource(t)
	start := definitionBody(t, "TunnelController.cpp", source,
		"bool TunnelController::StartProvider(")
	provideRequire(t, "StartProvider", start,
		"providerSpace_ = ImportNetworkSpaceLocked(request.network_space_json);",
		"providerDevice_ = std::make_unique<urnet::DeviceLocal>(",
		"NewDeviceLocked(*providerSpace_",
		"providerDevice_->setProviderTransportSettings(",
		"providerDevice_->setProvideControlMode(request.provide_mode);",
		"PublishStatusLocked();")
	for _, forbidden := range []string{
		"Wintun::Load(", "WintunAdapter::Create(", "std::make_unique<NetworkConfig>",
		"ApplyWfpLocked(", "wfp_.Apply(", "setRpcServer(", "std::make_unique<EgressMonitor>",
		"std::make_unique<PacketPump>", "SetActiveMarker(", "splitTunnel_.",
		"PushExcludedToDriver(", "setFlowOwnerLookup(", "setEgressInterfaceIndex(",
		"trace_.Start(", "deadTunnelWatchdog_.Start(", "SetStateLocked(", " device_ = ",
		"networkSpace_ = ",
	} {
		if strings.Contains(start, forbidden) {
			t.Errorf("StartProvider must not call %q: the provider-only device changes "+
				"nothing on this machine", forbidden)
		}
	}
}

// Every refusal is decided with the shared helper, before anything is retired
// or built: a tunnel session, the armed floor, a held device, a restart.
func TestProvideWiringServiceRefusesBeforeBuilding(t *testing.T) {
	start := definitionBody(t, "TunnelController.cpp", tunnelControllerSource(t),
		"bool TunnelController::StartProvider(")
	provideRequire(t, "StartProvider", start,
		"lock.try_lock_for(kStopLockBudget)",
		"proto::IsSessionLive(state_)",
		"device_.has_value()",
		"state.firewallInForce = wfp_.State() != WfpState::Off;",
		"SweepAbandonedTeardowns()",
		"state.deviceStillHeld = abandoned.outstanding > 0;",
		"state.restartPending = SelfRestartPending();",
		"provide::RefusalReason(refusal)")
	provideRequireOrder(t, "StartProvider", start,
		"provide::ProviderStartRefusal(mode, state)", "RetireProviderDeviceLocked();")
	provideRequireOrder(t, "StartProvider", start,
		"provide::ProviderStartRefusal(mode, state)", "NewDeviceLocked(")
	// the same request keeps the device and only applies the mode
	provideRequireOrder(t, "StartProvider", start,
		"proto::SameProviderDevice(providerRequest_, request)", "RetireProviderDeviceLocked();")
}

// Every teardown — and so the head of every bring-up, which opens with one —
// retires the provider-only device, after the machine is given back and before
// the session's own device (the same identity) exists. Both devices come from
// one copy of the identity rules.
func TestProvideWiringEveryTeardownRetiresTheProvider(t *testing.T) {
	source := tunnelControllerSource(t)
	stop := definitionBody(t, "TunnelController.cpp", source,
		"void TunnelController::StopLocked(bool finalDisarm)")
	provideRequireOrder(t, "StopLocked", stop,
		"RevertMachineStateLocked(finalDisarm, hadRoutes);", "RetireProviderDeviceLocked();")
	provideRequireOrder(t, "StopLocked", stop,
		"RetireProviderDeviceLocked();", "TearDownSessionLocked();")
	startLocked := definitionBody(t, "TunnelController.cpp", source,
		"proto::TunnelStatus TunnelController::StartLocked(")
	provideRequireOrder(t, "StartLocked", startLocked, "StopLocked(", "device_ = NewDeviceLocked(")
	provideRequire(t, "StartLocked", startLocked,
		"networkSpace_ = ImportNetworkSpaceLocked(config.network_space_json);")
	if strings.Contains(startLocked, "newDeviceLocalWithMemoryTarget(") {
		t.Error("StartLocked builds its DeviceLocal directly instead of through NewDeviceLocked: " +
			"the tunnel and the provider-only device must share one copy of the identity rules")
	}
	retire := definitionBody(t, "TunnelController.cpp", source,
		"void TunnelController::RetireProviderDeviceLocked()")
	provideRequire(t, "RetireProviderDeviceLocked", retire,
		"providerDevice_.reset();", "device->close();", "AbandonHazard::HoldsSessionDevice")
	provideRequireOrder(t, "RetireProviderDeviceLocked", retire, "PublishStatusLocked();", "RunBounded(")
	logout := definitionBody(t, "TunnelController.cpp", source,
		"bool TunnelController::Logout(const std::string& networkSpaceJson)")
	provideRequire(t, "Logout", logout, "StopLocked(")
	stopProvider := definitionBody(t, "TunnelController.cpp", source,
		"bool TunnelController::StopProvider()")
	provideRequire(t, "StopProvider", stopProvider, "RetireProviderDeviceLocked();")
	for _, forbidden := range []string{"StopLocked(", "ApplyWfpLocked(", "wfp_."} {
		if strings.Contains(stopProvider, forbidden) {
			t.Errorf("StopProvider must retire the provider-only device and touch nothing "+
				"else, but calls %q", forbidden)
		}
	}
}

// The status reports the provider from the members that own it, never from an
// SDK call on the publish path; the facts are read only after a build or a
// mode change.
func TestProvideWiringStatusReportsTheProvider(t *testing.T) {
	source := tunnelControllerSource(t)
	compose := definitionBody(t, "TunnelController.cpp", source,
		"proto::TunnelStatus TunnelController::ComposeStatusLocked()")
	provideRequire(t, "ComposeStatusLocked", compose,
		"s.provider_running = providerDevice_ != nullptr;",
		"s.provider_control_mode = providerRequest_.provide_mode;",
		"s.provider_mode = providerTier_;",
		"s.provider_network_key = providerNetworkKey_;")
	if strings.Contains(compose, "providerDevice_->") {
		t.Error("ComposeStatusLocked calls into the provider device: it runs on the teardown " +
			"path and may not re-enter the SDK")
	}
	facts := definitionBody(t, "TunnelController.cpp", source,
		"void TunnelController::ReadProviderFactsLocked()")
	provideRequire(t, "ReadProviderFactsLocked", facts,
		"providerDevice_->getProvideMode()", "providerDevice_->getProvideSecretKeys()",
		"urnet::ProvideModeNetwork")
}

// start_provider and stop_provider reach the controller, the request names the
// device exactly before anything happens, and the new state is pushed.
func TestProvideWiringControlServerVerbs(t *testing.T) {
	server := stripComments(readServiceSource(t, "ControlServer.cpp"))
	handle := definitionBody(t, "ControlServer.cpp", server,
		"nlohmann::json ControlServer::Handle(")
	provideRequireOrder(t, "Handle", handle, "proto::msg::kStartProvider", "tunnel_.StartProvider(req, error)")
	provideRequireOrder(t, "Handle", handle,
		"rpcsession::IsPairableInstanceId(req.instance_id)", "tunnel_.StartProvider(req, error)")
	provideRequireOrder(t, "Handle", handle, "proto::msg::kStopProvider", "tunnel_.StopProvider()")
	at := strings.Index(handle, "proto::msg::kStartProvider")
	if at >= 0 {
		branch := handle[at:]
		if end := strings.Index(branch, "proto::msg::kStopProvider"); end >= 0 {
			branch = branch[:end]
		}
		provideRequire(t, "the start_provider branch", branch,
			"reply.ok = tunnel_.StartProvider(req, error);", "reply.status = tunnel_.Status();",
			"PushState();")
	}
}

// Disconnect keeps providing: every pass of the session worker that leaves no
// session ends with the reconcile — after the Disconnect's stop_tunnel, and
// after a launch, service recovery or network-server change that found nothing
// to reattach to — and a provider request runs it without a gesture.
func TestProvideWiringAppReconcilesAfterDisconnectAndLaunch(t *testing.T) {
	source := sdkHostSource(t)
	worker := definitionBody(t, "SdkHost.cpp", source, "void SdkHost::SessionWorkerLoop()")
	reconcile := "if (!device_) ReconcileProviderLocked(req.reason);"
	provideRequireOrder(t, "SessionWorkerLoop", worker, "service_.StopTunnel();", reconcile)
	provideRequireOrder(t, "SessionWorkerLoop", worker,
		"ok = BootstrapSession(req.reason, attachOnly);", reconcile)
	provideRequireOrder(t, "SessionWorkerLoop", worker, "if (req.kind == ConnectKind::Provider)",
		"const gesture::Plan plan = gesture::Decide(g, facts, app);")
	provider := worker[strings.Index(worker, "if (req.kind == ConnectKind::Provider)")+1:]
	if end := strings.Index(provider, "continue;"); end >= 0 {
		provider = provider[:end]
		provideRequire(t, "the provider pass", provider, "ReconcileProviderLocked(req.reason);")
	} else {
		t.Error("the provider pass must end the iteration (continue) before the gesture table")
	}
	initialize := definitionBody(t, "SdkHost.cpp", source, "bool SdkHost::Initialize()")
	provideRequire(t, "Initialize", initialize, `EnsureSession("resume");`,
		`RequestProviderReconcile("launch, signed out");`)
	request := definitionBody(t, "SdkHost.cpp", source,
		"void SdkHost::RequestProviderReconcile(const char* reason)")
	provideRequire(t, "RequestProviderReconcile", request, "r.kind = ConnectKind::Provider;",
		"RequestSession(std::move(r));")
	session := definitionBody(t, "SdkHost.cpp", source,
		"void SdkHost::RequestSession(SessionRequest request)")
	provideRequire(t, "RequestSession", session,
		"request.kind == ConnectKind::Provider && pending_.kind != ConnectKind::Provider",
		"pending_.kind != ConnectKind::Provider")
}

// A provide mode or provider policy change, a kill switch lift, a sign-in, a
// space change and an unexpected drop each keep the service in step — queued
// on the worker, never on the calling (UI) thread.
func TestProvideWiringAppReconcilesOnModePolicyAndKillSwitch(t *testing.T) {
	source := sdkHostSource(t)
	mode := definitionBody(t, "SdkHost.cpp", source,
		"void SdkHost::SetProvideControlMode(const std::string& mode)")
	provideRequireOrder(t, "SetProvideControlMode", mode,
		"localState_->setProvideControlMode(mode);", `if (!device_) RequestProviderReconcile("provide mode changed");`)
	transport := definitionBody(t, "SdkHost.cpp", source,
		"void SdkHost::ApplyTransportSettings(")
	provideRequireOrder(t, "ApplyTransportSettings", transport,
		"localState_->setProviderTransportSettings(settings);",
		"if (provider && !device_) RequestProviderReconcile(")
	killSwitch := definitionBody(t, "SdkHost.cpp", source, "void SdkHost::SetKillSwitch(bool on,")
	provideRequireOrder(t, "SetKillSwitch", killSwitch, "service_.SetKillSwitch(on)",
		"if (!on && !device_) RequestProviderReconcile(")
	register := definitionBody(t, "SdkHost.cpp", source, "void SdkHost::RegisterNetworkClient(")
	provideRequireOrder(t, "RegisterNetworkClient", register,
		"localState_->setByClientJwt(*result->by_client_jwt);", `RequestProviderReconcile("signed in");`)
	server := definitionBody(t, "SdkHost.cpp", source, "bool SdkHost::ApplyNetworkServer(")
	provideRequire(t, "ApplyNetworkServer", server, `EnsureSession("network server change");`,
		`RequestProviderReconcile("network server change, signed out");`)
	initialize := definitionBody(t, "SdkHost.cpp", source, "bool SdkHost::Initialize()")
	handler := handlerSource(t, "SdkHost.cpp", initialize, "service_.SetStateHandler(")
	provideRequireOrder(t, "the service state handler", handler,
		"proto::IsFailsafeStop(st.stop_reason)", `EnsureSession("unexpected drop"`)
	provideRequire(t, "the service state handler", handler, "proto::IsSessionLive(before)")
}

// The reconcile decides with the shared step from one get_state, sends the
// stored identity, space, mode and provider policy, and leaves a session alone.
func TestProvideWiringReconcileUsesTheSharedStep(t *testing.T) {
	source := sdkHostSource(t)
	reconcile := definitionBody(t, "SdkHost.cpp", source,
		"void SdkHost::ReconcileProviderLocked(const char* reason)")
	provideRequireOrder(t, "ReconcileProviderLocked", reconcile, "if (device_ || !localState_) return;",
		"CurrentServiceStatusLocked(answered)")
	provideRequireOrder(t, "ReconcileProviderLocked", reconcile, "CurrentServiceStatusLocked(answered)",
		"provide::DisconnectedProviderStep(mode, proto::ProviderFactsFrom(st, answered))")
	provideRequire(t, "ReconcileProviderLocked", reconcile,
		`std::string mode = "never";`,
		"mode = localState_->getProvideControlMode();",
		"request.by_jwt = clientJwt;",
		"request.instance_id = instanceId;",
		"request.provide_mode = mode;",
		"request.network_space_json = networkSpace_->toJson();",
		"localState_->getProviderTransportSettings()",
		"service_.StartProvider(request, &after, &error)",
		"service_.StopProvider(&after, &error)",
		"AdoptServiceFacts(*after);")
	client := stripComments(readAppSource(t, "ServiceClient.cpp"))
	provideRequire(t, "ServiceClient.cpp", client, "proto::msg::kStartProvider",
		"proto::msg::kStopProvider")
	call := definitionBody(t, "ServiceClient.cpp", client, "bool ServiceClient::CallProvider(")
	if strings.Contains(call, "TunnelState::Error") {
		t.Error("CallProvider folds a refusal into the status state: a refused provider says " +
			"nothing about the tunnel")
	}
}

// With no DeviceRemote the provide dot, its ring and the discoverable line read
// the service's provider-only device, not a hard "not providing"; the client
// count is its get_provider_stats answer, and one it does not know is not shown
// as zero.
func TestProvideWiringIndicatorShowsTheProvider(t *testing.T) {
	source := sdkHostSource(t)
	stats := definitionBody(t, "SdkHost.cpp", source, "LiveStats SdkHost::ReadStats()")
	provideRequireOrder(t, "ReadStats", stats, "} else if (!device_) {", "FillProviderOnlyStats(s);")
	fill := definitionBody(t, "SdkHost.cpp", source,
		"void SdkHost::FillProviderOnlyStats(LiveStats& stats) const")
	provideRequire(t, "FillProviderOnlyStats", fill,
		"if (!serviceProviderRunning_.load()) return;",
		"stats.provideMode = serviceProviderMode_.load();",
		"stats.provideEnabled = stats.provideMode != 0;",
		"stats.provideHasNetworkKey = serviceProviderNetworkKey_.load();",
		"const int64_t clients = serviceProviderClients_.load();",
		"stats.provideClients = clients < 0 ? 0 : clients;",
		"stats.provideClientsUnknown = stats.provideEnabled && clients < 0;")
	adopt := definitionBody(t, "SdkHost.cpp", source,
		"void SdkHost::AdoptServiceFacts(const proto::TunnelStatus& st)")
	provideRequire(t, "AdoptServiceFacts", adopt,
		"serviceProviderRunning_.exchange(st.provider_running);",
		"serviceProviderKnown_.store(true);")
	for _, name := range []string{"void SdkHost::OnServiceDisconnected()", "void SdkHost::Logout()"} {
		body := definitionBody(t, "SdkHost.cpp", source, name)
		provideRequire(t, name, body, "serviceProviderRunning_.store(false);")
	}
	initialize := definitionBody(t, "SdkHost.cpp", source, "bool SdkHost::Initialize()")
	handler := handlerSource(t, "SdkHost.cpp", initialize, "service_.SetStateHandler(")
	provideRequireOrder(t, "the service state handler", handler,
		"if (!HasSession()) FillProviderOnlyStats(stats);", "onStats_(stats);")
	page := stripComments(readAppSource(t, "ConnectPage.cpp"))
	provideRequire(t, "ConnectPage.cpp", page,
		"if (stats.provideEnabled && !stats.provideClientsUnknown) {",
		"urnw::ProvideModeVisualFor(stats.provideMode, stats.providePaused)",
		"stats.provideEnabled && stats.provideHasNetworkKey")
	project := readCommonSource(t, "Common.vcxproj")
	provideRequire(t, "Common.vcxproj", project, `<ClInclude Include="ProvideLifecycle.h" />`)
}
