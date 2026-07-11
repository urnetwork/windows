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
  bool Start();
  void Stop();

 private:
  nlohmann::json Handle(const nlohmann::json& request);
  void PushState();

  TunnelController tunnel_;
  PipeServer pipe_;
};

}  // namespace urnw
