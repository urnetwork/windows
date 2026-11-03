// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"strings"
	"testing"
)

// The lag.dev speed tests switch (lag.dev/PLAN.md C4, decision 18): under the
// Connect page's Extender switch, whether the extender role serves lag.dev's
// measurement API, on by default. It is the Extender switch's twin: read beside
// the role's status, written through a session's device, and with no session
// through the service (set_measurements_enabled) when its get_provider_stats
// said it takes it. The pure parts run in extender-tests.cpp and
// provide-protocol-tests.cpp; these read the service and app sources, which
// need Windows and WinRT, with comments stripped so prose cannot satisfy a
// contract.

// The service answers the verb on the one control pipe, under the session lock
// in budget, writes where the Extender switch's rule says, and reports the
// setting beside the role, never without it.
func TestSpeedTestsSwitchServiceWiring(t *testing.T) {
	server := stripComments(readServiceSource(t, "ControlServer.cpp"))
	handle := definitionBody(t, "ControlServer.cpp", server, "nlohmann::json ControlServer::Handle(")
	branch := sourceBetween(t, "ControlServer::Handle", handle,
		"type == proto::msg::kSetMeasurementsEnabled", "} else if")
	provideRequireOrder(t, "the set_measurements_enabled branch", branch,
		"request.get<proto::SetMeasurementsEnabled>()",
		"reply.ok = tunnel_.SetMeasurementsEnabled(req.measurements_enabled, error);")
	provideRequire(t, "the set_measurements_enabled branch", branch, "reply.error = error;")

	source := tunnelControllerSource(t)
	write := definitionBody(t, "TunnelController.cpp", source,
		"bool TunnelController::SetMeasurementsEnabled(bool on, std::string& error)")
	// a wedged bring-up refuses it in budget rather than holding the pipe
	provideRequireOrder(t, "SetMeasurementsEnabled", write,
		"lock.try_lock_for(kStopLockBudget)", "provide::ExtenderSettingTargetFor(")
	provideRequire(t, "SetMeasurementsEnabled", write,
		"providerDevice_ != nullptr, device_ != nullptr, spaceManager_ && lastSpaceKey_)",
		"device_->setMeasurementsEnabled(on);",
		"spaceManager_->getNetworkSpace(lastSpaceKey_)",
		".getAsyncLocalState().getLocalState().setMeasurementsEnabled(on);")
	// the provider-only device's next answer carries the write
	provideRequireOrder(t, "SetMeasurementsEnabled", write,
		"providerDevice_->setMeasurementsEnabled(on);", "RefreshProviderMeasurementsLocked();")
	refused := sourceBetween(t, "SetMeasurementsEnabled", write,
		"case provide::ExtenderSettingTarget::None:", "")
	provideRequireOrder(t, "the refused target", refused, "error = ", "return false;")
	// it writes its own setting and nothing else
	requireNone(t, "SetMeasurementsEnabled", write, "setProvideExtender(")

	refresh := definitionBody(t, "TunnelController.cpp", source,
		"void TunnelController::RefreshProviderMeasurementsLocked()")
	// the device is read before the statistics lock, never under it
	provideRequireOrder(t, "RefreshProviderMeasurementsLocked", refresh,
		"providerDevice_->getMeasurementsEnabled()", "std::scoped_lock lock(providerStatsMutex_);")
	locked := sourceBetween(t, "RefreshProviderMeasurementsLocked", refresh,
		"std::scoped_lock lock(providerStatsMutex_);", "")
	provideRequireOrder(t, "RefreshProviderMeasurementsLocked", locked,
		"if (!providerExtender_) return;", "providerMeasurementsSetting_ = setting;")
	requireNone(t, "RefreshProviderMeasurementsLocked under its lock", locked, "providerDevice_")

	// read with the role when the statistics open, so a reading that fails
	// reports neither
	open := definitionBody(t, "TunnelController.cpp", source,
		"void TunnelController::OpenProviderStatsLocked()")
	provideRequire(t, "OpenProviderStatsLocked", open,
		"measurementsSetting = providerDevice_->getMeasurementsEnabled();",
		"providerMeasurementsSetting_ = measurementsSetting;")
	provideRequireOrder(t, "OpenProviderStatsLocked", open,
		"extenderSetting = providerDevice_->getProvideExtender();",
		"measurementsSetting = providerDevice_->getMeasurementsEnabled();")

	stats := definitionBody(t, "TunnelController.cpp", source,
		"proto::ProviderStats TunnelController::ProviderStats()")
	beside := sourceBetween(t, "ProviderStats", stats, "if (providerExtender_) {", "return stats;")
	provideRequire(t, "ProviderStats beside the role", beside,
		"stats.measurements_enabled = providerMeasurementsSetting_;",
		"stats.measurements_enabled_writable = true;")
}

// The app reads the setting where it reads the Extender switch's, writes it by
// the switch's own route, and draws the row hidden, never disabled, and never
// writes it while hidden.
func TestSpeedTestsSwitchAppWiring(t *testing.T) {
	source := sdkHostSource(t)
	// beside the role's status: from the device with a session...
	publish := definitionBody(t, "SdkHost.cpp", source,
		"void SdkHost::PublishExtenderProvideStatus(std::optional<urnet::ExtenderProvideStatus> status)")
	provideRequire(t, "PublishExtenderProvideStatus", publish,
		"device_->getMeasurementsEnabled()",
		"return lastExtenderProvideStatus_.measurementsEnabled;",
		"ExtenderProvideStatusViewOf(status, readSetting, readMeasurements)")
	// ...and with none from the service's answer, with its say on the write
	show := definitionBody(t, "SdkHost.cpp", source,
		"void SdkHost::ShowProviderOnlyStatsLocked(const proto::ProviderStats& stats)")
	provideRequire(t, "ShowProviderOnlyStatsLocked", show,
		"return stats.measurements_enabled;",
		"extender.measurementsServiceWritable = stats.measurements_enabled_writable;")
	provideRequireOrder(t, "ShowProviderOnlyStatsLocked", show,
		"extender.measurementsServiceWritable = stats.measurements_enabled_writable;",
		"PublishExtenderProvideView(std::move(extender));")

	// the device, or the service off the UI thread, by the switch's own route
	set := definitionBody(t, "SdkHost.cpp", source, "void SdkHost::SetMeasurementsEnabled(bool on)")
	provideRequire(t, "SdkHost::SetMeasurementsEnabled", set,
		"SpeedTestsWriteRouteFor(device_.has_value(), shown)",
		"QueueProviderOnlyMeasurementsWrite(on);",
		"device_->setMeasurementsEnabled(on);",
		"extenderProvideRepublish_ = true;")
	requireNone(t, "SdkHost::SetMeasurementsEnabled", set,
		"service_.SetMeasurementsEnabled(", "ExtenderProvideWriteRouteFor(")
	written := definitionBody(t, "SdkHost.cpp", source,
		"void SdkHost::WriteProviderOnlyMeasurements(bool on)")
	provideRequireOrder(t, "WriteProviderOnlyMeasurements", written,
		"service_.SetMeasurementsEnabled(on, &error)", "extenderProvideRepublish_ = true;")
	loop := definitionBody(t, "SdkHost.cpp", source, "void SdkHost::ProviderOnlyStatsLoop()")
	provideRequire(t, "ProviderOnlyStatsLoop", loop,
		"measurementsWrite = std::exchange(providerOnlyMeasurementsWrite_, std::nullopt);")
	// sent before this pass reads, so its answer carries the write
	provideRequireOrder(t, "ProviderOnlyStatsLoop", loop,
		"if (measurementsWrite) WriteProviderOnlyMeasurements(*measurementsWrite);",
		"service_.GetProviderStats(stats)")

	// the Connect page
	connect := stripComments(readAppSource(t, "ConnectPage.cpp"))
	row := definitionBody(t, "ConnectPage.cpp", connect, "void ConnectPage::ApplySpeedTestsRow()")
	provideRequire(t, "ConnectPage::ApplySpeedTestsRow", row,
		"urnw::SpeedTestsRowModelFor(extenderProvideView_)",
		"w_.SpeedTestsRow().Visibility(model.visible ? Visibility::Visible : Visibility::Collapsed);",
		"updatingControls_ = true;")
	requireNone(t, "ConnectPage::ApplySpeedTestsRow", row, "IsEnabled(", "SetMeasurementsEnabled(")
	extenderRow := definitionBody(t, "ConnectPage.cpp", connect,
		"void ConnectPage::ApplyExtenderProvideRow()")
	provideRequire(t, "ConnectPage::ApplyExtenderProvideRow", extenderRow, "ApplySpeedTestsRow();")
	toggled := definitionBody(t, "ConnectPage.cpp", connect, "void ConnectPage::OnSpeedTestsToggled(")
	provideRequireOrder(t, "ConnectPage::OnSpeedTestsToggled", toggled,
		"if (updatingControls_) return;",
		"if (!urnw::SpeedTestsRowModelFor(extenderProvideView_).visible) return;")
	provideRequireOrder(t, "ConnectPage::OnSpeedTestsToggled", toggled,
		"if (!urnw::SpeedTestsRowModelFor(extenderProvideView_).visible) return;",
		"Sdk().SetMeasurementsEnabled(on);")
	applyStrings := definitionBody(t, "ConnectPage.cpp", connect, "void ConnectPage::ApplyStrings()")
	provideRequire(t, "ConnectPage::ApplyStrings", applyStrings,
		`w_.SpeedTestsLabel().Text(Loc("extender_speed_tests"));`,
		`w_.SpeedTestsDescription().Text(Loc("extender_speed_tests_description"));`)

	// the markup: the row under the Extender description, its title naming
	// the switch, and its handler
	xaml := readAppSource(t, "MainWindow.xaml")
	provideRequire(t, "MainWindow.xaml", xaml,
		`x:Name="SpeedTestsRow" Visibility="Collapsed"`,
		`x:Name="SpeedTestsLabel"`,
		`AutomationProperties.LabeledBy="{Binding ElementName=SpeedTestsLabel}"`,
		`Toggled="OnSpeedTestsToggled"`)
	provideRequireOrder(t, "MainWindow.xaml", xaml,
		`x:Name="ExtenderDescriptionRow"`, `x:Name="SpeedTestsRow"`)
	window := readAppSource(t, "MainWindow.xaml.cpp")
	provideRequire(t, "MainWindow.xaml.cpp", window, "connect_->OnSpeedTestsToggled(s, e);")
}

// The spec catches each way the switch could show where its write has no
// taker, write wider than it shows, or lose its position: a speed tests switch
// over a service that takes only the Extender switch's write would be dead
// (N1).
func TestExtenderPresentationRejectsABrokenSpeedTestsSwitch(t *testing.T) {
	requireExtenderPresentationFailure(t, "ExtenderPresentation.cpp", func(source string) string {
		return strings.Replace(source,
			"(!view.providerOnly || view.measurementsServiceWritable);", "true;", 1)
	}, "a service that takes only the Extender switch's write: hidden")
	requireExtenderPresentationFailure(t, "ExtenderPresentation.cpp", func(source string) string {
		return strings.Replace(source,
			"  if (shown.supported && shown.providerOnly && shown.serviceWritable &&\n"+
				"      shown.measurementsServiceWritable) {",
			"  if (shown.supported && shown.providerOnly && shown.serviceWritable) {", 1)
	}, "a service that takes only the Extender switch's write is not")
	requireExtenderPresentationFailure(t, "ExtenderPresentation.cpp", func(source string) string {
		return strings.Replace(source,
			"  guess.measurementsEnabled = current.measurementsEnabled;\n", "", 1)
	}, "flipping the Extender switch leaves the speed tests switch where it was")
	requireExtenderPresentationFailure(t, "ExtenderPresentation.h", func(source string) string {
		return strings.Replace(source, "  view.measurementsEnabled = readMeasurements();\n", "", 1)
	}, "which the speed tests switch shows")
}
