// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"strings"
	"testing"
)

// How the service tells its devices that the network moved, and how the app
// follows the binding it is told, read from sources that need Windows and the
// sdk with every comment blanked so prose cannot satisfy a contract. The pure
// rules they call — what counts as the network moving (EgressChangeFilter) and
// the watchdog sampler's sleep (PlanSamplerSleep) — run in
// network-change-notify-tests.cpp (network_change_notify_test.go).

// The service's EgressMonitor.cpp, comments stripped.
func egressMonitorSource(t *testing.T) string {
	return stripComments(readServiceSource(t, "EgressMonitor.cpp"))
}

// The struct that opens at opener, through the "};" that closes it.
func switchStruct(t *testing.T, where, text, opener string) string {
	t.Helper()
	body := text[switchIndex(t, where, text, opener):]
	return body[:switchIndex(t, where, body, "\n};")+len("\n};")]
}

// Where needle starts in text; the contract stops when it is missing, since what
// follows would slice from it.
func switchIndex(t *testing.T, where, text, needle string) int {
	t.Helper()
	at := strings.Index(text, needle)
	if at < 0 {
		t.Fatalf("%s is missing %q; update this contract", where, needle)
	}
	return at
}

// The watchdog's sdk thread tells the device once per burst, through the sdk's
// one seam: notifyNetworkChange() is networkChanged() under its mobile name, so
// calling both kicked every transport twice, and a second kick cancels the dial
// the first one started.
func TestSamplerTellsTheDeviceOncePerBurst(t *testing.T) {
	source := stripComments(readServiceSource(t, "TunnelWatchdog.cpp"))
	sampler := definitionBody(t, "TunnelWatchdog.cpp", source,
		"void TunnelWatchdog::RunSampler(std::shared_ptr<WatchdogChannel> channel)")
	if n := strings.Count(sampler, "device->networkChanged();"); n != 1 {
		t.Errorf("RunSampler must call device->networkChanged() exactly once, found %d", n)
	}
	requireNone(t, "RunSampler", sampler, "notifyNetworkChange(")
	// handled once the call returns, so a replacement watchdog does not replay
	// the same event into the same device
	kick := switchIndex(t, "RunSampler", sampler, "device->networkChanged();")
	handled := switchIndex(t, "RunSampler", sampler,
		"channel->sessionReadiness->NetworkEventHandled(*networkEvent);")
	if handled < kick {
		t.Fatal("RunSampler must mark the network event handled after telling the device")
	}
	requireNone(t, "RunSampler, between the kick and its handling", sampler[kick:handled], "return;")
}

// A burst that begins while the sampler sleeps towards its next exit sample ends
// that sleep: the device is told when the burst's window closes, not at the next
// 2 s sample.
func TestSamplerSleepEndsForABurst(t *testing.T) {
	source := stripComments(readServiceSource(t, "TunnelWatchdog.cpp"))
	sampler := definitionBody(t, "TunnelWatchdog.cpp", source,
		"void TunnelWatchdog::RunSampler(std::shared_ptr<WatchdogChannel> channel)")
	provideRequire(t, "RunSampler", sampler,
		"const SamplerSleep plan = PlanSamplerSleep(NowMillis(), nextSampleMillis, channel->coalescer,",
		"SamplerSleepInterrupted(plan, channel->coalescer, channel->qualityCoalescer);")
	provideRequireOrder(t, "RunSampler", sampler, "PlanSamplerSleep(", "channel->wake.wait_for(")
	provideRequireOrder(t, "RunSampler", sampler, "channel->wake.wait_for(", "SamplerSleepInterrupted(")
	// the predicate that ended the sleep only on cancellation
	requireNone(t, "RunSampler", sampler, "[&] { return channel->cancelled.load(); });")
}

// A freeze long enough to have been a sleep (a resume, a hibernate) tells the
// device the network moved, in a live tunnel as well as a pending capture: the
// resume's os events may move nothing the monitor compares, and connections left
// across a sleep are likely dead. A shorter freeze, a load stall, rebases the
// failsafe and holds a pending capture's proof, and tells the device nothing.
// The kick goes through the network-change bar only.
func TestFreezeTellsTheDeviceOnlyAfterASleep(t *testing.T) {
	source := stripComments(readServiceSource(t, "TunnelWatchdog.cpp"))
	evaluator := definitionBody(t, "TunnelWatchdog.cpp", source, "void TunnelWatchdog::RunEvaluator(")
	// both bars from the channel's settings, never constants
	provideRequire(t, "RunEvaluator", evaluator,
		"FreezeResponseFor(tickGap, channel->settings.frozenMillis,",
		"channel->settings.freezeNetworkChangeMillis);")
	branch := evaluator[switchIndex(t, "RunEvaluator", evaluator, "if (freeze.rebase) {"):]
	if end := strings.Index(branch, "continue;"); end >= 0 {
		branch = branch[:end]
	}
	// the kick, and only under the network-change bar
	if n := strings.Count(evaluator, "channel->coalescer.Observe("); n != 1 {
		t.Errorf("RunEvaluator must queue a network change in one place only, found %d", n)
	}
	if n := strings.Count(evaluator, "->NetworkChanged("); n != 1 {
		t.Errorf("RunEvaluator must tell a readiness the network moved in one place only, found %d", n)
	}
	kick := switchIndex(t, "the freeze branch", branch, "if (freeze.networkChanged && channel->sessionReadiness) {")
	stall := switchIndex(t, "the freeze branch", branch, "} else if (channel->captureReadiness) {")
	if !(kick < stall) {
		t.Fatal("the freeze branch must test the network-change bar before its stall arm")
	}
	provideRequire(t, "the freeze branch's sleep arm", branch[kick:stall],
		"channel->sessionReadiness->NetworkChanged(now);",
		"channel->coalescer.Observe(now);")
	provideRequire(t, "the freeze branch's stall arm", branch[stall:],
		"channel->captureReadiness->ProofBarrier(now);")
	requireNone(t, "the freeze branch's stall arm", branch[stall:], "NetworkChanged(", "coalescer.Observe(")
	// the bars: settings with their defaults, the network-change bar next to the
	// freeze bar, and the freeze bar's default tied to the evaluator's ticks over
	// the default instance
	settings := stripComments(readServiceSource(t, "TunnelWatchdogSettings.h"))
	watchdogSettings := switchStruct(t, "TunnelWatchdogSettings.h", settings, "struct TunnelWatchdogSettings {")
	provideRequireOrder(t, "TunnelWatchdogSettings", watchdogSettings,
		"int64_t frozenMillis = 5000;", "int64_t freezeNetworkChangeMillis = 15000;")
	provideRequire(t, "TunnelWatchdogSettings.h", settings,
		"static_assert(TunnelWatchdogSettings{}.frozenMillis ==\n                  5 * TunnelWatchdogSettings{}.evaluateIntervalMillis,",
		"static_assert(TunnelWatchdogSettings{}.freezeNetworkChangeMillis >")
}

// Every tunable of the network-change path and of the watchdog it changed is a
// setting with its default in one struct, read from the instance the watchdog
// was built with — never a constant — so a test or a caller can move it: the
// notification window, the two threads' cadence, both freeze bars, the join
// budget, the verdict's thresholds and the capture timing.
func TestNetworkChangeTimingIsSettings(t *testing.T) {
	notify := stripComments(readServiceSource(t, "NetworkChangeNotify.h"))
	provideRequire(t, "NetworkChangeNotify.h", notify,
		"explicit NotifyCoalescer(const NetworkNotifySettings& settings = {})",
		": windowMillis_(settings.windowMillis) {}",
		"NotifyClock clock = {}, NetworkNotifySettings settings = {})",
		"channel_->network = NotifyCoalescer(settings);",
		"channel_->quality = NotifyCoalescer(settings);")
	settings := stripComments(readServiceSource(t, "TunnelWatchdogSettings.h"))
	watchdogSettings := switchStruct(t, "TunnelWatchdogSettings.h", settings, "struct TunnelWatchdogSettings {")
	provideRequire(t, "TunnelWatchdogSettings", watchdogSettings,
		"int64_t sampleIntervalMillis = 2000;",
		"int64_t evaluateIntervalMillis = 1000;",
		"int64_t frozenMillis = 5000;",
		"int64_t freezeNetworkChangeMillis = 15000;",
		"int64_t joinBudgetMillis = 250;",
		"int64_t joinPollMillis = 10;",
		"DeadTunnelSettings deadTunnel;",
		"CaptureReadinessSettings capture;",
		"NetworkNotifySettings networkNotify;")
	deadTunnel := switchStruct(t, "TunnelWatchdogSettings.h", settings, "struct DeadTunnelSettings {")
	provideRequire(t, "DeadTunnelSettings", deadTunnel,
		"int64_t deadFastMillis = 20000;",
		"uint64_t deadFastOutboundPackets = 8;",
		"int64_t deadSlowMillis = 90000;",
		"int64_t sdkUnresponsiveMillis = 30000;",
		"int64_t sdkUnresponsiveArmMillis = 10000;",
		"int64_t failsafeNoticeMillis = 30000;")
	// portable, so its harness reads the defaults the service runs with
	raw := readServiceSource(t, "TunnelWatchdogSettings.h")
	requireNone(t, "TunnelWatchdogSettings.h", raw, "#include <windows.h>", "#include <winsock2.h>",
		`#include "Sdk.h"`, `#include "Log.h"`)
	capture := switchStruct(t, "CaptureReadiness.h", stripComments(readServiceSource(t, "CaptureReadiness.h")),
		"struct CaptureReadinessSettings {")
	provideRequire(t, "CaptureReadinessSettings", capture,
		"int64_t probeAgeResolutionMillis = 1000;",
		"int64_t freshnessMillis = 8000;",
		"int64_t sampleStallMillis = 30000;")

	header := stripComments(readServiceSource(t, "TunnelWatchdog.h"))
	provideRequire(t, "TunnelWatchdog.h", header,
		"explicit TunnelWatchdog(TunnelWatchdogSettings settings = {}) : settings_(settings) {}",
		"const TunnelWatchdogSettings settings_;",
		"const TunnelWatchdogSettings settings;",
		"coalescer(timing.networkNotify),",
		"qualityCoalescer(timing.networkNotify) {}")
	provideRequire(t, "TunnelWatchdog.h", header,
		"const TunnelWatchdogSettings& settings() const { return settings_; }")
	provideRequire(t, "DeadTunnelVerdict.h", stripComments(readServiceSource(t, "DeadTunnelVerdict.h")),
		"const DeadTunnelSettings& settings) {",
		"inboundAge < settings.deadFastMillis) return v;",
		"sampleAge >= settings.sdkUnresponsiveMillis",
		"s.outboundSinceInbound >= settings.deadFastOutboundPackets;",
		"provenAge >= settings.deadSlowMillis",
		"sampleAge >= settings.sdkUnresponsiveArmMillis",
		"soonest <= settings.failsafeNoticeMillis")
	source := stripComments(readServiceSource(t, "TunnelWatchdog.cpp"))
	provideRequire(t, "TunnelWatchdog.cpp", source,
		"std::make_shared<WatchdogChannel>(settings_);",
		"std::chrono::milliseconds(channel->settings.joinBudgetMillis);",
		"std::chrono::milliseconds(channel->settings.joinPollMillis)")
	sampler := definitionBody(t, "TunnelWatchdog.cpp", source,
		"void TunnelWatchdog::RunSampler(std::shared_ptr<WatchdogChannel> channel)")
	provideRequire(t, "RunSampler", sampler,
		"nextSampleMillis = NowMillis() + channel->settings.sampleIntervalMillis;",
		"NowMillis() - proofSinceMillis >= channel->settings.capture.probeAgeResolutionMillis",
		"channel->settings.capture)) ++usableProven;")
	evaluator := definitionBody(t, "TunnelWatchdog.cpp", source, "void TunnelWatchdog::RunEvaluator(")
	provideRequire(t, "RunEvaluator", evaluator,
		"channel->settings.evaluateIntervalMillis),",
		"Evaluate(signals, now, channel->settings.deadTunnel);",
		"now, channel->settings.capture);")
	// each session's readiness on the watchdog's capture timing
	controller := tunnelControllerSource(t)
	provideRequire(t, "TunnelController.cpp", controller,
		"std::make_shared<CaptureReadiness>(deadTunnelWatchdog_.settings().capture);")
	readiness := stripComments(readServiceSource(t, "CaptureReadiness.h"))
	provideRequire(t, "CaptureReadiness.h", readiness,
		"age <= settings_.freshnessMillis;",
		"nowMillis - last >= settings.sampleStallMillis;",
		"settings.probeAgeResolutionMillis > 0 ? settings.probeAgeResolutionMillis : 0;")

	// the constants they replace are gone from every source that read them
	for _, name := range []string{"NetworkChangeNotify.h", "TunnelWatchdog.h", "TunnelWatchdog.cpp",
		"TunnelWatchdogSettings.h", "SelfTest.cpp", "TunnelController.cpp", "EgressMonitor.cpp",
		"CaptureReadiness.h", "PacketPump.cpp"} {
		requireNone(t, name, stripComments(readServiceSource(t, name)),
			"kNetworkNotifyDebounceMillis", "kSdkSampleIntervalMillis", "kEvaluatorFrozenMillis",
			"kFreezeNetworkChangeMillis", "kDeadFastMillis", "kDeadFastOutboundPackets", "kDeadSlowMillis",
			"kSdkUnresponsiveMillis", "kSdkUnresponsiveArmMillis", "kFailsafeNoticeMillis",
			"kEvaluateIntervalMillis", "kWatchdogJoinBudgetMillis")
	}
	// and no timing literal stands in for one
	requireNone(t, "CaptureReadiness.h", readiness, "age <= 8000", ">= 30000", "elapsed >= 1000")
	requireNone(t, "TunnelWatchdog.cpp", source, "proofSinceMillis >= 1000", "milliseconds(10)")
}

// The egress monitor reports what moved, never what did not: every reading is
// compared with the last (EgressChangeFilter), the binding is set before the
// report so the kick re-dials over it, the binding's followers hear about the
// binding and the device about the network, and the reading Start() takes is a
// baseline, not a network event.
func TestEgressMonitorReportsOnlyWhatMoved(t *testing.T) {
	monitor := egressMonitorSource(t)
	update := definitionBody(t, "EgressMonitor.cpp", monitor, "void EgressMonitor::Update(Reading reading)")
	bind := update[switchIndex(t, "EgressMonitor::Update", update, "EgressChange change;"):]
	provideRequire(t, "EgressMonitor::Update", bind,
		"const EgressSignature signature = NetworkConfig::ObserveEgress(egress);",
		"filter_.Seed(signature);",
		"change.binding = indexMoved;",
		"change = filter_.Observe(signature);",
		"change.binding = change.binding || indexMoved;",
		"if (change.binding && handler) handler(egress);",
		"if (change.network && networkEvent) networkEvent();")
	provideRequireOrder(t, "EgressMonitor::Update", bind, "urnet::setEgressInterfaceIndex(",
		"if (change.network && networkEvent) networkEvent();")
	provideRequireOrder(t, "EgressMonitor::Update", bind, "if (reading == Reading::Baseline) {",
		"filter_.Seed(signature);")
	// the unconditional report every observation used to make
	requireNone(t, "EgressMonitor::Update", update, "if (networkEvent) networkEvent();",
		"if (changed && handler)")
	refresh := monitor[switchIndex(t, "EgressMonitor.cpp", monitor, "void EgressMonitor::Refresh()"):]
	refresh = refresh[:switchIndex(t, "EgressMonitor::Refresh", refresh, "\n")]
	provideRequire(t, "EgressMonitor::Refresh", refresh, "Update(Reading::Observation);")

	header := stripComments(readServiceSource(t, "EgressMonitor.h"))
	provideRequire(t, "EgressMonitor.h", header,
		`#include "NetworkChangeNotify.h"`,
		"enum class Reading { Baseline, Observation };",
		"EgressChangeFilter filter_;",
		"HANDLE addressNotifyHandle_ = nullptr;")
}

// The address half: an address that changes with no interface or route event (a
// new lease on the same adapter, duplicate detection finishing after the route)
// moves what the filter compares, so the monitor hears about addresses too —
// never our tun's, and the subscription ends with the others.
func TestEgressMonitorWatchesAddresses(t *testing.T) {
	monitor := egressMonitorSource(t)
	start := definitionBody(t, "EgressMonitor.cpp", monitor, "bool EgressMonitor::Start()")
	provideRequire(t, "EgressMonitor::Start", start,
		"::NotifyUnicastIpAddressChange(AF_UNSPEC, &EgressMonitor::OnAddressChange, this,")
	stop := definitionBody(t, "EgressMonitor.cpp", monitor, "void EgressMonitor::Stop()")
	provideRequire(t, "EgressMonitor::Stop", stop, "::CancelMibChangeNotify2(addressNotifyHandle_);")
	// cancelled before the lock the callbacks take
	provideRequireOrder(t, "EgressMonitor::Stop", stop, "::CancelMibChangeNotify2(addressNotifyHandle_);",
		"std::scoped_lock lock(mutex_);")
	callback := definitionBody(t, "EgressMonitor.cpp", monitor, "void __stdcall EgressMonitor::OnAddressChange(")
	provideRequire(t, "EgressMonitor::OnAddressChange", callback,
		"if (type == MibInitialNotification || row == nullptr) return;",
		"row->InterfaceLuid.Value == self->tunLuid_.Value",
		`RunGuarded("egress-address-change", [&] { self->Refresh(); });`)
}

// What the filter compares is read from the binding: whether its interface can
// carry a socket (connected, a default route, a preferred source), the source
// the split-tunnel driver is given, the network (v4 source, v6 /64) and the
// gateway.
func TestObserveEgressReadsTheBinding(t *testing.T) {
	source := stripComments(readServiceSource(t, "NetworkConfig.cpp"))
	observe := definitionBody(t, "NetworkConfig.cpp", source,
		"EgressSignature NetworkConfig::ObserveEgress(const EgressInterfaces& bound)")
	provideRequire(t, "NetworkConfig::ObserveEgress", observe,
		"signature.v4 = ObserveEgressFamily(AF_INET, bound.index4);",
		"signature.v6 = ObserveEgressFamily(AF_INET6, bound.index6);")
	family := definitionBody(t, "NetworkConfig.cpp", source,
		"EgressFamilySignature ObserveEgressFamily(ADDRESS_FAMILY family, uint32_t ifIndex)")
	provideRequire(t, "ObserveEgressFamily", family,
		"connected = ifEntry.MediaConnectState != MediaConnectStateDisconnected;",
		"row.DestinationPrefix.PrefixLength != 0 || row.InterfaceIndex != ifIndex",
		"signature.gateway = AddressBytes(row.NextHop, family);",
		"NetworkConfig::InterfaceSourceAddress(ifIndex, family, source.data());",
		"signature.network = family == AF_INET ? signature.source : LowestPreferredPrefix64(ifIndex);",
		"signature.usable = connected && hasDefaultRoute && hasSource;")
	prefix := definitionBody(t, "NetworkConfig.cpp", source,
		"std::array<uint8_t, 16> LowestPreferredPrefix64(uint32_t ifIndex)")
	provideRequire(t, "LowestPreferredPrefix64", prefix,
		"row.DadState != IpDadStatePreferred",
		"std::memcpy(prefix.data(), a.u.Byte, 8);",
		"if (!found || prefix < lowest) {")
	header := stripComments(readServiceSource(t, "NetworkConfig.h"))
	provideRequire(t, "NetworkConfig.h", header,
		`#include "NetworkChangeNotify.h"`,
		"static EgressSignature ObserveEgress(const EgressInterfaces& bound);")
}

// The session's monitor reports to the watchdog only what moved the network, and
// the controller follows the binding: the interface Status() serves, live and
// pushed from a worker of its own (never the system thread that saw the move,
// which EgressMonitor::Stop() waits for, and never the watchdog's sampler, whose
// stall reads as an unresponsive sdk), and the split-tunnel driver.
func TestTunnelFollowsTheEgressBinding(t *testing.T) {
	source := tunnelControllerSource(t)
	follow := definitionBody(t, "TunnelController.cpp", source,
		"void TunnelController::OnEgressChanged(EgressInterfaces egress)")
	provideRequire(t, "OnEgressChanged", follow,
		"const int64_t before4 = liveEgressIndex4_.exchange(index4);",
		"const int64_t before6 = liveEgressIndex6_.exchange(index6);",
		"const bool moved = before4 != index4 || before6 != index6;",
		"reported = statusMirror_.routes_installed;",
		"if (moved && reported) egressPush_.Request();",
		"PushPhysicalAddressesLocked(egress);")
	requireNone(t, "OnEgressChanged", follow, "NotifyStateChanged(", "PushState(", "lock(mutex_",
		"mutex_, std::defer_lock", "PublishStatusLocked(")

	status := definitionBody(t, "TunnelController.cpp", source, "proto::TunnelStatus TunnelController::Status()")
	provideRequire(t, "Status", status,
		"if (s.routes_installed) {",
		"s.egress_index4 = liveEgressIndex4_.load();",
		"s.egress_index6 = liveEgressIndex6_.load();")
	provideRequireOrder(t, "Status", status, "s = statusMirror_;", "s.egress_index4 = liveEgressIndex4_.load();")

	startLocked := definitionBody(t, "TunnelController.cpp", source,
		"proto::TunnelStatus TunnelController::StartLocked(")
	provideRequireOrder(t, "StartLocked", startLocked, "egress_->Start();",
		"liveEgressIndex4_.store(static_cast<int64_t>(bound.index4));")
	teardown := definitionBody(t, "TunnelController.cpp", source, "bool TunnelController::TearDownSessionLocked()")
	provideRequireOrder(t, "TearDownSessionLocked", teardown, "egress_->SetOnChange(nullptr);",
		"liveEgressIndex4_.store(0);")
	provideRequire(t, "TearDownSessionLocked", teardown, "liveEgressIndex6_.store(0);")

	header := stripComments(readServiceSource(t, "TunnelController.h"))
	provideRequire(t, "TunnelController.h", header,
		`#include "CoalescedWorker.h"`,
		"std::atomic<int64_t> liveEgressIndex4_{0};",
		"std::atomic<int64_t> liveEgressIndex6_{0};",
		"void StopStatePushes() { egressPush_.Stop(); }")
	// declared last: destroyed first, so a push in flight is waited out before
	// anything it reads goes
	push := "CoalescedWorker egressPush_{[this] { NotifyStateChanged(); }};"
	at := switchIndex(t, "TunnelController.h", header, push)
	if rest := strings.TrimSpace(header[at+len(push):]); !strings.HasPrefix(rest, "};") {
		t.Error("egressPush_ must be the last member of TunnelController")
	}

	// the control server ends the pushes after its pipe stops, before anything
	// a push reaches is destroyed
	control := stripComments(readServiceSource(t, "ControlServer.cpp"))
	stop := definitionBody(t, "ControlServer.cpp", control, "void ControlServer::Stop()")
	provideRequireOrder(t, "ControlServer::Stop", stop, "tunnel_.SetOnStateChanged(nullptr);", "pipe_.Stop();")
	provideRequireOrder(t, "ControlServer::Stop", stop, "pipe_.Stop();", "tunnel_.StopStatePushes();")
	provideRequireOrder(t, "ControlServer::Stop", stop, "tunnel_.StopStatePushes();", "tunnel_.Stop();")

	// one copy of the worker, shared by both processes
	common := readCommonSource(t, "Common.vcxproj")
	provideRequire(t, "Common.vcxproj", common, `<ClInclude Include="CoalescedWorker.h" />`)
	requireNone(t, "App.vcxproj", readAppSource(t, "App.vcxproj"), `<ClInclude Include="CoalescedWorker.h" />`)
}

// An rpc-only session's device is told about network changes too: the watchdog's
// sampler is the only thread that tells a device, and an rpc-only session used to
// start none. Its failsafe gets no handler (it judges only a tunnel that is up)
// and there is no capture to wait for.
func TestRpcOnlySessionIsToldTheNetworkMoved(t *testing.T) {
	source := tunnelControllerSource(t)
	startLocked := definitionBody(t, "TunnelController.cpp", source,
		"proto::TunnelStatus TunnelController::StartLocked(")
	fence := startLocked[switchIndex(t, "StartLocked", startLocked,
		"if (rpcOnly) {\n      SetStateLocked(proto::TunnelState::RpcOnly);"):]
	fence = fence[:switchIndex(t, "the rpc-only fence", fence, "return StatusLocked();")]
	provideRequire(t, "the rpc-only fence", fence, "deadTunnelWatchdog_.Start(&*device_, nullptr, {}, readiness);")
	requireNone(t, "the rpc-only fence", fence, "WatchForCaptureLocked(", "ActivateCapture(")
}

// The app follows what it is told, and tells its own sdk the network moved when
// the binding moves to an interface — off the pipe reader and the bootstrap
// thread, and stopped with the app's other workers.
func TestAppKicksItsOwnSdkWhenTheBindingMoves(t *testing.T) {
	source := sdkHostSource(t)
	bind := definitionBody(t, "SdkHost.cpp", source,
		"void SdkHost::ApplySdkEgressBind(int64_t index4, int64_t index6, const char* why)")
	provideRequire(t, "ApplySdkEgressBind", bind, "if (index4 != 0 || index6 != 0) sdkNetworkKick_.Request();")
	// after the change gate, outside the egress lock
	provideRequireOrder(t, "ApplySdkEgressBind", bind, "if (sdkEgressBound_ == packed) return;",
		"sdkNetworkKick_.Request();")
	provideRequireOrder(t, "ApplySdkEgressBind", bind, "urnet::setEgressInterfaceIndex(index4, index6);\n  }",
		"sdkNetworkKick_.Request();")
	requireNone(t, "ApplySdkEgressBind", bind, "urnet::networkChanged(")
	if n := strings.Count(source, "sdkNetworkKick_.Stop();"); n != 2 {
		t.Errorf("SdkHost.cpp must stop sdkNetworkKick_ in its destructor and in Quit, found %d stops", n)
	}
	header := stripComments(readAppSource(t, "SdkHost.h"))
	provideRequire(t, "SdkHost.h", header, "CoalescedWorker sdkNetworkKick_{[] { urnet::networkChanged(); }};")
}

// The sdk export the app calls, when this host has an sdk header new enough to
// say (the header is git-ignored; URNETWORK_SDK_INCLUDE names a directory).
func TestSdkExportsAProcessNetworkChange(t *testing.T) {
	header := sdkHeaderWith(t, "inline void networkChanged()")
	if !strings.Contains(header, "urnet_network_changed();") {
		t.Error("urnetwork_sdk.hpp: urnet::networkChanged does not call urnet_network_changed")
	}
}
