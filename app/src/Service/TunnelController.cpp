// SPDX-License-Identifier: MPL-2.0
#include "TunnelController.h"

#include <chrono>
#include <fstream>

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>   // AF_INET / AF_INET6
#include <windows.h>

#include "Ids.h"
#include "Log.h"
#include "Paths.h"
#include "Strings.h"

namespace urnw {
namespace {

constexpr DWORD kRingCapacity = 0x400000;  // 4 MiB (power of two, within wintun bounds)
constexpr uint32_t kTunnelMtu = 1440;      // matches macOS

std::filesystem::path ExeDir() {
  wchar_t buf[MAX_PATH];
  DWORD n = ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
  return std::filesystem::path(std::wstring(buf, n)).parent_path();
}

int64_t NowMillis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::vector<uint8_t> ReadFileBytes(const std::filesystem::path& p) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return {};
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(f),
                              std::istreambuf_iterator<char>());
}

void WriteFileBytes(const std::filesystem::path& p, const std::vector<uint8_t>& b) {
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  if (f) f.write(reinterpret_cast<const char*>(b.data()), b.size());
}

}  // namespace

TunnelController::TunnelController() {
  storageDir_ = StorageRoot(/*isService=*/true);
}

TunnelController::~TunnelController() { Stop(); }

std::optional<urnet::DeviceLocalKeyMaterial> TunnelController::LoadKeyMaterial() {
  auto seed = ReadFileBytes(storageDir_ / L"client_key_seed.bin");
  auto cert = ReadFileBytes(storageDir_ / L"provide_cert.pem");
  auto key = ReadFileBytes(storageDir_ / L"provide_key.pem");
  if (seed.empty() || cert.empty() || key.empty()) return std::nullopt;
  return urnet::newDeviceLocalKeyMaterial(
      seed.data(), static_cast<int32_t>(seed.size()), cert.data(),
      static_cast<int32_t>(cert.size()), key.data(),
      static_cast<int32_t>(key.size()));
}

void TunnelController::PersistKeyMaterial(const urnet::DeviceLocalKeyMaterial& km) {
  WriteFileBytes(storageDir_ / L"client_key_seed.bin", km.getClientKeySeed());
  WriteFileBytes(storageDir_ / L"provide_cert.pem", km.getProvideTlsCertificatePem());
  WriteFileBytes(storageDir_ / L"provide_key.pem", km.getProvideTlsPrivateKeyPem());
}

proto::TunnelStatus TunnelController::Start(const proto::StartTunnel& config) {
  std::scoped_lock lock(mutex_);
  return StartLocked(config);
}

proto::TunnelStatus TunnelController::StartLocked(const proto::StartTunnel& config) {
  StopLocked();  // idempotent restart
  state_ = proto::TunnelState::Starting;
  error_.clear();

  try {
    // --- NetworkSpace (own storage; import the app's space json) ---
    if (!spaceManager_) {
      spaceManager_ =
          urnet::newNetworkSpaceManager(Narrow(SdkStorageDir(true).wstring()));
    }
    networkSpace_ = spaceManager_->importNetworkSpaceFromJson(config.network_space_json);

    // --- wintun adapter (installs the driver on first use; needs SYSTEM) ---
    wintun_ = Wintun::Load(ExeDir() / L"wintun.dll");
    if (!wintun_) throw std::runtime_error("failed to load wintun.dll");
    adapter_ = WintunAdapter::Create(*wintun_, ids::kTunAdapterName,
                                     ids::kTunAdapterGuid, kRingCapacity);
    if (!adapter_) throw std::runtime_error("failed to create wintun adapter");

    // --- R1: bind SDK egress to the physical interface BEFORE the device
    //     opens its sockets. At this point no tun routes exist yet, so the
    //     physical default route is discovered correctly. ---
    egress_ = std::make_unique<EgressMonitor>(adapter_->Luid());
    egress_->Start();

    // --- DeviceLocal (stable provider identity via persisted key material) ---
    auto km = LoadKeyMaterial();
    if (km) {
      device_ = urnet::newDeviceLocalWithKeyMaterial(
          *networkSpace_, config.by_jwt, config.device_description,
          config.device_spec, config.app_version, config.instance_id,
          /*enable_rpc=*/false, *km);
    } else {
      device_ = urnet::newDeviceLocalWithDefaults(
          *networkSpace_, config.by_jwt, config.device_description,
          config.device_spec, config.app_version, config.instance_id,
          /*enable_rpc=*/false);
      PersistKeyMaterial(device_->getKeyMaterial());
    }

    // --- mTLS RPC listener the app's DeviceRemote dials ---
    device_->setRpcServer(config.rpc_server_pem, config.rpc_client_cert_pem,
                          config.rpc_listen_hostport);
    rpcHostPort_ = config.rpc_listen_hostport;

    // --- network settings (address/MTU/routes/DNS), from the device ---
    TunnelNetworkSettings settings;
    settings.local_address_v4 = device_->tunnelLocalAddress();
    if (settings.local_address_v4.empty()) settings.local_address_v4 = "169.254.2.1";
    settings.prefix_v4 = 24;
    settings.mtu = kTunnelMtu;
    // dns from the device, like the tunnel address: the dns settings' unencrypted
    // local servers when set, otherwise the default plain 1.1.1.1 (which the
    // UpgradeMux can intercept and upgrade). the tunnel is ipv4-only, so only the
    // ipv4 resolvers apply
    if (auto dns = device_->tunnelDnsAddressesIpv4(); dns && !dns->empty()) {
      settings.dns_servers = *dns;
    } else {
      settings.dns_servers = {"1.1.1.1"};
    }
    netConfig_ = std::make_unique<NetworkConfig>(adapter_->Luid());
    if (!netConfig_->Apply(settings)) throw std::runtime_error("network config failed");

    // --- split tunneling (driver optional) ---
    splitTunnel_.Open();
    excludedPaths_ = config.excluded_app_paths;
    allowlist_ = config.allowlist_mode;
    PushExcludedToDriver(excludedPaths_, allowlist_);

    // --- packet pump ---
    pump_ = std::make_unique<PacketPump>(*adapter_, *device_);
    pump_->Start();

    state_ = proto::TunnelState::Up;
    upSinceMillis_ = NowMillis();
    LogInfo("tunnel: up (rpc={})", rpcHostPort_);
  } catch (const std::exception& e) {
    error_ = e.what();
    state_ = proto::TunnelState::Error;
    LogError("tunnel: start failed: {}", error_);
    StopLocked();
    state_ = proto::TunnelState::Error;  // StopLocked resets to Stopped
  }

  return Status();
}

void TunnelController::Stop() {
  std::scoped_lock lock(mutex_);
  StopLocked();
}

void TunnelController::StopLocked() {
  if (state_ == proto::TunnelState::Up || state_ == proto::TunnelState::Starting)
    state_ = proto::TunnelState::Stopping;

  // Tear down in reverse dependency order.
  if (pump_) { pump_->Stop(); pump_.reset(); }
  if (netConfig_) { netConfig_->Revert(); netConfig_.reset(); }
  splitTunnel_.Close();
  if (egress_) { egress_->Stop(); egress_.reset(); }
  // Reset egress binding so a later non-tunnel run isn't pinned to a stale nic.
  urnet::setEgressInterfaceIndex(0, 0);
  if (device_) { device_->close(); device_.reset(); }
  if (adapter_) adapter_.reset();
  wintun_.reset();
  // networkSpace_/spaceManager_ persist across sessions.
  networkSpace_.reset();

  rpcHostPort_.clear();
  upSinceMillis_ = 0;
  state_ = proto::TunnelState::Stopped;
}

void TunnelController::PushExcludedToDriver(const std::vector<std::string>& paths, bool allowlist) {
  if (!splitTunnel_.IsAvailable()) return;
  // The driver rebinds excluded sockets to the physical interface's source
  // address, so resolve the current physical interface + its preferred source.
  EgressInterfaces egress = NetworkConfig::DiscoverEgress(adapter_->Luid());
  uint8_t addr4[4] = {0};
  uint8_t addr6[16] = {0};
  bool has4 = egress.index4 != 0 &&
              NetworkConfig::InterfaceSourceAddress(egress.index4, AF_INET, addr4);
  bool has6 = egress.index6 != 0 &&
              NetworkConfig::InterfaceSourceAddress(egress.index6, AF_INET6, addr6);
  splitTunnel_.SetPhysicalAddresses(has4 ? egress.index4 : 0, has4 ? addr4 : nullptr,
                                    has6 ? egress.index6 : 0, has6 ? addr6 : nullptr);
  splitTunnel_.SetMode(allowlist);
  splitTunnel_.SetExcludedPaths(paths);
  // Enable whenever there is a rule set. In allowlist mode an empty keep-set would
  // route nothing through the tunnel, so the service only sends allowlist mode with
  // a non-empty set (see SdkHost); either way !empty is the right enable signal.
  splitTunnel_.SetEnabled(!paths.empty());
}

bool TunnelController::SetSplitTunnel(const std::vector<std::string>& excludedPaths, bool allowlist) {
  std::scoped_lock lock(mutex_);
  excludedPaths_ = excludedPaths;
  allowlist_ = allowlist;
  if (state_ != proto::TunnelState::Up) return true;  // applied on next Start
  PushExcludedToDriver(excludedPaths_, allowlist_);
  return true;
}

void TunnelController::Logout() {
  std::scoped_lock lock(mutex_);
  StopLocked();
  // Clear persisted device identity so the next login starts clean (mirrors the
  // macOS logout provider message clearing LocalState).
  std::error_code ec;
  std::filesystem::remove(storageDir_ / L"client_key_seed.bin", ec);
  std::filesystem::remove(storageDir_ / L"provide_cert.pem", ec);
  std::filesystem::remove(storageDir_ / L"provide_key.pem", ec);
  LogInfo("tunnel: logged out (cleared device identity)");
}

proto::TunnelStatus TunnelController::Status() {
  proto::TunnelStatus s;
  s.state = state_;
  s.rpc_listen_hostport = rpcHostPort_;
  s.error = error_;
  s.service_version = urnet::version();
  s.protocol_version = proto::kProtocolVersion;
  s.tunnel_local_up_millis = upSinceMillis_ ? (NowMillis() - upSinceMillis_) : 0;
  return s;
}

}  // namespace urnw
