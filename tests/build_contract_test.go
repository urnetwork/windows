// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"encoding/xml"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"runtime"
	"sort"
	"strings"
	"testing"
)

const (
	wixNamespace     = "http://wixtoolset.org/schemas/v4/wxs"
	utilNamespace    = "http://wixtoolset.org/schemas/v4/wxs/util"
	msbuildNamespace = "http://schemas.microsoft.com/developer/msbuild/2003"
)

var fontNames = map[string]bool{
	"abcgravity_extended.otf":        true,
	"abcgravity_extra_condensed.otf": true,
	"pp_neue_bit_bold.ttf":           true,
	"pp_neue_montreal_regular.ttf":   true,
}

type xmlNode struct {
	XMLName  xml.Name
	Attrs    []xml.Attr `xml:",any,attr"`
	Text     string     `xml:",chardata"`
	Children []xmlNode  `xml:",any"`
}

func repositoryRoot(t *testing.T) string {
	t.Helper()
	_, filename, _, ok := runtime.Caller(0)
	if !ok {
		t.Fatal("cannot locate build contract test source")
	}
	return filepath.Dir(filepath.Dir(filename))
}

func parseXML(t *testing.T, filename string) xmlNode {
	t.Helper()
	data, err := os.ReadFile(filename)
	if err != nil {
		t.Fatalf("read %s: %v", filename, err)
	}
	var root xmlNode
	if err := xml.Unmarshal(data, &root); err != nil {
		t.Fatalf("parse %s: %v", filename, err)
	}
	return root
}

func (node *xmlNode) attribute(name string) (string, bool) {
	for _, attribute := range node.Attrs {
		if attribute.Name.Local == name {
			return attribute.Value, true
		}
	}
	return "", false
}

func (node *xmlNode) child(namespace, local string) *xmlNode {
	for index := range node.Children {
		child := &node.Children[index]
		if child.XMLName.Space == namespace && child.XMLName.Local == local {
			return child
		}
	}
	return nil
}

func (node *xmlNode) children(namespace, local string) []*xmlNode {
	var matches []*xmlNode
	for index := range node.Children {
		child := &node.Children[index]
		if child.XMLName.Space == namespace && child.XMLName.Local == local {
			matches = append(matches, child)
		}
	}
	return matches
}

func (node *xmlNode) descendants(namespace, local string) []*xmlNode {
	var matches []*xmlNode
	var visit func(*xmlNode)
	visit = func(current *xmlNode) {
		for index := range current.Children {
			child := &current.Children[index]
			if child.XMLName.Space == namespace && child.XMLName.Local == local {
				matches = append(matches, child)
			}
			visit(child)
		}
	}
	visit(node)
	return matches
}

func findByID(nodes []*xmlNode, id string) *xmlNode {
	for _, node := range nodes {
		if value, ok := node.attribute("Id"); ok && value == id {
			return node
		}
	}
	return nil
}

func windowsBase(filename string) string {
	return filepath.Base(strings.ReplaceAll(filename, `\`, `/`))
}

func missingNames(found map[string]*xmlNode) []string {
	var missing []string
	for name := range fontNames {
		if found[name] == nil {
			missing = append(missing, name)
		}
	}
	sort.Strings(missing)
	return missing
}

func TestWin32MacroGuards(t *testing.T) {
	root := repositoryRoot(t)
	sourceRoot := filepath.Join(root, "app", "src")
	err := filepath.WalkDir(sourceRoot, func(path string, entry os.DirEntry, walkErr error) error {
		if walkErr != nil {
			return walkErr
		}
		if entry.IsDir() {
			return nil
		}
		switch strings.ToLower(filepath.Ext(path)) {
		case ".cpp", ".h", ".hpp":
		default:
			return nil
		}
		data, err := os.ReadFile(path)
		if err != nil {
			return err
		}
		lines := strings.Split(strings.TrimPrefix(string(data), "\ufeff"), "\n")
		for index, line := range lines {
			if strings.TrimSpace(strings.TrimSuffix(line, "\r")) != "#define WIN32_LEAN_AND_MEAN" {
				continue
			}
			previous := index - 1
			for previous >= 0 && strings.TrimSpace(lines[previous]) == "" {
				previous--
			}
			if previous < 0 || strings.TrimSpace(lines[previous]) != "#ifndef WIN32_LEAN_AND_MEAN" {
				relative, _ := filepath.Rel(root, path)
				return fmt.Errorf("%s:%d: unguarded WIN32_LEAN_AND_MEAN", relative, index+1)
			}
		}
		return nil
	})
	if err != nil {
		t.Fatal(err)
	}

	props := parseXML(t, filepath.Join(root, "app", "Directory.Build.props"))
	var options strings.Builder
	for _, node := range props.descendants(msbuildNamespace, "AdditionalOptions") {
		options.WriteString(node.Text)
		options.WriteByte(' ')
	}
	for _, warning := range []string{"/we4005", "/we4651"} {
		if !strings.Contains(options.String(), warning) {
			t.Fatalf("Directory.Build.props does not keep %s fatal", warning)
		}
	}
}

func TestGeneratedXamlPCHContract(t *testing.T) {
	root := repositoryRoot(t)
	project := parseXML(t, filepath.Join(root, "app", "src", "App", "App.vcxproj"))
	foundDefinition := false
	for _, group := range project.children(msbuildNamespace, "ItemDefinitionGroup") {
		compile := group.child(msbuildNamespace, "ClCompile")
		if compile == nil {
			continue
		}
		definitions := compile.child(msbuildNamespace, "PreprocessorDefinitions")
		if definitions != nil && strings.Contains(definitions.Text, "MICROSOFT_WINDOWSAPPSDK_SELFCONTAINED=1") {
			foundDefinition = true
		}
	}
	if !foundDefinition {
		t.Fatal("generated XAML units cannot inherit the self-contained definition from the project-wide ClCompile defaults")
	}

	var target *xmlNode
	for _, candidate := range project.descendants(msbuildNamespace, "Target") {
		if name, _ := candidate.attribute("Name"); name == "UrnCompileGeneratedXamlImpl" {
			target = candidate
			break
		}
	}
	if target == nil {
		t.Fatal("UrnCompileGeneratedXamlImpl target is missing")
	}
	var dynamic *xmlNode
	for _, candidate := range target.descendants(msbuildNamespace, "ClCompile") {
		if include, _ := candidate.attribute("Include"); include == "@(_UrnGenXamlCpp)" {
			dynamic = candidate
			break
		}
	}
	if dynamic == nil {
		t.Fatal("generated XAML compile item is missing")
	}
	if dynamic.child(msbuildNamespace, "PreprocessorDefinitions") != nil {
		t.Fatal("generated XAML items override inherited ClCompile definitions; an unqualified metadata expansion fails with MSB4096")
	}
}

func readAppSource(t *testing.T, name string) string {
	t.Helper()
	filename := filepath.Join(repositoryRoot(t), "app", "src", "App", name)
	data, err := os.ReadFile(filename)
	if err != nil {
		t.Fatalf("read %s: %v", filename, err)
	}
	return string(data)
}

func readServiceSource(t *testing.T, name string) string {
	t.Helper()
	filename := filepath.Join(repositoryRoot(t), "app", "src", "Service", name)
	data, err := os.ReadFile(filename)
	if err != nil {
		t.Fatalf("read %s: %v", filename, err)
	}
	return string(data)
}

func TestReferralSheetIncludesCompleteBalanceStoreType(t *testing.T) {
	source := readAppSource(t, "SettingsSheets.cpp")
	if !strings.Contains(source, `#include "SubscriptionBalance.h"`) {
		t.Fatal("SettingsSheets.cpp calls SubscriptionBalanceStore methods through PageContext but includes only its forward declaration")
	}
}

func TestReferralCardIncludesCompleteBalanceStoreType(t *testing.T) {
	source := readAppSource(t, "ReferralCard.cpp")
	if !strings.Contains(source, `#include "SubscriptionBalance.h"`) {
		t.Fatal("ReferralCard.cpp calls SubscriptionBalanceStore methods through PageContext but includes only its forward declaration")
	}
}

func TestReferralsPageIncludesCompleteAutomationType(t *testing.T) {
	source := readAppSource(t, "ReferralsPage.cpp")
	if !strings.Contains(source, `#include <winrt/Microsoft.UI.Xaml.Automation.h>`) {
		t.Fatal("ReferralsPage.cpp calls AutomationProperties methods without including their complete C++/WinRT type")
	}
}

func TestWalletPositionIndicatorIncludesCompletePointerPointType(t *testing.T) {
	source := readAppSource(t, "WalletPage.cpp")
	projection := strings.Index(source, `#include <winrt/Microsoft.UI.Input.h>`)
	position := strings.Index(source, ".GetCurrentPoint(")
	if projection < 0 || position < 0 || projection > position {
		t.Fatal("WalletPage.cpp calls PointerPoint::Position without first including the complete Microsoft.UI.Input projection")
	}
}

func TestOnboardingLinkHandlerIsPublicForAppController(t *testing.T) {
	header := readAppSource(t, "MainWindow.xaml.h")
	classStart := strings.Index(header, "struct MainWindow :")
	if classStart < 0 {
		t.Fatal("MainWindow declaration is missing")
	}
	classBody := header[classStart:]
	privateStart := strings.Index(classBody, "private:")
	handler := strings.Index(classBody, "void HandleOnboardingLink(std::string const& url);")
	if privateStart < 0 || handler < 0 || handler >= privateStart {
		t.Fatal("MainWindow::HandleOnboardingLink must be public because AppController invokes it")
	}

	controller := readAppSource(t, "AppController.cpp")
	if !strings.Contains(controller, "self->HandleOnboardingLink(url);") {
		t.Fatal("AppController no longer routes onboarding deep links to MainWindow")
	}
}

func TestClientEventLocaleUsesTheExportedResourceLanguage(t *testing.T) {
	header := readAppSource(t, "Localization.h")
	if !strings.Contains(header, "std::string PrimaryLanguage();") {
		t.Fatal("Localization.h does not export the resource language needed by client registration")
	}

	localization := readAppSource(t, "Localization.cpp")
	internalEnd := strings.Index(localization, "}  // namespace")
	definition := strings.Index(localization, "std::string PrimaryLanguage()")
	if internalEnd < 0 || definition < 0 || definition < internalEnd {
		t.Fatal("PrimaryLanguage has internal linkage and cannot be used by ClientEvents.cpp")
	}

	events := readAppSource(t, "ClientEvents.cpp")
	if !strings.Contains(events, "const std::string tag = PrimaryLanguage();") {
		t.Fatal("client events do not use the app's exported BCP 47 resource language")
	}
	if strings.Contains(events, "Narrow(PrimaryLanguage())") {
		t.Fatal("ClientEventLocale narrows PrimaryLanguage even though it already returns UTF-8")
	}
}

func TestReferralReloadIsPublicForMainWindowNavigation(t *testing.T) {
	header := readAppSource(t, "SettingsPage.h")
	classStart := strings.Index(header, "class SettingsPage {")
	if classStart < 0 {
		t.Fatal("SettingsPage declaration is missing")
	}
	classBody := header[classStart:]
	publicStart := strings.Index(classBody, "public:")
	privateStart := strings.Index(classBody, "private:")
	loadReferral := strings.Index(classBody, "void LoadReferral();")
	if publicStart < 0 || privateStart < 0 || loadReferral < publicStart || loadReferral >= privateStart {
		t.Fatal("SettingsPage::LoadReferral must be public because MainWindow invokes it when opening the referrals destination")
	}

	window := readAppSource(t, "MainWindow.xaml.cpp")
	if !strings.Contains(window, "settings_->LoadReferral();") {
		t.Fatal("MainWindow no longer reloads shared referral state when opening the referrals destination")
	}
}

func TestOnboardingUsesUnambiguousWinRTNumericAndInspectableTypes(t *testing.T) {
	source := readAppSource(t, "Onboarding.cpp")
	for _, required := range []string{
		"using winrt::Windows::Foundation::IInspectable;",
		"fade.From(0.0);",
		"fade.To(1.0);",
		"column == 0 ? 0.0 : 8.0",
		"column == 4 ? 0.0 : 8.0",
	} {
		if !strings.Contains(source, required) {
			t.Fatalf("Onboarding.cpp is missing the C++/WinRT compile contract %q", required)
		}
	}
	for _, ambiguous := range []string{"fade.From(0);", "fade.To(1);"} {
		if strings.Contains(source, ambiguous) {
			t.Fatalf("Onboarding.cpp passes ambiguous integral literal in %q", ambiguous)
		}
	}
}

func TestWalletSetErrorIsAdaptedToCommonSnError(t *testing.T) {
	source := readAppSource(t, "WalletPage.cpp")
	start := strings.Index(source, "Sdk().api().snSetWallet(")
	if start < 0 {
		t.Fatal("SnSetWallet call is missing")
	}
	setWalletCall := source[start:]
	if !strings.Contains(setWalletCall, "error = SetWalletError(*result->error);") {
		t.Fatal("SnSetWalletError is not explicitly adapted to the common SnError result channel")
	}
	rawAssignment := strings.Index(setWalletCall, "error = result->error;")
	deliver := strings.Index(setWalletCall, "deliver(error")
	if 0 <= rawAssignment && (deliver < 0 || rawAssignment < deliver) {
		t.Fatal("unrelated optional<SnSetWalletError> is assigned to optional<SnError>")
	}
}

func TestWalletProvideStateCompileContracts(t *testing.T) {
	for _, name := range []string{"ConnectPage.cpp", "WalletPage.cpp"} {
		source := readAppSource(t, name)
		pch := strings.Index(source, `#include "pch.h"`)
		visual := strings.Index(source, `#include "ProvideModeVisual.h"`)
		if pch < 0 || visual < 0 || pch > visual {
			t.Fatalf("%s must include pch.h before ProvideModeVisual.h because the app compiles with /Yu", name)
		}
	}

	header := readAppSource(t, "WalletPage.h")
	classStart := strings.Index(header, "class WalletPage {")
	if classStart < 0 {
		t.Fatal("WalletPage declaration is missing")
	}
	classBody := header[classStart:]
	publicStart := strings.Index(classBody, "public:")
	privateStart := strings.Index(classBody, "private:")
	applyProvideState := strings.Index(classBody, "void ApplyProvideState(urnw::LiveStats const& stats);")
	if publicStart < 0 || privateStart < 0 || applyProvideState < publicStart || applyProvideState >= privateStart {
		t.Fatal("WalletPage::ApplyProvideState must be public because MainWindow relays live stats to it")
	}

	source := readAppSource(t, "WalletPage.cpp")
	definition := strings.Index(source, "void WalletPage::ApplyProvideState(urnw::LiveStats const& stats)")
	namespaceEnd := strings.LastIndex(source, "}  // namespace urnw")
	if definition < 0 || namespaceEnd < definition {
		t.Fatal("WalletPage::ApplyProvideState must be defined inside namespace urnw")
	}

	window := readAppSource(t, "MainWindow.xaml.cpp")
	if !strings.Contains(window, "wallet_->ApplyProvideState(stats);") {
		t.Fatal("MainWindow no longer relays live stats to the Earnings provide-state renderer")
	}
}

func TestTunnelWatchdogObservesDestinationGenerationAndReadiness(t *testing.T) {
	header := readServiceSource(t, "DeadTunnelVerdict.h")
	source := readServiceSource(t, "TunnelWatchdog.cpp")
	for _, required := range []string{
		"ConnectionEpochTracker",
		"providerWindowReady",
		"trafficStartMillis",
	} {
		if !strings.Contains(header, required) {
			t.Fatalf("DeadTunnelVerdict.h is missing the connection-epoch contract %q", required)
		}
	}
	for _, required := range []string{
		"addWindowStatusChangeListener",
		"status->ConnectionGeneration",
		"status->MinSatisfied",
		"connectionEpoch.FastVerdictEligible()",
	} {
		if !strings.Contains(source, required) {
			t.Fatalf("TunnelWatchdog.cpp is missing the live connection-epoch wiring %q", required)
		}
	}
}

func readCommonSource(t *testing.T, name string) string {
	t.Helper()
	filename := filepath.Join(repositoryRoot(t), "app", "src", "Common", name)
	data, err := os.ReadFile(filename)
	if err != nil {
		t.Fatalf("read %s: %v", filename, err)
	}
	return string(data)
}

// The service's process budget and its per-device memory target are two
// surfaces, not one number: SdkInit sizes the pools and the go soft limit,
// while the device target is passed at device creation. The constructors that
// take no target fall back to the process budget, which is what the service
// used to do. Both come from one measured host tier.
//
// The tier logic itself is executed, not read, by tools/memory-tier-tests.cpp,
// which builds on any host; this pins the wiring and the numbers.
func TestServiceMemoryBudgetAndDeviceTargetPair(t *testing.T) {
	tiers := readCommonSource(t, "MemoryTiers.h")
	for _, required := range []string{
		"inline constexpr int64_t kProcessMemoryBudgetByteCount = 384ll * 1024 * 1024;",
		"inline constexpr int64_t kDeviceMemoryTargetByteCount = 128ll * 1024 * 1024;",
		"inline constexpr int64_t kLargeHostProcessMemoryBudgetByteCount = 768ll * 1024 * 1024;",
		"inline constexpr int64_t kLargeHostDeviceMemoryTargetByteCount = 256ll * 1024 * 1024;",
		"inline constexpr int64_t kLargeHostMemoryByteCount = 7ll * 1024 * 1024 * 1024;",
	} {
		if !strings.Contains(tiers, required) {
			t.Fatalf("Common/MemoryTiers.h does not declare %q", required)
		}
	}
	// Both constraints on both tiers, computed here rather than restated: the
	// pools take 14 of 34 parts so a target is at most 20/34 of its budget, and
	// the go collector wants a budget at three times its target.
	for _, tier := range []struct {
		name   string
		target int64
		budget int64
	}{
		{"base", 128 * 1024 * 1024, 384 * 1024 * 1024},
		{"large-host", 256 * 1024 * 1024, 768 * 1024 * 1024},
	} {
		if tier.target*34 > tier.budget*(34-14) {
			t.Errorf("the %s device target %d is not backed by its process budget %d",
				tier.name, tier.target, tier.budget)
		}
		if 3*tier.target > tier.budget {
			t.Errorf("the %s process budget %d is too close to its device target %d",
				tier.name, tier.budget, tier.target)
		}
	}
	for _, assertion := range []string{
		"the base device memory target is not backed by its process budget",
		"the base process budget is too close to its device memory target",
		"the large-host device memory target is not backed by its process budget",
		"the large-host process budget is too close to its device memory target",
		"an unknown host must take the base device memory target",
		"an unknown host must take the base process budget",
		"a host one byte under the bar must take the base memory tier",
		"a host exactly at the bar must take the base memory tier",
		"a host one byte over the bar must take the large memory tier",
		"a machine reporting exactly 8 GiB must take the large memory tier",
		"a machine sold as 8 GiB, reporting its usable 7.68 GiB, must take the large tier",
	} {
		if !strings.Contains(tiers, assertion) {
			t.Errorf("Common/MemoryTiers.h no longer static_asserts %q", assertion)
		}
	}

	// The tier is chosen from a MEASURED host, once, and both numbers come from
	// that one measurement.
	sdk := readCommonSource(t, "Sdk.cpp")
	if !strings.Contains(sdk, "::GlobalMemoryStatusEx(&status)") {
		t.Error("Common/Sdk.cpp no longer measures host memory with GlobalMemoryStatusEx")
	}
	if !strings.Contains(sdk, "static const int64_t byteCount = MeasureHostMemoryByteCount();") {
		t.Error("Common/Sdk.cpp no longer caches the host memory measurement")
	}
	for _, required := range []string{
		"return MemoryTierForHost(HostMemoryByteCount()).process_budget_byte_count;",
		"return MemoryTierForHost(HostMemoryByteCount()).device_target_byte_count;",
	} {
		if !strings.Contains(sdk, required) {
			t.Errorf("Common/Sdk.cpp does not derive both numbers from the tier: %q", required)
		}
	}

	main := readServiceSource(t, "main.cpp")
	if strings.Count(main, "SdkInit(/*isService=*/true, ProcessMemoryBudgetByteCount());") != 2 {
		t.Error("Service/main.cpp does not initialize the SDK with the tier's process budget")
	}
}

func TestTunnelControllerConstructsEveryDeviceAtTheMemoryTarget(t *testing.T) {
	source := readServiceSource(t, "TunnelController.cpp")
	for _, forbidden := range []string{
		"urnet::newDeviceLocalWithDefaults(",
		"urnet::newDeviceLocalWithKeyMaterial(",
	} {
		if strings.Contains(source, forbidden) {
			t.Errorf("TunnelController.cpp still constructs a device with %q", forbidden)
		}
	}
	const call = "urnet::newDeviceLocalWithMemoryTarget("
	calls := strings.Split(source, call)[1:]
	if len(calls) != 2 {
		t.Fatalf("TunnelController.cpp has %d newDeviceLocalWithMemoryTarget calls, want 2", len(calls))
	}
	for index, rest := range calls {
		end := strings.Index(rest, ");")
		if end < 0 || !strings.Contains(rest[:end], "memoryTargetByteCount") {
			t.Errorf("newDeviceLocalWithMemoryTarget call %d does not pass the tier's device target", index+1)
		}
	}
	// ...and that target comes from the same cached measurement the process
	// budget used, so a large target can never be paired with a small budget.
	if !strings.Contains(source, "const int64_t memoryTargetByteCount = DeviceMemoryTargetByteCount();") {
		t.Error("TunnelController.cpp does not take its device target from the measured memory tier")
	}
}

func TestTunnelCleanupDiagnosticsDoNotCertifyKernelState(t *testing.T) {
	source := readServiceSource(t, "TunnelController.cpp")
	// Check the actual log call, joining adjacent C++ string literals. These
	// diagnostics describe controller ownership, not an independent OS readback.
	joinLiterals := regexp.MustCompile(`"\s*"`)
	for _, c := range []struct {
		prefix   string
		required []string
	}{
		{
			prefix: `LogInfo("tunnel: stage=capture outcome=rolled-back`,
			required: []string{
				"cleanup=attempted", "routes=false dns=false", "facts=ownership",
				"os_state=unverified", "firewall={}",
			},
		},
		{
			prefix: `LogInfo("tunnel: stage=capture outcome=superseded`,
			required: []string{
				"routes=false dns=false", "facts=ownership", "os_state=unverified",
			},
		},
		{
			prefix: `LogInfo("tunnel: stopped,`,
			required: []string{
				"route_dns_cleanup={}", `hadRoutes ? "attempted" : "not-owned"`,
				"routes=false dns=false", "facts=ownership", "os_state=unverified",
				"SDK TEARDOWN ABANDONED",
			},
		},
		{
			prefix: `LogInfo("tunnel: machine cleanup requested`,
			required: []string{
				"routes=false dns=false", "facts=ownership", "os_state=unverified",
				"resolver_cache_flush=attempted", "firewall={}",
			},
		},
		{
			prefix: `LogWarn("tunnel: the failsafe teardown`,
			required: []string{
				"cleanup=attempted", "facts=ownership", "os_state=unverified",
				"Armed was requested", "firewall removal was requested",
			},
		},
		{
			prefix: `LogWarn("tunnel: ======== STOPPED AFTER STEP`,
			required: []string{
				"CONTROLLER OWNERS EMPTY", "facts=ownership", "os_state=unverified",
				"Route/DNS cleanup was attempted", "No route/DNS configuration owner",
			},
		},
		{
			prefix: `LogError("tunnel: ======== STOPPED AFTER STEP`,
			required: []string{
				"CONTROLLER CLEANUP IS INCOMPLETE", "facts=ownership", "os_state=unverified",
			},
		},
		{
			prefix: `LogError("tunnel: REFUSING to start`,
			required: []string{
				"cleanup=attempted", "os_state=unverified",
			},
		},
		{
			prefix: `LogWarn("tunnel: [{}] dropping the leak-prevention firewall`,
			required: []string{
				"route_cleanup=requested", "os_state=unverified",
			},
		},
	} {
		start := strings.Index(source, c.prefix)
		if start < 0 {
			t.Errorf("missing cleanup diagnostic %q", c.prefix)
			continue
		}
		rest := source[start:]
		end := strings.Index(rest, ");")
		if end < 0 {
			t.Fatalf("unterminated cleanup diagnostic %q", c.prefix)
		}
		call := joinLiterals.ReplaceAllString(rest[:end], "")
		for _, required := range c.required {
			if !strings.Contains(call, required) {
				t.Errorf("cleanup diagnostic %q is missing %q", c.prefix, required)
			}
		}
		for _, forbidden := range []string{
			"network restored", "network is BACK", "routes reverted",
			"dns cleared", "nothing was applied", "network is already back",
			"internet is back", "NOTHING IS LEFT APPLIED", "routes/dns reverted",
		} {
			if strings.Contains(call, forbidden) {
				t.Errorf("cleanup diagnostic %q certifies unread OS state with %q", c.prefix, forbidden)
			}
		}
	}
}

func TestAcceptanceHarnessImmutabilityContract(t *testing.T) {
	root := repositoryRoot(t)
	filename := filepath.Join(root, "test-main.sh")
	data, err := os.ReadFile(filename)
	if err != nil {
		t.Fatal(err)
	}
	source := string(data)
	mainStart := strings.Index(source, "\nmain() {\n")
	workStart := strings.Index(source, "\nset -euo pipefail\n")
	if mainStart < 0 || workStart < 0 || mainStart > workStart {
		t.Fatal("test-main.sh must parse its complete long-running body as main before executing work")
	}

	trimmed := strings.TrimSpace(source)
	if !strings.HasSuffix(trimmed, "}\n\nmain \"$@\"") {
		t.Fatal("test-main.sh must invoke its already-parsed main function as the final command")
	}
	initialized := strings.Index(source, "acceptance_finished=0")
	guarded := strings.Index(source, "${acceptance_finished:-0}")
	finished := strings.LastIndex(source, "acceptance_finished=1")
	finalExit := strings.LastIndex(source, "exit \"$acceptance_status\"")
	if initialized < 0 || guarded < 0 || finished < 0 || finalExit < 0 ||
		!(initialized < guarded && guarded < finished && finished < finalExit) {
		t.Fatal("test-main.sh completion sentinel cannot distinguish an early exit from a completed acceptance run")
	}
}

func TestAcceptanceGuestIsHardenedBeforeInstall(t *testing.T) {
	root := repositoryRoot(t)
	filename := filepath.Join(root, "test-main.sh")
	data, err := os.ReadFile(filename)
	if err != nil {
		t.Fatal(err)
	}
	source := string(data)
	prepare := strings.Index(source, "win_prepare_hermetic_guest")
	remoteDir := strings.Index(source, "remote=C:/acceptance")
	install := strings.Index(source, `-File $remote/run.ps1 -Msi $remote/urnetwork.msi`)
	if prepare < 0 || remoteDir < 0 || install < 0 {
		t.Fatalf("acceptance boundary missing: prepare=%d remote=%d install=%d", prepare, remoteDir, install)
	}
	if !(prepare < remoteDir && prepare < install) {
		t.Fatalf("acceptance work starts before guest policy verification: prepare=%d remote=%d install=%d", prepare, remoteDir, install)
	}
}

func TestAcceptanceBuildSelectsARM64WithoutOwningUnitSuites(t *testing.T) {
	root := repositoryRoot(t)
	data, err := os.ReadFile(filepath.Join(root, "test-main.sh"))
	if err != nil {
		t.Fatal(err)
	}
	source := string(data)
	for _, forbidden := range []string{
		"go test ./tests",
		"run-windows-lib.test.ps1",
	} {
		if strings.Contains(source, forbidden) {
			t.Fatalf("Windows acceptance still owns unit-test invocation %q", forbidden)
		}
	}
	architecture := strings.Index(source, "WINDOWS_BUILD_ARCHITECTURES=arm64")
	skipContracts := strings.Index(source, "WINDOWS_BUILD_SKIP_CONTRACT_TESTS=1")
	build := strings.Index(source, `"$root/build/all/build-windows.sh"`)
	if architecture < 0 || skipContracts < 0 || build < 0 ||
		!(architecture < build && skipContracts < build) {
		t.Fatalf("acceptance ARM64 build selection is not attached to the build invocation: architecture=%d skip=%d build=%d", architecture, skipContracts, build)
	}
}

func TestHostUnitRunnerOwnsPortableWindowsContracts(t *testing.T) {
	root := repositoryRoot(t)
	data, err := os.ReadFile(filepath.Join(root, "test.sh"))
	if err != nil {
		t.Fatal(err)
	}
	source := string(data)
	for _, required := range []string{
		`run-all run-all-windows-host`,
		`--verify-held run-all`,
		`(cd "$here" && go test -timeout 30m -parallel 2 "$@" ./tests)`,
		`(cd "$here" && go test "$@" ./tests/insufficientbalance)`,
		`(cd "$root/build/all/windows" && go test "$@" ./...)`,
	} {
		if !strings.Contains(source, required) {
			t.Errorf("windows/test.sh is missing host-unit ownership contract %q", required)
		}
	}
}

// The license drift check (sdk/licenses -check windows) keeps sdk/license.yml,
// which the Licenses page shows, in step with what the app ships. It belongs to
// the macOS host runner, where the sdk sibling and its Go module cache are, and
// never to the Windows app build, which has no Go.
func TestLicenseDriftCheckRunsOnTheHost(t *testing.T) {
	root := repositoryRoot(t)
	const check = `(cd "$here" && go -C "$root/sdk" run ./licenses -check windows)`
	for _, script := range []string{"test.sh", "build.sh"} {
		data, err := os.ReadFile(filepath.Join(root, script))
		if err != nil {
			t.Fatal(err)
		}
		if !strings.Contains(string(data), check) {
			t.Errorf("windows/%s is missing the license drift check %q", script, check)
		}
	}
	data, err := os.ReadFile(filepath.Join(root, "app", "build.ps1"))
	if err != nil {
		t.Fatal(err)
	}
	if strings.Contains(string(data), "./licenses") {
		t.Error("app/build.ps1 must not run the Go license check; the Windows app build has no Go")
	}
}

// Every store key the Licenses surfaces look up must exist in the neutral
// resources, or Loc() renders the key id itself on screen.
func TestLicensesStringsExist(t *testing.T) {
	root := repositoryRoot(t)
	document := parseXML(t, filepath.Join(root, "app", "src", "App", "Strings", "en", "Resources.resw"))
	names := map[string]bool{}
	for _, node := range document.descendants("", "data") {
		if name, ok := node.attribute("name"); ok {
			names[name] = true
		}
	}
	keyPattern := regexp.MustCompile(`\bLoc\("([a-z0-9_]+)"\)|section\("([a-z0-9_]+)"`)
	for _, file := range []string{"LicensesPage.cpp", "SettingsPage.cpp"} {
		source := readAppSource(t, file)
		for _, match := range keyPattern.FindAllStringSubmatch(source, -1) {
			key := match[1] + match[2]
			if file == "SettingsPage.cpp" && !strings.HasPrefix(key, "licenses") {
				continue
			}
			if !names[key] {
				t.Errorf("%s looks up %q, which en/Resources.resw does not define", file, key)
			}
		}
	}
	if !strings.Contains(readAppSource(t, "SettingsPage.cpp"), `Loc("licenses")`) {
		t.Error("Settings has no Licenses row")
	}
}

func TestLicensesPageIncludesCompleteAutomationType(t *testing.T) {
	source := readAppSource(t, "LicensesPage.cpp")
	for _, include := range []string{
		`#include <winrt/Microsoft.UI.Xaml.Automation.h>`,
		`#include <winrt/Microsoft.UI.Xaml.Automation.Peers.h>`,
	} {
		if !strings.Contains(source, include) {
			t.Errorf("LicensesPage.cpp calls AutomationProperties without %s", include)
		}
	}
}

func TestWindowsScriptsApplyTheSelectedArchitectureEndToEnd(t *testing.T) {
	root := repositoryRoot(t)
	read := func(relative string) string {
		filename := filepath.Join(root, filepath.FromSlash(relative))
		data, err := os.ReadFile(filename)
		if err != nil {
			t.Fatalf("read %s: %v", filename, err)
		}
		return string(data)
	}

	sdkBuild := read("build-sdk.ps1")
	for _, required := range []string{
		`[ValidateSet("amd64", "arm64")][string[]]$Architectures = @("amd64", "arm64")`,
		"foreach ($architecture in $Architectures)",
		"foreach ($arch in $Architectures)",
	} {
		if !strings.Contains(sdkBuild, required) {
			t.Errorf("build-sdk.ps1 is missing selected-architecture contract %q", required)
		}
	}
	if strings.Contains(sdkBuild, `foreach ($arch in @("amd64", "arm64"))`) {
		t.Fatal("build-sdk.ps1 still compiles both SDK architectures unconditionally")
	}

	appBuild := read("app/build.ps1")
	for _, required := range []string{
		`[ValidateSet("x64", "ARM64")][string[]]$Platforms = @("x64", "ARM64")`,
		`fetch-deps.ps1" -SdkZip $SdkZip -Platforms $Platforms`,
		"foreach ($platform in $Platforms)",
		`/t:restore `,
		`/p:Configuration=$Configuration /p:Platform=$platform /nologo /v:minimal`,
		`Remove-Item (Join-Path $OutDir "*.msi")`,
	} {
		if !strings.Contains(appBuild, required) {
			t.Errorf("app/build.ps1 is missing selected-platform contract %q", required)
		}
	}
	restore := strings.Index(appBuild, "& $msbuild URnetwork.sln /t:restore")
	platformLoop := strings.Index(appBuild, "foreach ($platform in $Platforms)")
	// The whole solution-compile command, not one substring of it: it must
	// carry the version UrVersion.ps1 derived (TestReleaseBuildStampsTheVersion),
	// where it used to pass only /p:Version, which no project reads.
	solutionBuild := consecutiveLines(appBuild,
		"& $msbuild URnetwork.sln `",
		"/p:Configuration=$Configuration /p:Platform=$platform `",
		"@urMsbuildArgs `",
		"/p:Version=$Version /m /nologo /v:minimal")
	if restore < 0 || platformLoop < 0 || solutionBuild < 0 || !(platformLoop < restore && restore < solutionBuild) {
		t.Fatalf("solution compile is not scoped to selected platforms: restore=%d loop=%d build=%d", restore, platformLoop, solutionBuild)
	}

	fetchDependencies := read("app/tools/fetch-deps.ps1")
	for _, required := range []string{
		`[ValidateSet("x64", "ARM64")][string[]]$Platforms = @("x64", "ARM64")`,
		`New-Item -ItemType Directory -Force -Path $wintunDir`,
		"$architectures = foreach ($platform in $Platforms)",
		"foreach ($architecture in $architectures)",
		"foreach ($arch in $architectures)",
	} {
		if !strings.Contains(fetchDependencies, required) {
			t.Errorf("fetch-deps.ps1 is missing selected-platform contract %q", required)
		}
	}
	if strings.Contains(fetchDependencies, `foreach ($arch in @("amd64", "arm64"))`) {
		t.Fatal("fetch-deps.ps1 still prepares both SDK architectures unconditionally")
	}
	wintunRoot := strings.Index(fetchDependencies, `New-Item -ItemType Directory -Force -Path $wintunDir`)
	wintunHeader := strings.Index(fetchDependencies, `Copy-Item "$wintunExtract\wintun\include\wintun.h"`)
	if wintunRoot < 0 || wintunHeader < 0 || wintunRoot >= wintunHeader {
		t.Fatalf("Wintun root must exist before copying its header: root=%d header=%d", wintunRoot, wintunHeader)
	}
}

// consecutiveLines returns the byte offset of the first of len(want) adjacent
// lines whose trimmed text equals want, in order, or -1. It ignores
// indentation and a CR before the LF, so a CRLF checkout matches as well.
func consecutiveLines(source string, want ...string) int {
	lines := strings.Split(source, "\n")
	offset := 0
	for start := range lines {
		if start+len(want) <= len(lines) {
			matched := true
			for index, line := range want {
				if strings.TrimSpace(lines[start+index]) != line {
					matched = false
					break
				}
			}
			if matched {
				return offset
			}
		}
		offset += len(lines[start]) + 1
	}
	return -1
}

// urnetwork/build's release VM builds through app/build.ps1, which used to
// pass only /p:Version, a property no project reads: every official MSI
// shipped as ProductVersion 0.0.1 around code-0 binaries that never update.
// build.ps1 must derive the version with tools/UrVersion.ps1 and hand every
// value to both builds. The arrays' exact content is checked against the Go
// oracle in ur_version_test.go; this checks that they reach both builds, that
// the projects read them down to the VERSIONINFO fields, and that the local
// builds' 0.0.0-0 still builds, unstamped.
func TestReleaseBuildStampsTheVersion(t *testing.T) {
	root := repositoryRoot(t)
	read := func(relative string) string {
		data, err := os.ReadFile(filepath.Join(root, filepath.FromSlash(relative)))
		if err != nil {
			t.Fatal(err)
		}
		return string(data)
	}
	appBuild := read("app/build.ps1")
	urVersion := read("app/tools/UrVersion.ps1")
	props := read("app/Directory.Build.props")
	wixProject := read("app/installer/Installer.wixproj")

	// The six msbuild properties and the installer's version, each built
	// exactly once into the arrays UrVersion.ps1 returns, each read by a
	// project.
	for _, stamp := range []struct{ argument, reader, readerFile string }{
		{`"/p:UrVersion=$Version"`, "UR_VERSION_RAW=$(UrVersion);", props},
		{`"/p:UrVersionCode=$code"`, "UR_VERSION_CODE=$(UrVersionCode);", props},
		{`"/p:UrVersionMajor=$year"`, "UR_VER_MAJOR=$(UrVersionMajor);", props},
		{`"/p:UrVersionMinor=$month"`, "UR_VER_MINOR=$(UrVersionMinor);", props},
		{`"/p:UrVersionPatch=$day"`, "UR_VER_PATCH=$(UrVersionPatch);", props},
		{`"/p:UrVersionBuild=$build"`, "UR_VERSION_BUILD=$(UrVersionBuild);", props},
		{`"-p:UrMsiVersion=$msiVersion"`, "UrMsiVersion=$(UrMsiVersion)", wixProject},
	} {
		if count := strings.Count(urVersion, stamp.argument); count != 1 {
			t.Errorf("UrVersion.ps1 builds %s %d times, want once", stamp.argument, count)
		}
		// ClCompile and ResourceCompile each need the definition.
		want := 1
		if stamp.readerFile == props {
			want = 2
		}
		if count := strings.Count(stamp.readerFile, stamp.reader); count != want {
			t.Errorf("%s appears %d times in its project file, want %d", stamp.reader, count, want)
		}
	}

	// build.ps1 derives once, before the platform loop, from the version
	// run.sh passes, and splats the two arrays into the two builds.
	derive := strings.Index(appBuild, `$urVersion = & (Join-Path $PSScriptRoot "tools\UrVersion.ps1") -Version $Version`)
	platformLoop := strings.Index(appBuild, "foreach ($platform in $Platforms)")
	if derive < 0 || platformLoop < 0 || derive > platformLoop {
		t.Errorf("build.ps1 must derive the version through tools/UrVersion.ps1 before building: derive=%d loop=%d", derive, platformLoop)
	}
	for _, required := range []string{
		"$urMsbuildArgs = @($urVersion.MsbuildArgs)",
		"$urWixArgs = @($urVersion.WixArgs)",
		`"-p:Platform=$platform", "-p:BinDir=$bin", "-p:Version=$Version") + $urWixArgs`,
		"dotnet @wixArgs",
	} {
		if strings.Count(appBuild, required) != 1 {
			t.Errorf("build.ps1 must contain %q exactly once", required)
		}
	}
	if consecutiveLines(appBuild, "@urMsbuildArgs `", "/p:Version=$Version /m /nologo /v:minimal") < 0 {
		t.Error("build.ps1's solution build does not carry @urMsbuildArgs")
	}

	// 0.0.0-0 is the version build.sh, test-main.sh and urnetwork/build's local
	// Windows build pass. It is not a release version (UrVersion.ps1 refuses
	// it), so build.ps1 builds it unstamped, as before: its branch empties both
	// arrays, and the arrays are the only way a Ur* property reaches a build.
	dev := consecutiveLines(appBuild,
		`if ($Version -eq "0.0.0-0") {`,
		"$urMsbuildArgs = @()",
		"$urWixArgs = @()")
	release := consecutiveLines(appBuild,
		"} else {",
		`$urVersion = & (Join-Path $PSScriptRoot "tools\UrVersion.ps1") -Version $Version`)
	if dev < 0 || release < 0 || !(dev < release && release < platformLoop) {
		t.Errorf("build.ps1 must build 0.0.0-0 with no Ur* arguments and derive every other version: dev=%d release=%d loop=%d",
			dev, release, platformLoop)
	}
	if count := strings.Count(appBuild, "p:Ur"); count != 0 {
		t.Errorf(`build.ps1 names a Ur* property itself (%d "p:Ur"), outside the arrays a 0.0.0-0 build empties`, count)
	}
	for _, caller := range []struct{ name, line string }{
		{"build.sh", `EXTERNAL_WARP_VERSION="${EXTERNAL_WARP_VERSION:-0.0.0-0}" \`},
		{"test-main.sh", `version="${EXTERNAL_WARP_VERSION:-0.0.0-0}"`},
	} {
		if !strings.Contains(read(caller.name), caller.line) {
			t.Errorf("%s no longer defaults to the 0.0.0-0 that build.ps1 builds unstamped: want %q", caller.name, caller.line)
		}
	}

	// Both VERSIONINFO resources end FILEVERSION and PRODUCTVERSION in
	// UR_VERSION_BUILD, so two builds of one day differ in file version.
	statement := regexp.MustCompile(`(?m)^[ \t]*(FILEVERSION|PRODUCTVERSION)[ \t]+([^\r\n]*)`)
	const wantFields = "UR_VER_MAJOR,UR_VER_MINOR,UR_VER_PATCH,UR_VERSION_BUILD"
	for _, resource := range []string{"app/src/App/App.rc", "app/src/Service/Service.rc"} {
		found := map[string]int{}
		for _, match := range statement.FindAllStringSubmatch(read(resource), -1) {
			found[match[1]]++
			if fields := strings.Join(strings.Fields(match[2]), ""); fields != wantFields {
				t.Errorf("%s: %s %s, want %s", resource, match[1], match[2], wantFields)
			}
		}
		if found["FILEVERSION"] != 1 || found["PRODUCTVERSION"] != 1 {
			t.Errorf("%s: want one FILEVERSION and one PRODUCTVERSION statement, found %v", resource, found)
		}
	}
}

func TestInstallerContract(t *testing.T) {
	root := repositoryRoot(t)
	packageXML := parseXML(t, filepath.Join(root, "app", "installer", "Package.wxs"))
	for _, node := range packageXML.descendants(wixNamespace, "ServiceConfig") {
		if _, ok := node.attribute("DelayedAutoStart"); ok {
			t.Fatal("ordinary auto-start service redundantly authors delayed-auto-start metadata")
		}
	}
	if len(packageXML.descendants(utilNamespace, "ServiceConfig")) == 0 {
		t.Fatal("urnetworkd failure-action service configuration is missing")
	}

	runtimeFiles := findByID(packageXML.descendants(wixNamespace, "ComponentGroup"), "RuntimeFiles")
	if runtimeFiles == nil {
		t.Fatal("RuntimeFiles component group is missing")
	}
	harvested := runtimeFiles.child(wixNamespace, "Files")
	if harvested == nil {
		t.Fatal("RuntimeFiles harvester is missing")
	}
	excluded := map[string]bool{}
	for _, node := range harvested.children(wixNamespace, "Exclude") {
		if files, ok := node.attribute("Files"); ok {
			excluded[windowsBase(files)] = true
		}
	}
	var missingExclusions []string
	for name := range fontNames {
		if !excluded[name] {
			missingExclusions = append(missingExclusions, name)
		}
	}
	sort.Strings(missingExclusions)
	if len(missingExclusions) != 0 {
		t.Fatalf("private fonts are not excluded from generic harvesting: %v", missingExclusions)
	}

	components := packageXML.descendants(wixNamespace, "Component")
	for _, component := range components {
		componentID, _ := component.attribute("Id")
		if componentID == "" {
			componentID = "<anonymous>"
		}
		for _, file := range component.children(wixNamespace, "File") {
			if _, ok := file.attribute("Subdirectory"); ok {
				source, _ := file.attribute("Source")
				t.Fatalf("%s/%s uses File/@Subdirectory under Component; WiX v5 requires Component/@Subdirectory", componentID, windowsBase(source))
			}
		}
	}

	if findByID(components, "AppExe") == nil {
		t.Fatal("AppExe component is missing")
	}
	privateFonts := findByID(components, "PrivateFonts")
	if privateFonts == nil {
		t.Fatal("PrivateFonts component is missing")
	}
	subdirectory, _ := privateFonts.attribute("Subdirectory")
	if strings.ReplaceAll(subdirectory, `\`, `/`) != "Assets/Fonts" {
		t.Fatal("PrivateFonts component would not install under Assets/Fonts")
	}
	keyPath, _ := privateFonts.attribute("KeyPath")
	if !strings.EqualFold(keyPath, "yes") {
		t.Fatal("PrivateFonts must use its directory as the component key path")
	}
	guid, _ := privateFonts.attribute("Guid")
	if guid == "" || guid == "*" {
		t.Fatal("directory-keyed PrivateFonts component needs an explicit stable Guid")
	}

	explicit := map[string]*xmlNode{}
	for _, node := range privateFonts.children(wixNamespace, "File") {
		source, _ := node.attribute("Source")
		name := windowsBase(source)
		if fontNames[name] {
			explicit[name] = node
		}
	}
	if missing := missingNames(explicit); len(missing) != 0 || len(explicit) != len(fontNames) {
		t.Fatalf("private fonts are not companion files in the PrivateFonts component: %v", missing)
	}
	for name, node := range explicit {
		companion, _ := node.attribute("CompanionFile")
		if companion != "URnetworkExe" {
			t.Fatalf("%s does not inherit versioning from URnetworkExe", name)
		}
		if _, ok := node.attribute("DefaultLanguage"); ok {
			t.Fatalf("%s invents language metadata absent from the font file", name)
		}
		if fileKeyPath, _ := node.attribute("KeyPath"); strings.EqualFold(fileKeyPath, "yes") {
			t.Fatalf("%s is both a key path and a companion file", name)
		}
		if _, ok := node.attribute("TrueType"); ok {
			t.Fatalf("%s would be registered globally instead of remaining app-local", name)
		}
		if _, ok := node.attribute("FontTitle"); ok {
			t.Fatalf("%s would be registered globally instead of remaining app-local", name)
		}
	}

	mainFeature := findByID(packageXML.descendants(wixNamespace, "Feature"), "Main")
	if mainFeature == nil {
		t.Fatal("Main feature is missing")
	}
	installsPrivateFonts := false
	for _, reference := range mainFeature.children(wixNamespace, "ComponentRef") {
		if id, _ := reference.attribute("Id"); id == "PrivateFonts" {
			installsPrivateFonts = true
		}
	}
	if !installsPrivateFonts {
		t.Fatal("Main feature does not install the PrivateFonts component")
	}

	wixProject := parseXML(t, filepath.Join(root, "app", "installer", "Installer.wixproj"))
	warningPolicies := wixProject.descendants("", "TreatWarningsAsErrors")
	if len(warningPolicies) == 0 || !strings.EqualFold(strings.TrimSpace(warningPolicies[0].Text), "true") {
		t.Fatal("WiX warnings are not fatal")
	}

	// An update removes the old product inside its own transaction, so a
	// failure rolls back to the old version rather than leaving neither, and
	// of two codes that share a ProductVersion, the package installed second
	// replaces the other instead of installing beside it (Package.wxs).
	majorUpgrades := packageXML.descendants(wixNamespace, "MajorUpgrade")
	if len(majorUpgrades) != 1 {
		t.Fatalf("want one MajorUpgrade, got %d", len(majorUpgrades))
	}
	if schedule, _ := majorUpgrades[0].attribute("Schedule"); schedule != "afterInstallInitialize" {
		t.Errorf("MajorUpgrade Schedule = %q, want afterInstallInitialize", schedule)
	}
	if same, _ := majorUpgrades[0].attribute("AllowSameVersionUpgrades"); !strings.EqualFold(same, "yes") {
		t.Errorf("MajorUpgrade AllowSameVersionUpgrades = %q, want yes", same)
	}
	suppressed := map[string]bool{}
	for _, node := range wixProject.descendants("", "SuppressIces") {
		for _, ice := range strings.Split(node.Text, ";") {
			suppressed[strings.TrimSpace(ice)] = true
		}
	}
	if len(suppressed) != 2 || !suppressed["ICE03"] || !suppressed["ICE61"] {
		t.Errorf("SuppressIces = %v, want exactly ICE03 and ICE61", suppressed)
	}

	// The running app is closed before the files are replaced, by nothing that
	// needs a deferred action, and the in-app update's relaunch goes through
	// the helper (update_apply_wiring_test.go).
	for _, problem := range checkInstallerCloseAndRelaunch(packageXML) {
		t.Error(problem)
	}
}

// A package must not install over a newer urnetworkd.exe. Windows Installer
// would skip ("disallow") each component whose installed key file is newer,
// and the old product's removal would then delete those files, leaving the
// machine with no service. The guard is an AppSearch for urnetworkd.exe where
// INSTALLFOLDER puts it, at or above this build's FILEVERSION with the fourth
// field plus one, and a launch condition on what it finds (Package.wxs).
func TestInstallerRefusesAnOlderService(t *testing.T) {
	root := repositoryRoot(t)
	read := func(relative string) string {
		data, err := os.ReadFile(filepath.Join(root, filepath.FromSlash(relative)))
		if err != nil {
			t.Fatal(err)
		}
		return string(data)
	}
	packageXML := parseXML(t, filepath.Join(root, "app", "installer", "Package.wxs"))

	// The search: one secure property, holding a search of the folder the
	// service is installed in, at depth 0, for urnetworkd.exe at the version
	// Installer.wixproj derives.
	var guard *xmlNode
	for _, node := range packageXML.descendants(wixNamespace, "Property") {
		if id, _ := node.attribute("Id"); id == "NEWER_URNETWORKD" {
			guard = node
		}
	}
	if guard == nil {
		t.Fatal("Package.wxs has no NEWER_URNETWORKD search for an installed, newer urnetworkd.exe")
	}
	if secure, _ := guard.attribute("Secure"); !strings.EqualFold(secure, "yes") {
		t.Errorf("NEWER_URNETWORKD Secure = %q, want yes", secure)
	}
	searches := guard.children(wixNamespace, "DirectorySearch")
	if len(searches) != 1 {
		t.Fatalf("NEWER_URNETWORKD holds %d DirectorySearch elements, want 1", len(searches))
	}
	for attribute, want := range map[string]string{"Path": "[ProgramFiles64Folder]URnetwork", "Depth": "0"} {
		if got, _ := searches[0].attribute(attribute); got != want {
			t.Errorf("the guard's DirectorySearch %s = %q, want %q", attribute, got, want)
		}
	}
	fileSearches := searches[0].children(wixNamespace, "FileSearch")
	if len(fileSearches) != 1 {
		t.Fatalf("the guard's DirectorySearch holds %d FileSearch elements, want 1", len(fileSearches))
	}
	for attribute, want := range map[string]string{"Name": "urnetworkd.exe", "MinVersion": "$(var.UrNewerFileVersion)"} {
		if got, _ := fileSearches[0].attribute(attribute); got != want {
			t.Errorf("the guard's FileSearch %s = %q, want %q", attribute, got, want)
		}
	}

	// That folder is where the service really goes: INSTALLFOLDER, named
	// URnetwork under ProgramFiles64Folder, directly holds the ServiceExe
	// component and its urnetworkd.exe. AppSearch runs before directories
	// resolve, so the search cannot name INSTALLFOLDER itself.
	var installFolder *xmlNode
	for _, standard := range packageXML.descendants(wixNamespace, "StandardDirectory") {
		if id, _ := standard.attribute("Id"); id == "ProgramFiles64Folder" {
			installFolder = findByID(standard.children(wixNamespace, "Directory"), "INSTALLFOLDER")
		}
	}
	if installFolder == nil {
		t.Fatal("INSTALLFOLDER is no longer a Directory directly under ProgramFiles64Folder; move the guard's search with it")
	}
	if name, _ := installFolder.attribute("Name"); name != "URnetwork" {
		t.Errorf("INSTALLFOLDER Name = %q, but the guard searches [ProgramFiles64Folder]URnetwork", name)
	}
	service := findByID(installFolder.children(wixNamespace, "Component"), "ServiceExe")
	if service == nil {
		t.Fatal("the ServiceExe component is no longer directly in INSTALLFOLDER, where the guard searches")
	}
	installsService := false
	for _, file := range service.children(wixNamespace, "File") {
		if source, _ := file.attribute("Source"); strings.HasSuffix(source, `\urnetworkd.exe`) {
			installsService = true
		}
	}
	if !installsService {
		t.Error("ServiceExe no longer installs urnetworkd.exe, the file the guard searches for")
	}

	// The refusal: allowed only for the installed product itself (repair,
	// uninstall), never for a package installing over a newer service.
	refuses := false
	for _, launch := range packageXML.descendants(wixNamespace, "Launch") {
		condition, _ := launch.attribute("Condition")
		message, _ := launch.attribute("Message")
		if condition == "Installed OR NOT NEWER_URNETWORKD" && strings.TrimSpace(message) != "" {
			refuses = true
		}
	}
	if !refuses {
		t.Error(`Package.wxs has no Launch Condition="Installed OR NOT NEWER_URNETWORKD" with a message`)
	}

	// The version: Installer.wixproj derives UrNewerFileVersion from this
	// build's FILEVERSION, the fourth field plus one, with an unstamped
	// build's 0.0.0.0 as the default, and hands it to WiX.
	wixProject := read("app/installer/Installer.wixproj")
	for _, line := range []string{
		`<UrFileVersion Condition="'$(UrFileVersion)'==''">0.0.0.0</UrFileVersion>`,
		`<UrNewerFileVersion Condition="$([System.Version]::Parse('$(UrFileVersion)').Revision) &gt;= 0">` +
			`$([System.Version]::Parse('$(UrFileVersion)').ToString(3)).` +
			`$([MSBuild]::Add($([System.Version]::Parse('$(UrFileVersion)').Revision), 1))</UrNewerFileVersion>`,
		`<DefineConstants>BinDir=$(BinDir);UrMsiVersion=$(UrMsiVersion);UrNewerFileVersion=$(UrNewerFileVersion)</DefineConstants>`,
	} {
		if consecutiveLines(wixProject, line) < 0 {
			t.Errorf("Installer.wixproj is missing %s", line)
		}
	}

	// And every stamped build passes its FILEVERSION: UrVersion.ps1's WiX
	// arguments, which build.ps1 splats into the WiX build.
	if count := strings.Count(read("app/tools/UrVersion.ps1"), `"-p:UrFileVersion=$year.$month.$day.$build"`); count != 1 {
		t.Errorf("UrVersion.ps1 builds -p:UrFileVersion %d times, want once", count)
	}
}

func TestNeutralPluralResources(t *testing.T) {
	root := repositoryRoot(t)
	stringsRoot := filepath.Join(root, "app", "src", "App", "Strings")
	resourceNames := func(filename string) map[string]bool {
		document := parseXML(t, filename)
		result := map[string]bool{}
		for _, node := range document.descendants("", "data") {
			if name, ok := node.attribute("name"); ok {
				result[name] = true
			}
		}
		return result
	}

	neutral := resourceNames(filepath.Join(stringsRoot, "en", "Resources.resw"))
	files, err := filepath.Glob(filepath.Join(stringsRoot, "*", "Resources.resw"))
	if err != nil {
		t.Fatal(err)
	}
	pluralSuffixes := map[string]bool{
		"zero": true, "one": true, "two": true,
		"few": true, "many": true, "other": true,
	}
	qualified := map[string]bool{}
	for _, filename := range files {
		for name := range resourceNames(filename) {
			separator := strings.LastIndexByte(name, '.')
			if separator > 0 && pluralSuffixes[name[separator+1:]] {
				qualified[name] = true
			}
		}
	}
	var missing []string
	for name := range qualified {
		if !neutral[name] {
			missing = append(missing, name)
		}
	}
	sort.Strings(missing)
	if len(missing) != 0 {
		if len(missing) > 8 {
			missing = missing[:8]
		}
		t.Fatalf("neutral Resources.resw omits plural resource names: %v", missing)
	}
}

func TestWalletBridgeReturnsAreRouted(t *testing.T) {
	host := readAppSource(t, "SdkHost.cpp")
	for _, required := range []string{
		"bridge::RoutePublicKey(",
		"bridge::RouteSignature(",
	} {
		if !strings.Contains(host, required) {
			t.Fatalf("SdkHost.cpp does not route wallet-bridge returns through %s; a return no flow is waiting for would fall through to a wallet sign-in", required)
		}
	}

	const cancel = `CancelPendingWalletFlows("`
	reasons := 0
	for rest := host; ; {
		index := strings.Index(rest, cancel)
		if index < 0 {
			break
		}
		rest = rest[index+len(cancel):]
		reasons++
		if !strings.HasPrefix(rest, "superseded by ") {
			end := strings.IndexByte(rest, '"')
			if end < 0 {
				end = len(rest)
			}
			t.Errorf("CancelPendingWalletFlows reason %q does not start with %q, the prefix a page settles a superseded flow by", rest[:end], "superseded by ")
		}
	}
	if reasons == 0 {
		t.Fatal("SdkHost.cpp no longer answers a superseded wallet flow with a literal reason")
	}

	page := readAppSource(t, "WalletPage.cpp")
	if !strings.Contains(page, "bridge::IsSuperseded(") {
		t.Fatal("WalletPage.cpp shows a superseded wallet flow as an error; it must settle one quietly through bridge::IsSuperseded")
	}
}

func TestWalletChallengesCheckTheirFlow(t *testing.T) {
	host := readAppSource(t, "SdkHost.cpp")
	if !strings.Contains(host, "walletFlows_.Start()") {
		t.Fatal("CancelPendingWalletFlows no longer starts a new wallet flow number")
	}
	// every call site, less the definition
	challenges := strings.Count(host, "RequestWalletChallenge(") - 1
	checks := strings.Count(host, "walletFlows_.IsCurrent(")
	if challenges < 1 {
		t.Fatal("SdkHost.cpp no longer fetches a wallet challenge; update this contract")
	}
	if checks < challenges {
		t.Fatalf("SdkHost.cpp fetches %d wallet challenges but checks the flow serial %d times; a flow superseded while its challenge was on its way would open the bridge over the current flow", challenges, checks)
	}
}

func TestWalletSignInClearsAnAbandonedSsoAttempt(t *testing.T) {
	host := readAppSource(t, "SdkHost.cpp")
	body := func(signature string) string {
		start := strings.Index(host, signature)
		if start < 0 {
			t.Fatalf("SdkHost.cpp no longer defines %s", signature)
		}
		end := strings.Index(host[start:], "\n}\n")
		if end < 0 {
			t.Fatalf("cannot find the end of %s", signature)
		}
		return host[start : start+end]
	}
	// A wallet sign-in is waiting when walletAuthDone_ is set and no sso attempt
	// owns it. An abandoned Google or Apple attempt left in place would drop the
	// wallet's return, and the login screen would wait for it forever.
	if !strings.Contains(body("SdkHost::CancelPendingWalletFlows("), "ssoAttempt_.reset();") {
		t.Fatal("CancelPendingWalletFlows no longer clears the sso attempt, so a wallet sign-in started after an abandoned Google or Apple tab would never see its return")
	}
	for _, signature := range []string{"SdkHost::SignInWithSolana(", "SdkHost::SignInWithBittensor("} {
		definition := body(signature)
		cancelAt := strings.Index(definition, "CancelPendingWalletFlows(")
		waitingAt := strings.Index(definition, "walletAuthDone_ = ")
		if cancelAt < 0 || waitingAt < 0 || cancelAt > waitingAt {
			t.Fatalf("%s must clear the pending flows, the sso attempt among them, before it waits for its sign-in", signature)
		}
	}
}

// stripLineComments blanks every // comment so a contract reads the code and
// not the prose around it. Lines are kept, so an offset still names its line; a
// // inside a string literal ("https://") is not a comment, and a quote inside a
// character literal ('"') does not open a string.
func stripLineComments(source string) string {
	lines := strings.Split(source, "\n")
	for index, line := range lines {
		inString := false
	scan:
		for at := 0; at < len(line); at++ {
			switch character := line[at]; {
			case inString && character == '\\':
				at++
			case character == '"':
				inString = !inString
			case !inString && character == '\'' && opensCharacterLiteral(line, at):
				// a character literal
				for at++; at < len(line) && line[at] != '\''; at++ {
					if line[at] == '\\' {
						at++
					}
				}
			case !inString && character == '/' && at+1 < len(line) && line[at+1] == '/':
				lines[index] = line[:at]
				break scan
			}
		}
	}
	return strings.Join(lines, "\n")
}

func isIdentifierByte(character byte) bool {
	return character == '_' ||
		'0' <= character && character <= '9' ||
		'a' <= character && character <= 'z' ||
		'A' <= character && character <= 'Z'
}

// Whether the quote at `at` opens a character literal. A quote after a word is
// a digit separator (21'600, 0xFF'FF), except after a whole encoding-prefix
// word (L'x', u'x', U'x', u8'x'), which opens a literal as a bare quote does.
func opensCharacterLiteral(code string, at int) bool {
	start := at
	for start > 0 && isIdentifierByte(code[start-1]) {
		start--
	}
	switch code[start:at] {
	case "", "L", "u", "U", "u8":
		return true
	}
	return false
}

// The comment strippers read a prefixed character literal (L'"', u8'[') as one,
// so a quote inside it opens no string and the comment after it is still
// stripped, and still read a quote after a digit as a digit separator.
func TestCommentStrippersReadPrefixedCharacterLiterals(t *testing.T) {
	cases := []struct {
		source  string
		comment string
	}{
		{source: `auto quote = text.find(L'"'); `, comment: `// a "quoted" note`},
		{source: `auto open = text.find(u8'['); `, comment: `/* a "quoted" note */`},
		{source: `auto mask = 0xFF'FF + U'x'; `, comment: `// it's a mask`},
		{source: `int ms = 21'600; `, comment: `/* 1'000 "ms" */`},
	}
	for _, c := range cases {
		code := stripComments(c.source + c.comment)
		if want := c.source + strings.Repeat(" ", len(c.comment)); code != want {
			t.Errorf("stripComments(%q) = %q, want %q", c.source+c.comment, code, want)
		}
		if !strings.HasPrefix(c.comment, "//") {
			continue
		}
		if lineCode := stripLineComments(c.source + c.comment); lineCode != c.source {
			t.Errorf("stripLineComments(%q) = %q, want %q", c.source+c.comment, lineCode, c.source)
		}
	}
}

// stripComments blanks every // and /* */ comment, as stripLineComments does
// for //, so a contract that must find a statement cannot find it in a comment.
// Line breaks are kept, so an offset still names its line, and string and
// character literals are read the same way.
func stripComments(source string) string {
	out := []byte(source)
	skipLiteral := func(at int, quote byte) int {
		for at++; at < len(out) && out[at] != quote && out[at] != '\n'; at++ {
			if out[at] == '\\' {
				at++
			}
		}
		return at
	}
	blank := func(from, to int) {
		for ; from < to && from < len(out); from++ {
			if out[from] != '\n' {
				out[from] = ' '
			}
		}
	}
	for at := 0; at < len(out); at++ {
		switch {
		case out[at] == '"':
			at = skipLiteral(at, '"')
		case out[at] == '\'' && opensCharacterLiteral(source, at):
			// a character literal; the word before a quote in code is never
			// part of a comment, so the source answers for `out`
			at = skipLiteral(at, '\'')
		case out[at] == '/' && at+1 < len(out) && out[at+1] == '/':
			end := strings.IndexByte(string(out[at:]), '\n')
			if end < 0 {
				end = len(out) - at
			}
			blank(at, at+end)
			at += end
		case out[at] == '/' && at+1 < len(out) && out[at+1] == '*':
			end := strings.Index(string(out[at+2:]), "*/")
			stop := len(out)
			if end >= 0 {
				stop = at + 2 + end + 2
			}
			blank(at, stop)
			at = stop - 1
		}
	}
	return string(out)
}

// A closing brace in the first column, then the end of its line. stripComments leaves blanks where a
// trailing comment was, so "}  // end" and "}  " end a definition too.
var definitionEnd = regexp.MustCompile(`\n\}[ \t]*\n`)

// definitionBody is the source of the top-level definition that starts at
// signature, through the closing brace in its first column.
func definitionBody(t *testing.T, name, source, signature string) string {
	t.Helper()
	start := strings.Index(source, signature)
	if start < 0 {
		t.Fatalf("%s no longer defines %s; update this contract", name, signature)
	}
	end := definitionEnd.FindStringIndex(source[start:])
	if end == nil {
		t.Fatalf("cannot find the end of %s in %s", signature, name)
	}
	return source[start : start+end[0]+2]
}

// handlerSource is the source of the handler registered at opener, through the
// "});" that closes its registration.
func handlerSource(t *testing.T, name, source, opener string) string {
	t.Helper()
	start := strings.Index(source, opener)
	if start < 0 {
		t.Fatalf("%s no longer registers %s; update this contract", name, opener)
	}
	end := strings.Index(source[start:], "});")
	if end < 0 {
		t.Fatalf("cannot find the end of the %s registration in %s", opener, name)
	}
	return source[start : start+end+len("});")]
}

// appSourceFiles is every app/src/App file with one of the extensions, keyed by
// its slash-separated path under that directory. Build output is not source.
func appSourceFiles(t *testing.T, extensions ...string) map[string]string {
	t.Helper()
	root := filepath.Join(repositoryRoot(t), "app", "src", "App")
	skipped := map[string]bool{"Generated Files": true, "x64": true, "ARM64": true, "bin": true, "obj": true}
	files := map[string]string{}
	err := filepath.WalkDir(root, func(path string, entry os.DirEntry, walkErr error) error {
		if walkErr != nil {
			return walkErr
		}
		if entry.IsDir() {
			if path != root && skipped[entry.Name()] {
				return filepath.SkipDir
			}
			return nil
		}
		for _, extension := range extensions {
			if !strings.EqualFold(filepath.Ext(path), extension) {
				continue
			}
			data, err := os.ReadFile(path)
			if err != nil {
				return err
			}
			relative, err := filepath.Rel(root, path)
			if err != nil {
				return err
			}
			files[filepath.ToSlash(relative)] = string(data)
			break
		}
		return nil
	})
	if err != nil {
		t.Fatal(err)
	}
	return files
}

func sortedNames(files map[string]string) []string {
	names := make([]string, 0, len(files))
	for name := range files {
		names = append(names, name)
	}
	sort.Strings(names)
	return names
}

// A WM_CLOSE sent to the tray's hidden window from outside the app (`taskkill
// /im` without /f, an installer closing the app) exits the app through the
// close-request callback. DefWindowProc would destroy the window and leave the
// app running with no icon and no way to quit it. It is not the menu's Quit,
// which also stops the tunnel and the provider (AppLifetime.h), and Alt+F4 no
// longer reaches it (quit_tray_wiring_test.go). The contract reads the code
// with every comment blanked, and the case may only log and then call the
// close-request callback, so a commented-out call, a call behind a constant
// condition or a preprocessor block, an early return and DefWindowProc all
// fail it.
func TestTrayWindowExitsOnCloseRequest(t *testing.T) {
	source := stripComments(readAppSource(t, "TrayIcon.cpp"))
	wndProc := definitionBody(t, "TrayIcon.cpp", source, "LRESULT CALLBACK TrayIcon::WndProc(")
	const label = "case WM_CLOSE:"
	if count := strings.Count(wndProc, label); count != 1 {
		t.Fatalf("TrayIcon::WndProc handles WM_CLOSE %d times, want once", count)
	}
	body := wndProc[strings.Index(wndProc, label)+len(label):]
	end := strings.Index(body, "return 0;")
	if end < 0 {
		t.Fatal("TrayIcon::WndProc's WM_CLOSE case never returns 0")
	}
	exits := 0
	for _, line := range strings.Split(body[:end], "\n") {
		switch statement := strings.TrimSpace(line); {
		case statement == "":
		case statement == "if (self->cb_.onCloseRequest) self->cb_.onCloseRequest();":
			exits++
		case strings.HasPrefix(statement, "Log"):
		default:
			t.Errorf("TrayIcon::WndProc's WM_CLOSE case runs %q; it should only log and exit", statement)
		}
	}
	if exits != 1 {
		t.Errorf("TrayIcon::WndProc's WM_CLOSE case calls the close-request callback %d times, want once", exits)
	}
}

// A hide to the tray closes whatever sheet is open, through one sweep of the
// open dialogs that a sheet added later cannot slip past; a minimize keeps its
// sheet, as it keeps the window.
func TestHideToTrayClosesEveryOpenSheet(t *testing.T) {
	controller := stripLineComments(readAppSource(t, "AppController.cpp"))
	hide := definitionBody(t, "AppController.cpp", controller, "void AppController::HideWindow() {")
	sweep := strings.Index(hide, "->CloseSheetsForHide();")
	windowHide := strings.Index(hide, ".AppWindow().Hide();")
	if sweep < 0 || windowHide < 0 || sweep > windowHide {
		t.Fatal("AppController::HideWindow must close the open sheets through MainWindow::CloseSheetsForHide before the window hides to the tray")
	}
	if count := strings.Count(controller, "CloseSheetsForHide("); count != 1 {
		t.Fatalf("AppController.cpp closes the sheets from %d places; only HideWindow may", count)
	}

	// The minimize box never reaches HideWindow. It raises Window.VisibilityChanged,
	// whose handler re-reads IsIconic and reconciles the presentation, and nothing
	// on that path closes a sheet.
	for _, minimize := range []string{
		handlerSource(t, "AppController.cpp", controller, "window_.VisibilityChanged("),
		definitionBody(t, "AppController.cpp", controller, "void AppController::SyncWindowMinimized() {"),
		definitionBody(t, "AppController.cpp", controller, "void AppController::ReconcileWindowPresentation() {"),
	} {
		if strings.Contains(minimize, "CloseSheetsForHide") || strings.Contains(minimize, "HideWindow") {
			t.Fatalf("a minimize would close the open sheet:\n%s", minimize)
		}
	}
	// HideWindow's one caller is the window's Closing handler (the caption X,
	// Alt+F4), which hides to the tray instead of closing.
	callers := 0
	for _, source := range appSourceFiles(t, ".cpp") {
		callers += strings.Count(stripLineComments(source), "HideWindow();")
	}
	closing := handlerSource(t, "AppController.cpp", controller, "appWindow.Closing(")
	if callers != 1 || !strings.Contains(closing, "HideWindow();") {
		t.Fatalf("HideWindow is called from %d places; the window's Closing handler must be the only one", callers)
	}

	// The sweep: every open ContentDialog on the window's XamlRoot, dismissed with
	// Hide(), which runs the dialog's own Closing and Closed and returns its
	// ShowAsync as any dismissal does.
	window := stripLineComments(readAppSource(t, "MainWindow.xaml.cpp"))
	sweeper := definitionBody(t, "MainWindow.xaml.cpp", window, "void MainWindow::CloseSheetsForHide() {")
	for _, required := range []string{
		".XamlRoot()",
		"VisualTreeHelper::GetOpenPopupsForXamlRoot(",
		".Child().try_as<ContentDialog>()",
		".Hide();",
	} {
		if !strings.Contains(sweeper, required) {
			t.Errorf("MainWindow::CloseSheetsForHide is missing the open-dialog sweep step %q", required)
		}
	}
	if strings.Contains(sweeper, "IsOpen(") {
		t.Error("MainWindow::CloseSheetsForHide closes a popup directly; a dialog closed that way never returns from ShowAsync, so the sheet gate stays set, and its Closing guard is skipped")
	}
	if count := strings.Count(window, "CloseSheetsForHide("); count != 1 {
		t.Errorf("MainWindow.xaml.cpp names CloseSheetsForHide %d times; only its definition may, since the window's presentation path also runs on a minimize", count)
	}

	// One mechanism: outside a sheet, nothing hides a sheet but the sweep (a sheet
	// still closes itself with dialog_.Hide() when its own work is done). The sweep
	// finds a dialog as its popup's child, which is how a dialog built in code is
	// hosted; one declared in markup is hosted otherwise and would be missed.
	files := appSourceFiles(t, ".cpp", ".h", ".xaml")
	markupDialog := regexp.MustCompile(`<(?:[A-Za-z_][\w.]*:)?ContentDialog[\s/>]`)
	for _, name := range sortedNames(files) {
		if strings.HasSuffix(name, ".xaml") {
			if markupDialog.MatchString(files[name]) {
				t.Errorf("%s declares a ContentDialog in markup; the hide's sweep finds dialogs built in code, so this one would stay open across a hide", name)
			}
			continue
		}
		code := stripLineComments(files[name])
		if strings.Contains(code, "Dialog().Hide()") {
			t.Errorf("%s hides a sheet it holds; a hide to the tray closes every sheet through MainWindow::CloseSheetsForHide", name)
		}
		if strings.Contains(code, "CloseButtonClick(") {
			t.Errorf("%s acts on a sheet's CloseButtonClick; Hide() and Esc never raise it, so a hide to the tray would skip that work (use Closing or Closed)", name)
		}
	}

	// The one sheet a hide leaves open: the seedphrase sheet refuses every close
	// but its confirm, the sweep's included, because it shows the only copy of the
	// credential of a network the server has already created.
	auth := stripLineComments(readAppSource(t, "AuthSheets.cpp"))
	guard := handlerSource(t, "AuthSheets.cpp", auth, "dialog_.Closing(")
	if !strings.Contains(guard, "args.Result() == ContentDialogResult::None && !self->confirmed_") ||
		!strings.Contains(guard, "args.Cancel(true);") {
		t.Error("the seedphrase sheet no longer refuses a close without its confirm; a hide to the tray would discard a network whose only credential is on screen")
	}
}

// Every sheet's ShowAsync sits inside the single-sheet gate: checked before the
// sheet opens, set while it shows, and cleared once ShowAsync returns, which it
// does for a programmatic Hide() as for any dismissal. A sheet that skipped the
// clear would lock every other sheet out after a hide to the tray.
func TestEverySheetClearsTheSheetGateWhenItCloses(t *testing.T) {
	files := appSourceFiles(t, ".cpp")
	shows := 0
	for _, name := range sortedNames(files) {
		source := stripLineComments(files[name])
		for offset := 0; ; {
			found := strings.Index(source[offset:], ".ShowAsync(")
			if found < 0 {
				break
			}
			at := offset + found
			offset = at + len(".ShowAsync(")
			shows++
			line := strings.Count(source[:at], "\n") + 1
			start := strings.LastIndex(source[:at], "\n}\n") + 1
			end := strings.Index(source[at:], "\n}\n")
			if end < 0 {
				t.Errorf("%s:%d: cannot find the end of the function that shows this sheet", name, line)
				continue
			}
			before, after := source[start:at], source[at:at+end]
			checked := strings.Contains(before, "sheetOpen()") || strings.Contains(before, "if (sheetOpen_)")
			set := strings.Contains(before, "SetSheetOpen(true)") || strings.Contains(before, "sheetOpen_ = true")
			cleared := strings.Contains(after, "SetSheetOpen(false)") || strings.Contains(after, "sheetOpen_ = false")
			if !checked || !set || !cleared {
				t.Errorf("%s:%d shows a sheet outside the single-sheet gate (checked %t, set %t, cleared after ShowAsync %t)", name, line, checked, set, cleared)
			}
		}
	}
	if shows == 0 {
		t.Fatal("no sheet calls ShowAsync any more; update this contract")
	}
}
