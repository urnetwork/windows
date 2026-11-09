// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os/exec"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
)

// Compile and execute the portable DNS settings spec: the Fast DNS on connect
// toggle's settings mapping and its labels in the DNS editor and the drawer,
// the provider picker's sdk keys (App/DnsPresetPresentation.h), and the
// generated English strings both read.
func TestFastDnsOnConnectSetting(t *testing.T) {
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("dns settings tests require a C++20 compiler: ", err)
	}
	root := repositoryRoot(t)
	appDir := filepath.Join(root, "app")
	program := filepath.Join(t.TempDir(), "dns-settings-tests")
	build := exec.Command(compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror",
		"-I"+filepath.Join(appDir, "src", "App"),
		filepath.Join(appDir, "tools", "dns-settings-tests.cpp"), "-o", program)
	if output, err := build.CombinedOutput(); err != nil {
		t.Fatalf("build dns settings tests: %v\n%s", err, output)
	}
	if output, err := exec.Command(program, appDir).CombinedOutput(); err != nil {
		t.Fatalf("dns settings: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}
}

// The DNS editor's source between its section header and the next sheet's,
// comments blanked.
func dnsEditorSource(t *testing.T) string {
	t.Helper()
	source := readAppSource(t, "StatsSheets.cpp")
	start := strings.Index(source, "// ---- DnsEditorSheet")
	end := strings.Index(source, "// ---- TransportSettingsSheet")
	if start < 0 || end < start {
		t.Fatal("StatsSheets.cpp no longer has the DnsEditorSheet section before TransportSettingsSheet; update this contract")
	}
	return stripComments(source[start:end])
}

// The defect (connect ADBLOCK3RD 3.8, E3): the editor kept a draft of its own
// thirteen fields and built a fresh DnsResolverSettings from it on Update, so
// every save dropped what the draft did not model -- the mask then, and the
// preset selection and its personal id with the presets. The draft is the sdk
// struct the reading gave, every change to it starts from an sdk helper or
// edits that struct under Custom, and Update writes it whole.
func TestDnsEditorSavesTheSdkSettings(t *testing.T) {
	editor := dnsEditorSource(t)
	for _, forbidden := range []string{
		"DnsEditorSheet::ToSettings(",
		"DnsEditorSheet::FromSettings(",
		"struct Draft",
	} {
		if strings.Contains(editor, forbidden) {
			t.Errorf("the DNS editor still has %q: a draft of its own drops the fields it does not model", forbidden)
		}
	}
	fresh := regexp.MustCompile(`urnet::DnsResolverSettings\s+\w+\s*(;|\{\s*\})`)
	if match := fresh.FindString(editor); match != "" {
		t.Errorf("the DNS editor builds settings from scratch (%q); start from an sdk helper", match)
	}
	for _, want := range []string{
		// a pick, Custom and Fast DNS on connect through the sdk's helpers
		"urnet::dnsResolverSettingsWithPreset(draft_, pick.PresetId, std::string())",
		"urnet::dnsResolverSettingsAsCustom(draft_)",
		"urnet::dnsResolverSettingsWithFallback(self->draft_, on)",
		// a personal id normalized and checked by the sdk
		"urnet::validateDnsPresetPersonalId(pick.PresetId, input)",
		"urnet::normalizeDnsPresetPersonalId(pick.PresetId, input)",
		// Update writes the draft whole, over the reading it opened on
		"self->sdk_.ApplyDnsSettings(self->opened_, *self->draft_);",
		// the picks in the sdk's order
		"urnet::getDnsPicks(countryCode_)",
	} {
		if !strings.Contains(editor, want) {
			t.Errorf("the DNS editor is missing %q", want)
		}
	}
	header := stripComments(readAppSource(t, "StatsSheets.h"))
	if !strings.Contains(header, "std::optional<urnet::DnsResolverSettings> draft_;") {
		t.Error("StatsSheets.h: the DNS editor's draft is not the sdk's DnsResolverSettings")
	}
}

// The dns card's pill came from the app's own whole-value comparison
// (DnsSettingsEquivalent) against the regional recommendation or the defaults,
// so every preset would have read "The default safe settings are not applied"
// (ADBLOCK3RD F2). The selection row, the status line and the pill are the
// sdk's summaries of the settings and the resolver's status.
func TestDnsCardShowsTheSdkSummary(t *testing.T) {
	page := stripComments(readAppSource(t, "ConnectPage.cpp"))
	if strings.Contains(page, "DnsSettingsEquivalent") {
		t.Error("ConnectPage.cpp still compares dns settings itself; the pill is the sdk's (describeDnsResolverSettings)")
	}
	summary := functionBody(page, "void ConnectPage::ApplyDnsSummary(")
	if summary == "" {
		t.Fatal("ConnectPage.cpp no longer defines ApplyDnsSummary; update this contract")
	}
	for _, want := range []string{
		"urnet::describeDnsResolverSettings(dnsSettings_, ToLower(countryCode_))",
		"urnet::describeDnsResolverStatus(dnsSettings_, dnsStatus_)",
		"dnspresets::PillOf(summary->PillKey)",
		"summary->SummaryKey",
	} {
		if !strings.Contains(summary, want) {
			t.Errorf("ConnectPage::ApplyDnsSummary is missing %q", want)
		}
	}
}

// The DNS editor's blocked lookups count is read off the UI thread, by the
// session's device handle (the client limit feed's shape): the bootstrap sets
// the handle and its session generation and the teardown clears them, under
// their own leaf lock, before the device closes; the read never goes through
// device_ or mutex_, and drops a count of a session that ended or was
// replaced while it was out; the reader stops before the destructor and the
// quit take mutex_. The editor takes the status's count with each status, so a
// reconnect's zero replaces a count read before it.
func TestDnsBlockedCountReaderLifecycle(t *testing.T) {
	host := stripComments(readAppSource(t, "SdkHost.cpp"))
	bootstrap := functionBody(host, "bool SdkHost::BootstrapSession(")
	if bootstrap == "" {
		t.Fatal("SdkHost.cpp no longer defines BootstrapSession; update this contract")
	}
	quitRequireSequence(t, "SdkHost::BootstrapSession", bootstrap,
		regexp.QuoteMeta("const std::uint64_t generation = sessionGeneration_;"),
		regexp.QuoteMeta("std::scoped_lock lock(dnsBlockedCountMutex_);"),
		regexp.QuoteMeta("dnsBlockedCountDevice_ = device_->handle();"),
		regexp.QuoteMeta("dnsBlockedCountGeneration_ = generation;"))
	teardown := definitionBody(t, "SdkHost.cpp", host, "void SdkHost::TeardownSessionLocked(bool stopTunnel) {")
	quitRequireSequence(t, "SdkHost::TeardownSessionLocked", teardown,
		regexp.QuoteMeta("std::scoped_lock lock(dnsBlockedCountMutex_);"),
		regexp.QuoteMeta("dnsBlockedCountDevice_ = 0;"),
		regexp.QuoteMeta("dnsBlockedCountGeneration_ = 0;"),
		regexp.QuoteMeta("if (device_) { device_->close(); device_.reset(); }"))

	reader := functionBody(host, "void SdkHost::ReadDnsBlockedCountOnWorker(")
	quitRequireSequence(t, "SdkHost::ReadDnsBlockedCountOnWorker", reader,
		regexp.QuoteMeta("device = dnsBlockedCountDevice_;"),
		regexp.QuoteMeta("generation = dnsBlockedCountGeneration_;"),
		regexp.QuoteMeta("const int64_t count = urnet_device_get_dns_blocked_count(device);"),
		// the delivery's own lock, the second of the read
		regexp.QuoteMeta("std::scoped_lock lock(dnsBlockedCountMutex_);")+`\s*`+
			regexp.QuoteMeta("if (generation == 0 || generation != dnsBlockedCountGeneration_) return;"),
		regexp.QuoteMeta("onDnsBlockedCount_(count);"))
	quitForbid(t, "SdkHost::ReadDnsBlockedCountOnWorker", reader,
		"the read runs off mutex_ and by the device's handle, so a teardown cannot free what it reads",
		"device_->", "lock(mutex_")

	for _, signature := range []string{"SdkHost::~SdkHost(", "void SdkHost::QuitInOrder("} {
		quitRequireSequence(t, signature, functionBody(host, signature),
			regexp.QuoteMeta("dnsBlockedCountReader_.Stop();"), regexp.QuoteMeta("std::scoped_lock lock(mutex_);"))
	}
	if n := strings.Count(host, "dnsBlockedCountReader_.Stop();"); n != 2 {
		t.Errorf("SdkHost.cpp stops the blocked count reader in %d places, want 2 (the destructor and the quit)", n)
	}
	header := stripComments(readAppSource(t, "SdkHost.h"))
	for _, want := range []string{
		"void RequestDnsBlockedCount() { dnsBlockedCountReader_.Request(); }",
		"CoalescedWorker dnsBlockedCountReader_{[this] { ReadDnsBlockedCountOnWorker(); }};",
	} {
		if !strings.Contains(header, want) {
			t.Errorf("SdkHost.h is missing %q", want)
		}
	}

	setStatus := functionBody(dnsEditorSource(t), "void DnsEditorSheet::SetStatus(")
	quitRequireSequence(t, "DnsEditorSheet::SetStatus", setStatus,
		regexp.QuoteMeta("status_ = std::move(status);"),
		regexp.QuoteMeta("blockedCount_ = status_ ? std::optional<int64_t>{status_->BlockedCount} : std::nullopt;"),
		regexp.QuoteMeta("RenderStatus();"))
}
