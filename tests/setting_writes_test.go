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

// The user's setting writes off the UI thread (owner report 2026-10-16): the
// ordered write worker (App/OrderedWorker.h) as a C++ spec with negative
// controls that rewrite a copy of the header, and the SdkHost and caller lines
// that use it, read with every comment blanked.

func orderedWorkerProgram(t *testing.T, mutate func(string) string) string {
	t.Helper()
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("ordered worker tests require a C++20 compiler: ", err)
	}
	root := repositoryRoot(t)
	includeDir := t.TempDir()
	source, err := os.ReadFile(filepath.Join(root, "app", "src", "App", "OrderedWorker.h"))
	if err != nil {
		t.Fatal(err)
	}
	content := string(source)
	if mutate != nil {
		content = mutate(content)
		if content == string(source) {
			t.Fatal("negative control did not change the production OrderedWorker.h")
		}
	}
	if err := os.WriteFile(filepath.Join(includeDir, "OrderedWorker.h"), []byte(content), 0600); err != nil {
		t.Fatal(err)
	}
	program := filepath.Join(includeDir, "ordered-worker-tests")
	build := exec.Command(compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I"+includeDir,
		filepath.Join(root, "app", "tools", "ordered-worker-tests.cpp"), "-o", program)
	if output, err := build.CombinedOutput(); err != nil {
		t.Fatalf("build ordered worker tests: %v\n%s", err, output)
	}
	return program
}

func TestOrderedWorker(t *testing.T) {
	if !strings.Contains(readAppSource(t, "App.vcxproj"), `<ClInclude Include="OrderedWorker.h" />`) {
		t.Error("App.vcxproj does not list OrderedWorker.h")
	}
	if output, err := exec.Command(orderedWorkerProgram(t, nil)).CombinedOutput(); err != nil {
		t.Fatalf("ordered worker: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}
}

func requireOrderedWorkerFailure(t *testing.T, old, replacement, want string) {
	t.Helper()
	program := orderedWorkerProgram(t, func(source string) string {
		if strings.Count(source, old) != 1 {
			t.Fatalf("OrderedWorker.h no longer has exactly one %q; update this control", old)
		}
		return strings.Replace(source, old, replacement, 1)
	})
	output, err := exec.Command(program).CombinedOutput()
	if err == nil || !strings.Contains(string(output), want) {
		t.Fatalf("negative control was not detected (want %q): %v\n%s", want, err, output)
	}
}

// The defect: a write made on the caller's thread, the UI thread, waits there
// for mutex_ and the device's rpc.
func TestOrderedWorkerRejectsRunningOnTheCallersThread(t *testing.T) {
	requireOrderedWorkerFailure(t,
		"    std::scoped_lock lock(stateLock_);\n    if (stopped_) return false;\n    jobs_.push_back(std::move(job));",
		"    if (job) job();\n    return true;\n    std::scoped_lock lock(stateLock_);\n    if (stopped_) return false;\n    jobs_.push_back(std::move(job));",
		"jobs run off the caller's thread")
}

// Folding writes into the last one would drop a setting's earlier edits and
// reorder two settings' writes.
func TestOrderedWorkerRejectsCoalescing(t *testing.T) {
	requireOrderedWorkerFailure(t, "    jobs_.push_back(std::move(job));",
		"    jobs_.clear();\n    jobs_.push_back(std::move(job));",
		"every job posted runs, in order, none folded into another")
}

// A stop that drops the queued writes loses the user's last edit at a quit.
func TestOrderedWorkerRejectsAStopThatDropsTheQueue(t *testing.T) {
	requireOrderedWorkerFailure(t, "      stopped_ = true;\n    }",
		"      stopped_ = true;\n      jobs_.clear();\n    }",
		"a job queued when the stop came still runs")
}

// A throw that ends the loop leaves every later write queued forever.
func TestOrderedWorkerRejectsAThrowThatEndsTheLoop(t *testing.T) {
	requireOrderedWorkerFailure(t, "      } catch (...) {\n      }",
		"      } catch (...) {\n        return;\n      }",
		"a throwing job does not stop the jobs behind it")
}

// Every drawer mutation hands its write to the worker before it touches
// mutex_ or the device, and the worker stops above the lock with the other
// threads, in the destructor and in the quit.
func TestSettingWritesRunOffTheUiThread(t *testing.T) {
	host := stripComments(readAppSource(t, "SdkHost.cpp"))
	posted := regexp.MustCompile(`^\{\s*PostWrite\(`)
	for _, signature := range []string{
		"void SdkHost::SetPerformanceSettings(",
		"void SdkHost::SetBlockerEnabled(",
		"void SdkHost::SetKillSwitch(",
		"void SdkHost::SetProvideControlMode(",
		"void SdkHost::SetProvideExtender(",
		"void SdkHost::SetMeasurementsEnabled(",
		"void SdkHost::ApplyDnsSettings(",
		"void SdkHost::ApplyTransportSettings(",
		"void SdkHost::UpdateSplitRule(",
		"void SdkHost::RemoveSplitRule(",
		"void SdkHost::SetAppRule(",
		"void SdkHost::RemoveAppRule(",
	} {
		body := functionBody(host, signature)
		if body == "" {
			t.Errorf("%s is missing", signature)
			continue
		}
		if !posted.MatchString(body) {
			t.Errorf("%s does not hand its write to the write worker first", signature)
		}
	}
	for _, signature := range []string{
		"std::string SdkHost::CreateSplitRule(",
		"std::string SdkHost::CreateTunnelRule(",
		"std::string SdkHost::CreateBlockRule(",
	} {
		body := functionBody(host, signature)
		if !strings.Contains(body, "return PostRuleCreate(") || strings.Contains(body, "mutex_") ||
			strings.Contains(body, "device_") {
			t.Errorf("%s must mint its id and hand the write to the worker, reading nothing locked", signature)
		}
	}
	create := functionBody(host, "std::string SdkHost::PostRuleCreate(")
	quitRequireSequence(t, "PostRuleCreate", create,
		`if \(!HasSession\(\) \|\| hosts\.empty\(\)\) return \{\};`, `overrideId = urnet::newId\(\);`,
		`PostWrite\(what,`, `std::scoped_lock lock\(mutex_\);`, `device_->addBlockActionOverride\(over\);`,
		`PublishSplitRules\(\);`, `return overrideId;`)
	if at := strings.Index(create, "PostWrite(what,"); at >= 0 &&
		(strings.Contains(create[:at], "mutex_") || strings.Contains(create[:at], "device_")) {
		t.Error("PostRuleCreate reads mutex_ or the device on the caller's thread")
	}
	kill := functionBody(host, "void SdkHost::SetKillSwitch(")
	quitRequireSequence(t, "SdkHost::SetKillSwitch", kill,
		`localState_->setRouteLocal\(!on\)`, `device_->setRouteLocal\(!on\)`, `service_\.SetKillSwitch\(on\)`,
		`actual = CurrentKillSwitch\(\);`, `if \(done\) done\(applied, actual\);`)
	post := functionBody(host, "void SdkHost::PostWrite(")
	quitRequireSequence(t, "SdkHost::PostWrite", post, `writer_\.Post\(std::move\(job\)\)`)

	destructor := definitionBody(t, "SdkHost.cpp", host, "SdkHost::~SdkHost()")
	quitRequireSequence(t, "SdkHost's destructor", destructor, `writer_\.Stop\(\);`, `std::scoped_lock lock\(mutex_\);`)
	quit := functionBody(host, "void SdkHost::QuitInOrder(")
	quitRequireSequence(t, "SdkHost::QuitInOrder", quit,
		`statsPublisher_\.Stop\(\);`, `writer_\.Stop\(\);`, `std::scoped_lock lock\(mutex_\);`)
}

// The settings page shows the kill switch the write left, as the write's
// answer, marshalled back to its thread; it reads nothing on the UI thread.
func TestKillSwitchToggleReadsBackOffTheUiThread(t *testing.T) {
	page := stripComments(readAppSource(t, "SettingsPage.cpp"))
	toggled := functionBody(page, "void SettingsPage::OnKillSwitchToggled(")
	quitRequireSequence(t, "SettingsPage::OnKillSwitchToggled", toggled,
		`Sdk\(\)\.SetKillSwitch\(wanted, \[`, `queue\.TryEnqueue\(`, `if \(applied && actual == wanted\) return;`,
		`page\.killSwitch_\.IsOn\(actual\);`)
	quitForbid(t, "SettingsPage::OnKillSwitchToggled", toggled,
		"the read-back is the write's answer, taken on the write worker", "CurrentKillSwitch(")
}
