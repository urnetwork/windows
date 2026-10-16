// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"regexp"
	"testing"
)

// SdkHost::SelectedLocation answers from the selection the presented
// controller's listener last reported, under a leaf lock (owner report
// 2026-10-16). The chooser sheet, the Network pane and the provider-locations
// sheet read it on every render on the UI thread, and it took mutex_, which
// the session worker holds through a whole connect, and with no presented
// controller read the DeviceRemote. Read with every comment blanked.
func TestSelectedLocationIsKeptByItsListener(t *testing.T) {
	host := stripComments(readAppSource(t, "SdkHost.cpp"))
	getter := functionBody(host, "std::optional<urnet::ConnectLocation> SdkHost::SelectedLocation(")
	if getter == "" {
		t.Fatal("SdkHost::SelectedLocation is missing")
	}
	quitRequireSequence(t, "SdkHost::SelectedLocation", getter,
		regexp.QuoteMeta("std::scoped_lock lock(selectedLocationMutex_);"),
		regexp.QuoteMeta("return selectedLocation_;"))
	quitForbid(t, "SdkHost::SelectedLocation", getter,
		"the sheets read it on every render, on the UI thread",
		"mutex_)", "connectVc_", "device_", "getConnectLocation", "getSelectedLocation")

	subscribe := functionBody(host, "void SdkHost::SubscribeStats(")
	quitRequireSequence(t, "SdkHost::SubscribeStats", subscribe,
		regexp.QuoteMeta("connectVc_ = device_->openConnectViewController();"),
		regexp.QuoteMeta("KeepSelectedLocation(connectVc_->getSelectedLocation());"),
		regexp.QuoteMeta("connectVc_->addSelectedLocationListener("),
		regexp.QuoteMeta("KeepSelectedLocation(std::move(location));"))

	closing := functionBody(host, "void SdkHost::ClosePresentationLocked(")
	quitRequireSequence(t, "SdkHost::ClosePresentationLocked", closing,
		regexp.QuoteMeta("if (sessionEnding || !device_) KeepSelectedLocation(std::nullopt);"))
}
