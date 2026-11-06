// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// Compile the production registration, logout and UI delivery bodies against
// controllable local fakes. Hooks force the precise lifecycle changes that a
// pure SignOutRun model missed: admission before logout, late error callbacks,
// an outstanding local-state delete, and a publication delayed behind logout.
func signOutRegistrationProgram(t *testing.T, mutate func(string) string) string {
	t.Helper()
	return signOutRegistrationProgramWithUi(t, mutate, nil)
}

func signOutRegistrationProgramWithUi(t *testing.T, mutate, mutateUi func(string) string) string {
	t.Helper()
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("registration/logout tests require a C++20 compiler: ", err)
	}
	root := repositoryRoot(t)
	dir := t.TempDir()
	host := stripComments(readAppSource(t, "SdkHost.cpp"))
	if mutate != nil {
		before := host
		host = mutate(host)
		if host == before {
			t.Fatal("negative control did not change the production SdkHost.cpp")
		}
	}
	// These helpers are now part of the real tail/registration dependency graph.
	// Keep the production wait declaration rather than copying its value.
	const waitPrefix = "constexpr std::chrono::milliseconds kServerSignOutWait{"
	if strings.Count(host, waitPrefix) != 1 {
		t.Fatal("SdkHost.cpp no longer has exactly one server sign-out wait declaration")
	}
	waitStart := strings.Index(host, waitPrefix)
	waitEnd := strings.Index(host[waitStart:], ";")
	if waitEnd < 0 {
		t.Fatal("SdkHost.cpp server sign-out wait declaration has no terminator")
	}
	bodies := []string{host[waitStart : waitStart+waitEnd+1]}
	for _, signature := range []string{
		"void SdkHost::RegisterNetworkClient(",
		"void SdkHost::ServerSignOutLocked(",
		"void SdkHost::RemoveAbandonedClientLocked(",
		"void SdkHost::Logout(",
		"void SdkHost::SignOutTail(",
		"void SdkHost::SignOutTailLocked(",
		"void SdkHost::SetAuthState(",
		"bool SdkHost::SetRegistrationAuthState(",
		"bool SdkHost::ContinueAuthAttempt(",
		"void SdkHost::LoginWithPassword(",
		"void SdkHost::LoginWithCode(",
		"void SdkHost::LoginWithSeedphrase(",
		"void SdkHost::VerifyCode(",
		"void SdkHost::StartLogin(",
		"void SdkHost::SubmitCreateNetwork(",
		"void SdkHost::CreateNetwork(",
		"void SdkHost::CreateInstantAccount(",
		"void SdkHost::ConfirmInstantAccount(",
		"void SdkHost::ConfirmApprovedNetwork(",
		"void SdkHost::RedeemApprovedCode(",
		"void SdkHost::AuthLoginWithSso(",
		"void SdkHost::AuthLoginWithWallet(",
	} {
		bodies = append(bodies, definitionBody(t, "SdkHost.cpp", host, signature))
	}
	controller := stripComments(readAppSource(t, "AppController.cpp"))
	if mutateUi != nil {
		before := controller
		controller = mutateUi(controller)
		if controller == before {
			t.Fatal("negative control did not change the production AppController.cpp")
		}
	}
	for _, handler := range []struct{ prefix, method string }{
		{"sdk_.SetAuthStateHandler(", "WireAuth"},
		{"sdk_.SetSignOutProgressHandler(", "WireProgress"},
	} {
		start := strings.Index(controller, handler.prefix)
		if start < 0 {
			t.Fatalf("AppController no longer installs %s", handler.prefix)
		}
		end := strings.Index(controller[start:], "\n  });")
		if end < 0 {
			t.Fatalf("AppController's %s has no closing registration", handler.prefix)
		}
		bodies = append(bodies, "void AppController::"+handler.method+"() {\n"+
			controller[start:start+end+len("\n  });")]+"\n}")
	}
	if err := os.WriteFile(filepath.Join(dir, "sign-out-registration-source.inc"), []byte(strings.Join(bodies, "\n")), 0600); err != nil {
		t.Fatal(err)
	}
	program := filepath.Join(dir, "sign-out-registration-tests")
	build := exec.Command(compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-pthread",
		"-I"+dir, "-I"+filepath.Join(root, "app", "src", "App"),
		filepath.Join(root, "app", "tools", "sign-out-registration-tests.cpp"), "-o", program)
	if output, err := build.CombinedOutput(); err != nil {
		t.Fatalf("compile production registration/logout methods: %v\n%s", err, output)
	}
	return program
}

func TestSignOutRegistrationUsesTheOwningGeneration(t *testing.T) {
	program := signOutRegistrationProgram(t, nil)
	if output, err := exec.Command(program).CombinedOutput(); err != nil {
		t.Fatalf("production registration/logout methods: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}
}

// The old catch logged the storage exception and continued with a successful
// sign-in. Removing only the error assignment must reproduce that failure.
func TestSignOutRegistrationRejectsFailedCredentialStorage(t *testing.T) {
	program := signOutRegistrationProgram(t, func(source string) string {
		old := `persistenceError = "could not store the device credentials";`
		if strings.Count(source, old) != 1 {
			t.Fatalf("SdkHost.cpp no longer has exactly one %q; update this control", old)
		}
		return strings.Replace(source, old, "(void)e;", 1)
	})
	output, err := exec.Command(program).CombinedOutput()
	if err == nil || !strings.Contains(string(output), "failed credential storage never publishes LoggedIn") {
		t.Fatalf("ignored credential write failure was not detected: %v\n%s", err, output)
	}
}

func requireSignOutRegistrationFailure(t *testing.T, old, replacement, want string) {
	t.Helper()
	program := signOutRegistrationProgram(t, func(source string) string {
		if strings.Count(source, old) != 1 {
			t.Fatalf("SdkHost.cpp no longer has exactly one %q; update this control", old)
		}
		return strings.Replace(source, old, replacement, 1)
	})
	output, err := exec.Command(program).CombinedOutput()
	if err == nil || !strings.Contains(string(output), want) {
		t.Fatalf("negative control did not reach the expected runtime assertion %q: %v\n%s", want, err, output)
	}
}

func TestSignOutRegistrationRejectsAnAuthResponseThatBorrowsTheNextEpoch(t *testing.T) {
	requireSignOutRegistrationFailure(t,
		"const std::uint64_t signOutEpoch = authEpoch;",
		"(void)authEpoch; const std::uint64_t signOutEpoch = signOutRun_.Epoch();",
		"passes its original epoch through registration, even after its earlier check passed")
}

func TestSignOutRegistrationRejectsAnOutstandingCredentialDelete(t *testing.T) {
	requireSignOutRegistrationFailure(t,
		"if (localState_) localState_->logout();",
		"if (asyncLocalState_) asyncLocalState_->logout([](bool) {});",
		"draining the old account's writes cannot erase the released sign-in")
}

func TestSignOutRegistrationRejectsOldApprovalCleanup(t *testing.T) {
	requireSignOutRegistrationFailure(t,
		`if (AuthEventIsCurrent(authEpoch) && api_) api_->setByJwt("");`,
		`(void)authEpoch; if (api_) api_->setByJwt("");`,
		"the old approval's failure cleanup cannot clear the next account's API credential")
}

func TestSignOutRegistrationRejectsDeferredReleaseUnderANewerEpoch(t *testing.T) {
	requireSignOutRegistrationFailure(t,
		"RegisterNetworkClient(next->byJwt, std::move(next->done), next->epoch);",
		"RegisterNetworkClient(next->byJwt, std::move(next->done), signOutRun_.Epoch());",
		"a second logout invalidates an already released deferred registration")
}

func TestSignOutRegistrationRejectsAnAuthResponseWithoutItsOriginalGuard(t *testing.T) {
	requireSignOutRegistrationFailure(t,
		"if (AuthEventIsCurrent(epoch)) return true;",
		"const bool current = AuthEventIsCurrent(epoch); (void)current; if (true) return true;",
		"answers the old caller exactly once as cancelled")
}

func TestSignOutRegistrationRejectsQueuedProgressFromAnOlderTail(t *testing.T) {
	program := signOutRegistrationProgramWithUi(t, nil, func(source string) string {
		old := "sdk_.SetSignOutProgressHandler([this](bool) {\n    OnUi([this] {"
		if strings.Count(source, old) != 1 {
			t.Fatal("AppController's sign-out progress handler changed; update this control")
		}
		start := strings.Index(source, old)
		end := strings.Index(source[start:], "\n  });")
		if end < 0 {
			t.Fatal("AppController's sign-out progress handler has no closing registration")
		}
		end += start + len("\n  });")
		handler := source[start:end]
		if strings.Count(handler, "self->SetSigningOut(sdk_.SigningOut());") != 1 {
			t.Fatal("AppController's sign-out progress render changed; update this control")
		}
		handler = strings.Replace(handler, old,
			"sdk_.SetSignOutProgressHandler([this](bool signingOut) {\n    OnUi([this, signingOut] {", 1)
		handler = strings.Replace(handler, "self->SetSigningOut(sdk_.SigningOut());", "self->SetSigningOut(signingOut);", 1)
		return source[:start] + handler + source[end:]
	})
	output, err := exec.Command(program).CombinedOutput()
	if err == nil || !strings.Contains(string(output), "an old false sign-out progress event cannot hide a second logout still running") {
		t.Fatalf("stale sign-out progress did not reach its runtime assertion: %v\n%s", err, output)
	}
}

// Both server cleanup dependencies execute their extracted production bodies.
// Immediate local callbacks expose call ordering without sleeping or networking.
func TestSignOutRegistrationKeepsServerCleanupBeforeCredentialDisposal(t *testing.T) {
	program := signOutRegistrationProgram(t, nil)
	if output, err := exec.Command(program, "server-cleanup").CombinedOutput(); err != nil {
		t.Fatalf("production server cleanup ordering: %v\n%s", err, output)
	} else if !strings.Contains(string(output), "server-cleanup dependency checks passed") {
		t.Fatalf("server cleanup control did not run: %s", output)
	}
}

func TestSignOutRegistrationRemovesTheAbandonedResultClient(t *testing.T) {
	program := signOutRegistrationProgram(t, nil)
	if output, err := exec.Command(program, "abandoned-client").CombinedOutput(); err != nil {
		t.Fatalf("production abandoned-client cleanup: %v\n%s", err, output)
	} else if !strings.Contains(string(output), "abandoned-client dependency checks passed") {
		t.Fatalf("abandoned-client control did not run: %s", output)
	}
}
