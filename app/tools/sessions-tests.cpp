// Executable spec for the Account > Sessions page (App/SessionsPresentation.h)
// and the relative time it shares with the rest of the app
// (App/RelativeTimeSpan.h), against server session/REVOKE-UI-FINAL.md §10:
// snapshot shapes (never loaded, loading, empty, current and other rows, a
// known and a nil LastUsed, Unix seconds, long and missing location, device
// type and app version), every device label and logo, every method label and
// the omitted legacy kinds, the country circle's colour and its empty-code
// fallback, the 8-character id, relative times across seconds, minutes,
// hours, days and the 7-day date cut-over, the pending, failed and bulk
// states and their words, the confirmations, the error states (sign-in
// required and the trusted remote sign-out among them) and the fence that
// drops an old controller's snapshots. And the app-wide half of §5
// (App/AuthLogoutNotice.h): the logout cause the sdk reports mapped to the
// sign-in page's notice, shown once, for one sign-out per rejection, never
// for the app's own sign-out. The WinUI half (App/SessionsPage.cpp, the
// sign-in page) cannot be built off Windows; this is every decision it makes
// before it touches a XAML object, run on the same headers the app compiles.
//
//   c++ -std=c++20 -I ../src/App sessions-tests.cpp -o /tmp/sessions-tests && /tmp/sessions-tests
//
// With URNW_SESSIONS_TESTS_SDK the snapshot reader also runs on the generated
// header's own classes (urnet::ClientSessionSnapshot and its family), over a
// fake of the C ABI functions they call: the real wrapper's handles, strings
// and lists are what SnapshotFrom reads, and every handle it takes is released
// once. So do the logout listeners' reads of the cause (App/AuthLogoutCause.h)
// and the wrapper's own getters, against the header's constant. The header
// needs nlohmann/json; both are system includes because the generated code
// does not build with -Wextra -Werror:
//
//   c++ -std=c++20 -Wall -Wextra -Werror -DURNW_SESSIONS_TESTS_SDK -I ../src/App -isystem <dir of urnetwork_sdk.hpp> -isystem <dir of nlohmann/> sessions-tests.cpp -o ...
//
// SPDX-License-Identifier: MPL-2.0

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "AuthLogoutNotice.h"
#include "RelativeTimeSpan.h"
#include "SessionsPresentation.h"

#if defined(URNW_SESSIONS_TESTS_SDK)
#include "urnetwork_sdk.hpp"
#include "AuthLogoutCause.h"
#endif

namespace al = urnw::authlogout;
namespace rt = urnw::relativetime;
namespace ss = urnw::sessions;

namespace {

int gFailures = 0;
int gCases = 0;

void Check(bool condition, const std::string& what) {
  ++gCases;
  if (!condition) {
    ++gFailures;
    std::cout << "  FAIL " << what << "\n";
  }
}

void CheckText(const std::string& want, const std::string& got, const std::string& what) {
  Check(want == got, what + ": want \"" + want + "\", got \"" + got + "\"");
}

void CheckEq(int64_t want, int64_t got, const std::string& what) {
  Check(want == got, what + ": want " + std::to_string(want) + ", got " + std::to_string(got));
}

// ---- the store's English ------------------------------------------------------
//
// The catalog's own English for every key the page reads, with its
// placeholders as the catalog lowers them ({} for one, {0} and {1} for two).
// tests/sessions_test.go checks each entry against Strings/en/Resources.resw,
// so the cases below read the words the app shows.
const std::map<std::string, std::string, std::less<>> kEnglish = {
    {"now", "now"},
    {"seconds_ago_abbrev", "{}s ago"},
    {"minutes_ago_abbrev", "{}m ago"},
    {"hours_ago_abbrev", "{}h ago"},
    {"days_ago_abbrev", "{}d ago"},
    {"sessions_device_android", "Android"},
    {"sessions_device_ios", "iOS"},
    {"sessions_device_macos", "macOS"},
    {"sessions_device_windows", "Windows"},
    {"sessions_device_linux", "Linux"},
    {"sessions_device_web", "Web"},
    {"sessions_device_cli", "Command line"},
    {"sessions_device_server", "Server"},
    {"sessions_device_unknown", "Unknown device"},
    {"sessions_kind_password", "Password"},
    {"sessions_kind_verify", "Verification code"},
    {"sessions_kind_apple", "Apple"},
    {"sessions_kind_google", "Google"},
    {"sessions_kind_sso", "Single sign-on"},
    {"sessions_kind_wallet", "Wallet"},
    {"sessions_kind_seedphrase", "Recovery phrase"},
    {"sessions_kind_signup", "New account"},
    {"sessions_kind_auth_code", "Auth code"},
    {"sessions_kind_device_adopt", "Device pairing"},
    {"sessions_kind_api_key_client", "API key"},
    {"sessions_last_used", "Last used {}"},
    {"sessions_last_use_unavailable", "Last use unavailable"},
    {"sessions_signed_in", "Signed in {}"},
    {"sessions_id", "ID {}"},
    {"sessions_sign_out_accessibility", "Sign out {}"},
    {"sessions_confirm_title", "Sign out this session?"},
    {"sessions_confirm_body", "{0} in {1} will be signed out."},
    {"sessions_confirm_body_no_place", "{} will be signed out."},
    {"sessions_confirm_self_body",
     "This is the session you're using. This app will be signed out."},
    {"sessions_confirm_others_title", "Sign out all other sessions?"},
    {"sessions_confirm_others_body",
     "Every other session in this list will be signed out. This session stays signed in. Anyone "
     "who knows your sign-in details can still sign in again."},
    {"sessions_action_failed", "Couldn't sign out this session. Try again."},
    {"sessions_sign_out_others_failed", "Couldn't sign out the other sessions. Try again."},
    {"sessions_sign_in_required", "Sign in again to manage sessions."},
    {"sessions_signed_out_remotely", "This session was signed out from another device."},
};

std::string English(std::string_view key) {
  const auto found = kEnglish.find(key);
  if (found == kEnglish.end()) {
    Check(false, "the spec has no English for " + std::string(key));
    return std::string(key);
  }
  return found->second;
}

std::string Replace(std::string text, std::string_view placeholder, const std::string& value) {
  const auto at = text.find(placeholder);
  if (at != std::string::npos) text.replace(at, placeholder.size(), value);
  return text;
}

// a fixed clock: 2026-10-09T12:00:00Z
constexpr int64_t kNowMillis = 1'791'547'200'000;
constexpr int64_t kSecond = 1000;
constexpr int64_t kMinute = 60 * kSecond;
constexpr int64_t kHour = 60 * kMinute;
constexpr int64_t kDay = 24 * kHour;

// what a platform date reads as here: the millis it was asked for, so a case
// can tell which time a line used
std::string FakeDate(int64_t millis) { return "<date " + std::to_string(millis) + ">"; }
std::string FakeDateTime(int64_t millis) { return "<full " + std::to_string(millis) + ">"; }

// urnw::RelativeTime's composition (StatsFormat.cpp) with the English above
std::string FakeRelative(int64_t millis) {
  const rt::Span span = rt::SpanFor(millis, kNowMillis);
  if (span.unit == rt::Unit::Date) return FakeDate(millis);
  if (span.unit == rt::Unit::Now) return English(rt::KeyFor(span.unit));
  return Replace(English(rt::KeyFor(span.unit)), "{}", std::to_string(span.count));
}

ss::Text EnglishText() {
  ss::Text text;
  text.localized = [](std::string_view key) { return English(key); };
  text.format = [](std::string_view key, const std::string& value) {
    return Replace(English(key), "{}", value);
  };
  text.format2 = [](std::string_view key, const std::string& first, const std::string& second) {
    return Replace(Replace(English(key), "{0}", first), "{1}", second);
  };
  text.relative = FakeRelative;
  text.date = FakeDate;
  text.dateTime = FakeDateTime;
  return text;
}

// ---- snapshots ------------------------------------------------------------------

constexpr const char* kCurrentId = "01a1f3c2-7d1e-4b8a-9f00-1c2d3e4f5a6b";
constexpr const char* kOtherId = "02b2e4d3-8e2f-4c9b-8e11-2d3e4f5a6b7c";
constexpr const char* kThirdId = "03c3f5e4-9f30-4dac-9d22-3e4f5a6b7c8d";

ss::LastUsed Used(std::string deviceType, std::string appVersion, int64_t agoMillis,
                  std::string city = "Chicago", std::string region = "Illinois",
                  std::string country = "United States", std::string countryCode = "us") {
  ss::LastUsed used;
  used.unixTime = (kNowMillis - agoMillis) / 1000;  // the server's seconds
  used.city = std::move(city);
  used.region = std::move(region);
  used.country = std::move(country);
  used.countryCode = std::move(countryCode);
  used.deviceType = std::move(deviceType);
  used.appVersion = std::move(appVersion);
  return used;
}

ss::Session MakeSession(std::string id, bool current, std::string kind, int64_t createMillis,
                        std::optional<ss::LastUsed> used) {
  ss::Session session;
  session.sessionId = std::move(id);
  session.current = current;
  session.kind = std::move(kind);
  session.createTimeMillis = createMillis;
  session.lastUsed = std::move(used);
  return session;
}

ss::Snapshot Loaded(std::vector<ss::Session> sessions) {
  ss::Snapshot snapshot;
  snapshot.loaded = true;
  snapshot.currentSessionId = kCurrentId;
  snapshot.legacyCoverage = "complete";
  snapshot.sessions = std::move(sessions);
  return snapshot;
}

ss::Error MakeError(bool retryable, bool signInRequired, bool unsupported,
                    bool sessionRevoked = false) {
  ss::Error error;
  error.message = "Session request could not be completed.";
  error.retryable = retryable;
  error.signInRequired = signInRequired;
  error.sessionRevoked = sessionRevoked;
  error.unsupported = unsupported;
  return error;
}

ss::Action MakeAction(std::string sessionId, bool loading, bool pending,
                      std::optional<ss::Error> error = std::nullopt) {
  ss::Action action;
  action.sessionId = std::move(sessionId);
  action.loading = loading;
  action.pending = pending;
  action.error = std::move(error);
  return action;
}

// ---- fakes of the generated wrapper's classes -----------------------------------
//
// The getters SnapshotFrom calls, by the generated names and return types. A
// nil object tests false, as a handle of 0 does.

struct FakeError {
  bool present = false;
  ss::Error value;
  explicit operator bool() const { return present; }
  std::string getMessage() const { return value.message; }
  bool getRetryable() const { return value.retryable; }
  bool getSignInRequired() const { return value.signInRequired; }
  bool getSessionRevoked() const { return value.sessionRevoked; }
  bool getUnsupported() const { return value.unsupported; }
};

struct FakeAction {
  bool present = false;
  std::string sessionId;
  bool loading = false;
  bool pending = false;
  FakeError error;
  explicit operator bool() const { return present; }
  std::string getSessionId() const { return sessionId; }
  bool getLoading() const { return loading; }
  bool getPending() const { return pending; }
  FakeError getError() const { return error; }
};

struct FakeLastUsed {
  bool present = false;
  ss::LastUsed value;
  explicit operator bool() const { return present; }
  int64_t getUnixTime() const { return value.unixTime; }
  std::string getCity() const { return value.city; }
  std::string getRegion() const { return value.region; }
  std::string getCountry() const { return value.country; }
  std::string getCountryCode() const { return value.countryCode; }
  std::string getDeviceType() const { return value.deviceType; }
  std::string getAppVersion() const { return value.appVersion; }
};

struct FakeSession {
  std::string sessionId;
  bool current = false;
  std::string kind;
  int64_t createTime = 0;
  FakeLastUsed lastUsed;
  std::string getSessionId() const { return sessionId; }
  bool getCurrent() const { return current; }
  std::string getKind() const { return kind; }
  int64_t getCreateTime() const { return createTime; }
  FakeLastUsed getLastUsed() const { return lastUsed; }
};

template <typename T>
struct FakeList {
  bool present = true;
  std::vector<T> values;
  explicit operator bool() const { return present; }
  int64_t len() const { return static_cast<int64_t>(values.size()); }
  T get(int64_t i) const { return values[static_cast<size_t>(i)]; }
};

struct FakeSnapshot {
  FakeList<FakeSession> sessions;
  std::string currentSessionId;
  std::string legacyCoverage;
  bool loaded = false;
  bool loading = false;
  bool refreshing = false;
  bool supported = true;
  FakeAction bulkAction;
  FakeList<FakeAction> actions;
  FakeError error;
  FakeList<FakeSession> getSessions() const { return sessions; }
  std::string getCurrentSessionId() const { return currentSessionId; }
  std::string getLegacyCoverage() const { return legacyCoverage; }
  bool getLoaded() const { return loaded; }
  bool getLoading() const { return loading; }
  bool getRefreshing() const { return refreshing; }
  bool getSupported() const { return supported; }
  FakeAction getBulkAction() const { return bulkAction; }
  FakeList<FakeAction> getActions() const { return actions; }
  FakeError getError() const { return error; }
};

#if defined(URNW_SESSIONS_TESTS_SDK)
// ---- a fake of the C ABI the generated classes call -------------------------------
//
// One snapshot's objects behind numbered handles, the way the sdk hands them
// out: each getter that returns an object hands out a NEW handle the caller
// must release, nil is 0, and strings are malloc'd for urnet_free_string.
namespace abi {

enum class Kind { Snapshot, SessionList, Session, LastUsed, ActionList, Action, Error, Api, Device };

struct Object {
  Kind kind;
  const void* value;
};

// An Api or a Device as its logout cause reads: what GetAuthLogoutCause
// returns, or nil.
struct FakeLogout {
  std::optional<std::string> cause;
};

const FakeSnapshot* gSnapshot = nullptr;
std::map<uint64_t, Object> gLive;
uint64_t gNext = 1000;
int64_t gIssued = 0;
int64_t gReleased = 0;
int64_t gUnknownReleases = 0;
// strings handed out, and freed through urnet_free_string
int64_t gStringsIssued = 0;
int64_t gStringsFreed = 0;

uint64_t Issue(Kind kind, const void* value) {
  const uint64_t handle = gNext++;
  gLive[handle] = Object{kind, value};
  ++gIssued;
  return handle;
}

template <typename T>
const T* Get(uint64_t handle, Kind kind) {
  const auto found = gLive.find(handle);
  if (found == gLive.end() || found->second.kind != kind) return nullptr;
  return static_cast<const T*>(found->second.value);
}

char* Copy(const std::string& value) {
  char* out = static_cast<char*>(std::malloc(value.size() + 1));
  std::memcpy(out, value.c_str(), value.size() + 1);
  ++gStringsIssued;
  return out;
}

}  // namespace abi
#endif

}  // namespace

#if defined(URNW_SESSIONS_TESTS_SDK)
extern "C" {

void urnet_free_string(char* s) {
  if (s != nullptr) ++abi::gStringsFreed;
  std::free(s);
}

// an AuthLogout's cause, on the Api and on a Device
char* urnet_api_get_auth_logout_cause(uint64_t self) {
  const auto* l = abi::Get<abi::FakeLogout>(self, abi::Kind::Api);
  return l && l->cause ? abi::Copy(*l->cause) : nullptr;
}
char* urnet_device_get_auth_logout_cause(uint64_t self) {
  const auto* l = abi::Get<abi::FakeLogout>(self, abi::Kind::Device);
  return l && l->cause ? abi::Copy(*l->cause) : nullptr;
}

bool urnet_release(uint64_t handle) {
  if (abi::gLive.erase(handle) == 0) {
    ++abi::gUnknownReleases;
    return false;
  }
  ++abi::gReleased;
  return true;
}

// the snapshot
uint64_t urnet_client_session_snapshot_get_sessions(uint64_t self) {
  const auto* s = abi::Get<FakeSnapshot>(self, abi::Kind::Snapshot);
  return s && s->sessions.present ? abi::Issue(abi::Kind::SessionList, &s->sessions) : 0;
}
char* urnet_client_session_snapshot_get_current_session_id(uint64_t self) {
  const auto* s = abi::Get<FakeSnapshot>(self, abi::Kind::Snapshot);
  return s ? abi::Copy(s->currentSessionId) : nullptr;
}
char* urnet_client_session_snapshot_get_legacy_coverage(uint64_t self) {
  const auto* s = abi::Get<FakeSnapshot>(self, abi::Kind::Snapshot);
  return s ? abi::Copy(s->legacyCoverage) : nullptr;
}
bool urnet_client_session_snapshot_get_loaded(uint64_t self) {
  const auto* s = abi::Get<FakeSnapshot>(self, abi::Kind::Snapshot);
  return s && s->loaded;
}
bool urnet_client_session_snapshot_get_loading(uint64_t self) {
  const auto* s = abi::Get<FakeSnapshot>(self, abi::Kind::Snapshot);
  return s && s->loading;
}
bool urnet_client_session_snapshot_get_refreshing(uint64_t self) {
  const auto* s = abi::Get<FakeSnapshot>(self, abi::Kind::Snapshot);
  return s && s->refreshing;
}
bool urnet_client_session_snapshot_get_supported(uint64_t self) {
  const auto* s = abi::Get<FakeSnapshot>(self, abi::Kind::Snapshot);
  return s && s->supported;
}
uint64_t urnet_client_session_snapshot_get_bulk_action(uint64_t self) {
  const auto* s = abi::Get<FakeSnapshot>(self, abi::Kind::Snapshot);
  return s && s->bulkAction.present ? abi::Issue(abi::Kind::Action, &s->bulkAction) : 0;
}
uint64_t urnet_client_session_snapshot_get_actions(uint64_t self) {
  const auto* s = abi::Get<FakeSnapshot>(self, abi::Kind::Snapshot);
  return s && s->actions.present ? abi::Issue(abi::Kind::ActionList, &s->actions) : 0;
}
uint64_t urnet_client_session_snapshot_get_error(uint64_t self) {
  const auto* s = abi::Get<FakeSnapshot>(self, abi::Kind::Snapshot);
  return s && s->error.present ? abi::Issue(abi::Kind::Error, &s->error) : 0;
}

// the session list and a session
int64_t urnet_network_session_info_list_len(uint64_t self) {
  const auto* l = abi::Get<FakeList<FakeSession>>(self, abi::Kind::SessionList);
  return l ? l->len() : 0;
}
uint64_t urnet_network_session_info_list_get(uint64_t self, int64_t i) {
  const auto* l = abi::Get<FakeList<FakeSession>>(self, abi::Kind::SessionList);
  return l ? abi::Issue(abi::Kind::Session, &l->values[static_cast<size_t>(i)]) : 0;
}
char* urnet_network_session_info_get_session_id(uint64_t self) {
  const auto* s = abi::Get<FakeSession>(self, abi::Kind::Session);
  return s ? abi::Copy(s->sessionId) : nullptr;
}
bool urnet_network_session_info_get_current(uint64_t self) {
  const auto* s = abi::Get<FakeSession>(self, abi::Kind::Session);
  return s && s->current;
}
char* urnet_network_session_info_get_kind(uint64_t self) {
  const auto* s = abi::Get<FakeSession>(self, abi::Kind::Session);
  return s ? abi::Copy(s->kind) : nullptr;
}
int64_t urnet_network_session_info_get_create_time(uint64_t self) {
  const auto* s = abi::Get<FakeSession>(self, abi::Kind::Session);
  return s ? s->createTime : 0;
}
uint64_t urnet_network_session_info_get_last_used(uint64_t self) {
  const auto* s = abi::Get<FakeSession>(self, abi::Kind::Session);
  return s && s->lastUsed.present ? abi::Issue(abi::Kind::LastUsed, &s->lastUsed) : 0;
}

// last used
int64_t urnet_session_last_used_get_unix_time(uint64_t self) {
  const auto* u = abi::Get<FakeLastUsed>(self, abi::Kind::LastUsed);
  return u ? u->value.unixTime : 0;
}
char* urnet_session_last_used_get_city(uint64_t self) {
  const auto* u = abi::Get<FakeLastUsed>(self, abi::Kind::LastUsed);
  return u ? abi::Copy(u->value.city) : nullptr;
}
char* urnet_session_last_used_get_region(uint64_t self) {
  const auto* u = abi::Get<FakeLastUsed>(self, abi::Kind::LastUsed);
  return u ? abi::Copy(u->value.region) : nullptr;
}
char* urnet_session_last_used_get_country(uint64_t self) {
  const auto* u = abi::Get<FakeLastUsed>(self, abi::Kind::LastUsed);
  return u ? abi::Copy(u->value.country) : nullptr;
}
char* urnet_session_last_used_get_country_code(uint64_t self) {
  const auto* u = abi::Get<FakeLastUsed>(self, abi::Kind::LastUsed);
  return u ? abi::Copy(u->value.countryCode) : nullptr;
}
char* urnet_session_last_used_get_device_type(uint64_t self) {
  const auto* u = abi::Get<FakeLastUsed>(self, abi::Kind::LastUsed);
  return u ? abi::Copy(u->value.deviceType) : nullptr;
}
char* urnet_session_last_used_get_app_version(uint64_t self) {
  const auto* u = abi::Get<FakeLastUsed>(self, abi::Kind::LastUsed);
  return u ? abi::Copy(u->value.appVersion) : nullptr;
}

// the action list, an action and an error
int64_t urnet_client_session_action_list_len(uint64_t self) {
  const auto* l = abi::Get<FakeList<FakeAction>>(self, abi::Kind::ActionList);
  return l ? l->len() : 0;
}
uint64_t urnet_client_session_action_list_get(uint64_t self, int64_t i) {
  const auto* l = abi::Get<FakeList<FakeAction>>(self, abi::Kind::ActionList);
  return l ? abi::Issue(abi::Kind::Action, &l->values[static_cast<size_t>(i)]) : 0;
}
char* urnet_client_session_action_get_session_id(uint64_t self) {
  const auto* a = abi::Get<FakeAction>(self, abi::Kind::Action);
  return a ? abi::Copy(a->sessionId) : nullptr;
}
bool urnet_client_session_action_get_loading(uint64_t self) {
  const auto* a = abi::Get<FakeAction>(self, abi::Kind::Action);
  return a && a->loading;
}
bool urnet_client_session_action_get_pending(uint64_t self) {
  const auto* a = abi::Get<FakeAction>(self, abi::Kind::Action);
  return a && a->pending;
}
uint64_t urnet_client_session_action_get_error(uint64_t self) {
  const auto* a = abi::Get<FakeAction>(self, abi::Kind::Action);
  return a && a->error.present ? abi::Issue(abi::Kind::Error, &a->error) : 0;
}
char* urnet_client_session_error_get_message(uint64_t self) {
  const auto* e = abi::Get<FakeError>(self, abi::Kind::Error);
  return e ? abi::Copy(e->value.message) : nullptr;
}
bool urnet_client_session_error_get_retryable(uint64_t self) {
  const auto* e = abi::Get<FakeError>(self, abi::Kind::Error);
  return e && e->value.retryable;
}
bool urnet_client_session_error_get_sign_in_required(uint64_t self) {
  const auto* e = abi::Get<FakeError>(self, abi::Kind::Error);
  return e && e->value.signInRequired;
}
bool urnet_client_session_error_get_session_revoked(uint64_t self) {
  const auto* e = abi::Get<FakeError>(self, abi::Kind::Error);
  return e && e->value.sessionRevoked;
}
bool urnet_client_session_error_get_unsupported(uint64_t self) {
  const auto* e = abi::Get<FakeError>(self, abi::Kind::Error);
  return e && e->value.unsupported;
}

}  // extern "C"
#endif

namespace {

// A FakeSnapshot holding `snapshot`, field for field.
FakeSnapshot FakeOf(const ss::Snapshot& snapshot) {
  FakeSnapshot fake;
  for (const auto& session : snapshot.sessions) {
    FakeSession s;
    s.sessionId = session.sessionId;
    s.current = session.current;
    s.kind = session.kind;
    s.createTime = session.createTimeMillis;
    if (session.lastUsed) {
      s.lastUsed.present = true;
      s.lastUsed.value = *session.lastUsed;
    }
    fake.sessions.values.push_back(s);
  }
  const auto fakeError = [](const std::optional<ss::Error>& error) {
    FakeError e;
    if (error) {
      e.present = true;
      e.value = *error;
    }
    return e;
  };
  const auto fakeAction = [&](const ss::Action& action) {
    FakeAction a;
    a.present = true;
    a.sessionId = action.sessionId;
    a.loading = action.loading;
    a.pending = action.pending;
    a.error = fakeError(action.error);
    return a;
  };
  fake.currentSessionId = snapshot.currentSessionId;
  fake.legacyCoverage = snapshot.legacyCoverage;
  fake.loaded = snapshot.loaded;
  fake.loading = snapshot.loading;
  fake.refreshing = snapshot.refreshing;
  fake.supported = snapshot.supported;
  if (snapshot.bulkAction) fake.bulkAction = fakeAction(*snapshot.bulkAction);
  for (const auto& action : snapshot.actions) fake.actions.values.push_back(fakeAction(action));
  fake.error = fakeError(snapshot.error);
  return fake;
}

// Every field of a snapshot the readers can carry, nil and set.
ss::Snapshot RichSnapshot() {
  ss::Snapshot snapshot =
      Loaded({MakeSession(kCurrentId, true, "google", kNowMillis - 6 * kDay,
                          Used("windows", "2026.10.9-1", 5 * kMinute)),
              MakeSession(kOtherId, false, "legacy", 0, std::nullopt),
              MakeSession(kThirdId, false, "password", kNowMillis - 40 * kDay,
                          Used("android", "", 3 * kHour, "", "", "Germany", "de"))});
  snapshot.legacyCoverage = "partial";
  snapshot.refreshing = true;
  snapshot.bulkAction = MakeAction("", false, true);
  snapshot.actions = {MakeAction(kOtherId, true, false),
                      MakeAction(kThirdId, false, false, MakeError(true, false, false))};
  snapshot.error = MakeError(false, true, false, /*sessionRevoked=*/true);
  return snapshot;
}

void TestRelativeTime() {
  // the buckets, at every boundary
  const struct {
    int64_t agoMillis;
    rt::Span want;
    const char* what;
  } cases[] = {
      {0, {rt::Unit::Now, 0}, "the same instant is now"},
      {4 * kSecond + 999, {rt::Unit::Now, 0}, "4.999s is still now"},
      {5 * kSecond, {rt::Unit::Seconds, 5}, "5s is the first second count"},
      {59 * kSecond + 999, {rt::Unit::Seconds, 59}, "59.999s is 59s"},
      {kMinute, {rt::Unit::Minutes, 1}, "60s is 1m"},
      {59 * kMinute + 59 * kSecond, {rt::Unit::Minutes, 59}, "59m59s is 59m"},
      {kHour, {rt::Unit::Hours, 1}, "60m is 1h"},
      {23 * kHour + 59 * kMinute, {rt::Unit::Hours, 23}, "23h59m is 23h"},
      {kDay, {rt::Unit::Days, 1}, "24h is 1d"},
      {6 * kDay + 23 * kHour, {rt::Unit::Days, 6}, "6d23h is 6d"},
      {7 * kDay - kSecond, {rt::Unit::Days, 6}, "a second short of a week is still 6d"},
      {7 * kDay, {rt::Unit::Date, 0}, "a week is a date"},
      {400 * kDay, {rt::Unit::Date, 0}, "a year back is a date"},
      {-3 * kMinute, {rt::Unit::Now, 0}, "a time ahead of the clock (skew) is now"},
  };
  for (const auto& c : cases) {
    const rt::Span got = rt::SpanFor(kNowMillis - c.agoMillis, kNowMillis);
    Check(got == c.want, std::string(c.what) + ": got unit " +
                             std::to_string(static_cast<int>(got.unit)) + " count " +
                             std::to_string(got.count));
  }
  CheckText("now", rt::KeyFor(rt::Unit::Now), "now's key");
  CheckText("seconds_ago_abbrev", rt::KeyFor(rt::Unit::Seconds), "seconds' key");
  CheckText("minutes_ago_abbrev", rt::KeyFor(rt::Unit::Minutes), "minutes' key");
  CheckText("hours_ago_abbrev", rt::KeyFor(rt::Unit::Hours), "hours' key");
  CheckText("days_ago_abbrev", rt::KeyFor(rt::Unit::Days), "days' key");
  CheckText("", rt::KeyFor(rt::Unit::Date), "a date has no key: the platform formats it");

  // the words, through RelativeTime's composition
  CheckText("now", FakeRelative(kNowMillis - 2 * kSecond), "2s ago reads now");
  CheckText("12s ago", FakeRelative(kNowMillis - 12 * kSecond), "seconds");
  CheckText("3m ago", FakeRelative(kNowMillis - 3 * kMinute - 10 * kSecond), "minutes");
  CheckText("2h ago", FakeRelative(kNowMillis - 2 * kHour), "hours");
  CheckText("6d ago", FakeRelative(kNowMillis - 6 * kDay - kHour), "days, up to a week");
  CheckText(FakeDate(kNowMillis - 7 * kDay), FakeRelative(kNowMillis - 7 * kDay),
            "from a week on, the localized date of that time");
}

void TestLabels() {
  const struct {
    const char* deviceType;
    const char* label;
    ss::DeviceGlyph glyph;
    const wchar_t* path;
  } devices[] = {
      {"android", "Android", ss::DeviceGlyph::Android, urnw::glyph::kDeviceAndroidPath},
      {"ios", "iOS", ss::DeviceGlyph::Apple, urnw::glyph::kDeviceApplePath},
      {"macos", "macOS", ss::DeviceGlyph::Apple, urnw::glyph::kDeviceApplePath},
      {"windows", "Windows", ss::DeviceGlyph::Windows, urnw::glyph::kDeviceWindowsPath},
      {"linux", "Linux", ss::DeviceGlyph::Linux, urnw::glyph::kDeviceLinuxPath},
      {"web", "Web", ss::DeviceGlyph::Web, urnw::glyph::kDeviceWebPath},
      {"cli", "Command line", ss::DeviceGlyph::Cli, urnw::glyph::kDeviceCliPath},
      {"server", "Server", ss::DeviceGlyph::Server, urnw::glyph::kDeviceServerPath},
      {"unknown", "Unknown device", ss::DeviceGlyph::Unknown, urnw::glyph::kDeviceUnknownPath},
      {"", "Unknown device", ss::DeviceGlyph::Unknown, urnw::glyph::kDeviceUnknownPath},
      {"toaster", "Unknown device", ss::DeviceGlyph::Unknown, urnw::glyph::kDeviceUnknownPath},
      {"Windows", "Unknown device", ss::DeviceGlyph::Unknown, urnw::glyph::kDeviceUnknownPath},
  };
  for (const auto& d : devices) {
    const std::string what = std::string("device type \"") + d.deviceType + "\"";
    CheckText(d.label, English(ss::DeviceLabelKey(d.deviceType)), what + ": label");
    Check(ss::DeviceGlyphFor(d.deviceType) == d.glyph, what + ": logo");
    Check(ss::GlyphPath(ss::DeviceGlyphFor(d.deviceType)) == d.path, what + ": logo path");
  }
  // the account row's glyph is its own, not a device's
  Check(std::wstring_view(urnw::glyph::kSessionFaceProfilePath).starts_with(L"M13 1C8.4 1"),
        "the Sessions row's head-outline path");

  const struct {
    const char* kind;
    const char* label;  // "" is omitted
  } kinds[] = {
      {"password", "Password"},
      {"verify", "Verification code"},
      {"apple", "Apple"},
      {"google", "Google"},
      {"sso", "Single sign-on"},
      {"wallet", "Wallet"},
      {"seedphrase", "Recovery phrase"},
      {"signup", "New account"},
      {"auth_code", "Auth code"},
      {"device_adopt", "Device pairing"},
      {"api_key_client", "API key"},
      {"legacy", ""},
      {"legacy_proxy", ""},
      {"", ""},
      {"magic_link", ""},
  };
  for (const auto& k : kinds) {
    const char* key = ss::KindLabelKey(k.kind);
    CheckText(k.label, *key ? English(key) : std::string(),
              std::string("kind \"") + k.kind + "\": method label");
  }

  CheckText("01a1f3c2", ss::ShortId(kCurrentId), "the short id is the first 8 characters");
  CheckText("abc", ss::ShortId("abc"), "a shorter id is shown whole");
  CheckText("", ss::ShortId(""), "no id, nothing");

  CheckText("Chicago, Illinois, United States", ss::PlaceText(Used("web", "", 0)), "the place");
  CheckText("Illinois, United States", ss::PlaceText(Used("web", "", 0, "", "Illinois")),
            "a missing city is left out");
  CheckText("Germany", ss::PlaceText(Used("web", "", 0, "", "", "Germany", "de")), "country only");
  CheckText("", ss::PlaceText(Used("web", "", 0, "", "", "", "")), "no location at all");
}

void TestCircleColor() {
  // the sdk colours any string, the empty one too, which is why an empty code
  // must not reach it
  std::vector<std::string> asked;
  const auto colorHex = [&asked](const std::string& code) {
    asked.push_back(code);
    if (code == "us") return std::string("3cdd67");
    if (code == "de") return std::string("#DD4F3C");
    if (code.empty()) return std::string("0a0b0c");
    return std::string("not a colour");
  };
  Check(ss::CircleColorFor("us", colorHex) == ss::Rgb{0x3c, 0xdd, 0x67}, "the sdk's colour");
  Check(ss::CircleColorFor("de", colorHex) == ss::Rgb{0xdd, 0x4f, 0x3c},
        "a leading # and upper case parse");
  Check(ss::CircleColorFor("zz", colorHex) == ss::kUnknownCountryRgb,
        "a colour that does not parse is the unknown-country colour");
  const size_t before = asked.size();
  Check(ss::CircleColorFor("", colorHex) == ss::kUnknownCountryRgb,
        "an empty code is the unknown-country colour");
  CheckEq(static_cast<int64_t>(before), static_cast<int64_t>(asked.size()),
          "an empty code does not ask the sdk");
  Check(ss::kUnknownCountryRgb == ss::Rgb{0x00, 0x99, 0xFF}, "the globe's neutral blue");
}

void TestRows() {
  const ss::Text text = EnglishText();
  const int64_t created = kNowMillis - 6 * kDay;
  const ss::Snapshot snapshot = Loaded(
      {MakeSession(kCurrentId, true, "google", created,
                   Used("windows", "2026.10.9-101", 5 * kMinute + 7 * kSecond)),
       MakeSession(kOtherId, false, "legacy", 0, std::nullopt),
       MakeSession(kThirdId, false, "password", kNowMillis - 40 * kDay,
                   Used("android", "", 9 * kDay, "", "", "Germany", "de"))});
  const ss::View view = ss::ViewFor(snapshot, text);
  Check(view.body == ss::Body::Rows, "a loaded list shows its rows");
  if (view.rows.size() != 3) {
    Check(false, "three sessions, three rows");
    return;
  }

  // the current session, everything known
  const ss::Row& current = view.rows[0];
  CheckText(kCurrentId, current.sessionId, "the row keeps the whole id for the copy");
  Check(current.current, "the current session is tagged This session");
  Check(current.glyph == ss::DeviceGlyph::Windows, "the windows logo");
  CheckText("us", current.countryCode, "the circle's country");
  CheckText("Windows", current.device, "the device label");
  CheckText("Windows \xC2\xB7 2026.10.9-101", current.line1, "line 1: device and version");
  CheckText("Windows, 2026.10.9-101", current.line1Spoken,
            "line 1 is read with a pause, not a dot");
  CheckText("Chicago, Illinois, United States \xC2\xB7 Last used 5m ago", current.line2,
            "line 2: place and the relative last use");
  const int64_t usedMillis = (kNowMillis - 5 * kMinute - 7 * kSecond) / 1000 * 1000;
  CheckText("Chicago, Illinois, United States, Last used " + FakeDateTime(usedMillis),
            current.line2Spoken, "line 2 is read with the full date and time, in milliseconds");
  CheckText("Signed in " + FakeDate(created) + " \xC2\xB7 Google \xC2\xB7 ID 01a1f3c2",
            current.line3, "line 3: signed in, method, short id");
  CheckText("Signed in " + FakeDateTime(created) + ", Google, ID 01a1f3c2", current.line3Spoken,
            "line 3 is read with the full date and time");
  CheckText("01a1f3c2", current.shortId, "the short id");
  CheckText("ID 01a1f3c2", current.idText, "what the copy button's id is");
  CheckText("Sign out Windows", current.signOutName, "the sign-out action's name");
  Check(current.action == ss::ActionState::Idle, "no action: Sign out is offered");

  // no last use known: unknown device, no version, no place, legacy method
  const ss::Row& legacy = view.rows[1];
  Check(!legacy.current, "another session is not tagged");
  Check(legacy.glyph == ss::DeviceGlyph::Unknown, "a nil LastUsed draws the unknown logo");
  CheckText("", legacy.countryCode, "a nil LastUsed has no country: the unknown colour");
  CheckText("Unknown device", legacy.line1, "line 1 without a version");
  CheckText("Unknown device", legacy.line1Spoken, "and read without one");
  CheckText("Last use unavailable", legacy.line2, "line 2 says the last use is unknown");
  CheckText("Last use unavailable", legacy.line2Spoken, "and is read the same");
  CheckText("ID 02b2e4d3", legacy.line3, "a legacy kind omits its method; no create time");
  CheckText("", legacy.place, "no place");

  // last used more than a week ago, no app version, country only
  const ss::Row& old = view.rows[2];
  CheckText("Android", old.line1, "an empty app version is left out");
  CheckText("Germany \xC2\xB7 Last used " + FakeDate((kNowMillis - 9 * kDay) / 1000 * 1000),
            old.line2, "a last use over a week ago is a date");
  CheckText("Germany", old.place, "the place is the country alone");
  CheckText("Signed in " + FakeDate(kNowMillis - 40 * kDay) +
                " \xC2\xB7 Password \xC2\xB7 ID 03c3f5e4",
            old.line3, "password method");

  // the current session found by id when the flag is not set
  ss::Snapshot byId = snapshot;
  byId.sessions[0].current = false;
  Check(ss::ViewFor(byId, text).rows[0].current, "the snapshot's current id marks the row");

  // long strings are carried whole: the page trims, the screen reader reads all
  const std::string longCity(120, 'x');
  const std::string longVersion = "2026.10.9-" + std::string(80, '9');
  const ss::Row longRow = ss::RowFor(
      MakeSession(kOtherId, false, "sso", created, Used("linux", longVersion, kHour, longCity)),
      snapshot, text);
  CheckText("Linux \xC2\xB7 " + longVersion, longRow.line1, "a long version is kept whole");
  Check(longRow.line2.starts_with(longCity + ", Illinois"), "a long city is kept whole");
  CheckText("Single sign-on", longRow.line3.substr(longRow.line3.find(" \xC2\xB7 ") + 4, 14),
            "the sso method label");

  // a last use the server stamped 0 is not 1970
  ss::LastUsed zero = Used("web", "1.0", 0);
  zero.unixTime = 0;
  const ss::Row unstamped =
      ss::RowFor(MakeSession(kOtherId, false, "", created, zero), snapshot, text);
  CheckText("Chicago, Illinois, United States \xC2\xB7 Last use unavailable", unstamped.line2,
            "no time: the place, and the last use unavailable");
}

void TestStates() {
  const ss::Text text = EnglishText();
  const auto body = [&](const ss::Snapshot& snapshot) { return ss::ViewFor(snapshot, text).body; };

  ss::Snapshot never;  // before the first snapshot lands
  Check(body(never) == ss::Body::Progress, "never loaded: progress");
  ss::Snapshot loading;
  loading.loading = true;
  Check(body(loading) == ss::Body::Progress, "the first load running: progress");

  ss::Snapshot failed;
  failed.error = MakeError(true, false, false);
  Check(body(failed) == ss::Body::LoadFailed, "the first load failed: Couldn't load + Try again");
  ss::Snapshot nonRetryable;
  nonRetryable.error = MakeError(false, false, false);
  Check(body(nonRetryable) == ss::Body::LoadFailed, "a non-retryable first failure too");
  ss::Snapshot retrying = failed;
  retrying.loading = true;
  Check(body(retrying) == ss::Body::Progress, "Try again shows progress while it runs");

  ss::Snapshot unsupported;
  unsupported.supported = false;
  unsupported.error = MakeError(false, false, true);
  Check(body(unsupported) == ss::Body::Unsupported, "no sessions route: unsupported");
  ss::Snapshot unsupportedFlagOnly;
  unsupportedFlagOnly.supported = false;
  Check(body(unsupportedFlagOnly) == ss::Body::Unsupported, "Supported == false alone");

  // sign-in required (§5): the screen's own words, which name no cause, and
  // the remote sign-out's only with the controller's trusted cause
  ss::Snapshot signIn = Loaded({MakeSession(kCurrentId, true, "google", 1, std::nullopt)});
  signIn.error = MakeError(false, true, false);
  const ss::View signInView = ss::ViewFor(signIn, text);
  Check(signInView.body == ss::Body::SignInRequired, "sign-in required wins over a loaded list");
  Check(!signInView.signedOutRemotely, "a rejection without the trusted cause names none");
  CheckText("Sign in again to manage sessions.",
            English(ss::SignInRequiredKey(signInView.signedOutRemotely)),
            "sign-in required shows the sessions screen's sign-in words");
  ss::Snapshot signInFirst;
  signInFirst.error = MakeError(false, true, false);
  Check(body(signInFirst) == ss::Body::SignInRequired,
        "sign-in required before anything loaded is not a failed load");
  ss::Snapshot revoked = signIn;
  revoked.error = MakeError(false, true, false, /*sessionRevoked=*/true);
  const ss::View revokedView = ss::ViewFor(revoked, text);
  Check(revokedView.body == ss::Body::SignInRequired && revokedView.signedOutRemotely,
        "the controller's trusted cause: signed out from another device");
  CheckText("This session was signed out from another device.",
            English(ss::SignInRequiredKey(revokedView.signedOutRemotely)),
            "the remote sign-out's words, only with the trusted cause");
  ss::Snapshot causeAlone = signIn;
  causeAlone.error = MakeError(true, false, false, /*sessionRevoked=*/true);
  const ss::View causeAloneView = ss::ViewFor(causeAlone, text);
  Check(causeAloneView.body == ss::Body::Rows && !causeAloneView.signedOutRemotely,
        "the cause without sign-in required changes nothing");

  ss::Snapshot empty = Loaded({});
  const ss::View emptyView = ss::ViewFor(empty, text);
  Check(emptyView.body == ss::Body::Empty, "loaded and empty: No active sessions");
  Check(!emptyView.bulkShown, "no rows, no bulk sign-out");
  Check(!emptyView.lastUsedHelp, "no rows, no Last used note");
  Check(!emptyView.legacyNote, "complete coverage: no legacy note");
  empty.legacyCoverage = "partial";
  Check(ss::ViewFor(empty, text).legacyNote, "partial coverage: the legacy note, even when empty");

  ss::Snapshot refreshing = Loaded({MakeSession(kCurrentId, true, "google", 1, std::nullopt)});
  refreshing.refreshing = true;
  const ss::View refreshingView = ss::ViewFor(refreshing, text);
  Check(refreshingView.body == ss::Body::Rows && refreshingView.refreshing,
        "a refresh keeps the rows and shows the indicator");
  Check(refreshingView.lastUsedHelp, "rows carry the Last used note");
  Check(!refreshingView.refreshFailed, "no error, no notice");

  ss::Snapshot refreshFailed = refreshing;
  refreshFailed.refreshing = false;
  refreshFailed.error = MakeError(true, false, false);
  const ss::View refreshFailedView = ss::ViewFor(refreshFailed, text);
  Check(refreshFailedView.body == ss::Body::Rows && refreshFailedView.refreshFailed,
        "a failed refresh keeps the rows with the notice");
  CheckEq(1, static_cast<int64_t>(refreshFailedView.rows.size()), "the last list stays");
}

void TestActions() {
  const ss::Text text = EnglishText();
  const auto current = MakeSession(kCurrentId, true, "google", 1, Used("windows", "1", kMinute));
  const auto other = MakeSession(kOtherId, false, "password", 1, Used("android", "2", kHour));
  const auto third = MakeSession(kThirdId, false, "", 1, std::nullopt);

  // the bulk button: a current session and at least one other
  Check(!ss::ViewFor(Loaded({current}), text).bulkShown, "the current session alone: no bulk");
  ss::Snapshot othersOnly = Loaded({other, third});
  othersOnly.currentSessionId.clear();
  Check(!ss::ViewFor(othersOnly, text).bulkShown, "no current session: no bulk");
  Check(ss::ViewFor(Loaded({current, other}), text).bulkShown, "current and another: bulk");

  // row states: loading and 202 pending show progress until the snapshot drops
  // the row; an error offers Sign out again; the other rows are untouched
  ss::Snapshot acting = Loaded({current, other, third});
  acting.actions = {MakeAction(kOtherId, true, false),
                    MakeAction(kThirdId, false, false, MakeError(true, false, false))};
  ss::View view = ss::ViewFor(acting, text);
  Check(view.rows[0].action == ss::ActionState::Idle, "a row with no action is idle");
  Check(view.rows[1].action == ss::ActionState::Pending, "a request out: Signing out…");
  Check(view.rows[2].action == ss::ActionState::Failed, "an error: Couldn't sign out");
  acting.actions[0] = MakeAction(kOtherId, false, true);
  Check(ss::ViewFor(acting, text).rows[1].action == ss::ActionState::Pending,
        "a 202 stays pending until enforcement is confirmed");
  acting.actions[0] = MakeAction(kOtherId, false, true, MakeError(true, false, false));
  Check(ss::ViewFor(acting, text).rows[1].action == ss::ActionState::Pending,
        "a retryable status error while pending is still pending");
  // the controller drops a confirmed row from its next snapshot
  acting.sessions = {current, third};
  acting.actions = {MakeAction(kOtherId, false, false)};
  view = ss::ViewFor(acting, text);
  CheckEq(2, static_cast<int64_t>(view.rows.size()), "the confirmed row is gone");

  // the bulk button's states
  ss::Snapshot bulk = Loaded({current, other});
  bulk.bulkAction = MakeAction("", true, false);
  Check(ss::ViewFor(bulk, text).bulk == ss::ActionState::Pending, "bulk loading");
  bulk.bulkAction = MakeAction("", false, true);
  Check(ss::ViewFor(bulk, text).bulk == ss::ActionState::Pending, "bulk pending");
  bulk.bulkAction = MakeAction("", false, false, MakeError(false, false, false));
  const ss::View bulkFailed = ss::ViewFor(bulk, text);
  Check(bulkFailed.bulk == ss::ActionState::Failed, "bulk failed");
  Check(bulkFailed.bulkShown, "a failed bulk sign-out keeps its button");
  // under the button: the bulk failure's own words, not a row's or a generic
  // error; the button is not pending, so it is enabled for a retry
  CheckText("Couldn't sign out the other sessions. Try again.",
            English(ss::ActionFailedKey(/*bulk=*/true)),
            "a failed bulk sign-out says so under its button");
  CheckText("Couldn't sign out this session. Try again.",
            English(ss::ActionFailedKey(/*bulk=*/false)), "a row's failure keeps its words");
  bulk.bulkAction = MakeAction("", false, false, MakeError(true, false, false));
  Check(ss::ViewFor(bulk, text).bulk == ss::ActionState::Failed,
        "a failed bulk sign-out the controller no longer retries is offered again, whatever its "
        "error's flags");
  bulk.bulkAction = MakeAction("", false, false);
  Check(ss::ViewFor(bulk, text).bulk == ss::ActionState::Idle, "bulk done");
  Check(ss::ActionStateFor(std::nullopt) == ss::ActionState::Idle, "no bulk action yet");

  // the confirmations
  const ss::View confirmView = ss::ViewFor(Loaded({current, other, third}), text);
  const ss::Confirmation self = ss::ConfirmationFor(confirmView.rows[0], text);
  CheckText("Sign out this session?", self.title, "the confirmation title");
  CheckText("This is the session you're using. This app will be signed out.", self.body,
            "the current session warns this app signs out");
  CheckText("Android in Chicago, Illinois, United States will be signed out.",
            ss::ConfirmationFor(confirmView.rows[1], text).body,
            "another session is named with its place");
  CheckText("Unknown device will be signed out.",
            ss::ConfirmationFor(confirmView.rows[2], text).body, "no place, the device alone");
  const ss::Confirmation others = ss::BulkConfirmationFor(text);
  CheckText("Sign out all other sessions?", others.title, "the bulk title");
  Check(others.body.starts_with("Every other session in this list will be signed out.") &&
            others.body.find("all devices") == std::string::npos,
        "the bulk body, which never claims every device is signed out");
}

void TestFence() {
  // a snapshot read under one open applies only while that open is current
  ss::OpenFence fence;
  Check(!fence.Admits(0), "nothing open admits nothing");
  CheckEq(0, static_cast<int64_t>(fence.Current()), "nothing open is 0");
  const uint64_t first = fence.Open();
  Check(fence.Admits(first), "the open controller's snapshots apply");
  Check(fence.Current() == first, "a confirmation opened now is for this controller");
  fence.Close();
  Check(!fence.Admits(first), "a closed controller's queued snapshot is dropped");
  Check(!fence.Admits(fence.Current()), "a confirmation from a closed page signs nothing out");
  const uint64_t second = fence.Open();
  Check(second != first, "every open is a new number");
  Check(!fence.Admits(first) && fence.Admits(second),
        "after switching accounts the older controller's response cannot repaint the page");
  const uint64_t third = fence.Open();
  Check(!fence.Admits(second) && fence.Admits(third), "reopening without a close fences too");
}

// The app-wide sign-out a rejection takes, and the sign-in page's notice for
// it (AuthLogoutNotice.h), as AppController drives them: each report of a
// rejection arrives on the UI thread with the cause its listener read, and a
// report that signs the app out runs SdkHost::Logout there at once, which
// signs the app out before anything else runs.
struct FakeApp {
  al::SignedOutNotice notice;
  bool signedIn = true;
  int signOuts = 0;

  // AppController::OnAuthInvalid
  void Report(std::string_view cause) {
    if (!notice.Rejected(signedIn, cause)) return;
    ++signOuts;
    signedIn = false;
  }
  // the user's own Sign out (Settings, the account menu): SdkHost::Logout,
  // with no rejection behind it
  void SignOut() {
    ++signOuts;
    signedIn = false;
  }
  // AppController::OnAuthState(LoggedIn)
  void SignIn() {
    signedIn = true;
    notice.Clear();
  }
};

void TestSignedOutNotice() {
  // the cause the sdk reports with an AuthLogout: only the trusted
  // session-revoked cause, spelled exactly, is explained
  Check(al::NoticeFor("session_revoked") == al::Notice::SignedOutRemotely,
        "session_revoked: signed out from another device");
  for (const char* cause : {"", "Session_Revoked", "SESSION_REVOKED", "session_revoked ",
                            " session_revoked", "session_revoked\n", "session-revoked", "revoked",
                            "client_removed", "sign_in_required"}) {
    Check(al::NoticeFor(cause) == al::Notice::None,
          std::string("the cause \"") + cause + "\" shows nothing new");
  }
  CheckText("This session was signed out from another device.",
            English(al::NoticeKey(al::Notice::SignedOutRemotely)), "the sign-in page's notice");
  CheckText("", al::NoticeKey(al::Notice::None), "nothing new has no words");
  CheckText(ss::SignInRequiredKey(/*signedOutRemotely=*/true),
            al::NoticeKey(al::Notice::SignedOutRemotely),
            "the sign-in page says what a sessions screen still up says");

  // Another device signed this session out while a session was up: the Api's
  // listener reports the rejection, then the device's, built on that Api,
  // reports it again (a 401 that came back over the rpc as well).
  {
    FakeApp app;
    app.Report("session_revoked");
    app.Report("session_revoked");
    CheckEq(1, app.signOuts, "the Api's and the device's reports of one rejection sign out once");
    Check(app.notice.Take() == al::Notice::SignedOutRemotely,
          "the sign-in page says this session was signed out from another device");
    Check(app.notice.Take() == al::Notice::None,
          "once: the sign-in page shown again says nothing new");
  }
  // the device's report first, should the order ever change
  {
    FakeApp app;
    app.Report("session_revoked");
    app.Report("");
    CheckEq(1, app.signOuts, "the second report signs nothing out, whichever comes first");
    Check(app.notice.Take() == al::Notice::SignedOutRemotely,
          "and a later report does not take the notice away");
  }
  // signed in with no session up: the Api's listener alone
  {
    FakeApp app;
    app.Report("session_revoked");
    CheckEq(1, app.signOuts, "the Api's report alone signs out");
    Check(app.notice.Take() == al::Notice::SignedOutRemotely, "and says why");
  }
  // any other rejection: the generic sign-out, as before
  {
    FakeApp app;
    app.Report("");
    app.Report("");
    CheckEq(1, app.signOuts, "a generic rejection signs out once");
    Check(app.notice.Take() == al::Notice::None, "a generic rejection says nothing new");
  }
  // this session signed out from the Sessions screen: the sdk reports ""
  {
    FakeApp app;
    app.Report("");
    CheckEq(1, app.signOuts, "signing this session out from the list signs the app out");
    Check(app.notice.Take() == al::Notice::None,
          "signing this session out here is not another device's sign-out");
  }
  // the app's own Sign out, and a revocation that raced it
  {
    FakeApp app;
    app.SignOut();
    Check(app.notice.Take() == al::Notice::None, "the user's own sign-out says nothing");
    app.Report("session_revoked");
    CheckEq(1, app.signOuts, "a report behind the user's sign-out signs nothing out again");
    Check(app.notice.Take() == al::Notice::None,
          "and leaves no notice for the user's own sign-out");
  }
  // a notice the page never showed, then a sign-in while the window was
  // hidden (a deep link's auth code): the next sign-out does not explain the
  // old one
  {
    FakeApp app;
    app.Report("session_revoked");
    app.SignIn();
    Check(app.notice.Take() == al::Notice::None, "a sign-in forgets an unshown notice");
    app.SignOut();
    Check(app.notice.Take() == al::Notice::None,
          "the user's sign-out after it does not show the old notice");
  }
  // every remote sign-out of a later sign-in is explained again, once
  {
    FakeApp app;
    app.Report("session_revoked");
    Check(app.notice.Take() == al::Notice::SignedOutRemotely, "the first sign-out's notice");
    app.SignIn();
    app.Report("session_revoked");
    CheckEq(2, app.signOuts, "the second sign-in's rejection signs out");
    Check(app.notice.Take() == al::Notice::SignedOutRemotely, "and is explained too");
    Check(app.notice.Take() == al::Notice::None, "once");
  }
  // nothing signed in: a report changes nothing
  {
    FakeApp app;
    app.signedIn = false;
    app.Report("session_revoked");
    CheckEq(0, app.signOuts, "a signed-out app is not signed out again");
    Check(app.notice.Take() == al::Notice::None, "and says nothing");
  }
}

void TestReader() {
  // fakes of the generated classes, every field
  const ss::Snapshot rich = RichSnapshot();
  const ss::Snapshot read = ss::SnapshotFrom(FakeOf(rich));
  Check(read == rich, "SnapshotFrom reads every field of the snapshot");
  Check(read.sessions.size() == 3 && !read.sessions[1].lastUsed, "a nil LastUsed stays nil");
  Check(read.bulkAction && read.bulkAction->pending, "the bulk action");
  Check(read.actions.size() == 2 && read.actions[1].error && read.actions[1].error->retryable,
        "an action's error");
  Check(read.error && read.error->signInRequired && read.error->sessionRevoked,
        "the error's trusted session-revoked cause is read");

  // nil lists and objects
  FakeSnapshot nils;
  nils.sessions.present = false;
  nils.actions.present = false;
  const ss::Snapshot empty = ss::SnapshotFrom(nils);
  Check(empty.sessions.empty() && empty.actions.empty(), "nil lists read as empty");
  Check(!empty.bulkAction && !empty.error, "nil objects read as nullopt");

  // the Unix seconds become milliseconds only where a line uses them
  CheckEq(rich.sessions[0].lastUsed->unixTime, read.sessions[0].lastUsed->unixTime,
          "LastUsed.UnixTime is carried in seconds");
}

#if defined(URNW_SESSIONS_TESTS_SDK)
void TestSdkReader() {
  // the same snapshot through the generated header's own classes
  const ss::Snapshot rich = RichSnapshot();
  const FakeSnapshot fake = FakeOf(rich);
  abi::gSnapshot = &fake;
  const uint64_t handle = abi::Issue(abi::Kind::Snapshot, &fake);
  {
    // the listener receives an owned handle (urnetwork_sdk.h)
    const urnet::ClientSessionSnapshot snapshot(handle);
    const ss::Snapshot read = ss::SnapshotFrom(snapshot);
    Check(read == rich, "SnapshotFrom reads every field through urnet::ClientSessionSnapshot");
  }
  CheckEq(abi::gIssued, abi::gReleased, "every handle the reader took is released");
  CheckEq(0, abi::gUnknownReleases, "no handle is released twice");
  CheckEq(0, static_cast<int64_t>(abi::gLive.size()), "nothing is left live");

  FakeSnapshot nils;
  nils.sessions.present = false;
  nils.actions.present = false;
  const int64_t issuedBefore = abi::gIssued;
  {
    const urnet::ClientSessionSnapshot snapshot(abi::Issue(abi::Kind::Snapshot, &nils));
    const ss::Snapshot read = ss::SnapshotFrom(snapshot);
    Check(read.sessions.empty() && read.actions.empty() && !read.bulkAction && !read.error,
          "nil handles (0) read as empty lists and nullopt through the wrapper");
  }
  CheckEq(1, abi::gIssued - issuedBefore, "a nil getter hands out no handle");
  CheckEq(abi::gIssued, abi::gReleased, "and the snapshot handle is released");

  // the sdk's own spellings of the strings the page compares against
  Check(std::string_view(ss::DeviceLabelKey("windows")) == "sessions_device_windows",
        "the device type windows reports");
  CheckEq(abi::gStringsIssued, abi::gStringsFreed, "every string the snapshot reader took is freed");
}

// The logout listeners' read of the cause (AuthLogoutCause.h), by the handle
// each listener was added on, through the c abi the generated header
// declares; the wrapper's own getters read the same; and the header's
// constant is the cause the notice is shown for.
void TestSdkLogoutCause() {
  CheckText(std::string(al::kCauseSessionRevoked), URNET_AUTH_LOGOUT_CAUSE_SESSION_REVOKED,
            "the c abi's session-revoked cause");
  CheckText(std::string(al::kCauseSessionRevoked), urnet::AuthLogoutCauseSessionRevoked,
            "the wrapper's session-revoked cause");

  abi::FakeLogout revoked{std::string(URNET_AUTH_LOGOUT_CAUSE_SESSION_REVOKED)};
  abi::FakeLogout generic{std::string()};
  abi::FakeLogout nil{std::nullopt};
  const uint64_t api = abi::Issue(abi::Kind::Api, &revoked);
  const uint64_t device = abi::Issue(abi::Kind::Device, &revoked);
  const int64_t stringsBefore = abi::gStringsIssued;
  const int64_t freedBefore = abi::gStringsFreed;

  // what each listener reads before it marshals, and the notice it leads to
  CheckText("session_revoked", al::ApiCause(api), "the Api's listener reads the Api's cause");
  CheckText("session_revoked", al::DeviceCause(device),
            "the device's listener reads the device's cause");
  Check(al::NoticeFor(al::ApiCause(api)) == al::Notice::SignedOutRemotely &&
            al::NoticeFor(al::DeviceCause(device)) == al::Notice::SignedOutRemotely,
        "either listener's read of a revoked session leads to the notice");
  CheckEq(4, abi::gStringsIssued - stringsBefore, "each read takes the sdk's string");
  CheckEq(abi::gStringsIssued - stringsBefore, abi::gStringsFreed - freedBefore,
          "and frees every one of them");
  CheckText("", al::ApiCause(0), "a nil Api reads no cause");
  Check(al::DeviceCause(api).empty() && al::ApiCause(device).empty(),
        "a handle of another kind reads no cause");
  // a listener still in flight while its session is torn down
  const uint64_t released = abi::Issue(abi::Kind::Device, &revoked);
  urnet_release(released);
  CheckText("", al::DeviceCause(released), "a device handle released since reads no cause");

  const uint64_t genericApi = abi::Issue(abi::Kind::Api, &generic);
  const uint64_t nilDevice = abi::Issue(abi::Kind::Device, &nil);
  CheckText("", al::ApiCause(genericApi), "a generic rejection's cause is empty");
  Check(al::NoticeFor(al::ApiCause(genericApi)) == al::Notice::None,
        "and leads to no notice");
  CheckText("", al::DeviceCause(nilDevice), "a nil string reads as no cause");

  // the generated wrapper's own getters make the same calls
  {
    const urnet::Api wrapped(abi::Issue(abi::Kind::Api, &revoked));
    CheckText("session_revoked", wrapped.getAuthLogoutCause(), "Api::getAuthLogoutCause");
    const urnet::Device wrappedDevice(abi::Issue(abi::Kind::Device, &generic));
    CheckText("", wrappedDevice.getAuthLogoutCause(), "Device::getAuthLogoutCause");
  }
  CheckEq(abi::gStringsIssued, abi::gStringsFreed, "every cause string is freed");
  for (const uint64_t handle : {api, device, genericApi, nilDevice}) urnet_release(handle);
  CheckEq(0, static_cast<int64_t>(abi::gLive.size()), "every handle is released");
}
#endif

}  // namespace

int main() {
  TestRelativeTime();
  TestLabels();
  TestCircleColor();
  TestRows();
  TestStates();
  TestActions();
  TestFence();
  TestSignedOutNotice();
  TestReader();
#if defined(URNW_SESSIONS_TESTS_SDK)
  TestSdkReader();
  TestSdkLogoutCause();
#endif
  if (gFailures != 0) {
    std::cout << gFailures << " of " << gCases << " sessions checks failed\n";
    return 1;
  }
  std::cout << gCases << "/" << gCases << " sessions checks passed\n";
  return 0;
}
