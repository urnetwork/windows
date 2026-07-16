// Orchestrates one tunnel session, mirroring the macOS PacketTunnelProvider:
// build the NetworkSpace + DeviceLocal from the app's config, start the mTLS RPC
// listener the app's DeviceRemote dials, bring up the wintun adapter, apply
// network settings, wire the packet pump, and keep R1 egress current.
//
// Thread-safety: Start/Stop are serialized by the ControlServer (single client).
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "EgressMonitor.h"
#include "NetworkConfig.h"
#include "PacketPump.h"
#include "Protocol.h"
#include "Sdk.h"
#include "SplitTunnelClient.h"
#include "Wintun.h"

namespace urnw {

class TunnelController {
 public:
  TunnelController();
  ~TunnelController();

  // Bring the tunnel up from the app's config. Stops any prior session first.
  // Returns a status; on failure state == Error with error set.
  proto::TunnelStatus Start(const proto::StartTunnel& config);

  // Tear the tunnel down and restore the network.
  void Stop();

  // Update the split-tunnel app set + mode (driver, if present). allowlist=false:
  // excludedPaths bypass the tunnel; allowlist=true: ONLY excludedPaths tunnel.
  bool SetSplitTunnel(const std::vector<std::string>& excludedPaths, bool allowlist);

  // Clear persisted auth/session state (mirrors the macOS logout message).
  void Logout();

  proto::TunnelStatus Status();

 private:
  proto::TunnelStatus StartLocked(const proto::StartTunnel& config);
  void StopLocked();
  // Load persisted DeviceLocalKeyMaterial blobs, or return nullopt on first run.
  std::optional<urnet::DeviceLocalKeyMaterial> LoadKeyMaterial();
  void PersistKeyMaterial(const urnet::DeviceLocalKeyMaterial& km);
  void PushExcludedToDriver(const std::vector<std::string>& paths, bool allowlist);

  std::mutex mutex_;
  proto::TunnelState state_ = proto::TunnelState::Stopped;
  std::string error_;
  std::string rpcHostPort_;
  int64_t upSinceMillis_ = 0;

  std::filesystem::path storageDir_;

  // SDK objects. NetworkSpaceManager persists across sessions; the rest are
  // per-session.
  std::optional<urnet::NetworkSpaceManager> spaceManager_;
  std::optional<urnet::NetworkSpace> networkSpace_;
  std::optional<urnet::DeviceLocal> device_;

  // Native tunnel plumbing.
  std::unique_ptr<Wintun> wintun_;
  std::unique_ptr<WintunAdapter> adapter_;
  std::unique_ptr<NetworkConfig> netConfig_;
  std::unique_ptr<EgressMonitor> egress_;
  std::unique_ptr<PacketPump> pump_;
  SplitTunnelClient splitTunnel_;
  std::vector<std::string> excludedPaths_;
  bool allowlist_ = false;
};

}  // namespace urnw
