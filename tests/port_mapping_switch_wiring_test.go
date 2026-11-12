// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"strings"
	"testing"
)

// The router port mapping switch (connect EXTENDER.md S8; owner, 2026-11-11):
// "Open a port on my router", under the Connect page's speed tests switch,
// whether the extender role asks the router, by UPnP, NAT-PMP or PCP, to
// forward its ports (tcp 443, udp 443 and udp 4053) to this device. The miner
// turns it on by default; the apps keep it off until the user turns it on. It
// is the speed tests switch's twin: read beside the role's status, written
// through a session's device, and with no session through the service
// (set_provide_extender_port_mapping) when its get_provider_stats said it
// takes it, which writes it where it writes the speed tests setting: through
// the device that runs, or with none into the space the last device ran in
// (the sdk's LocalState.SetProvideExtenderPortMapping), so a toggle that lands
// between two devices is the next one's setting. The pure parts run in
// extender-tests.cpp and provide-protocol-tests.cpp; these read the service
// and app sources, which need Windows and WinRT, with comments stripped so
// prose cannot satisfy a contract.

// The service answers the verb on the one control pipe, under the session lock
// in budget, writes where the speed tests switch's write goes, and reports the
// setting beside the role, never without it, off until read.
func TestPortMappingSwitchServiceWiring(t *testing.T) {
	server := stripComments(readServiceSource(t, "ControlServer.cpp"))
	handle := definitionBody(t, "ControlServer.cpp", server, "nlohmann::json ControlServer::Handle(")
	branch := sourceBetween(t, "ControlServer::Handle", handle,
		"type == proto::msg::kSetProvideExtenderPortMapping", "} else if")
	provideRequireOrder(t, "the set_provide_extender_port_mapping branch", branch,
		"request.get<proto::SetProvideExtenderPortMapping>()",
		"reply.ok = tunnel_.SetProvideExtenderPortMapping(req.provide_extender_port_mapping, error);")
	provideRequire(t, "the set_provide_extender_port_mapping branch", branch, "reply.error = error;")

	source := tunnelControllerSource(t)
	write := definitionBody(t, "TunnelController.cpp", source,
		"bool TunnelController::SetProvideExtenderPortMapping(bool on, std::string& error)")
	// a wedged bring-up refuses it in budget rather than holding the pipe
	provideRequireOrder(t, "SetProvideExtenderPortMapping", write,
		"lock.try_lock_for(kStopLockBudget)", "provide::ExtenderSettingTargetFor(")
	provideRequire(t, "SetProvideExtenderPortMapping", write,
		"providerDevice_ != nullptr, device_ != nullptr, spaceManager_ && lastSpaceKey_)",
		"device_->setProvideExtenderPortMapping(on);")
	// the provider-only device's next answer carries the write
	provideRequireOrder(t, "SetProvideExtenderPortMapping", write,
		"providerDevice_->setProvideExtenderPortMapping(on);", "RefreshProviderPortMappingLocked();")
	// With no device the space the last device ran in takes it, through the
	// sdk's LocalState as the speed tests setting's, so a toggle that lands
	// between two devices is not dropped: the next device reads it from its
	// space. A space that is gone refuses it before anything is written.
	space := sourceBetween(t, "SetProvideExtenderPortMapping", write,
		"case provide::ExtenderSettingTarget::NetworkSpace:", "case provide::ExtenderSettingTarget::None:")
	provideRequireOrder(t, "the space target", space,
		"urnet::NetworkSpace space = spaceManager_->getNetworkSpace(lastSpaceKey_);", "if (!space) {")
	gone := sourceBetween(t, "the space target", space, "if (!space) {", "return false;")
	provideRequire(t, "a space that is gone", gone, "error = ")
	provideRequireOrder(t, "the space target", space, "return false;",
		"space.getAsyncLocalState().getLocalState().setProvideExtenderPortMapping(on);")
	refused := sourceBetween(t, "SetProvideExtenderPortMapping", write,
		"case provide::ExtenderSettingTarget::None:", "} catch")
	provideRequireOrder(t, "the refused target", refused, "error = ", "return false;")
	// it writes its own setting and nothing else
	requireNone(t, "SetProvideExtenderPortMapping", write, "setProvideExtender(", "setMeasurementsEnabled(")

	refresh := definitionBody(t, "TunnelController.cpp", source,
		"void TunnelController::RefreshProviderPortMappingLocked()")
	// the device is read before the statistics lock, never under it, and a
	// failed reading leaves the copy as it was
	provideRequire(t, "RefreshProviderPortMappingLocked", refresh, "bool setting = false;")
	provideRequireOrder(t, "RefreshProviderPortMappingLocked", refresh,
		"providerDevice_->getProvideExtenderPortMapping()", "std::scoped_lock lock(providerStatsMutex_);")
	locked := sourceBetween(t, "RefreshProviderPortMappingLocked", refresh,
		"std::scoped_lock lock(providerStatsMutex_);", "")
	provideRequireOrder(t, "RefreshProviderPortMappingLocked", locked,
		"if (!providerExtender_) return;", "providerPortMappingSetting_ = setting;")
	requireNone(t, "RefreshProviderPortMappingLocked under its lock", locked, "providerDevice_")

	// read with the role when the statistics open, off until it is, so a
	// reading that fails reports neither
	open := definitionBody(t, "TunnelController.cpp", source,
		"void TunnelController::OpenProviderStatsLocked()")
	provideRequire(t, "OpenProviderStatsLocked", open,
		"bool portMappingSetting = false;",
		"portMappingSetting = providerDevice_->getProvideExtenderPortMapping();")
	provideRequireOrder(t, "OpenProviderStatsLocked", open,
		"extenderSetting = providerDevice_->getProvideExtender();",
		"portMappingSetting = providerDevice_->getProvideExtenderPortMapping();")
	provideRequireOrder(t, "OpenProviderStatsLocked", open,
		"std::scoped_lock lock(providerStatsMutex_);", "providerPortMappingSetting_ = portMappingSetting;")

	stats := definitionBody(t, "TunnelController.cpp", source,
		"proto::ProviderStats TunnelController::ProviderStats()")
	beside := sourceBetween(t, "ProviderStats", stats, "if (providerExtender_) {", "return stats;")
	provideRequire(t, "ProviderStats beside the role", beside,
		"stats.provide_extender_port_mapping = providerPortMappingSetting_;",
		"stats.provide_extender_port_mapping_writable = true;")
	// the copy the opening and the write left: never a call into the device
	requireNone(t, "ProviderStats", stats, "getProvideExtenderPortMapping(")

	// a device that goes takes its reading with it, back to off
	for _, signature := range []string{
		"void TunnelController::RetireProviderDeviceLocked()",
		"TunnelController::HandedOffDevice TunnelController::TakeProviderDeviceLocked(",
	} {
		body := definitionBody(t, "TunnelController.cpp", source, signature)
		provideRequireOrder(t, signature, body,
			"std::scoped_lock lock(providerStatsMutex_);", "providerPortMappingSetting_ = false;")
	}

	header := stripComments(readServiceSource(t, "TunnelController.h"))
	provideRequire(t, "TunnelController.h", header,
		"bool SetProvideExtenderPortMapping(bool on, std::string& error);",
		"void RefreshProviderPortMappingLocked();",
		"bool providerPortMappingSetting_ = false;")
}

// The app reads the setting where it reads the speed tests switch's, writes it
// by the switch's own route, and draws the row under the speed tests row,
// hidden, never disabled, and never writes it while hidden.
func TestPortMappingSwitchAppWiring(t *testing.T) {
	client := stripComments(readAppSource(t, "ServiceClient.cpp"))
	send := definitionBody(t, "ServiceClient.cpp", client,
		"bool ServiceClient::SetProvideExtenderPortMapping(bool on, std::string* error)")
	provideRequire(t, "ServiceClient::SetProvideExtenderPortMapping", send,
		"s.provide_extender_port_mapping = on;", "proto::msg::kSetProvideExtenderPortMapping")

	source := sdkHostSource(t)
	// beside the role's status: from the device with a session...
	publish := definitionBody(t, "SdkHost.cpp", source,
		"void SdkHost::PublishExtenderProvideStatus(std::optional<urnet::ExtenderProvideStatus> status)")
	provideRequire(t, "PublishExtenderProvideStatus", publish,
		"device_->getProvideExtenderPortMapping()",
		"return lastExtenderProvideStatus_.provideExtenderPortMapping;",
		"ExtenderProvideStatusViewOf(status, readSetting, readMeasurements, readPortMapping)")
	// ...and with none from the service's answer, with its say on the write
	show := definitionBody(t, "SdkHost.cpp", source,
		"void SdkHost::ShowProviderOnlyStatsLocked(const proto::ProviderStats& stats)")
	provideRequire(t, "ShowProviderOnlyStatsLocked", show,
		"return stats.provide_extender_port_mapping;",
		"extender.portMappingServiceWritable = stats.provide_extender_port_mapping_writable;")
	provideRequireOrder(t, "ShowProviderOnlyStatsLocked", show,
		"extender.portMappingServiceWritable = stats.provide_extender_port_mapping_writable;",
		"PublishExtenderProvideView(std::move(extender));")

	// the device, or the service off the UI thread, by the switch's own route
	set := definitionBody(t, "SdkHost.cpp", source, "void SdkHost::SetProvideExtenderPortMapping(bool on)")
	provideRequireOrder(t, "SdkHost::SetProvideExtenderPortMapping", set,
		"shown = lastExtenderProvideStatus_;", "PortMappingWriteRouteFor(device_.has_value(), shown)")
	provideRequire(t, "SdkHost::SetProvideExtenderPortMapping", set,
		"QueueProviderOnlyPortMappingWrite(on);", "extenderProvideRepublish_ = true;")
	provideRequireOrder(t, "SdkHost::SetProvideExtenderPortMapping", set,
		"case ExtenderProvideWriteRoute::None:", "device_->setProvideExtenderPortMapping(on);")
	service := sourceBetween(t, "SdkHost::SetProvideExtenderPortMapping", set,
		"case ExtenderProvideWriteRoute::Service:", "case ExtenderProvideWriteRoute::None:")
	// a pipe call on the write worker could wait behind a start_tunnel
	requireNone(t, "the service route", service, "service_.")
	requireNone(t, "SdkHost::SetProvideExtenderPortMapping", set,
		"service_.SetProvideExtenderPortMapping(", "ExtenderProvideWriteRouteFor(", "SpeedTestsWriteRouteFor(")
	queue := definitionBody(t, "SdkHost.cpp", source,
		"void SdkHost::QueueProviderOnlyPortMappingWrite(bool on)")
	provideRequire(t, "QueueProviderOnlyPortMappingWrite", queue,
		"std::scoped_lock lock(providerOnlyMutex_);", "providerOnlyPortMappingWrite_ = on;",
		"providerOnlyKick_ = true;", "providerOnlyCv_.notify_all();")
	written := definitionBody(t, "SdkHost.cpp", source, "void SdkHost::WriteProviderOnlyPortMapping(bool on)")
	// the guess goes once the service answered, written or refused
	provideRequireOrder(t, "WriteProviderOnlyPortMapping", written,
		"service_.SetProvideExtenderPortMapping(on, &error)", "extenderProvideRepublish_ = true;")
	requireNone(t, "WriteProviderOnlyPortMapping", written, "lock(mutex_)")
	loop := definitionBody(t, "SdkHost.cpp", source, "void SdkHost::ProviderOnlyStatsLoop()")
	provideRequire(t, "ProviderOnlyStatsLoop", loop,
		"portMappingWrite = std::exchange(providerOnlyPortMappingWrite_, std::nullopt);")
	// sent outside mutex_, before this pass reads the answer that carries it
	provideRequireOrder(t, "ProviderOnlyStatsLoop", loop,
		"if (portMappingWrite) WriteProviderOnlyPortMapping(*portMappingWrite);",
		"service_.GetProviderStats(stats)")
	provideRequireOrder(t, "ProviderOnlyStatsLoop", loop,
		"if (portMappingWrite) WriteProviderOnlyPortMapping(*portMappingWrite);",
		"std::scoped_lock lock(mutex_);")

	// the Connect page
	connect := stripComments(readAppSource(t, "ConnectPage.cpp"))
	row := definitionBody(t, "ConnectPage.cpp", connect, "void ConnectPage::ApplyPortMappingRow()")
	provideRequire(t, "ConnectPage::ApplyPortMappingRow", row,
		"urnw::PortMappingRowModelFor(extenderProvideView_)",
		"w_.PortMappingRow().Visibility(model.visible ? Visibility::Visible : Visibility::Collapsed);",
		"updatingControls_ = true;")
	requireNone(t, "ConnectPage::ApplyPortMappingRow", row, "IsEnabled(", "SetProvideExtenderPortMapping(")
	extenderRow := definitionBody(t, "ConnectPage.cpp", connect,
		"void ConnectPage::ApplyExtenderProvideRow()")
	provideRequireOrder(t, "ConnectPage::ApplyExtenderProvideRow", extenderRow,
		"ApplySpeedTestsRow();", "ApplyPortMappingRow();")
	toggled := definitionBody(t, "ConnectPage.cpp", connect, "void ConnectPage::OnPortMappingToggled(")
	provideRequireOrder(t, "ConnectPage::OnPortMappingToggled", toggled,
		"if (updatingControls_) return;",
		"if (!urnw::PortMappingRowModelFor(extenderProvideView_).visible) return;")
	provideRequireOrder(t, "ConnectPage::OnPortMappingToggled", toggled,
		"if (!urnw::PortMappingRowModelFor(extenderProvideView_).visible) return;",
		"Sdk().SetProvideExtenderPortMapping(on);")
	provideRequire(t, "ConnectPage::OnPortMappingToggled", toggled,
		"extenderProvideView_ = urnw::PortMappingGuessFor(extenderProvideView_, on);")
	applyStrings := definitionBody(t, "ConnectPage.cpp", connect, "void ConnectPage::ApplyStrings()")
	provideRequire(t, "ConnectPage::ApplyStrings", applyStrings,
		`w_.PortMappingLabel().Text(Loc("extender_port_mapping"));`,
		`w_.PortMappingDescription().Text(Loc("extender_port_mapping_description"));`)

	// The markup: the row directly under the speed tests row, its title naming
	// the switch with the description under the title, its handler, and the
	// switch's position left to the setting.
	xaml := readAppSource(t, "MainWindow.xaml")
	provideRequire(t, "MainWindow.xaml", xaml,
		`x:Name="PortMappingRow" Visibility="Collapsed"`,
		`AutomationProperties.LabeledBy="{Binding ElementName=PortMappingLabel}"`,
		`Toggled="OnPortMappingToggled"`)
	provideRequireOrder(t, "MainWindow.xaml", xaml, `x:Name="SpeedTestsRow"`, `x:Name="PortMappingRow"`)
	// nothing named between the speed tests switch and this row: the speed
	// tests row closes, and this one opens
	between := strings.TrimPrefix(
		sourceBetween(t, "MainWindow.xaml", xaml, `x:Name="SpeedTestsToggle"`, `x:Name="PortMappingRow"`),
		`x:Name="SpeedTestsToggle"`)
	if strings.Contains(between, "x:Name=") {
		t.Error("MainWindow.xaml must put the port mapping row directly under the speed tests row")
	}
	markup := sourceBetween(t, "MainWindow.xaml", xaml, `x:Name="PortMappingRow"`, "</Border>")
	provideRequireOrder(t, "the port mapping row", markup, `x:Name="PortMappingLabel"`,
		`x:Name="PortMappingDescription"`)
	provideRequireOrder(t, "the port mapping row", markup, `x:Name="PortMappingDescription"`,
		`x:Name="PortMappingToggle"`)
	requireNone(t, "the port mapping row", markup, "IsOn=", "IsEnabled=")
	window := readAppSource(t, "MainWindow.xaml.cpp")
	provideRequire(t, "MainWindow.xaml.cpp", window, "connect_->OnPortMappingToggled(s, e);")
}

// The spec catches each way the switch could show where its write has no
// taker or without the speed tests switch, write wider than it shows, lose its
// position, or start on.
func TestExtenderPresentationRejectsABrokenPortMappingSwitch(t *testing.T) {
	requireExtenderPresentationFailure(t, "ExtenderPresentation.cpp", func(source string) string {
		return strings.Replace(source,
			"(!view.providerOnly || view.portMappingServiceWritable);", "true;", 1)
	}, "a service that takes the other switches' writes but not this one: hidden")
	requireExtenderPresentationFailure(t, "ExtenderPresentation.cpp", func(source string) string {
		return strings.Replace(source,
			"model.visible = SpeedTestsRowModelFor(view).visible &&",
			"model.visible = ExtenderProvideRowModelFor(view).switchVisible &&", 1)
	}, "the port mapping switch never shows without the speed tests switch")
	requireExtenderPresentationFailure(t, "ExtenderPresentation.cpp", func(source string) string {
		return strings.Replace(source, " &&\n      shown.portMappingServiceWritable) {", ") {", 1)
	}, "a service that takes only the other switches' writes is not")
	requireExtenderPresentationFailure(t, "ExtenderPresentation.cpp", func(source string) string {
		return strings.Replace(source,
			"  guess.provideExtenderPortMapping = current.provideExtenderPortMapping;\n", "", 1)
	}, "flipping the Extender switch leaves the port mapping switch where it was")
	requireExtenderPresentationFailure(t, "ExtenderPresentation.h", func(source string) string {
		return strings.Replace(source, "  view.provideExtenderPortMapping = readPortMapping();\n", "", 1)
	}, "which the port mapping switch shows")
	requireExtenderPresentationFailure(t, "ExtenderPresentation.h", func(source string) string {
		return strings.Replace(source, "  bool provideExtenderPortMapping = false;\n",
			"  bool provideExtenderPortMapping = true;\n", 1)
	}, "the view starts with the setting off")
	requireExtenderPresentationFailure(t, "ExtenderPresentation.h", func(source string) string {
		return strings.Replace(source,
			"           provideExtenderPortMapping == o.provideExtenderPortMapping &&\n", "", 1)
	}, "the port mapping setting alone is a change")
}

// A reader that drops the writer keeps the switch hidden over a service that
// takes it, and one that drops the setting shows it off over a router told to
// forward; a lenient request writes a value nobody chose; a reader that
// defaults the setting on shows a port open that nobody opened.
func TestProvideProtocolRejectsABrokenPortMappingSwitch(t *testing.T) {
	extra := provideJsonFlags(t)
	requireProvideFailure(t, "provide-protocol-tests.cpp", map[string]func(string) string{
		"Protocol.h": func(source string) string {
			return strings.Replace(source,
				"  get(\"provide_extender_port_mapping_writable\", v.provide_extender_port_mapping_writable);\n",
				"", 1)
		},
	}, "provider stats: provide_extender_port_mapping_writable round-trips", extra...)
	requireProvideFailure(t, "provide-protocol-tests.cpp", map[string]func(string) string{
		"Protocol.h": func(source string) string {
			return strings.Replace(source,
				"  get(\"provide_extender_port_mapping\", v.provide_extender_port_mapping);\n", "", 1)
		},
	}, "provider stats: provide_extender_port_mapping round-trips on", extra...)
	// the tolerant reader every report uses, which leaves the default in place
	strict := `  auto it = j.find("provide_extender_port_mapping");
  if (it == j.end() || !it->is_boolean()) {
    throw std::runtime_error(
        "set_provide_extender_port_mapping requires provide_extender_port_mapping (a boolean)");
  }
  v.provide_extender_port_mapping = it->get<bool>();
`
	tolerant := `  if (auto it = j.find("provide_extender_port_mapping"); it != j.end() && !it->is_null())
    it->get_to(v.provide_extender_port_mapping);
`
	requireProvideFailure(t, "provide-protocol-tests.cpp", map[string]func(string) string{
		"Protocol.h": func(source string) string {
			return strings.Replace(source, strict, tolerant, 1)
		},
	}, "port mapping: a request without a boolean value is refused", extra...)
	requireProvideFailure(t, "provide-protocol-tests.cpp", map[string]func(string) string{
		"Protocol.h": func(source string) string {
			// the last is ProviderStats', after the request's own
			field := "  bool provide_extender_port_mapping = false;\n"
			at := strings.LastIndex(source, field)
			if at < 0 {
				return source
			}
			return source[:at] + "  bool provide_extender_port_mapping = true;\n" + source[at+len(field):]
		},
	}, "port mapping: and its setting as the default, off", extra...)
}
