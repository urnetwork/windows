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
  // The egress push's worker can be inside PushState right now, having read the
  // handler before it was cleared. Waited out here: after the pipe has stopped,
  // so what is left of that push writes nothing (the pipe has no connection),
  // and before anything it reaches is destroyed — pipe_ is destroyed before
  // tunnel_.
  tunnel_.StopStatePushes();
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
      // The user's system proxy, as the app read it for this connect.
      diagnostics_.NoteStart(cfg);
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
      // The user's disconnect: the session's device may stay, providing
      // (TunnelController::Disconnect). The service's own stop closes it.
      tunnel_.Disconnect();
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
      // Not ok when the session lock was not free or the account's sdk state
      // could not be cleared: the app keeps the sign-out owed and sends it
      // again (Common/SignOut.h).
      const proto::Logout req = request.get<proto::Logout>();
      reply.ok = tunnel_.Logout(req.network_space_json);
      reply.status = tunnel_.Status();
      PushState();
    } else if (type == proto::msg::kStartProvider) {
      // Keep providing while disconnected (TunnelController::StartProvider). The
      // device registers under this machine's persisted identity with the
      // request's credentials, so the request must name the device exactly —
      // validated before anything is retired or built, like start_tunnel's
      // identity above. The mode and the machine's state are the controller's
      // to judge (provide::ProviderStartRefusal).
      proto::StartProvider req = request.get<proto::StartProvider>();
      if (!rpcsession::IsPairableInstanceId(req.instance_id) || req.by_jwt.empty() ||
          req.network_space_json.empty()) {
        throw std::runtime_error(
            "start_provider requires the device's client jwt, its exact instance "
            "id, and the network space");
      }
      std::string error;
      reply.ok = tunnel_.StartProvider(req, error);
      reply.error = error;
      // The status rides on a refusal too, so the app sees what is running (a
      // tunnel session, an armed floor) beside the reason.
      reply.status = tunnel_.Status();
      PushState();
    } else if (type == proto::msg::kStopProvider) {
      reply.ok = tunnel_.StopProvider();
      reply.status = tunnel_.Status();
      PushState();
    } else if (type == proto::msg::kSetNetworkCountry) {
      // The network country the app reads changed, or the app has just greeted
      // this service (TunnelController::SetNetworkCountry). No session lock, and
      // nothing a status reports moves, so nothing is pushed.
      proto::SetNetworkCountry s = request.get<proto::SetNetworkCountry>();
      tunnel_.SetNetworkCountry(s.network_country_code, s.network_country_source);
      // The country now in force, for the log feedback uploads, when it moved.
      diagnostics_.NoteNetworkCountry(tunnel_.NetworkCountry());
      reply.ok = true;
      reply.status = tunnel_.Status();
    } else if (type == proto::msg::kUploadLogs) {
      // "Send feedback with logs" whether or not a tunnel runs
      // (TunnelController::UploadLogs). The feedback id becomes part of the url
      // the device posts to, and a standalone device registers under this
      // machine's persisted identity with these credentials, so both are
      // validated before anything is touched, like start_provider's above.
      proto::UploadLogs req = request.get<proto::UploadLogs>();
      if (!proto::LooksLikeFeedbackId(req.feedback_id) ||
          !rpcsession::IsPairableInstanceId(req.instance_id) || req.by_jwt.empty() ||
          req.network_space_json.empty()) {
        throw std::runtime_error(
            "upload_logs requires the server's feedback id, the device's client "
            "jwt, its exact instance id, and the network space");
      }
      // The app's own log files ride in the upload, under app/
      // (Common/AppLogFiles.h): listed and opened while this thread acts as
      // the app's pipe client, so every open is the app's to make, never this
      // service's. An app that names no directory (an older one) sends none.
      AppLogHandles appLogFiles;
      if (!req.app_log_dir.empty() &&
          !pipe_.RunAsClient([&] { appLogFiles = OpenAppLogHandles(req.app_log_dir); })) {
        LogWarn("logs: the app's log files are left out: the app's pipe client could not be "
                "acted as");
      }
      // The carrier's line goes into the upload itself, so it is written from
      // inside, once the device is chosen (ServiceDiagnostics.h). The reply
      // comes once the upload is admitted; its end pushes the status that
      // carries its outcome (TunnelController's flight hook).
      const TunnelController::LogUploadResult result =
          tunnel_.UploadLogs(req, [this](std::string_view chosen) {
            diagnostics_.NoteLogUpload(chosen);
          }, std::move(appLogFiles));
      reply.ok = result.ok;
      reply.error = result.error;
      reply.log_upload_carrier = result.carrier;
      reply.log_upload_id = result.uploadId;
      reply.log_upload_busy = result.busy;
    } else if (type == proto::msg::kGetProviderStats) {
      // The provider-only device's statistics (TunnelController::ProviderStats),
      // answered like get_state: no session lock, no device call, and nothing
      // changed, so nothing is pushed.
      reply.ok = true;
      reply.provider_stats = tunnel_.ProviderStats();
    } else if (type == proto::msg::kSetProvideExtender) {
      // The Extender switch while disconnected (TunnelController::
      // SetProvideExtender), on this pipe under its access rule (kPipeSddl)
      // like every other request. A request without the value throws out of
      // the get<> as a failed reply. The tunnel status does not change, so
      // nothing is pushed; the next get_provider_stats carries the setting.
      proto::SetProvideExtender req = request.get<proto::SetProvideExtender>();
      std::string error;
      reply.ok = tunnel_.SetProvideExtender(req.provide_extender, error);
      reply.error = error;
    } else if (type == proto::msg::kSetMeasurementsEnabled) {
      // The lag.dev speed tests switch while disconnected (TunnelController::
      // SetMeasurementsEnabled), on this pipe under its access rule like the
      // Extender switch's. A request without the value throws out of the get<>
      // as a failed reply; the next get_provider_stats carries the setting.
      proto::SetMeasurementsEnabled req = request.get<proto::SetMeasurementsEnabled>();
      std::string error;
      reply.ok = tunnel_.SetMeasurementsEnabled(req.measurements_enabled, error);
      reply.error = error;
    } else if (type == proto::msg::kSetProvideExtenderPortMapping) {
      // The router port mapping switch while disconnected (TunnelController::
      // SetProvideExtenderPortMapping), on this pipe under its access rule like
      // the Extender switch's. A request without the value throws out of the
      // get<> as a failed reply, so nothing opens a port the user did not ask
      // for; the next get_provider_stats carries the setting.
      proto::SetProvideExtenderPortMapping req =
          request.get<proto::SetProvideExtenderPortMapping>();
      std::string error;
      reply.ok = tunnel_.SetProvideExtenderPortMapping(req.provide_extender_port_mapping, error);
      reply.error = error;
    } else if (type == proto::msg::kResetExtenders) {
      // "Reset extenders" in the app's Account > Extenders (TunnelController::
      // ResetExtenders), on this pipe under its access rule (kPipeSddl) like
      // every other request. A request that does not name its space and its
      // reset throws out of the get<> as a failed reply. A busy refusal says so
      // (reset_busy), and the app sends it again once the operation holding the
      // session lock has pushed the status that ends it. The tunnel status does
      // not change, so nothing is pushed.
      const proto::ResetExtenders req = request.get<proto::ResetExtenders>();
      const TunnelController::ExtenderResetResult result = tunnel_.ResetExtenders(req);
      reply.ok = result.ok;
      reply.reset = result.reset;
      reply.reset_busy = result.busy;
      reply.error = result.error;
    } else if (type == proto::msg::kSetIngressDns) {
      // The connection region and the bootstrap DNS-over-HTTPS servers the app
      // saved, or holds as it reattaches (TunnelController::SetIngressDns), on
      // this pipe under its access rule (kPipeSddl) like every other request. A
      // request that does not name its space and both values throws out of the
      // get<> as a failed reply. A busy refusal says so (ingress_dns_busy), and
      // the app sends the values again once the operation holding the session
      // lock has pushed the status that ends it. The tunnel status does not
      // change, so nothing is pushed.
      const proto::SetIngressDns req = request.get<proto::SetIngressDns>();
      const TunnelController::IngressDnsResult result = tunnel_.SetIngressDns(req);
      reply.ok = result.ok;
      reply.ingress_dns_held = result.held;
      reply.ingress_dns_busy = result.busy;
      reply.error = result.error;
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
  //
  // The diagnostic lines of the state being pushed go first, for the same
  // reason and so the flush takes them along (ServiceDiagnostics.h).
  diagnostics_.NoteStatus(tunnel_.Status(), tunnel_.KillSwitchPreference(),
                          tunnel_.NetworkCountry());
  urnet::flushGlog();
  nlohmann::json event;
  event["event"] = "tunnel_state";
  event["status"] = tunnel_.Status();
  pipe_.PushEvent(event);
}

}  // namespace urnw
