// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"encoding/xml"
	"fmt"
	"os"
	"path/filepath"
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

func TestTunnelWatchdogObservesDestinationGenerationAndReadiness(t *testing.T) {
	header := readServiceSource(t, "TunnelWatchdog.h")
	source := readServiceSource(t, "TunnelWatchdog.cpp")
	for _, required := range []string{
		"ConnectionEpochTracker",
		"providerWindowReady",
		"trafficStartMillis",
	} {
		if !strings.Contains(header, required) {
			t.Fatalf("TunnelWatchdog.h is missing the connection-epoch contract %q", required)
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
