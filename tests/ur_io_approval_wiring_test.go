// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
)

// The call sites of the browser approval ("Continue in browser"). What the attempt is and decides is
// pure and runs in ur_io_approval_test.go; how the links reach the gate is auth_link_wiring_test.go; the
// registration it needs is protocol_handler_wiring_test.go. AppController, SdkHost, the login page and the
// checks need Windows and WinRT, so these read their sources with every comment blanked, and a
// function that is a decision is pinned to the statements it may hold.

func urIoController(t *testing.T) string {
	return stripComments(lf(readAppSource(t, "AppController.cpp")))
}

func urIoHost(t *testing.T) string {
	return stripComments(lf(readAppSource(t, "SdkHost.cpp")))
}

// The attempt ends wherever the contract says it ends, and nowhere starts it but a click: the three
// auth-state hooks, the quit, the five minutes, a server change, a sign-out and every other flow.
func TestApprovalAttemptEndsAtEveryHook(t *testing.T) {
	controller := urIoController(t)

	// what the app was before this push is read first: read after the assignment, every sign-out would look like a repeat of
	// one, and the attempt, the balance store and the latch would never see it end
	onAuth := definitionBody(t, "AppController.cpp", controller, "void AppController::OnAuthState(AuthState state, const std::string& error) {")
	signOutRequireInOrder(t, "AppController::OnAuthState", onAuth,
		regexp.QuoteMeta("const bool wasLoggedIn = (authState_ == AuthState::LoggedIn);"),
		regexp.QuoteMeta("authState_ = state;"),
		`if \(state == AuthState::Authenticating\) \{\s*CancelApproval\(approval::ClearReason::SignInStarted\);\s*\} else if \(state == AuthState::LoggedIn\) \{\s*CancelApproval\(approval::ClearReason::SignedIn\);\s*\} else if \(state == AuthState::LoggedOut && wasLoggedIn\) \{\s*CancelApproval\(approval::ClearReason::SignedOut\);\s*\} else if \(approval_\.Live\(\) && approval_\.Flow\(\) != sdk_\.CurrentBrowserFlow\(\)\) \{\s*CancelApproval\(approval::ClearReason::Superseded\);\s*\}`,
		regexp.QuoteMeta("UpdateTray();"))

	shutdown := definitionBody(t, "AppController.cpp", controller, "void AppController::Shutdown(lifetime::Ending ending) {")
	signOutRequireInOrder(t, "AppController::Shutdown", shutdown,
		regexp.QuoteMeta("if (quitting_.exchange(true, std::memory_order_acq_rel)) return;"),
		regexp.QuoteMeta("CancelApproval(approval::ClearReason::Shutdown);"),
		regexp.QuoteMeta("balance_.Stop();"))

	// the five minutes: one one-shot timer, created with the controller's others, restarted by Begin and
	// stopped by every end of the attempt, and by the destructor
	start := definitionBody(t, "AppController.cpp", controller, "void AppController::Start() {")
	provideRequire(t, "AppController::Start", start,
		"approvalTimer_ = uiThread_.CreateTimer();", "approvalTimer_.Interval(approval::kLife);",
		"approvalTimer_.IsRepeating(false);", "approvalTimer_.Tick([this](auto const&, auto const&) { OnApprovalTimer(); });")
	provideRequireOrder(t, "AppController::Start", start, "approvalTimer_.IsRepeating(false);", "OnApprovalTimer();")
	destructor := definitionBody(t, "AppController.cpp", controller, "AppController::~AppController() {")
	provideRequire(t, "AppController::~AppController", destructor, "if (approvalTimer_) approvalTimer_.Stop();")
	cancel := definitionBody(t, "AppController.cpp", controller, "void AppController::CancelApproval(approval::ClearReason reason) {")
	signOutRequireInOrder(t, "AppController::CancelApproval", cancel,
		regexp.QuoteMeta("if (approvalTimer_) approvalTimer_.Stop();"),
		regexp.QuoteMeta("if (!approval_.Live()) return;"),
		regexp.QuoteMeta("approval_.Clear(reason);"))
	timer := definitionBody(t, "AppController.cpp", controller, "void AppController::OnApprovalTimer() {")
	signOutRequireInOrder(t, "AppController::OnApprovalTimer", timer,
		regexp.QuoteMeta("CancelApproval(approval::ClearReason::TimedOut);"),
		regexp.QuoteMeta("login->OnApprovalExpired();"))
	// the sheet is told whether or not the slot still held the attempt: the gate wipes one that it finds dead (a deadline
	// passed on the wall clock, another flow), and a tick that returned for that would leave the sheet waiting for good
	if strings.Contains(timer, "return;") || strings.Contains(timer, "Live()") {
		t.Error("AppController::OnApprovalTimer returns early or asks whether the slot is live: the sheet must hear of a timeout the gate already wiped")
	}
	begin := definitionBody(t, "AppController.cpp", controller, "std::optional<approval::Slot::View> AppController::BeginApproval() {")
	signOutRequireInOrder(t, "AppController::BeginApproval", begin,
		regexp.QuoteMeta("approval_.Begin(std::move(params), RandomBytes)"),
		regexp.QuoteMeta("approvalTimer_.Stop();"),
		regexp.QuoteMeta("approvalTimer_.Start();"))

	// the slot is started, consumed and cleared from these places and no others
	for pattern, want := range map[string]int{
		"approval_.Begin(":      1, // BeginApproval
		"approval_.Consume(":    1, // HandleAuthLink
		"approval_.Clear(":      1, // CancelApproval
		"approval_.Check(":      1, // HandleAuthLink
		"approval_.Peek(":       1, // BeginApproval
		"approval_.Reopenable(": 1, // BeginApproval: a second click re-opens only a request the page still accepts
		"approval_.SecretKey()": 1, // HandleAuthLink, for the unseal
		"approval_.Flow()":      2, // OnAuthState (a server change leaves a dead attempt) and BeginApproval (a second click)
		"approval_.Live()":      2, // OnAuthState, CancelApproval
	} {
		if count := strings.Count(controller, pattern); count != want {
			t.Errorf("AppController.cpp calls %s %d times, want %d", pattern, count, want)
		}
	}
	for name, code := range authLinkSourceFiles(t) {
		if name != "App/AppController.cpp" && (strings.Contains(code, "approval_.") || strings.Contains(code, "approval().")) {
			t.Errorf("%s reaches the approval slot: the controller owns the attempt, and the sheet asks it through BeginApproval, ApprovalRequestUrl, CancelApproval, ApprovalOffered and WindowHides", name)
		}
	}
}

// The SDK half of the same ends, and the flow number an attempt rides on. Every browser or wallet
// flow, a sign-out and a server change take the next number (CancelPendingWalletFlows), so an attempt
// that began before them is a stray when its answer comes: that is what binds an attempt that names
// a network (version 2) to the network it started in, and what makes its return link a stray once the
// network has changed. Every other sign-in pushes Authenticating when it starts, which ends it
// through the controller's hook.
func TestApprovalAttemptRidesTheFlowSerial(t *testing.T) {
	host := urIoHost(t)

	server := definitionBody(t, "SdkHost.cpp", host, "bool SdkHost::ApplyNetworkServer(")
	cancelAt := strings.Index(server, `CancelPendingWalletFlows("superseded by a network change");`)
	lockAt := strings.Index(server, "std::scoped_lock lock(mutex_);")
	if cancelAt < 0 || lockAt < 0 || cancelAt > lockAt {
		t.Error(`ApplyNetworkServer must call CancelPendingWalletFlows("superseded by a network change") before it takes mutex_: it answers the callbacks of the flows it supersedes`)
	}
	provideRequire(t, "SdkHost::ApplyNetworkServer", server, "pendingApprovedJwt_.reset();")
	// The snapshot of the network is cleared before the switch and read again from the space that is in use after it, whether it
	// worked or threw. A switch that threw before it replaced the space leaves the old one in use, and a snapshot left empty would
	// hide the action on the network still in use until the next switch or a restart. One that threw after networkSpace_ was
	// replaced leaves api_ on the old server (the space is assigned a few statements before api_ is), and a code minted for the
	// network the new space names must not be redeemed there, so the snapshot stays empty. So the read is after the catch, under
	// the same lock and not in the try, for a switch that worked or never got as far as replacing the space. These three
	// stretches of the function are pinned as they are, with nothing between their statements: a call moved into the catch or
	// put under `if (!ok)` leaves the snapshot empty after every successful switch, and the ordered fragments that were here let
	// both through.
	squashed := authLinkSquash(server)
	for _, stretch := range []struct{ why, text string }{
		{"the snapshot is cleared, and the local that says the space was replaced starts false, before the switch",
			"SetApprovalNetwork(approval::Network()); bool spaceReplaced = false; try {"},
		{"the local is set the moment networkSpace_ is replaced, before anything else in the switch can throw",
			"networkSpace_ = spaceManager_->updateNetworkSpaceValues(key, values); spaceReplaced = true; spaceManager_->setActiveNetworkSpace(*networkSpace_);"},
		{"the snapshot is read again after the catch, for a switch that worked or never replaced the space, and a failed switch returns",
			`ok = true; } catch (const std::exception& e) { LogError("sdkhost: switch network space to '{}' failed: {}", hostName, e.what()); ok = false; } if (ok || !spaceReplaced) UpdateApprovalNetworkLocked(); } if (!ok) return false;`},
	} {
		if !strings.Contains(squashed, stretch.text) {
			t.Errorf("SdkHost::ApplyNetworkServer does not have this, as written and with nothing between its statements (comments and whitespace aside): %s\n  %s", stretch.why, stretch.text)
		}
	}
	if count := strings.Count(server, "spaceReplaced"); count != 3 {
		t.Errorf("SdkHost::ApplyNetworkServer names spaceReplaced %d times; it is declared, set when the space is replaced and read after the catch, and nothing else touches it", count)
	}

	begin := definitionBody(t, "SdkHost.cpp", host, "std::uint64_t SdkHost::BeginBrowserApproval() {")
	signOutRequireInOrder(t, "SdkHost::BeginBrowserApproval", begin,
		regexp.QuoteMeta(`const std::uint64_t flow = CancelPendingWalletFlows("superseded by a ur.io approval");`),
		regexp.QuoteMeta("approvalFlow_.store(flow, std::memory_order_release);"),
		regexp.QuoteMeta("DiscardApprovedNetwork();"),
		regexp.QuoteMeta("if (authState_.load() == AuthState::Authenticating) SetAuthState(AuthState::LoggedOut);"),
		regexp.QuoteMeta("return flow;"))
	quitForbid(t, "SdkHost::BeginBrowserApproval", begin,
		"it supersedes the other flows, resets a stuck Authenticating and starts nothing else", "std::scoped_lock", "RegisterNetworkClient(",
		"LoginWith", "SignInWith", "api_")
	provideRequire(t, "SdkHost.h", stripComments(lf(readAppSource(t, "SdkHost.h"))),
		"std::uint64_t CurrentBrowserFlow() const { return walletFlows_.Current(); }",
		"approval::Network CurrentApprovalNetwork() const;",
		"mutable std::mutex approvalNetworkMutex_;", "approval::Network approvalNetwork_;",
		"void UpdateApprovalNetworkLocked();", "void SetApprovalNetwork(approval::Network network);",
		"std::atomic<AuthState> authState_{AuthState::LoggedOut};",
		"std::optional<std::string> pendingApprovedJwt_;",
		"std::atomic<std::uint64_t> approvalFlow_{0};")

	// the number the redeem of a code is held to is written by the start of an approval and read by the redeem, and by nothing else
	if count := strings.Count(host, "approvalFlow_"); count != 2 {
		t.Errorf("SdkHost.cpp touches approvalFlow_ %d times; BeginBrowserApproval writes it and RedeemApprovedCode reads it", count)
	}
	if !strings.Contains(begin, "approvalFlow_.store(flow, std::memory_order_release);") ||
		!strings.Contains(definitionBody(t, "SdkHost.cpp", host, "void SdkHost::RedeemApprovedCode(const std::string& code, std::function<void(ApprovedNetwork)> done) {"), "approvalFlow_.load(") {
		t.Error("approvalFlow_ is not written by BeginBrowserApproval and read by RedeemApprovedCode")
	}

	logout := signOutSource(t, host)
	signOutRequireInOrder(t, "SdkHost::Logout", logout,
		regexp.QuoteMeta(`CancelPendingWalletFlows("superseded by signing out");`),
		regexp.QuoteMeta("pendingInstantJwt_.reset();"),
		regexp.QuoteMeta("pendingApprovedJwt_.reset();"),
		regexp.QuoteMeta("asyncLocalState_->logout("))

	// the network of the space: the official one is the operator's host and no url of the user's own, any other is named by the API
	// url the client really uses (what getApiUrl answers: what Change Network API typed, or what the host derives), and one whose url
	// cannot be named is none. Cached for the UI thread.
	space := definitionBody(t, "SdkHost.cpp", host, "void SdkHost::UpdateApprovalNetworkLocked() {")
	provideRequire(t, "SdkHost::UpdateApprovalNetworkLocked", space,
		"networkSpace_->getHostName() == ids::kNetworkSpaceHostName",
		"networkSpace_->getConfiguredApiUrl().empty()", "networkSpace_->getConfiguredPlatformUrl().empty()",
		"approval::NetworkOf(official, networkSpace_->getApiUrl(), ids::kNetworkSpaceHostName)", "SetApprovalNetwork(std::move(network));")
	initialize := definitionBody(t, "SdkHost.cpp", host, "bool SdkHost::Initialize() {")
	// after the legacy space has been re-keyed to the operator's host (StartBundledSpace migrates first and binds second), which is
	// how the re-keying reaches the snapshot: it is read from the space that came out of it
	provideRequireOrder(t, "SdkHost::Initialize", initialize, "netspace::StartBundledSpace(", "UpdateApprovalNetworkLocked();")
	if strings.Count(host, "UpdateApprovalNetworkLocked();") != 2 {
		t.Error("SdkHost.cpp must refresh the snapshot of the approval's network in Initialize and ApplyNetworkServer, and nowhere else")
	}
	// the snapshot is written by those two and read by one accessor; nothing else touches it or sets it
	if count := strings.Count(host, "SetApprovalNetwork("); count != 3 { // the definition, UpdateApprovalNetworkLocked, ApplyNetworkServer
		t.Errorf("SdkHost.cpp names SetApprovalNetwork %d times; the definition, UpdateApprovalNetworkLocked and ApplyNetworkServer's clear are all that may", count)
	}
	if count := strings.Count(host, "approvalNetwork_"); count != 2 { // the accessor's read, SetApprovalNetwork's write
		t.Errorf("SdkHost.cpp touches approvalNetwork_ %d times; CurrentApprovalNetwork reads it and SetApprovalNetwork writes it, under their own lock", count)
	}
	// ... and read without the lock a switch holds across calls into the SDK
	reader := definitionBody(t, "SdkHost.cpp", host, "approval::Network SdkHost::CurrentApprovalNetwork() const {")
	quitForbid(t, "SdkHost::CurrentApprovalNetwork", reader, "the UI thread reads the snapshot under its own small lock, never the one a switch holds across SDK calls",
		"(mutex_)", "api_", "networkSpace_", "getApiUrl", "CurrentNetworkServer")
	// never from a name a user or a server could choose, or from the host name that was typed: the network is named by the API url the client uses
	for _, forbidden := range []string{"linkHostName()", "kOfficialLinkHostName", "link_host_name", "hostName", "CurrentNetworkServer"} {
		if strings.Contains(space, forbidden) {
			t.Errorf("UpdateApprovalNetworkLocked reads %s: whether the space is the official one is its host and its urls, and any other is named by the API url the client uses", forbidden)
		}
	}
	if strings.Count(space, "getApiUrl()") != 1 || strings.Count(space, "getHostName()") != 1 {
		t.Error("UpdateApprovalNetworkLocked reads the host name once (is it the operator's) and the API url once (what the client really uses)")
	}
	// ... through the getter that CurrentNetworkServer (what the Change Network API sheet shows as the live API url) reports it by
	provideRequire(t, "SdkHost::CurrentNetworkServer", definitionBody(t, "SdkHost.cpp", host, "SdkHost::NetworkServer SdkHost::CurrentNetworkServer() {"),
		"out.apiUrl = networkSpace_->getApiUrl();")

	// every sign-in start pushes Authenticating before anything else: that is what ends an attempt that
	// the flow serial does not (a password, a seedphrase, a code, a sign-up)
	for _, signature := range []string{
		"void SdkHost::LoginWithPassword(", "void SdkHost::LoginWithCode(", "void SdkHost::CreateNetwork(",
		"void SdkHost::VerifyCode(", "void SdkHost::LoginWithSeedphrase(", "void SdkHost::ConfirmInstantAccount(",
		"void SdkHost::ConfirmApprovedNetwork(", "void SdkHost::SignInWithSolana(", "void SdkHost::SignInWithBittensor(",
		"void SdkHost::SignInWithSso(",
	} {
		body := definitionBody(t, "SdkHost.cpp", host, signature)
		if !strings.Contains(body, "SetAuthState(AuthState::Authenticating);") {
			t.Errorf("%s no longer pushes AuthState::Authenticating when it starts: that push is what ends a browser approval that is waiting", signature)
		}
	}
}

// The credential an approval redeems is held, not registered or stored, until the person has been shown which
// network this computer would join and said yes. Redeem takes the code to the server and holds what comes back;
// Confirm is the one place it is registered, after pushing Authenticating, and takes the credential off the api
// again if the registration is refused; Discard, a sign-out, a server change and a new approval drop it. A
// credential with no network name to show is refused outright, and a refusal is never retried.
func TestRedeemHoldsTheCredentialUntilConfirmed(t *testing.T) {
	host := urIoHost(t)

	redeem := definitionBody(t, "SdkHost.cpp", host, "void SdkHost::RedeemApprovedCode(const std::string& code, std::function<void(ApprovedNetwork)> done) {")
	quitForbid(t, "SdkHost::RedeemApprovedCode", redeem, "redeeming holds the credential: it registers nothing, stores nothing and announces nothing",
		"RegisterNetworkClient(", "SetAuthState(", "setByJwt(", "localState_", "asyncLocalState_", "Authenticating", "LoginWithCode(")
	signOutRequireInOrder(t, "SdkHost::RedeemApprovedCode", redeem,
		regexp.QuoteMeta("args.auth_code = code;"),
		regexp.QuoteMeta("const std::uint64_t flow = approvalFlow_.load(std::memory_order_acquire);"),
		regexp.QuoteMeta("if (err || !result) {"),
		regexp.QuoteMeta("ApprovedNetwork::Failure::Transport"),
		regexp.QuoteMeta("if (result->error && !result->error->message.empty()) {"),
		regexp.QuoteMeta("ApprovedNetwork::Failure::Refused"),
		regexp.QuoteMeta(`JwtClaimString(result->by_jwt, "network_name")`),
		regexp.QuoteMeta("if (!networkName || networkName->empty()) {"),
		regexp.QuoteMeta("superseded = flow == 0 || !walletFlows_.IsCurrent(flow);"),
		regexp.QuoteMeta("if (!superseded) pendingApprovedJwt_ = result->by_jwt;"),
		regexp.QuoteMeta("out.ok = true;"),
		`std::scoped_lock lock\(mutex_\);\s*if \(api_\) \{\s*superseded = flow == 0 \|\| !walletFlows_\.IsCurrent\(flow\);\s*if \(!superseded\) \{\s*api_->authCodeLogin\(args, answer\);\s*return;\s*\}\s*\}`)
	if count := strings.Count(redeem, "authCodeLogin("); count != 1 {
		t.Errorf("RedeemApprovedCode dispatches authCodeLogin %d times; a code is spent by its first redeem and nothing retries", count)
	}
	// the code is held to the flow its approval began in, twice and no more: before it goes out and before what comes back is kept,
	// each under the lock the handle or the hold is touched under, and against the one number that was read before either
	if count := strings.Count(redeem, "walletFlows_.IsCurrent(flow)"); count != 2 {
		t.Errorf("RedeemApprovedCode judges the flow %d times; it does before the code goes out and before the credential is held, and nowhere else", count)
	}
	if count := strings.Count(redeem, "approvalFlow_"); count != 1 || !strings.Contains(redeem, "const std::uint64_t flow = approvalFlow_.load(std::memory_order_acquire);") {
		t.Error("RedeemApprovedCode reads the flow of the approval once, before it builds the answer, and the answer and the dispatch judge that one number")
	}
	// the dispatch is the only thing under the lock here: the answer is delivered outside it
	if strings.Count(redeem, "done(out)") < 5 {
		t.Error("RedeemApprovedCode no longer answers every outcome")
	}

	confirm := definitionBody(t, "SdkHost.cpp", host, "void SdkHost::ConfirmApprovedNetwork(std::function<void(AuthResult)> done) {")
	signOutRequireInOrder(t, "SdkHost::ConfirmApprovedNetwork", confirm,
		regexp.QuoteMeta("jwt = std::move(*pendingApprovedJwt_);"),
		regexp.QuoteMeta("pendingApprovedJwt_.reset();"),
		regexp.QuoteMeta(`done({false, false, "no approved sign-in is pending"});`),
		regexp.QuoteMeta("SetAuthState(AuthState::Authenticating);"),
		regexp.QuoteMeta("RegisterNetworkClient(jwt, [this, done](AuthResult r) {"),
		regexp.QuoteMeta("if (!r.ok) {"),
		regexp.QuoteMeta(`if (api_) api_->setByJwt("");`),
		regexp.QuoteMeta("if (done) done(r);"))
	if count := strings.Count(confirm, "RegisterNetworkClient("); count != 1 {
		t.Errorf("ConfirmApprovedNetwork registers %d times; once, with the credential it took out of the hold", count)
	}
	discard := definitionBody(t, "SdkHost.cpp", host, "void SdkHost::DiscardApprovedNetwork() {")
	provideRequire(t, "SdkHost::DiscardApprovedNetwork", discard, "std::scoped_lock lock(mutex_);", "pendingApprovedJwt_.reset();")

	// the hold is touched by these and by nothing else
	holders := 0
	for _, signature := range []string{
		"void SdkHost::RedeemApprovedCode(", "void SdkHost::ConfirmApprovedNetwork(", "void SdkHost::DiscardApprovedNetwork(",
		"bool SdkHost::ApplyNetworkServer(", "void SdkHost::SignOutTailLocked() {",
	} {
		holders += strings.Count(definitionBody(t, "SdkHost.cpp", host, signature), "pendingApprovedJwt_")
	}
	if total := strings.Count(host, "pendingApprovedJwt_"); total != holders {
		t.Errorf("SdkHost.cpp touches the held credential %d times, %d of them outside the functions that own it", total, total-holders)
	}
	// the one way the held credential is registered is the sheet's confirm: ConfirmApprovedNetwork alone
	// takes it out of the hold
	if strings.Count(host, "std::move(*pendingApprovedJwt_)") != 1 {
		t.Error("the held credential leaves the hold in more than ConfirmApprovedNetwork")
	}
}

// The browser sheet's pins: a tray hide sweeps every sheet and the attempt outlives it; nothing hides a sheet but the
// sheet and the sweep; every launch is awaited and the paste field is a password box; the request is the controller's;
// and a credential held for a confirm that never came is discarded. The sheet lives in its own file, so that the
// seedphrase sheet's Closing handler stays the first in AuthSheets.cpp.
func TestApprovalSheetObeysTheSheetPins(t *testing.T) {
	path := filepath.Join(repositoryRoot(t), "app", "src", "App", "BrowserApprovalSheet.cpp")
	if _, err := os.Stat(path); err != nil {
		t.Fatal("BrowserApprovalSheet.cpp is missing: the browser sign-in sheet has its own file")
	}
	raw := lf(readFileOrFail(t, path))
	sheet := stripComments(raw)
	for _, forbidden := range []string{"Dialog().Hide()", "CloseButtonClick(", "mutex_", "SignInWithAuthCode(", "LoginWithCode("} {
		if strings.Contains(sheet, forbidden) {
			t.Errorf("BrowserApprovalSheet.cpp uses %q: a hide to the tray closes every sheet through MainWindow::CloseSheetsForHide, and the sheet closes itself with its own dialog_.Hide()", forbidden)
		}
	}
	// every launch of the browser is awaited, and the controller starts the attempt before it
	launches := regexp.MustCompile(`(?s)(co_await\s+)?winrt::Windows::System::Launcher::LaunchUriAsync\(`).FindAllStringSubmatch(sheet, -1)
	for _, launch := range launches {
		if launch[1] == "" {
			t.Error("BrowserApprovalSheet.cpp: a LaunchUriAsync result is discarded; the sheet observes the launcher's verdict")
		}
	}
	if strings.Contains(sheet, "LaunchUriAsync(") {
		provideRequireOrder(t, "BrowserApprovalSheet.cpp", sheet, "BeginApproval()", "LaunchUriAsync(")
		provideRequire(t, "BrowserApprovalSheet.cpp", sheet, "ApprovalRequestUrl(")
	}
	// the paste field holds a credential: a password box, emptied on submit
	if strings.Contains(sheet, "TextBox field") || strings.Contains(sheet, "TextBox code") {
		t.Error("BrowserApprovalSheet.cpp pastes a code into a TextBox: a PasswordBox keeps it out of the UI Automation tree")
	}
	provideRequire(t, "BrowserApprovalSheet.cpp", sheet, "PasswordBox", "WindowHides()", "CancelApproval(", "DiscardApprovedNetwork()", "SanitizeExternalDisplayText(")
	// a copied link goes to the clipboard without history and without roaming
	if strings.Contains(sheet, "SetContent(") {
		t.Error("BrowserApprovalSheet.cpp copies with SetContent: use SetContentWithOptions with history and roaming off")
	}
	if strings.Contains(sheet, "SetContentWithOptions(") {
		provideRequire(t, "BrowserApprovalSheet.cpp", sheet, "IsAllowedInHistory(false)", "IsRoamable(false)")
	}
	// the action is gated by the controller, and by nothing the sheet could widen
	provideRequire(t, "BrowserApprovalSheet.cpp", sheet, "ApprovalOffered()")
	if !strings.Contains(readAppSource(t, "App.vcxproj"), `<ClCompile Include="BrowserApprovalSheet.cpp" />`) ||
		!strings.Contains(readAppSource(t, "App.vcxproj"), `<ClInclude Include="BrowserApprovalSheet.h" />`) {
		t.Error("App.vcxproj does not list BrowserApprovalSheet.cpp (with the pch, like the other sheets) and BrowserApprovalSheet.h")
	}
	// and it lives in its own file, so the seedphrase sheet's Closing handler is still the first in AuthSheets.cpp
	if strings.Contains(stripComments(lf(readAppSource(t, "AuthSheets.cpp"))), "BrowserApprovalSheet") {
		t.Error("AuthSheets.cpp holds the browser approval sheet: build_contract_test.go finds the seedphrase sheet's Closing handler first in that file")
	}
}

// Wallet and sign-in state is hashed and compared with a random source of the OS and nothing else: the state of an
// attempt is the only thing that makes a link worth acting on.
func TestApprovalStateComesFromTheOsRandomSource(t *testing.T) {
	controller := urIoController(t)
	provideRequire(t, "AppController.cpp", controller,
		"::BCryptGenRandom(nullptr, out, static_cast<ULONG>(length), BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0",
		"approval_.Begin(std::move(params), RandomBytes)")
	weak := regexp.MustCompile(`\brand\s*\(|\bsrand\s*\(|mt19937|random_device|uniform_int_distribution|\bGetTickCount|std::chrono::[a-z_]+::now\(\)\.time_since_epoch\(\)\.count\(\)\s*%`)
	for _, name := range []string{
		"App/AppController.cpp", "App/UrIoApproval.h", "App/AuthLink.h", "App/ProtocolHandler.cpp", "App/ApprovalCheck.cpp",
		"App/ApprovalDescriptor.h", "App/ApprovalNetwork.h", "App/ApprovalUnseal.h", "App/BrowserApprovalSheet.cpp", "Common/ProtocolRegistration.h",
	} {
		code, ok := authLinkSourceFiles(t)[name]
		if !ok {
			continue
		}
		if match := weak.FindString(code); match != "" {
			t.Errorf("%s uses %q: the state and the keys of an attempt come from the OS (BCryptGenRandom) and the sdk, never from a generator of its own", name, match)
		}
	}
	// and the key pair is the sdk's, drawn again when its public key is one the page refuses. Which half of it goes to the
	// browser is decided in one place, approval::NewKeyPair (ApprovalUnseal.h), that the interop spec runs against the sdk,
	// and the controller makes no pair of its own
	provideRequire(t, "AppController.cpp", controller, "approval::FreshKeyPair(approval::NewKeyPair)")
	provideRequire(t, "ApprovalUnseal.h", stripComments(lf(readAppSource(t, "ApprovalUnseal.h"))), "urnet::generateWalletKeyPair()")
	if strings.Contains(controller, "generateWalletKeyPair") || strings.Contains(controller, "PrivateKeyBase58") || strings.Contains(controller, "PublicKeyBase58") {
		t.Error("AppController.cpp reads the halves of a key pair itself: approval::NewKeyPair (ApprovalUnseal.h) is the one place that says which half goes to the browser")
	}
}

// The attempt starts only from a click, on a build that has the flag on, in a network a request can name (the official one, or
// another whose API url is https://host[:port]), in a process that is not elevated, signed out, once the page has
// answered its descriptor and lists the version that network needs (1 for the official network, 2 for another), and no other
// program holds the urnetwork:// scheme.
// The order of a start is the contract's: the registration the link comes back through first, the key pair next, the
// flows superseded last, so a start that cannot finish disturbs none. A second click reads the live attempt back, unless
// another flow has taken the number since (a server change does, and pushes no sign-in state): that attempt is dead, and
// a request for it would send the person to approve something that can never finish. Nor is a request read back once the
// page would call it expired (it refuses one older than 240 s, and the attempt lives for 300): the click starts a new attempt.
// The page check is begun wherever a person lands on a signed-out window: when one is shown, and when the app signs out (or
// switches to another network) under a window that is already on screen.
func TestApprovalIsOfferedOnlyWhenEverythingChecks(t *testing.T) {
	controller := urIoController(t)
	provideRequire(t, "AppController::ApprovalOffered", definitionBody(t, "AppController.cpp", controller, "bool AppController::ApprovalOffered() {"),
		"return ApprovalOfferedIn(sdk_.CurrentApprovalNetwork());")
	// every reason it gives for hiding the action is a closed sentence with no placeholder in it, so that no host, version or link
	// can be in the log: eight reasons, and each is `LogInfo("approval: not offered: <words>");` and nothing else
	offeredBody := definitionBody(t, "AppController.cpp", controller, "bool AppController::ApprovalOfferedIn(const approval::Network& network) {")
	reasons := regexp.MustCompile(`LogInfo\("approval: not offered:[^;]*;`).FindAllString(offeredBody, -1)
	if len(reasons) != 8 {
		t.Errorf("ApprovalOfferedIn gives %d reasons for hiding the action, want 8 (no network a request can name, elevated, signed in, page not live, page lists no version for the network, another program's scheme, an unsafe path, a registry that cannot be read)", len(reasons))
	}
	for _, reason := range reasons {
		if !regexp.MustCompile(`^LogInfo\("approval: not offered: [A-Za-z][A-Za-z' :/]*"\);$`).MatchString(reason) {
			t.Errorf("a reason for hiding the action is not a closed sentence without placeholders: %s", reason)
		}
	}
	offered := definitionBody(t, "AppController.cpp", controller, "bool AppController::ApprovalOfferedIn(const approval::Network& network) {")
	signOutRequireInOrder(t, "AppController::ApprovalOfferedIn", offered,
		regexp.QuoteMeta("if (!config::kBrowserApprovalEnabled) return false;"),
		regexp.QuoteMeta("if (network.kind == approval::NetworkKind::None) {"),
		regexp.QuoteMeta("if (ProcessIsElevated()) {"),
		regexp.QuoteMeta("if (ApprovalContext().signedIn) {"),
		regexp.QuoteMeta("StartApprovalPageCheck();"),
		regexp.QuoteMeta("if (ApprovalPageState() != ApprovalPage::Live) {"),
		regexp.QuoteMeta("if (!ApprovalPageLists(approval::ContractFor(network.kind))) {"),
		regexp.QuoteMeta("switch (ProtocolHandlerStatus()) {"),
		regexp.QuoteMeta("case ProtocolHandlerState::Ours:"),
		regexp.QuoteMeta("case ProtocolHandlerState::Absent:"),
		regexp.QuoteMeta("return true;"),
		regexp.QuoteMeta("case ProtocolHandlerState::Foreign:"),
		regexp.QuoteMeta("case ProtocolHandlerState::Unsafe:"),
		regexp.QuoteMeta("case ProtocolHandlerState::Failed:"))

	begin := definitionBody(t, "AppController.cpp", controller, "std::optional<approval::Slot::View> AppController::BeginApproval() {")
	signOutRequireInOrder(t, "AppController::BeginApproval", begin,
		regexp.QuoteMeta("const approval::Clocks now = approval::Now();"),
		regexp.QuoteMeta("approval_.Peek(now)"),
		regexp.QuoteMeta("if (approval_.Flow() == sdk_.CurrentBrowserFlow() && approval_.Reopenable(now)) return live;"),
		regexp.QuoteMeta("CancelApproval(approval::ClearReason::Superseded);"),
		regexp.QuoteMeta("const approval::Network network = sdk_.CurrentApprovalNetwork();"),
		regexp.QuoteMeta("if (!ApprovalOfferedIn(network)) return std::nullopt;"),
		regexp.QuoteMeta("if (!EnsureProtocolHandler()) {"),
		regexp.QuoteMeta("approval::FreshKeyPair(approval::NewKeyPair)"),
		regexp.QuoteMeta("params.publicKey = std::move(keys->publicKey);"),
		regexp.QuoteMeta("params.secretKey = std::move(keys->secretKey);"),
		regexp.QuoteMeta("params.api = network.api;"),
		regexp.QuoteMeta("params.now = approval::Now();"),
		regexp.QuoteMeta("params.flow = sdk_.BeginBrowserApproval();"),
		regexp.QuoteMeta("approval_.Begin(std::move(params), RandomBytes)"),
		regexp.QuoteMeta("approvalTimer_.Start();"),
		regexp.QuoteMeta(`LogInfo("approval: an attempt started (version {})", approval::ContractFor(network.kind));`),
		regexp.QuoteMeta("return view;"))
	// the network is read once, and the one read decides both whether the action is offered and what the request names: an attempt
	// cannot be offered for one network and name another (the official network's code would go to the other one's server)
	if strings.Contains(begin, "ApprovalOffered()") || strings.Count(begin, "CurrentApprovalNetwork()") != 1 || strings.Count(begin, "network.api") != 1 {
		t.Error("BeginApproval reads the network more than once, or does not take what the request names from the network it decided on")
	}
	if count := strings.Count(controller, "sdk_.CurrentApprovalNetwork()"); count != 3 { // ApprovalOffered, StartApprovalCheck, BeginApproval
		t.Errorf("AppController.cpp reads the approval's network %d times; ApprovalOffered, StartApprovalCheck and BeginApproval do, once each", count)
	}
	if count := strings.Count(controller, "ApprovalOfferedIn("); count != 3 { // the definition, ApprovalOffered, BeginApproval
		t.Errorf("AppController.cpp names ApprovalOfferedIn %d times; its definition, ApprovalOffered and BeginApproval are all that may", count)
	}
	if count := strings.Count(controller, "sdk_.BeginBrowserApproval()"); count != 1 {
		t.Errorf("AppController.cpp supersedes the other flows %d times; BeginApproval does, once, after everything that can still fail", count)
	}

	// the request is the page's build constant and the attempt's own values, built by the model
	provideRequire(t, "AppController.cpp", controller, "return approval::BuildRequestUrl(config::kApprovalPageUrl, view);")
	for name, code := range authLinkSourceFiles(t) {
		if strings.Contains(code, "kApprovalPageUrl") && name != "App/Config.h" && name != "App/AppController.cpp" {
			t.Errorf("%s names the approval page's url: the controller builds the request from the build constant, and nothing else opens it", name)
		}
		if strings.Contains(code, "BuildRequestUrl(") && name != "App/UrIoApproval.h" && name != "App/AppController.cpp" {
			t.Errorf("%s builds the approval request: only the model's BuildRequestUrl does, through AppController::ApprovalRequestUrl", name)
		}
	}

	// the page check is begun only where the action could be offered, and when a person is looking at a signed-out window:
	// that is when the network is likely to be up, and a check made at launch (an autostart at logon, with the network
	// still coming up) would keep the action hidden for the whole run
	start := definitionBody(t, "AppController.cpp", controller, "void AppController::Start() {")
	quitForbid(t, "AppController::Start", start, "the page check is begun when a signed-out window is shown, not when the app launches",
		"StartApprovalPageCheck(", "StartApprovalCheck(")
	showWindow := definitionBody(t, "AppController.cpp", controller, "void AppController::ShowWindowImpl(const POINT* anchor) {")
	signOutRequireInOrder(t, "AppController::ShowWindowImpl", showWindow,
		regexp.QuoteMeta("windowShown_ = true;"),
		regexp.QuoteMeta("if (!ApprovalContext().signedIn) StartApprovalCheck();"),
		regexp.QuoteMeta("window_.Activate();"))
	// ... and when the app signs out, or is switched to another server, under a window that is already shown: no window show
	// follows those, so a person who started signed in and then clicked Sign in with browser would meet a sheet that asked
	// while the check was still to begin (it asks once, when it is built) and has only the paste flow to offer. The statement
	// follows the end of the attempt's own hooks directly, and only a pushed LoggedOut with the window shown begins it: a launch
	// or an autostart that shows no window begins nothing.
	onAuth := definitionBody(t, "AppController.cpp", controller, "void AppController::OnAuthState(AuthState state, const std::string& error) {")
	signOutRequireInOrder(t, "AppController::OnAuthState", onAuth,
		`CancelApproval\(approval::ClearReason::Superseded\);\s*\}\s*if \(state == AuthState::LoggedOut && windowShown_\) StartApprovalCheck\(\);\s*UpdateTray\(\);`)
	signOutRequireInOrder(t, "AppController::StartApprovalCheck",
		definitionBody(t, "AppController.cpp", controller, "void AppController::StartApprovalCheck() {"),
		regexp.QuoteMeta("if constexpr (config::kBrowserApprovalEnabled) {"),
		regexp.QuoteMeta("if (sdk_.CurrentApprovalNetwork().kind != approval::NetworkKind::None && !ProcessIsElevated()) StartApprovalPageCheck();"))
	if count := strings.Count(controller, "StartApprovalCheck("); count != 3 { // the calls in ShowWindowImpl and OnAuthState, the definition
		t.Errorf("AppController.cpp names StartApprovalCheck %d times; ShowWindowImpl and OnAuthState call it once each and it is defined once", count)
	}
	for name, code := range authLinkSourceFiles(t) {
		if strings.Contains(code, "StartApprovalPageCheck(") && name != "App/AppController.cpp" && name != "App/ApprovalCheck.cpp" && name != "App/ApprovalCheck.h" {
			t.Errorf("%s starts the approval page check: AppController does, when the action could be offered", name)
		}
		// the snapshot of the network is the SDK host's, and the controller is the one that asks it
		if strings.Contains(code, "CurrentApprovalNetwork(") && name != "App/AppController.cpp" && name != "App/SdkHost.cpp" && name != "App/SdkHost.h" {
			t.Errorf("%s asks for the approval's network: AppController does, and the sheet asks the controller (ApprovalOffered)", name)
		}
		// the request names a network only through the attempt, and only the controller says which: the host of a request is never
		// taken from a name, a link or a server in any other file
		if strings.Contains(code, "params.api") && name != "App/AppController.cpp" && name != "App/UrIoApproval.h" {
			t.Errorf("%s sets or reads the attempt's api: AppController::BeginApproval hands it to the model, and the model keeps it", name)
		}
	}
}

// The contract says when a second click re-opens the same request (section 5, rule 1), and the model's constant is what it
// says: the page refuses a request more than 240 s old, so the window is a margin inside that, and later the click starts a
// new attempt. BeginApproval's use of it is pinned whole in ur_io_approval_exact_test.go, and the arithmetic in the spec.
func TestApprovalContractSaysWhenASecondClickReopens(t *testing.T) {
	data, err := os.ReadFile(filepath.Join(repositoryRoot(t), "docs", "app-approval-contract.md"))
	if err != nil {
		t.Fatal(err)
	}
	provideRequire(t, "section 5, rule 1 of the approval contract", authLinkSquash(lf(string(data))),
		"a second click re-opens the same request instead of starting another, but only while the page would still accept it "+
			"(200 s after `ts`, inside the page's 240 s window); later it starts a new attempt, which replaces the old one.")
	provideRequire(t, "UrIoApproval.h", stripComments(lf(readAppSource(t, "UrIoApproval.h"))),
		"inline constexpr std::chrono::seconds kReopenWindow{200};", "static_assert(kReopenWindow < kLife);")
}

// "Continue in browser" is off in this build, and the origin of the page is a build constant that no project file
// of this repository can change. The macro that points a local build at another page is defined on a compiler's
// command line by whoever runs it, never here.
func TestBrowserApprovalIsOffByDefaultAndItsOriginIsABuildConstant(t *testing.T) {
	config := stripComments(lf(readAppSource(t, "Config.h")))
	provideRequire(t, "Config.h", config,
		"inline constexpr bool kBrowserApprovalEnabled = false;",
		"#if defined(URN_APPROVAL_TEST_ORIGIN)", "#define URN_APPROVAL_ORIGIN URN_APPROVAL_TEST_ORIGIN",
		`#define URN_APPROVAL_ORIGIN "https://beta.app.ur.network"`,
		`inline constexpr char kApprovalPageUrl[] = URN_APPROVAL_ORIGIN "/login-app";`,
		`inline constexpr char kApprovalDescriptorUrl[] = URN_APPROVAL_ORIGIN "/approval-contract.json";`,
		"#undef URN_APPROVAL_ORIGIN",
		"inline constexpr std::string_view kApprovalHost = HostOfUrl(kApprovalPageUrl);",
		`static_assert(std::string_view(kApprovalPageUrl) == "https://beta.app.ur.network/login-app");`,
		`static_assert(std::string_view(kApprovalDescriptorUrl) == "https://beta.app.ur.network/approval-contract.json");`)
	if strings.Count(config, "kBrowserApprovalEnabled") != 1 {
		t.Error("Config.h names kBrowserApprovalEnabled more than once; it is one constant")
	}
	// A build that has the override says so: true only on its branch, false on the real page's. An environment variable can
	// carry the override into a build nobody meant it for, and the app's log at start is where that shows.
	signOutRequireInOrder(t, "Config.h", config,
		`#if defined\(URN_APPROVAL_TEST_ORIGIN\)\s*#define URN_APPROVAL_ORIGIN URN_APPROVAL_TEST_ORIGIN\s*inline constexpr bool kApprovalOriginIsATest = true;\s*#else\s*#define URN_APPROVAL_ORIGIN "https://beta\.app\.ur\.network"\s*inline constexpr bool kApprovalOriginIsATest = false;\s*#endif`,
		regexp.QuoteMeta("static_assert(!kApprovalOriginIsATest);"))
	if strings.Count(config, "kApprovalOriginIsATest") != 3 { // each branch sets it, and the real page's is asserted false
		t.Error("Config.h names kApprovalOriginIsATest other than where it is set on each branch and asserted false for the real page")
	}
	// ... and the controller says it, once, at start: one closed line with the host only, never the link
	appStart := definitionBody(t, "AppController.cpp", urIoController(t), "void AppController::Start() {")
	signOutRequireInOrder(t, "AppController::Start", appStart,
		`UpdateTray\(\);\s*if constexpr \(config::kApprovalOriginIsATest\) \{\s*LogWarn\("approval: this build talks to a test origin: \{\}", config::kApprovalHost\);\s*\}\s*LogInfo\("app: started"\);`)
	for name, code := range authLinkSourceFiles(t) {
		if strings.Contains(code, "kApprovalOriginIsATest") && name != "App/Config.h" && name != "App/AppController.cpp" {
			t.Errorf("%s names kApprovalOriginIsATest: Config.h sets it and AppController::Start reports it, and nothing else asks", name)
		}
	}
	// the flag is read by the controller (and, once it is written, the sheet) and by nothing else
	for name, code := range authLinkSourceFiles(t) {
		if strings.Contains(code, "kBrowserApprovalEnabled") && !map[string]bool{
			"App/Config.h": true, "App/AppController.cpp": true, "App/BrowserApprovalSheet.cpp": true, "App/LoginPage.cpp": true,
		}[name] {
			t.Errorf("%s reads kBrowserApprovalEnabled: the controller decides whether the action is offered", name)
		}
	}
	// no file of the repository that builds, packages or releases the app defines the override
	root := repositoryRoot(t)
	buildFiles := map[string]bool{
		".vcxproj": true, ".props": true, ".targets": true, ".sln": true, ".yml": true, ".yaml": true, ".ps1": true,
		".cmd": true, ".bat": true, ".sh": true, ".wxs": true, ".wixproj": true, ".json": true, ".mk": true, ".cmake": true,
	}
	skipped := map[string]bool{".git": true, ".localstate-verify": true, ".local-deps-new": true, "node_modules": true, "x64": true, "ARM64": true,
		"obj": true, "build": true, "tests": true, "third_party": true, "Generated Files": true}
	err := filepath.WalkDir(root, func(path string, entry os.DirEntry, walkErr error) error {
		if walkErr != nil {
			return walkErr
		}
		if entry.IsDir() {
			if path != root && (skipped[entry.Name()] || strings.HasPrefix(entry.Name(), ".localstate")) {
				return filepath.SkipDir
			}
			return nil
		}
		if !buildFiles[strings.ToLower(filepath.Ext(path))] {
			return nil
		}
		data, err := os.ReadFile(path)
		if err != nil {
			return err
		}
		if strings.Contains(string(data), "URN_APPROVAL_TEST_ORIGIN") {
			relative, _ := filepath.Rel(root, path)
			t.Errorf("%s defines or names URN_APPROVAL_TEST_ORIGIN: it is defined on a compiler's command line by whoever runs a local end-to-end test, never in a file of the repository", filepath.ToSlash(relative))
		}
		return nil
	})
	if err != nil {
		t.Fatal(err)
	}
	for name, code := range authLinkSourceFiles(t) {
		if strings.Contains(code, "URN_APPROVAL_TEST_ORIGIN") && name != "App/Config.h" {
			t.Errorf("%s names URN_APPROVAL_TEST_ORIGIN: Config.h alone does", name)
		}
	}
}

// The page check is one GET of the descriptor build constant on a worker thread: three seconds, no redirect, one answer
// for the run whichever way it goes, judged by the descriptor's own reader (ApprovalDescriptor.h), and the elevated-process
// guard fails closed.
func TestApprovalCheckIsOneBoundedRequest(t *testing.T) {
	raw := lf(readAppSource(t, "ApprovalCheck.cpp"))
	check := stripComments(raw)
	if strings.Contains(raw, `#include "pch.h"`) || strings.Contains(check, "winrt::") {
		t.Error("ApprovalCheck.cpp includes pch.h or uses WinRT: it is plain Win32 and built without the precompiled header")
	}
	provideRequire(t, "ApprovalCheck.cpp", check,
		"Widen(config::kApprovalDescriptorUrl)",
		"WINHTTP_OPTION_REDIRECT_POLICY_NEVER",
		"::WinHttpSetTimeouts(session.h, static_cast<int>(budgetMs), static_cast<int>(budgetMs), static_cast<int>(budgetMs),",
		"std::chrono::steady_clock::now() - started > approval::kCheckBudget",
		"status != 200",
		"WINHTTP_QUERY_CONTENT_TYPE",
		"body.size() > approval::kMaxDescriptorBytes",
		"approval::ReadDescriptor(Narrow(contentType), body)",
		"g_state.compare_exchange_strong(expected, static_cast<int>(ApprovalPage::Checking))",
		"std::thread(Run).detach();",
		"g_state.store(static_cast<int>(live ? ApprovalPage::Live : ApprovalPage::Down), std::memory_order_release);",
		"g_contracts.store(live ? listed : 0U, std::memory_order_release);", "std::atomic<std::uint32_t> g_contracts{0};",
		`WinHttpAddRequestHeaders(request.h, L"Accept: application/json"`,
		// every WinHTTP handle is closed when it goes, once, and is not copied (the functions are pinned whole in ur_io_approval_exact_test.go)
		"Handle(const Handle&) = delete;", "Handle& operator=(const Handle&) = delete;", "if (h) ::WinHttpCloseHandle(h);")
	if count := strings.Count(check, "WinHttpOpenRequest("); count != 1 {
		t.Errorf("ApprovalCheck.cpp opens %d requests; the check is one GET", count)
	}
	quitForbid(t, "ApprovalCheck.cpp", check, "the check is one GET of the descriptor constant: no other url, no body sent, no retry",
		`L"http`, `"http`, `L"POST"`, `L"PUT"`, "WinHttpSendRequest(request.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, \"", "WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS",
		"WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP")
	// the page is where the constant says and its certificate is checked as WinHTTP checks every one: the check can only
	// show or hide the action, but a probe that accepts any certificate is a probe anyone on the path can answer for the page
	quitForbid(t, "ApprovalCheck.cpp", check, "the page's certificate is validated as WinHTTP does by default, with no flag turned off",
		"SECURITY_FLAG_IGNORE", "WINHTTP_OPTION_SECURITY_FLAGS")
	// success and failure alike are kept: nothing sets the state back to NotChecked
	if strings.Contains(check, "ApprovalPage::NotChecked)") && strings.Count(check, "ApprovalPage::NotChecked") != 2 {
		t.Error("ApprovalCheck.cpp sets the state back to NotChecked: the answer is cached for the run, a failure too")
	}
	// the versions the page lists are published before the state that says Live, so a reader that sees Live sees them; a page that
	// is not Live lists nothing; and the one reader of the list asks the descriptor's own Lists
	provideRequireOrder(t, "ApprovalCheck.cpp", definitionBody(t, "ApprovalCheck.cpp", check, "void Run() {"), "g_contracts.store(", "g_state.store(")
	lists := definitionBody(t, "ApprovalCheck.cpp", check, "bool ApprovalPageLists(std::int64_t contract) {")
	signOutRequireInOrder(t, "ApprovalPageLists", lists,
		regexp.QuoteMeta("if (ApprovalPageState() != ApprovalPage::Live) return false;"),
		regexp.QuoteMeta("answer.contracts = g_contracts.load(std::memory_order_acquire);"),
		regexp.QuoteMeta("return answer.Lists(contract);"))
	if count := strings.Count(check, "g_contracts"); count != 3 { // the definition, Run's store, ApprovalPageLists' load
		t.Errorf("ApprovalCheck.cpp names g_contracts %d times; the definition, Run's store and ApprovalPageLists' load are all that may", count)
	}
	for name, code := range authLinkSourceFiles(t) {
		if strings.Contains(code, "ApprovalPageLists(") && name != "App/AppController.cpp" && name != "App/ApprovalCheck.cpp" && name != "App/ApprovalCheck.h" {
			t.Errorf("%s asks what the page lists: AppController::ApprovalOfferedIn does", name)
		}
	}
	elevated := definitionBody(t, "ApprovalCheck.cpp", check, "bool ProcessIsElevated() {")
	signOutRequireInOrder(t, "ProcessIsElevated", elevated,
		regexp.QuoteMeta("if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) return true;"),
		regexp.QuoteMeta("::GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size)"),
		regexp.QuoteMeta("return !read || elevation.TokenIsElevated != 0;"))
	// closed words in the log
	for _, leak := range authLinkLogLeaks(check) {
		t.Errorf("ApprovalCheck.cpp %s", leak)
	}
	for _, line := range regexp.MustCompile(`Log(?:Info|Warn|Error)\(([^;]*)\);`).FindAllStringSubmatch(check, -1) {
		if strings.Contains(authLinkStringLiteral.ReplaceAllString(line[1], `""`), "body") {
			t.Errorf("ApprovalCheck.cpp logs the response: %s", line[0])
		}
	}
}

// The hooks the controller calls on the login page are public, and hand the browser sign-in sheet what arrived: the page
// itself redeems nothing, signs nobody in, registers no device and changes no step for an approval. The sheet is shown
// from one place, which holds the single-sheet gate. An approved answer finds the sheet that is open and gives it the
// code; when none is (a hide to the tray swept it away, and the attempt outlived it) it opens one on the redeem, after
// closing any sheet of another kind that is in the way and waiting for that one to be gone.
func TestApprovalHooksHandTheSheetWhatArrives(t *testing.T) {
	header := stripComments(lf(readAppSource(t, "LoginPage.h")))
	public := header[:strings.Index(header, " private:")]
	for _, declaration := range []string{
		"void OnApprovedCode(std::string code);", "void OnApprovalDenied();", "void OnApprovalExpired();", "void ShowStrayNotice();",
	} {
		if !strings.Contains(public, declaration) {
			t.Errorf("LoginPage.h does not declare %s in its public part", declaration)
		}
	}
	provideRequire(t, "LoginPage.h", header,
		"std::shared_ptr<urnw::BrowserApprovalSheet> approvalSheet_;", "winrt::fire_and_forget ShowApprovalSheet(std::string approvedCode);",
		"void ContinueApprovedCode();", "approval::NeutralLine strayLine_;")

	login := stripComments(lf(readAppSource(t, "LoginPage.cpp")))
	// none of them does the sheet's work
	for _, signature := range []string{
		"void LoginPage::OnApprovedCode(std::string code) {", "void LoginPage::ContinueApprovedCode() {",
		"void LoginPage::OnApprovalDenied() {", "void LoginPage::OnApprovalExpired() {", "void LoginPage::ShowStrayNotice() {",
	} {
		quitForbid(t, signature, definitionBody(t, "LoginPage.cpp", login, signature),
			"a hook hands the sheet what arrived: it signs nobody in, redeems nothing, registers nothing and changes no step",
			"Sdk()", "SignInWithAuthCode(", "LoginWithCode(", "RedeemApprovedCode(", "ConfirmApprovedNetwork(", "DiscardApprovedNetwork(",
			"SetInitialLoginError(", "ShowLoginStep(", "ResetToInitialStep(", "ShowErrorOnCurrentStep(", "SetWalletSignInEnabled(",
			"ShowAsync(", "LaunchUriAsync(", "Dialog().Hide()")
	}

	// the answer: the open sheet first; otherwise the code waits, wiped when it is replaced, used or dropped
	approved := definitionBody(t, "LoginPage.cpp", login, "void LoginPage::OnApprovedCode(std::string code) {")
	signOutRequireInOrder(t, "LoginPage::OnApprovedCode", approved,
		regexp.QuoteMeta("if (approvalSheet_ && approvalSheet_->Open()) {"),
		regexp.QuoteMeta("approvalSheet_->BeginRedeem(std::move(code));"),
		regexp.QuoteMeta("return;"),
		regexp.QuoteMeta("approval::Wipe(pendingApprovedCode_);"),
		regexp.QuoteMeta("pendingApprovedCode_ = std::move(code);"),
		regexp.QuoteMeta("if (w_.sheetOpen()) w_.CloseSheetsForHide();"),
		regexp.QuoteMeta("ContinueApprovedCode();"))
	waiting := definitionBody(t, "LoginPage.cpp", login, "void LoginPage::ContinueApprovedCode() {")
	signOutRequireInOrder(t, "LoginPage::ContinueApprovedCode", waiting,
		regexp.QuoteMeta("if (w_.sheetOpen() && ++approvedCodeWaits_ <= kMaxWaits) {"),
		regexp.QuoteMeta("approvedCodeTimer_.Start();"),
		regexp.QuoteMeta("approvedCodeTimer_.Stop();"),
		regexp.QuoteMeta("std::string code = std::move(pendingApprovedCode_);"),
		regexp.QuoteMeta("pendingApprovedCode_.clear();"),
		regexp.QuoteMeta("if (w_.sheetOpen()) {"),
		regexp.QuoteMeta("approval::Wipe(code);"),
		regexp.QuoteMeta(`snackbar_->Show(Loc("auth_code_error"), InfoBarSeverity::Informational, 15000);`),
		regexp.QuoteMeta("ShowApprovalSheet(std::move(code));"))
	provideRequire(t, "LoginPage.cpp", login,
		"approvedCodeTimer_ = queue.CreateTimer();", "approvedCodeTimer_.Interval(std::chrono::milliseconds(50));",
		"approvedCodeTimer_.IsRepeating(true);", "self->login().ContinueApprovedCode();")
	// ... and each tick of that timer does one thing, for a page that is still there
	tick := regexp.MustCompile(`(?s)approvedCodeTimer_\.Tick\(\[weak = w_\.get_weak\(\)\]\(auto const&, auto const&\) \{(.*?)\}\);`).FindStringSubmatch(login)
	if tick == nil {
		t.Fatal("LoginPage.cpp no longer registers the approved code's timer as approvedCodeTimer_.Tick([weak = w_.get_weak()](...) {...}); update this contract")
	}
	if got, want := authLinkSquash(tick[1]), "if (auto self = weak.get()) self->login().ContinueApprovedCode();"; got != want {
		t.Errorf("the tick of the approved code's timer is %q, want %q", got, want)
	}
	destructor := definitionBody(t, "LoginPage.cpp", login, "LoginPage::~LoginPage() {")
	provideRequire(t, "LoginPage::~LoginPage", destructor, "if (approvedCodeTimer_) approvedCodeTimer_.Stop();", "approval::Wipe(pendingApprovedCode_);")
	// the other four ends of the sheet
	provideRequire(t, "LoginPage::OnApprovalDenied", definitionBody(t, "LoginPage.cpp", login, "void LoginPage::OnApprovalDenied() {"),
		"if (approvalSheet_) approvalSheet_->ShowDenied();")
	provideRequire(t, "LoginPage::OnApprovalExpired", definitionBody(t, "LoginPage.cpp", login, "void LoginPage::OnApprovalExpired() {"),
		"if (approvalSheet_) approvalSheet_->ShowTimedOut();")
	stray := definitionBody(t, "LoginPage.cpp", login, "void LoginPage::ShowStrayNotice() {")
	signOutRequireInOrder(t, "LoginPage::ShowStrayNotice", stray,
		regexp.QuoteMeta("w_.LoginRoot().Visibility() != Visibility::Visible"),
		regexp.QuoteMeta("if (!urnw::App().WindowShown()) return;"),
		regexp.QuoteMeta("if (!strayLine_.Allow(std::chrono::steady_clock::now())) return;"),
		regexp.QuoteMeta(`snackbar_->Show(Loc("auth_code_error"), InfoBarSeverity::Informational, 15000);`))

	// the one place the sheet is shown, with its gate, and the click that opens it is thin
	host := definitionBody(t, "LoginPage.cpp", login, "winrt::fire_and_forget LoginPage::ShowApprovalSheet(std::string approvedCode) {")
	signOutRequireInOrder(t, "LoginPage::ShowApprovalSheet", host,
		regexp.QuoteMeta("if (w_.sheetOpen()) {"),
		regexp.QuoteMeta("auto self = w_.get_strong();"),
		regexp.QuoteMeta("w_.SetSheetOpen(true);"),
		regexp.QuoteMeta("approvalSheet_ = urnw::BrowserApprovalSheet::Create(self->Content().XamlRoot());"),
		regexp.QuoteMeta("approvalSheet_->BeginRedeem(std::move(approvedCode));"),
		regexp.QuoteMeta("co_await approvalSheet_->Dialog().ShowAsync();"),
		regexp.QuoteMeta("pasted = approvalSheet_->TakePastedCode();"),
		regexp.QuoteMeta("approvalSheet_.reset();"),
		regexp.QuoteMeta("w_.SetSheetOpen(false);"),
		// an answer that came while this sheet was closing is waiting for the gate that has just been cleared, and goes on at
		// once: the timer that polls for the gate is the backstop, not the way it usually goes
		regexp.QuoteMeta("if (!pendingApprovedCode_.empty()) ContinueApprovedCode();"),
		regexp.QuoteMeta("SignInWithAuthCode(std::move(pasted));"))
	for pattern, want := range map[string]int{
		"BrowserApprovalSheet::Create(":        1,
		"approvalSheet_->Dialog().ShowAsync()": 1,
		"ShowApprovalSheet(":                   3, // the click, the answer that finds the sheet gone, the definition
		"ContinueApprovedCode(":                4, // the answer, the sheet's own closing, the timer's tick, the definition
	} {
		if count := strings.Count(login, pattern); count != want {
			t.Errorf("LoginPage.cpp has %d of %s, want %d: the sheet is shown from ShowApprovalSheet, which the click and the answer call", count, pattern, want)
		}
	}
	requireExactDefinition(t, "LoginPage.cpp", login, "winrt::fire_and_forget LoginPage::OnSignInWithBrowser(", `
winrt::fire_and_forget LoginPage::OnSignInWithBrowser(IInspectable const&,
                                                      RoutedEventArgs const&) {
  SetInitialLoginError(hstring());
  return ShowApprovalSheet(std::string());
}`)
}

// The stubs the controller stage left for the sheet are gone, and no marker of one is left anywhere in the sources.
func TestNoApprovalStubMarkersRemain(t *testing.T) {
	root := filepath.Join(repositoryRoot(t), "app", "src")
	skipped := map[string]bool{"Generated Files": true, "x64": true, "ARM64": true, "bin": true, "obj": true}
	text := map[string]bool{
		".cpp": true, ".h": true, ".hpp": true, ".c": true, ".xaml": true, ".idl": true, ".rc": true, ".vcxproj": true,
		".props": true, ".targets": true, ".manifest": true, ".md": true, ".txt": true, ".ps1": true, ".json": true,
	}
	checked := 0
	err := filepath.WalkDir(root, func(path string, entry os.DirEntry, walkErr error) error {
		if walkErr != nil {
			return walkErr
		}
		if entry.IsDir() {
			if path != root && skipped[entry.Name()] {
				return filepath.SkipDir
			}
			return nil
		}
		if !text[strings.ToLower(filepath.Ext(path))] {
			return nil
		}
		data, err := os.ReadFile(path)
		if err != nil {
			return err
		}
		checked++
		if strings.Contains(string(data), "STUB(") {
			relative, _ := filepath.Rel(root, path)
			t.Errorf("%s still has a STUB( marker: the sheet stage replaced every stub of the controller stage", filepath.ToSlash(relative))
		}
		return nil
	})
	if err != nil {
		t.Fatal(err)
	}
	if checked < 200 {
		t.Fatalf("only %d sources of app/src were read; update this contract", checked)
	}
}

// What the sheet closes and keeps. Its Closed handler is where the person closing it is told from a hide to the tray:
// the hide counted itself (AppController::WindowHides) before it swept the sheets, so a sheet whose count has moved was
// swept, and the attempt is kept for the browser's answer, and anything else cancels it. That is decided by the count
// and not by where the window is when the event comes, which is after the popup's closing transition. A credential
// redeemed for a confirm that never came is discarded there. The sheet closes itself only from Finish, and the redeem and
// the confirm are the sheet's alone.
func TestApprovalSheetClosesAsTheContractSays(t *testing.T) {
	sheet := stripComments(lf(readAppSource(t, "BrowserApprovalSheet.cpp")))
	closed := definitionBody(t, "BrowserApprovalSheet.cpp", sheet, "void BrowserApprovalSheet::OnClosed() {")
	signOutRequireInOrder(t, "BrowserApprovalSheet::OnClosed", closed,
		regexp.QuoteMeta("closed_ = true;"),
		regexp.QuoteMeta(`pasteBox_.Password(L"");`),
		regexp.QuoteMeta("approval::Wipe(link_);"),
		regexp.QuoteMeta("if (page_ == Page::Confirm && !confirmed_) {"),
		regexp.QuoteMeta("Sdk().DiscardApprovedNetwork();"),
		regexp.QuoteMeta("if (urnw::App().WindowHides() == hides_) urnw::App().CancelApproval(approval::ClearReason::Cancel);"))
	provideRequire(t, "BrowserApprovalSheet.cpp", sheet,
		"dialog_.Closed([weak = weak_from_this()](auto const&, auto const&) {", "if (auto self = weak.lock()) self->OnClosed();")
	// the sheet takes the count when it is built, and is Open only while the count has not moved
	provideRequire(t, "BrowserApprovalSheet::Build", definitionBody(t, "BrowserApprovalSheet.cpp", sheet, "void BrowserApprovalSheet::Build(XamlRoot const& root) {"),
		"hides_ = urnw::App().WindowHides();")
	provideRequire(t, "BrowserApprovalSheet::Open", definitionBody(t, "BrowserApprovalSheet.cpp", sheet, "bool BrowserApprovalSheet::Open() const {"),
		"return !closed_ && urnw::App().WindowHides() == hides_;")
	quitForbid(t, "BrowserApprovalSheet.cpp", sheet, "the window's state when the Closed event comes says nothing about what closed the sheet",
		"WindowShown()")
	for pattern, want := range map[string]int{
		"WindowHides()":                   3, // Build's snapshot, Open, and the Closed handler
		"CancelApproval(":                 2, // the Closed handler, and a start that cannot open its request
		"dialog_.Hide();":                 1, // Finish
		"Sdk().RedeemApprovedCode(":       1,
		"Sdk().ConfirmApprovedNetwork(":   1,
		"BeginApproval()":                 1,
		"ApprovalOffered()":               1,
		"ApprovalRequestUrl(":             1,
		"SanitizeExternalDisplayText(":    1,
		"Sdk().DiscardApprovedNetwork();": 4, // closed unconfirmed, an answer for a sheet that is gone, one for a closed sheet, a name with nothing to show
	} {
		if count := strings.Count(sheet, pattern); count != want {
			t.Errorf("BrowserApprovalSheet.cpp has %d of %s, want %d", count, pattern, want)
		}
	}
	finish := definitionBody(t, "BrowserApprovalSheet.cpp", sheet, "void BrowserApprovalSheet::Finish() {")
	provideRequire(t, "BrowserApprovalSheet::Finish", finish, "if (!closed_) dialog_.Hide();")

	// the redeem: the code is wiped the moment the SDK has it, and the answer comes back to the UI thread
	redeem := definitionBody(t, "BrowserApprovalSheet.cpp", sheet, "void BrowserApprovalSheet::BeginRedeem(std::string code) {")
	signOutRequireInOrder(t, "BrowserApprovalSheet::BeginRedeem", redeem,
		regexp.QuoteMeta("confirmed_ = false;"),
		regexp.QuoteMeta("ShowPage(Page::SigningIn);"),
		regexp.QuoteMeta("Sdk().RedeemApprovedCode(code, [weak, queue](SdkHost::ApprovedNetwork answer) {"),
		regexp.QuoteMeta("queue.TryEnqueue("),
		regexp.QuoteMeta("self->OnRedeemed(std::move(answer));"),
		regexp.QuoteMeta("Sdk().DiscardApprovedNetwork();"),
		regexp.QuoteMeta("approval::Wipe(code);"))
	// what the person is shown before anything is registered: the name, filtered, and nothing else of the credential
	answered := definitionBody(t, "BrowserApprovalSheet.cpp", sheet, "void BrowserApprovalSheet::OnRedeemed(SdkHost::ApprovedNetwork answer) {")
	signOutRequireInOrder(t, "BrowserApprovalSheet::OnRedeemed", answered,
		regexp.QuoteMeta("if (closed_) {"),
		regexp.QuoteMeta("if (answer.ok) Sdk().DiscardApprovedNetwork();"),
		regexp.QuoteMeta("if (!answer.ok) {"),
		regexp.QuoteMeta("SdkHost::ApprovedNetwork::Failure::Transport"),
		regexp.QuoteMeta("const std::string name = kit::SanitizeExternalDisplayText(answer.networkName);"),
		regexp.QuoteMeta("if (name.empty()) {"),
		regexp.QuoteMeta("ShowConfirm(name);"))
	// the yes: the credential leaves the hold only here, and only for a person who clicked the one button of that page
	confirm := definitionBody(t, "BrowserApprovalSheet.cpp", sheet, "void BrowserApprovalSheet::Confirm() {")
	signOutRequireInOrder(t, "BrowserApprovalSheet::Confirm", confirm,
		regexp.QuoteMeta("confirmed_ = true;"), regexp.QuoteMeta("Sdk().ConfirmApprovedNetwork("), regexp.QuoteMeta("self->OnConfirmed(result);"))
	primary := definitionBody(t, "BrowserApprovalSheet.cpp", sheet, "void BrowserApprovalSheet::OnPrimary() {")
	signOutRequireInOrder(t, "BrowserApprovalSheet::OnPrimary", primary,
		regexp.QuoteMeta("case Page::Confirm:"), regexp.QuoteMeta("Confirm();"), regexp.QuoteMeta("case Page::TimedOut:"), regexp.QuoteMeta("ShowPage(Page::Start);"))
	// ... and the person's click is the only way there. Confirm() has one caller (OnPrimary, for the Confirm page), OnPrimary()
	// has one caller (the dialog's primary-button handler, which cancels the close and asks it), and neither the answer of the
	// redeem nor the page that asks the question can reach the registration: the network a page's answer names is registered
	// only after "Sign in" on the Confirm page, never because the answer arrived. (A call added to OnRedeemed, to ShowConfirm
	// or to the handler is a way to register a network nobody agreed to join, and no other pin reads who calls these.)
	confirmCall := regexp.MustCompile(`(?:^|[^A-Za-z_])Confirm\(\);`)
	if count := len(confirmCall.FindAllString(sheet, -1)); count != 1 {
		t.Errorf("BrowserApprovalSheet.cpp calls Confirm() %d times; OnPrimary's case for the Confirm page is the only call", count)
	}
	if count := strings.Count(sheet, "OnPrimary();"); count != 1 {
		t.Errorf("BrowserApprovalSheet.cpp calls OnPrimary() %d times; the dialog's primary-button handler is the only call", count)
	}
	handler := regexp.MustCompile(`(?s)dialog_\.PrimaryButtonClick\(\[weak = weak_from_this\(\)\]\(.*?\{(.*?)\}\);`).FindStringSubmatch(sheet)
	if handler == nil {
		t.Fatal("BrowserApprovalSheet.cpp no longer registers the primary-button handler as dialog_.PrimaryButtonClick([weak = weak_from_this()](...) {...}); update this contract")
	}
	if got, want := authLinkSquash(handler[1]), "args.Cancel(true); if (auto self = weak.lock()) self->OnPrimary();"; got != want {
		t.Errorf("the primary-button handler is %q, want %q: it cancels the close and asks OnPrimary, and does nothing else", got, want)
	}
	// the paste flow's button is the other one that closes the sheet, and it hands the page nothing but the code in its field
	secondary := regexp.MustCompile(`(?s)dialog_\.SecondaryButtonClick\(\s*\[weak = weak_from_this\(\)\]\(.*?\{(.*?)\}\);`).FindStringSubmatch(sheet)
	if secondary == nil {
		t.Fatal("BrowserApprovalSheet.cpp no longer registers the secondary-button handler as dialog_.SecondaryButtonClick([weak = weak_from_this()](...) {...}); update this contract")
	}
	if got, want := authLinkSquash(secondary[1]), "if (auto self = weak.lock()) self->SubmitPastedCode();"; got != want {
		t.Errorf("the secondary-button handler is %q, want %q: Sign in with a pasted code takes the code from the field and does nothing else", got, want)
	}
	for _, signature := range []string{
		"void BrowserApprovalSheet::OnRedeemed(", "void BrowserApprovalSheet::ShowConfirm(", "void BrowserApprovalSheet::BeginRedeem(",
		"void BrowserApprovalSheet::ShowPage(", "void BrowserApprovalSheet::OnClosed(", "void BrowserApprovalSheet::OnConfirmed(",
	} {
		body := definitionBody(t, "BrowserApprovalSheet.cpp", sheet, signature)
		if confirmCall.MatchString(body) || strings.Contains(body, "OnPrimary();") || strings.Contains(body, "ConfirmApprovedNetwork(") {
			t.Errorf("%s asks for the yes itself: the yes is the person's click on the Confirm page", signature)
		}
	}
	// and nothing outside the sheet registers the held network: the controller hands a code on, the page hosts the sheet
	for name, code := range authLinkSourceFiles(t) {
		if name != "App/BrowserApprovalSheet.cpp" && name != "App/SdkHost.cpp" && name != "App/SdkHost.h" &&
			(strings.Contains(code, "ConfirmApprovedNetwork(") || strings.Contains(code, "RedeemApprovedCode(")) {
			t.Errorf("%s redeems or confirms an approved network: the sheet that shows the person which network they would join does, and nothing else", name)
		}
	}
	// no default button on the page that asks: a stray Enter is not the person's yes
	provideRequire(t, "BrowserApprovalSheet.cpp", sheet, `SetBar(dialog_, Loc("sign_in"), none, Loc("cancel"), ContentDialogButton::None);`)
	// the sheet holds a link and a pasted code, and no credential, key or state of its own
	header := stripComments(lf(readAppSource(t, "BrowserApprovalSheet.h")))
	members := regexp.MustCompile(`\bstd::string\s+(\w+_)\s*;`).FindAllStringSubmatch(header, -1)
	if len(members) != 2 || members[0][1] != "link_" || members[1][1] != "pasted_" {
		t.Errorf("BrowserApprovalSheet.h holds string members %v; the link and the pasted code are all it may keep", members)
	}
	for _, forbidden := range []string{"CheckCode", "check code", "checkCode", `Loc("code")`, "BCrypt", "state_"} {
		if strings.Contains(sheet, forbidden) || strings.Contains(header, forbidden) {
			t.Errorf("the sheet mentions %q: there is no check code, and the attempt's secrets are the controller's", forbidden)
		}
	}
}

// The sheet's controls do what their pages say: each button goes to the one method it is for (a button with no handler is a
// page that cannot be used), the field the person pastes a credential into does not show it, the paste flow's field gets the focus
// it has always had, and the sheet opens on its first page. The decisions of the pages are pinned whole in
// ur_io_approval_exact_test.go (with three of the builders); this is the wiring between a control and them, and
// TestApprovalSheetPagesHoldWhatTheyShow is what each page is made of.
func TestApprovalSheetHooksUpItsControls(t *testing.T) {
	sheet := stripComments(lf(readAppSource(t, "BrowserApprovalSheet.cpp")))
	for control, call := range map[string]string{
		"actionButton_": "self->OnAction();", "againButton_": "self->OnAction();", "copyButton_": "self->CopyLink();",
	} {
		if count := strings.Count(sheet, control+".Click("); count != 1 {
			t.Errorf("BrowserApprovalSheet.cpp registers %s.Click %d times, want 1", control, count)
		}
		click := regexp.MustCompile(`(?s)` + control + `\.Click\(\[weak = weak_from_this\(\)\]\(auto const&, auto const&\) \{(.*?)\}\);`).FindStringSubmatch(sheet)
		if click == nil {
			t.Errorf("BrowserApprovalSheet.cpp no longer registers %s.Click([weak = weak_from_this()](auto const&, auto const&) {...}); update this contract", control)
			continue
		}
		if got, want := authLinkSquash(click[1]), "if (auto self = weak.lock()) "+call; got != want {
			t.Errorf("the click of %s is %q, want %q: a button asks the one method it is for, and only for a sheet that is still there", control, got, want)
		}
	}
	opened := regexp.MustCompile(`(?s)dialog_\.Opened\(\[weak = weak_from_this\(\)\]\(auto const&, auto const&\) \{(.*?)\}\);`).FindStringSubmatch(sheet)
	if opened == nil {
		t.Fatal("BrowserApprovalSheet.cpp no longer registers dialog_.Opened([weak = weak_from_this()](auto const&, auto const&) {...}); update this contract")
	}
	if got, want := authLinkSquash(opened[1]),
		"auto self = weak.lock(); if (self && !self->approvalOffered_ && self->page_ == Page::Start) { self->pasteBox_.Focus(FocusState::Programmatic); }"; got != want {
		t.Errorf("the Opened handler is %q, want %q: the paste flow opens on its field, as it always has, and the approval's Start page does not take the focus", got, want)
	}
	build := definitionBody(t, "BrowserApprovalSheet.cpp", sheet, "void BrowserApprovalSheet::Build(XamlRoot const& root) {")
	signOutRequireInOrder(t, "BrowserApprovalSheet::Build", build,
		regexp.QuoteMeta("hides_ = urnw::App().WindowHides();"),
		regexp.QuoteMeta("dialog_.PrimaryButtonClick("),
		regexp.QuoteMeta("dialog_.SecondaryButtonClick("),
		regexp.QuoteMeta("dialog_.Opened("),
		regexp.QuoteMeta("dialog_.Closed("),
		regexp.QuoteMeta("ShowPage(Page::Start);"))
	provideRequire(t, "BrowserApprovalSheet.cpp", sheet, "pasteBox_ = PasswordBox();", "pasteBox_.PasswordRevealMode(PasswordRevealMode::Hidden);")
}

// The ordered targets of every `<panel>.Children().Append(<x>);` in a body: what a panel holds, in the order it holds it.
func urIoAppends(body, panel string) []string {
	var targets []string
	pattern := regexp.MustCompile(`\b` + regexp.QuoteMeta(panel) + `\.Children\(\)\.Append\(([^;]*)\);`)
	for _, match := range pattern.FindAllStringSubmatch(body, -1) {
		targets = append(targets, authLinkSquash(match[1]))
	}
	return targets
}

func urIoRequireAppends(t *testing.T, where, body, panel string, want ...string) {
	t.Helper()
	if got := urIoAppends(body, panel); strings.Join(got, " | ") != strings.Join(want, " | ") {
		t.Errorf("%s puts [%s] in %s, want [%s]", where, strings.Join(got, ", "), panel, strings.Join(want, ", "))
	}
}

// What each page of the sheet is made of, and that the dialog holds every page. The margins, alignments and sizes of the
// pages are free; which control is in which panel, and which panels are in the dialog, is not. A Confirm page that does not
// hold the network's name, or is not in the dialog at all, asks the person to say yes to nothing they can read; a Start page
// without its field loses the paste flow, the only live path of a build with the approval off; a Waiting page without its
// Copy button has no way to open the request by hand. The Confirm, signing-in and message builders are pinned whole in
// ur_io_approval_exact_test.go. The request link (the attempt's state and public key are in it) is read by the launcher and
// the clipboard and by nothing that draws or describes a control.
func TestApprovalSheetPagesHoldWhatTheyShow(t *testing.T) {
	sheet := urIoSheet(t)

	// the dialog holds the five pages, in a scroll viewer, over the window it opens on
	build := definitionBody(t, "BrowserApprovalSheet.cpp", sheet, "void BrowserApprovalSheet::Build(XamlRoot const& root) {")
	signOutRequireInOrder(t, "BrowserApprovalSheet::Build", build,
		regexp.QuoteMeta("dialog_ = ContentDialog();"),
		regexp.QuoteMeta("dialog_.XamlRoot(root);"),
		regexp.QuoteMeta("BuildStart();"),
		regexp.QuoteMeta("BuildWaiting();"),
		regexp.QuoteMeta("BuildSigningIn();"),
		regexp.QuoteMeta("BuildConfirm();"),
		regexp.QuoteMeta("BuildMessage();"),
		regexp.QuoteMeta("content.Children().Append("),
		regexp.QuoteMeta("scroll.Content(content);"),
		regexp.QuoteMeta("dialog_.Content(scroll);"))
	urIoRequireAppends(t, "BrowserApprovalSheet::Build", build, "content",
		"startPanel_", "waitingPanel_", "signingInPanel_", "confirmPanel_", "messagePanel_")

	// the Start page: the action beside the host it goes to, and the field. With the approval the action leads, "or" and the
	// paste instructions follow; without it the page is what it has always been, instructions first. The field is on both.
	start := definitionBody(t, "BrowserApprovalSheet.cpp", sheet, "void BrowserApprovalSheet::BuildStart() {")
	urIoRequireAppends(t, "BrowserApprovalSheet::BuildStart", start, "row", "actionButton_", "host")
	// the three things only the approval's page has, each under the plain condition: the accent on the action, the host beside
	// it, and the layout that leads with it. (A condition turned round would give the paste flow a host that is not where its
	// button goes, or the approval a page that leads with instructions.)
	signOutRequireInOrder(t, "BrowserApprovalSheet::BuildStart", start,
		`if \(approvalOffered_\) \{\s*if \(auto style = StyleNamed\(L"AccentButtonStyle"\)\) actionButton_\.Style\(style\);\s*\}`,
		`row\.Children\(\)\.Append\(actionButton_\);\s*if \(approvalOffered_\) \{\s*TextBlock host = MakeText\(H\(std::string\(config::kApprovalHost\)\), 12, colors::MutedBrush\(\), false\);[^}]*row\.Children\(\)\.Append\(host\);\s*\}`,
		`if \(approvalOffered_\) \{\s*startPanel_\.Children\(\)\.Append\(row\);`)
	if count := strings.Count(start, "approvalOffered_"); count != 3 {
		t.Errorf("BuildStart names approvalOffered_ %d times; the accent, the host and the layout are each under `if (approvalOffered_)`, and nothing else is", count)
	}
	layoutAt := strings.LastIndex(start, "if (approvalOffered_) {")
	if layoutAt < 0 {
		t.Fatal("BuildStart no longer lays its page out as `if (approvalOffered_) { ... } else { ... }`; update this contract")
	}
	branches := strings.SplitN(start[layoutAt:], "} else {", 2)
	if len(branches) != 2 {
		t.Fatal("BuildStart's layout has no else branch for the page without the approval; update this contract")
	}
	urIoRequireAppends(t, "BrowserApprovalSheet::BuildStart (with the approval)", branches[0], "startPanel_",
		"row", "startNote_", "either", "body")
	urIoRequireAppends(t, "BrowserApprovalSheet::BuildStart (the paste flow)", branches[1], "startPanel_",
		"body", "row", "startNote_", "pasteBox_")
	provideRequire(t, "BrowserApprovalSheet::BuildStart", start,
		"actionButton_.Content(winrt::box_value(ActionText()));",
		`TextBlock either = MakeText(Loc("or"), 12, colors::MutedBrush(), false);`,
		`pasteBox_.PlaceholderText(Loc("auth_code"));`,
		`Automation::AutomationProperties::SetName(pasteBox_, Loc("auth_code"));`)

	// the Waiting page: the spinner, what is awaited, where the browser went, why it did not open, the two buttons
	waiting := definitionBody(t, "BrowserApprovalSheet.cpp", sheet, "void BrowserApprovalSheet::BuildWaiting() {")
	urIoRequireAppends(t, "BrowserApprovalSheet::BuildWaiting", waiting, "waitingPanel_",
		"ring", "title", "host", "launchError_", "buttons")
	urIoRequireAppends(t, "BrowserApprovalSheet::BuildWaiting", waiting, "buttons", "againButton_", "copyButton_")
	provideRequire(t, "BrowserApprovalSheet::BuildWaiting", waiting,
		`TextBlock title = MakeText(Loc("waiting_for_approval"), 18, colors::TextBrush());`,
		`TextBlock host = MakeText(H(std::string(config::kApprovalHost)), 12, colors::MutedBrush());`,
		"againButton_.Content(winrt::box_value(ActionText()));",
		`copyButton_.Content(winrt::box_value(Loc("copy_to_clipboard")));`)

	// the paste flow's button opens the site this sheet has always opened
	provideRequire(t, "BrowserApprovalSheet.cpp", sheet, `constexpr const wchar_t* kManualSignInUrl = L"https://ur.io";`)
	if count := strings.Count(sheet, "kManualSignInUrl"); count != 2 { // the constant, and OpenManualSignIn (pinned whole)
		t.Errorf("BrowserApprovalSheet.cpp names kManualSignInUrl %d times; the constant and OpenManualSignIn are all that may", count)
	}

	// the link: these seven uses and no other, in the sheet's source
	linkUses := []struct {
		statement string
		count     int
	}{
		{"approval::Wipe(link_);", 2},                                 // a page that is not Waiting, and a closed sheet
		{"link_ = std::move(url);", 1},                                // StartApproval
		{"co_await LaunchInBrowser(H(link_));", 1},                    // StartApproval
		{"if (link_.empty()) return;", 1},                             // CopyLink
		{"package.SetText(H(link_));", 1},                             // CopyLink
		{"ClearClipboardLater(dialog_.DispatcherQueue(), link_);", 1}, // CopyLink
	}
	expected := 0
	for _, use := range linkUses {
		expected += use.count
		if got := strings.Count(sheet, use.statement); got != use.count {
			t.Errorf("BrowserApprovalSheet.cpp has %d of %q, want %d", got, use.statement, use.count)
		}
	}
	if got := len(regexp.MustCompile(`\blink_\b`).FindAllStringIndex(sheet, -1)); got != expected {
		t.Errorf("BrowserApprovalSheet.cpp uses link_ %d times, %d of them where the request may go (the launcher, the clipboard, the wipes): "+
			"the link must not reach a control's text, content, tooltip or automation property, or a log", got, expected)
	}
}

// A hide to the tray keeps the attempt, and the person closing the sheet cancels it. Which of the two happened is decided
// when it happens, by a count the hide moves before it sweeps the sheets, and not by where the window is when the dialog's
// Closed event comes: that event follows the popup's closing transition, which a hidden window may not run until it is shown
// again. By then the window can already be shown (and the sheet would cancel the attempt the hide meant to keep), or an
// answer can have landed on a sheet that is closing (and its one-time code would be spent into a dialog that is going away).
func TestAHideToTrayIsCountedBeforeTheSheetsAreSwept(t *testing.T) {
	controller := urIoController(t)
	hide := definitionBody(t, "AppController.cpp", controller, "void AppController::HideWindow() {")
	signOutRequireInOrder(t, "AppController::HideWindow", hide,
		regexp.QuoteMeta("windowShown_ = false;"),
		regexp.QuoteMeta("++windowHides_;"),
		regexp.QuoteMeta("self->CloseSheetsForHide();"))
	if count := strings.Count(controller, "windowHides_"); count != 1 {
		t.Errorf("AppController.cpp names windowHides_ %d times; HideWindow counts a hide, once, and nothing else writes it", count)
	}
	header := stripComments(lf(readAppSource(t, "AppController.h")))
	provideRequire(t, "AppController.h", header,
		"std::uint64_t WindowHides() const { return windowHides_; }", "std::uint64_t windowHides_ = 0;")
	if count := strings.Count(header, "windowHides_"); count != 2 {
		t.Errorf("AppController.h names windowHides_ %d times; the accessor reads it and the member is declared", count)
	}
	// the sheet and the login page are the readers: the sheet asks whether a hide happened since it was built, and the page
	// asks the sheet whether it is still open
	for name, code := range authLinkSourceFiles(t) {
		if strings.Contains(code, "WindowHides()") && name != "App/AppController.h" && name != "App/BrowserApprovalSheet.cpp" {
			t.Errorf("%s reads the count of hides: only the browser sign-in sheet tells a hide from a close with it", name)
		}
	}
}

// Code that was written for the approval and nothing calls any more is deleted, not kept for later: each of these had
// no caller (or its caller was a pin that forbade it) and would only mislead the next reader about what the app can do.
func TestNoDeadApprovalCodeIsLeftBehind(t *testing.T) {
	for name, code := range authLinkSourceFiles(t) {
		for _, gone := range []string{
			"RemoveProtocolHandler(", "PlanRemove(", "Decision::Remove", "HasApprovedNetwork(", "NewApprovalKeyPair(", "approval().",
		} {
			if strings.Contains(code, gone) {
				t.Errorf("%s has %q again: it was removed because nothing called it", name, gone)
			}
		}
	}
	// what was kept for a reason: the registration's state is asked for by the offer, and nothing else
	if !strings.Contains(urIoController(t), "ProtocolHandlerStatus()") {
		t.Error("AppController.cpp no longer asks ProtocolHandlerStatus(): a key that starts another program must hide the action")
	}
	// the model keeps one copy of each alphabet: the parser's, which its spec sweeps character by character
	model := stripComments(lf(readAppSource(t, "UrIoApproval.h")))
	for _, duplicate := range []string{"constexpr bool IsBase64UrlChar(", "constexpr bool IsBase58Char(", "bool IsBase58Char(", "bool IsBase64UrlChar("} {
		if strings.Contains(model, duplicate) {
			t.Errorf("UrIoApproval.h defines %q again: it uses the parser's predicate (authlink::detail), whose spec sweeps every character", duplicate)
		}
	}
	provideRequire(t, "UrIoApproval.h", model, "using authlink::detail::IsBase58Char;", "using authlink::detail::IsBase64UrlChar;")
}

// The two coroutines nobody awaits, which start the browser, keep every call that can throw on a page inside a
// try: an exception that leaves a fire-and-forget coroutine ends the app, the tray and the service's window with it.
func TestApprovalSheetCoroutinesDoNotLetAnExceptionOut(t *testing.T) {
	sheet := stripComments(lf(readAppSource(t, "BrowserApprovalSheet.cpp")))
	for _, signature := range []string{
		"winrt::fire_and_forget BrowserApprovalSheet::StartApproval() {", "winrt::fire_and_forget BrowserApprovalSheet::OpenManualSignIn() {",
	} {
		body := definitionBody(t, "BrowserApprovalSheet.cpp", sheet, signature)
		signOutRequireInOrder(t, signature, body,
			regexp.QuoteMeta("starting_ = true;"), regexp.QuoteMeta("try {"), regexp.QuoteMeta("co_await LaunchInBrowser("),
			regexp.QuoteMeta("} catch (...) {"), regexp.QuoteMeta("starting_ = false;"))
	}
	// the launcher's own coroutine answers false for whatever it throws
	launch := definitionBody(t, "BrowserApprovalSheet.cpp", sheet,
		"winrt::Windows::Foundation::IAsyncOperation<bool> LaunchInBrowser(hstring url) {")
	signOutRequireInOrder(t, "LaunchInBrowser", launch,
		regexp.QuoteMeta("try {"), regexp.QuoteMeta("co_await winrt::Windows::System::Launcher::LaunchUriAsync("),
		regexp.QuoteMeta("} catch (...) {"), regexp.QuoteMeta("launched = false;"), regexp.QuoteMeta("co_return launched;"))
}

// The strings: the sheet looks up only what the blueprint lists, every one of them a key the localization store tags
// windows, so it adds nothing to the store's backlog. The Start page's action is the one that says where the browser goes.
func TestApprovalSheetUsesOnlyTheStringsThatAreListed(t *testing.T) {
	sheet := stripComments(lf(readAppSource(t, "BrowserApprovalSheet.cpp")))
	tagged := map[string]bool{
		"adv_browser_sign_in_title": true, "adv_browser_sign_in_body": true, "adv_open_ur_io": true,
		"sso_sign_in_timed_out": true, "sso_browser_open_failed": true,
		"auth_code": true, "or": true, "waiting_for_approval": true, "copy_to_clipboard": true, "network_name_label": true,
		"sign_in": true, "cancel": true, "try_again": true, "something_went_wrong": true, "auth_code_error": true,
	}
	data, err := os.ReadFile(filepath.Join(repositoryRoot(t), "app", "src", "App", "Strings", "windows-keys.txt"))
	if err != nil {
		t.Fatal(err)
	}
	windows := map[string]bool{}
	for _, line := range strings.Split(lf(string(data)), "\n") {
		windows[strings.TrimSpace(line)] = true
	}
	used := map[string]bool{}
	for _, match := range regexp.MustCompile(`\b(?:Loc|LocBox|Adv|AdvW)\(\s*"([a-z0-9_]+)"`).FindAllStringSubmatch(sheet, -1) {
		used[match[1]] = true
		key := match[1]
		switch {
		case tagged[key]:
			if !windows[key] {
				t.Errorf("the sheet looks up %q, which the store does not tag windows", key)
			}
		default:
			t.Errorf("the sheet looks up %q: it is not one of the strings the blueprint lists, and a key the store lacks is the owner's backlog", key)
		}
	}
	// the English of the action: where it goes is shown beside it, as plain text from the build constant
	provideRequire(t, "BrowserApprovalSheet.cpp", sheet, `Adv("adv_open_ur_io", L"Open ur.io")`,
		"H(std::string(config::kApprovalHost))", "approvalOffered_ = urnw::App().ApprovalOffered();")
	if strings.Count(sheet, "config::kApprovalHost") != 2 {
		t.Error("the Start page and the Waiting page each show the host the browser goes to, from the build constant")
	}
	for _, forbidden := range []string{"kApprovalPageUrl", "kApprovalDescriptorUrl", "BuildRequestUrl("} {
		if strings.Contains(sheet, forbidden) {
			t.Errorf("BrowserApprovalSheet.cpp names %s: the controller builds the request, and the sheet shows only the host", forbidden)
		}
	}
}

// The link of an attempt is a secret until it is opened, so Copy puts it on the clipboard without history and without
// roaming, and takes it off again a minute later, but only if it is still what is there.
func TestApprovalSheetCopiesTheLinkQuietly(t *testing.T) {
	sheet := stripComments(lf(readAppSource(t, "BrowserApprovalSheet.cpp")))
	provideRequire(t, "BrowserApprovalSheet.cpp", sheet,
		"constexpr std::chrono::seconds kClipboardLife{60};", "co_await winrt::resume_after(kClipboardLife);",
		"IsAllowedInHistory(false)", "IsRoamable(false)", "dt::Clipboard::SetContentWithOptions(package, options)")
	if count := strings.Count(sheet, "SetContentWithOptions("); count != 1 {
		t.Errorf("BrowserApprovalSheet.cpp writes the clipboard %d times; CopyLink does, once", count)
	}
	clear := definitionBody(t, "BrowserApprovalSheet.cpp", sheet, "winrt::fire_and_forget ClearClipboardIfUnchanged(std::string copied) {")
	signOutRequireInOrder(t, "ClearClipboardIfUnchanged", clear,
		regexp.QuoteMeta("dt::Clipboard::GetContent()"), regexp.QuoteMeta("content.GetTextAsync()"),
		regexp.QuoteMeta("if (Narrow(now.c_str()) == copied) dt::Clipboard::Clear();"), regexp.QuoteMeta("approval::Wipe(copied);"))
	if count := strings.Count(sheet, "Clipboard::Clear()"); count != 1 {
		t.Errorf("BrowserApprovalSheet.cpp clears the clipboard %d times; once, and only when it still holds the link", count)
	}
	copyLink := definitionBody(t, "BrowserApprovalSheet.cpp", sheet, "void BrowserApprovalSheet::CopyLink() {")
	signOutRequireInOrder(t, "BrowserApprovalSheet::CopyLink", copyLink,
		regexp.QuoteMeta("SetContentWithOptions(package, options)"), regexp.QuoteMeta("ClearClipboardLater(dialog_.DispatcherQueue(), link_);"))
}

// The model, the registration, the page's descriptor and the checks are listed in the projects they belong to,
// as the rows the build needs: the two plain Win32 units without the precompiled header, the rest as headers.
func TestProjectListsTheApprovalFiles(t *testing.T) {
	project := lf(readAppSource(t, "App.vcxproj"))
	provideRequire(t, "App.vcxproj", project,
		`<ClCompile Include="ProtocolHandler.cpp"><PrecompiledHeader>NotUsing</PrecompiledHeader></ClCompile>`,
		`<ClCompile Include="ApprovalCheck.cpp"><PrecompiledHeader>NotUsing</PrecompiledHeader></ClCompile>`,
		`<ClInclude Include="AuthLink.h" />`, `<ClInclude Include="UrIoApproval.h" />`, `<ClInclude Include="ProtocolHandler.h" />`,
		`<ClInclude Include="ApprovalDescriptor.h" />`, `<ClInclude Include="ApprovalUnseal.h" />`, `<ClInclude Include="ApprovalCheck.h" />`,
		`<ClInclude Include="ApprovalNetwork.h" />`,
		// the sheet is a WinRT unit like the other sheets: it uses the precompiled header
		`<ClCompile Include="BrowserApprovalSheet.cpp" />`, `<ClInclude Include="BrowserApprovalSheet.h" />`)
	provideRequire(t, "Common.vcxproj", lf(readCommonSource(t, "Common.vcxproj")), `<ClInclude Include="ProtocolRegistration.h" />`)
	// the project file is still the XML MSBuild reads, with the byte order mark it had
	parseXML(t, filepath.Join(repositoryRoot(t), "app", "src", "App", "App.vcxproj"))
	if raw := readAppSource(t, "App.vcxproj"); !strings.HasPrefix(raw, string([]byte{0xEF, 0xBB, 0xBF})) {
		t.Error("App.vcxproj lost its byte order mark")
	}
	// no unit of the app that is not plain Win32 skips the precompiled header, and the new ones are not listed twice
	for _, row := range []string{`Include="ProtocolHandler.cpp"`, `Include="ApprovalCheck.cpp"`, `Include="UrIoApproval.h"`, `Include="ApprovalNetwork.h"`,
		`Include="BrowserApprovalSheet.cpp"`, `Include="BrowserApprovalSheet.h"`} {
		if count := strings.Count(project, row); count != 1 {
			t.Errorf("App.vcxproj lists %s %d times", row, count)
		}
	}
	// the pure headers stay pure: a Windows header in one would stop the host tests building it
	for _, name := range []string{"UrIoApproval.h", "AuthLink.h", "ApprovalDescriptor.h", "ApprovalNetwork.h"} {
		code := stripComments(lf(readAppSource(t, name)))
		for _, forbidden := range []string{"winrt::", "<windows.h>", "urnetwork_sdk", "<winhttp.h>", "#include \"pch.h\""} {
			if strings.Contains(code, forbidden) {
				t.Errorf("%s mentions %q: it is a pure header that the host tests build", name, forbidden)
			}
		}
	}
}

// A request that names its network (contract version 2, section 11) is the contract's own text as far as the app goes: the
// sentences the code relies on are pinned here, in the repository's copy, so that a change to them is a change made on purpose.
// (The same contract is the page's, which commits its own copy; the vector below is what both copies hold.)
func TestApprovalContractNamesTheNetworkOfVersionTwo(t *testing.T) {
	data, err := os.ReadFile(filepath.Join(repositoryRoot(t), "docs", "app-approval-contract.md"))
	if err != nil {
		t.Fatal(err)
	}
	contract := authLinkSquash(lf(string(data)))
	provideRequire(t, "section 11 of the approval contract", contract,
		"Version 2 is version 1 plus one key, `api`:",
		"<page URL>#v=2&platform=windows&state=<S>&pk=<PK>&ts=<T>&api=<A>",
		"the host of the app's API, `host` or `host:port`, `https` implied: ASCII lower-case letters, digits, hyphens and dots, never a scheme, path, userinfo or query.",
		"Labels of 1 to 63 characters that start and end with a letter or digit; at least two labels; "+
			"the last label is neither all digits nor `0x` followed by hex digits (a browser reads either as a number, so no IP literal in any spelling); "+
			"a port is 1 to 65535 without a leading zero and is never `443`; at most 200 characters in all.",
		"The app sends version 2 only when its network space is not the official one, its API URL is `https://` plus a valid `api` value "+
			"(no path, query or fragment; an `http` API, an IP literal or a non-ASCII host is not offered), and the descriptor lists 2. "+
			"In the official network it sends version 1, whatever else the descriptor lists.",
		// how the url is read: the way a URL is, which is how the client and the page read it (ApiHostOfUrl)
		"It is read the way a URL is read, which is how the client and the page read it: capitals in the scheme or the host are the same host in small letters, "+
			"an explicit `:443` is https' own port and names the same server without it, and a single trailing `/` is ignored; "+
			"the `api` value is the small-letter form without `:443`.",
		// what the official network is (NetworkOf): the operator's space whose API really is the operator's own
		"The official network is the operator's own space (its host, and no API or connect URL of the person's own) whose API really is the operator's `api.<host>`: "+
			"a space with the operator's host but another API under it (a test environment of the operator, whose name the SDK puts in front of the service label) "+
			"is named by that API like any other network",
		// the descriptor is asked for wherever the action could be offered
		"The descriptor (section 1) is fetched once per run in every network whose API can be named, not only in the official one, because the version a network needs is listed in it.",
		"that request carries nothing of the network, and the app makes none when no network can be named.",
		"The attempt is bound to the network it started in: a network change ends it (section 5, rule 5), and the redeem goes to the API the app is on.",
		// ... and the binding is checked again where the code leaves the app (SdkHost::RedeemApprovedCode)
		"The binding is checked once more where the code leaves the app, because an accepted answer can wait for a sheet in the way to close: "+
			"if a network change (or any other sign-in flow) has begun since the attempt did, nothing is sent and the redeem is refused.",
		"The page waits up to 30 s for each API call",
		"body `{\"contracts\":[1,2],\"platforms\":[\"windows\"]}`",
		// the page side: version 1's one action signs the browser out, a session is the server's and no tab's, and the host is text until the browser reads it back
		"version 1 keeps its one action, 'Use the default server', which first signs this browser out of the server it leaves, then removes the override and goes on to the sign-in on the default server.",
		"the stored session belongs to the other server and is never sent to the new one, by this tab or by any other tab of the browser",
		"and then only if the browser's own URL parser gives it back exactly as written (`new URL('https://' + host).host` is `host`)",
		"For a host it reads otherwise Switch does nothing, and Cancel still answers.",
		// the app rules of section 5 say it too
		"only in a network a request can name (the official one, version 1; another whose API url section 11 allows, version 2) and after the descriptor check lists the version that request is.",
		"and only while no network change or other sign-in flow has begun since the attempt did (section 11).",
		"a network-space change and quit")
}

// The cases of section 7 and the vectors of section 9 say what the page does with a server that is not the request's, and what the app
// does with an answer that outlives its network. The row that called a custom API base a non-interactive message was true of version 1
// before there was a card for it, and is gone; so is the sentence that said no IP literal gets past a grammar that let 0x labels through.
func TestApprovalContractCasesNameTheServerMismatchAndTheRefusedRedeem(t *testing.T) {
	data, err := os.ReadFile(filepath.Join(repositoryRoot(t), "docs", "app-approval-contract.md"))
	if err != nil {
		t.Fatal(err)
	}
	contract := authLinkSquash(lf(string(data)))
	provideRequire(t, "the approval contract", contract,
		// section 1, the API row
		"if the browser holds a custom API base the page does not run the approval but shows a 'Different API server' card whose one action is 'Use the default server' (section 7)",
		// section 7, page side
		"| Framed, or has an opener | Non-interactive message | nothing |",
		"| The browser's API server is not the one the request is for (version 1: not the default server; version 2: not the host `api` names) | "+
			"Before any sign-in redirect and before any call to any API: 'Different API server', the hosts as plain text. "+
			"Version 1: 'Use the default server', which signs this browser out of the server it leaves, removes the override and goes on to the sign-in. "+
			"Version 2: Cancel, and 'Switch to {host}' behind 'Only continue if you set this server in URnetwork yourself.' "+
			"(signs this browser out, points it at `https://{host}`, signs in, back to the card) | nothing, or `access_denied` after Cancel |",
		// section 7, app side
		"| A network change (or any other sign-in flow) begins after the answer was accepted | "+
			"Before the code is sent: nothing is sent, and the redeem is refused like a spent code. "+
			"While the redeem is out: what it brings back is dropped, not held for a confirm, and the redeem is refused likewise |",
		// section 9, the reject vectors the page's tests commit too
		"an all-digit last label, a last label of `0x` and hex digits (`a.0x10`, `1.2.3.0x4`, `0x7f.0x0.0x0.0x1`, `a.0x`), an empty or hyphen-edged label")
	for _, gone := range []string{
		"Framed, has opener, or custom API base",
		"the page refuses to run if the browser holds a custom API base",
		"the last label is not all digits (so no IP literal)",
		"its API URL is exactly `https://`",
	} {
		if strings.Contains(contract, gone) {
			t.Errorf("the approval contract still says %q, which is no longer what the page and the app do", gone)
		}
	}
	// the plan the contract grew from used to reserve v=2 for the server-checked challenge, which is v=3 in the contract now;
	// the plan is a planning note that only some checkouts carry
	plan, err := os.ReadFile(filepath.Join(repositoryRoot(t), "docs", "superpowers", "2026-10-09-website-approval-signin-plan.md"))
	if os.IsNotExist(err) {
		return
	}
	if err != nil {
		t.Fatal(err)
	}
	squashedPlan := authLinkSquash(lf(string(plan)))
	provideRequire(t, "the approval plan", squashedPlan, "In the contract `v=2` is the request that names its network (section 11, added 2026-10-11) and `v=3` is the one reserved for the challenge.")
	if strings.Contains(squashedPlan, "**Versioning.** `v=2` is reserved for a server-checked code challenge") {
		t.Error("the approval plan still reserves v=2 for the server-checked challenge; the contract uses v=2 for the request that names its network")
	}
}

// The request vector both repositories commit (contract section 9): the constants of tools/approval-vector.h put together in the
// contract's order are the string the contract prints, and the spec builds exactly that from them.
func TestApprovalRequestVectorIsTheContractsOwn(t *testing.T) {
	root := repositoryRoot(t)
	header := stripComments(lf(readFileOrFail(t, filepath.Join(root, "app", "tools", "approval-vector.h"))))
	literal := func(name string) string {
		match := regexp.MustCompile(name + `\s*=\s*((?:"[^"]*"\s*)+);`).FindStringSubmatch(header)
		if match == nil {
			t.Fatalf("approval-vector.h no longer defines %s as string literals; update this contract", name)
		}
		var out string
		for _, piece := range regexp.MustCompile(`"([^"]*)"`).FindAllStringSubmatch(match[1], -1) {
			out += piece[1]
		}
		return out
	}
	state, key, api, request := literal("kState"), literal("kPublicKey"), literal("kApi"), literal("kRequestV2")
	startedUnix := regexp.MustCompile(`kStartedUnix\s*=\s*(\d+);`).FindStringSubmatch(header)
	if startedUnix == nil {
		t.Fatal("approval-vector.h no longer defines kStartedUnix; update this contract")
	}
	if state != strings.Repeat("S", 43) || api != "api.beta-test.net" || startedUnix[1] != "1790000000" || len(key) != 43 {
		t.Errorf("the vector's state, key, second or api is not the contract's: %q %q %q %s", state, key, api, startedUnix[1])
	}
	if want := "v=2&platform=windows&state=" + state + "&pk=" + key + "&ts=" + startedUnix[1] + "&api=" + api; request != want {
		t.Errorf("kRequestV2 is not the request its own parts make:\n got: %s\nwant: %s", request, want)
	}
	if len(request) != 153 {
		t.Errorf("kRequestV2 is %d characters, the contract says 153", len(request))
	}
	data, err := os.ReadFile(filepath.Join(root, "docs", "app-approval-contract.md"))
	if err != nil {
		t.Fatal(err)
	}
	contract := authLinkSquash(lf(string(data)))
	provideRequire(t, "section 9 of the approval contract", contract, request, "(153 characters, the keys in this order)", "`api` = api.beta-test.net")
	// the spec that builds it reads the vector from the header and nowhere else
	spec := lf(readFileOrFail(t, filepath.Join(root, "app", "tools", "ur-io-approval-tests.cpp")))
	provideRequire(t, "ur-io-approval-tests.cpp", spec, `#include "approval-vector.h"`, "approval_vector::kRequestV2", "approval_vector::kPublicKey", "approval_vector::kApi")
}

// The attempt for another network is bound to that network by the number of the flow it began in. A switch of network
// (ApplyNetworkServer) takes the next number before anything else in it can fail, whatever its outcome: the one way out before it is
// an empty host name, which switches nothing. So an attempt that names the network it left is a stray when its answer comes (the
// gate compares the numbers; the spec runs that with a version 2 attempt), the auth-state hook ends it, and a second click does
// not read it back (BeginApproval compares the same numbers). These are the statements that carry it from the SDK host to the gate.
func TestApprovalAttemptForAnotherNetworkIsBoundToItsNetwork(t *testing.T) {
	host := urIoHost(t)
	server := definitionBody(t, "SdkHost.cpp", host, "bool SdkHost::ApplyNetworkServer(")
	cancelAt := strings.Index(server, `CancelPendingWalletFlows("superseded by a network change");`)
	if cancelAt < 0 {
		t.Fatal("SdkHost::ApplyNetworkServer no longer takes the next flow number")
	}
	const wantBefore = "bool SdkHost::ApplyNetworkServer(const std::string& hostName, const std::string& apiUrl, const std::string& connectUrl) { if (hostName.empty()) return false;"
	if before := authLinkSquash(server[:cancelAt]); before != wantBefore {
		t.Errorf("ApplyNetworkServer does something before it takes the next flow number: %q (only an empty host name may leave first)", before)
	}
	cancel := definitionBody(t, "SdkHost.cpp", host, "uint64_t SdkHost::CancelPendingWalletFlows(const char* reason) {")
	signOutRequireInOrder(t, "SdkHost::CancelPendingWalletFlows", cancel, regexp.QuoteMeta("const uint64_t flow = walletFlows_.Start();"), regexp.QuoteMeta("return flow;"))

	controller := urIoController(t)
	context := definitionBody(t, "AppController.cpp", controller, "approval::Context AppController::ApprovalContext() {")
	provideRequire(t, "AppController::ApprovalContext", context, "context.flow = sdk_.CurrentBrowserFlow();")
	onAuth := definitionBody(t, "AppController.cpp", controller, "void AppController::OnAuthState(AuthState state, const std::string& error) {")
	provideRequire(t, "AppController::OnAuthState", onAuth,
		"} else if (approval_.Live() && approval_.Flow() != sdk_.CurrentBrowserFlow()) {", "CancelApproval(approval::ClearReason::Superseded);")
	begin := definitionBody(t, "AppController.cpp", controller, "std::optional<approval::Slot::View> AppController::BeginApproval() {")
	provideRequire(t, "AppController::BeginApproval", begin,
		"if (approval_.Flow() == sdk_.CurrentBrowserFlow() && approval_.Reopenable(now)) return live;")
	// the redeem goes to the API the app is on now, and to nothing else
	redeem := definitionBody(t, "SdkHost.cpp", host, "void SdkHost::RedeemApprovedCode(const std::string& code, std::function<void(ApprovedNetwork)> done) {")
	if strings.Count(redeem, "authCodeLogin(") != 1 || strings.Contains(redeem, "https://") || strings.Contains(redeem, "approvalNetwork_") {
		t.Error("RedeemApprovedCode redeems once, through api_, which is the API the app is on; it names no server of its own")
	}
	// ... and only while no flow has begun since the approval did. The gate judged the same number when it accepted the answer, but a
	// sheet in the way is closed first and the code waits up to two seconds for it (LoginPage::ContinueApprovedCode), time enough for
	// a network change, which takes the next number before it takes the lock it replaces the handle under. So the number is
	// judged again where the code leaves, under the lock the handle is read under, and a switch that has begun is a refusal.
	provideRequire(t, "SdkHost::BeginBrowserApproval", definitionBody(t, "SdkHost.cpp", host, "std::uint64_t SdkHost::BeginBrowserApproval() {"),
		"approvalFlow_.store(flow, std::memory_order_release);")
	signOutRequireInOrder(t, "SdkHost::RedeemApprovedCode", redeem,
		`bool superseded = false;\s*\{\s*std::scoped_lock lock\(mutex_\);\s*if \(api_\) \{\s*superseded = flow == 0 \|\| !walletFlows_\.IsCurrent\(flow\);`,
		regexp.QuoteMeta("api_->authCodeLogin(args, answer);"),
		regexp.QuoteMeta("if (superseded) {"),
		regexp.QuoteMeta("ApprovedNetwork::Failure::Refused;"))
	if strings.Contains(redeem, "ApplyNetworkServer") || strings.Contains(redeem, "CurrentApprovalNetwork") {
		t.Error("RedeemApprovedCode asks the flow serial and nothing else whether its code is still for the network the app is on")
	}
}
