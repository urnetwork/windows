// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// Settings' "Add sign-in method" sheet offered only an email or phone, while
// ur.io and the other apps offer Apple, Google, a Solana or Bittensor wallet,
// and an email or phone. The option set and the add flow are App/AddSignIn.h,
// compiled and run here (app/tools/add-sign-in-tests.cpp).
func TestAddSignInOptionsAndFlow(t *testing.T) {
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("add sign-in tests require a C++20 compiler: ", err)
	}
	root := repositoryRoot(t)
	spec := filepath.Join(root, "app", "tools", "add-sign-in-tests.cpp")
	if _, err := os.Stat(spec); err != nil {
		t.Fatalf("the add sign-in spec is missing: %v", err)
	}
	program := filepath.Join(t.TempDir(), "add-sign-in-tests")
	build := exec.Command(compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror",
		"-I"+filepath.Join(root, "app", "src", "App"), spec, "-o", program)
	if output, err := build.CombinedOutput(); err != nil {
		t.Fatalf("build add sign-in tests: %v\n%s", err, output)
	}
	if output, err := exec.Command(program).CombinedOutput(); err != nil {
		t.Fatalf("add sign-in: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}
}

// The sheet lists the shared option set, and its Apple, Google and wallet legs
// go through the add flow.
func TestAddAuthSheetOffersEveryMethod(t *testing.T) {
	sheets := stripLineComments(readAppSource(t, "SettingsSheets.cpp"))
	build := functionBody(sheets, "void AddAuthSheet::Build(XamlRoot const& root)")
	for _, want := range []string{
		"add_sign_in::kMethods[i]",
		"add_sign_in::kWalletChains[c]",
		"bittensor::kChooserWallets[i]",
		"std::make_unique<add_sign_in::AddSignInFlow>(*addSession_)",
		"self->add_->StartProvider(self->method_)",
	} {
		if !strings.Contains(build, want) {
			t.Errorf("AddAuthSheet::Build: missing %s", want)
		}
	}
	if !strings.Contains(functionBody(sheets, "void AddAuthSheet::StartWallet("), "add_->StartWallet(chain, walletId)") {
		t.Error("AddAuthSheet::StartWallet: the wallet leg does not go through the add flow")
	}
	// the credential is posted to addAuth and nothing else
	session := functionBody(sheets, "class SdkAddSignInSession")
	for _, want := range []string{
		"sdk_.SsoTokenForAdd(",
		"sdk_.SignSolanaForAdd(",
		"sdk_.SignBittensorForAdd(",
		"sdk_.api().addAuth(",
		"sdk_.CancelAddSignIn()",
	} {
		if !strings.Contains(session, want) {
			t.Errorf("SdkAddSignInSession: missing %s", want)
		}
	}
	for _, unwanted := range []string{"authLogin", "SignInWith", "RefreshJwt", "Logout", "VerifyCode", "SetAuthState"} {
		if strings.Contains(session, unwanted) {
			t.Errorf("SdkAddSignInSession: adding a method must not sign in or move the session (%s)", unwanted)
		}
	}
}

// An add-owned Google / Apple return goes to addAuth and never to authLogin,
// and the add paths never move the auth state or touch the pending sign-in.
func TestAddSignInNeverSignsIn(t *testing.T) {
	host := stripLineComments(readAppSource(t, "SdkHost.cpp"))
	if !strings.Contains(host, "switch (add_sign_in::RouteSsoReturn(attempt.purpose))") {
		t.Error("SdkHost.cpp: the sso return is not routed by who started the attempt")
	}
	add := functionBody(host, "void SdkHost::SsoTokenForAdd(")
	if !strings.Contains(add, "OpenSsoAttempt(provider, add_sign_in::SsoPurpose::Add)") {
		t.Error("SdkHost::SsoTokenForAdd: the attempt is not owned by the add sheet")
	}
	if !strings.Contains(functionBody(host, "void SdkHost::SignInWithSso("), "OpenSsoAttempt(provider, add_sign_in::SsoPurpose::SignIn)") {
		t.Error("SdkHost::SignInWithSso: the login attempt is not marked as a sign-in")
	}
	if !strings.Contains(functionBody(host, "void SdkHost::SignBittensorForAdd("), "bittensor::kPurposeAdd") {
		t.Error("SdkHost::SignBittensorForAdd: the proof is not under the add purpose")
	}
	for _, name := range []string{
		"void SdkHost::SsoTokenForAdd(",
		"void SdkHost::SignSolanaForAdd(",
		"void SdkHost::SignBittensorForAdd(",
		"void SdkHost::CancelAddSignIn(",
	} {
		body := functionBody(host, name)
		if body == "" {
			t.Errorf("SdkHost.cpp: %s is missing", name)
			continue
		}
		for _, unwanted := range []string{
			"SetAuthState", "AuthLoginWithSso", "AuthLoginWithWallet", "RegisterNetworkClient",
			"walletAuthDone_", "pendingAuthJwt_", "pendingWalletAuth_",
		} {
			if strings.Contains(body, unwanted) {
				t.Errorf("%s: adding a method must not sign in or move the session (%s)", name, unwanted)
			}
		}
	}
	// a superseded or failed add attempt is answered, not left busy
	if !strings.Contains(functionBody(host, "uint64_t SdkHost::CancelPendingWalletFlows("), "std::exchange(ssoAddDone_, nullptr)") {
		t.Error("SdkHost::CancelPendingWalletFlows: an add attempt is not answered when superseded")
	}
}
