// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// The DNS and transport editors' compare and set (App/EditorApply.h, owner
// report 2026-10-16) as a C++ spec with negative controls, and the SdkHost and
// editor lines that use it, read with every comment blanked.

func editorApplyProgram(t *testing.T, mutate func(string) string) string {
	t.Helper()
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("editor apply tests require a C++20 compiler: ", err)
	}
	root := repositoryRoot(t)
	includeDir := t.TempDir()
	source, err := os.ReadFile(filepath.Join(root, "app", "src", "App", "EditorApply.h"))
	if err != nil {
		t.Fatal(err)
	}
	content := string(source)
	if mutate != nil {
		content = mutate(content)
		if content == string(source) {
			t.Fatal("negative control did not change the production EditorApply.h")
		}
	}
	if err := os.WriteFile(filepath.Join(includeDir, "EditorApply.h"), []byte(content), 0600); err != nil {
		t.Fatal(err)
	}
	program := filepath.Join(includeDir, "editor-apply-tests")
	build := exec.Command(compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I"+includeDir,
		filepath.Join(root, "app", "tools", "editor-apply-tests.cpp"), "-o", program)
	if output, err := build.CombinedOutput(); err != nil {
		t.Fatalf("build editor apply tests: %v\n%s", err, output)
	}
	return program
}

func TestEditorApply(t *testing.T) {
	if !strings.Contains(readAppSource(t, "App.vcxproj"), `<ClInclude Include="EditorApply.h" />`) {
		t.Error("App.vcxproj does not list EditorApply.h")
	}
	if output, err := exec.Command(editorApplyProgram(t, nil)).CombinedOutput(); err != nil {
		t.Fatalf("editor apply: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}
}

func requireEditorApplyFailure(t *testing.T, old, replacement, want string) {
	t.Helper()
	program := editorApplyProgram(t, func(source string) string {
		if strings.Count(source, old) != 1 {
			t.Fatalf("EditorApply.h no longer has exactly one %q; update this control", old)
		}
		return strings.Replace(source, old, replacement, 1)
	})
	output, err := exec.Command(program).CombinedOutput()
	if err == nil || !strings.Contains(string(output), want) {
		t.Fatalf("negative control was not detected (want %q): %v\n%s", want, err, output)
	}
}

// The defect: an Update wrote the draft whatever the settings were now.
func TestEditorApplyRejectsWritingWhatever(t *testing.T) {
	requireEditorApplyFailure(t, "  if (!currentKnown) return Step::Reseed;",
		"  (void)opened; (void)current; (void)currentKnown; (void)equal;\n  return Step::Write;",
		"changed since the editor opened: nothing is written, the editor re-seeds")
}

// A fallback reading of a detached device is not the policy in force.
func TestEditorApplyRejectsAnUnknownReading(t *testing.T) {
	requireEditorApplyFailure(t, "  if (!currentKnown) return Step::Reseed;\n", "  (void)currentKnown;\n",
		"none from a detached device is its fallback: nothing is written over it")
}

// The default the editor opened on is not a real policy set since.
func TestEditorApplyRejectsADefaultOverARealPolicy(t *testing.T) {
	requireEditorApplyFailure(t, "  if (opened.has_value() != current.has_value()) return Step::Reseed;\n", "",
		"opened on the default over a real policy: nothing is written, the editor re-seeds")
}

// The DNS editor opened on blanks over no reading.
func TestEditorApplyRejectsADnsEditorOverNoReading(t *testing.T) {
	requireEditorApplyFailure(t, "  return reading.has_value();", "  (void)reading;\n  return true;",
		"no DNS reading takes no Update")
}

// The writers decide under the lock, on the write worker, before they write,
// and the editors carry the reading they opened on.
func TestEditorApplyWiring(t *testing.T) {
	host := stripComments(readAppSource(t, "SdkHost.cpp"))
	dns := functionBody(host, "void SdkHost::ApplyDnsSettings(")
	quitRequireSequence(t, "SdkHost::ApplyDnsSettings", dns,
		`PostWrite\(`, `std::scoped_lock lock\(mutex_\);`, `CurrentDnsSettings\(\);`,
		`editorapply::Decide\(opened, current, editorapply::DnsReadingKnown\(current\)`,
		`if \(write\) \{`, `device_->setDnsResolverSettings\(settings\);`, `onDnsSettings_\(CurrentDnsSettings\(\)\)`)
	transport := functionBody(host, "void SdkHost::ApplyTransportSettings(")
	quitRequireSequence(t, "SdkHost::ApplyTransportSettings", transport,
		`PostWrite\(`, `std::scoped_lock lock\(mutex_\);`,
		`known = current\.has_value\(\) \|\| device_->getRemoteConnected\(\);`,
		`editorapply::Decide\(opened, current, known,`, `if \(!write\) \{`, `return;`,
		`device_->setProviderTransportSettings\(settings\);`, `localState_->setProviderTransportSettings\(settings\);`)

	sheets := stripComments(readAppSource(t, "StatsSheets.cpp"))
	for _, want := range []string{
		"sheet->opened_ = current;\n  sheet->draft_ = FromSettings(current);",
		"self->sdk_.ApplyDnsSettings(self->opened_, ToSettings(self->draft_));",
		"dialog_.IsPrimaryButtonEnabled(editorapply::DnsReadingKnown(opened_) && !(draft_ == original_));",
		`if (!editorapply::DnsReadingKnown(opened_)) {`,
		`Loc("dns_settings_unavailable")`,
		"sheet->opened_ = current;\n  sheet->original_ = current ? current : sheet->defaults_;",
		"self->sdk_.ApplyTransportSettings(self->kind_, self->opened_, *self->draft_);",
	} {
		if !strings.Contains(sheets, want) {
			t.Errorf("StatsSheets.cpp is missing %q", want)
		}
	}
}
