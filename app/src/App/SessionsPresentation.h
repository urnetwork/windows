// Everything the Account > Sessions page (SessionsPage.h) decides before it
// touches a XAML object (server session/REVOKE-UI-FINAL.md): the snapshot read
// out of the sdk's ClientSessionViewController as plain values, which of
// progress, the rows, empty, failed, unsupported and sign-in required the
// page shows, every row's three lines and spoken lines, its device label and
// logo, its sign-in method and short id, its country circle's colour, its
// sign-out state, the device sign-ins' Show more, the client groups below them
// (each a heading with its count that opens and closes, its rows while open,
// and their Show more), the bulk "Sign out all other sessions" button, the
// footer notes and the confirmations.
//
// The controller does the fetching, paging, polling, operation ids, retries,
// stale-response protection, which session goes in which group, every count
// and which sessions the bulk sign-out signs out; nothing here repeats any of
// it. It publishes a ClientSessionSnapshot on its own thread, SnapshotFrom
// copies it into a Snapshot there, and the page renders ViewFor(snapshot) on
// the UI thread.
//
// Pure, for the reason ExtenderPresentation.h gives: a WinUI 3 app cannot be
// built off Windows, so tools/sessions-tests.cpp verifies every decision on
// any host with a C++20 compiler. The readers are templates over the getters
// of the generated header's ClientSessionSnapshot family, so the tests run
// them on fakes and compile them against urnetwork_sdk.hpp; SessionsPage.cpp
// passes the sdk's objects straight in.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ExtenderRingGeometry.h"  // ParseExtenderColorHex: the sdk's six-digit colours
#include "SessionGlyphs.h"

namespace urnw::sessions {

// ---- the snapshot, as plain values ------------------------------------------

// SessionLastUsed: the server's last observed authenticated use.
struct LastUsed {
  int64_t unixTime = 0;  // UTC seconds
  std::string city;
  std::string region;
  std::string country;
  std::string countryCode;  // lower-case ISO alpha-2, "" when unknown
  // the sdk's device kind (LastUsed.DeviceKind): android, ios, macos, windows,
  // linux, web, extension (the browser extension), cli, server, unknown
  std::string deviceType;
  std::string appVersion;   // "" when unknown

  bool operator==(const LastUsed&) const = default;
};

// NetworkSessionInfo, the fields the rows show. LastMintTime and
// TokenExpireTime are never shown (§3.1), so they are not read.
struct Session {
  std::string sessionId;
  bool current = false;
  std::string kind;
  int64_t createTimeMillis = 0;  // 0 when the server sent none
  std::optional<LastUsed> lastUsed;

  bool operator==(const Session&) const = default;
};

// ClientSessionError. The page acts on the flags; the message is the
// controller's English and is only logged (§5).
struct Error {
  std::string message;
  bool retryable = false;
  bool signInRequired = false;
  // with signInRequired: the server confirmed another device signed this
  // session out; never for a generic rejection or a sign-out made here
  bool sessionRevoked = false;
  bool unsupported = false;

  bool operator==(const Error&) const = default;
};

// ClientSessionAction: a revoke of one session, or the bulk revoke of the others.
struct Action {
  std::string sessionId;  // "" for the bulk action
  bool loading = false;   // the request is out
  bool pending = false;   // accepted (202), not yet confirmed enforced
  std::optional<Error> error;

  bool operator==(const Action&) const = default;
};

// ClientSessionGroup: one kind of client's sessions, below the device
// sign-ins, collapsed to its count until the page opens it.
struct Group {
  // kGroupHostedProxies, kGroupApiClients, kGroupPairedDevices or
  // kGroupOtherClients
  std::string kind;
  // the group's sessions; a lower bound while the snapshot is truncated
  int64_t count = 0;
  // opened (SetGroupExpanded)
  bool expanded = false;
  // while expanded, the rows to show in the controller's order; none while
  // collapsed
  std::vector<Session> sessions;
  // expanded, with rows past `sessions`: Show more (ShowMoreSessions)
  bool hasMore = false;

  bool operator==(const Group&) const = default;
};

// ClientSessionSnapshot.
struct Snapshot {
  // the device sign-ins to show, in the controller's order: the current
  // session first, then the most recent use; a session a client owns is in a
  // group instead
  std::vector<Session> sessions;
  // every device sign-in, those past `sessions` too; a lower bound while
  // truncated
  int64_t sessionCount = 0;
  // more device sign-ins than `sessions`: Show more
  // (ShowMoreSessions(kGroupSignIns))
  bool sessionsHasMore = false;
  // the client groups in the controller's order; a kind with no sessions has
  // none
  std::vector<Group> groups;
  // the sessions "Sign out all other sessions" signs out: every listed one but
  // the current session and the hosted proxies, which it keeps
  int64_t otherSessionCount = 0;
  // the list was longer than the controller reads: the counts are lower bounds
  bool truncated = false;
  std::string currentSessionId;
  std::string legacyCoverage;  // "partial" while older sign-ins may be missing
  bool loaded = false;
  bool loading = false;
  bool refreshing = false;
  bool supported = true;
  std::optional<Action> bulkAction;
  std::vector<Action> actions;
  std::optional<Error> error;

  bool operator==(const Snapshot&) const = default;
};

// ---- reading the sdk's objects ----------------------------------------------
//
// Getters only, by the generated wrapper's names; a nil object is a handle of
// 0, which tests false, and every list is read by len() and get(i). Run on the
// thread the controller publishes on: each getter is a call into the sdk, and
// none of it belongs on the UI thread.

template <typename SdkError>
std::optional<Error> ErrorFrom(const SdkError& error) {
  if (!error) return std::nullopt;
  Error out;
  out.message = error.getMessage();
  out.retryable = error.getRetryable();
  out.signInRequired = error.getSignInRequired();
  out.sessionRevoked = error.getSessionRevoked();
  out.unsupported = error.getUnsupported();
  return out;
}

template <typename SdkAction>
std::optional<Action> ActionFrom(const SdkAction& action) {
  if (!action) return std::nullopt;
  Action out;
  out.sessionId = action.getSessionId();
  out.loading = action.getLoading();
  out.pending = action.getPending();
  out.error = ErrorFrom(action.getError());
  return out;
}

template <typename SdkLastUsed>
std::optional<LastUsed> LastUsedFrom(const SdkLastUsed& lastUsed) {
  if (!lastUsed) return std::nullopt;
  LastUsed out;
  out.unixTime = lastUsed.getUnixTime();
  out.city = lastUsed.getCity();
  out.region = lastUsed.getRegion();
  out.country = lastUsed.getCountry();
  out.countryCode = lastUsed.getCountryCode();
  // the sdk's kind names the browser extension; a type from an sdk without it
  // is the same for every other device
  out.deviceType = lastUsed.getDeviceKind();
  if (out.deviceType.empty()) out.deviceType = lastUsed.getDeviceType();
  out.appVersion = lastUsed.getAppVersion();
  return out;
}

template <typename SdkSession>
Session SessionFrom(const SdkSession& session) {
  Session out;
  out.sessionId = session.getSessionId();
  out.current = session.getCurrent();
  out.kind = session.getKind();
  out.createTimeMillis = session.getCreateTime();  // unix millis, 0 for none
  out.lastUsed = LastUsedFrom(session.getLastUsed());
  return out;
}

template <typename SdkSessions>
std::vector<Session> SessionsFrom(const SdkSessions& sessions) {
  std::vector<Session> out;
  if (!sessions) return out;
  const int64_t count = sessions.len();
  for (int64_t i = 0; i < count; ++i) out.push_back(SessionFrom(sessions.get(i)));
  return out;
}

template <typename SdkGroup>
Group GroupFrom(const SdkGroup& group) {
  Group out;
  out.kind = group.getKind();
  out.count = group.getCount();
  out.expanded = group.getExpanded();
  out.sessions = SessionsFrom(group.getSessions());
  out.hasMore = group.getHasMore();
  return out;
}

template <typename SdkSnapshot>
Snapshot SnapshotFrom(const SdkSnapshot& snapshot) {
  Snapshot out;
  out.sessions = SessionsFrom(snapshot.getSessions());
  out.sessionCount = snapshot.getSessionCount();
  out.sessionsHasMore = snapshot.getSessionsHasMore();
  if (const auto groups = snapshot.getGroups()) {
    const int64_t count = groups.len();
    for (int64_t i = 0; i < count; ++i) {
      if (const auto group = groups.get(i)) out.groups.push_back(GroupFrom(group));
    }
  }
  out.otherSessionCount = snapshot.getOtherSessionCount();
  out.truncated = snapshot.getTruncated();
  out.currentSessionId = snapshot.getCurrentSessionId();
  out.legacyCoverage = snapshot.getLegacyCoverage();
  out.loaded = snapshot.getLoaded();
  out.loading = snapshot.getLoading();
  out.refreshing = snapshot.getRefreshing();
  out.supported = snapshot.getSupported();
  out.bulkAction = ActionFrom(snapshot.getBulkAction());
  if (const auto actions = snapshot.getActions()) {
    const int64_t count = actions.len();
    for (int64_t i = 0; i < count; ++i) {
      if (auto action = ActionFrom(actions.get(i))) out.actions.push_back(std::move(*action));
    }
  }
  out.error = ErrorFrom(snapshot.getError());
  return out;
}

// ---- labels (§3.2) ----------------------------------------------------------

// The store key of a device type's label; an empty or unknown type is
// "Unknown device".
constexpr const char* DeviceLabelKey(std::string_view deviceType) {
  if (deviceType == "android") return "sessions_device_android";
  if (deviceType == "ios") return "sessions_device_ios";
  if (deviceType == "macos") return "sessions_device_macos";
  if (deviceType == "windows") return "sessions_device_windows";
  if (deviceType == "linux") return "sessions_device_linux";
  if (deviceType == "web") return "sessions_device_web";
  // the browser extension's own sign-in (urnet::SessionDeviceKindExtension)
  if (deviceType == "extension") return "sessions_device_extension";
  if (deviceType == "cli") return "sessions_device_cli";
  if (deviceType == "server") return "sessions_device_server";
  return "sessions_device_unknown";
}

// The device logos (§8). ios and macos share the apple logo.
enum class DeviceGlyph { Android, Apple, Windows, Linux, Web, Cli, Server, Unknown };

constexpr DeviceGlyph DeviceGlyphFor(std::string_view deviceType) {
  if (deviceType == "android") return DeviceGlyph::Android;
  if (deviceType == "ios" || deviceType == "macos") return DeviceGlyph::Apple;
  if (deviceType == "windows") return DeviceGlyph::Windows;
  if (deviceType == "linux") return DeviceGlyph::Linux;
  // the browser extension is a browser: the web logo
  if (deviceType == "web" || deviceType == "extension") return DeviceGlyph::Web;
  if (deviceType == "cli") return DeviceGlyph::Cli;
  if (deviceType == "server") return DeviceGlyph::Server;
  return DeviceGlyph::Unknown;
}

// A logo's path on its 24x24 box (SessionGlyphs.h).
constexpr const wchar_t* GlyphPath(DeviceGlyph glyph) {
  switch (glyph) {
    case DeviceGlyph::Android: return glyph::kDeviceAndroidPath;
    case DeviceGlyph::Apple: return glyph::kDeviceApplePath;
    case DeviceGlyph::Windows: return glyph::kDeviceWindowsPath;
    case DeviceGlyph::Linux: return glyph::kDeviceLinuxPath;
    case DeviceGlyph::Web: return glyph::kDeviceWebPath;
    case DeviceGlyph::Cli: return glyph::kDeviceCliPath;
    case DeviceGlyph::Server: return glyph::kDeviceServerPath;
    case DeviceGlyph::Unknown: return glyph::kDeviceUnknownPath;
  }
  return glyph::kDeviceUnknownPath;
}

// The store key of how a session signed in, the kinds the server mints today;
// "" for legacy, legacy_proxy and any kind this build does not know, whose
// method is omitted from the row.
constexpr const char* KindLabelKey(std::string_view kind) {
  if (kind == "password") return "sessions_kind_password";
  if (kind == "verify") return "sessions_kind_verify";
  if (kind == "apple") return "sessions_kind_apple";
  if (kind == "google") return "sessions_kind_google";
  if (kind == "sso") return "sessions_kind_sso";
  if (kind == "wallet") return "sessions_kind_wallet";
  if (kind == "seedphrase") return "sessions_kind_seedphrase";
  if (kind == "signup") return "sessions_kind_signup";
  if (kind == "auth_code") return "sessions_kind_auth_code";
  if (kind == "device_adopt") return "sessions_kind_device_adopt";
  if (kind == "api_key_client") return "sessions_kind_api_key_client";
  return "";
}

// The row shows this much of the id; the copy puts the whole id on the
// clipboard (§1.3).
inline constexpr std::size_t kShortIdLength = 8;

inline std::string ShortId(std::string_view sessionId) {
  return std::string(sessionId.substr(0, kShortIdLength));
}

// The parts of a row line, and of a spoken line, which a screen reader reads
// with a pause rather than as "middle dot".
inline constexpr std::string_view kSeparator = " \xC2\xB7 ";  // " · "
inline constexpr std::string_view kSpokenSeparator = ", ";

inline std::string JoinParts(const std::vector<std::string>& parts, std::string_view separator) {
  std::string out;
  for (const auto& part : parts) {
    if (part.empty()) continue;
    if (!out.empty()) out.append(separator);
    out.append(part);
  }
  return out;
}

// "Chicago, Illinois, United States", leaving out the parts the server did not
// know; "" when it knew none.
inline std::string PlaceText(const LastUsed& lastUsed) {
  return JoinParts({lastUsed.city, lastUsed.region, lastUsed.country}, ", ");
}

// ---- the groups -------------------------------------------------------------
//
// The sdk's kinds of section (sdk ClientSessionGroup*), spelled here so this
// header needs no sdk header; tests/sessions_test.go and the spec built
// against urnetwork_sdk.hpp hold them to the sdk's. The device sign-ins are
// the page's first section; the others are the client groups below them, in
// the controller's order.
inline constexpr const char* kGroupSignIns = "sign_ins";
inline constexpr const char* kGroupHostedProxies = "hosted_proxies";
inline constexpr const char* kGroupApiClients = "api_clients";
inline constexpr const char* kGroupPairedDevices = "paired_devices";
inline constexpr const char* kGroupOtherClients = "other_clients";

// The store key of a group's heading, "Hosted proxies ({count})". A kind this
// build does not know is another client's, as the sdk files an unknown kind
// of client.
constexpr const char* GroupLabelKey(std::string_view kind) {
  if (kind == kGroupHostedProxies) return "sessions_group_hosted_proxies";
  if (kind == kGroupApiClients) return "sessions_group_api_clients";
  if (kind == kGroupPairedDevices) return "sessions_group_paired_devices";
  return "sessions_group_other_clients";
}

// The store key of a group's row label: line 1's name, the sign-out's
// accessible name and the confirmation's {device}, in place of the device
// label, since the session is the client's own and not a device's sign-in.
constexpr const char* GroupRowLabelKey(std::string_view kind) {
  if (kind == kGroupHostedProxies) return "manage_devices_proxy";
  if (kind == kGroupApiClients) return "sessions_client_api_key";
  if (kind == kGroupPairedDevices) return "sessions_client_device";
  return "sessions_client_other";
}

// ---- the country circle -----------------------------------------------------

struct Rgb {
  uint8_t r = 0;
  uint8_t g = 0;
  uint8_t b = 0;

  bool operator==(const Rgb&) const = default;
};

// The colour this app gives a country it does not know: the provider globe's
// kUnknownCountryColor (ProviderGlobe.cpp), the web globe's neutral blue.
inline constexpr Rgb kUnknownCountryRgb{0x00, 0x99, 0xFF};

// The circle's fill: the sdk's colour for the country code (getColorHex, the
// palette the location list and the globe use), or the unknown-country colour
// for an empty code or a colour that does not parse.
template <typename ColorHex>
Rgb CircleColorFor(std::string_view countryCode, ColorHex colorHex) {
  if (countryCode.empty()) return kUnknownCountryRgb;
  ExtenderRgb parsed;
  if (!ParseExtenderColorHex(colorHex(std::string(countryCode)), parsed)) return kUnknownCountryRgb;
  return {parsed.r, parsed.g, parsed.b};
}

// ---- the words --------------------------------------------------------------

// How the page's words are made: the store's strings, the relative time, the
// dates and the counts, all UTF-8. SessionsPage passes urnw::Localized and
// Format, urnw::RelativeTime against its clock, and the Windows dates and
// digit grouping (StatsFormat.h); the tests pass English, a fixed clock and a
// fixed grouping.
struct Text {
  std::function<std::string(std::string_view key)> localized;
  // a key with one placeholder
  std::function<std::string(std::string_view key, const std::string& value)> format;
  // a key with two placeholders, {0} and {1}
  std::function<std::string(std::string_view key, const std::string& first,
                            const std::string& second)>
      format2;
  // "5m ago", or a date from a week on
  std::function<std::string(int64_t unixMillis)> relative;
  // the localized date a session signed in on
  std::function<std::string(int64_t unixMillis)> date;
  // the full localized date and time, which a relative time or a date is read
  // out as (§3.1)
  std::function<std::string(int64_t unixMillis)> dateTime;
  // a count in the user's digit grouping, "1,204"
  std::function<std::string(int64_t count)> count;
};

// A group heading's {count}: the count in the user's digit grouping, with "+"
// while the list was longer than the controller reads and the count is a lower
// bound ("10,000+").
inline std::string CountText(int64_t count, bool truncated, const Text& text) {
  std::string out = text.count(count);
  if (truncated) out += "+";
  return out;
}

// ---- the view ---------------------------------------------------------------

// A revoke's state on its row, and the bulk revoke's on its button.
enum class ActionState {
  Idle,     // "Sign out" is offered
  Pending,  // Loading or Pending: progress and "Signing out…", the control disabled
  Failed,   // the failure's words (ActionFailedKey), and the control offered again
};

constexpr ActionState ActionStateFor(const std::optional<Action>& action) {
  if (!action) return ActionState::Idle;
  if (action->loading || action->pending) return ActionState::Pending;
  if (action->error) return ActionState::Failed;
  return ActionState::Idle;
}

// The store key of a failed sign-out's words (§5): a row's under its lines,
// the bulk one's under "Sign out all other sessions". Either control stays
// enabled, and its retry goes through the controller again.
constexpr const char* ActionFailedKey(bool bulk) {
  return bulk ? "sessions_sign_out_others_failed" : "sessions_action_failed";
}

// One session's row (§3), a device sign-in's or, the same row, a client
// group's.
struct Row {
  std::string sessionId;  // the whole id: the copy and the revoke take it
  bool current = false;   // the "This session" tag, and the self sign-out body
  // the kind of the group the row is listed in; "" for a device sign-in
  std::string group;
  DeviceGlyph glyph = DeviceGlyph::Unknown;
  std::string countryCode;  // the circle's colour; "" is the unknown-country colour
  // "Android", or a group's row label ("Hosted proxy"): the row's name in the
  // confirmation and its action
  std::string device;
  std::string place;        // "" when the server knew no location
  std::string line1;        // "Android · 2026.10.8-1067"
  std::string line1Spoken;  // "Android, 2026.10.8-1067"
  std::string line2;        // "Chicago, Illinois, United States · Last used 5m ago"
  std::string line2Spoken;  // the same, with the full date and time
  std::string line3;        // "Signed in 10/3/2026 · Google · ID 01a1f3c2"
  std::string line3Spoken;
  std::string shortId;      // "01a1f3c2"
  std::string idText;       // "ID 01a1f3c2": what the copy of the whole id is for
  std::string signOutName;  // "Sign out Android": the trailing button's accessible name
  ActionState action = ActionState::Idle;

  bool operator==(const Row&) const = default;
};

// What the page's body shows (§5).
enum class Body {
  Progress,        // never loaded, and the load has not failed (or is being retried)
  LoadFailed,      // the first load failed: "Couldn't load sessions." and Try again
  Unsupported,     // the server has no sessions yet: "Sessions aren't available yet."
  SignInRequired,  // "Sign in again to manage sessions." (SignInRequiredKey); the
                   // app's sign-in flow follows
  Empty,           // loaded, with no device sign-in and no client group: "No active sessions"
  Rows,
};

// A client group below the device sign-ins: a heading that opens and closes
// it, its rows while it is open, and Show more under them.
struct GroupView {
  std::string kind;   // what SetGroupExpanded and ShowMoreSessions take
  std::string label;  // "Hosted proxies (1,204+)": the heading, and its button's name
  bool expanded = false;
  std::vector<Row> rows;  // while expanded
  bool showMore = false;  // expanded, with more rows: Show more under them

  bool operator==(const GroupView&) const = default;
};

struct View {
  Body body = Body::Progress;
  // with SignInRequired: the controller reported the trusted cause, another
  // device signed this session out, which the body says instead
  bool signedOutRemotely = false;
  // the header's refresh indicator: a refresh is running over the rows
  bool refreshing = false;
  // a refresh failed over a loaded list: the rows stay, with the notice
  bool refreshFailed = false;
  // the device sign-ins
  std::vector<Row> rows;
  // Show more under the device sign-ins (ShowMoreSessions(kGroupSignIns))
  bool showMore = false;
  // the client groups, below the device sign-ins
  std::vector<GroupView> groups;
  // "Sign out all other sessions": a current session, and other sessions it
  // signs out (the controller's count, which leaves the hosted proxies out)
  bool bulkShown = false;
  // the bulk sign-out keeps the hosted proxies listed here, and its
  // confirmation says so
  bool bulkKeepsProxies = false;
  ActionState bulk = ActionState::Idle;
  // under the rows: what Last used means
  bool lastUsedHelp = false;
  // under the rows: older app versions' sign-ins may be missing
  bool legacyNote = false;

  bool operator==(const View&) const = default;
};

inline bool IsCurrent(const Session& session, std::string_view currentSessionId) {
  return session.current || (!currentSessionId.empty() && session.sessionId == currentSessionId);
}

inline std::optional<Action> ActionFor(const Snapshot& snapshot, std::string_view sessionId) {
  for (const auto& action : snapshot.actions) {
    if (action.sessionId == sessionId) return action;
  }
  return std::nullopt;
}

// One row's words (§3): line 1 the device and app version, line 2 where and
// when it was last used, line 3 when and how it signed in and its short id. A
// row of the client group `group` is the same row, named for its kind of
// client in place of the device (GroupRowLabelKey).
inline Row RowFor(const Session& session, const Snapshot& snapshot, const Text& text,
                  std::string_view group = {}) {
  Row row;
  row.sessionId = session.sessionId;
  row.current = IsCurrent(session, snapshot.currentSessionId);
  row.group = std::string(group);
  row.shortId = ShortId(session.sessionId);
  row.action = ActionStateFor(ActionFor(snapshot, session.sessionId));

  const std::string deviceType = session.lastUsed ? session.lastUsed->deviceType : std::string();
  row.glyph = DeviceGlyphFor(deviceType);
  row.device =
      text.localized(group.empty() ? DeviceLabelKey(deviceType) : GroupRowLabelKey(group));
  const std::string appVersion = session.lastUsed ? session.lastUsed->appVersion : std::string();
  row.line1 = JoinParts({row.device, appVersion}, kSeparator);
  row.line1Spoken = JoinParts({row.device, appVersion}, kSpokenSeparator);

  if (session.lastUsed) {
    row.countryCode = session.lastUsed->countryCode;
    row.place = PlaceText(*session.lastUsed);
  }
  if (session.lastUsed && 0 < session.lastUsed->unixTime) {
    const int64_t usedMillis = session.lastUsed->unixTime * 1000;  // the server's seconds
    row.line2 = JoinParts({row.place, text.format("sessions_last_used", text.relative(usedMillis))},
                          kSeparator);
    const std::string usedSpoken = text.format("sessions_last_used", text.dateTime(usedMillis));
    row.line2Spoken = JoinParts({row.place, usedSpoken}, kSpokenSeparator);
  } else {
    const std::string unavailable = text.localized("sessions_last_use_unavailable");
    row.line2 = JoinParts({row.place, unavailable}, kSeparator);
    row.line2Spoken = JoinParts({row.place, unavailable}, kSpokenSeparator);
  }

  const char* kindKey = KindLabelKey(session.kind);
  const std::string method = *kindKey ? text.localized(kindKey) : std::string();
  row.idText = text.format("sessions_id", row.shortId);
  std::string signedIn;
  std::string signedInSpoken;
  if (session.createTimeMillis != 0) {
    signedIn = text.format("sessions_signed_in", text.date(session.createTimeMillis));
    signedInSpoken = text.format("sessions_signed_in", text.dateTime(session.createTimeMillis));
  }
  row.line3 = JoinParts({signedIn, method, row.idText}, kSeparator);
  row.line3Spoken = JoinParts({signedInSpoken, method, row.idText}, kSpokenSeparator);

  row.signOutName = text.format("sessions_sign_out_accessibility", row.device);
  return row;
}

// The page from one snapshot (§5). Unsupported and sign-in required win over a
// list; a list that loaded stays through a failed refresh, with its notice.
inline View ViewFor(const Snapshot& snapshot, const Text& text) {
  View view;
  view.refreshing = snapshot.refreshing;
  if (!snapshot.supported || (snapshot.error && snapshot.error->unsupported)) {
    view.body = Body::Unsupported;
    return view;
  }
  if (snapshot.error && snapshot.error->signInRequired) {
    view.body = Body::SignInRequired;
    view.signedOutRemotely = snapshot.error->sessionRevoked;
    return view;
  }
  if (!snapshot.loaded) {
    // a Try again re-runs the load with the failure still on the snapshot
    view.body = snapshot.error && !snapshot.loading ? Body::LoadFailed : Body::Progress;
    return view;
  }
  view.refreshFailed = snapshot.error.has_value();
  view.legacyNote = snapshot.legacyCoverage == "partial";
  // no device sign-in (a row on screen counts, whatever the count says) and
  // no client group
  if (snapshot.sessionCount == 0 && snapshot.sessions.empty() && snapshot.groups.empty()) {
    view.body = Body::Empty;
    return view;
  }
  view.body = Body::Rows;
  view.lastUsedHelp = true;
  bool current = false;
  for (const auto& session : snapshot.sessions) {
    Row row = RowFor(session, snapshot, text);
    if (row.current) current = true;
    view.rows.push_back(std::move(row));
  }
  view.showMore = snapshot.sessionsHasMore;
  for (const auto& group : snapshot.groups) {
    GroupView groupView;
    groupView.kind = group.kind;
    groupView.label =
        text.format(GroupLabelKey(group.kind), CountText(group.count, snapshot.truncated, text));
    groupView.expanded = group.expanded;
    // a collapsed group carries no rows
    if (group.expanded) {
      for (const auto& session : group.sessions) {
        groupView.rows.push_back(RowFor(session, snapshot, text, group.kind));
      }
      groupView.showMore = group.hasMore;
    }
    if (group.kind == kGroupHostedProxies) view.bulkKeepsProxies = true;
    view.groups.push_back(std::move(groupView));
  }
  view.bulkShown = current && snapshot.otherSessionCount > 0;
  view.bulk = ActionStateFor(snapshot.bulkAction);
  return view;
}

// The row of `sessionId` on the page, a device sign-in or a row of an open
// group; nullptr when it is not shown.
inline const Row* FindRow(const View& view, std::string_view sessionId) {
  for (const auto& row : view.rows) {
    if (row.sessionId == sessionId) return &row;
  }
  for (const auto& group : view.groups) {
    for (const auto& row : group.rows) {
      if (row.sessionId == sessionId) return &row;
    }
  }
  return nullptr;
}

// ---- the confirmations (§4) -------------------------------------------------

struct Confirmation {
  std::string title;
  std::string body;

  bool operator==(const Confirmation&) const = default;
};

// Every sign-out asks first, naming the session; the current one warns that
// this app signs out with it, and a hosted proxy's that the proxy stops
// working.
inline Confirmation ConfirmationFor(const Row& row, const Text& text) {
  Confirmation confirmation;
  confirmation.title = text.localized("sessions_confirm_title");
  if (row.current) {
    confirmation.body = text.localized("sessions_confirm_self_body");
  } else if (row.group == kGroupHostedProxies) {
    confirmation.body = text.localized("sessions_confirm_body_hosted_proxy");
  } else if (!row.place.empty()) {
    confirmation.body = text.format2("sessions_confirm_body", row.device, row.place);
  } else {
    confirmation.body = text.format("sessions_confirm_body_no_place", row.device);
  }
  return confirmation;
}

// "Sign out all other sessions?": the body says the hosted proxies listed here
// stay, when there are any (the controller's bulk sign-out keeps them).
inline Confirmation BulkConfirmationFor(const View& view, const Text& text) {
  return {text.localized("sessions_confirm_others_title"),
          text.localized(view.bulkKeepsProxies ? "sessions_confirm_others_body_keeps_proxies"
                                               : "sessions_confirm_others_body")};
}

// ---- the page's fence against an old controller -----------------------------

// A snapshot is read on the controller's thread and applied on the UI thread
// later, and by then the page may have closed that controller (a sign-out, a
// rail navigation) and opened another, for another account. Each open takes a
// new number, the listener carries the number it was added under, and only
// the open controller's snapshots apply. UI thread only.
class OpenFence {
 public:
  uint64_t Open() {
    open_ = ++last_;
    return open_;
  }
  void Close() { open_ = 0; }
  // the open controller's number, 0 with none open: what a confirmation that
  // outlives its list is checked against
  uint64_t Current() const { return open_; }
  bool Admits(uint64_t generation) const { return generation != 0 && generation == open_; }

 private:
  uint64_t last_ = 0;
  uint64_t open_ = 0;
};

// ---- sign-in required (§5) --------------------------------------------------

// The store key of the sign-in-required body: the generic wording, which
// names no cause, or "This session was signed out from another device." only
// for the trusted cause the controller reports (View::signedOutRemotely). The
// app-wide sign-out that follows says the same on the sign-in page
// (AuthLogoutNotice.h); these are the page's words while it is still up.
constexpr const char* SignInRequiredKey(bool signedOutRemotely) {
  return signedOutRemotely ? "sessions_signed_out_remotely" : "sessions_sign_in_required";
}

}  // namespace urnw::sessions
