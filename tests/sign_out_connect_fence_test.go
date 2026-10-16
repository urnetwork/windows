// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// The sign-out connect fence (App/SignOutConnectFence.h, owner report
// 2026-10-16) as a C++ spec with negative controls, and the SdkHost lines that
// ask it, read with every comment blanked.

func signOutConnectFenceProgram(t *testing.T, mutate func(string) string) string {
	t.Helper()
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("sign-out connect fence tests require a C++20 compiler: ", err)
	}
	root := repositoryRoot(t)
	includeDir := t.TempDir()
	source, err := os.ReadFile(filepath.Join(root, "app", "src", "App", "SignOutConnectFence.h"))
	if err != nil {
		t.Fatal(err)
	}
	content := string(source)
	if mutate != nil {
		content = mutate(content)
		if content == string(source) {
			t.Fatal("negative control did not change the production SignOutConnectFence.h")
		}
	}
	if err := os.WriteFile(filepath.Join(includeDir, "SignOutConnectFence.h"), []byte(content), 0600); err != nil {
		t.Fatal(err)
	}
	program := filepath.Join(includeDir, "sign-out-connect-fence-tests")
	build := exec.Command(compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I"+includeDir,
		filepath.Join(root, "app", "tools", "sign-out-connect-fence-tests.cpp"), "-o", program)
	if output, err := build.CombinedOutput(); err != nil {
		t.Fatalf("build sign-out connect fence tests: %v\n%s", err, output)
	}
	return program
}

func TestSignOutConnectFence(t *testing.T) {
	if !strings.Contains(readAppSource(t, "App.vcxproj"), `<ClInclude Include="SignOutConnectFence.h" />`) {
		t.Error("App.vcxproj does not list SignOutConnectFence.h")
	}
	if output, err := exec.Command(signOutConnectFenceProgram(t, nil)).CombinedOutput(); err != nil {
		t.Fatalf("sign-out connect fence: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}
}

func requireSignOutConnectFenceFailure(t *testing.T, replacement, want string) {
	t.Helper()
	const rule = "return signedIn || work != Work::Connect;"
	program := signOutConnectFenceProgram(t, func(source string) string {
		if strings.Count(source, rule) != 1 {
			t.Fatalf("SignOutConnectFence.h no longer has exactly one %q; update this control", rule)
		}
		return strings.Replace(source, rule, replacement, 1)
	})
	output, err := exec.Command(program).CombinedOutput()
	if err == nil || !strings.Contains(string(output), want) {
		t.Fatalf("negative control was not detected (want %q): %v\n%s", want, err, output)
	}
}

// The defect: nothing fenced a connect once a sign-out began.
func TestSignOutConnectFenceRejectsAdmittingEverything(t *testing.T) {
	requireSignOutConnectFenceFailure(t, "(void)work; (void)signedIn; return true;",
		"a connect is refused once the sign-out has started")
}

// The fence must not swallow the account's disconnect.
func TestSignOutConnectFenceRejectsRefusingTheDisconnect(t *testing.T) {
	requireSignOutConnectFenceFailure(t, "(void)work; return signedIn;",
		"a disconnect of the signed-out account still runs")
}

// RequestSession refuses a connect before it reaches the slot, and a pass that
// took one before the sign-out began skips its connect; Logout clears the flag
// before it waits for that pass.
func TestSignOutConnectFenceWiring(t *testing.T) {
	host := stripComments(readAppSource(t, "SdkHost.cpp"))
	request := functionBody(host, "void SdkHost::RequestSession(")
	quitRequireSequence(t, "SdkHost::RequestSession", request,
		`signoutfence::Admits\(FenceWorkOf\(request\.kind\),\s*loggedIn_\.load\(`,
		`pending_ = std::move\(request\)`)
	worker := functionBody(host, "void SdkHost::SessionWorkerLoop(")
	quitRequireSequence(t, "SdkHost::SessionWorkerLoop", worker,
		`plan\.sdkConnect && req\.kind != ConnectKind::None &&\s*signoutfence::Admits\(FenceWorkOf\(req\.kind\),\s*loggedIn_\.load\(`,
		`ConnectLocked\(req\)`)
	logout := functionBody(host, "void SdkHost::Logout(")
	quitRequireSequence(t, "SdkHost::Logout", logout,
		`loggedIn_\.store\(false`, `std::scoped_lock lock\(mutex_\)`)
	header := stripComments(readAppSource(t, "SdkHost.h"))
	for _, row := range []string{
		`case ConnectKind::BestAvailable: return signoutfence::Work::Connect;`,
		`case ConnectKind::Location:      return signoutfence::Work::Connect;`,
		`case ConnectKind::Disconnect:    return signoutfence::Work::Disconnect;`,
	} {
		if !strings.Contains(header, row) {
			t.Errorf("SdkHost::FenceWorkOf is missing %q", row)
		}
	}
}
