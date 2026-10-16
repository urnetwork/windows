// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
)

// The show path reads no device on the UI thread (owner report 2026-10-16): a
// window built or signed in seeded the connect drawer with six DeviceRemote
// getters (ConnectPage::ResyncDrawer, SeedConnectControls), and the Settings,
// Earnings and onboarding pages read the device's kill switch and provide
// mode as they showed, the Earnings page on every stats push. A held command
// keeps the device's state lock for the 60 s rpc timeout. The device-only
// settings are now read off the UI thread, ordered against their listeners'
// pushes (App/SeedOrder.h); the three the app stores are read from
// LocalState. A C++ spec with negative controls, and wiring read with every
// comment blanked.

func seedOrderProgram(t *testing.T, mutate func(string) string) string {
	t.Helper()
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("seed order tests require a C++20 compiler: ", err)
	}
	root := repositoryRoot(t)
	includeDir := t.TempDir()
	source, err := os.ReadFile(filepath.Join(root, "app", "src", "App", "SeedOrder.h"))
	if err != nil {
		t.Fatal(err)
	}
	content := string(source)
	if mutate != nil {
		content = mutate(content)
		if content == string(source) {
			t.Fatal("negative control did not change the production SeedOrder.h")
		}
	}
	if err := os.WriteFile(filepath.Join(includeDir, "SeedOrder.h"), []byte(content), 0600); err != nil {
		t.Fatal(err)
	}
	program := filepath.Join(includeDir, "seed-order-tests")
	build := exec.Command(compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I"+includeDir,
		filepath.Join(root, "app", "tools", "seed-order-tests.cpp"), "-o", program)
	if output, err := build.CombinedOutput(); err != nil {
		t.Fatalf("build seed order tests: %v\n%s", err, output)
	}
	return program
}

func TestSeedOrder(t *testing.T) {
	if !strings.Contains(readAppSource(t, "App.vcxproj"), `<ClInclude Include="SeedOrder.h" />`) {
		t.Error("App.vcxproj does not list SeedOrder.h")
	}
	if output, err := exec.Command(seedOrderProgram(t, nil)).CombinedOutput(); err != nil {
		t.Fatalf("seed order: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}
}

func requireSeedOrderFailure(t *testing.T, old, replacement, want string) {
	t.Helper()
	program := seedOrderProgram(t, func(source string) string {
		if strings.Count(source, old) != 1 {
			t.Fatalf("SeedOrder.h no longer has exactly one %q; update this control", old)
		}
		return strings.Replace(source, old, replacement, 1)
	})
	output, err := exec.Command(program).CombinedOutput()
	if err == nil || !strings.Contains(string(output), want) {
		t.Fatalf("negative control was not detected (want %q): %v\n%s", want, err, output)
	}
}

// A read that applies whatever came since would roll a newer push back.
func TestSeedOrderRejectsAReadOverANewerPush(t *testing.T) {
	requireSeedOrderFailure(t, "bool Admits() const { return pushes_ == asked_; }",
		"bool Admits() const { return true; }",
		"a push after the request is newer than the read: the read is dropped")
}

// A request that forgets the pushes before it drops a read that carries them.
func TestSeedOrderRejectsARequestThatForgetsEarlierPushes(t *testing.T) {
	requireSeedOrderFailure(t, "void Asked() { asked_ = pushes_; }", "void Asked() {}",
		"a push before the request is in the read: the read applies")
}

func TestShowPathReadsNoDeviceOnTheUiThread(t *testing.T) {
	deviceReads := []string{
		"CurrentDnsSettings(", "CurrentTransportSettings(", "CurrentPerformanceSettings(",
		"CurrentBlockerEnabled(", "CurrentProvideControlMode(", "CurrentKillSwitch(",
	}
	why := "it calls the DeviceRemote on the UI thread, behind a held command"
	page := stripComments(readAppSource(t, "ConnectPage.cpp"))
	for _, signature := range []string{"void ConnectPage::ResyncDrawer(", "void ConnectPage::SeedConnectControls("} {
		quitForbid(t, signature, functionBody(page, signature), why, deviceReads...)
	}
	quitRequireSequence(t, "ConnectPage::ResyncDrawer", functionBody(page, "void ConnectPage::ResyncDrawer("),
		regexp.QuoteMeta("dnsSettings_.reset();"), regexp.QuoteMeta("dnsOrder_.Asked();"),
		regexp.QuoteMeta("blockerOrder_.Asked();"), regexp.QuoteMeta("sdk.RequestDrawerSeed();"))
	quitRequireSequence(t, "ConnectPage::SeedConnectControls", functionBody(page, "void ConnectPage::SeedConnectControls("),
		regexp.QuoteMeta("Sdk().StoredPerformanceSettings();"), regexp.QuoteMeta("Sdk().StoredProvideControlMode();"))
	feeds := functionBody(page, "void ConnectPage::WireDrawerFeeds(")
	quitRequireSequence(t, "the DNS push", handlerSource(t, "ConnectPage.cpp", feeds, "sdk.SetDnsSettingsHandler("),
		regexp.QuoteMeta("page.dnsOrder_.Pushed();"))
	quitRequireSequence(t, "the transport push", handlerSource(t, "ConnectPage.cpp", feeds, "sdk.SetTransportSettingsHandler("),
		regexp.QuoteMeta("page.providerTransportOrder_.Pushed();"), regexp.QuoteMeta("page.clientTransportOrder_.Pushed();"))
	quitRequireSequence(t, "the blocker push", handlerSource(t, "ConnectPage.cpp", feeds, "sdk.SetBlockerEnabledHandler("),
		regexp.QuoteMeta("page.blockerOrder_.Pushed();"))
	quitRequireSequence(t, "the drawer seed", handlerSource(t, "ConnectPage.cpp", feeds, "sdk.SetDrawerSeedHandler("),
		regexp.QuoteMeta("if (page.dnsOrder_.Admits()) {"), regexp.QuoteMeta("if (page.clientTransportOrder_.Admits()) {"),
		regexp.QuoteMeta("if (page.providerTransportOrder_.Admits()) {"), regexp.QuoteMeta("if (page.blockerOrder_.Admits())"))

	for _, c := range []struct{ file, signature, stored string }{
		{"SettingsPage.cpp", "void SettingsPage::ApplyLocalDeviceState(", "Sdk().StoredKillSwitch()"},
		{"WalletPage.cpp", "void WalletPage::ApplyStrings(", "Sdk().StoredProvideControlMode()"},
		{"WalletPage.cpp", "void WalletPage::ApplyProvideState(", "Sdk().StoredProvideControlMode()"},
		{"Onboarding.cpp", "StackPanel Onboarding::BuildProviding(", "Sdk().StoredProvideControlMode()"},
	} {
		body := functionBody(stripComments(readAppSource(t, c.file)), c.signature)
		if body == "" {
			t.Fatalf("%s is missing", c.signature)
		}
		quitForbid(t, c.signature, body, why, deviceReads...)
		if !strings.Contains(body, c.stored) {
			t.Errorf("%s does not read %s", c.signature, c.stored)
		}
	}

	host := stripComments(readAppSource(t, "SdkHost.cpp"))
	for _, signature := range []string{
		"bool SdkHost::StoredKillSwitch(", "std::string SdkHost::StoredProvideControlMode(",
		"PerformanceSettings SdkHost::StoredPerformanceSettings(",
	} {
		body := functionBody(host, signature)
		if body == "" || !strings.Contains(body, "localState_") {
			t.Errorf("%s does not read the stored value", signature)
		}
		quitForbid(t, signature, body, "it must not reach the device", "device_", "mutex_)")
	}
	quitRequireSequence(t, "SdkHost::ReadDrawerSeedOnWorker", functionBody(host, "void SdkHost::ReadDrawerSeedOnWorker("),
		regexp.QuoteMeta("seed.dns = CurrentDnsSettings();"), regexp.QuoteMeta("seed.blockerEnabled = CurrentBlockerEnabled();"),
		regexp.QuoteMeta("onDrawerSeed_(std::move(seed));"))
	for _, signature := range []string{"SdkHost::~SdkHost(", "void SdkHost::QuitInOrder("} {
		quitRequireSequence(t, signature, functionBody(host, signature),
			regexp.QuoteMeta("drawerSeedReader_.Stop();"), regexp.QuoteMeta("std::scoped_lock lock(mutex_);"))
	}
	header := stripComments(readAppSource(t, "SdkHost.h"))
	for _, want := range []string{
		"void RequestDrawerSeed() { drawerSeedReader_.Request(); }",
		"CoalescedWorker drawerSeedReader_{[this] { ReadDrawerSeedOnWorker(); }};",
	} {
		if !strings.Contains(header, want) {
			t.Errorf("SdkHost.h is missing %q", want)
		}
	}
}
