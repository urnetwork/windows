// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"strings"
	"testing"
)

// The Connect page's Extender switch while disconnected (support inbox 1521).
// The provider-only device runs the extender role, and the service now takes
// the switch's write (set_provide_extender): through the device that runs,
// else into the space the last device ran in, so a session and the
// provider-only device read the one setting. The pure parts run in
// provide-protocol-tests.cpp, provide-lifecycle-tests.cpp and
// extender-tests.cpp; these read the service and app sources, which need
// Windows and WinRT, with comments stripped so prose cannot satisfy a contract.

// The part of text from the first `from` up to the first `to` after it, or to
// its end when `to` is empty; fatal when either is missing, so a contract
// cannot pass on an empty slice.
func sourceBetween(t *testing.T, where, text, from, to string) string {
	t.Helper()
	start := strings.Index(text, from)
	if start < 0 {
		t.Fatalf("%s no longer has %q; update this contract", where, from)
	}
	part := text[start:]
	if to == "" {
		return part
	}
	end := strings.Index(part, to)
	if end < 0 {
		t.Fatalf("%s has nothing after %q up to %q; update this contract", where, from, to)
	}
	return part[:end]
}

// The service answers the verb on the one control pipe, under the session
// lock in budget, and writes where the shared rule says.
func TestExtenderSwitchOfflineServiceWiring(t *testing.T) {
	server := stripComments(readServiceSource(t, "ControlServer.cpp"))
	// the one pipe's handler, so the pipe's access rule (kPipeSddl) is the verb's
	start := definitionBody(t, "ControlServer.cpp", server, "bool ControlServer::Start()")
	provideRequire(t, "ControlServer::Start", start, "return Handle(req);")
	handle := definitionBody(t, "ControlServer.cpp", server, "nlohmann::json ControlServer::Handle(")
	branch := sourceBetween(t, "ControlServer::Handle", handle,
		"type == proto::msg::kSetProvideExtender", "} else {")
	provideRequireOrder(t, "the set_provide_extender branch", branch,
		"request.get<proto::SetProvideExtender>()",
		"reply.ok = tunnel_.SetProvideExtender(req.provide_extender, error);")
	provideRequire(t, "the set_provide_extender branch", branch, "reply.error = error;")

	source := tunnelControllerSource(t)
	write := definitionBody(t, "TunnelController.cpp", source,
		"bool TunnelController::SetProvideExtender(bool on, std::string& error)")
	// a wedged bring-up refuses it in budget rather than holding the pipe
	provideRequireOrder(t, "SetProvideExtender", write,
		"lock.try_lock_for(kStopLockBudget)", "provide::ExtenderSettingTargetFor(")
	provideRequire(t, "SetProvideExtender", write,
		"providerDevice_ != nullptr, device_ != nullptr, spaceManager_ && lastSpaceKey_)",
		"device_->setProvideExtender(on);",
		"spaceManager_->getNetworkSpace(lastSpaceKey_)",
		".getAsyncLocalState().getLocalState().setProvideExtender(on);")
	// the provider-only device's next answer carries the write
	provideRequireOrder(t, "SetProvideExtender", write,
		"providerDevice_->setProvideExtender(on);", "RefreshProviderExtenderLocked();")
	refused := sourceBetween(t, "SetProvideExtender", write,
		"case provide::ExtenderSettingTarget::None:", "")
	provideRequireOrder(t, "the refused target", refused, "error = ", "return false;")

	refresh := definitionBody(t, "TunnelController.cpp", source,
		"void TunnelController::RefreshProviderExtenderLocked()")
	// the device is read before the statistics lock, never under it
	provideRequireOrder(t, "RefreshProviderExtenderLocked", refresh,
		"providerDevice_->getExtenderProvideStatus()", "std::scoped_lock lock(providerStatsMutex_);")
	provideRequireOrder(t, "RefreshProviderExtenderLocked", refresh,
		"providerDevice_->getProvideExtender()", "std::scoped_lock lock(providerStatsMutex_);")
	locked := sourceBetween(t, "RefreshProviderExtenderLocked", refresh,
		"std::scoped_lock lock(providerStatsMutex_);", "")
	provideRequire(t, "RefreshProviderExtenderLocked", locked,
		"if (!providerExtender_) return;", "providerExtender_->Store(std::move(status));",
		"providerExtenderSetting_ = setting;")
	requireNone(t, "RefreshProviderExtenderLocked under the statistics lock", locked,
		"providerDevice_")

	// every start imports through here, so the key names the space the next
	// start reads
	importer := definitionBody(t, "TunnelController.cpp", source,
		"urnet::NetworkSpace TunnelController::ImportNetworkSpaceLocked(")
	provideRequireOrder(t, "ImportNetworkSpaceLocked", importer,
		"spaceManager_->importNetworkSpaceFromJson(networkSpaceJson)", "lastSpaceKey_ = space.getKey();")

	stats := definitionBody(t, "TunnelController.cpp", source,
		"proto::ProviderStats TunnelController::ProviderStats()")
	role := sourceBetween(t, "ProviderStats", stats, "if (providerExtender_) {", "return stats;")
	provideRequire(t, "ProviderStats", role, "stats.provide_extender_writable = true;")
}

// The app writes over a session's device as before, and with no session sends
// the write to the service off the UI thread, then reads the answer that
// carries it.
func TestExtenderSwitchOfflineAppWiring(t *testing.T) {
	client := stripComments(readAppSource(t, "ServiceClient.cpp"))
	send := definitionBody(t, "ServiceClient.cpp", client,
		"bool ServiceClient::SetProvideExtender(bool on, std::string* error)")
	provideRequire(t, "ServiceClient::SetProvideExtender", send,
		"s.provide_extender = on;", "proto::msg::kSetProvideExtender")

	source := sdkHostSource(t)
	set := definitionBody(t, "SdkHost.cpp", source, "void SdkHost::SetProvideExtender(bool on)")
	provideRequireOrder(t, "SdkHost::SetProvideExtender", set,
		"shown = lastExtenderProvideStatus_;", "ExtenderProvideWriteRouteFor(device_.has_value(), shown)")
	service := sourceBetween(t, "SdkHost::SetProvideExtender", set,
		"case ExtenderProvideWriteRoute::Service:", "case ExtenderProvideWriteRoute::None:")
	provideRequire(t, "the service route", service, "QueueProviderOnlyExtenderWrite(on);", "return;")
	// a pipe call on the UI thread could wait behind a start_tunnel
	requireNone(t, "the service route", service, "service_.")
	provideRequireOrder(t, "SdkHost::SetProvideExtender", set,
		"case ExtenderProvideWriteRoute::None:", "device_->setProvideExtender(on);")

	queue := definitionBody(t, "SdkHost.cpp", source,
		"void SdkHost::QueueProviderOnlyExtenderWrite(bool on)")
	provideRequire(t, "QueueProviderOnlyExtenderWrite", queue,
		"std::scoped_lock lock(providerOnlyMutex_);", "providerOnlyExtenderWrite_ = on;",
		"providerOnlyKick_ = true;", "providerOnlyCv_.notify_all();")

	loop := definitionBody(t, "SdkHost.cpp", source, "void SdkHost::ProviderOnlyStatsLoop()")
	provideRequireOrder(t, "ProviderOnlyStatsLoop", loop, "std::unique_lock lock(providerOnlyMutex_);",
		"extenderWrite = std::exchange(providerOnlyExtenderWrite_, std::nullopt);")
	// sent outside mutex_, before this pass reads the answer that carries it
	provideRequireOrder(t, "ProviderOnlyStatsLoop", loop,
		"WriteProviderOnlyExtender(*extenderWrite);", "service_.GetProviderStats(stats)")
	provideRequireOrder(t, "ProviderOnlyStatsLoop", loop,
		"WriteProviderOnlyExtender(*extenderWrite);", "std::scoped_lock lock(mutex_);")

	write := definitionBody(t, "SdkHost.cpp", source, "void SdkHost::WriteProviderOnlyExtender(bool on)")
	// the guess goes once the service answered, written or refused
	provideRequireOrder(t, "WriteProviderOnlyExtender", write,
		"service_.SetProvideExtender(on, &error)", "extenderProvideRepublish_ = true;")
	requireNone(t, "WriteProviderOnlyExtender", write, "lock(mutex_)")

	show := definitionBody(t, "SdkHost.cpp", source,
		"void SdkHost::ShowProviderOnlyStatsLocked(const proto::ProviderStats& stats)")
	provideRequireOrder(t, "ShowProviderOnlyStatsLocked", show,
		"extender.serviceWritable = stats.provide_extender_writable;",
		"PublishExtenderProvideView(std::move(extender));")
}

// A view that shows the switch for an older service would be a dead switch,
// and a write route wider than the switch would write what no switch showed.
func TestExtenderPresentationRejectsAWiderWriteThanTheSwitch(t *testing.T) {
	requireExtenderPresentationFailure(t, "ExtenderPresentation.cpp", func(source string) string {
		return strings.Replace(source,
			"model.switchVisible = view.supported && (!view.providerOnly || view.serviceWritable);",
			"model.switchVisible = view.supported && !view.providerOnly;", 1)
	}, "the service writes shows both rows")
	requireExtenderPresentationFailure(t, "ExtenderPresentation.cpp", func(source string) string {
		return strings.Replace(source, "  guess.serviceWritable = current.serviceWritable;\n", "", 1)
	}, "a guess over a status the service writes keeps its writer")
	requireExtenderPresentationFailure(t, "ExtenderPresentation.h", func(source string) string {
		return strings.Replace(source, " &&\n           serviceWritable == o.serviceWritable;", ";", 1)
	}, "whether the service writes it alone is a change")
	requireExtenderPresentationFailure(t, "ExtenderPresentation.cpp", func(source string) string {
		return strings.Replace(source,
			"if (shown.supported && shown.providerOnly && shown.serviceWritable) {",
			"if (shown.supported && shown.providerOnly) {", 1)
	}, "an older service's provider-only status is never written")
}

// A reader that drops the writer keeps the switch hidden over a service that
// takes it; a lenient request writes a default nobody chose; a target rule
// that skips the space drops a write the next start should read.
func TestProvideSpecsRejectABrokenExtenderSwitch(t *testing.T) {
	extra := provideJsonFlags(t)
	requireProvideFailure(t, "provide-protocol-tests.cpp", map[string]func(string) string{
		"Protocol.h": func(source string) string {
			return strings.Replace(source,
				"  get(\"provide_extender_writable\", v.provide_extender_writable);\n", "", 1)
		},
	}, "provider stats: provide_extender_writable round-trips", extra...)
	// the tolerant reader every report uses, which leaves the default in place
	strict := `  auto it = j.find("provide_extender");
  if (it == j.end() || !it->is_boolean()) {
    throw std::runtime_error("set_provide_extender requires provide_extender (a boolean)");
  }
  v.provide_extender = it->get<bool>();
`
	tolerant := `  if (auto it = j.find("provide_extender"); it != j.end() && !it->is_null())
    it->get_to(v.provide_extender);
`
	requireProvideFailure(t, "provide-protocol-tests.cpp", map[string]func(string) string{
		"Protocol.h": func(source string) string {
			return strings.Replace(source, strict, tolerant, 1)
		},
	}, "switch: a request without a boolean value is refused", extra...)
	requireProvideFailure(t, "provide-lifecycle-tests.cpp", map[string]func(string) string{
		"ProvideLifecycle.h": func(source string) string {
			return strings.Replace(source, "if (lastSpace) return ExtenderSettingTarget::NetworkSpace;",
				"if (lastSpace) return ExtenderSettingTarget::None;", 1)
		},
	}, "with no device it goes into the space the next start reads")
}
