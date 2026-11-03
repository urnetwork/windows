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

// The network country (open bug P052): the country of the mobile broadband
// network carrying the default route, which the sdk's extender dials fall back
// to while the extender hint cannot be fetched. The pure parts run in
// app/tools/network-country-tests.cpp (the decision and the watch) and
// network-country-protocol-tests.cpp (the wire) against the production
// headers; the reader, SdkHost, ServiceClient and the service need Windows
// (and the app WinRT), so what they must keep doing is read off their sources,
// with comments stripped so prose cannot satisfy a contract.

// The headers the harnesses include, by their directory under app/src. They
// are copied into a fixture tree of the same shape (NetworkCountryWatch.h
// reaches the shared coalescer as ../Service/NetworkChangeNotify.h), so
// `mutate`, keyed by file name, can rewrite one for a negative control.
var networkCountryHeaderDirs = map[string]string{
	"NetworkCountry.h":      "Common",
	"Protocol.h":            "Common",
	"ProvideLifecycle.h":    "Common",
	"DiagnosticLines.h":     "Common",
	"NetworkCountryWatch.h": "App",
	"NetworkChangeNotify.h": "Service",
}

const (
	networkCountryHarness         = "network-country-tests.cpp"
	networkCountryProtocolHarness = "network-country-protocol-tests.cpp"
)

// Builds `harness` against a fixture copy of the headers, `mutate` applied, and
// returns the program's path.
func networkCountryTestProgram(t *testing.T, harness string, mutate map[string]func(string) string) string {
	t.Helper()
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("network country tests require a C++20 compiler: ", err)
	}
	root := repositoryRoot(t)
	fixture := t.TempDir()
	for name, dir := range networkCountryHeaderDirs {
		source, err := os.ReadFile(filepath.Join(root, "app", "src", dir, name))
		if err != nil {
			t.Fatal(err)
		}
		content := string(source)
		if change := mutate[name]; change != nil {
			content = change(content)
			if content == string(source) {
				t.Fatalf("negative control did not change the production %s", name)
			}
		}
		if err := os.MkdirAll(filepath.Join(fixture, dir), 0700); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(filepath.Join(fixture, dir, name), []byte(content), 0600); err != nil {
			t.Fatal(err)
		}
	}
	program := filepath.Join(fixture, strings.TrimSuffix(harness, ".cpp"))
	args := []string{"-std=c++20", "-pthread", "-Wall", "-Wextra", "-Werror",
		"-I" + filepath.Join(fixture, "Common"), "-I" + filepath.Join(fixture, "App")}
	if harness == networkCountryProtocolHarness {
		args = append(args, provideJsonFlags(t)...)
	}
	args = append(args, filepath.Join(root, "app", "tools", harness), "-o", program)
	if output, err := exec.Command(compiler, args...).CombinedOutput(); err != nil {
		t.Fatalf("build %s: %v\n%s", harness, err, output)
	}
	return program
}

// Builds and runs `harness` against the production headers; it must pass.
func runNetworkCountryHarness(t *testing.T, harness string) {
	t.Helper()
	program := networkCountryTestProgram(t, harness, nil)
	if output, err := exec.Command(program).CombinedOutput(); err != nil {
		t.Fatalf("%s: %v\n%s", harness, err, output)
	} else {
		t.Logf("%s", output)
	}
}

// Execute the spec: the table, the provider id, the election, the adapter and
// registration rules, the decision, what the service makes of a payload, and
// the watch's thread.
func TestNetworkCountry(t *testing.T) {
	runNetworkCountryHarness(t, networkCountryHarness)
}

// Execute the wire spec: the verb, the start fields and an older app's
// silence, and the provider-only device kept when only the country moves.
func TestNetworkCountryProtocol(t *testing.T) {
	runNetworkCountryHarness(t, networkCountryProtocolHarness)
}

// One negative control: a rewrite of the production headers, and the failure
// line the harness must print for it.
type networkCountryControl struct {
	name   string
	mutate map[string]func(string) string
	want   string
}

// Runs `harness` against each control's rewritten headers and reports, by
// name, every control the harness does not fail on with the wanted line.
func requireNetworkCountryFailures(t *testing.T, harness string, controls []networkCountryControl) {
	t.Helper()
	for _, control := range controls {
		program := networkCountryTestProgram(t, harness, control.mutate)
		output, err := exec.Command(program).CombinedOutput()
		if err == nil || !strings.Contains(string(output), control.want) {
			t.Errorf("negative control %q was not detected (want %q): %v\n%s",
				control.name, control.want, err, output)
		}
	}
}

// A rewrite that replaces the first `old` in a header with `new`.
func replaceOnce(old, new string) func(string) string {
	return func(source string) string { return strings.Replace(source, old, new, 1) }
}

// Each rule of the decision, put back the way that reports a wrong country.
func TestNetworkCountryRejectsDishonestRules(t *testing.T) {
	requireNetworkCountryFailures(t, networkCountryHarness, []networkCountryControl{
		{
			name: "a country for Wi-Fi",
			mutate: map[string]func(string) string{"NetworkCountry.h": replaceOnce(
				"if (!facts.mobileBroadband) return none(kSourceNotMobileBroadband);",
				"if (!facts.mobileBroadband) return Reading{.code = \"us\", .source = std::string(kSourceUnknown)};")},
			want: "decision: Wi-Fi or Ethernet, no country (never the locale)",
		},
		{
			name: "a CDMA system id read as an MCC",
			mutate: map[string]func(string) string{"NetworkCountry.h": replaceOnce(
				"return (dataClass & kDataClass3gpp) != 0 && (dataClass & kDataClass3gpp2) == 0;",
				"return (dataClass & kDataClass3gpp) != 0;")},
			want: "data class: a mixed 3GPP and CDMA class is not 3GPP",
		},
		{
			name: "a searching modem taken as registered",
			mutate: map[string]func(string) string{"NetworkCountry.h": replaceOnce(
				"return registerState == kRegisterStateHome ||",
				"return registerState == 2 || registerState == kRegisterStateHome ||")},
			want: "registration: state 2 is not registered",
		},
		{
			name: "a link that is down elected",
			mutate: map[string]func(string) string{"NetworkCountry.h": replaceOnce(
				"if (!route.connected || route.ifIndex == 0) continue;",
				"if (route.ifIndex == 0) continue;")},
			want: "election: a link that is down carries nothing",
		},
		{
			name: "a provider id of any length",
			mutate: map[string]func(string) string{"NetworkCountry.h": replaceOnce(
				"if (providerId.size() != 5 && providerId.size() != 6) return {};",
				"if (providerId.size() < 5) return {};")},
			want: "provider id: \"2500123\" is not MCC+MNC",
		},
	})
}

// The wire: a start_tunnel reader that drops the country, and a provider-only
// device rebuilt for a new country instead of taking it in place.
func TestNetworkCountryRejectsWireRegressions(t *testing.T) {
	requireNetworkCountryFailures(t, networkCountryProtocolHarness, []networkCountryControl{
		{
			name: "start_tunnel drops the country",
			mutate: map[string]func(string) string{"Protocol.h": replaceOnce(
				"  get(\"network_country_code\", v.network_country_code);\n", "")},
			want: "pipe: start_tunnel carries the network country",
		},
		{
			name: "a new country rebuilds the provider-only device",
			mutate: map[string]func(string) string{"Protocol.h": replaceOnce(
				"  return provide::SameIdentity(IdentityOf(a), IdentityOf(b));\n}",
				"  return provide::SameIdentity(IdentityOf(a), IdentityOf(b)) &&\n"+
					"         a.network_country_code == b.network_country_code;\n}")},
			want: "pipe: a new country keeps the running provider-only device",
		},
	})
}

// The watch: a report for every read, a destructor that does not wait for the
// thread, one that waits for a read without a bound, a cancel that does not
// wait out a running report, a read reported after the cancel, and a first
// wait that returns at once.
func TestNetworkCountryRejectsWatchRegressions(t *testing.T) {
	watch := func(old, new string) map[string]func(string) string {
		return map[string]func(string) string{"NetworkCountryWatch.h": replaceOnce(old, new)}
	}
	requireNetworkCountryFailures(t, networkCountryHarness, []networkCountryControl{
		{
			name:   "every reading reported",
			mutate: watch("if (!reported || *reported != reading) {", "if (true) {"),
			want:   "burst: an unchanged reading is not reported again",
		},
		{
			name: "destruction does not wait for the thread",
			mutate: watch(
				"return channel_->wake.wait_for(lock, readJoinBudget_, [this] { return channel_->finished; });",
				"return channel_->finished && readJoinBudget_.count() >= 0;"),
			want: "join: destruction returns only after the read already running has returned",
		},
		{
			name: "destruction waits for a read without a bound",
			mutate: watch(
				"return channel_->wake.wait_for(lock, readJoinBudget_, [this] { return channel_->finished; });",
				"channel_->wake.wait(lock, [this] { return channel_->finished; });\n      return readJoinBudget_.count() >= 0;"),
			want: "hung read: destruction returns while a read past the budget is still running",
		},
		{
			name:   "a running report not waited out",
			mutate: watch("    channel_->wake.wait(lock, [this] { return !channel_->reporting; });\n", ""),
			want:   "report: destruction returns only after the report already running has returned",
		},
		{
			name: "a read after the cancel reported",
			mutate: watch(
				"        if (channel->cancelled) return;\n        channel->reporting = true;\n",
				"        channel->reporting = true;\n"),
			want: "cancel: a read that finishes after the cancel is not reported",
		},
		{
			name:   "the first report not waited for",
			mutate: watch("    return channel_->firstReported;\n", "    return true;\n"),
			want:   "blocked: a read that does not answer is not waited out",
		},
	})
}

// The app reads the country, applies it to its own sdk before the space
// manager is built, and hands it to the service: in start_tunnel and
// start_provider, and in set_network_country on every change and every hello.
func TestNetworkCountryAppWiring(t *testing.T) {
	host := sdkHostSource(t)
	initialize := definitionBody(t, "SdkHost.cpp", host, "bool SdkHost::Initialize()")
	provideRequireOrder(t, "SdkHost::Initialize", initialize,
		"StartNetworkCountryWatch();", "urnet::newNetworkSpaceManager(")

	start := definitionBody(t, "SdkHost.cpp", host, "void SdkHost::StartNetworkCountryWatch()")
	provideRequire(t, "StartNetworkCountryWatch", start,
		"std::make_unique<NetworkCountryWatch>(",
		// the read may be left running past destruction: it captures nothing
		"[] { return ReadNetworkCountry(); }",
		"ApplyNetworkCountry(reading);",
		"std::make_unique<DefaultRouteChanges>(networkCountryWatch_->NetworkEventSink())",
		"networkCountryWatch_->WaitFirstReport(kNetworkCountryFirstReadWait)")

	apply := definitionBody(t, "SdkHost.cpp", host,
		"void SdkHost::ApplyNetworkCountry(const netcountry::Reading& reading)")
	provideRequireOrder(t, "ApplyNetworkCountry", apply,
		"urnet::setNetworkCountryCode(reading.code);", "PushNetworkCountry(")
	// Initialize holds mutex_ while it waits for the first report
	requireNone(t, "ApplyNetworkCountry", apply, "lock(mutex_", "mutex_, std::defer_lock")

	push := definitionBody(t, "SdkHost.cpp", host, "void SdkHost::PushNetworkCountry(const char* why)")
	provideRequire(t, "PushNetworkCountry", push, "if (!service_.IsConnected()) return;")
	provideRequireOrder(t, "PushNetworkCountry", push,
		"std::scoped_lock pushLock(networkCountryPushMutex_);", "CurrentNetworkCountry()")
	provideRequireOrder(t, "PushNetworkCountry", push,
		"CurrentNetworkCountry()", "service_.SetNetworkCountry(country)")
	requireNone(t, "PushNetworkCountry", push, "lock(mutex_", "mutex_, std::defer_lock")

	bootstrap := definitionBody(t, "SdkHost.cpp", host, "bool SdkHost::BootstrapSession(")
	provideRequireOrder(t, "BootstrapSession", bootstrap, "AdoptServiceFacts(hello);", "PushNetworkCountry(")
	provideRequireOrder(t, "BootstrapSession", bootstrap,
		"cfg.network_country_code = networkCountry.code;", "service_.StartTunnel(cfg)")
	provideRequireOrder(t, "BootstrapSession", bootstrap,
		"cfg.network_country_source = networkCountry.source;", "service_.StartTunnel(cfg)")

	reconcile := definitionBody(t, "SdkHost.cpp", host, "void SdkHost::ReconcileProviderLocked(")
	provideRequireOrder(t, "ReconcileProviderLocked", reconcile,
		"request.network_country_code = networkCountry.code;", "service_.StartProvider(request")
	provideRequireOrder(t, "ReconcileProviderLocked", reconcile,
		"request.network_country_source = networkCountry.source;", "service_.StartProvider(request")

	destructor := definitionBody(t, "SdkHost.cpp", host, "SdkHost::~SdkHost()")
	provideRequireOrder(t, "~SdkHost", destructor,
		"networkCountryChanges_.reset();", "networkCountryWatch_.reset();")
	provideRequireOrder(t, "~SdkHost", destructor,
		"networkCountryWatch_.reset();", "std::scoped_lock lock(mutex_);")

	// the notifications die before the watch even without the explicit resets:
	// members are destroyed in reverse declaration order
	header := stripComments(readAppSource(t, "SdkHost.h"))
	provideRequireOrder(t, "SdkHost.h", header,
		"std::unique_ptr<NetworkCountryWatch> networkCountryWatch_;",
		"std::unique_ptr<DefaultRouteChanges> networkCountryChanges_;")

	client := stripComments(readAppSource(t, "ServiceClient.cpp"))
	set := definitionBody(t, "ServiceClient.cpp", client,
		"bool ServiceClient::SetNetworkCountry(const proto::SetNetworkCountry& country)")
	provideRequire(t, "ServiceClient::SetNetworkCountry", set, "proto::msg::kSetNetworkCountry")
}

// A start request carries the reading it was built with, and the watch's
// thread can push a newer one while it is built. The pipe serves one call at a
// time, so that push can reach the service first, and the start then puts the
// older country back for as long as the network stays put. So after
// start_tunnel and start_provider return, whether they succeeded or not (the
// service applies the country before it can refuse), the app pushes again when
// its reading is no longer the one the request carried.
func TestNetworkCountryStartRequestsConverge(t *testing.T) {
	host := sdkHostSource(t)
	moved := definitionBody(t, "SdkHost.cpp", host,
		"void SdkHost::PushNetworkCountryIfMoved(const netcountry::Reading& sent, const char* why)")
	provideRequireOrder(t, "PushNetworkCountryIfMoved", moved,
		"if (CurrentNetworkCountry() == sent) return;", "PushNetworkCountry(why);")
	// BootstrapSession and ReconcileProviderLocked hold mutex_
	requireNone(t, "PushNetworkCountryIfMoved", moved, "lock(mutex_", "mutex_, std::defer_lock")

	for _, site := range []struct {
		name      string
		signature string
		start     string
		outcome   string
	}{
		{
			name:      "BootstrapSession",
			signature: "bool SdkHost::BootstrapSession(",
			start:     "service_.StartTunnel(cfg)",
			outcome:   "if (!proto::IsSessionLive(st.state)) {",
		},
		{
			name:      "ReconcileProviderLocked",
			signature: "void SdkHost::ReconcileProviderLocked(",
			start:     "service_.StartProvider(request, &after, &error)",
			outcome:   "if (started) {",
		},
	} {
		body := definitionBody(t, "SdkHost.cpp", host, site.signature)
		at := strings.Index(body, site.start)
		if at < 0 {
			t.Errorf("%s no longer calls %s; update this contract", site.name, site.start)
			continue
		}
		// from the start request on: the push follows it, ahead of the branch
		// that reads its outcome
		provideRequireOrder(t, site.name, body[at:], "PushNetworkCountryIfMoved(networkCountry, ", site.outcome)
	}
}

// The service applies what the app sends to its own sdk, process-wide: before
// a tunnel session's space is imported, before the provider-only device is
// kept or built (after its refusals), and on set_network_country, which takes
// no session lock and pushes no state.
func TestNetworkCountryServiceWiring(t *testing.T) {
	source := tunnelControllerSource(t)
	set := definitionBody(t, "TunnelController.cpp", source,
		"void TunnelController::SetNetworkCountry(const std::string& code, const std::string& source)")
	provideRequire(t, "TunnelController::SetNetworkCountry", set,
		"netcountry::Normalized(code, source)",
		"std::scoped_lock lock(networkCountryMutex_);",
		"urnet::setNetworkCountryCode(reading.code);")
	requireNone(t, "TunnelController::SetNetworkCountry", set,
		"lock(mutex_", "mutex_, std::defer_lock", "StatusLocked(", "PublishStatusLocked(")

	startLocked := definitionBody(t, "TunnelController.cpp", source,
		"proto::TunnelStatus TunnelController::StartLocked(")
	provideRequireOrder(t, "StartLocked", startLocked,
		"SetNetworkCountry(config.network_country_code, config.network_country_source);",
		"ImportNetworkSpaceLocked(config.network_space_json)")

	startProvider := definitionBody(t, "TunnelController.cpp", source,
		"bool TunnelController::StartProvider(")
	const applied = "SetNetworkCountry(request.network_country_code, request.network_country_source);"
	provideRequireOrder(t, "StartProvider", startProvider, "provide::ProviderStartRefusal(", applied)
	provideRequireOrder(t, "StartProvider", startProvider, applied,
		"proto::SameProviderDevice(providerRequest_, request)")
	provideRequireOrder(t, "StartProvider", startProvider, applied,
		"ImportNetworkSpaceLocked(request.network_space_json)")

	server := stripComments(readServiceSource(t, "ControlServer.cpp"))
	handle := definitionBody(t, "ControlServer.cpp", server, "nlohmann::json ControlServer::Handle(")
	at := strings.Index(handle, "proto::msg::kSetNetworkCountry")
	if at < 0 {
		t.Fatal("ControlServer::Handle does not answer set_network_country")
	}
	// the branch, through the brace that opens the next one
	branch := handle[at:]
	if end := strings.Index(branch, "} else"); end >= 0 {
		branch = branch[:end]
	}
	provideRequire(t, "the set_network_country branch", branch,
		"tunnel_.SetNetworkCountry(s.network_country_code, s.network_country_source);",
		"reply.ok = true;")
	requireNone(t, "the set_network_country branch", branch, "PushState()")
}

// The honesty rule, on the source: the country comes from the registered
// mobile broadband network or not at all — never from the locale or the
// region setting — and the reader touches nothing that carries the device's or
// the SIM's identifiers. Only SdkHost's apply and the service's setter hand a
// country to the sdk, and the service never reads one itself.
func TestNetworkCountryReaderIsHonest(t *testing.T) {
	reader := stripComments(readAppSource(t, "MobileBroadband.cpp"))
	provideRequire(t, "MobileBroadband.cpp", reader,
		"IMbnRegistration",
		"GetRegisterState(&state)",
		"GetCurrentDataClass(&dataClass)",
		"GetProviderID(&providerId)",
		"netcountry::ElectDefaultRoute(routes)",
		"netcountry::IsMobileBroadbandInterface(",
		"netcountry::ReadingFor(facts)",
		"row->DestinationPrefix.PrefixLength != 0",
		"CancelMibChangeNotify2(")
	requireNone(t, "MobileBroadband.cpp", reader,
		"GeoID", "GeoName", "GetUserDefaultLocaleName", "GetSystemDefaultLocaleName",
		"GetUserDefaultLCID", "GetLocaleInfo", "GeographicRegion", "GlobalizationPreferences",
		"GetInterfaceCapability", "IMbnSubscriberInformation", "GetSubscriberInformation",
		"GetHomeProvider", "#include \"pch.h\"")

	setter := regexp.MustCompile(`setNetworkCountryCode\(`)
	for name, source := range map[string]string{
		"App/SdkHost.cpp":              sdkHostSource(t),
		"Service/TunnelController.cpp": tunnelControllerSource(t),
	} {
		if count := len(setter.FindAllStringIndex(source, -1)); count != 1 {
			t.Errorf("%s calls setNetworkCountryCode %d times; one place hands the country to the sdk", name, count)
		}
	}
	for _, dir := range []string{"App", "Service", "Common"} {
		entries, err := os.ReadDir(filepath.Join(repositoryRoot(t), "app", "src", dir))
		if err != nil {
			t.Fatal(err)
		}
		for _, entry := range entries {
			name := entry.Name()
			if entry.IsDir() || !(strings.HasSuffix(name, ".cpp") || strings.HasSuffix(name, ".h")) {
				continue
			}
			if (dir == "App" && name == "SdkHost.cpp") || (dir == "Service" && name == "TunnelController.cpp") {
				continue
			}
			data, err := os.ReadFile(filepath.Join(repositoryRoot(t), "app", "src", dir, name))
			if err != nil {
				t.Fatal(err)
			}
			text := stripComments(string(data))
			if setter.MatchString(text) {
				t.Errorf("%s/%s hands a country to the sdk; only SdkHost::ApplyNetworkCountry and "+
					"TunnelController::SetNetworkCountry may", dir, name)
			}
			if dir == "Service" && (strings.Contains(text, "ReadNetworkCountry(") ||
				strings.Contains(text, "MobileBroadband.h")) {
				t.Errorf("Service/%s reads the network country; the app reads it and the service applies what it sends", name)
			}
		}
	}
}

// The new files are in their projects, the reader builds without the WinRT
// precompiled header, and the pure headers stay portable.
func TestNetworkCountryProjectEntries(t *testing.T) {
	common := readCommonSource(t, "Common.vcxproj")
	provideRequire(t, "Common.vcxproj", common, `<ClInclude Include="NetworkCountry.h" />`)
	app := readAppSource(t, "App.vcxproj")
	provideRequire(t, "App.vcxproj", app,
		`<ClCompile Include="MobileBroadband.cpp"><PrecompiledHeader>NotUsing</PrecompiledHeader></ClCompile>`,
		`<ClInclude Include="MobileBroadband.h" />`,
		`<ClInclude Include="NetworkCountryWatch.h" />`)
	portable := regexp.MustCompile(`#include\s*<(windows|winsock2|mbnapi|objbase)\.h>|#include\s*"(Sdk|Log|pch)\.h"`)
	for _, header := range []struct{ dir, name string }{
		{dir: "Common", name: "NetworkCountry.h"},
		{dir: "App", name: "NetworkCountryWatch.h"},
		{dir: "App", name: "MobileBroadband.h"},
	} {
		data, err := os.ReadFile(filepath.Join(repositoryRoot(t), "app", "src", header.dir, header.name))
		if err != nil {
			t.Fatal(err)
		}
		if portable.MatchString(string(data)) {
			t.Errorf("%s/%s must stay portable: its harness runs off Windows", header.dir, header.name)
		}
	}
}
