// SPDX-License-Identifier: MPL-2.0

// The real host launcher must give the complete C++ census its declared package
// budget without changing its phases, caller overrides or fail-fast behavior.
package tests

import (
	"bytes"
	"context"
	"errors"
	"os"
	"os/exec"
	"path/filepath"
	"reflect"
	"runtime"
	"strings"
	"testing"
	"time"
)

// Records value-only command arguments; the first test phase delegates to real Go.
type windowsHostBudgetCall struct {
	directory string
	arguments []string
}

// Every file and executable below belongs to this fixture; no shared gate,
// compiler, SDK build, license catalog, network or external module is used.
type windowsHostBudgetFixture struct {
	directory   string
	tracePath   string
	environment []string
}

// Reads the actual adjacent launcher, including private before/after copies.
func newWindowsHostBudgetFixture(t *testing.T, budget string) *windowsHostBudgetFixture {
	t.Helper()
	_, filename, _, ok := runtime.Caller(0)
	if !ok {
		t.Fatal("cannot locate the launcher regression source")
	}
	source, err := os.ReadFile(filepath.Join(filepath.Dir(filename), "..", "test.sh"))
	if err != nil {
		t.Fatal(err)
	}
	realGo, err := exec.LookPath("go")
	if err != nil {
		t.Fatal(err)
	}
	self := &windowsHostBudgetFixture{directory: filepath.Join(t.TempDir(), "host fixture with spaces")}
	self.tracePath = filepath.Join(self.directory, "commands")
	for _, directory := range []string{"windows/tests", "windows/tests/insufficientbalance", "build/all/windows", "sdk", "tests", "bin", "markers"} {
		if err := os.MkdirAll(filepath.Join(self.directory, directory), 0700); err != nil {
			t.Fatal(err)
		}
	}
	self.write(t, "windows/test.sh", string(source), 0700)
	self.write(t, "windows/go.mod", "module windows-host.example/fixture\n\ngo 1.23.0\n", 0600)
	self.write(t, "tests/network-intensive-suite-lock.sh", `#!/bin/sh
[ "$#" -eq 2 ] && [ "$1" = --verify-held ] && [ "$2" = run-all ] && exit 0
exit 71
`, 0700)
	shim := `#!/bin/sh
printf '%s\000' "$PWD" "$@" "" >>"$WINDOWS_HOST_BUDGET_TRACE"
last=
for argument do last=$argument; done
if [ "$1" = test ] && [ "$PWD" = "$WINDOWS_HOST_BUDGET_ROOT/windows" ] && [ "$last" = ./tests ]; then
  [ "$WINDOWS_HOST_BUDGET_FAIL_FIRST" != 1 ] || exit 23
  exec "$WINDOWS_HOST_BUDGET_REAL_GO" "$@"
fi
exit 0
`
	// MSYS bash spells $PWD /tmp/...; pwd -W gives the path Go knows.
	if runtime.GOOS == "windows" {
		shim = strings.ReplaceAll(shim, `"$PWD"`, `"$(pwd -W)"`)
	}
	self.write(t, "bin/go", shim, 0700)
	self.write(t, "windows/tests/budget_test.go", `package fixture

import (
	"flag"
	"os"
	"path/filepath"
	"testing"
	"time"
)

func TestActualPackageBudget(t *testing.T) {
	want, err := time.ParseDuration(os.Getenv("WINDOWS_HOST_BUDGET_EXPECTED"))
	if err != nil { t.Fatal(err) }
	got, err := time.ParseDuration(flag.Lookup("test.timeout").Value.String())
	if err != nil || got != want {
		t.Fatalf("actual Go package budget=%v, want=%v, err=%v", got, want, err)
	}
	if _, ok := t.Deadline(); !ok {
		t.Fatal("the explicit finite package alarm was not installed")
	}
	if err := os.WriteFile(filepath.Join(os.Getenv("WINDOWS_HOST_BUDGET_ROOT"), "markers", "budget"), nil, 0600); err != nil { t.Fatal(err) }
}

func TestFullSiblingCensus(t *testing.T) {
	if err := os.WriteFile(filepath.Join(os.Getenv("WINDOWS_HOST_BUDGET_ROOT"), "markers", "sibling"), nil, 0600); err != nil { t.Fatal(err) }
}
`, 0600)
	replaced := map[string]bool{
		"PATH": true, "URNETWORK_ROOT": true, "URNETWORK_NETWORK_TEST_LOCK_HELD": true,
		"URNETWORK_WINDOWS_HOST_OFFLINE_ADMISSION": true,
		"GOTOOLCHAIN": true, "GOPROXY": true, "GOSUMDB": true, "GOWORK": true,
		"WINDOWS_HOST_BUDGET_ROOT": true, "WINDOWS_HOST_BUDGET_TRACE": true,
		"WINDOWS_HOST_BUDGET_REAL_GO": true, "WINDOWS_HOST_BUDGET_EXPECTED": true,
		"WINDOWS_HOST_BUDGET_FAIL_FIRST": true,
	}
	for _, entry := range os.Environ() {
		name, _, _ := strings.Cut(entry, "=")
		if !replaced[name] {
			self.environment = append(self.environment, entry)
		}
	}
	self.environment = append(self.environment,
		"PATH="+filepath.Join(self.directory, "bin")+string(os.PathListSeparator)+os.Getenv("PATH"),
		"URNETWORK_ROOT="+filepath.ToSlash(self.directory), "URNETWORK_NETWORK_TEST_LOCK_HELD=1",
		"GOTOOLCHAIN=local", "GOPROXY=off", "GOSUMDB=off", "GOWORK=off",
		"WINDOWS_HOST_BUDGET_ROOT="+filepath.ToSlash(self.directory), "WINDOWS_HOST_BUDGET_TRACE="+self.tracePath,
		"WINDOWS_HOST_BUDGET_REAL_GO="+realGo, "WINDOWS_HOST_BUDGET_EXPECTED="+budget,
	)
	return self
}

// Only fixture-owned paths are created; names above are fixed synthetic assets.
func (self *windowsHostBudgetFixture) write(t *testing.T, name, source string, mode os.FileMode) {
	t.Helper()
	if err := os.WriteFile(filepath.Join(self.directory, name), []byte(source), mode); err != nil {
		t.Fatal(err)
	}
}

// A short outer guard protects the fixture, not the package-alarm assertion.
func (self *windowsHostBudgetFixture) run(t *testing.T, arguments ...string) ([]byte, []windowsHostBudgetCall, error) {
	t.Helper()
	ctx, cancel := context.WithTimeout(t.Context(), 45*time.Second)
	defer cancel()
	command := exec.CommandContext(ctx, "bash", append([]string{filepath.Join(self.directory, "windows", "test.sh")}, arguments...)...)
	command.Env = self.environment
	command.WaitDelay = time.Second
	output, commandErr := command.CombinedOutput()
	commandErr = errors.Join(commandErr, ctx.Err())
	trace, err := os.ReadFile(self.tracePath)
	if err != nil {
		t.Fatal(err)
	}
	var calls []windowsHostBudgetCall
	var fields []string
	for _, field := range bytes.Split(trace, []byte{0}) {
		if len(field) != 0 {
			fields = append(fields, string(field))
		} else if len(fields) != 0 {
			call := windowsHostBudgetCall{directory: filepath.Clean(fields[0]), arguments: append([]string(nil), fields[1:]...)}
			// the shell's spelling of the one path argument; Clean changes nothing on unix
			if len(call.arguments) > 1 && call.arguments[0] == "-C" {
				call.arguments[1] = filepath.Clean(call.arguments[1])
			}
			calls = append(calls, call)
			fields = nil
		}
	}
	return output, calls, commandErr
}

// All original phases and their exact selectors remain, including license drift.
func (self *windowsHostBudgetFixture) requireCensus(t *testing.T, calls []windowsHostBudgetCall, arguments ...string) {
	t.Helper()
	first := []string{"test", "-timeout", "30m", "-parallel", "2"}
	explicitBudget := false
	for _, argument := range arguments {
		if argument == "-timeout" || strings.HasPrefix(argument, "-timeout=") {
			explicitBudget = true
		}
	}
	if explicitBudget {
		first = []string{"test", "-parallel", "2"}
		// Only the explicit override control accepts an optional default prefix.
		// Copy the observation before normalization; retain all other exact args.
		if len(calls) > 0 && len(calls[0].arguments) >= 3 &&
			calls[0].arguments[0] == "test" && calls[0].arguments[1] == "-timeout" && calls[0].arguments[2] == "30m" {
			calls = append([]windowsHostBudgetCall(nil), calls...)
			calls[0].arguments = append([]string{"test"}, calls[0].arguments[3:]...)
		}
	}
	first = append(first, arguments...)
	first = append(first, "./tests")
	second := append([]string{"test"}, arguments...)
	second = append(second, "./tests/insufficientbalance")
	third := append([]string{"test"}, arguments...)
	third = append(third, "./...")
	want := []windowsHostBudgetCall{
		{directory: filepath.Join(self.directory, "windows"), arguments: first},
		{directory: filepath.Join(self.directory, "windows"), arguments: second},
		{directory: filepath.Join(self.directory, "build", "all", "windows"), arguments: third},
		{directory: filepath.Join(self.directory, "windows"), arguments: []string{"-C", filepath.Join(self.directory, "sdk"), "run", "./licenses", "-check", "windows"}},
	}
	if !reflect.DeepEqual(calls, want) {
		t.Fatalf("host launcher changed its complete command census: got=%+v want=%+v", calls, want)
	}
	for _, marker := range []string{"budget", "sibling"} {
		if _, err := os.Stat(filepath.Join(self.directory, "markers", marker)); err != nil {
			t.Fatalf("actual Go fixture omitted %s: %v", marker, err)
		}
	}
}

// The prior launcher implicitly installs 10m; this assertion fails immediately,
// without waiting for a slow child or inferring elapsed-time failure.
func TestWindowsHostScriptDeclaresAggregatePackageBudget(t *testing.T) {
	fixture := newWindowsHostBudgetFixture(t, "30m")
	output, calls, err := fixture.run(t)
	if err != nil {
		t.Fatalf("declared complete-package budget: %v\n%s", err, output)
	}
	fixture.requireCensus(t, calls)
}

// A focused caller retains its deliberately narrower deadline in all Go phases.
func TestWindowsHostScriptPreservesExplicitPackageBudget(t *testing.T) {
	for _, c := range []struct {
		budget    string
		arguments []string
	}{
		{budget: "17s", arguments: []string{"-timeout=17s", "-count=1"}},
		{budget: "19s", arguments: []string{"-timeout", "19s", "-count=1"}},
	} {
		fixture := newWindowsHostBudgetFixture(t, c.budget)
		output, calls, err := fixture.run(t, c.arguments...)
		if err != nil {
			t.Fatalf("explicit caller package budget %q: %v\n%s", c.budget, err, output)
		}
		fixture.requireCensus(t, calls, c.arguments...)
	}
}

// An actual failed first phase still stops the script before unrelated work.
func TestWindowsHostScriptPreservesFirstFailure(t *testing.T) {
	fixture := newWindowsHostBudgetFixture(t, "30m")
	fixture.environment = append(fixture.environment, "WINDOWS_HOST_BUDGET_FAIL_FIRST=1")
	_, calls, err := fixture.run(t)
	var exited *exec.ExitError
	if !errors.As(err, &exited) || exited.ExitCode() != 23 || len(calls) != 1 {
		t.Fatalf("launcher hid or continued beyond first failure: status=%v calls=%+v", err, calls)
	}
}

// The real shell preserves both Go modes and one space-containing selector,
// with exact argument order and the original working directory in every phase.
func TestWindowsHostScriptPreservesCallerModesArgumentsAndDirectories(t *testing.T) {
	selector := "^(TestActualPackageBudget|TestFullSiblingCensus)$|synthetic selector with spaces"
	for _, arguments := range [][]string{
		{"-count=1", "-run", selector},
		{"-race", "-count=1", "-run", selector},
	} {
		fixture := newWindowsHostBudgetFixture(t, "30m")
		output, calls, err := fixture.run(t, arguments...)
		if err != nil {
			t.Fatalf("caller mode and quoted arguments %q: %v\n%s", arguments, err, output)
		}
		fixture.requireCensus(t, calls, arguments...)
	}
}
