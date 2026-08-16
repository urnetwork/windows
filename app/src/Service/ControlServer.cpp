// SPDX-License-Identifier: MPL-2.0
#include "ControlServer.h"

#include "Log.h"
#include "RpcSessionBlob.h"
#include "Sdk.h"  // urnet::flushGlog — see PushState

namespace urnw {

bool ControlServer::Start() {
  // THE ONE TRANSITION WITH NO REPLY TO RIDE ON. Every other state change here
  // is the direct result of a request, so the handler below pushes the new
  // status when it answers. The dead-tunnel failsafe is not: it stops the
  // tunnel on its own initiative, from its own thread, and without this the app
  // would keep rendering a live tunnel until its next poll — which is exactly
  // the window in which the user is looking at the screen wondering what
  // happened to their internet.
  //
  // Invoked with the session lock RELEASED (see TunnelController::
  // NotifyStateChanged), so PushState's Status() call cannot deadlock against
  // the teardown that raised it.
  tunnel_.SetOnStateChanged([this] { PushState(); });
  return pipe_.Start([this](const nlohmann::json& req) { return Handle(req); });
}

void ControlServer::Stop() {
  // Cleared FIRST. The teardown below can push a transition, and a handler that
  // reaches a pipe already being torn down is a use of a stopping object for no
  // benefit — the client is going away with the service.
  tunnel_.SetOnStateChanged(nullptr);
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
      // Validate the security-critical adoption identity BEFORE Start(), whose
      // first operation tears down any existing tunnel. This also makes an old
      // app fail closed instead of replacing a live v3 session with one that no
      // future process can identify safely.
      if (!rpcsession::IsPairableInstanceId(cfg.instance_id) ||
          !rpcsession::IsOpaqueSessionId(cfg.rpc_session_id) ||
          cfg.rpc_server_pem.empty() || cfg.rpc_client_cert_pem.empty() ||
          !cfg.rpc_listen_hostport.starts_with("127.0.0.1:")) {
        throw std::runtime_error(
            "start_tunnel requires an exact instance id, RPC session id, "
            "loopback endpoint, and per-session mTLS credentials");
      }
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
    } else if (type == proto::msg::kSetKillSwitch) {
      // The app owns the setting and its persistence; this only tells the
      // service what the setting now is, so the firewall policy follows it at
      // the next transition (and immediately when it is turned OFF while
      // armed). A service that never hears about a mid-session change would
      // hold a policy the user has already switched off.
      proto::SetKillSwitch s = request.get<proto::SetKillSwitch>();
      reply.ok = tunnel_.SetKillSwitch(s.on);
      reply.status = tunnel_.Status();
      PushState();
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
  // EVERY TUNNEL STATE TRANSITION FLUSHES THE SDK'S LOG, and this is the place
  // to do it from.
  //
  // The SDK's own INFO log is where the cause of a death is most likely
  // written, glog buffers it, and before this change urnet::flushGlog() had ONE
  // call site in the whole repo (WindowTrace.cpp, at the teardown of an opt-in
  // trace) — so one of the four silent deaths lost 28 seconds of exactly the
  // evidence being looked for.
  //
  // HERE rather than inside TunnelController's state setter, which is where it
  // looks like it belongs: this runs at the RPC boundary, after the tunnel call
  // has returned and released mutex_, so it cannot sequence a call that may
  // block inside the Go runtime ahead of the route revert. See the note on
  // TunnelController::SetStateLocked.
  urnet::flushGlog();
  nlohmann::json event;
  event["event"] = "tunnel_state";
  event["status"] = tunnel_.Status();
  pipe_.PushEvent(event);
}

}  // namespace urnw
