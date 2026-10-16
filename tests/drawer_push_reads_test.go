// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"regexp"
	"testing"
)

// The connect page's drawer pushes, its sheets as they open and the Network
// destination as it shows render from what the pushes carried, never from
// SdkHost's getters (owner report 2026-10-16). ConnectedProvidePeers,
// CurrentFilteredLocations, CurrentFilteredLocationState, ConnectedPeerCount,
// RemoteConnected and EnsureLocations took mutex_, which the session worker
// holds through a whole connect, and RemoteConnected and EnsureLocations call
// the DeviceRemote, whose state lock a held command keeps for the 60 s rpc
// timeout. Read with every comment blanked.

var drawerGetterReads = []string{
	"Sdk().RemoteConnected()", "Sdk().ConnectedPeerCount()", "Sdk().ConnectedProvidePeers()",
	"Sdk().CurrentFilteredLocations()", "Sdk().CurrentFilteredLocationState()",
}

func TestDrawerPushesReadNothingOnTheUiThread(t *testing.T) {
	page := stripComments(readAppSource(t, "ConnectPage.cpp"))
	why := "it takes mutex_ on the UI thread, behind a whole connect"
	for _, signature := range []string{
		"void ConnectPage::WireDrawerFeeds(",
		"void ConnectPage::ApplyPeerCount(",
		"winrt::fire_and_forget ConnectPage::ShowLocationChooserSheet(",
		"winrt::fire_and_forget ConnectPage::ShowProviderLocationsSheet(",
	} {
		body := functionBody(page, signature)
		if body == "" {
			t.Fatalf("%s is missing", signature)
		}
		quitForbid(t, signature, body, why, drawerGetterReads...)
	}
	feeds := functionBody(page, "void ConnectPage::WireDrawerFeeds(")
	remote := handlerSource(t, "ConnectPage.cpp", feeds, "sdk.SetRemoteChangedHandler(")
	quitRequireSequence(t, "the remote-changed push", remote,
		regexp.QuoteMeta("[queue, weak](bool remoteConnected)"),
		regexp.QuoteMeta("page.peerLine_.remoteConnected = remoteConnected;"),
		regexp.QuoteMeta("page.ApplyPeerCount(std::nullopt);"))
	peers := handlerSource(t, "ConnectPage.cpp", feeds, "sdk.SetPeersHandler(")
	quitRequireSequence(t, "the peers push", peers,
		regexp.QuoteMeta("page.ApplyPeerCount(peers);"), regexp.QuoteMeta("Sdk().RepublishStats();"),
		regexp.QuoteMeta("page.locationSheet_->Update(page.locations_, peers);"))
	locations := handlerSource(t, "ConnectPage.cpp", feeds, "sdk.SetLocationsHandler(")
	quitRequireSequence(t, "the locations push", locations,
		regexp.QuoteMeta("page.locations_ = locations;"),
		regexp.QuoteMeta("page.locationSheet_->Update(locations, page.peers_);"))
	apply := functionBody(page, "void ConnectPage::ApplyPeerCount(")
	quitRequireSequence(t, "ConnectPage::ApplyPeerCount", apply,
		regexp.QuoteMeta("if (reading) peerLine_ = *reading;"),
		regexp.QuoteMeta("if (!peerLine_.remoteConnected) {"),
		regexp.QuoteMeta("const int64_t count = peerLine_.connectedPeerCount;"))

	sheets := stripComments(readAppSource(t, "LocationSheets.cpp"))
	reconcile := functionBody(sheets, "void NetworkPage::ReconcileFeeds(")
	if reconcile == "" {
		t.Fatal("NetworkPage::ReconcileFeeds is missing")
	}
	quitForbid(t, "NetworkPage::ReconcileFeeds", reconcile, why, drawerGetterReads...)
}

// EnsureLocations hands its work to a worker of its own, which stops with the
// stats publisher before the teardown in the destructor and the quit.
func TestEnsureLocationsRunsOffTheCallersThread(t *testing.T) {
	host := stripComments(readAppSource(t, "SdkHost.cpp"))
	ensure := functionBody(host, "void SdkHost::EnsureLocations(")
	quitRequireSequence(t, "SdkHost::EnsureLocations", ensure, regexp.QuoteMeta("locationsOpener_.Request();"))
	quitForbid(t, "SdkHost::EnsureLocations", ensure,
		"its callers are on the UI thread", "scoped_lock", "EnsureLocationsLocked")
	job := functionBody(host, "void SdkHost::EnsureLocationsOnWorker(")
	quitRequireSequence(t, "SdkHost::EnsureLocationsOnWorker", job,
		`try \{`, regexp.QuoteMeta("std::scoped_lock lock(mutex_);"),
		regexp.QuoteMeta("EnsureLocationsLocked();"), `catch`)
	destructor := functionBody(host, "SdkHost::~SdkHost(")
	quitRequireSequence(t, "SdkHost's destructor", destructor,
		regexp.QuoteMeta("locationsOpener_.Stop();"), regexp.QuoteMeta("std::scoped_lock lock(mutex_);"))
	quit := functionBody(host, "void SdkHost::QuitInOrder(")
	quitRequireSequence(t, "SdkHost::QuitInOrder", quit,
		regexp.QuoteMeta("locationsOpener_.Stop();"), regexp.QuoteMeta("std::scoped_lock lock(mutex_);"))
	header := stripComments(readAppSource(t, "SdkHost.h"))
	if !regexp.MustCompile(regexp.QuoteMeta("CoalescedWorker locationsOpener_{[this] { EnsureLocationsOnWorker(); }};")).MatchString(header) {
		t.Error("SdkHost.h does not run EnsureLocationsOnWorker on locationsOpener_")
	}
}
