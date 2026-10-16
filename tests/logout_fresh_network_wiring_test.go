// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"regexp"
	"testing"
)

// The owner's 2026-10-05 decision that a sign-out must not cross contaminate
// networks: each network starts fresh. The service's logout already severs
// the device identity and clears what its sdk stored for the account
// (sign_out_wiring_test.go); these pin what the app itself clears. SdkHost
// needs Windows, so they read its source with every comment blanked.

// Logout first answers and forgets the browser and wallet flows the account
// started, before it takes either lock, so a late add-sign-in return cannot
// add a method to the next account signed in; and it clears the credential the
// api attaches to its calls with the local logout, before the service is
// asked.
func TestLogoutFreshNetworkWiringEndsTheAccountsFlowsAndApiCredential(t *testing.T) {
	host := sdkHostSource(t)
	logout := signOutSource(t, host)
	signOutRequireInOrder(t, "SdkHost::Logout", logout,
		regexp.QuoteMeta(`CancelPendingWalletFlows("superseded by signing out");`),
		regexp.QuoteMeta("std::scoped_lock lock(pendingMutex_);"),
		regexp.QuoteMeta("std::scoped_lock lock(mutex_);"),
		regexp.QuoteMeta("pendingAuthJwt_.reset();"),
		regexp.QuoteMeta("pendingInstantJwt_.reset();"),
		regexp.QuoteMeta("asyncLocalState_->logout("),
		regexp.QuoteMeta(`if (api_) api_->setByJwt("");`),
		regexp.QuoteMeta("signOut_.Begin(SignOutServiceLocked());"))

	// what that cancel ends: the attempt's state and nonce, and the add
	// sheet's answer, so the attempt's return matches nothing
	cancel := definitionBody(t, "SdkHost.cpp", host,
		"uint64_t SdkHost::CancelPendingWalletFlows(const char* reason) {")
	signOutRequireInOrder(t, "SdkHost::CancelPendingWalletFlows", cancel,
		regexp.QuoteMeta("const uint64_t flow = walletFlows_.Start();"),
		regexp.QuoteMeta("ssoAttempt_.reset();"),
		regexp.QuoteMeta("std::exchange(ssoAddDone_, nullptr)"),
		regexp.QuoteMeta("std::exchange(walletAuthDone_, nullptr)"))
}
