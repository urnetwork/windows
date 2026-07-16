// Best-effort enumeration of installed desktop apps for the split-tunnel picker.
//
// The WFP driver matches on the full exe IMAGE PATH (case-insensitive), so we want
// each app's real .exe path. We read the registry Uninstall keys (HKLM + HKCU, both
// the 64- and 32-bit views) and derive the .exe from DisplayIcon / InstallLocation.
// This is intentionally best-effort: Store/MSIX apps have no stable classic image
// path and are skipped, and some classic entries won't resolve to an .exe - the
// manual file picker in the sheet remains the fallback for anything not listed.
//
// Header-only (inline) so no new .cpp needs wiring into the App project.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "Strings.h"  // urnw::Narrow / urnw::Widen

namespace urnw {

struct InstalledApp {
  std::string name;     // display name
  std::string exePath;  // full exe image path (what the driver matches)
};

namespace detail {

inline std::wstring RegString(HKEY key, const wchar_t* name) {
  DWORD type = 0, bytes = 0;
  if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS)
    return {};
  if (type != REG_SZ && type != REG_EXPAND_SZ) return {};
  std::wstring buf(bytes / sizeof(wchar_t) + 1, L'\0');
  if (RegQueryValueExW(key, name, nullptr, nullptr,
                       reinterpret_cast<BYTE*>(buf.data()), &bytes) != ERROR_SUCCESS)
    return {};
  buf.resize(wcslen(buf.c_str()));
  if (type == REG_EXPAND_SZ) {
    DWORD n = ExpandEnvironmentStringsW(buf.c_str(), nullptr, 0);
    if (n) {
      std::wstring expanded(n, L'\0');
      ExpandEnvironmentStringsW(buf.c_str(), expanded.data(), n);
      expanded.resize(n ? n - 1 : 0);
      return expanded;
    }
  }
  return buf;
}

// Pull an .exe path out of a DisplayIcon value ("C:\\path\\app.exe,0" or quoted).
inline std::wstring ExeFromDisplayIcon(std::wstring icon) {
  if (icon.empty()) return {};
  if (icon.front() == L'"') {
    auto close = icon.find(L'"', 1);
    icon = (close == std::wstring::npos) ? icon.substr(1) : icon.substr(1, close - 1);
  } else if (auto comma = icon.rfind(L','); comma != std::wstring::npos) {
    // strip a trailing ",<index>" (icon index), but only if it looks numeric
    std::wstring tail = icon.substr(comma + 1);
    bool numeric = !tail.empty() &&
                   std::all_of(tail.begin(), tail.end(),
                               [](wchar_t c) { return c == L'-' || iswdigit(c); });
    if (numeric) icon = icon.substr(0, comma);
  }
  // trim whitespace
  while (!icon.empty() && iswspace(icon.back())) icon.pop_back();
  size_t start = icon.find_first_not_of(L" \t");
  if (start != std::wstring::npos) icon = icon.substr(start);
  if (icon.size() < 4) return {};
  std::wstring lower = icon;
  std::transform(lower.begin(), lower.end(), lower.begin(), towlower);
  if (lower.size() < 4 || lower.compare(lower.size() - 4, 4, L".exe") != 0) return {};
  return icon;
}

inline void ScanUninstallKey(HKEY root, REGSAM view, std::vector<InstalledApp>& out) {
  HKEY base;
  if (RegOpenKeyExW(root, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
                    0, KEY_READ | view, &base) != ERROR_SUCCESS)
    return;
  for (DWORD i = 0;; ++i) {
    wchar_t sub[256];
    DWORD subLen = 256;
    LONG r = RegEnumKeyExW(base, i, sub, &subLen, nullptr, nullptr, nullptr, nullptr);
    if (r == ERROR_NO_MORE_ITEMS) break;
    if (r != ERROR_SUCCESS) continue;
    HKEY app;
    if (RegOpenKeyExW(base, sub, 0, KEY_READ | view, &app) != ERROR_SUCCESS) continue;
    std::wstring name = RegString(app, L"DisplayName");
    // skip system components / updates / entries without a user-facing name
    DWORD sysComponent = 0, bytes = sizeof(sysComponent), type = 0;
    RegQueryValueExW(app, L"SystemComponent", nullptr, &type,
                     reinterpret_cast<BYTE*>(&sysComponent), &bytes);
    std::wstring exe = ExeFromDisplayIcon(RegString(app, L"DisplayIcon"));
    if (exe.empty()) {
      // fall back to InstallLocation + DisplayName.exe if plausible (weak; skipped
      // when it doesn't resolve, the picker covers it)
      exe.clear();
    }
    RegCloseKey(app);
    if (name.empty() || exe.empty() || sysComponent) continue;
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
    out.push_back({Narrow(name), Narrow(exe)});
  }
  RegCloseKey(base);
}

inline std::string SelfExePathLower() {
  wchar_t buf[MAX_PATH];
  DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
  std::wstring w(buf, n);
  std::transform(w.begin(), w.end(), w.begin(), towlower);
  return Narrow(w);
}

}  // namespace detail

// Enumerate installed desktop apps, sorted by name, de-duplicated by exe path,
// excluding this process's own exe. Best-effort (see file header).
inline std::vector<InstalledApp> EnumerateInstalledApps() {
  std::vector<InstalledApp> apps;
  detail::ScanUninstallKey(HKEY_LOCAL_MACHINE, KEY_WOW64_64KEY, apps);
  detail::ScanUninstallKey(HKEY_LOCAL_MACHINE, KEY_WOW64_32KEY, apps);
  detail::ScanUninstallKey(HKEY_CURRENT_USER, KEY_WOW64_64KEY, apps);

  const std::string self = detail::SelfExePathLower();
  auto lower = [](std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
  };
  // de-dup by lowercased exe path + drop self
  std::sort(apps.begin(), apps.end(), [&](const InstalledApp& a, const InstalledApp& b) {
    return lower(a.exePath) < lower(b.exePath);
  });
  apps.erase(std::unique(apps.begin(), apps.end(),
                         [&](const InstalledApp& a, const InstalledApp& b) {
                           return lower(a.exePath) == lower(b.exePath);
                         }),
             apps.end());
  apps.erase(std::remove_if(apps.begin(), apps.end(),
                            [&](const InstalledApp& a) { return lower(a.exePath) == self; }),
             apps.end());
  std::sort(apps.begin(), apps.end(), [](const InstalledApp& a, const InstalledApp& b) {
    return _stricmp(a.name.c_str(), b.name.c_str()) < 0;
  });
  return apps;
}

}  // namespace urnw
