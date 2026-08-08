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

// A label whose store key does NOT EXIST YET.
//
// READ THIS BEFORE ADDING A CALL.
//
// The rule for this app is that every user-facing string comes from the shared
// localization store (@urnetwork/localizations) through Loc/LocBox/Format/Plural,
// and inventing an English literal in the UI breaks it. The one sanctioned
// exception is A SURFACE THE STORE HAS NEVER COVERED. The developer screen has
// been living in that exception since it was built — 88 `dev_*` ids in
// DeveloperPage.cpp, whose header block is the long-form version of this comment
// — and Advanced Mode's inline labels are the same category: 945 keys, and not
// one of them names a field of a connection inspector (`host`, `verdict`,
// `packets`, `via exit`, ...).
//
// So a call carries BOTH the store id the label should have AND the English it
// renders until that id lands, and this prefers the store THE MOMENT the key
// appears — no code change, no second pass. Localized() already returns the key
// id itself on a miss (that is how a typo is made visible) and Plural() already
// uses that same equality as its miss test, so the mechanism is the established
// one here rather than a new one.
//
// Every id introduced this way is reported for the store, and the list extracts
// mechanically rather than living in a doc that would go stale on the next row:
//
//     grep -ohE '"(dev|adv)_[a-z0-9_]+"' app/src/App/*.cpp | sort -u
//
// Do NOT use this for a key that DOES exist — that hides a working translation
// behind an English default. And do not put a bare literal in the UI instead.
winrt::hstring Adv(std::string_view key, const wchar_t* english);
// The same, as a std::wstring, for the places that compose text.
std::wstring AdvW(std::string_view key, const wchar_t* english);

std::string TrimWhitespace(std::string const& value);

// a user auth is an email or a phone number (light shape check; the server is
// the real validator — macOS ValidationUtils parity in spirit)
bool LooksLikeUserAuth(std::string const& value);

}  // namespace pages
}  // namespace urnw
