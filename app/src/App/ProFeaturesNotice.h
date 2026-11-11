// The network's Pro features in use, as the app shows them (owner, 2026-11-11):
// SOCKS and WireGuard hosted proxies are part of Pro (pro.yml), and a network
// without Pro whose proxies hold them is told so before the server's
// enforce_features switch turns on, and what stopped once it is on. Nothing
// disconnects and nothing holds a connect back: HTTP and HTTPS proxies keep
// working, and so does this device.
//
// What to say and when the main window says it are the sdk's decisions, the
// same on every platform: ProFeaturesStatus.Prompt (pro_features_status.go),
// which the device list shows from its snapshot's ProFeatures, and the
// ProFeaturesViewController's MainPrompt, which the main window's notice shows.
// The controller reads GET /network/proxies at its start, hourly and at each
// refresh, alerts once per sign-in and holds a dismissal while the prompt stays
// the same; AppController opens one per sign-in. This header holds only what the
// app does with them: the words for a prompt, whether the device list offers
// Get Pro, and when a balance read is the purchase that reads the proxies again.
// Pure, with no WinRT or sdk header, so tools/pro-features-notice-tests.cpp runs
// it on any host.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <optional>
#include <string>

namespace urnw::profeatures {

// The sdk's spellings (sdk pro_features_status.go), here so this header needs no
// sdk header; tests/pro_features_notice_test.go holds them to urnetwork_sdk.hpp.
inline constexpr const char* kPromptNone = "";
inline constexpr const char* kPromptUpgrade = "upgrade";
inline constexpr const char* kPromptStopped = "stopped";

// The words: a title and a body for each prompt, each looked up here and
// nowhere else. Before enforcement, SOCKS and WireGuard are part of Pro and an
// upgrade keeps them working; after it, they stopped and an upgrade turns them
// back on. The buttons beside them are the store's get_pro, manage_devices and
// close, as the client limit notice's are.
inline constexpr const char* kUpgradeTitleKey = "pro_features_in_use";
inline constexpr const char* kUpgradeMessageKey = "pro_features_upgrade_message";
inline constexpr const char* kStoppedTitleKey = "pro_features_stopped";
inline constexpr const char* kStoppedMessageKey = "pro_features_stopped_message";

struct Words {
  const char* titleKey = nullptr;
  const char* messageKey = nullptr;
};

// nullopt for no prompt, and for a prompt this app has no words for: a notice
// it cannot word does not show.
inline std::optional<Words> WordsFor(const std::string& prompt) {
  if (prompt == kPromptUpgrade) return Words{kUpgradeTitleKey, kUpgradeMessageKey};
  if (prompt == kPromptStopped) return Words{kStoppedTitleKey, kStoppedMessageKey};
  return std::nullopt;
}

// The notice above the device list, from its snapshot's ProFeatures.Prompt. It
// has no close control: the list shows the prompt whatever the main window's
// notice does. Get Pro is offered unless the list is the one of a sign-in the
// client limit refused, which has no signed-in account to buy with (that list
// offers no Get Pro for the limit either).
struct ListNotice {
  Words words;
  bool offersUpgrade = true;
};

inline std::optional<ListNotice> ListNoticeFor(const std::string& prompt, bool forSignIn) {
  const std::optional<Words> words = WordsFor(prompt);
  if (!words) return std::nullopt;
  return ListNotice{*words, !forSignIn};
}

// The plan turned Pro in this sign-in (a balance read before it, not Pro, and
// one after it, Pro): a purchase, after which the controller reads the proxies
// again, so the notice goes once the server reads the plan as Pro rather than
// at the next hourly read. Never on the first read of a sign-in: the controller
// read them when it started.
inline bool PlanTurnedPro(bool wasLoaded, bool wasPro, bool isLoaded, bool isPro) {
  return wasLoaded && !wasPro && isLoaded && isPro;
}

}  // namespace urnw::profeatures
