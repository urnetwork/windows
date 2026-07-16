// SPDX-License-Identifier: MPL-2.0
// the project compiles with /Yu"pch.h" (App.vcxproj), so every translation unit
// must include it first
#include "pch.h"

#include "SdkHost.h"

#include <algorithm>
#include <fstream>
#include <random>
#include <thread>
#include <unordered_map>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "Ids.h"
#include "Log.h"
#include "Paths.h"
#include "Strings.h"

namespace urnw {
namespace {

// Persisted RPC session (last-good), mirroring macOS RpcSessionStore. Lets the
// app reattach its DeviceRemote to a still-running service tunnel.
struct RpcSession {
  std::string client_pem;
  std::string server_cert_pem;
  std::string host_port;
};

void SaveRpcSession(const RpcSession& s) {
  nlohmann::json j = {{"client_pem", s.client_pem},
                      {"server_cert_pem", s.server_cert_pem},
                      {"host_port", s.host_port}};
  std::ofstream f(RpcSessionFile(), std::ios::trunc);
  if (f) f << j.dump();
}

std::optional<RpcSession> LoadRpcSession() {
  std::ifstream f(RpcSessionFile());
  if (!f) return std::nullopt;
  try {
    nlohmann::json j = nlohmann::json::parse(f);
    RpcSession s;
    s.client_pem = j.value("client_pem", "");
    s.server_cert_pem = j.value("server_cert_pem", "");
    s.host_port = j.value("host_port", "");
    if (s.host_port.empty()) return std::nullopt;
    return s;
  } catch (...) {
    return std::nullopt;
  }
}

void ClearRpcSession() {
  std::error_code ec;
  std::filesystem::remove(RpcSessionFile(), ec);
}

}  // namespace

SdkHost::~SdkHost() {
  std::scoped_lock lock(mutex_);
  subs_.clear();
}

std::string SdkHost::RandomLoopbackHostPort() {
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<int> dist(12000, 12100);
  return "127.0.0.1:" + std::to_string(dist(gen));
}

std::string SdkHost::DeviceDescription() {
  wchar_t name[MAX_COMPUTERNAME_LENGTH + 1];
  DWORD n = MAX_COMPUTERNAME_LENGTH + 1;
  if (::GetComputerNameW(name, &n)) return Narrow(std::wstring(name, n));
  return "windows-desktop";
}

std::string SdkHost::DeviceSpec() {
#if defined(_M_ARM64)
  return "windows arm64";
#else
  return "windows amd64";
#endif
}

urnet::NetworkSpace SdkHost::BuildNetworkSpace() {
  // Matches macOS DeviceManager.initializeNetworkSpace.
  urnet::NetworkSpaceKey key;
  key.host_name = std::string(ids::kNetworkSpaceHostName);
  key.env_name = std::string(ids::kNetworkSpaceEnvName);

  urnet::NetworkSpaceValues values;
  values.bundled = true;
  values.net_expose_server_ips = true;
  values.net_expose_server_host_names = true;
  values.link_host_name = "ur.io";
  values.migration_host_name = "bringyour.com";
  values.store = "";
  values.wallet = "circle";
  values.sso_google = false;
  values.env_secret = "";

  return spaceManager_->updateNetworkSpaceValues(key, values);
}

bool SdkHost::Initialize() {
  std::scoped_lock lock(mutex_);
  try {
    spaceManager_ =
        urnet::newNetworkSpaceManager(Narrow(SdkStorageDir(false).wstring()));
    networkSpace_ = BuildNetworkSpace();
    api_ = networkSpace_->getApi();
    asyncLocalState_ = networkSpace_->getAsyncLocalState();
    localState_ = asyncLocalState_->getLocalState();
    // sign-up network-name availability (bound once; api-scoped)
    networkNameVc_ = urnet::newNetworkNameValidationViewController(*api_);
    networkNameVc_->start();
    SetupWalletCallbacks();

    service_.SetStateHandler([this](const proto::TunnelStatus& st) {
      if (onTunnel_) onTunnel_(st);
    });
    service_.Connect();  // ok if the service isn't up yet; retried on demand

    if (!localState_->getByClientJwt().empty()) {
      SetAuthState(AuthState::LoggedIn);
      // resume the session (reattach or restart the tunnel) off the UI path
      std::thread([this] {
        std::scoped_lock lock(mutex_);
        BootstrapSession();
      }).detach();
    } else {
      SetAuthState(AuthState::LoggedOut);
    }
    return true;
  } catch (const std::exception& e) {
    LogError("sdkhost: initialize failed: {}", e.what());
    SetAuthState(AuthState::Error, e.what());
    return false;
  }
}

bool SdkHost::IsLoggedIn() {
  std::scoped_lock lock(mutex_);
  return localState_ && !localState_->getByClientJwt().empty();
}

void SdkHost::SetAuthState(AuthState s, const std::string& error) {
  authState_ = s;
  if (onAuth_) onAuth_(s, error);
}

void SdkHost::LoginWithPassword(const std::string& userAuth,
                                const std::string& password,
                                std::function<void(AuthResult)> done) {
  SetAuthState(AuthState::Authenticating);
  urnet::AuthLoginWithPasswordArgs args;
  args.user_auth = userAuth;
  args.password = password;
  // an unverified account gets a numeric one-time code (macOS parity); the UI
  // routes verification_required into the verify step
  args.verify_otp_numeric = true;

  api_->authLoginWithPassword(
      args, [this, done](std::optional<urnet::AuthLoginWithPasswordResult> result,
                         std::optional<std::string> err) {
        if (err || !result) {
          AuthResult r{false, false, err ? *err : "no result"};
          SetAuthState(AuthState::Error, r.error);
          if (done) done(r);
          return;
        }
        if (result->error && !result->error->message.empty()) {
          AuthResult r{false, false, result->error->message};
          SetAuthState(AuthState::Error, r.error);
          if (done) done(r);
          return;
        }
        if (result->verification_required) {
          AuthResult r{false, true, ""};
          SetAuthState(AuthState::LoggedOut);
          if (done) done(r);  // UI routes to the verify screen
          return;
        }
        if (result->network && result->network->by_jwt) {
          RegisterNetworkClient(*result->network->by_jwt, done);
        } else {
          AuthResult r{false, false, "login returned no network"};
          SetAuthState(AuthState::Error, r.error);
          if (done) done(r);
        }
      });
}

void SdkHost::LoginWithCode(const std::string& authCode,
                            std::function<void(AuthResult)> done) {
  SetAuthState(AuthState::Authenticating);
  urnet::AuthCodeLoginArgs args;
  args.auth_code = authCode;
  api_->authCodeLogin(args, [this, done](std::optional<urnet::AuthCodeLoginResult> result,
                                         std::optional<std::string> err) {
    if (err || !result) {
      AuthResult r{false, false, err ? *err : "no result"};
      SetAuthState(AuthState::Error, r.error);
      if (done) done(r);
      return;
    }
    if (result->error && !result->error->message.empty()) {
      AuthResult r{false, false, result->error->message};
      SetAuthState(AuthState::Error, r.error);
      if (done) done(r);
      return;
    }
    if (!result->by_jwt.empty()) {
      RegisterNetworkClient(result->by_jwt, done);
    } else {
      AuthResult r{false, false, "code login returned no jwt"};
      SetAuthState(AuthState::Error, r.error);
      if (done) done(r);
    }
  });
}

void SdkHost::LoginAsGuest(std::function<void(AuthResult)> done) {
  SetAuthState(AuthState::Authenticating);
  urnet::NetworkCreateArgs args;
  args.terms = true;  // the sheet's button is gated on the terms consent
  args.guest_mode = true;

  api_->networkCreate(args, [this, done](std::optional<urnet::NetworkCreateResult> result,
                                         std::optional<std::string> err) {
    if (err || !result) {
      AuthResult r{false, false, err ? *err : "no result"};
      SetAuthState(AuthState::Error, r.error);
      if (done) done(r);
      return;
    }
    if (result->error && !result->error->message.empty()) {
      AuthResult r{false, false, result->error->message};
      SetAuthState(AuthState::LoggedOut);  // a request error, not a session error
      if (done) done(r);
      return;
    }
    if (result->network && result->network->by_jwt && !result->network->by_jwt->empty()) {
      RegisterNetworkClient(*result->network->by_jwt, done);
      return;
    }
    AuthResult r{false, false, "guest create returned no network"};
    SetAuthState(AuthState::Error, r.error);
    if (done) done(r);
  });
}

// ---- account discovery / sign-up / verify / reset ---------------------------
// macOS Authenticate/** parity. All results are delivered on SDK callback
// threads; the UI marshals onto its thread.

void SdkHost::StartLogin(const std::string& userAuth,
                         std::function<void(LoginRouting)> done) {
  urnet::AuthLoginArgs args;
  args.user_auth = userAuth;

  api_->authLogin(args, [this, userAuth, done](std::optional<urnet::AuthLoginResult> result,
                                               std::optional<std::string> err) {
    LoginRouting routing;
    routing.userAuth = userAuth;
    if (err || !result) {
      routing.route = LoginRoute::Error;
      routing.error = err ? *err : "no result";
      if (done) done(routing);
      return;
    }
    if (result->user_auth && !result->user_auth->empty()) {
      routing.userAuth = *result->user_auth;  // the normalized echo
    }
    if (result->error && !result->error->message.empty()) {
      routing.route = LoginRoute::Error;
      routing.error = result->error->message;
      if (done) done(routing);
      return;
    }
    // a jwt straight from discovery (not the user-auth path, but handle it)
    if (result->network && !result->network->by_jwt.empty()) {
      RegisterNetworkClient(result->network->by_jwt, [done](AuthResult r) {
        LoginRouting routed;
        routed.route = r.ok ? LoginRoute::Login : LoginRoute::Error;
        routed.error = r.error;
        if (done) done(routed);
      });
      return;
    }
    if (result->auth_allowed && !result->auth_allowed->empty()) {
      const auto& allowed = *result->auth_allowed;
      if (std::find(allowed.begin(), allowed.end(), "password") != allowed.end()) {
        routing.route = LoginRoute::Password;
      } else {
        // the account exists under another sign-in method (e.g. a wallet)
        routing.route = LoginRoute::IncorrectAuth;
        for (const auto& method : allowed) {
          if (!routing.authAllowed.empty()) routing.authAllowed += ", ";
          routing.authAllowed += method;
        }
      }
      if (done) done(routing);
      return;
    }
    // unknown user auth: create a new network
    routing.route = LoginRoute::Create;
    if (done) done(routing);
  });
}

void SdkHost::CreateNetwork(const CreateNetworkParams& params,
                            std::function<void(AuthResult)> done) {
  SetAuthState(AuthState::Authenticating);
  urnet::NetworkCreateArgs args;
  args.user_name = std::string();
  args.network_name = params.networkName;
  args.terms = params.terms;
  args.verify_use_numeric = true;
  if (params.useWalletAuth) {
    std::scoped_lock lock(mutex_);
    if (!pendingWalletAuth_) {
      AuthResult r{false, false, "no wallet sign-in is pending"};
      SetAuthState(AuthState::Error, r.error);
      if (done) done(r);
      return;
    }
    args.wallet_auth = *pendingWalletAuth_;
  } else {
    args.user_auth = params.userAuth;
    args.password = params.password;
  }
  if (!params.referralCode.empty()) args.referral_code = params.referralCode;

  api_->networkCreate(args, [this, done](std::optional<urnet::NetworkCreateResult> result,
                                         std::optional<std::string> err) {
    if (err || !result) {
      AuthResult r{false, false, err ? *err : "no result"};
      SetAuthState(AuthState::Error, r.error);
      if (done) done(r);
      return;
    }
    if (result->error && !result->error->message.empty()) {
      AuthResult r{false, false, result->error->message};
      SetAuthState(AuthState::LoggedOut);  // a form error, not a session error
      if (done) done(r);
      return;
    }
    if (result->verification_required) {
      AuthResult r{false, true, ""};
      SetAuthState(AuthState::LoggedOut);
      if (done) done(r);  // the UI routes to the verify step
      return;
    }
    if (result->network && result->network->by_jwt && !result->network->by_jwt->empty()) {
      {
        std::scoped_lock lock(mutex_);
        pendingWalletAuth_.reset();  // consumed (wallet mode) or unused
      }
      RegisterNetworkClient(*result->network->by_jwt, done);
      return;
    }
    AuthResult r{false, false, "create network returned no network"};
    SetAuthState(AuthState::Error, r.error);
    if (done) done(r);
  });
}

void SdkHost::UpgradeGuest(const std::string& networkName, const std::string& userAuth,
                           const std::string& password,
                           std::function<void(AuthResult)> done) {
  // No auth-state pushes on request errors: unlike the sign-in flows the caller
  // is still signed in (as the guest), and the create step surfaces the error
  // inline. Success lands in RegisterNetworkClient, which pushes LoggedIn once
  // the device is re-registered under the upgraded network's jwt.
  urnet::UpgradeGuestArgs args;
  args.network_name = networkName;
  args.user_auth = userAuth;
  args.password = password;

  api_->upgradeGuest(args, [this, done](std::optional<urnet::UpgradeGuestResult> result,
                                        std::optional<std::string> err) {
    if (err || !result) {
      if (done) done({false, false, err ? *err : "no result"});
      return;
    }
    if (result->error && !result->error->message.empty()) {
      if (done) done({false, false, result->error->message});
      return;
    }
    if (result->verification_required) {
      if (done) done({false, true, ""});  // the UI routes to the verify step
      return;
    }
    if (result->network && result->network->by_jwt && !result->network->by_jwt->empty()) {
      RegisterNetworkClient(*result->network->by_jwt, done);
      return;
    }
    if (done) done({false, false, "guest upgrade returned no network"});
  });
}

void SdkHost::VerifyCode(const std::string& userAuth, const std::string& code,
                         std::function<void(AuthResult)> done) {
  SetAuthState(AuthState::Authenticating);
  urnet::AuthVerifyArgs args;
  args.user_auth = userAuth;
  args.verify_code = code;

  api_->authVerify(args, [this, done](std::optional<urnet::AuthVerifyResult> result,
                                      std::optional<std::string> err) {
    if (err || !result) {
      AuthResult r{false, false, err ? *err : "no result"};
      SetAuthState(AuthState::Error, r.error);
      if (done) done(r);
      return;
    }
    if (result->error && !result->error->message.empty()) {
      AuthResult r{false, false, result->error->message};
      SetAuthState(AuthState::LoggedOut);  // a wrong code, not a session error
      if (done) done(r);
      return;
    }
    if (result->network && !result->network->by_jwt.empty()) {
      RegisterNetworkClient(result->network->by_jwt, done);
      return;
    }
    AuthResult r{false, false, "verify returned no network"};
    SetAuthState(AuthState::Error, r.error);
    if (done) done(r);
  });
}

void SdkHost::ResendVerifyCode(const std::string& userAuth,
                               std::function<void(bool ok)> done) {
  urnet::AuthVerifySendArgs args;
  args.user_auth = userAuth;
  args.use_numeric = true;
  api_->authVerifySend(args, [done](std::optional<urnet::AuthVerifySendResult> result,
                                    std::optional<std::string> err) {
    if (done) done(!err && result.has_value());
  });
}

void SdkHost::SendPasswordResetLink(const std::string& userAuth,
                                    std::function<void(bool ok)> done) {
  urnet::AuthPasswordResetArgs args;
  args.user_auth = userAuth;
  api_->authPasswordReset(args, [done](std::optional<urnet::AuthPasswordResetResult> result,
                                       std::optional<std::string> err) {
    if (done) done(!err && result.has_value());
  });
}

void SdkHost::CheckNetworkName(const std::string& networkName,
                               std::function<void(bool ok, bool available)> done) {
  if (!networkNameVc_) {
    if (done) done(false, false);
    return;
  }
  networkNameVc_->networkCheck(
      networkName, [done](std::optional<urnet::NetworkCheckResult> result,
                          std::optional<std::string> err) {
        if (err || !result) {
          if (done) done(false, false);
          return;
        }
        if (done) done(true, result->available);
      });
}

bool SdkHost::HasPendingWalletAuth() {
  std::scoped_lock lock(mutex_);
  return pendingWalletAuth_.has_value();
}

std::optional<urnet::ByJwt> SdkHost::ParsedJwt() {
  std::scoped_lock lock(mutex_);
  if (!localState_) return std::nullopt;
  try {
    return localState_->parseByJwt();
  } catch (const std::exception& e) {
    LogWarn("sdkhost: parse jwt failed: {}", e.what());
    return std::nullopt;
  }
}

void SdkHost::RefreshJwt() {
  std::scoped_lock lock(mutex_);
  if (!device_) return;
  try {
    device_->refreshToken(0);
  } catch (const std::exception& e) {
    LogWarn("sdkhost: refresh token failed: {}", e.what());
  }
}

void SdkHost::RegisterNetworkClient(const std::string& byJwt,
                                    std::function<void(AuthResult)> done) {
  {
    // A new network jwt invalidates a running session (guest upgrade, verify
    // after an upgrade): tear the device + tunnel down so the registration
    // below rebuilds them under the new auth (linux SdkHost parity). Fresh
    // sign-ins have no device and skip this.
    std::scoped_lock lock(mutex_);
    if (device_) {
      try {
        TeardownSessionLocked();
      } catch (const std::exception& e) {
        LogWarn("sdkhost: pre-registration teardown failed: {}", e.what());
      }
    }
  }
  // Persist the network JWT, then register this device to obtain a client JWT.
  localState_->getByJwt();  // touch
  api_->setByJwt(byJwt);

  urnet::AuthNetworkClientArgs args;
  args.description = DeviceDescription();
  args.device_spec = DeviceSpec();

  api_->authNetworkClient(args, [this, byJwt, done](std::optional<urnet::AuthNetworkClientResult> result,
                                                    std::optional<std::string> err) {
    if (err || !result) {
      AuthResult r{false, false, err ? *err : "no result"};
      SetAuthState(AuthState::Error, r.error);
      if (done) done(r);
      return;
    }
    if (result->error && !result->error->message.empty()) {
      AuthResult r{false, false, result->error->message};
      SetAuthState(AuthState::Error, r.error);
      if (done) done(r);
      return;
    }
    if (result->by_client_jwt) {
      try {
        // persist the network + client JWT for the device
        asyncLocalState_->setByJwt(byJwt, [](bool) {});
        asyncLocalState_->setByClientJwt(*result->by_client_jwt, [](bool) {});
      } catch (const std::exception& e) {
        LogWarn("sdkhost: persist jwt failed: {}", e.what());
      }
      bool ok = false;
      {
        std::scoped_lock lock(mutex_);
        ok = BootstrapSession();
      }
      AuthResult r{ok, false, ok ? "" : "failed to start tunnel session"};
      SetAuthState(ok ? AuthState::LoggedIn : AuthState::Error, r.error);
      if (done) done(r);
    } else {
      AuthResult r{false, false, "device registration returned no client jwt"};
      SetAuthState(AuthState::Error, r.error);
      if (done) done(r);
    }
  });
}

// ---- Sign in with a wallet (Solana / Bittensor via ur.io/wallet-connect) ----

// The challenge every client signs for wallet sign-in (macOS/Linux/android
// parity). No nonce: the server only enforces one when present.
static constexpr const char* kWalletSignInMessage = "Welcome to URnetwork";

void SdkHost::SetupWalletCallbacks() {
  wallet_.on_public_key = [this](std::string, WalletConnect::Provider provider) {
    // Solana connects first, then signs. Bittensor has no connect step (it
    // returns the address with the signature), so nothing to chain here.
    if (provider == WalletConnect::Provider::Bittensor) return;
    wallet_.SignMessage(kWalletSignInMessage);
  };
  wallet_.on_signature = [this](std::string publicKey, std::string signature,
                                WalletConnect::Provider provider) {
    AuthLoginWithWallet(publicKey, signature, kWalletSignInMessage, provider);
  };
  wallet_.on_error = [this](std::string err) {
    auto done = walletAuthDone_;
    walletAuthDone_ = nullptr;
    SetAuthState(AuthState::Error, err);
    if (done) done({false, false, err});
  };
}

void SdkHost::SignInWithSolana(WalletConnect::Provider provider,
                               std::function<void(AuthResult)> done) {
  SetAuthState(AuthState::Authenticating);
  {
    std::scoped_lock lock(mutex_);
    pendingWalletAuth_.reset();  // a fresh sign-in supersedes any retained auth
  }
  walletAuthDone_ = std::move(done);
  wallet_.Connect(provider);  // opens the browser; the rest continues on the deep-link callback
}

void SdkHost::SignInWithBittensor(std::function<void(AuthResult)> done) {
  SetAuthState(AuthState::Authenticating);
  {
    std::scoped_lock lock(mutex_);
    pendingWalletAuth_.reset();  // a fresh sign-in supersedes any retained auth
  }
  walletAuthDone_ = std::move(done);
  // one step: the bridge returns the address and the signature together
  wallet_.SignMessageBittensor(kWalletSignInMessage);
}

void SdkHost::HandleDeepLink(const std::string& url) {
  wallet_.HandleDeepLink(url);  // returns false for non-wallet links (future: OAuth)
}

void SdkHost::AuthLoginWithWallet(const std::string& address, const std::string& signature,
                                  const std::string& message,
                                  WalletConnect::Provider provider) {
  urnet::WalletAuthArgs w;
  w.wallet_address = address;
  w.wallet_signature = signature;
  w.wallet_message = message;
  // TAO is sr25519 over an ss58 address; SOL is ed25519 over a base58 pubkey.
  w.blockchain = provider == WalletConnect::Provider::Bittensor ? urnet::TAO : urnet::SOL;
  urnet::AuthLoginArgs args;
  args.wallet_auth = w;
  api_->authLogin(args, [this, w](std::optional<urnet::AuthLoginResult> result,
                                  std::optional<std::string> err) {
    auto done = walletAuthDone_;
    walletAuthDone_ = nullptr;
    if (err || !result) {
      AuthResult r{false, false, err ? *err : "no result"};
      SetAuthState(AuthState::Error, r.error);
      if (done) done(r);
      return;
    }
    if (result->error && !result->error->message.empty()) {
      AuthResult r{false, false, result->error->message};
      SetAuthState(AuthState::Error, r.error);
      if (done) done(r);
      return;
    }
    if (result->network && !result->network->by_jwt.empty()) {
      RegisterNetworkClient(result->network->by_jwt, done ? done : [](AuthResult) {});
      return;
    }
    // The wallet authenticated but isn't linked to a network yet: retain the
    // signed wallet auth and let the UI route to the create-network step, which
    // calls CreateNetwork{useWalletAuth} (name + terms, no password).
    {
      std::scoped_lock lock(mutex_);
      pendingWalletAuth_ = w;
    }
    AuthResult r;
    r.wallet_needs_network = true;
    SetAuthState(AuthState::LoggedOut);
    if (done) done(r);
  });
}

// Invert a BlockActionOverride list into the driver's {paths, allowlist}, the same
// way the SDK's getLocalOverrideAppIds does: RouteOverride.Local=true => bypass,
// false => through-tunnel. Android's "inclusions take precedence": if any app is
// through-tunnel, use ALLOWLIST (keep only those on the tunnel); else DENYLIST
// (bypass those). App rules only (AppIds present); host rules are ignored here.
static void ComputeAppSplit(const urnet::BlockActionOverrideList& overrides,
                            std::vector<std::string>& paths, bool& allowlist) {
  std::vector<std::string> bypass, tunnel;
  for (const auto& over : overrides) {
    if (!over.AppIds || over.AppIds->empty()) continue;
    std::vector<std::string>& dst =
        (over.RouteOverride && over.RouteOverride->Local) ? bypass : tunnel;
    for (const auto& id : *over.AppIds) dst.push_back(id);
  }
  if (!tunnel.empty()) { paths = tunnel; allowlist = true; }
  else { paths = bypass; allowlist = false; }
}

// Upsert / remove one app rule in a BlockActionOverride list (app rules are keyed
// by the exe image path in AppIds; host rules with Hosts are left untouched).
// Shared by the localState_ (offline) and device_ (live) writes.
static void UrstUpsertAppRule(urnet::BlockActionOverrideList& list,
                              const std::string& imagePath, bool includeInTunnel) {
  for (auto& over : list) {
    if (over.AppIds && !over.AppIds->empty() && over.AppIds->front() == imagePath) {
      urnet::RouteOverride route;
      route.Local = !includeInTunnel;
      over.RouteOverride = route;
      return;
    }
  }
  urnet::BlockActionOverride over;
  over.OverrideId = urnet::newId();
  over.AppIds = urnet::StringList{imagePath};
  urnet::RouteOverride route;
  route.Local = !includeInTunnel;
  over.RouteOverride = route;
  list.push_back(std::move(over));
}

static void UrstRemoveAppRule(urnet::BlockActionOverrideList& list,
                              const std::string& imagePath) {
  list.erase(std::remove_if(list.begin(), list.end(),
                            [&](const urnet::BlockActionOverride& over) {
                              return over.AppIds && !over.AppIds->empty() &&
                                     over.AppIds->front() == imagePath;
                            }),
             list.end());
}

bool SdkHost::BootstrapSession() {
  // caller holds mutex_
  const std::string clientJwt = localState_->getByClientJwt();
  if (clientJwt.empty()) return false;
  const std::string instanceId = localState_->getInstanceId();

  if (!service_.IsConnected() && !service_.Connect()) {
    LogError("sdkhost: service not reachable");
    return false;
  }

  try {
    std::string clientPem, serverCertPem, hostPort;

    // Reattach to a live tunnel if the service reports one and we have a session.
    proto::TunnelStatus hello = service_.Hello();
    auto saved = LoadRpcSession();
    if (hello.state == proto::TunnelState::Up && saved &&
        hello.rpc_listen_hostport == saved->host_port) {
      clientPem = saved->client_pem;
      serverCertPem = saved->server_cert_pem;
      hostPort = saved->host_port;
      LogInfo("sdkhost: reattaching to live tunnel at {}", hostPort);
    } else {
      // fresh session: generate per-session RPC key material
      urnet::DeviceRpcKeyMaterial km = urnet::generateDeviceRpcKeyMaterial();
      hostPort = RandomLoopbackHostPort();

      proto::StartTunnel cfg;
      cfg.by_jwt = clientJwt;
      cfg.network_space_json = networkSpace_->toJson();
      cfg.instance_id = instanceId;
      cfg.device_description = DeviceDescription();
      cfg.device_spec = DeviceSpec();
      cfg.app_version = appVersion_;
      cfg.rpc_server_pem = km.getServerPem();
      cfg.rpc_client_cert_pem = km.getClientCertPem();
      cfg.rpc_listen_hostport = hostPort;
      // Seed split tunneling from the persisted per-app overrides so the driver is
      // correct at tunnel-up (device_ isn't connected yet - read the app LocalState).
      // PushLocalOverrideAppsToDriver re-applies it live once the device is up.
      if (localState_) {
        if (auto ov = localState_->getBlockActionOverrides())
          ComputeAppSplit(*ov, cfg.excluded_app_paths, cfg.allowlist_mode);
      }

      proto::TunnelStatus st = service_.StartTunnel(cfg);
      if (st.state != proto::TunnelState::Up) {
        LogError("sdkhost: service failed to start tunnel: {}", st.error);
        return false;
      }
      clientPem = km.getClientPem();
      serverCertPem = km.getServerCertPem();
      SaveRpcSession({clientPem, serverCertPem, hostPort});
    }

    // The controlling DeviceRemote dials the service's mTLS RPC listener.
    device_ = urnet::newDeviceRemoteWithDefaults(*networkSpace_, clientJwt, instanceId);
    device_->setRpcServer(clientPem, serverCertPem, hostPort);
    connectVc_ = device_->openConnectViewController();

    subs_.push_back(connectVc_->addConnectionStatusListener(
        [this] {
          // pull the current status and relay it to the UI as a tunnel event
          if (onTunnel_ && connectVc_) {
            proto::TunnelStatus st;
            st.state = connectVc_->getConnected() ? proto::TunnelState::Up
                                                  : proto::TunnelState::Stopped;
            onTunnel_(st);
          }
        }));
    connectVc_->start();
    // Re-apply the persisted performance profile onto the (re)created device
    // (macOS DeviceManager parity: LocalState is the profile's persistence).
    try {
      device_->setPerformanceProfile(localState_->getPerformanceProfile());
    } catch (const std::exception& e) {
      LogWarn("sdkhost: restore performance profile failed: {}", e.what());
    }
    // Seed the provide control mode the same way (macOS parity): the service's
    // DeviceLocal does not restore it from local state itself. There is no
    // provide toggle on windows yet, so this applies the stored default
    // ("never" — providing is opt-in) and keeps the device consistent with
    // local state once the toggle lands.
    try {
      device_->setProvideControlMode(localState_->getProvideControlMode());
    } catch (const std::exception& e) {
      LogWarn("sdkhost: restore provide control mode failed: {}", e.what());
    }
    SubscribeStats();   // live connection/throughput/provide feed (macOS parity)
    SubscribeDrawer();  // connect drawer feeds (charts, contracts, split rules, dns)

    LogInfo("sdkhost: session bootstrapped (rpc={})", hostPort);
    return true;
  } catch (const std::exception& e) {
    LogError("sdkhost: bootstrap failed: {}", e.what());
    return false;
  }
}

// ---- live stats (macOS parity: listener-push, not polling) ----------------

void SdkHost::SubscribeStats() {
  if (!device_ || !connectVc_) return;
  contractVc_ = device_->openContractViewController();  // live throughput feed
  auto pub = [this] { PublishStats(); };
  // The jwt refresh (which runs immediately at device creation) tells us when
  // the stored client no longer exists on the server. Only marshal from the
  // callback: it runs on an sdk thread, and Logout() clears subs_ -- which
  // would destroy the sub whose callback is running.
  subs_.push_back(device_->addAuthLogoutListener([this] {
    if (onAuthInvalid_) onAuthInvalid_();
  }));
  // ConnectViewController: status, provider grid/window size, selected location.
  subs_.push_back(connectVc_->addConnectionStatusListener(pub));
  subs_.push_back(connectVc_->addGridListener(pub));
  subs_.push_back(connectVc_->addSelectedLocationListener(
      [this](std::optional<urnet::ConnectLocation>) { PublishStats(); }));
  subs_.push_back(device_->addConnectLocationChangeListener(
      [this](std::optional<urnet::ConnectLocation>) { PublishStats(); }));
  // ContractViewController: throughput points (bytes/bit rate up/down).
  subs_.push_back(contractVc_->addThroughputListener(pub));
  // Device: contract status (balance/permission), provide on/off/paused, tunnel.
  subs_.push_back(device_->addContractStatusChangeListener(
      [this](std::optional<urnet::ContractStatus>) { PublishStats(); }));
  subs_.push_back(device_->addProvideChangeListener([this](bool) { PublishStats(); }));
  subs_.push_back(device_->addProvidePausedChangeListener([this](bool) { PublishStats(); }));
  subs_.push_back(device_->addTunnelChangeListener([this](bool) { PublishStats(); }));
  PublishStats();  // initial snapshot
}

LiveStats SdkHost::ReadStats() {
  LiveStats s;
  if (connectVc_) {
    s.connectionStatus = connectVc_->getConnectionStatus();
    s.connected = connectVc_->getConnected();
    auto grid = connectVc_->getGrid();
    s.providerCount = grid.getWindowCurrentSize();
  }
  if (contractVc_) {
    // Most recent throughput point that has a Remote (tunneled) sample.
    if (auto pts = contractVc_->getThroughputPoints(); pts && !pts->empty()) {
      for (auto it = pts->rbegin(); it != pts->rend(); ++it) {
        if (it->Remote) {
          s.downBitsPerSecond = it->Remote->IngressBitRate;
          s.upBitsPerSecond = it->Remote->EgressBitRate;
          break;
        }
      }
    }
  }
  if (device_) {
    if (auto cs = device_->getContractStatus(); cs) s.insufficientBalance = cs->InsufficientBalance;
    s.provideEnabled = device_->getProvideEnabled();
    s.providePaused = device_->getProvidePaused();
    if (auto np = device_->getNetworkPeers(); np && np->Connected) {
      s.provideClients = static_cast<int64_t>(np->Connected->size());
    }
    // selected provider (read-only row + dns regional recommendations)
    if (auto loc = device_->getConnectLocation()) {
      if (loc->name) s.locationName = *loc->name;
      if (loc->country_code) s.countryCode = *loc->country_code;
      if (loc->country) s.countryName = *loc->country;
    }
  }
  return s;
}

void SdkHost::PublishStats() {
  if (onStats_) onStats_(ReadStats());
}

LiveStats SdkHost::CurrentStats() { return ReadStats(); }

// ---- connect drawer feeds (macOS ThroughputStore/ContractDetailsStore/
// BlockActionsStore/DnsSettingsStore parity) --------------------------------

void SdkHost::SubscribeDrawer() {
  // caller holds mutex_ (BootstrapSession)
  if (!device_ || !contractVc_) return;
  blockVc_ = device_->openBlockActionViewController();
  contractDetailsVc_ = device_->openContractDetailsViewController();

  // Offline reconcile: the app LocalState is the source of truth for per-app rules;
  // on connect merge them into the device (which also holds host rules) so the live
  // tunnel matches what the user configured while disconnected. Host rules are kept.
  if (localState_) {
    try {
      if (auto local = localState_->getBlockActionOverrides()) {
        auto merged = device_->getBlockActionOverrides();
        if (!merged) merged = urnet::BlockActionOverrideList{};
        merged->erase(std::remove_if(merged->begin(), merged->end(),
                                     [](const urnet::BlockActionOverride& o) {
                                       return o.AppIds && !o.AppIds->empty();
                                     }),
                      merged->end());
        for (const auto& o : *local)
          if (o.AppIds && !o.AppIds->empty()) merged->push_back(o);
        device_->setBlockActionOverrides(merged);
      }
    } catch (const std::exception& e) {
      LogWarn("sdkhost: merge offline app rules failed: {}", e.what());
    }
  }

  // throughput points feed the three transfer charts
  subs_.push_back(contractVc_->addThroughputListener([this] { PublishThroughput(); }));
  // aggregated per-peer contract rows: the ContractDetailsViewController coalesces
  // the egress + ingress change streams and does the per-peer aggregation +
  // closing lifecycle, then fires one settled ContractRowsChanged we re-read
  subs_.push_back(contractDetailsVc_->addContractRowsListener([this] { PublishContractRows(); }));
  contractDetailsVc_->start();
  // live routing decisions + allow/block counters + overrides ("split rules")
  subs_.push_back(blockVc_->addBlockActionsListener([this] { PublishBlockActions(); }));
  subs_.push_back(blockVc_->addBlockActionStatsListener([this] { PublishBlockStats(); }));
  subs_.push_back(device_->addBlockActionOverridesChangeListener(
      [this](std::optional<urnet::BlockActionOverrideList>) {
        PublishSplitRules();
        PushLocalOverrideAppsToDriver();  // re-drive the split-tunnel driver on any override change
      }));
  // dns resolver settings + ad/tracker blocker
  subs_.push_back(device_->addDnsResolverSettingsChangeListener(
      [this](std::optional<urnet::DnsResolverSettings> settings) {
        if (onDnsSettings_) onDnsSettings_(std::move(settings));
      }));
  subs_.push_back(device_->addBlockerEnabledChangeListener([this](bool on) {
    if (onBlockerEnabled_) onBlockerEnabled_(on);
  }));

  // initial snapshots
  PublishThroughput();
  PublishContractRows();
  PublishBlockActions();
  PublishBlockStats();
  PublishSplitRules();
  PushLocalOverrideAppsToDriver();  // seed the driver once the device + service are up
  if (onDnsSettings_) onDnsSettings_(device_->getDnsResolverSettings());
  if (onBlockerEnabled_) onBlockerEnabled_(device_->getBlockerEnabled());
}

void SdkHost::PublishThroughput() {
  if (!contractVc_) return;
  std::vector<urnet::ThroughputPoint> points;
  if (auto p = contractVc_->getThroughputPoints()) points = std::move(*p);
  int64_t window = contractVc_->getWindowDurationSeconds();
  if (window <= 0) window = 60;
  {
    std::scoped_lock lock(drawerMutex_);
    lastThroughputPoints_ = points;
    throughputWindowSeconds_ = window;
  }
  if (onThroughput_) onThroughput_(std::move(points), window);
}

void SdkHost::PublishContractRows() {
  if (!contractDetailsVc_) return;

  // The view controller returns fully aggregated, render-ready rows: per-peer
  // sums, the contract-id swap signatures, and the closing flag. Map them onto
  // the app row type -- no per-app aggregation, ordering, or coalescing here
  // (macOS ContractDetailsStore.update parity).
  std::vector<ContractClientRow> rows;
  if (auto list = contractDetailsVc_->getClientContractRows()) {
    rows.reserve(list->size());
    for (const auto& r : *list) {
      ContractClientRow row;
      row.clientId = r.ClientId;
      row.contractId = r.ContractId;
      row.companionContractId = r.CompanionContractId;
      row.contractUsedByteCount = r.ContractUsedByteCount;
      row.contractByteCount = r.ContractByteCount;
      row.contractBitRate = r.ContractBitRate;
      row.companionContractUsedByteCount = r.CompanionContractUsedByteCount;
      row.companionContractByteCount = r.CompanionContractByteCount;
      row.companionContractBitRate = r.CompanionContractBitRate;
      row.pairCount = r.PairCount;
      row.closing = r.Closing;
      rows.push_back(std::move(row));
    }
  }

  bool changed = false;
  {
    std::scoped_lock lock(drawerMutex_);
    changed = rows != lastContractRows_;
    if (changed) lastContractRows_ = rows;
  }
  if (changed && onContractRows_) onContractRows_(std::move(rows));
}

void SdkHost::PublishBlockActions() {
  if (!blockVc_) return;
  std::vector<BlockActionItem> items;
  if (auto list = blockVc_->getBlockActions()) {
    items.reserve(list->size());
    // the sdk window is oldest first; the UI wants newest first
    for (auto it = list->rbegin(); it != list->rend(); ++it) {
      BlockActionItem item;
      item.id = it->BlockActionId ? *it->BlockActionId
                                  : std::to_string(it->Time) + ":" +
                                        (it->Hosts && !it->Hosts->empty() ? (*it->Hosts)[0] : "");
      item.timeMillis = it->Time;
      if (it->Hosts) item.hosts = *it->Hosts;
      if (it->Ips) item.ips = *it->Ips;
      item.block = it->Block;
      item.local = it->Local;
      if (it->OverrideId) item.overrideId = *it->OverrideId;
      item.hasBlockOverride = it->BlockOverride.has_value();
      item.hasRouteOverride = it->RouteOverride.has_value();
      item.packetCount = it->PacketCount;
      item.byteCount = it->ByteCount;
      items.push_back(std::move(item));
    }
  }
  bool changed = false;
  {
    // the sdk re-emits per routing decision; only publish when the list changed
    std::scoped_lock lock(drawerMutex_);
    changed = items != lastBlockActions_;
    if (changed) lastBlockActions_ = items;
  }
  if (changed && onBlockActions_) onBlockActions_(std::move(items));
}

void SdkHost::PublishBlockStats() {
  if (!blockVc_) return;
  int64_t allowed = 0, blocked = 0;
  if (auto stats = blockVc_->getBlockStats()) {
    allowed = stats->AllowedCount;
    blocked = stats->BlockedCount;
  }
  bool changed = false;
  {
    std::scoped_lock lock(drawerMutex_);
    changed = allowed != lastAllowedCount_ || blocked != lastBlockedCount_;
    lastAllowedCount_ = allowed;
    lastBlockedCount_ = blocked;
  }
  if (changed && onBlockStats_) onBlockStats_(allowed, blocked);
}

void SdkHost::PublishSplitRules() {
  if (!device_) return;
  std::vector<SplitRule> rules;
  if (auto list = device_->getBlockActionOverrides()) {
    rules.reserve(list->size());
    for (const auto& over : *list) {
      if (!over.OverrideId) continue;
      SplitRule rule;
      rule.overrideId = *over.OverrideId;
      if (over.Hosts) rule.hosts = *over.Hosts;
      rule.routeLocal = over.RouteOverride && over.RouteOverride->Local;
      rules.push_back(std::move(rule));
    }
  }
  bool changed = false;
  {
    std::scoped_lock lock(drawerMutex_);
    changed = rules != lastSplitRules_;
    if (changed) lastSplitRules_ = rules;
  }
  if (changed && onSplitRules_) onSplitRules_(std::move(rules));
}

void SdkHost::PushLocalOverrideAppsToDriver() {
  if (!device_) return;
  // getLocalOverrideAppIds() already inverts: Included = Local (bypass), Excluded =
  // remote (through the tunnel). Android's "inclusions take precedence": any through-
  // tunnel app => ALLOWLIST with the tunnel set; else DENYLIST with the bypass set.
  std::vector<std::string> paths;
  bool allowlist = false;
  try {
    if (auto ids = device_->getLocalOverrideAppIds()) {
      if (ids->Excluded && !ids->Excluded->empty()) {
        paths = *ids->Excluded;   // through-tunnel apps => allowlist keep-set
        allowlist = true;
      } else if (ids->Included) {
        paths = *ids->Included;   // bypass apps => denylist redirect-set
      }
    }
  } catch (const std::exception& e) {
    LogWarn("sdkhost: read local override app ids failed: {}", e.what());
    return;
  }
  if (service_.IsConnected()) service_.SetSplitTunnel(paths, allowlist);
}

void SdkHost::ClearDrawer() {
  {
    std::scoped_lock lock(drawerMutex_);
    lastThroughputPoints_.clear();
    lastContractRows_.clear();
    lastBlockActions_.clear();
    lastAllowedCount_ = 0;
    lastBlockedCount_ = 0;
    lastSplitRules_.clear();
  }
  if (onThroughput_) onThroughput_({}, 60);
  if (onContractRows_) onContractRows_({});
  if (onBlockActions_) onBlockActions_({});
  if (onBlockStats_) onBlockStats_(0, 0);
  if (onSplitRules_) onSplitRules_({});
  if (onDnsSettings_) onDnsSettings_(std::nullopt);
  // clear the chooser's peer-count sub-label + any open sheet on logout
  if (onLocations_) onLocations_(std::nullopt, std::string());
  if (onPeers_) onPeers_(std::nullopt);
}

std::vector<urnet::ThroughputPoint> SdkHost::CurrentThroughputPoints(int64_t& windowSeconds) {
  std::scoped_lock lock(drawerMutex_);
  windowSeconds = throughputWindowSeconds_;
  return lastThroughputPoints_;
}

std::vector<ContractClientRow> SdkHost::CurrentContractRows() {
  std::scoped_lock lock(drawerMutex_);
  return lastContractRows_;
}

std::vector<BlockActionItem> SdkHost::CurrentBlockActions() {
  std::scoped_lock lock(drawerMutex_);
  return lastBlockActions_;
}

void SdkHost::CurrentBlockCounts(int64_t& allowed, int64_t& blocked) {
  std::scoped_lock lock(drawerMutex_);
  allowed = lastAllowedCount_;
  blocked = lastBlockedCount_;
}

std::vector<SplitRule> SdkHost::CurrentSplitRules() {
  std::scoped_lock lock(drawerMutex_);
  return lastSplitRules_;
}

std::optional<urnet::DnsResolverSettings> SdkHost::CurrentDnsSettings() {
  if (!device_) return std::nullopt;
  try {
    return device_->getDnsResolverSettings();
  } catch (const std::exception& e) {
    LogWarn("sdkhost: get dns settings failed: {}", e.what());
    return std::nullopt;
  }
}

bool SdkHost::CurrentBlockerEnabled() {
  if (!device_) return false;
  try {
    return device_->getBlockerEnabled();
  } catch (const std::exception& e) {
    LogWarn("sdkhost: get blocker failed: {}", e.what());
    return false;
  }
}

PerformanceSettings SdkHost::CurrentPerformanceSettings() {
  PerformanceSettings s;
  std::optional<urnet::PerformanceProfile> profile;
  try {
    if (device_) {
      profile = device_->getPerformanceProfile();
    } else if (localState_) {
      profile = localState_->getPerformanceProfile();
    }
  } catch (const std::exception& e) {
    LogWarn("sdkhost: get performance profile failed: {}", e.what());
  }
  if (!profile) return s;  // nil profile -> Auto
  s.mode = profile->window_type == urnet::WindowTypeQuality ? ConnectionMode::Web
                                                            : ConnectionMode::Streaming;
  s.allowDirect = profile->allow_direct;
  s.fixedIp = profile->window_size && profile->window_size->window_size_min == 1 &&
              profile->window_size->window_size_max == 1;
  return s;
}

void SdkHost::SetPerformanceSettings(const PerformanceSettings& settings) {
  std::scoped_lock lock(mutex_);
  std::optional<urnet::PerformanceProfile> profile;
  if (settings.mode != ConnectionMode::Auto) {
    urnet::PerformanceProfile p;
    p.window_type = settings.mode == ConnectionMode::Web ? urnet::WindowTypeQuality
                                                         : urnet::WindowTypeSpeed;
    p.allow_direct = settings.allowDirect;
    urnet::WindowSizeSettings ws;
    ws.window_size_min = settings.fixedIp ? 1 : 2;
    ws.window_size_max = settings.fixedIp ? 1 : 4;
    p.window_size = ws;
    profile = std::move(p);
  }
  try {
    if (localState_) localState_->setPerformanceProfile(profile);  // persistence
    if (device_) device_->setPerformanceProfile(profile);          // live device
  } catch (const std::exception& e) {
    LogWarn("sdkhost: set performance profile failed: {}", e.what());
  }
}

void SdkHost::SetBlockerEnabled(bool on) {
  std::scoped_lock lock(mutex_);
  if (!device_) return;
  try {
    device_->setBlockerEnabled(on);  // the device persists and restores this
  } catch (const std::exception& e) {
    LogWarn("sdkhost: set blocker failed: {}", e.what());
  }
}

void SdkHost::ApplyDnsSettings(const urnet::DnsResolverSettings& settings) {
  std::scoped_lock lock(mutex_);
  if (!device_) return;
  try {
    device_->setDnsResolverSettings(settings);
  } catch (const std::exception& e) {
    LogWarn("sdkhost: set dns settings failed: {}", e.what());
  }
  if (onDnsSettings_) onDnsSettings_(CurrentDnsSettings());
}

void SdkHost::CreateSplitRule(const std::vector<std::string>& hosts) {
  std::scoped_lock lock(mutex_);
  if (!device_ || hosts.empty()) return;
  try {
    urnet::BlockActionOverride over;
    over.OverrideId = urnet::newId();
    over.Hosts = hosts;
    urnet::RouteOverride route;
    route.Local = true;
    over.RouteOverride = route;
    device_->addBlockActionOverride(over);
  } catch (const std::exception& e) {
    LogWarn("sdkhost: create split rule failed: {}", e.what());
  }
  PublishSplitRules();
}

void SdkHost::UpdateSplitRule(const std::string& overrideId,
                              const std::vector<std::string>& hosts) {
  {
    std::scoped_lock lock(mutex_);
    if (!device_) return;
    if (!hosts.empty()) {
      try {
        // rebuild the full override list with the rule's hosts replaced
        auto list = device_->getBlockActionOverrides();
        if (!list) return;
        bool found = false;
        for (auto& over : *list) {
          if (over.OverrideId && *over.OverrideId == overrideId) {
            over.Hosts = hosts;
            found = true;
            break;
          }
        }
        if (found) device_->setBlockActionOverrides(list);
      } catch (const std::exception& e) {
        LogWarn("sdkhost: update split rule failed: {}", e.what());
      }
      PublishSplitRules();
      return;
    }
  }
  // empty selection removes the rule (RemoveSplitRule takes the lock itself)
  RemoveSplitRule(overrideId);
}

void SdkHost::RemoveSplitRule(const std::string& overrideId) {
  std::scoped_lock lock(mutex_);
  if (!device_) return;
  try {
    device_->removeBlockActionOverride(overrideId);
  } catch (const std::exception& e) {
    LogWarn("sdkhost: remove split rule failed: {}", e.what());
  }
  PublishSplitRules();
}

void SdkHost::SetAppRule(const std::string& imagePath, bool includeInTunnel) {
  std::scoped_lock lock(mutex_);
  if (imagePath.empty()) return;
  try {
    // localState_ is the OFFLINE source of truth (persists, readable while
    // disconnected). device_ drives the LIVE tunnel when connected -
    // setBlockActionOverrides fires the change listener -> re-drives the driver.
    // Write both so the config is durable and applies immediately when up.
    if (localState_) {
      auto list = localState_->getBlockActionOverrides();
      if (!list) list = urnet::BlockActionOverrideList{};
      UrstUpsertAppRule(*list, imagePath, includeInTunnel);
      localState_->setBlockActionOverrides(list);
    }
    if (device_) {
      auto list = device_->getBlockActionOverrides();
      if (!list) list = urnet::BlockActionOverrideList{};
      UrstUpsertAppRule(*list, imagePath, includeInTunnel);
      device_->setBlockActionOverrides(list);
    }
  } catch (const std::exception& e) {
    LogWarn("sdkhost: set app rule failed: {}", e.what());
  }
}

void SdkHost::RemoveAppRule(const std::string& imagePath) {
  std::scoped_lock lock(mutex_);
  if (imagePath.empty()) return;
  try {
    if (localState_) {
      if (auto list = localState_->getBlockActionOverrides()) {
        UrstRemoveAppRule(*list, imagePath);
        localState_->setBlockActionOverrides(list);
      }
    }
    if (device_) {
      if (auto list = device_->getBlockActionOverrides()) {
        UrstRemoveAppRule(*list, imagePath);
        device_->setBlockActionOverrides(list);
      }
    }
  } catch (const std::exception& e) {
    LogWarn("sdkhost: remove app rule failed: {}", e.what());
  }
}

std::vector<AppRule> SdkHost::CurrentAppRules() {
  std::scoped_lock lock(mutex_);
  std::vector<AppRule> rules;
  try {
    // Read the offline source of truth so the sheet works while disconnected.
    std::optional<urnet::BlockActionOverrideList> list;
    if (localState_) list = localState_->getBlockActionOverrides();
    if (list) {
      for (const auto& over : *list) {
        if (!over.AppIds || over.AppIds->empty()) continue;  // app rules only
        AppRule rule;
        rule.imagePath = over.AppIds->front();
        rule.includeInTunnel = !(over.RouteOverride && over.RouteOverride->Local);
        rules.push_back(std::move(rule));
      }
    }
  } catch (const std::exception& e) {
    LogWarn("sdkhost: current app rules failed: {}", e.what());
  }
  return rules;
}

// ---- location/provider chooser --------------------------------------------
// The bucketed location feed + the connected, provide-enabled peers pinned atop
// the chooser. Opened lazily on the first chooser open; start() kicks the
// initial load (filterLocations("")). The listeners fire on SDK callback
// threads and only marshal (never re-enter SdkHost), so pushing the initial
// snapshot under mutex_ here is safe.
void SdkHost::EnsureLocations() {
  std::scoped_lock lock(mutex_);
  if (!device_ || locationsVc_) return;  // idempotent; needs a live session
  locationsVc_ = device_->openLocationsViewController();
  subs_.push_back(locationsVc_->addFilteredLocationsListener(
      [this](std::optional<urnet::FilteredLocations> locations, std::string state) {
        if (onLocations_) onLocations_(std::move(locations), std::move(state));
      }));
  locationsVc_->start();
  // PeerViewController: connected AND provide-enabled peers only (SDK filters).
  peerVc_ = device_->openPeerViewController();
  subs_.push_back(peerVc_->addPeersListener(
      [this](std::optional<urnet::NetworkPeerList> peers) {
        if (onPeers_) onPeers_(std::move(peers));
      }));
  peerVc_->start();
  // seed the chooser + the drawer's peer-count sub-label (the listeners only
  // fire on later changes)
  if (onLocations_) {
    onLocations_(locationsVc_->getFilteredLocations(),
                 locationsVc_->getFilteredLocationState());
  }
  if (onPeers_) onPeers_(peerVc_->getPeers());
}

void SdkHost::SetLocationFilter(const std::string& query) {
  std::scoped_lock lock(mutex_);
  if (locationsVc_) locationsVc_->filterLocations(query);
}

std::optional<urnet::FilteredLocations> SdkHost::CurrentFilteredLocations() {
  std::scoped_lock lock(mutex_);
  if (locationsVc_) return locationsVc_->getFilteredLocations();
  return std::nullopt;
}

std::string SdkHost::CurrentFilteredLocationState() {
  std::scoped_lock lock(mutex_);
  if (locationsVc_) return locationsVc_->getFilteredLocationState();
  return std::string();
}

std::optional<urnet::NetworkPeerList> SdkHost::ConnectedProvidePeers() {
  std::scoped_lock lock(mutex_);
  if (peerVc_) return peerVc_->getPeers();
  return std::nullopt;
}

std::optional<urnet::ConnectLocation> SdkHost::SelectedLocation() {
  std::scoped_lock lock(mutex_);
  if (connectVc_) return connectVc_->getSelectedLocation();
  if (device_) return device_->getConnectLocation();
  return std::nullopt;
}

void SdkHost::ConnectBestAvailable() {
  std::scoped_lock lock(mutex_);
  if (connectVc_) connectVc_->connectBestAvailable();
}

void SdkHost::Connect(const std::string& connectLocationJson) {
  std::scoped_lock lock(mutex_);
  if (!connectVc_) return;
  try {
    urnet::ConnectLocation loc =
        nlohmann::json::parse(connectLocationJson).get<urnet::ConnectLocation>();
    connectVc_->connect(loc);
  } catch (const std::exception& e) {
    LogWarn("sdkhost: connect parse failed: {}", e.what());
  }
}

// Connect to an SDK-supplied ConnectLocation as-is (the chooser already holds
// the typed struct; skip the json round-trip). connect() takes an optional.
void SdkHost::Connect(const urnet::ConnectLocation& location) {
  std::scoped_lock lock(mutex_);
  if (connectVc_) connectVc_->connect(location);
}

void SdkHost::Disconnect() {
  std::scoped_lock lock(mutex_);
  if (connectVc_) connectVc_->disconnect();
}

void SdkHost::TeardownSessionLocked() {
  subs_.clear();
  connectVc_.reset();
  contractVc_.reset();
  // the details VC spawns a coalescing run loop in Start(); Close() cancels it
  // (macOS ContractDetailsStore.reset -> viewController.close parity)
  if (contractDetailsVc_) {
    contractDetailsVc_->close();
    contractDetailsVc_.reset();
  }
  blockVc_.reset();
  locationsVc_.reset();
  peerVc_.reset();
  ClearDrawer();
  if (device_) { device_->close(); device_.reset(); }
  if (service_.IsConnected()) {
    service_.StopTunnel();
    service_.Logout();
  }
  ClearRpcSession();
}

void SdkHost::Logout() {
  std::scoped_lock lock(mutex_);
  try {
    pendingWalletAuth_.reset();
    TeardownSessionLocked();
    if (asyncLocalState_) asyncLocalState_->logout([](bool) {});
    SetAuthState(AuthState::LoggedOut);
    LogInfo("sdkhost: logged out");
  } catch (const std::exception& e) {
    LogError("sdkhost: logout failed: {}", e.what());
  }
}

}  // namespace urnw
