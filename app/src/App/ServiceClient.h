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

  bool Connect();
  bool IsConnected() const { return pipe_.IsConnected(); }
  void SetStateHandler(StateHandler h) { onState_ = std::move(h); }

  proto::TunnelStatus Hello();
  proto::TunnelStatus StartTunnel(const proto::StartTunnel& config);
  proto::TunnelStatus StopTunnel();
  proto::TunnelStatus GetState();
  bool SetSplitTunnel(const std::vector<std::string>& excludedPaths, bool allowlist = false);
  bool Logout();

 private:
  proto::TunnelStatus CallStatus(const nlohmann::json& request);

  PipeClient pipe_;
  StateHandler onState_;
};

}  // namespace urnw
