// The device list's words (ManageDevicesSheet; owner, 2026-11-08: the windows
// app at parity with apple and android). The list is the sdk's
// ManageDevicesViewController (client limit SDK-API.md): every client that takes
// a slot of the network's concurrent client limit, this device first, then the
// connected ones, then by the time last seen, and a removal that frees a slot
// adds this device again (it ends the device's client limit hold, or runs a
// refused sign-in's auth-client again). Which devices there are, their order,
// what can be removed and what happens after are the sdk's. This header holds
// only how a snapshot reads: each row's title and line under it, what the sheet
// says about the list's state, and which keys say it, as the apple app's
// ManageDevicesLabels words them. Pure, with no WinRT or sdk header, so
// tools/manage-devices-tests.cpp runs it on any host; ManageDevicesSheet.cpp
// copies the sdk's snapshot into the plain values below.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace urnw::managedevices {

// The sdk's spellings (sdk manage_devices_view_controller.go), here so this
// header needs no sdk header; tests/manage_devices_test.go holds them to
// urnetwork_sdk.hpp.
inline constexpr const char* kKindProxy = "proxy";
inline constexpr const char* kErrorSignInRequired = "sign_in_required";
inline constexpr const char* kErrorLoadFailed = "load_failed";
inline constexpr const char* kErrorRemoveFailed = "remove_failed";

// One ManagedDevice in plain values.
struct Device {
  std::string clientId;
  // ManagedDeviceKindDevice or ManagedDeviceKindProxy
  std::string kind;
  // the device's name, else its description; may be empty
  std::string name;
  // its system and model; may be empty
  std::string spec;
  bool thisDevice = false;
  bool connected = false;
  // its last sign-in or renewal, unix epoch milliseconds; 0 when unknown
  int64_t lastSeenMillis = 0;
};

// One ManageDevicesSnapshot in plain values.
struct Snapshot {
  std::vector<Device> devices;
  bool loaded = false;
  bool loading = false;
  // the client a removal is in flight for, "" with none
  std::string removingClientId;
  // a ManageDevicesError constant, "" after a success
  std::string errorId;
  // removals that freed a slot and added this device again
  int64_t addRetryCount = 0;
  // the network's Pro features in use, from the same proxies list: its
  // ProFeatures.Prompt, "" for none (ProFeaturesNotice.h words it)
  std::string proFeaturesPrompt;
};

// The words a row and the sheet are made of, from the store (the sheet's Loc,
// Format and RelativeTime), so this header carries no strings of its own.
struct Text {
  std::function<std::string(const char* key)> loc;
  // `key` with its one placeholder filled
  std::function<std::string(const char* key, const std::string& arg)> format;
  // how long ago `unixMillis` was, as the sessions screen says it
  std::function<std::string(int64_t unixMillis)> relative;
};

// The keys, each looked up here and nowhere else.
inline constexpr const char* kTitleKey = "manage_devices";
inline constexpr const char* kHelpKey = "manage_devices_help";
inline constexpr const char* kThisDeviceKey = "manage_devices_this_device";
inline constexpr const char* kProxyKey = "manage_devices_proxy";
inline constexpr const char* kUnknownDeviceKey = "sessions_device_unknown";
inline constexpr const char* kConnectedKey = "connected";
inline constexpr const char* kLastSeenKey = "manage_devices_last_seen";
inline constexpr const char* kConnectingKey = "manage_devices_connecting";
inline constexpr const char* kRemoveTitleKey = "manage_devices_remove_title";
inline constexpr const char* kRemoveBodyKey = "sessions_confirm_body_no_place";
inline constexpr const char* kSignInRequiredKey = "manage_devices_sign_in_required";
inline constexpr const char* kLoadFailedKey = "manage_devices_load_failed";
inline constexpr const char* kRemoveFailedKey = "manage_devices_remove_failed";

inline bool IsProxy(const Device& device) { return device.kind == kKindProxy; }

// The row's first line: the device's name, else what it is.
inline std::string TitleOf(const Device& device, const Text& text) {
  if (!device.name.empty()) return device.name;
  return text.loc(IsProxy(device) ? kProxyKey : kUnknownDeviceKey);
}

// The line under it: "This device · 10.0.26100 x64 · Connected", or
// "Last seen 5m ago". Parts that say nothing are left out.
inline std::string DetailOf(const Device& device, const Text& text) {
  std::vector<std::string> parts;
  if (device.thisDevice) parts.push_back(text.loc(kThisDeviceKey));
  // a proxy with a name says what it is here; without one its title does
  if (IsProxy(device) && !device.name.empty()) parts.push_back(text.loc(kProxyKey));
  if (!device.spec.empty()) parts.push_back(device.spec);
  if (device.connected) {
    parts.push_back(text.loc(kConnectedKey));
  } else if (0 < device.lastSeenMillis) {
    parts.push_back(text.format(kLastSeenKey, text.relative(device.lastSeenMillis)));
  }
  std::string detail;
  for (const std::string& part : parts) {
    if (!detail.empty()) detail += " · ";
    detail += part;
  }
  return detail;
}

// What the sheet shows about the list's state, above the rows.
struct Status {
  // the line under the help: a failure, or "Connecting this device…"; nullptr
  // for none
  const char* lineKey = nullptr;
  // the line is a failure
  bool error = false;
  // Retry (Refresh) beside it: the list failed to load
  bool offersRetry = false;
  // the first load is in flight: a progress ring in the list's place
  bool loading = false;
  // the rows, or none while there is no credential to show them with
  bool showsRows = true;
  // Remove is offered: no removal is in flight (one at a time)
  bool removeEnabled = true;
};

inline Status StatusOf(const Snapshot& snapshot, bool canManage) {
  Status status;
  status.removeEnabled = snapshot.removingClientId.empty();
  if (!canManage || snapshot.errorId == kErrorSignInRequired) {
    // no network credential, or the server refused it: sign in again
    status.lineKey = kSignInRequiredKey;
    status.error = true;
    status.showsRows = false;
    return status;
  }
  if (snapshot.errorId == kErrorLoadFailed) {
    status.lineKey = kLoadFailedKey;
    status.error = true;
    status.offersRetry = true;
  } else if (snapshot.errorId == kErrorRemoveFailed) {
    status.lineKey = kRemoveFailedKey;
    status.error = true;
  } else if (0 < snapshot.addRetryCount) {
    status.lineKey = kConnectingKey;
  }
  status.loading = !snapshot.loaded && snapshot.loading;
  return status;
}

// A row offers Remove: never this device (it signs out instead), and only with
// a client to name.
inline bool Removable(const Device& device) {
  return !device.thisDevice && !device.clientId.empty();
}

}  // namespace urnw::managedevices
