// Orchestrates one tunnel session, mirroring the macOS PacketTunnelProvider:
// build the NetworkSpace + DeviceLocal from the app's config, start the mTLS RPC
// listener the app's DeviceRemote dials, bring up the wintun adapter, apply
// network settings, wire the packet pump, and keep R1 egress current.
//
// Thread-safety: Start/Stop are serialized by the ControlServer (single client).
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <atomic>
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

  // Bring a session up from the app's config. Stops any prior session first.
  // Returns a status; on failure state == Error with error set.
  //
  // config.mode selects what "up" means:
  //   StartMode::Tunnel  — all eight steps; rewrites routes and DNS.
  //   StartMode::RpcOnly — steps 2-5 only (no wintun adapter, so no elevation
  //                        needed), ending at the RPC listener. It returns
  //                        BEFORE step 6/8, the first call that touches the
  //                        machine's network, and reports state RpcOnly.
  proto::TunnelStatus Start(const proto::StartTunnel& config);

  // Force every subsequent Start into StartMode::RpcOnly regardless of what was
  // asked for. One-way and irreversible by design: `urnetworkd console
  // --rpc-only` calls this before the control pipe is served, so from then on
  // no client request of any shape can make this process reach step 6. The
  // downgrade is reported in the log and in TunnelStatus::mode, so a caller
  // that wanted a tunnel can see it did not get one.
  void ClampToRpcOnly();

  // Tear the tunnel down and restore the network.
  void Stop();

  // Update the split-tunnel app set + mode (driver, if present). allowlist=false:
  // excludedPaths bypass the tunnel; allowlist=true: ONLY excludedPaths tunnel.
  bool SetSplitTunnel(const std::vector<std::string>& excludedPaths, bool allowlist);

  // Clear persisted auth/session state (mirrors the macOS logout message).
  void Logout();

  proto::TunnelStatus Status();

  // "Routes are installed right now" marker under the service storage root.
  // Written when the network config is applied and removed when it is reverted,
  // so the NEXT start can tell an orderly shutdown from a crash. Purely a
  // reporting aid — the machine's actual restore path is the wintun adapter
  // going away with the process (see NetworkConfig.h).
  static std::filesystem::path ActiveMarkerPath();
  static void SetActiveMarker(bool active);
  // True if a marker was left behind; clears it either way.
  static bool TakeActiveMarker();
  // True if a marker was left behind, WITHOUT clearing it. For a run that is
  // only observing — the unelevated rpc-only mode — which must not consume the
  // evidence that an earlier elevated run died with routes installed. Taking it
  // there makes the next real start report a clean exit it did not have, and
  // that report is the only way the owner learns a crash cost them their
  // network.
  static bool PeekActiveMarker();

 private:
  proto::TunnelStatus StartLocked(const proto::StartTunnel& config);
  // Steps 6-8: network settings, split tunnel, packet pump. Split out of
  // StartLocked so the destructive half of the sequence is a named unit with
  // its OWN precondition, checked against the stored mode rather than against
  // the caller's argument. StartLocked already returns before reaching it in
  // rpc-only mode; this is the second, independent gate, and the one a future
  // caller cannot get wrong. Caller holds mutex_; `step` is the diagnostic
  // cursor StartLocked reports on failure.
  void BringUpTunnelLocked(const proto::StartTunnel& config, const char*& step);
  void StopLocked();
  // Load persisted DeviceLocalKeyMaterial blobs, or return nullopt on first run.
  std::optional<urnet::DeviceLocalKeyMaterial> LoadKeyMaterial();
  void PersistKeyMaterial(const urnet::DeviceLocalKeyMaterial& km);
  void PushExcludedToDriver(const std::vector<std::string>& paths, bool allowlist);
  // Re-point the driver at a new physical interface. Runs from the egress
  // monitor's change callback, on a system worker thread.
  void OnEgressChanged(EgressInterfaces egress);
  // Shared by both; the caller holds splitMutex_.
  void PushPhysicalAddressesLocked(const EgressInterfaces& egress);

  std::mutex mutex_;
  // Guards calls into splitTunnel_ ONLY, and is always the inner lock when both
  // are held. The egress change callback has to reach the driver WITHOUT
  // mutex_: StopLocked holds mutex_ while EgressMonitor::Stop() waits for that
  // very callback to return, so taking mutex_ there would deadlock the two
  // against each other. Correspondingly, nothing may hold splitMutex_ across a
  // call into the egress monitor.
  std::mutex splitMutex_;
  proto::TunnelState state_ = proto::TunnelState::Stopped;
  // The mode of the session being started / running. Set once at the top of
  // StartLocked and read by BringUpTunnelLocked's precondition and by Status();
  // both the reported state and the reported mode derive from this one value,
  // so they cannot disagree.
  proto::StartMode startMode_ = proto::StartMode::Tunnel;
  // Process-level clamp (see ClampToRpcOnly). Atomic and never cleared.
  std::atomic<bool> rpcOnlyClamp_{false};
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
