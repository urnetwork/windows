// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// The dead-tunnel failsafe on an account out of balance (Service/DeadTunnelVerdict.h)
// as a C++ spec, with negative controls that rewrite a copy of the header, and the
// watchdog's two threads, which need Windows and the sdk, read with every comment
// blanked so prose cannot satisfy a contract.

// The headers the spec compiles against, copied so a control can rewrite one.
var deadTunnelVerdictHeaders = []string{"DeadTunnelVerdict.h", "TunnelWatchdogSettings.h", "NetworkChangeNotify.h", "CaptureReadiness.h"}

// Compile the spec (app/tools/dead-tunnel-verdict-tests.cpp). `mutate`, when set,
// rewrites the copy of DeadTunnelVerdict.h.
func deadTunnelVerdictProgram(t *testing.T, mutate func(string) string) string {
	t.Helper()
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("dead tunnel verdict tests require a C++20 compiler: ", err)
	}
	root := repositoryRoot(t)
	includeDir := t.TempDir()
	for _, header := range deadTunnelVerdictHeaders {
		source, err := os.ReadFile(filepath.Join(root, "app", "src", "Service", header))
		if err != nil {
			t.Fatal(err)
		}
		content := string(source)
		if mutate != nil && header == "DeadTunnelVerdict.h" {
			content = mutate(content)
			if content == string(source) {
				t.Fatal("negative control did not change the production DeadTunnelVerdict.h")
			}
		}
		if err := os.WriteFile(filepath.Join(includeDir, header), []byte(content), 0600); err != nil {
			t.Fatal(err)
		}
	}
	program := filepath.Join(includeDir, "dead-tunnel-verdict-tests")
	build := exec.Command(compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-pthread",
		"-I"+includeDir,
		"-I"+filepath.Join(root, "app", "src", "Common"),
		"-I"+filepath.Join(root, "app", "src", "App"),
		filepath.Join(root, "app", "tools", "dead-tunnel-verdict-tests.cpp"), "-o", program)
	if output, err := build.CombinedOutput(); err != nil {
		t.Fatalf("build dead tunnel verdict tests: %v\n%s", err, output)
	}
	return program
}

// The spec: an out-of-balance session with no exit past every window is not
// stopped; the same session in balance is; the balance returning starts every
// window again; a stale or missing signal keeps the hold; a balance never reported
// is judged by the rules; a wedged sdk still stops the session; and the notice, the
// banner and the Disconnect stay for the whole hold.
func TestDeadTunnelVerdict(t *testing.T) {
	project := readServiceSource(t, "Service.vcxproj")
	if !strings.Contains(project, `<ClInclude Include="DeadTunnelVerdict.h" />`) {
		t.Error("Service.vcxproj does not list DeadTunnelVerdict.h")
	}
	if output, err := exec.Command(deadTunnelVerdictProgram(t, nil)).CombinedOutput(); err != nil {
		t.Fatalf("dead tunnel verdict: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}
}

// Rewrite the copy and require the spec to fail, naming want.
func requireDeadTunnelVerdictFailure(t *testing.T, old, replacement, want string) {
	t.Helper()
	program := deadTunnelVerdictProgram(t, func(source string) string {
		return strings.ReplaceAll(source, old, replacement)
	})
	output, err := exec.Command(program).CombinedOutput()
	if err == nil {
		t.Fatalf("the spec passed with %q rewritten\n%s", old, output)
	}
	if !strings.Contains(string(output), want) {
		t.Fatalf("the spec did not name %q:\n%s", want, output)
	}
}

// The defect: nothing tells the verdict the session is out of balance, which is
// what the verdict was before the hold.
func TestDeadTunnelVerdictNegativeNoHold(t *testing.T) {
	requireDeadTunnelVerdictFailure(t,
		"if (!reading.sampled) return BalanceStep::None;",
		"return BalanceStep::None;",
		"a session that starts out of balance is not stopped past NoExit's window")
}

// The other way to get it wrong: a hold for everybody takes the failsafe away from
// the dead tunnels it exists for.
func TestDeadTunnelVerdictNegativeHoldsEverybody(t *testing.T) {
	requireDeadTunnelVerdictFailure(t,
		"constexpr bool held() const { return held_; }",
		"constexpr bool held() const { return true; }",
		"the same session in balance stops with NoExit, at its window")
}

// A hold that believes a cleared status whatever cleared it: a reset stops the
// account it was holding.
func TestDeadTunnelVerdictNegativeReleasesOnAReset(t *testing.T) {
	requireDeadTunnelVerdictFailure(t,
		"const bool granted = reading.connectionGeneration <= refusedGeneration_ ||\n                         reading.providerAddedCount > 0;",
		"const bool granted = true;",
		"a reset by a new destination, with nothing admitted, keeps the hold")
}

// A hold that believes the window alone: providers from before the refusal.
func TestDeadTunnelVerdictNegativeReleasesOnStaleProviders(t *testing.T) {
	requireDeadTunnelVerdictFailure(t,
		"    if (reading.insufficientBalance) {\n      refusedGeneration_",
		"    if (reading.insufficientBalance && reading.providerAddedCount == 0) {\n      refusedGeneration_",
		"providers admitted before the refusal, still counted Added, keep the hold")
}

// A no-permission result pushing the refusal out reads as a grant.
func TestDeadTunnelVerdictNegativeReleasesOnNoPermission(t *testing.T) {
	requireDeadTunnelVerdictFailure(t,
		"if (!held_ || reading.noPermission) return BalanceStep::None;",
		"if (!held_) return BalanceStep::None;",
		"no-permission results pushing the refusal out of the status keep the hold")
}

// An unreadable status reads as the balance being back.
func TestDeadTunnelVerdictNegativeUnreadableStatusReleases(t *testing.T) {
	requireDeadTunnelVerdictFailure(t,
		"if (!reading.sampled) return BalanceStep::None;",
		"if (!reading.sampled) { const bool was = held_; held_ = false; return was ? BalanceStep::Released : BalanceStep::None; }",
		"a status that can no longer be read keeps the hold")
}

// The hold taking a hung process with it.
func TestDeadTunnelVerdictNegativeHoldsAHungSdk(t *testing.T) {
	requireDeadTunnelVerdictFailure(t,
		"if (sampleAge >= settings.sdkUnresponsiveMillis) {",
		"if (!s.outOfBalance && sampleAge >= settings.sdkUnresponsiveMillis) {",
		"a wedged sdk stops the session, out of balance")
}

// A countdown left running for a rule that will not fire.
func TestDeadTunnelVerdictNegativeCountsDownForAHeldRule(t *testing.T) {
	requireDeadTunnelVerdictFailure(t,
		"if (slowApplies) consider(settings.deadSlowMillis - provenAge);",
		"if (s.provenCount == 0 && s.lastSampleMillis >= 0) consider(settings.deadSlowMillis - provenAge);",
		"no warning that the tunnel will be turned off")
}

// The two threads the spec cannot run. The sampler reads the balance off the
// session's device, on its own thread like every other call on it, ahead of the
// window status it is paired with, and publishes it whole under the channel's lock
// before the stamp that means a sample completed. The evaluator reads it there, and
// observes it ahead of the freeze rebase. On a release it rebases and reaches no
// verdict. Nothing else writes the reading: the app's balance fetch never reaches
// the service.
func TestDeadTunnelWatchdogReadsTheBalanceOffItsDevice(t *testing.T) {
	source := stripComments(readServiceSource(t, "TunnelWatchdog.cpp"))
	sampler := definitionBody(t, "TunnelWatchdog.cpp", source,
		"void TunnelWatchdog::RunSampler(std::shared_ptr<WatchdogChannel> channel)")
	order := []string{
		"device->getContractStatus();",
		"const auto balanceWindow = readWindow();",
		"if (contractStatus && balanceWindow) {",
		"std::scoped_lock lock(channel->mutex);",
		"channel->balance = BalanceReading{",
		".insufficientBalance = contractStatus->InsufficientBalance,",
		".noPermission = contractStatus->NoPermission,",
		".connectionGeneration = balanceWindow->ConnectionGeneration,",
		".providerAddedCount = balanceWindow->ProviderStateAdded,",
		"channel->lastSampleMillis.store(now);",
	}
	for i := 0; i+1 < len(order); i++ {
		provideRequireOrder(t, "RunSampler", sampler, order[i], order[i+1])
	}

	evaluator := definitionBody(t, "TunnelWatchdog.cpp", source, "void TunnelWatchdog::RunEvaluator(")
	order = []string{
		"balance = channel->balance;",
		"const BalanceStep balanceStep = outOfBalance.Observe(balance);",
		"if (freeze.rebase) {",
		"if (balanceStep == BalanceStep::Released) {",
		"signals.outOfBalance = outOfBalance.held();",
		"const DeadTunnelVerdict verdict = Evaluate(signals, now, channel->settings.deadTunnel);",
	}
	for i := 0; i+1 < len(order); i++ {
		provideRequireOrder(t, "RunEvaluator", evaluator, order[i], order[i+1])
	}
	released := evaluator[strings.Index(evaluator, "if (balanceStep == BalanceStep::Released) {"):]
	released = released[:strings.Index(released, "continue;")]
	provideRequire(t, "the release", released,
		"sessionStartMillis = now;",
		"trafficStartMillis = now;",
		"traffic.Reset(",
		"armed_.store(false, std::memory_order_relaxed);",
		"connectionEpoch.Observe(connectionGeneration,")
	// kept for the life of the watch: one hold, never rebuilt in the loop
	if n := strings.Count(evaluator, "OutOfBalanceHold outOfBalance;"); n != 1 {
		t.Errorf("RunEvaluator declares the hold %d times", n)
	}
	loop := evaluator[strings.Index(evaluator, "for (;;) {"):]
	requireNone(t, "RunEvaluator's loop", loop, "OutOfBalanceHold outOfBalance;", "outOfBalance = OutOfBalanceHold")

	// the channel holds one copy, and the app's side has no way to write it
	header := stripComments(readServiceSource(t, "TunnelWatchdog.h"))
	provideRequire(t, "WatchdogChannel", header, "BalanceReading balance;")
	requireNone(t, "TunnelController.cpp", tunnelControllerSource(t), "BalanceReading", "getContractStatus")
}

// What the header no longer keeps stays one copy.
func TestDeadTunnelVerdictHasOneHome(t *testing.T) {
	header := stripComments(readServiceSource(t, "TunnelWatchdog.h"))
	provideRequire(t, "TunnelWatchdog.h", header, `#include "DeadTunnelVerdict.h"`)
	requireNone(t, "TunnelWatchdog.h", header,
		"inline constexpr DeadTunnelVerdict Evaluate(", "class TrafficTracker", "class ConnectionEpochTracker",
		"struct DeadTunnelSignals", "class OutOfBalanceHold")
	verdict := stripComments(readServiceSource(t, "DeadTunnelVerdict.h"))
	requireNone(t, "DeadTunnelVerdict.h", verdict, "windows.h", "Sdk.h", "urnet::")
}
