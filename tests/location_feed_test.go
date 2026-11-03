// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os/exec"
	"path/filepath"
	"regexp"
	"testing"
)

// The location picker came up blank or empty after a search (owner report
// 2026-11-02). The SDK filter is shared and outlives a sheet, so a new sheet's
// empty box sat over the last search's buckets; a failed load was never retried
// while its view controller stayed open; and a cache hit on the api path pushed
// nothing after a closing device feed had emptied the consumers. A C++ spec, and
// the wiring read with every comment blanked.

// Compile and execute the spec of App/LocationFeed.h.
func TestLocationFeedDecisions(t *testing.T) {
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("location feed tests require a C++20 compiler: ", err)
	}
	appDir := filepath.Join(repositoryRoot(t), "app")
	program := filepath.Join(t.TempDir(), "location-feed-tests")
	if output, err := exec.Command(compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror",
		"-I"+filepath.Join(appDir, "src", "App"),
		filepath.Join(appDir, "tools", "location-feed-tests.cpp"),
		"-o", program).CombinedOutput(); err != nil {
		t.Fatalf("build location feed tests: %v\n%s", err, output)
	}
	if output, err := exec.Command(program).CombinedOutput(); err != nil {
		t.Fatalf("location feed: %v\n%s", err, output)
	}
}

// Opening the sheet resets the shared filter, and the state reaches the sheet
// ahead of the rows it describes.
func TestLocationSheetOpensOnTheIdleFilter(t *testing.T) {
	page := stripComments(readAppSource(t, "ConnectPage.cpp"))
	show := functionBody(page, "winrt::fire_and_forget ConnectPage::ShowLocationChooserSheet(")
	quitRequireSequence(t, "ConnectPage::ShowLocationChooserSheet", show,
		regexp.QuoteMeta("Sdk().SetLocationFilter(std::string());"),
		regexp.QuoteMeta("locationSheet_->SetFeedState(locationsState_);"),
		regexp.QuoteMeta("locationSheet_->Update(locations_, peers_);"))
	feeds := functionBody(page, "void ConnectPage::WireDrawerFeeds(")
	locations := handlerSource(t, "ConnectPage.cpp", feeds, "sdk.SetLocationsHandler(")
	quitRequireSequence(t, "the locations push", locations,
		regexp.QuoteMeta("page.locationsState_ = state;"),
		regexp.QuoteMeta("page.locationSheet_->SetFeedState(state);"),
		regexp.QuoteMeta("page.locationSheet_->Update(locations, page.peers_);"))
}

// The Network destination reclaims the filter with what its own box holds.
func TestNetworkPageReappliesItsQuery(t *testing.T) {
	sheets := stripComments(readAppSource(t, "LocationSheets.cpp"))
	reconcile := functionBody(sheets, "void NetworkPage::ReconcileFeeds(")
	quitRequireSequence(t, "NetworkPage::ReconcileFeeds", reconcile,
		regexp.QuoteMeta("Sdk().SetLocationFilter(query_);"),
		regexp.QuoteMeta("Sdk().EnsureLocations();"))
}

// The filter is set off the UI thread, an open controller retries through the
// same arming, a new controller runs the held query, and a good cache pushes.
func TestLocationFilterRunsOnTheFeedWorker(t *testing.T) {
	host := stripComments(readAppSource(t, "SdkHost.cpp"))
	set := functionBody(host, "void SdkHost::SetLocationFilter(")
	quitRequireSequence(t, "SdkHost::SetLocationFilter", set, regexp.QuoteMeta("locationsOpener_.Request();"))
	quitForbid(t, "SdkHost::SetLocationFilter", set,
		"it runs per keystroke on the UI thread, and mutex_ is held through a whole connect",
		"lock(mutex_)", "filterLocations", "EnsureApiLocationsLocked")
	ensure := functionBody(host, "void SdkHost::EnsureLocationsLocked(")
	quitRequireSequence(t, "SdkHost::EnsureLocationsLocked", ensure,
		regexp.QuoteMeta("LocationFilterRunNeeded(queryChanged, locationsVc_->getFilteredLocationState())"),
		regexp.QuoteMeta("locationsVc_->filterLocations(WantedLocationQuery());"),
		regexp.QuoteMeta("const std::string wantedQuery = WantedLocationQuery();"),
		regexp.QuoteMeta("locationsVc_ = device_->openLocationsViewController();"),
		regexp.QuoteMeta("if (wantedQuery.empty()) {"),
		regexp.QuoteMeta("locationsVc_->start();"),
		regexp.QuoteMeta("locationsVc_->filterLocations(wantedQuery);"),
		regexp.QuoteMeta("if (seedLocations) {"))
	api := functionBody(host, "void SdkHost::EnsureApiLocationsLocked(")
	quitRequireSequence(t, "SdkHost::EnsureApiLocationsLocked", api,
		`if \(cacheAnswers\) \{\s*PublishApiLocations\(\);\s*return;`)
}
