// The device's client limit hold, as the app shows it (owner, 2026-11-08: the
// windows app at parity with apple and android). The platform closed the
// session's device for its network's concurrent client limit, and the sdk holds
// it off the platform for 15 to 20 minutes: the service's DeviceLocal reports
// ClientLimitStatus, which SdkHost's client limit feed reads over the device rpc.
// While a connection is requested the connect screen's notice says what to do,
// and while the app is in the background the tray says it too. Nothing
// disconnects: the traffic stays in the tunnel (the service's failsafe holds for
// it as well, Service/DeadTunnelVerdict.h ClientLimitHold), and Disconnect is the
// user's.
//
// What to offer and when the tray says it are the sdk's decisions, the same on
// every platform: ClientLimitStatus.Prompt (upgrade for a free plan, which UR Pro
// lifts; remove_client for a plan already at its most devices, which only a free
// slot lifts), AuthNetworkClientError.ClientLimitPrompt for a refused sign-in,
// and ClientLimitNoticeTracker, which alerts once per connection request,
// replaces the notice quietly when the hold returns, and withdraws it when the
// hold or the request ends. This header holds only what the app does with them:
// the words, the buttons, and what the tray does for each of the tracker's
// actions. Pure, with no WinRT or sdk header, so tools/client-limit-notice-tests.cpp
// runs it on any host.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace urnw::clientlimit {

// The sdk's spellings (sdk client_limit_status.go), here so this header needs no
// sdk header; tests/client_limit_notice_test.go holds them to urnetwork_sdk.hpp.
inline constexpr const char* kStatusExceeded = "client_limit_exceeded";
inline constexpr const char* kPromptUpgrade = "upgrade";
inline constexpr const char* kPromptRemoveClient = "remove_client";
inline constexpr const char* kNoticePost = "post";
inline constexpr const char* kNoticeUpdate = "update";
inline constexpr const char* kNoticeRemove = "remove";

// One readout of the session device's ClientLimitStatus in plain values, so the
// feed pushes only a change and the window never sees an sdk type.
struct Reading {
  // ClientLimitStatusExceeded while the device is held, "" otherwise.
  std::string status;
  // When the hold ends and the device dials again, in unix epoch milliseconds.
  int64_t retryTimeMillis = 0;
  // ClientLimitPromptUpgrade or ClientLimitPromptRemoveClient while held.
  std::string prompt;

  bool operator==(const Reading&) const = default;
};

// The device is held off the platform.
inline bool Held(const std::optional<Reading>& reading) {
  return reading.has_value() && reading->status == kStatusExceeded;
}

// What the connect screen's notice shows. Present exactly while the device is
// held and a connection is requested, the same two facts the sdk's tracker
// posts the tray's notice on.
struct Notice {
  // UR Pro lifts the limit: Get Pro beside Retry. Otherwise the plan already
  // has its most devices, and no upgrade is offered.
  bool offersUpgrade = true;

  bool operator==(const Notice&) const = default;
};

inline std::optional<Notice> NoticeFor(const std::optional<Reading>& reading,
                                       bool connectRequested) {
  if (!connectRequested || !Held(reading)) return std::nullopt;
  // a device process too old to send a prompt reads as free, as on apple
  return Notice{reading->prompt != kPromptRemoveClient};
}

// The words: the title, and what to do for the plan. The same body answers a
// sign-in the limit refused.
inline constexpr const char* kTitleKey = "client_limit_reached";
inline const char* MessageKey(bool offersUpgrade) {
  return offersUpgrade ? "client_limit_upgrade_message" : "client_limit_remove_client_message";
}

// What the tray does for one of the tracker's actions. The tray's notice is for
// the user who is not looking at the window: with the window in front, its
// notice says it, and a post or an update shows nothing more. A withdrawal
// always goes through, so no notice outlives the hold.
enum class TrayStep {
  None,
  // show it and alert (a sound)
  Alert,
  // show it, or replace the one showing, with no sound
  Quiet,
  // take it down
  Withdraw,
};

inline TrayStep TrayStepFor(const std::string& action, bool inBackground) {
  if (action == kNoticeRemove) return TrayStep::Withdraw;
  if (!inBackground) return TrayStep::None;
  if (action == kNoticePost) return TrayStep::Alert;
  if (action == kNoticeUpdate) return TrayStep::Quiet;
  return TrayStep::None;
}

// The plan turned Pro in this run (a balance read before it, not Pro, and one
// after it, Pro) while the device is held: an upgrade, which the hold's prompt
// offered or was about to stop offering, so the device dials again now rather
// than at its retry time. Never on the first read of a launch: an account that
// is already Pro and held at the Pro limit gains nothing from a refused dial.
inline bool UpgradeEndsHold(bool wasLoaded, bool wasPro, bool isLoaded, bool isPro, bool held) {
  return held && wasLoaded && !wasPro && isLoaded && isPro;
}

// A sign-in the limit refused (AuthNetworkClientError.ClientLimitPrompt): the
// token the registration hands the sign-in page in place of the server's
// message, and the body the page shows for it. Any other refusal keeps the
// server's message.
inline constexpr const char* kSignInRefusedUpgrade = "client-limit:upgrade";
inline constexpr const char* kSignInRefusedRemoveClient = "client-limit:remove-client";

inline std::optional<std::string> SignInRefusalToken(const std::optional<std::string>& prompt) {
  if (!prompt) return std::nullopt;
  if (*prompt == kPromptUpgrade) return std::string(kSignInRefusedUpgrade);
  if (*prompt == kPromptRemoveClient) return std::string(kSignInRefusedRemoveClient);
  return std::nullopt;
}

// The body for a refusal token, or nullptr for any other error.
inline const char* SignInRefusalMessageKey(const std::string& error) {
  if (error == kSignInRefusedUpgrade) return MessageKey(true);
  if (error == kSignInRefusedRemoveClient) return MessageKey(false);
  return nullptr;
}

}  // namespace urnw::clientlimit
