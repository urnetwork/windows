// SPDX-License-Identifier: MPL-2.0
// the project compiles with /Yu"pch.h" (App.vcxproj), so every translation unit
// must include it first
#include "pch.h"

#include "ServiceClient.h"

#include "Log.h"

namespace urnw {

bool ServiceClient::Connect() {
  pipe_.SetEventHandler([this](const nlohmann::json& event) {
    if (event.value("event", "") == "tunnel_state" && onState_) {
      if (auto it = event.find("status"); it != event.end()) {
        onState_(it->get<proto::TunnelStatus>());
      }
    }
  });
  return pipe_.Connect();
}

proto::TunnelStatus ServiceClient::CallStatus(const nlohmann::json& request) {
  proto::TunnelStatus status;
  try {
    nlohmann::json reply = pipe_.Call(request);
    proto::Reply r = reply.get<proto::Reply>();
    if (r.status) status = *r.status;
    if (!r.ok && !r.error.empty()) {
      status.state = proto::TunnelState::Error;
      status.error = r.error;
    }
  } catch (const std::exception& e) {
    LogError("service: call failed: {}", e.what());
    status.state = proto::TunnelState::Error;
    status.error = e.what();
  }
  return status;
}

proto::TunnelStatus ServiceClient::Hello() {
  return CallStatus(proto::Request(proto::msg::kHello));
}

proto::TunnelStatus ServiceClient::StartTunnel(const proto::StartTunnel& config) {
  nlohmann::json body = config;
  return CallStatus(proto::Request(proto::msg::kStartTunnel, body));
}

proto::TunnelStatus ServiceClient::StopTunnel() {
  return CallStatus(proto::Request(proto::msg::kStopTunnel));
}

proto::TunnelStatus ServiceClient::GetState() {
  return CallStatus(proto::Request(proto::msg::kGetState));
}

bool ServiceClient::SetSplitTunnel(const std::vector<std::string>& excludedPaths, bool allowlist) {
  proto::SetSplitTunnel s;
  s.excluded_app_paths = excludedPaths;
  s.allowlist_mode = allowlist;
  nlohmann::json body = s;
  try {
    nlohmann::json reply = pipe_.Call(proto::Request(proto::msg::kSetSplitTunnel, body));
    return reply.value("ok", false);
  } catch (const std::exception& e) {
    LogError("service: set split tunnel failed: {}", e.what());
    return false;
  }
}

bool ServiceClient::Logout() {
  try {
    nlohmann::json reply = pipe_.Call(proto::Request(proto::msg::kLogout));
    return reply.value("ok", false);
  } catch (const std::exception& e) {
    LogError("service: logout failed: {}", e.what());
    return false;
  }
}

}  // namespace urnw
