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

class CryptoInvoiceStore;
class SdkHost;
class SubscriptionBalanceStore;
class UpdateChecker;

namespace pages {

// The process-wide SDK host, balance store, crypto invoice and update checker,
// through AppController. Declared here and defined in PageContext.cpp so this
// header does not have to pull in AppController.h (which reaches TrayIcon and
// the whole SDK surface).
SdkHost& Sdk();
SubscriptionBalanceStore& Balance();
CryptoInvoiceStore& CryptoInvoice();
UpdateChecker& Updates();
// Settings > General's "Notify me when my internet is slow": the tray's
// preference, kept by AppController (SlowInternetNotify).
bool SlowInternetNotify();
void SetSlowInternetNotify(bool on);
// The one-tap Bittensor key's one reminder, as the tray's notification, whose
// click opens Earnings with the Key screen (AppController). True once it went
// out.
bool PostBittensorKeyReminder();

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

// A label with an English fallback: the store's string for `key`, or `english`
// when the catalog lacks the id.
//
// Read this before adding a call.
//
// The rule for this app is that every user-facing string comes from the shared
// localization store (@urnetwork/localizations) through Loc/LocBox/Format/Plural.
// Adv() was the exception for surfaces the store did not cover yet: the
// developer screen (Dev() in DeveloperPage.cpp), Advanced Mode's inspector and
// status strip (adv_), the service banner and uninstall row (svc_), the update
// banner and Settings toggle (upd_), the connection-health lines and tray
// (conn_) and the transport names (transport_). Their ids are store keys now,
// and tests/catalog_lookup_test.go fails on any Adv, Dev, Loc or Format id the
// generated catalog lacks, so a new label lands with its key in the same change
// and uses Loc/Format. Adv() stays at the existing call sites.
//
// Do not put a bare literal in the UI instead.
winrt::hstring Adv(std::string_view key, const wchar_t* english);
// The same, as a std::wstring, for the places that compose text.
std::wstring AdvW(std::string_view key, const wchar_t* english);

std::string TrimWhitespace(std::string const& value);

// a user auth is an email or a phone number (light shape check; the server is
// the real validator — macOS ValidationUtils parity in spirit)
bool LooksLikeUserAuth(std::string const& value);

// `text` in capitals for a chip or a kicker, cased for the user's language
// (LCMapStringEx with linguistic casing). std::towupper maps ASCII only in the C
// runtime's default "C" locale, which left the Russian, Ukrainian and Greek
// chips in lower case.
winrt::hstring Upper(winrt::hstring const& text);

}  // namespace pages
}  // namespace urnw
