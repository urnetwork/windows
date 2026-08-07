// URnetwork control protocol — the contract between the tray app (URnetwork.exe,
// unprivileged, per user) and the service (urnetworkd.exe, LocalSystem).
//
// Transport: newline-delimited UTF-8 JSON over the named pipe
// \\.\pipe\urnetwork.control (see PipeName.h). Each request is one JSON object
// terminated by '\n'; each reply is one JSON object terminated by '\n'. The
// service also pushes unsolicited event objects on the same connection.
//
// This mirrors the macOS app<->extension boundary: `start_tunnel` carries the
// same fields the macOS app puts in NETunnelProviderProtocol.providerConfiguration,
// and the device RPC (mTLS WebSocket on loopback) is established separately by
// the SDK once the tunnel is up — this channel only carries lifecycle + config.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace urnw::proto {

// bump when the wire format changes incompatibly; hello negotiates it
inline constexpr int kProtocolVersion = 1;

// ---- message type tags ----------------------------------------------------

namespace msg {
inline constexpr const char* kHello = "hello";                   // app -> service
inline constexpr const char* kStartTunnel = "start_tunnel";      // app -> service
inline constexpr const char* kStopTunnel = "stop_tunnel";        // app -> service
inline constexpr const char* kGetState = "get_state";            // app -> service
inline constexpr const char* kSetSplitTunnel = "set_split_tunnel"; // app -> service
inline constexpr const char* kLogout = "logout";                 // app -> service
inline constexpr const char* kReply = "reply";                   // service -> app
inline constexpr const char* kEvent = "event";                   // service -> app (unsolicited)
}  // namespace msg

// What a `start_tunnel` is asking the service to bring up.
//
// RpcOnly exists so the app can be driven end to end before the tunnel itself
// is written: it runs steps 1-5 of TunnelController::StartLocked minus the
// wintun adapter (NetworkSpace, DeviceLocal, the mTLS RPC listener) and STOPS
// before step 6/8, which is the first call that rewrites the machine's routes
// and DNS. Nothing in an RpcOnly session touches the routing table, and because
// no adapter is created it needs no elevation at all.
enum class StartMode {
  Tunnel,   // the real thing: adapter, routes, DNS, packet pump
  RpcOnly,  // DeviceLocal + RPC listener only; the network is not touched
};

inline const char* ToString(StartMode m) {
  switch (m) {
    case StartMode::Tunnel: return "tunnel";
    case StartMode::RpcOnly: return "rpc_only";
  }
  return "tunnel";
}

// Absent parses as Tunnel — that is what every pre-mode client means. An
// unrecognized string returns nullopt rather than falling back: a request that
// meant "do not touch my network" and was misspelled must not be answered with
// a real tunnel. StartTunnel's from_json turns that nullopt into a throw, which
// the ControlServer answers as a failed reply.
inline std::optional<StartMode> StartModeFromString(const std::string& s) {
  if (s == "tunnel") return StartMode::Tunnel;
  if (s == "rpc_only") return StartMode::RpcOnly;
  return std::nullopt;
}

// tunnel lifecycle state, reported in replies and state-change events
enum class TunnelState {
  Stopped,
  Starting,
  Up,       // wintun adapter up, DeviceLocal running, RPC listener ready
  Stopping,
  Error,
  // An rpc-only session is serving: DeviceLocal + RPC listener are live and NO
  // routes exist. Deliberately NOT `Up` — every `state == Up` test in the app
  // means "connected", and this is the state in which the app must not say so.
  // An older peer that does not know this string parses it as `Stopped`, which
  // is the safe direction to be wrong in.
  RpcOnly,
};

inline const char* ToString(TunnelState s) {
  switch (s) {
    case TunnelState::Stopped: return "stopped";
    case TunnelState::Starting: return "starting";
    case TunnelState::Up: return "up";
    case TunnelState::Stopping: return "stopping";
    case TunnelState::Error: return "error";
    case TunnelState::RpcOnly: return "rpc_only";
  }
  return "unknown";
}

inline TunnelState TunnelStateFromString(const std::string& s) {
  if (s == "starting") return TunnelState::Starting;
  if (s == "up") return TunnelState::Up;
  if (s == "stopping") return TunnelState::Stopping;
  if (s == "error") return TunnelState::Error;
  if (s == "rpc_only") return TunnelState::RpcOnly;
  return TunnelState::Stopped;
}

// "The service has a live DeviceLocal and RPC listener the app can dial." True
// for both modes. Use this for session/reattach decisions.
inline bool IsSessionLive(TunnelState s) {
  return s == TunnelState::Up || s == TunnelState::RpcOnly;
}

// "Traffic is actually being carried." Use this, never IsSessionLive, for
// anything the user reads as "connected".
inline bool IsTunnelUp(TunnelState s) { return s == TunnelState::Up; }

// ---- request payloads -----------------------------------------------------

// StartTunnel mirrors the macOS providerConfiguration. The service builds its
// own NetworkSpace from network_space_json and constructs the DeviceLocal with
// these credentials, then starts the RPC listener the app's DeviceRemote dials.
struct StartTunnel {
  std::string by_jwt;              // client JWT for this device
  std::string network_space_json;  // NetworkSpace.toJson() from the app
  std::string instance_id;         // stable instance UUID
  std::string device_description;  // e.g. "windows-desktop"
  std::string device_spec;         // hardware/model string
  std::string app_version;         // "<version>-<build>"
  // device RPC key material (per session, from GenerateDeviceRpcKeyMaterial):
  std::string rpc_server_pem;      // server key+cert the service presents
  std::string rpc_client_cert_pem; // client cert the service pins (mTLS)
  std::string rpc_listen_hostport; // e.g. "127.0.0.1:12042"
  // split tunnel: process image paths. In denylist mode (allowlist_mode=false)
  // these are BYPASS paths (they leave the tunnel; everyone else tunnels). In
  // allowlist mode they are the KEEP-ON-TUNNEL paths (only these tunnel; everyone
  // else bypasses) - mirrors Android's "inclusions take precedence".
  std::vector<std::string> excluded_app_paths;
  bool allowlist_mode = false;
  // Tunnel unless the app explicitly asks otherwise, so an absent field on the
  // wire keeps its pre-mode meaning. See StartMode.
  StartMode mode = StartMode::Tunnel;
};

struct SetSplitTunnel {
  std::vector<std::string> excluded_app_paths;  // meaning depends on allowlist_mode (see StartTunnel)
  bool allowlist_mode = false;
};

// ---- reply / state payload ------------------------------------------------

struct TunnelStatus {
  TunnelState state = TunnelState::Stopped;
  std::string rpc_listen_hostport;  // echoed so the app can dial the DeviceRemote
  std::string error;                // set when state == Error
  std::string service_version;
  int protocol_version = kProtocolVersion;
  // best-effort counters (authoritative stats come over the device RPC)
  int64_t tunnel_local_up_millis = 0;
  // The mode of the session this status describes. Reported separately from
  // `state` so it survives Starting/Stopping/Error, where `state` says nothing
  // about which kind of session was asked for. In a live session the two agree
  // by construction: the controller derives both from one stored mode.
  StartMode mode = StartMode::Tunnel;
  // True only when routes and DNS are actually installed right now. This is the
  // field to trust for "is my traffic going through the tunnel"; it is false for
  // the whole life of an rpc-only session.
  bool routes_installed = false;
};

struct Reply {
  bool ok = false;
  std::string error;             // set when !ok
  std::optional<TunnelStatus> status;
  std::string in_reply_to;       // request type tag this answers
};

// ---- JSON (de)serialization ----------------------------------------------
// Uses nlohmann's find/get_to pattern so unknown/absent fields are tolerated,
// matching the SDK wrapper's forward-compatible convention.

inline void to_json(nlohmann::json& j, const StartTunnel& v) {
  j = {
      {"by_jwt", v.by_jwt},
      {"network_space_json", v.network_space_json},
      {"instance_id", v.instance_id},
      {"device_description", v.device_description},
      {"device_spec", v.device_spec},
      {"app_version", v.app_version},
      {"rpc_server_pem", v.rpc_server_pem},
      {"rpc_client_cert_pem", v.rpc_client_cert_pem},
      {"rpc_listen_hostport", v.rpc_listen_hostport},
      {"excluded_app_paths", v.excluded_app_paths},
      {"allowlist_mode", v.allowlist_mode},
      {"mode", ToString(v.mode)},
  };
}

inline void from_json(const nlohmann::json& j, StartTunnel& v) {
  auto get = [&](const char* k, auto& out) {
    if (auto it = j.find(k); it != j.end() && !it->is_null()) it->get_to(out);
  };
  if (auto it = j.find("mode"); it != j.end() && it->is_string()) {
    const std::string raw = it->get<std::string>();
    auto mode = StartModeFromString(raw);
    // Deliberately fatal. Silently defaulting an unrecognized mode to Tunnel
    // would answer "do not touch my network" with a rewritten route table.
    if (!mode) throw std::runtime_error("unknown start mode: " + raw);
    v.mode = *mode;
  }
  get("by_jwt", v.by_jwt);
  get("network_space_json", v.network_space_json);
  get("instance_id", v.instance_id);
  get("device_description", v.device_description);
  get("device_spec", v.device_spec);
  get("app_version", v.app_version);
  get("rpc_server_pem", v.rpc_server_pem);
  get("rpc_client_cert_pem", v.rpc_client_cert_pem);
  get("rpc_listen_hostport", v.rpc_listen_hostport);
  get("excluded_app_paths", v.excluded_app_paths);
  get("allowlist_mode", v.allowlist_mode);
}

inline void to_json(nlohmann::json& j, const SetSplitTunnel& v) {
  j = {{"excluded_app_paths", v.excluded_app_paths},
       {"allowlist_mode", v.allowlist_mode}};
}
inline void from_json(const nlohmann::json& j, SetSplitTunnel& v) {
  auto get = [&](const char* k, auto& out) {
    if (auto it = j.find(k); it != j.end() && !it->is_null()) it->get_to(out);
  };
  get("excluded_app_paths", v.excluded_app_paths);
  get("allowlist_mode", v.allowlist_mode);
}

inline void to_json(nlohmann::json& j, const TunnelStatus& v) {
  j = {
      {"state", ToString(v.state)},
      {"rpc_listen_hostport", v.rpc_listen_hostport},
      {"error", v.error},
      {"service_version", v.service_version},
      {"protocol_version", v.protocol_version},
      {"tunnel_local_up_millis", v.tunnel_local_up_millis},
      {"mode", ToString(v.mode)},
      {"routes_installed", v.routes_installed},
  };
}

inline void from_json(const nlohmann::json& j, TunnelStatus& v) {
  if (auto it = j.find("state"); it != j.end() && it->is_string())
    v.state = TunnelStateFromString(it->get<std::string>());
  // A status is a report, so an unreadable mode degrades rather than throws —
  // but it degrades to RpcOnly, the mode that claims LESS. (The service always
  // writes a valid one; this is for a peer we do not control.)
  if (auto it = j.find("mode"); it != j.end() && it->is_string())
    v.mode = StartModeFromString(it->get<std::string>()).value_or(StartMode::RpcOnly);
  auto get = [&](const char* k, auto& out) {
    if (auto it = j.find(k); it != j.end() && !it->is_null()) it->get_to(out);
  };
  get("rpc_listen_hostport", v.rpc_listen_hostport);
  get("error", v.error);
  get("service_version", v.service_version);
  get("protocol_version", v.protocol_version);
  get("tunnel_local_up_millis", v.tunnel_local_up_millis);
  get("routes_installed", v.routes_installed);
}

inline void to_json(nlohmann::json& j, const Reply& v) {
  j = {{"type", msg::kReply}, {"ok", v.ok}, {"error", v.error},
       {"in_reply_to", v.in_reply_to}};
  if (v.status) j["status"] = *v.status;
}

inline void from_json(const nlohmann::json& j, Reply& v) {
  auto get = [&](const char* k, auto& out) {
    if (auto it = j.find(k); it != j.end() && !it->is_null()) it->get_to(out);
  };
  get("ok", v.ok);
  get("error", v.error);
  get("in_reply_to", v.in_reply_to);
  if (auto it = j.find("status"); it != j.end() && it->is_object()) {
    TunnelStatus s;
    it->get_to(s);
    v.status = s;
  }
}

// Envelope helpers: every message on the wire has a top-level "type" tag.
inline std::string TypeOf(const nlohmann::json& j) {
  if (auto it = j.find("type"); it != j.end() && it->is_string())
    return it->get<std::string>();
  return {};
}

inline nlohmann::json Request(const char* type, nlohmann::json body = nlohmann::json::object()) {
  body["type"] = type;
  return body;
}

}  // namespace urnw::proto
