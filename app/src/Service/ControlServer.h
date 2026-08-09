// Bridges the control pipe to the TunnelController: parses requests, drives the
// tunnel, and pushes state-change events back to the app.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "PipeServer.h"
#include "TunnelController.h"

namespace urnw {

class ControlServer {
 public:
  // Clamp this whole process to rpc-only: every start_tunnel is served as
  // StartMode::RpcOnly whatever it asked for, so no request from any client can
  // make this instance write a route. Set from `urnetworkd console --rpc-only`
  // BEFORE Start(). The clamp is one-way — nothing turns it back off — so a
  // process launched this way cannot become able to touch the network later.
  void ClampToRpcOnly() { tunnel_.ClampToRpcOnly(); }

  // Staged bring-up: every start_tunnel stops after step N of 8 and unwinds
  // through the ordinary teardown. Set from `urnetworkd console
  // --stop-after=<N>` BEFORE Start(), like the clamp above and for the same
  // reason — the flag must be in force before the pipe can accept a request,
  // not consulted per request. See TunnelController::SetStopAfterStep; it
  // narrows the sequence and can never widen it, including past the rpc-only
  // clamp when both are set.
  void SetStopAfterStep(int step) { tunnel_.SetStopAfterStep(step); }

  bool Start();
  void Stop();

 private:
  nlohmann::json Handle(const nlohmann::json& request);
  void PushState();

  TunnelController tunnel_;
  PipeServer pipe_;
};

}  // namespace urnw
