// The handful of helpers every per-page unit needs.
//
// MainWindow.xaml.cpp used to be one 2128-line translation unit whose anonymous
// namespace carried these. The per-page split (LoginPage / ConnectPage /
// AccountPage / WalletPage / SettingsPage) gave each surface its own unit, so
// the genuinely shared helpers live here and the surface-specific ones stayed
// in the anonymous namespace of the page that uses them.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <string>
#include <string_view>

#include <winrt/Windows.Foundation.h>

#include "Localization.h"

namespace winrt::URnetwork::implementation {
struct MainWindow;
}

namespace urnw {

class SdkHost;
class SubscriptionBalanceStore;

namespace pages {

// The process-wide SDK host and balance store, through AppController. Declared
// here and defined in PageContext.cpp so this header does not have to pull in
// AppController.h (which reaches TrayIcon and the whole SDK surface).
SdkHost& Sdk();
SubscriptionBalanceStore& Balance();

inline winrt::hstring H(std::string const& s) { return winrt::to_hstring(s); }

// A UI string from the shared localization store, by key id. Every user-facing
// string in the window comes through Loc/LocBox, urnw::Format or urnw::Plural —
// there are no literals (see MainWindow.xaml).
inline winrt::hstring Loc(std::string_view key) {
  return winrt::hstring{urnw::Localized(key)};
}
inline winrt::Windows::Foundation::IInspectable LocBox(std::string_view key) {
  return winrt::box_value(Loc(key));
}

std::string TrimWhitespace(std::string const& value);

// a user auth is an email or a phone number (light shape check; the server is
// the real validator — macOS ValidationUtils parity in spirit)
bool LooksLikeUserAuth(std::string const& value);

}  // namespace pages
}  // namespace urnw
