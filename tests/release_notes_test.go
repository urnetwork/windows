// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// The release note the app bundles (App/ReleaseNotes.h), run as its C++ spec
// against the asset the localization store generates; and the call sites that
// need Windows, read from their sources with every comment blanked: the asset
// ships beside the exe, and Settings' App version row opens it.

// Compile and execute the spec (app/tools/release-notes-tests.cpp), whose last
// case reads the shipped Assets/ReleaseNotes.json. Skips without
// nlohmann/json, as the other readers of JSON do.
func TestReleaseNotes(t *testing.T) {
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("release notes tests require a C++20 compiler: ", err)
	}
	jsonFlags := provideJsonFlags(t)
	root := repositoryRoot(t)
	program := filepath.Join(t.TempDir(), "release-notes-tests")
	args := []string{"-std=c++20", "-Wall", "-Wextra", "-Werror", "-I" + filepath.Join(root, "app", "src", "App")}
	args = append(args, jsonFlags...)
	args = append(args, filepath.Join(root, "app", "tools", "release-notes-tests.cpp"), "-o", program)
	if output, err := exec.Command(compiler, args...).CombinedOutput(); err != nil {
		t.Fatalf("build release notes tests: %v\n%s", err, output)
	}
	asset := filepath.Join(root, "app", "src", "App", "Assets", "ReleaseNotes.json")
	if output, err := exec.Command(program, asset).CombinedOutput(); err != nil {
		t.Fatalf("release notes: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}
}

// The app is unpackaged, so the note has to BE beside the exe: App.vcxproj
// copies it to $(OutDir)Assets, where the MSI's harvest and the portable zip
// take everything, and the sheet reads it from there for the UI language.
func TestReleaseNotesShipBesideTheExe(t *testing.T) {
	root := repositoryRoot(t)
	project := parseXML(t, filepath.Join(root, "app", "src", "App", "App.vcxproj"))
	copied := false
	for _, node := range project.descendants(msbuildNamespace, "CopyFileToFolders") {
		if include, _ := node.attribute("Include"); include != `Assets\ReleaseNotes.json` {
			continue
		}
		if destination := node.child(msbuildNamespace, "DestinationFolders"); destination != nil &&
			strings.TrimSpace(destination.Text) == "$(OutDir)Assets" {
			copied = true
		}
	}
	if !copied {
		t.Error(`App.vcxproj does not copy Assets\ReleaseNotes.json to $(OutDir)Assets`)
	}
	listed := false
	for _, node := range project.descendants(msbuildNamespace, "ClInclude") {
		if include, _ := node.attribute("Include"); include == "ReleaseNotes.h" {
			listed = true
		}
	}
	if !listed {
		t.Error("App.vcxproj does not list ReleaseNotes.h")
	}

	sheets := stripComments(readAppSource(t, "SettingsSheets.cpp"))
	bundled := definitionBody(t, "SettingsSheets.cpp", sheets, "releasenotes::Note const* ReleaseNotesSheet::Bundled() {")
	signOutRequireInOrder(t, "ReleaseNotesSheet::Bundled", bundled,
		`ExeDir\(\);`,
		`dir / L"Assets" / L"ReleaseNotes\.json"`,
		`releasenotes::Parse\(body\.str\(\), PrimaryLanguage\(\)\)`,
		`catch \(\.\.\.\)`)
}

// The sheet shows the note as every app does (localizations README, "Release
// notes"): titled release_notes, then the note's title, its own version and
// date (never the running build, which is the App version row's), its intro,
// then each section's heading over its bullets, in the note's own direction.
// The date is the reader's medium date of a calendar day: midnight UTC of the
// day, formatted in UTC.
func TestReleaseNotesTheSheetShowsTheNote(t *testing.T) {
	sheets := stripComments(readAppSource(t, "SettingsSheets.cpp"))
	build := definitionBody(t, "SettingsSheets.cpp", sheets,
		"void ReleaseNotesSheet::Build(XamlRoot const& root, releasenotes::Note const& note) {")
	signOutRequireInOrder(t, "ReleaseNotesSheet::Build", build,
		`MakeSheet\(root, Loc\("release_notes"\)\);`,
		`content\.FlowDirection\(releasenotes::IsRightToLeft\(note\.locale\) \? FlowDirection::RightToLeft\s+: FlowDirection::LeftToRight\);`,
		`NoteLine\(note\.title\)`,
		`Widen\(note\.version\)`,
		`NoteDate\(note\.date\)`,
		`NoteLine\(note\.intro\)`,
		`section\.heading`,
		`NoteLine\(item\)`,
		`dialog_\.Content\(scroll\);`)
	if strings.Contains(build, "version::kString") {
		t.Error("ReleaseNotesSheet::Build shows the running build; the sheet names the note's own version")
	}
	date := definitionBody(t, "SettingsSheets.cpp", sheets, "std::wstring NoteDate(std::string const& date) {")
	signOutRequireInOrder(t, "NoteDate", date,
		`releasenotes::ParseDay\(date\);`,
		`DateTimeFormatter formatter\(\s*L"month\.abbreviated day year"\);`,
		`releasenotes::EpochDays\(\*day\) \* 86'400`,
		`formatter\.Format\(winrt::clock::from_sys\(midnight\), L"UTC"\)`,
		`catch \(\.\.\.\)`)
}

// Both version sections (About's, and its fold copy at the foot of General)
// build the App version row in one place, which opens the note and is the
// plain value row when there is none.
func TestReleaseNotesTheAppVersionRowOpensThem(t *testing.T) {
	settings := stripComments(readAppSource(t, "SettingsPage.cpp"))
	for _, signature := range []string{
		"void SettingsPage::BuildVersionSection(Panel const& host) {",
		"void SettingsPage::BuildVersionFoldSection(Panel const& host) {",
	} {
		body := definitionBody(t, "SettingsPage.cpp", settings, signature)
		if strings.Count(body, "BuildAppVersionRow(card);") != 1 {
			t.Errorf("%s does not build the App version row with BuildAppVersionRow", signature)
		}
		if strings.Contains(body, `"app_version"`) {
			t.Errorf("%s builds an App version row of its own beside BuildAppVersionRow", signature)
		}
	}
	row := definitionBody(t, "SettingsPage.cpp", settings, "void SettingsPage::BuildAppVersionRow(Panel const& card) {")
	signOutRequireInOrder(t, "SettingsPage::BuildAppVersionRow", row,
		`Missing\("app_version", L"App version"\)`,
		`if \(!urnw::ReleaseNotesSheet::Bundled\(\)\) \{`,
		`ApplyValue\(ValueRow\(card, label\), urnw::version::kString\);`,
		`return;`,
		`NavRow\(card, label, value\);`,
		`ApplyValue\(value, urnw::version::kString\);`,
		`SetHelpText\(row, Loc\("release_notes"\)\);`,
		`row\.Click\(\[this\]\(auto const&, auto const&\) \{ ShowReleaseNotesSheet\(\); \}\);`)
	show := definitionBody(t, "SettingsPage.cpp", settings, "winrt::fire_and_forget SettingsPage::ShowReleaseNotesSheet() {")
	signOutRequireInOrder(t, "SettingsPage::ShowReleaseNotesSheet", show,
		`urnw::ReleaseNotesSheet::Bundled\(\);`,
		`if \(!note\) co_return;`,
		`urnw::ReleaseNotesSheet::Create\(self->Content\(\)\.XamlRoot\(\), \*note\);`,
		`releaseNotesSheet_->Dialog\(\)\.ShowAsync\(\);`)
}
