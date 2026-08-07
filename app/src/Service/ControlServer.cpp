// SPDX-License-Identifier: MPL-2.0
#include "ControlServer.h"

#include "Log.h"

namespace urnw {

bool ControlServer::Start() {
  return pipe_.Start([this](const nlohmann::json& req) { return Handle(req); });
}

void ControlServer::Stop() {
  pipe_.Stop();
  tunnel_.Stop();
}

nlohmann::json ControlServer::Handle(const nlohmann::json& request) {
  const std::string type = proto::TypeOf(request);
  proto::Reply reply;
  reply.in_reply_to = type;

  try {
    if (type == proto::msg::kHello || type == proto::msg::kGetState) {
      reply.ok = true;
      reply.status = tunnel_.Status();
    } else if (type == proto::msg::kStartTunnel) {
      proto::StartTunnel cfg = request.get<proto::StartTunnel>();
      proto::TunnelStatus st = tunnel_.Start(cfg);
      // "ok" means "I did what you asked" — live AND in the mode requested.
      // A clamped process serving a tunnel request produces a live session, but
      // not the one the caller asked for, and reporting ok for that is how a
      // caller ends up believing it has a tunnel. The status carries the mode
      // actually served, so the caller can see which way it differed.
      // (An unknown mode string throws out of the get<> above and is answered
      // as a failed reply, so a garbled mode never starts anything.)
      reply.ok = proto::IsSessionLive(st.state) && st.mode == cfg.mode;
      // Left EMPTY on a pure mode mismatch, deliberately: the status already
      // says what happened, and ServiceClient::CallStatus overwrites state with
      // Error whenever !ok carries an error string — which would erase the very
      // mode the caller needs in order to react correctly.
      reply.error = st.error;
      reply.status = st;
      PushState();
    } else if (type == proto::msg::kStopTunnel) {
      tunnel_.Stop();
      reply.ok = true;
      reply.status = tunnel_.Status();
      PushState();
    } else if (type == proto::msg::kSetSplitTunnel) {
      proto::SetSplitTunnel s = request.get<proto::SetSplitTunnel>();
      reply.ok = tunnel_.SetSplitTunnel(s.excluded_app_paths, s.allowlist_mode);
      reply.status = tunnel_.Status();
    } else if (type == proto::msg::kLogout) {
      tunnel_.Logout();
      reply.ok = true;
      reply.status = tunnel_.Status();
      PushState();
    } else {
      reply.ok = false;
      reply.error = "unknown request type: " + type;
    }
  } catch (const std::exception& e) {
    reply.ok = false;
    reply.error = e.what();
    LogError("control: handling {} failed: {}", type, e.what());
  }

  nlohmann::json j = reply;
  return j;
}

void ControlServer::PushState() {
  nlohmann::json event;
  event["event"] = "tunnel_state";
  event["status"] = tunnel_.Status();
  pipe_.PushEvent(event);
}

}  // namespace urnw
