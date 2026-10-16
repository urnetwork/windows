// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"regexp"
	"strings"
	"testing"
)

// The call sites of the owner's 2026-10-05 decision on signing out of
// URnetwork: it stops the tunnel and the provider the same as Quit, and
// nothing of the signed-out account runs or starts again. What a delivery
// sends and when the obligation clears is pure and runs in sign_out_test.go;
// SdkHost, the service and the protocol need Windows, so these read their
// sources with every comment blanked.

// Each pattern matches in text after the match of the one before it, so a
// statement that repeats (a return, a flag) is found where the sequence needs
// it rather than at its first occurrence.
func signOutRequireInOrder(t *testing.T, where, text string, patterns ...string) {
	t.Helper()
	from := 0
	for _, pattern := range patterns {
		location := regexp.MustCompile(pattern).FindStringIndex(text[from:])
		if location == nil {
			t.Errorf("%s is missing %s, in this order after the patterns before it", where, pattern)
			return
		}
		from += location[1]
	}
}

// Logout -> slot emptied -> signed out -> Quit's stop_tunnel and stop_provider
// and the logout, owed until done -> this side of the session. The local
// credentials go before the service is asked, so an app ended while the pipe
// waits is signed out on disk, and nothing in Logout starts anything.
func TestSignOutWiringLogoutStopsAsQuitThenForgets(t *testing.T) {
	host := sdkHostSource(t)
	logout := signOutSource(t, host)
	signOutRequireInOrder(t, "SdkHost::Logout", logout,
		regexp.QuoteMeta("std::scoped_lock lock(pendingMutex_);"),
		regexp.QuoteMeta("pending_ = SessionRequest{};"),
		regexp.QuoteMeta("pendingRequested_ = false;"),
		regexp.QuoteMeta("pendingCv_.notify_all();"),
		regexp.QuoteMeta("loggedIn_.store(false, std::memory_order_release);"),
		regexp.QuoteMeta("std::scoped_lock lock(mutex_);"),
		regexp.QuoteMeta("asyncLocalState_->logout("),
		regexp.QuoteMeta("signOut_.Begin(SignOutServiceLocked());"),
		regexp.QuoteMeta("device_->close();"),
		`TeardownSessionLocked\(\s*false\);`,
		regexp.QuoteMeta("SetAuthState(AuthState::LoggedOut);"))
	provideRequire(t, "SdkHost::Logout", logout,
		"if (delivery == signout::Delivery::Delivered) {", "ScheduleServiceRetry();")
	quitForbid(t, "SdkHost::Logout", logout,
		"signing out starts nothing, and stops through the obligation alone",
		"service_.StartProvider(", "service_.StartTunnel(", "RequestSession(", "EnsureSession(",
		"RequestProviderReconcile(", "service_.StopTunnel(", "service_.Logout(",
		"TeardownSessionLocked();")

	// The delivery is the service's own requests, each answer checked: the
	// channel is dialled when it is down, stop_tunnel must answer, an older
	// service's unknown stop_provider is done, and the logout names the space
	// whose sdk state the service clears.
	service := definitionBody(t, "SdkHost.cpp", host,
		"signout::Service SdkHost::SignOutServiceLocked() {")
	signOutRequireInOrder(t, "SdkHost::SignOutServiceLocked", service,
		regexp.QuoteMeta("if (!service_.IsConnected()) service_.Connect();"),
		regexp.QuoteMeta("return service_.IsConnected();"),
		regexp.QuoteMeta("case signout::Request::StopTunnel: {"),
		regexp.QuoteMeta("service_.StopTunnel(&answered);"),
		regexp.QuoteMeta("if (!answered) {"),
		regexp.QuoteMeta("case signout::Request::StopProvider: {"),
		regexp.QuoteMeta("service_.StopProvider(&after, &error);"),
		regexp.QuoteMeta("if (stopped || IsUnknownRequestReply(error)) return true;"),
		regexp.QuoteMeta("case signout::Request::Logout: {"),
		regexp.QuoteMeta("logout.network_space_json = networkSpace_->toJson();"),
		regexp.QuoteMeta("const bool done = service_.Logout(logout);"),
		regexp.QuoteMeta("return done;"))
	client := stripComments(readAppSource(t, "ServiceClient.cpp"))
	provideRequire(t, "ServiceClient.cpp", client,
		"return CallStatus(proto::Request(proto::msg::kStopTunnel), answered);",
		"pipe_.Call(proto::Request(proto::msg::kLogout, body));")
}

// Every pass of the session worker delivers an owed sign-out before anything
// else, and keeps the watchdog going while it stays owed.
func TestSignOutWiringEveryPassDeliversFirst(t *testing.T) {
	host := sdkHostSource(t)
	worker := definitionBody(t, "SdkHost.cpp", host, "void SdkHost::SessionWorkerLoop() {")
	provider := quitSourceFrom(t, "SdkHost::SessionWorkerLoop", worker,
		"if (req.kind == ConnectKind::Provider) {", "continue;")
	signOutRequireInOrder(t, "the provider pass", provider,
		regexp.QuoteMeta("std::scoped_lock lock(mutex_);"),
		regexp.QuoteMeta("SettleSignOutLocked(req.reason);"),
		regexp.QuoteMeta("ReconcileProviderLocked(req.reason);"))
	session := quitSourceFrom(t, "SdkHost::SessionWorkerLoop", worker,
		"bool ok = false;", "if (!device_) ReconcileProviderLocked(req.reason);")
	signOutRequireInOrder(t, "the session pass", session,
		regexp.QuoteMeta("std::scoped_lock lock(mutex_);"),
		regexp.QuoteMeta("SettleSignOutLocked(req.reason);"),
		regexp.QuoteMeta("CurrentServiceStatusLocked(answered);"),
		regexp.QuoteMeta("ok = BootstrapSession(req.reason, attachOnly);"),
		regexp.QuoteMeta("if (!device_) ReconcileProviderLocked(req.reason);"))

	settle := definitionBody(t, "SdkHost.cpp", host,
		"void SdkHost::SettleSignOutLocked(const char* reason) {")
	signOutRequireInOrder(t, "SdkHost::SettleSignOutLocked", settle,
		regexp.QuoteMeta("if (!signOut_.Owed()) return;"),
		regexp.QuoteMeta("signOut_.Settle(SignOutServiceLocked());"),
		regexp.QuoteMeta("if (delivery == signout::Delivery::Delivered) {"),
		regexp.QuoteMeta("return;"),
		regexp.QuoteMeta("ScheduleServiceRetry();"))

	// the launch reads what an earlier run left, before its first pass
	initialize := definitionBody(t, "SdkHost.cpp", host, "bool SdkHost::Initialize() {")
	signOutRequireInOrder(t, "SdkHost::Initialize", initialize,
		regexp.QuoteMeta("signOut_.Load();"), regexp.QuoteMeta(`EnsureSession("resume");`))
	provideRequireOrder(t, "SdkHost::Initialize", initialize,
		"signOut_.Load();", `RequestProviderReconcile("launch, signed out");`)

	// an owed sign-out keeps the watchdog, signed in or not; signed out, it asks
	// for a provider pass, which delivers it
	watchdog := definitionBody(t, "SdkHost.cpp", host, "void SdkHost::ServiceWatchdogLoop() {")
	provideRequire(t, "SdkHost::ServiceWatchdogLoop", watchdog,
		"return !serviceRecoveryNeeded_.load(std::memory_order_acquire) && !signOut_.Owed();",
		"return watchdogStop_ || idle();",
		"if (watchdogStop_ || idle()) {",
		"if (!signedIn && !signOut_.Owed()) {",
		`RequestProviderReconcile("automatic service recovery, sign-out owed");`)
	signOutRequireInOrder(t, "SdkHost::ServiceWatchdogLoop", watchdog,
		regexp.QuoteMeta("if (signedIn) {"),
		`EnsureSession\("automatic service recovery",\s*true\);`,
		regexp.QuoteMeta("} else {"),
		regexp.QuoteMeta(`RequestProviderReconcile("automatic service recovery, sign-out owed");`))
	header := stripComments(readAppSource(t, "SdkHost.h"))
	provideRequire(t, "SdkHost.h", header, "signout::Obligation signOut_{SignOutMarker()};")
}

// Nothing is adopted or started for a signed-out app, or while a sign-out is
// owed: the bootstrap declines when signed out and refuses while owed before
// it reads the saved session, and the reconcile provides nothing when signed
// out and starts nothing while owed. Both read loggedIn_, not only the stored
// jwt, whose asynchronous logout may not have landed.
func TestSignOutWiringNothingStartsWhileOwed(t *testing.T) {
	host := sdkHostSource(t)
	bootstrap := definitionBody(t, "SdkHost.cpp", host,
		"bool SdkHost::BootstrapSession(const char* reason, bool attachOnly) {")
	signOutRequireInOrder(t, "SdkHost::BootstrapSession", bootstrap,
		regexp.QuoteMeta("if (!loggedIn_.load(std::memory_order_acquire)) {"),
		regexp.QuoteMeta("bootstrapDeclined_ = true;"),
		regexp.QuoteMeta("return false;"),
		regexp.QuoteMeta("const std::string clientJwt = localState_->getByClientJwt();"),
		regexp.QuoteMeta("if (!service_.IsConnected() && !service_.Connect()) {"),
		regexp.QuoteMeta("if (signOut_.Owed()) {"),
		regexp.QuoteMeta("bootstrapServiceRetryable_ = true;"),
		regexp.QuoteMeta("return false;"),
		regexp.QuoteMeta("RpcSessionLoad loaded = LoadRpcSession();"),
		regexp.QuoteMeta("proto::TunnelStatus st = service_.StartTunnel(cfg);"))

	reconcile := definitionBody(t, "SdkHost.cpp", host,
		"void SdkHost::ReconcileProviderLocked(const char* reason) {")
	signOutRequireInOrder(t, "SdkHost::ReconcileProviderLocked", reconcile,
		regexp.QuoteMeta("const bool signedIn = loggedIn_.load(std::memory_order_acquire);"),
		regexp.QuoteMeta(`std::string mode = "never";`),
		regexp.QuoteMeta("if (signedIn && !clientJwt.empty() && !instanceId.empty())"),
		regexp.QuoteMeta("mode = localState_->getProvideControlMode();"),
		regexp.QuoteMeta("if (step == provide::DisconnectedStep::Stop) {"),
		regexp.QuoteMeta("if (signOut_.Owed()) {"),
		regexp.QuoteMeta("return;"),
		regexp.QuoteMeta("service_.StartProvider(request, &after, &error);"))
}

// The service's logout: timed like stop_tunnel, so a wedged session lock
// refuses it rather than holding the pipe; then the stop and the identity, and
// the sdk state of the account's space, which a restarted service imports from
// the request. Not ok keeps the sign-out owed in the app.
func TestSignOutWiringServiceForgetsTheAccount(t *testing.T) {
	server := stripComments(readServiceSource(t, "ControlServer.cpp"))
	branch := quitSourceFrom(t, "ControlServer::Handle", server,
		"type == proto::msg::kLogout", "} else if")
	signOutRequireInOrder(t, "ControlServer's logout", branch,
		regexp.QuoteMeta("const proto::Logout req = request.get<proto::Logout>();"),
		regexp.QuoteMeta("reply.ok = tunnel_.Logout(req.network_space_json);"),
		regexp.QuoteMeta("reply.status = tunnel_.Status();"),
		regexp.QuoteMeta("PushState();"))

	controller := tunnelControllerSource(t)
	logout := definitionBody(t, "TunnelController.cpp", controller,
		"bool TunnelController::Logout(const std::string& networkSpaceJson)")
	signOutRequireInOrder(t, "TunnelController::Logout", logout,
		regexp.QuoteMeta("if (!lock.try_lock_for(kStopLockBudget)) {"),
		regexp.QuoteMeta("return false;"),
		`StopLocked\(\s*true\);`,
		regexp.QuoteMeta(`std::filesystem::remove(storageDir_ / L"client_key_seed.bin", ec);`),
		regexp.QuoteMeta(`std::filesystem::remove(storageDir_ / L"provide_cert.pem", ec);`),
		regexp.QuoteMeta(`std::filesystem::remove(storageDir_ / L"provide_key.pem", ec);`),
		regexp.QuoteMeta("ImportNetworkSpaceLocked(networkSpaceJson);"),
		regexp.QuoteMeta("localState.logout();"),
		regexp.QuoteMeta("return cleared;"))
	if strings.Contains(logout, "std::scoped_lock lock(mutex_);") {
		t.Error("TunnelController::Logout waits on the session lock without a budget: " +
			"a wedged connect then holds the control pipe")
	}

	protocol := stripComments(readCommonSource(t, "Protocol.h"))
	provideRequire(t, "Protocol.h", protocol,
		"struct Logout {",
		`j = {{"network_space_json", v.network_space_json}};`,
		`if (auto it = j.find("network_space_json"); it != j.end() && it->is_string())`)
}

// The marker that outlives the app is a file of its own in the app's storage,
// read on launch, written when owed and removed when delivered.
func TestSignOutWiringMarkerOutlivesTheApp(t *testing.T) {
	paths := stripComments(readCommonSource(t, "Paths.cpp"))
	marker := definitionBody(t, "Paths.cpp", paths, "std::filesystem::path SignOutOwedFile() {")
	signOutRequireInOrder(t, "SignOutOwedFile", marker, `StorageRoot\(\s*false\) / L"sign_out_owed"`)
	host := sdkHostSource(t)
	store := definitionBody(t, "SdkHost.cpp", host, "signout::Marker SdkHost::SignOutMarker() {")
	provideRequire(t, "SdkHost::SignOutMarker", store,
		"return std::filesystem::exists(SignOutOwedFile(), ec);",
		"std::filesystem::remove(SignOutOwedFile(), ec);",
		"std::ofstream file(SignOutOwedFile(), std::ios::trunc);")
}
