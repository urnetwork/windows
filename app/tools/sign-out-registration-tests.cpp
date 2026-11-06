// Runs the actual SdkHost registration/logout methods and AppController auth
// handler, extracted by tests/sign_out_registration_test.go, over local fakes.
// Every interruption is an explicit hook: no network, SDK runtime, sleep or
// scheduler luck is needed to reproduce the old account crossing a logout.
// SPDX-License-Identifier: MPL-2.0

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <chrono>
#include <iostream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "SignOutRun.h"

namespace {
int failures = 0;
int checks = 0;
void Check(bool ok, const std::string& message) {
  ++checks;
  if (!ok) {
    ++failures;
    std::cout << "FAIL: " << message << '\n';
  }
}
template <class... Args> void LogInfo(Args&&...) {}
template <class... Args> void LogWarn(Args&&...) {}
template <class... Args> void LogError(Args&&...) {}

namespace urnet {
struct Error { std::string message; std::optional<std::string> code = {}; };
struct WalletAuthArgs {
  std::optional<std::string> wallet_address, wallet_signature, wallet_message, blockchain;
};
inline const std::string TAO = "tao", SOL = "sol";
struct AuthLoginArgs {
  std::string user_auth, seedphrase, auth_jwt_type, auth_jwt;
  std::optional<WalletAuthArgs> wallet_auth;
  bool result_errors = false;
};
struct AuthLoginWithPasswordArgs { std::string user_auth, password; bool verify_otp_numeric = false; };
struct AuthCodeLoginArgs { std::string auth_code; };
struct AuthVerifyArgs { std::string user_auth, verify_code; };
struct Network { std::string by_jwt; };
struct OptionalNetwork { std::optional<std::string> by_jwt; };
struct Verification { std::optional<Error> send_error; };
struct AuthLoginResult {
  std::optional<Error> error;
  std::optional<Network> network;
  std::optional<std::string> user_auth;
  std::optional<std::vector<std::string>> auth_allowed;
};
struct AuthLoginWithPasswordResult {
  std::optional<Error> error;
  std::optional<Verification> verification_required;
  std::optional<OptionalNetwork> network;
};
struct AuthCodeLoginResult { std::optional<Error> error; std::string by_jwt; };
using AuthVerifyResult = AuthLoginResult;
struct NetworkCreateArgs {
  std::string user_name, network_name, user_auth, password, auth_jwt_type, auth_jwt, referral_code;
  bool terms = false, verify_use_numeric = false, result_errors = false;
  std::optional<WalletAuthArgs> wallet_auth;
};
struct NetworkCreateResult {
  std::optional<Error> error;
  std::optional<Verification> verification_required;
  std::optional<OptionalNetwork> network;
  std::optional<std::string> seedphrase;
};
struct AuthNetworkClientArgs { std::string description; std::string device_spec; };
struct AuthNetworkClientResult {
  std::optional<Error> error;
  std::optional<std::string> by_client_jwt;
  std::optional<std::string> client_id{};
};
struct SessionSignOutResult {};
struct RemoveNetworkClientArgs { std::optional<std::string> client_id; };
struct RemoveNetworkClientResult { std::optional<Error> error; };
using AuthNetworkClientCallback = std::function<void(std::optional<AuthNetworkClientResult>,
                                                    std::optional<std::string>)>;
}
enum class AuthState { LoggedOut, Authenticating, LoggedIn, Error };
struct AuthResult {
  bool ok = false;
  bool verification_required = false;
  std::string error;
  int verify_send = 0;
  bool auth_needs_network = false, wallet_needs_network = false;
};
int VerifySendNoticeOf(const std::optional<urnet::Error>&) { return 0; }
std::string NormalizeSeedphrase(const std::string& value) { return value; }
std::optional<std::string> JwtClaimString(const std::string&, const std::string&) { return "synthetic network"; }
struct WalletConnect { enum class Provider { Solana, Bittensor }; };
namespace bittensor {
inline constexpr const char* kWalletTalisman = "synthetic-wallet";
inline constexpr const char* kPurposeCreate = "synthetic-create";
}
struct BittensorProofOutcome {
  bool ok = false;
  std::string error;
  struct Proof { std::string Address, Message, Signature; } proof;
};
namespace signout {
enum class Delivery { Delivered };
struct Service {};
const char* ToString(Delivery) { return "delivered"; }
struct SignOut { Delivery Begin(Service) { return Delivery::Delivered; } };
}

// The hook runs after the production helper releases its admission lock. The
// overloads let the unchanged harness exercise both the old split admission
// and epoch read, and the corrected single decision.
template <class SignIn>
struct ProbedSignOutRun {
  urnw::signoutrun::SignOutRun<SignIn> run;
  std::function<void()> afterAdmission;
  std::function<void()> afterFinish;
  bool Begin() { return run.Begin(); }
  auto Finish() {
    auto next = run.Finish();
    if (afterFinish) std::exchange(afterFinish, {})();
    return next;
  }
  auto Epoch() const { return run.Epoch(); }
  bool Running() const { return run.Running(); }
  bool Defer(SignIn signIn, std::optional<SignIn>& replaced) {
    bool deferred = false;
    if constexpr (requires { run.Defer(signIn, replaced); }) {
      deferred = run.Defer(std::move(signIn), replaced);
    } else {
      std::uint64_t epoch = 0;
      deferred = run.Defer(std::move(signIn), replaced, epoch);
    }
    if (!deferred && afterAdmission) std::exchange(afterAdmission, {})();
    return deferred;
  }
  bool Defer(SignIn signIn, std::optional<SignIn>& replaced, std::uint64_t epoch) {
    bool deferred = false;
    if constexpr (requires { run.Defer(signIn, replaced, epoch); }) {
      deferred = run.Defer(std::move(signIn), replaced, epoch);
    } else {
      deferred = run.Defer(std::move(signIn), replaced);
      epoch = run.Epoch();
    }
    if (!deferred && afterAdmission) std::exchange(afterAdmission, {})();
    return deferred;
  }
  template <class F> bool ApplyIfCurrent(std::uint64_t epoch, F&& apply) {
    if constexpr (requires { run.ApplyIfCurrent(epoch, std::forward<F>(apply)); }) {
      return run.ApplyIfCurrent(epoch, std::forward<F>(apply));
    } else {
      if (epoch != run.Epoch()) return false;
      apply();
      return true;
    }
  }
};

struct LocalState {
  std::string networkJwt = "initial-network";
  std::string clientJwt = "initial-client";
  std::function<void()> beforeClientWrite;
  bool failClientWrite = false;
  std::string getByJwt() { return networkJwt; }
  void setByJwt(const std::string& value) { networkJwt = value; }
  void setByClientJwt(const std::string& value) {
    if (beforeClientWrite) std::exchange(beforeClientWrite, {})();
    if (failClientWrite) throw std::runtime_error("synthetic storage failure");
    clientJwt = value;
  }
  void logout() { networkJwt.clear(); clientJwt.clear(); }
};
struct AsyncLocalState {
  LocalState& state;
  std::vector<std::function<void()>> writes;
  void logout(std::function<void(bool)> done) {
    writes.push_back([this, done] { state.logout(); done(true); });
  }
  void Drain() {
    auto pending = std::exchange(writes, {});
    for (auto& write : pending) write();
  }
};
template <class Result> using ApiAnswer = std::function<void(std::optional<Result>, std::optional<std::string>)>;
struct Api {
  std::string jwt = "initial-network";
  std::vector<ApiAnswer<urnet::AuthLoginResult>> logins;
  std::vector<ApiAnswer<urnet::AuthLoginWithPasswordResult>> passwords;
  std::vector<ApiAnswer<urnet::AuthCodeLoginResult>> codes;
  std::vector<ApiAnswer<urnet::AuthVerifyResult>> verifies;
  std::vector<ApiAnswer<urnet::NetworkCreateResult>> creates;
  std::function<void()> onSignOut;
  bool duplicateSignOutAnswer = false;
  int signOutRequests = 0;
  struct RemovedClient { std::optional<std::string> clientId; std::string networkJwt; };
  std::vector<RemovedClient> removedClients;
  void signOut(std::function<void(urnet::SessionSignOutResult, std::optional<std::string>)> answer) {
    ++signOutRequests;
    if (onSignOut) onSignOut();
    answer({}, {});
    if (duplicateSignOutAnswer) answer({}, {});
  }
  void removeNetworkClient(const std::optional<urnet::RemoveNetworkClientArgs>& args,
                           ApiAnswer<urnet::RemoveNetworkClientResult> answer) {
    removedClients.push_back({.clientId = args ? args->client_id : std::optional<std::string>{}, .networkJwt = jwt});
    answer(urnet::RemoveNetworkClientResult{}, {});
  }
  void setByJwt(std::string value) { jwt = std::move(value); }
  void authLogin(const urnet::AuthLoginArgs&, ApiAnswer<urnet::AuthLoginResult> answer) { logins.push_back(std::move(answer)); }
  void authLoginWithPassword(const urnet::AuthLoginWithPasswordArgs&, ApiAnswer<urnet::AuthLoginWithPasswordResult> answer) { passwords.push_back(std::move(answer)); }
  void authCodeLogin(const urnet::AuthCodeLoginArgs&, ApiAnswer<urnet::AuthCodeLoginResult> answer) { codes.push_back(std::move(answer)); }
  void authVerify(const urnet::AuthVerifyArgs&, ApiAnswer<urnet::AuthVerifyResult> answer) { verifies.push_back(std::move(answer)); }
  void networkCreate(const urnet::NetworkCreateArgs&, ApiAnswer<urnet::NetworkCreateResult> answer) { creates.push_back(std::move(answer)); }
};
struct Device { void close() {} };
struct Events { void NewSession() {} };
struct Worker { int requests = 0; void Request() { ++requests; } };

// The old handler took two arguments. Supporting it as well makes the original
// production method bodies executable as a negative control, without rewriting
// their behavior in the harness.
struct AuthCallback {
  std::function<void(AuthState, const std::string&, std::uint64_t)> callback;
  explicit operator bool() const { return static_cast<bool>(callback); }
  void operator()(AuthState state, const std::string& error, std::uint64_t epoch = 0) {
    callback(state, error, epoch);
  }
};

struct SdkHost {
  struct DeferredSignIn { std::string byJwt; std::function<void(AuthResult)> done; std::uint64_t epoch = 0; };
  enum class LoginRoute { Error, Login, Password, IncorrectAuth, Create };
  struct LoginRouting { LoginRoute route = LoginRoute::Error; std::string userAuth, error, authAllowed; };
  struct CreateNetworkParams {
    bool useWalletAuth = false, useAuthJwt = false, terms = true;
    std::string networkName, userAuth, password, referralCode;
  };
  struct InstantAccount { bool ok = false; std::string error, seedphrase; };
  struct ApprovedNetwork {
    enum class Failure { None, Transport, Refused };
    bool ok = false;
    Failure failure = Failure::None;
    std::string networkName, error;
  };
  struct Request { std::string networkJwt; urnet::AuthNetworkClientCallback callback; };
  struct SessionRequest {};
  ProbedSignOutRun<DeferredSignIn> signOutRun_;
  std::mutex mutex_, pendingMutex_;
  std::atomic<bool> loggedIn_{true};
  std::atomic<AuthState> authState_{AuthState::LoggedIn};
  AuthCallback onAuth_;
  std::function<void(bool)> onSignOutProgress_;
  LocalState local;
  AsyncLocalState async{local, {}};
  Api api;
  LocalState* localState_ = &local;
  AsyncLocalState* asyncLocalState_ = &async;
  Api* api_ = &api;
  std::optional<Device> device_;
  std::optional<Events> events_;
  std::optional<urnet::WalletAuthArgs> pendingWalletAuth_;
  std::optional<std::string> pendingAuthJwt_, pendingInstantJwt_, pendingApprovedJwt_;
  std::uint64_t pendingInstantEpoch_ = 0, pendingApprovedEpoch_ = 0;
  std::uint64_t pendingAuthEpoch_ = 0, pendingWalletEpoch_ = 0;
  std::string pendingBittensorWalletId_, walletSignMessage_;
  std::function<void(bool, std::string, std::string, std::string)> walletSignDone_;
  struct Wallet { void SignMessage(const std::string&) {} } wallet_;
  struct Flow {
    std::uint64_t current = 1;
    std::uint64_t Current() const { return current; }
    bool IsCurrent(std::uint64_t value) const { return current == value; }
  } walletFlows_;
  std::function<void(AuthResult)> walletAuthDone_;
  std::atomic<std::uint64_t> approvalFlow_{1};
  std::string pendingAuthJwtType_, sessionFailure_;
  SessionRequest pending_;
  bool pendingRequested_ = false;
  std::atomic<bool> serviceRecoveryNeeded_{false}, serviceProviderRunning_{false}, serviceProviderNetworkKey_{false};
  std::atomic<int> serviceProviderMode_{0}, serviceProviderClients_{-1};
  std::condition_variable pendingCv_, watchdogCv_;
  Worker signOutWorker_;
  signout::SignOut signOut_;
  std::vector<Request> requests;
  std::vector<AuthResult> answers;
  std::vector<AuthState> authEvents;
  int reconciles = 0;
  mutable std::function<void()> afterCurrentCheck;

  SdkHost() {
    onAuth_.callback = [this](AuthState state, const std::string&, std::uint64_t) { authEvents.push_back(state); };
  }
  void RegisterNetworkClient(const std::string&, std::function<void(AuthResult)>, std::uint64_t);
  void ServerSignOutLocked();
  void RemoveAbandonedClientLocked(const std::optional<std::string>&);
  void Logout();
  void SignOutTail();
  void SignOutTailLocked();
  void SetAuthState(AuthState, const std::string& = {});
  bool SetRegistrationAuthState(std::uint64_t, AuthState, const std::string& = {});
  bool ContinueAuthAttempt(std::uint64_t, const std::function<void(AuthResult)>&);
  bool SigningOut() const { return signOutRun_.Running(); }
  void SetSignOutProgressHandler(std::function<void(bool)> handler) { onSignOutProgress_ = std::move(handler); }
  void LoginWithPassword(const std::string&, const std::string&, std::function<void(AuthResult)>);
  void LoginWithCode(const std::string&, std::function<void(AuthResult)>);
  void LoginWithSeedphrase(const std::string&, std::function<void(AuthResult)>);
  void VerifyCode(const std::string&, const std::string&, std::function<void(AuthResult)>);
  void StartLogin(const std::string&, std::function<void(LoginRouting)>);
  void CreateNetwork(const CreateNetworkParams&, std::function<void(AuthResult)>);
  void SubmitCreateNetwork(const CreateNetworkParams&, std::optional<urnet::WalletAuthArgs>,
                           std::function<void(AuthResult)>, std::uint64_t);
  void CreateInstantAccount(std::function<void(InstantAccount)>);
  void ConfirmInstantAccount(std::function<void(AuthResult)>);
  void ConfirmApprovedNetwork(std::function<void(AuthResult)>);
  void RedeemApprovedCode(const std::string&, std::function<void(ApprovedNetwork)>);
  void AuthLoginWithSso(const std::string&, const std::string&, std::function<void(AuthResult)>, std::uint64_t);
  void AuthLoginWithWallet(const std::string&, const std::string&, const std::string&,
                           WalletConnect::Provider, std::uint64_t);
  void ApplySignupPreferences(urnet::NetworkCreateArgs&) const {}
  template <class F> void BeginBittensorProof(std::uint64_t, const std::string&, const std::string&,
                                              const std::string&, F) {}
  template <class F> void RequestWalletChallenge(const std::string&, const std::string&, F) {}
  bool AuthEventIsCurrent(std::uint64_t epoch) const {
    const bool current = signOutRun_.Epoch() == epoch;
    if (current && afterCurrentCheck) std::exchange(afterCurrentCheck, {})();
    return current;
  }
  template <class F> void SetAuthStateHandler(F handler) {
    onAuth_.callback = [handler](AuthState state, const std::string& error, std::uint64_t epoch) {
      if constexpr (std::is_invocable_v<F, AuthState, const std::string&, std::uint64_t>) handler(state, error, epoch);
      else handler(state, error);
    };
  }
  void AuthNetworkClientWithLocale(const urnet::AuthNetworkClientArgs&, urnet::AuthNetworkClientCallback callback) {
    requests.push_back({api.jwt, std::move(callback)});
  }
  std::string DeviceDescription() { return "synthetic-device"; }
  std::string DeviceSpec() { return "synthetic-spec"; }
  void TeardownSessionLocked(bool = true) { device_.reset(); }
  std::uint64_t CancelPendingWalletFlows(const char*) {
    ++walletFlows_.current;
    if (auto done = std::exchange(walletAuthDone_, {})) done(AuthResult{false, false, "superseded"});
    return walletFlows_.current;
  }
  void CancelStartWait(const char*) {}
  void ScheduleServiceRetry() {}
  signout::Service SignOutServiceLocked() { return {}; }
  void RequestProviderReconcile(const char*) { ++reconciles; }
  void Start(const std::string& jwt) {
    RegisterNetworkClient(jwt, [this](AuthResult result) { answers.push_back(result); }, signOutRun_.Epoch());
  }
  void Answer(std::size_t index, std::optional<urnet::AuthNetworkClientResult> result,
              std::optional<std::string> error = {}) {
    Check(index < requests.size(), "the expected registration reached the transport");
    if (index >= requests.size()) return;
    auto callback = std::exchange(requests[index].callback, {});
    Check(static_cast<bool>(callback), "a registration completion is consumed once");
    if (callback) callback(std::move(result), std::move(error));
  }
  void SignIn(const std::string& jwt) {
    const auto index = requests.size();
    Start(jwt);
    Answer(index, urnet::AuthNetworkClientResult{{}, jwt + "-client"});
  }
};

namespace winrt::URnetwork::implementation {
struct MainWindow {
  bool signingOut = false;
  void SetSigningOut(bool value) { signingOut = value; }
};
}
struct Window {
  winrt::URnetwork::implementation::MainWindow view;
  explicit operator bool() const { return true; }
  template <class T> T* try_as() { return &view; }
};
struct AppController {
  SdkHost& sdk_;
  std::vector<std::function<void()>> queued;
  std::function<void()> beforeEnqueue;
  std::vector<AuthState> delivered;
  AuthState state = AuthState::LoggedIn;
  bool windowVisible_ = true;
  Window window_;
  explicit AppController(SdkHost& sdk) : sdk_(sdk) { WireAuth(); WireProgress(); }
  void WireAuth();
  void WireProgress();
  void UpdateTray() {}
  template <class F> void OnUi(F callback) {
    if (beforeEnqueue) std::exchange(beforeEnqueue, {})();
    queued.push_back(std::move(callback));
  }
  void OnAuthState(AuthState value, const std::string&) { state = value; delivered.push_back(value); }
  void Drain() {
    auto pending = std::exchange(queued, {});
    for (auto& callback : pending) callback();
  }
};

// Exact current production bodies, supplied by the Go test driver.
#include "sign-out-registration-source.inc"

void TestAdmissionAndEpochAreOneDecision() {
  SdkHost host;
  host.signOutRun_.afterAdmission = [&] {
    host.Logout();
    host.SignOutTail();
    host.SignIn("next-network");
  };
  host.Start("old-network");
  // Before the fix the old attempt borrows the new epoch, issues a request
  // under its old network and can overwrite the account that just signed in.
  if (host.requests.size() > 1) host.Answer(1, urnet::AuthNetworkClientResult{{}, "old-client"});
  Check(host.requests.size() == 1, "logout between admission and epoch capture cancels the old registration before dispatch");
  Check(host.api.jwt == "next-network" && host.local.clientJwt == "next-network-client",
        "the old admission cannot replace the next account's API or stored credential");
}

void TestEveryStaleCompletionLeavesTheNextAccountAlone() {
  for (int outcome = 0; outcome < 5; ++outcome) {
    SdkHost host;
    host.Start("old-network");
    host.Logout();
    host.SignOutTail();
    host.SignIn("next-network");
    const auto events = host.authEvents.size();
    switch (outcome) {
      case 0: host.Answer(0, {}, "transport failed"); break;
      case 1: host.Answer(0, {}); break;
      case 2: host.Answer(0, urnet::AuthNetworkClientResult{urnet::Error{"refused"}, {}}); break;
      case 3: host.Answer(0, urnet::AuthNetworkClientResult{}); break;
      case 4: host.Answer(0, urnet::AuthNetworkClientResult{{}, "old-client"}); break;
    }
    Check(host.authState_ == AuthState::LoggedIn && host.authEvents.size() == events,
          "stale registration outcome " + std::to_string(outcome) + " publishes no auth state for the next account");
    Check(host.local.clientJwt == "next-network-client", "stale registration leaves the next credential intact");
    Check(host.answers.size() == 2 && !host.answers.back().ok, "the stale caller is answered once as cancelled");
  }
}

void TestLogoutFinishesItsCredentialWriteBeforeReleasingSignIn() {
  SdkHost host;
  host.Logout();
  host.Start("next-network");
  Check(host.requests.empty(), "the new account waits for the logout tail");
  host.SignOutTail();
  Check(host.async.writes.empty(), "Finish must not leave a destructive credential write queued");
  host.Answer(0, urnet::AuthNetworkClientResult{{}, "next-client"});
  host.async.Drain();
  Check(host.local.networkJwt == "next-network" && host.local.clientJwt == "next-client",
        "draining the old account's writes cannot erase the released sign-in");
}

void TestLogoutDuringPersistenceDoesNotRestoreSignedInState() {
  SdkHost host;
  host.Start("old-network");
  host.local.beforeClientWrite = [&] { host.Logout(); };
  host.Answer(0, urnet::AuthNetworkClientResult{{}, "old-client"});
  Check(!host.loggedIn_, "logout during credential persistence keeps connect admission closed");
  Check(host.authEvents.empty(), "logout during persistence prevents a terminal LoggedIn publication");
  Check(host.answers.size() == 1 && !host.answers.back().ok, "a registration invalidated during persistence is cancelled");
  host.SignOutTail();
}

void TestQueuedOldAuthEventsDoNotReachTheNextAccount() {
  for (bool success : {false, true}) {
    SdkHost host;
    AppController controller(host);
    host.Start("old-network");
    controller.beforeEnqueue = [&] {
      host.Logout();
      host.SignOutTail();
      host.SignIn("next-network");
    };
    if (success) host.Answer(0, urnet::AuthNetworkClientResult{{}, "old-client"});
    else host.Answer(0, {}, "old transport failure");
    controller.Drain();
    Check(controller.state == AuthState::LoggedIn, "a queued old error cannot replace the next account's screen");
    Check(controller.delivered == std::vector<AuthState>{AuthState::LoggedOut, AuthState::LoggedIn},
          "queued old auth events do not restart or overwrite the next account's UI lifecycle");
  }
}

void TestFailedCredentialStorageDoesNotReportSuccessfulSignIn() {
  SdkHost host;
  host.local.logout();
  host.loggedIn_ = false;
  host.authState_ = AuthState::LoggedOut;
  host.local.failClientWrite = true;
  host.SignIn("new-network");
  Check(host.answers.size() == 1 && !host.answers.front().ok && !host.answers.front().error.empty(),
        "a failed credential write answers the sign-in with a storage error");
  Check(!host.loggedIn_ && host.authState_ == AuthState::Error &&
            host.authEvents == std::vector<AuthState>{AuthState::Error},
        "failed credential storage never publishes LoggedIn");
  Check(host.reconciles == 0, "failed credential storage never starts a provider reconcile");
}

// Return one of the real entrypoints' API responses when the test chooses.
// The network and identity strings are synthetic and never leave this process.
std::function<void(int)> BeginAuth(SdkHost& host, int kind) {
  const auto done = [&host](AuthResult result) { host.answers.push_back(result); };
  if (kind == 0) {
    host.LoginWithPassword("old@example.invalid", "synthetic-password", done);
    return [&host](int outcome) {
      urnet::AuthLoginWithPasswordResult result;
      if (outcome == 0) result.network = urnet::OptionalNetwork{"old-network"};
      else if (outcome == 1) result.error = urnet::Error{"old refusal"};
      else if (outcome == 4) result.verification_required = urnet::Verification{};
      host.api.passwords.at(0)(outcome == 3 ? std::nullopt : std::optional(result),
                                outcome == 2 ? std::optional<std::string>("old transport failure") : std::nullopt);
    };
  }
  if (kind == 1) {
    host.LoginWithCode("synthetic-code", done);
    return [&host](int outcome) {
      urnet::AuthCodeLoginResult result;
      if (outcome == 0) result.by_jwt = "old-network";
      else if (outcome == 1) result.error = urnet::Error{"old refusal"};
      host.api.codes.at(0)(outcome == 3 ? std::nullopt : std::optional(result),
                            outcome == 2 ? std::optional<std::string>("old transport failure") : std::nullopt);
    };
  }
  if (kind == 2) host.LoginWithSeedphrase("synthetic phrase", done);
  else if (kind == 3) host.StartLogin("old@example.invalid", [&host](SdkHost::LoginRouting routed) {
    host.answers.push_back(AuthResult{routed.route != SdkHost::LoginRoute::Error, false, routed.error});
  });
  else if (kind == 4) host.VerifyCode("old@example.invalid", "synthetic-code", done);
  else if (kind == 5) {
    host.CreateNetwork({}, done);
    return [&host](int outcome) {
      urnet::NetworkCreateResult result;
      if (outcome == 0) result.network = urnet::OptionalNetwork{"old-network"};
      else if (outcome == 1) result.error = urnet::Error{"old refusal"};
      else if (outcome == 4) result.verification_required = urnet::Verification{};
      host.api.creates.at(0)(outcome == 3 ? std::nullopt : std::optional(result),
                              outcome == 2 ? std::optional<std::string>("old transport failure") : std::nullopt);
    };
  } else if (kind == 6) host.AuthLoginWithSso("synthetic-provider", "synthetic-token", done, host.signOutRun_.Epoch());
  else if (kind == 7) {
    host.walletAuthDone_ = done;
    host.AuthLoginWithWallet("synthetic-address", "synthetic-signature", "synthetic-message", WalletConnect::Provider::Solana, host.signOutRun_.Epoch());
  }
  return [&host, kind](int outcome) {
    urnet::AuthLoginResult result;
    if (outcome == 0) result.network = urnet::Network{"old-network"};
    else if (outcome == 1) result.error = urnet::Error{"old refusal"};
    const auto answer = kind == 4 ? host.api.verifies.at(0) : host.api.logins.at(0);
    answer(outcome == 3 ? std::nullopt : std::optional(result),
             outcome == 2 ? std::optional<std::string>("old transport failure") : std::nullopt);
  };
}

void TestDelayedAuthResponsesKeepTheirOriginalEpoch() {
  for (int kind = 0; kind < 8; ++kind) {
    for (int outcome = 0; outcome < 5; ++outcome) {
      SdkHost host;
      const auto answer = BeginAuth(host, kind);
      host.Logout();
      host.SignOutTail();
      host.SignIn("next-network");
      const auto events = host.authEvents.size();
      const auto reconciles = host.reconciles;
      int newerWalletAnswers = 0;
      host.walletAuthDone_ = [&](AuthResult) { ++newerWalletAnswers; };
      answer(outcome);
      const auto label = "auth entry " + std::to_string(kind) + " outcome " + std::to_string(outcome);
      Check(host.requests.size() == 1 && host.api.jwt == "next-network", label + " cannot begin an old registration under the next epoch");
      Check(host.authEvents.size() == events && host.authState_ == AuthState::LoggedIn && host.reconciles == reconciles,
            label + " publishes no old state or provider request");
      Check(host.local.clientJwt == "next-network-client", label + " leaves the next account stored");
      Check(host.answers.size() == 2 && std::count_if(host.answers.begin(), host.answers.end(),
              [](const AuthResult& result) {
                return !result.ok && !result.verification_required &&
                    (result.error.find("signed out") != std::string::npos || result.error == "superseded");
              }) == 1,
            label + " answers the old caller exactly once as cancelled");
      Check(!host.pendingWalletAuth_ && !host.pendingAuthJwt_, label + " retains no old identity for account creation");
      Check(newerWalletAnswers == 0 && static_cast<bool>(host.walletAuthDone_), label + " leaves the newer wallet caller alone");
    }
  }
}

void TestLogoutBetweenAuthCheckAndRegistrationCannotAdoptTheNewEpoch() {
  for (int kind = 0; kind < 7; ++kind) {
    SdkHost host;
    const auto answer = BeginAuth(host, kind);
    host.afterCurrentCheck = [&] {
      host.Logout();
      host.SignOutTail();
      host.SignIn("next-network");
    };
    answer(0);
    Check(host.requests.size() == 1 && host.local.clientJwt == "next-network-client" && host.api.jwt == "next-network",
          "auth entry " + std::to_string(kind) + " passes its original epoch through registration, even after its earlier check passed");
    Check(host.answers.size() == 2 && !host.answers.back().ok, "the invalidated auth attempt completes as cancelled");
  }
}

void TestAnOldAuthResponseCannotWaitBehindTheLogoutThatInvalidatedIt() {
  SdkHost host;
  const auto answer = BeginAuth(host, 0);
  host.Logout();
  answer(0);
  host.SignOutTail();
  Check(host.requests.empty() && host.local.clientJwt.empty(), "an old password response during the tail cannot queue itself to sign back in");
  Check(host.answers.size() == 1 && !host.answers.front().ok, "an old response during the tail is cancelled once");
}

void TestADeferredRegistrationKeepsItsEpochWhenTheTailReleasesIt() {
  SdkHost host;
  host.Logout();
  host.Start("first-deferred-network");
  host.signOutRun_.afterFinish = [&] {
    host.Logout();
    host.SignOutTail();
    host.SignIn("next-network");
  };
  host.SignOutTail();
  Check(host.requests.size() == 1 && host.api.jwt == "next-network", "a second logout invalidates an already released deferred registration");
  Check(host.answers.size() == 2 && !host.answers.back().ok, "the released but invalidated registration is cancelled once");
}

void TestPendingConfirmationsCannotReuseTokensFromBeforeLogout() {
  for (bool approved : {false, true}) {
    SdkHost host;
    if (approved) host.pendingApprovedJwt_ = "old-network";
    else host.pendingInstantJwt_ = "old-network";
    host.Logout();
    const auto done = [&](AuthResult result) { host.answers.push_back(result); };
    if (approved) host.ConfirmApprovedNetwork(done);
    else host.ConfirmInstantAccount(done);
    host.SignOutTail();
    Check(host.requests.empty() && host.local.clientJwt.empty(), "a confirmation during the tail preserves the held token's old epoch");
    Check(host.answers.size() == 1 && !host.answers.front().ok, "the old held token is cancelled once");
  }
}

void TestAnOldApprovedRegistrationFailureCannotClearTheNextApiCredential() {
  SdkHost host;
  host.pendingApprovedJwt_ = "old-network";
  host.ConfirmApprovedNetwork([&](AuthResult result) { host.answers.push_back(result); });
  host.Logout();
  host.SignOutTail();
  host.SignIn("next-network");
  host.Answer(0, {}, "old transport failure");
  Check(host.api.jwt == "next-network", "the old approval's failure cleanup cannot clear the next account's API credential");
}

void TestInstantCreationCannotRetainAnOldTokenAfterLogout() {
  for (bool betweenChecks : {false, true}) {
    SdkHost host;
    std::vector<SdkHost::InstantAccount> answers;
    host.CreateInstantAccount([&](SdkHost::InstantAccount result) { answers.push_back(result); });
    const auto logoutAndSignIn = [&] { host.Logout(); host.SignOutTail(); host.SignIn("next-network"); };
    if (betweenChecks) host.afterCurrentCheck = logoutAndSignIn;
    else logoutAndSignIn();
    urnet::NetworkCreateResult result;
    result.network = urnet::OptionalNetwork{"old-network"};
    result.seedphrase = "synthetic phrase";
    host.api.creates.at(0)(result, {});
    Check(!host.pendingInstantJwt_, "instant creation cannot retain an old token after logout, including between its check and storage");
    Check(answers.size() == 1 && !answers.front().ok, "old instant creation answers once as cancelled");
  }
}

void TestCreationCannotReuseAnOldWalletOrSsoIdentityDuringLogout() {
  for (bool wallet : {false, true}) {
    SdkHost host;
    if (wallet) host.pendingWalletAuth_ = urnet::WalletAuthArgs{};
    else host.pendingAuthJwt_ = "synthetic-old-token";
    host.Logout();
    SdkHost::CreateNetworkParams params;
    params.useWalletAuth = wallet;
    params.useAuthJwt = !wallet;
    host.CreateNetwork(params, [&](AuthResult result) { host.answers.push_back(result); });
    Check(host.api.creates.empty(), "creation cannot dispatch under an identity retained before logout");
    Check(host.answers.size() == 1 && !host.answers.front().ok,
          "creation from an old retained identity is cancelled once");
    host.SignOutTail();
  }
}

void TestCurrentAuthResponsesStillCompleteAfterLogout() {
  for (int kind = 0; kind < 8; ++kind) {
    SdkHost host;
    host.Logout();
    host.SignOutTail();
    const auto answer = BeginAuth(host, kind);
    answer(0);
    Check(host.requests.size() == 1, "a fresh auth entry still reaches registration after logout");
    host.Answer(0, urnet::AuthNetworkClientResult{{}, "fresh-client"});
    Check(host.answers.size() == 1 && host.answers.front().ok && host.local.clientJwt == "fresh-client" &&
              host.authState_ == AuthState::LoggedIn && host.loggedIn_,
          "a fresh auth entry completes its own sign-in after logout");
  }
}

void TestHeldConfirmationsRememberTheEpochInWhichTheyWereCreated() {
  for (bool approved : {false, true}) {
    SdkHost host;
    host.Logout();
    host.SignOutTail();
    if (approved) {
      host.approvalFlow_ = host.walletFlows_.Current();
      host.RedeemApprovedCode("synthetic-code", [&](SdkHost::ApprovedNetwork result) {
        Check(result.ok, "current approval is held for confirmation");
      });
      host.api.codes.at(0)(urnet::AuthCodeLoginResult{{}, "fresh-network"}, {});
      Check(host.pendingApprovedEpoch_ == host.signOutRun_.Epoch(), "the approval records its originating logout epoch");
      host.ConfirmApprovedNetwork([&](AuthResult result) { host.answers.push_back(result); });
    } else {
      host.CreateInstantAccount([&](SdkHost::InstantAccount result) {
        Check(result.ok, "current instant account is held for confirmation");
      });
      urnet::NetworkCreateResult result;
      result.network = urnet::OptionalNetwork{"fresh-network"};
      result.seedphrase = "synthetic phrase";
      host.api.creates.at(0)(result, {});
      Check(host.pendingInstantEpoch_ == host.signOutRun_.Epoch(), "the instant account records its originating logout epoch");
      host.ConfirmInstantAccount([&](AuthResult answer) { host.answers.push_back(answer); });
    }
    host.Answer(0, urnet::AuthNetworkClientResult{{}, "fresh-client"});
    Check(host.answers.size() == 1 && host.answers.front().ok, "a current held confirmation succeeds after an earlier logout");
  }
}

void TestRetainedIdentitiesRememberTheirCreationEpoch() {
  for (int kind : {6, 7}) {
    SdkHost host;
    host.Logout();
    host.SignOutTail();
    const auto answer = BeginAuth(host, kind);
    answer(4);
    Check(kind == 6 ? (host.pendingAuthJwt_ && host.pendingAuthEpoch_ == host.signOutRun_.Epoch()) :
                     (host.pendingWalletAuth_ && host.pendingWalletEpoch_ == host.signOutRun_.Epoch()),
          "a retained SSO or wallet identity records its originating logout epoch");
  }
}

void TestAnOldBrowserOrWalletHandoffDoesNotStartAuth() {
  for (bool wallet : {false, true}) {
    SdkHost host;
    const auto epoch = host.signOutRun_.Epoch();
    host.Logout();
    host.SignOutTail();
    host.SignIn("next-network");
    const auto events = host.authEvents.size();
    if (wallet) host.AuthLoginWithWallet("synthetic-address", "synthetic-signature", "synthetic-message",
                                         WalletConnect::Provider::Solana, epoch);
    else host.AuthLoginWithSso("synthetic-provider", "synthetic-token", [&](AuthResult result) {
      Check(!result.ok, "old browser handoff is answered as cancelled");
    }, epoch);
    Check(host.api.logins.empty() && host.authEvents.size() == events, "an invalidated browser or wallet handoff cannot send an auth API call");
  }
}

void TestQueuedSignOutProgressReadsTheCurrentTail() {
  SdkHost host;
  AppController controller(host);
  host.Logout();
  const auto progress = host.onSignOutProgress_;
  host.onSignOutProgress_ = [&](bool running) {
    if (!running) controller.beforeEnqueue = [&] { host.Logout(); };
    progress(running);
  };
  host.SignOutTail();
  controller.Drain();
  Check(host.SigningOut() && controller.window_.view.signingOut,
        "an old false sign-out progress event cannot hide a second logout still running");
  host.onSignOutProgress_ = progress;
  host.SignOutTail();
  controller.Drain();
  Check(!controller.window_.view.signingOut, "the sign-out row settles after the current tail finishes");
}
// The production tail must start server cleanup before either credential is
// erased. Duplicate local answers also exercise its exactly-once promise.
void TestCurrentServerCleanupDependencies() {
  SdkHost host;
  host.api.duplicateSignOutAnswer = true;
  host.api.onSignOut = [&] {
    Check(host.local.networkJwt == "initial-network" && host.local.clientJwt == "initial-client" &&
              host.api.jwt == "initial-network",
          "server cleanup starts before local and API credentials are cleared");
  };
  host.Logout();
  host.SignOutTail();
  Check(host.api.signOutRequests == 1, "the production tail invokes server sign-out exactly once");
  Check(host.local.networkJwt.empty() && host.local.clientJwt.empty() && host.api.jwt.empty(),
        "the production tail still clears local and API credentials after server cleanup");
  std::cout << "server-cleanup dependency checks passed\n";
}

// Failed installation must remove the ID returned by the registration, not a
// guessed JWT or a later account identity. No external API is contacted.
void TestCurrentAbandonedClientDependency() {
  SdkHost host;
  host.local.failClientWrite = true;
  host.Start("synthetic-network");
  urnet::AuthNetworkClientResult result;
  result.by_client_jwt = "synthetic-client-jwt";
  result.client_id = "synthetic-client-id";
  host.Answer(0, result);
  Check(host.api.removedClients.size() == 1, "failed registration removes its returned client ID");
  if (!host.api.removedClients.empty()) {
    Check(host.api.removedClients.front().clientId == result.client_id &&
              host.api.removedClients.front().networkJwt == "synthetic-network",
          "abandoned cleanup uses the exact response ID while its network credential is present");
  }
  Check(host.answers.size() == 1 && !host.answers.front().ok,
        "abandoned cleanup preserves the failed registration completion");
  host.RemoveAbandonedClientLocked({});
  host.RemoveAbandonedClientLocked(std::string{});
  Check(host.api.removedClients.size() == 1, "missing or empty client IDs never dispatch removal");
  std::cout << "abandoned-client dependency checks passed\n";
}
}  // namespace

int main(int argc, char** argv) {
  if (argc == 2) {
    const std::string mode = argv[1];
    if (mode == "server-cleanup") TestCurrentServerCleanupDependencies();
    else if (mode == "abandoned-client") TestCurrentAbandonedClientDependency();
    else { std::cerr << "unknown synthetic test mode\n"; return 2; }
    return failures ? 1 : 0;
  }
  TestCurrentServerCleanupDependencies();
  TestCurrentAbandonedClientDependency();
  TestAdmissionAndEpochAreOneDecision();
  TestEveryStaleCompletionLeavesTheNextAccountAlone();
  TestLogoutFinishesItsCredentialWriteBeforeReleasingSignIn();
  TestLogoutDuringPersistenceDoesNotRestoreSignedInState();
  TestQueuedOldAuthEventsDoNotReachTheNextAccount();
  TestFailedCredentialStorageDoesNotReportSuccessfulSignIn();
  TestDelayedAuthResponsesKeepTheirOriginalEpoch();
  TestLogoutBetweenAuthCheckAndRegistrationCannotAdoptTheNewEpoch();
  TestAnOldAuthResponseCannotWaitBehindTheLogoutThatInvalidatedIt();
  TestADeferredRegistrationKeepsItsEpochWhenTheTailReleasesIt();
  TestPendingConfirmationsCannotReuseTokensFromBeforeLogout();
  TestAnOldApprovedRegistrationFailureCannotClearTheNextApiCredential();
  TestInstantCreationCannotRetainAnOldTokenAfterLogout();
  TestCreationCannotReuseAnOldWalletOrSsoIdentityDuringLogout();
  TestCurrentAuthResponsesStillCompleteAfterLogout();
  TestHeldConfirmationsRememberTheEpochInWhichTheyWereCreated();
  TestRetainedIdentitiesRememberTheirCreationEpoch();
  TestAnOldBrowserOrWalletHandoffDoesNotStartAuth();
  TestQueuedSignOutProgressReadsTheCurrentTail();
  std::cout << (checks - failures) << '/' << checks << " registration/logout checks passed\n";
  return failures ? 1 : 0;
}
