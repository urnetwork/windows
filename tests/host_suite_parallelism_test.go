// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os/exec"
	"path/filepath"
	"testing"
)

func checkHostSuiteCompilerBudget(t *testing.T, control string) {
	t.Helper()
	script, err := filepath.Abs("host_suite_parallelism.test.sh")
	if err != nil {
		t.Fatal(err)
	}
	suite, err := filepath.Abs(filepath.Join("..", "test.sh"))
	if err != nil {
		t.Fatal(err)
	}
	output, err := exec.Command("bash", script, control, suite, t.TempDir()).CombinedOutput()
	if err != nil {
		t.Fatalf("host suite compiler budget: %v\n%s", err, output)
	}
	t.Logf("%s", output)
}

func TestWindowsHostSuiteDeclaresCompilerParallelism(t *testing.T) {
	checkHostSuiteCompilerBudget(t, "inherited-default")
}

func TestWindowsHostSuitePreservesCallerBudgetOverrides(t *testing.T) {
	checkHostSuiteCompilerBudget(t, "caller-overrides")
}

func TestWindowsHostSuiteStopsAfterCommandFailure(t *testing.T) {
	checkHostSuiteCompilerBudget(t, "first-command-failure")
}
