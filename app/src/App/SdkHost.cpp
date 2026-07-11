// SPDX-License-Identifier: MPL-2.0
#include "SdkHost.h"

#include <fstream>
#include <random>
#include <thread>

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

void SdkHost::RegisterNetworkClient(const std::string& byJwt,
                                    std::function<void(AuthResult)> done) {
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

// ---- Sign in with Solana (Phantom/Solflare via ur.io/wallet-connect) --------

// The challenge the wallet signs for Sign-in-with-Solana (macOS/Linux parity).
static constexpr const char* kWalletSignInMessage = "Welcome to URnetwork";

void SdkHost::SetupWalletCallbacks() {
  wallet_.on_public_key = [this](std::string publicKey, WalletConnect::Provider) {
    walletAddress_ = publicKey;
    wallet_.SignMessage(kWalletSignInMessage);  // now ask the wallet to sign the challenge
  };
  wallet_.on_signature = [this](std::string signatureB64) {
    AuthLoginWithWallet(walletAddress_, signatureB64, kWalletSignInMessage);
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
  walletAuthDone_ = std::move(done);
  wallet_.Connect(provider);  // opens the browser; the rest continues on the deep-link callback
}

void SdkHost::HandleDeepLink(const std::string& url) {
  wallet_.HandleDeepLink(url);  // returns false for non-wallet links (future: OAuth)
}

void SdkHost::AuthLoginWithWallet(const std::string& address, const std::string& signatureB64,
                                  const std::string& message) {
  urnet::WalletAuthArgs w;
  w.wallet_address = address;
  w.wallet_signature = signatureB64;
  w.wallet_message = message;
  w.blockchain = "solana";
  urnet::AuthLoginArgs args;
  args.wallet_auth = w;
  api_->authLogin(args, [this](std::optional<urnet::AuthLoginResult> result,
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
    // Wallet authenticated but isn't linked to a network yet - the create-account-
    // with-wallet path (NetworkCreate{wallet_auth}) needs the account UI.
    AuthResult r{false, false,
                 "This wallet isn't linked to a network yet - create an account to continue."};
    SetAuthState(AuthState::Error, r.error);
    if (done) done(r);
  });
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
      cfg.excluded_app_paths = excludedApps_;

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
    SubscribeStats();  // live connection/throughput/provide feed (macOS parity)

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
  // ConnectViewController: status, provider grid/window size, selected location.
  subs_.push_back(connectVc_->addConnectionStatusListener(pub));
  subs_.push_back(connectVc_->addGridListener(pub));
  subs_.push_back(connectVc_->addSelectedLocationListener(
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
  }
  return s;
}

void SdkHost::PublishStats() {
  if (onStats_) onStats_(ReadStats());
}

LiveStats SdkHost::CurrentStats() { return ReadStats(); }

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

void SdkHost::Disconnect() {
  std::scoped_lock lock(mutex_);
  if (connectVc_) connectVc_->disconnect();
}

void SdkHost::SetExcludedApps(const std::vector<std::string>& paths) {
  std::scoped_lock lock(mutex_);
  excludedApps_ = paths;
  if (service_.IsConnected()) service_.SetSplitTunnel(paths);
}

void SdkHost::Logout() {
  std::scoped_lock lock(mutex_);
  try {
    subs_.clear();
    connectVc_.reset();
    if (device_) { device_->close(); device_.reset(); }
    if (service_.IsConnected()) {
      service_.StopTunnel();
      service_.Logout();
    }
    ClearRpcSession();
    if (asyncLocalState_) asyncLocalState_->logout([](bool) {});
    SetAuthState(AuthState::LoggedOut);
    LogInfo("sdkhost: logged out");
  } catch (const std::exception& e) {
    LogError("sdkhost: logout failed: {}", e.what());
  }
}

}  // namespace urnw
