// The persisted "last-good RPC session" blob — the file the app reads at launch
// to decide whether it may REATTACH its DeviceRemote to a tunnel the service is
// already running, instead of starting a new one.
//
// WHY THIS IS A COMMON HEADER RATHER THAN THREE FUNCTIONS IN SdkHost.cpp.
// The blob is the whole of the app's memory of a session it did not start. Get
// it wrong and the app either refuses a live tunnel it could have adopted (an
// annoyance) or adopts one it cannot actually drive (the failure this file was
// written for: the app reattached, logged "session bootstrapped", and then read
// Disconnected with no provider dots for the rest of the session because every
// RPC sync was rejected). Neither outcome can be reproduced without an elevated
// service and a live tunnel, so the parsing half lives here — pure, no file
// handles, no Windows headers — where the service selftest links it and pins
// the migration and malformed cases as tested claims. Same arrangement
// VersionGrammar.h and ConnectionHealth.h already have, for the same reason.
//
// THE FIELD THAT MATTERS. `instance_id` is the device instance id the SERVICE's
// DeviceLocal was born with at start_tunnel. The remote's SyncRequest carries
// it and DeviceLocalRpc.Sync REFUSES every sync whose nonzero instance id is
// not the local's ("device instance mismatch: remote expects X, local is Y").
// The app used to pass whatever instance id was on disk AT LAUNCH — but the SDK
// rotates that id on any changed by-client JWT string, and a JWT refresh
// re-signs the same client, so the disk id rotates while the running
// DeviceLocal keeps the one it was born with. Any refresh between tunnel start
// and app restart therefore made reattach permanently unpairable. Persisting
// the id WITH the session is what makes the pairing survive the refresh.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace urnw::rpcsession {

// The blob, as it is on disk. Field names are the JSON keys.
struct Blob {
  // Per-session mTLS material for the loopback RPC listener.
  std::string client_pem;
  std::string server_cert_pem;
  // "127.0.0.1:<port>". Empty is not a session — see Parse.
  std::string host_port;
  // The instance id to pair with, canonical 36-char UUID form, or EMPTY.
  //
  // Empty means "this blob cannot be paired" and it is deliberately the same
  // answer for all three ways of getting there: written by a build older than
  // this field, absent, or present but not a shape the SDK can parse. Callers
  // must treat empty as NOT-REATTACHABLE and start a fresh session instead.
  //
  // They must NOT paper over it by handing the SDK an empty instance id: the
  // cgo boundary maps an empty string to a nil *Id (cgo/convert.go goId), and
  // NewDeviceRemoteWithDefaults dereferences it — the panic is swallowed by the
  // export's recover() and the call returns handle 0, so C++ gets a DeviceRemote
  // that throws nothing and works at nothing. Verified against sdk main; do not
  // rediscover it the hard way.
  std::string instance_id;
};

namespace blob_detail {

inline constexpr bool IsHex(char c) noexcept {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
         (c >= 'A' && c <= 'F');
}

// The string field at `key`, or empty when the key is absent OR holds something
// that is not a string. Total by construction — no exception can escape a parse
// of a file some other program may have rewritten, and "unreadable" and "absent"
// are the same answer because both mean the same thing to every caller here.
inline std::string StringField(const nlohmann::json& j, const char* key) {
  const auto it = j.find(key);
  if (it == j.end() || !it->is_string()) return {};
  return it->get<std::string>();
}

}  // namespace blob_detail

// Whether `s` is an instance id this app may hand to the SDK for pairing.
//
// STRICTER THAN THE SDK ON PURPOSE, in both directions:
//
//  * The SDK's parseUuid also accepts the 32-char dashless form, and for the
//    36-char form it slices at fixed offsets without checking that the
//    separators are actually dashes — so `0123456789abcdef0123456789abcdefXXXX`
//    reaches hex.DecodeString as garbage and fails there. Only the canonical
//    dashed form ever comes OUT of the SDK (encodeUuid), so that is the only
//    form we accept going back in. Anything else is a corrupt file, and the
//    cost of guessing is a device handle that silently does nothing.
//
//  * The NIL uuid is REFUSED even though it parses. A zero instance id is the
//    value DeviceLocalRpc.Sync uses to SKIP the pairing check entirely (kept
//    for remotes built before the field existed). Accepting it out of a file
//    would mean a corrupt or hand-edited blob could silently disable the exact
//    check this whole change exists to make work.
inline constexpr bool IsPairableInstanceId(std::string_view s) noexcept {
  if (s.size() != 36) return false;
  bool anyNonZero = false;
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (s[i] != '-') return false;
      continue;
    }
    if (!blob_detail::IsHex(s[i])) return false;
    if (s[i] != '0') anyNonZero = true;
  }
  return anyNonZero;
}

// The blob as it goes to disk. Always writes every key, including an empty
// instance_id: a writer that omits fields it happens not to have makes "this
// build did not know about the field" and "this session had no id" look
// identical to the next reader, and only one of those is worth a migration.
inline std::string Serialize(const Blob& b) {
  const nlohmann::json j = {{"client_pem", b.client_pem},
                            {"server_cert_pem", b.server_cert_pem},
                            {"host_port", b.host_port},
                            {"instance_id", b.instance_id}};
  return j.dump();
}

// The blob as it comes off disk. nullopt means "there is no session here" —
// unparseable, not an object, or carrying no host_port, which is the one field
// without which there is nothing to reattach TO.
//
// A blob that is otherwise fine but whose instance_id is missing or unusable is
// NOT nullopt: it comes back with instance_id empty, so the caller can tell
// "no session" from "a session I must not pair with" and log the difference.
inline std::optional<Blob> Parse(std::string_view text) {
  // The non-throwing parse overload: this file is on the boundary with the
  // filesystem, and a truncated write from a previous run must be a nullopt,
  // never an exception crossing into a bootstrap path.
  // Iterator pair rather than the string_view overload: the iterator form is the
  // one nlohmann adapts without argument-type guesswork, and a header this file
  // is included from must not depend on which of those adapters a future
  // nlohmann keeps.
  const nlohmann::json j = nlohmann::json::parse(
      text.begin(), text.end(), /*cb=*/nullptr, /*allow_exceptions=*/false);
  if (j.is_discarded() || !j.is_object()) return std::nullopt;

  Blob b;
  b.client_pem = blob_detail::StringField(j, "client_pem");
  b.server_cert_pem = blob_detail::StringField(j, "server_cert_pem");
  b.host_port = blob_detail::StringField(j, "host_port");
  if (b.host_port.empty()) return std::nullopt;

  const std::string id = blob_detail::StringField(j, "instance_id");
  b.instance_id = IsPairableInstanceId(id) ? id : std::string{};
  return b;
}

}  // namespace urnw::rpcsession
