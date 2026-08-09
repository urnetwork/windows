// Typed wrapper over the control PipeClient: the app's view of the service.
// Drives the tunnel lifecycle and surfaces state-change events.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <functional>

#include "PipeClient.h"
#include "Protocol.h"

namespace urnw {

class ServiceClient {
 public:
  using StateHandler = std::function<void(const proto::TunnelStatus&)>;
  // The control channel dropped without us asking. See PipeClient.h: the
  // service owns the tunnel, so this is the app's only notice that there is no
  // longer one. Runs on the pipe reader thread and must not call Connect().
  using DisconnectHandler = std::function<void()>;

  bool Connect();
  bool IsConnected() const { return pipe_.IsConnected(); }
  void SetStateHandler(StateHandler h) { onState_ = std::move(h); }
  void SetDisconnectHandler(DisconnectHandler h) { onDisconnect_ = std::move(h); }

  proto::TunnelStatus Hello();
  proto::TunnelStatus StartTunnel(const proto::StartTunnel& config);
  proto::TunnelStatus StopTunnel();
  proto::TunnelStatus GetState();
  bool SetSplitTunnel(const std::vector<std::string>& excludedPaths, bool allowlist = false);
  // Tell the service the kill switch changed. The app still owns the setting
  // and its persistence; the service owns the WFP policy the setting now drives.
  bool SetKillSwitch(bool on);
  bool Logout();

 private:
  proto::TunnelStatus CallStatus(const nlohmann::json& request);

  PipeClient pipe_;
  StateHandler onState_;
  DisconnectHandler onDisconnect_;
};

}  // namespace urnw
