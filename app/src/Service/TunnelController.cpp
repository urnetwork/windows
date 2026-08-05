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

std::string Join(const std::vector<std::string>& parts) {
  std::string out;
  for (const auto& p : parts) {
    if (!out.empty()) out += ",";
    out += p;
  }
  return out;
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

  // Named so a failure says which step threw. Nothing here has run before, so
  // "it stopped after step 3" is the whole diagnosis on the first real start.
  const char* step = "init";
  const int64_t startedAtMillis = NowMillis();
  LogInfo("tunnel: starting (rpc={} device=\"{}\" spec=\"{}\" app={} jwt={}B "
          "split={} paths={})",
          config.rpc_listen_hostport, config.device_description,
          config.device_spec, config.app_version, config.by_jwt.size(),
          config.allowlist_mode ? "allowlist" : "denylist",
          config.excluded_app_paths.size());

  try {
    // --- 1/8 wintun adapter (installs the driver on first use; needs SYSTEM) ---
    // Created FIRST, before any SDK object: the adapter is what the egress
    // binding below has to exclude, and it carries no address or route yet so
    // it cannot attract traffic while we set the rest up.
    step = "1/8 wintun";
    const std::filesystem::path dll = ExeDir() / L"wintun.dll";
    LogInfo("tunnel: [1/8] loading wintun from {}", dll.string());
    wintun_ = Wintun::Load(dll);
    if (!wintun_) throw std::runtime_error("failed to load wintun.dll (is it next to urnetworkd.exe?)");
    adapter_ = WintunAdapter::Create(*wintun_, ids::kTunAdapterName,
                                     ids::kTunAdapterGuid, kRingCapacity);
    if (!adapter_)
      throw std::runtime_error(
          "failed to create the wintun adapter (needs LocalSystem/admin and a "
          "loadable wintun driver)");
    NET_IFINDEX tunIndex = 0;
    NET_LUID tunLuid = adapter_->Luid();
    ::ConvertInterfaceLuidToIndex(&tunLuid, &tunIndex);
    LogInfo("tunnel: [1/8] adapter up: luid {:#x}, interface {}", tunLuid.Value,
            NetworkConfig::DescribeInterface(tunIndex));

    // --- 2/8 R1: bind the SDK's egress to the physical interface. ---
    // Ordering is the whole mechanism, and it is load-bearing twice over:
    //   * BEFORE any SDK object exists, so no socket is ever created unbound
    //     (an unbound socket keeps whatever route it resolved and will follow
    //     the tun once step 6 installs the routes);
    //   * BEFORE step 6 installs those routes, so DiscoverEgress still sees a
    //     clean table and picks the physical default route.
    // Do not move this below the NetworkSpace/DeviceLocal construction.
    step = "2/8 egress (R1)";
    LogInfo("tunnel: [2/8] binding sdk egress to the physical interface (R1)");
    egress_ = std::make_unique<EgressMonitor>(adapter_->Luid());
    egress_->Start();  // logs the chosen interface; keeps it current on change
    if (egress_->Current().index4 == 0) {
      // Not fatal — there may genuinely be no network yet, and the monitor will
      // bind as soon as one appears — but it is the R1 hazard, so it is loud.
      LogError("tunnel: [2/8] no physical ipv4 egress interface; R1 protection "
               "is NOT in force yet");
    }

    // --- 3/8 NetworkSpace (own storage; import the app's space json) ---
    step = "3/8 network space";
    LogInfo("tunnel: [3/8] opening the network space in {}",
            SdkStorageDir(true).string());
    if (!spaceManager_) {
      spaceManager_ =
          urnet::newNetworkSpaceManager(Narrow(SdkStorageDir(true).wstring()));
    }
    networkSpace_ = spaceManager_->importNetworkSpaceFromJson(config.network_space_json);

    // --- 4/8 DeviceLocal (stable provider identity via persisted key material) ---
    step = "4/8 device";
    auto km = LoadKeyMaterial();
    LogInfo("tunnel: [4/8] constructing DeviceLocal ({} identity)",
            km ? "persisted" : "new");
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
    LogInfo("tunnel: [4/8] device client_id={}", device_->getClientId());

    // --- 5/8 mTLS RPC listener the app's DeviceRemote dials ---
    step = "5/8 rpc";
    LogInfo("tunnel: [5/8] starting the device rpc listener on {}",
            config.rpc_listen_hostport);
    device_->setRpcServer(config.rpc_server_pem, config.rpc_client_cert_pem,
                          config.rpc_listen_hostport);
    rpcHostPort_ = config.rpc_listen_hostport;

    // --- 6/8 network settings (address/MTU/routes/DNS), from the device ---
    step = "6/8 network config";
    TunnelNetworkSettings settings;
    settings.local_address_v4 = device_->tunnelLocalAddress();
    if (settings.local_address_v4.empty()) settings.local_address_v4 = "169.254.2.1";
    settings.prefix_v4 = 24;
    settings.mtu = kTunnelMtu;
    // dns from the device: the dns settings' unencrypted local servers when set,
    // otherwise the distinct plain-DNS UpgradeMux mask. always plain :53, never OS-level
    // encrypted DNS: the mux performs the unencrypted-DNS -> DoH upgrade in-tunnel.
    // the tunnel is ipv4-only, so only the ipv4 resolvers apply
    if (auto dns = device_->tunnelDnsAddressesIpv4(); dns && !dns->empty()) {
      settings.dns_servers = *dns;
    } else {
      // Keep the exceptional fallback coupled to the SDK's separately tested
      // URnetwork-owned UpgradeMux identity.
      settings.dns_servers = {urnet::getDefaultTunnelDnsAddressIpv4()};
    }
    LogInfo("tunnel: [6/8] applying network settings addr={}/{} mtu={} dns=[{}]",
            settings.local_address_v4, settings.prefix_v4, settings.mtu,
            Join(settings.dns_servers));
    netConfig_ = std::make_unique<NetworkConfig>(adapter_->Luid());
    // Mark the machine as "routes installed" BEFORE installing them. The next
    // start reads this to tell an orderly shutdown from a crash; a marker left
    // by a run that died between the two is exactly the case we want reported.
    SetActiveMarker(true);
    if (!netConfig_->Apply(settings)) throw std::runtime_error("network config failed");

    // --- 7/8 split tunneling (driver optional) ---
    step = "7/8 split tunnel";
    LogInfo("tunnel: [7/8] split tunnel: {} path(s), {} mode",
            config.excluded_app_paths.size(),
            config.allowlist_mode ? "allowlist" : "denylist");
    splitTunnel_.Open();
    excludedPaths_ = config.excluded_app_paths;
    allowlist_ = config.allowlist_mode;
    PushExcludedToDriver(excludedPaths_, allowlist_);

    // --- 8/8 packet pump ---
    step = "8/8 pump";
    LogInfo("tunnel: [8/8] starting the packet pump");
    pump_ = std::make_unique<PacketPump>(*adapter_, *device_);
    pump_->Start();

    state_ = proto::TunnelState::Up;
    upSinceMillis_ = NowMillis();
    EgressInterfaces bound = egress_->Current();
    LogInfo("tunnel: UP in {}ms (rpc={} egress_v4_ifindex={} split_tunnel={})",
            upSinceMillis_ - startedAtMillis, rpcHostPort_, bound.index4,
            splitTunnel_.IsAvailable() ? "driver" : "none");
  } catch (const std::exception& e) {
    error_ = e.what();
    state_ = proto::TunnelState::Error;
    LogError("tunnel: start FAILED at step {}: {}", step, error_);
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
  const bool wasRunning = state_ != proto::TunnelState::Stopped;
  if (state_ == proto::TunnelState::Up || state_ == proto::TunnelState::Starting)
    state_ = proto::TunnelState::Stopping;
  if (wasRunning) LogInfo("tunnel: stopping (state was {})", proto::ToString(state_));

  // Tear down in reverse dependency order. The network config goes back FIRST
  // after the pump: while any of this is running the host is still pointed at
  // the tun, so the routes are the thing to give back soonest.
  if (pump_) { pump_->Stop(); pump_.reset(); }
  if (netConfig_) { netConfig_->Revert(); netConfig_.reset(); }
  // Routes are gone; the marker's job is done whether or not the rest unwinds.
  SetActiveMarker(false);
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
  if (wasRunning) LogInfo("tunnel: stopped, network restored");
}

// --- crash/orderly-exit bookkeeping ---------------------------------------
//
// This marker does NOT restore anything. The restore path is the wintun adapter
// dying with the process (see NetworkConfig.h). The marker exists so the next
// start can SAY that the last one ended badly, instead of the owner having to
// infer it — and so the startup sweep has a reason to shout.

std::filesystem::path TunnelController::ActiveMarkerPath() {
  return StorageRoot(/*isService=*/true) / L"tunnel_active";
}

void TunnelController::SetActiveMarker(bool active) {
  std::error_code ec;
  if (active) {
    std::ofstream f(ActiveMarkerPath(), std::ios::trunc);
    if (f) f << ::GetCurrentProcessId() << "\n";
  } else {
    std::filesystem::remove(ActiveMarkerPath(), ec);
  }
}

bool TunnelController::TakeActiveMarker() {
  std::error_code ec;
  if (!std::filesystem::exists(ActiveMarkerPath(), ec)) return false;
  std::filesystem::remove(ActiveMarkerPath(), ec);
  return true;
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
