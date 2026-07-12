// SPDX-License-Identifier: MPL-2.0
// the project compiles with /Yu"pch.h" (App.vcxproj), so every translation unit
// must include it first
#include "pch.h"

#include "TrayIcon.h"

#include <windowsx.h>  // GET_X_LPARAM / GET_Y_LPARAM

#include "Ids.h"
#include "Localization.h"
#include "Log.h"
#include "resource.h"

namespace urnw {
namespace {

constexpr UINT kTrayCallbackMsg = WM_APP + 1;
constexpr UINT kMenuOpen = 1;
constexpr UINT kMenuConnect = 2;
constexpr UINT kMenuQuit = 3;
constexpr wchar_t kWindowClass[] = L"URnetworkTrayWindow";

// Read the taskbar theme: SystemUsesLightTheme == 0 => dark taskbar.
bool IsDarkTaskbar() {
  HKEY key;
  if (::RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                      0, KEY_READ, &key) != ERROR_SUCCESS)
    return false;
  DWORD value = 1, size = sizeof(value), type = 0;
  ::RegQueryValueExW(key, L"SystemUsesLightTheme", nullptr, &type,
                     reinterpret_cast<BYTE*>(&value), &size);
  ::RegCloseKey(key);
  return value == 0;
}

}  // namespace

bool TrayIcon::Create(HINSTANCE instance, Callbacks callbacks) {
  instance_ = instance;
  cb_ = std::move(callbacks);
  darkTaskbar_ = IsDarkTaskbar();
  wmTaskbarCreated_ = ::RegisterWindowMessageW(L"TaskbarCreated");

  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = &TrayIcon::WndProc;
  wc.hInstance = instance;
  wc.lpszClassName = kWindowClass;
  ::RegisterClassExW(&wc);

  // A hidden top-level window receives the tray callback + broadcast messages
  // (a message-only window would miss TaskbarCreated). Its title is never shown,
  // so it stays an internal identifier.
  hwnd_ = ::CreateWindowExW(0, kWindowClass, L"URnetwork", 0, 0, 0, 0, 0, nullptr,
                            nullptr, instance, this);
  if (!hwnd_) {
    LogError("tray: CreateWindow failed: {}", ::GetLastError());
    return false;
  }

  AddIcon();
  LogInfo("tray: icon created");
  return true;
}

void TrayIcon::AddIcon() {
  NOTIFYICONDATAW nid{};
  nid.cbSize = sizeof(nid);
  nid.hWnd = hwnd_;
  nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_SHOWTIP | NIF_GUID;
  nid.guidItem = ids::kTrayIconGuid;
  nid.uCallbackMessage = kTrayCallbackMsg;
  nid.hIcon = CurrentIcon();
  wcsncpy_s(nid.szTip, Localized("app_name").c_str(), _TRUNCATE);
  // If a stale registration from a previous run lingers (same GUID), clear it.
  ::Shell_NotifyIconW(NIM_DELETE, &nid);
  if (!::Shell_NotifyIconW(NIM_ADD, &nid)) {
    LogError("tray: Shell_NotifyIcon(ADD) failed: {}", ::GetLastError());
    return;
  }
  nid.uVersion = NOTIFYICON_VERSION_4;
  ::Shell_NotifyIconW(NIM_SETVERSION, &nid);
}

void TrayIcon::Destroy() {
  if (!hwnd_) return;
  NOTIFYICONDATAW nid{};
  nid.cbSize = sizeof(nid);
  nid.uFlags = NIF_GUID;
  nid.guidItem = ids::kTrayIconGuid;
  ::Shell_NotifyIconW(NIM_DELETE, &nid);
  ::DestroyWindow(hwnd_);
  hwnd_ = nullptr;
}

void TrayIcon::SetState(TrayState state) {
  state_ = state;
  UpdateIcon();
}

void TrayIcon::SetTooltip(const std::wstring& tip) {
  NOTIFYICONDATAW nid{};
  nid.cbSize = sizeof(nid);
  nid.uFlags = NIF_TIP | NIF_SHOWTIP | NIF_GUID;
  nid.guidItem = ids::kTrayIconGuid;
  wcsncpy_s(nid.szTip, tip.c_str(), _TRUNCATE);
  ::Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void TrayIcon::ShowBalloon(const std::wstring& title, const std::wstring& text) {
  NOTIFYICONDATAW nid{};
  nid.cbSize = sizeof(nid);
  nid.uFlags = NIF_INFO | NIF_GUID;
  nid.guidItem = ids::kTrayIconGuid;
  wcsncpy_s(nid.szInfoTitle, title.c_str(), _TRUNCATE);
  wcsncpy_s(nid.szInfo, text.c_str(), _TRUNCATE);
  nid.dwInfoFlags = NIIF_USER;
  ::Shell_NotifyIconW(NIM_MODIFY, &nid);
}

HICON TrayIcon::CurrentIcon() {
  // Resource ids: light and dark variants of each of the four states.
  int base = darkTaskbar_ ? IDI_TRAY_DARK_BASE : IDI_TRAY_LIGHT_BASE;
  int id = base + static_cast<int>(state_);
  return ::LoadIconW(instance_, MAKEINTRESOURCEW(id));
}

void TrayIcon::UpdateIcon() {
  NOTIFYICONDATAW nid{};
  nid.cbSize = sizeof(nid);
  nid.uFlags = NIF_ICON | NIF_GUID;
  nid.guidItem = ids::kTrayIconGuid;
  nid.hIcon = CurrentIcon();
  ::Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void TrayIcon::OnThemeChanged() {
  bool dark = IsDarkTaskbar();
  if (dark != darkTaskbar_) {
    darkTaskbar_ = dark;
    UpdateIcon();
  }
}

void TrayIcon::ShowContextMenu(POINT pt) {
  // AppendMenuW copies the item text, so the temporaries are fine.
  HMENU menu = ::CreatePopupMenu();
  ::AppendMenuW(menu, MF_STRING, kMenuOpen, Localized("open_urnetwork").c_str());
  bool connected = cb_.isConnected && cb_.isConnected();
  ::AppendMenuW(menu, MF_STRING, kMenuConnect,
                Localized(connected ? "disconnect" : "connect").c_str());
  ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  ::AppendMenuW(menu, MF_STRING, kMenuQuit, Localized("quit_urnetwork").c_str());

  // Required so the menu dismisses correctly when focus is lost.
  ::SetForegroundWindow(hwnd_);
  UINT cmd = ::TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD, pt.x, pt.y, 0,
                              hwnd_, nullptr);
  ::DestroyMenu(menu);

  switch (cmd) {
    case kMenuOpen: if (cb_.onShowWindow) cb_.onShowWindow(); break;
    case kMenuConnect: if (cb_.onConnectToggle) cb_.onConnectToggle(); break;
    case kMenuQuit: if (cb_.onQuit) cb_.onQuit(); break;
  }
}

LRESULT CALLBACK TrayIcon::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                        reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
  }
  auto* self = reinterpret_cast<TrayIcon*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (!self) return ::DefWindowProcW(hwnd, msg, wParam, lParam);

  if (msg == self->wmTaskbarCreated_ && self->wmTaskbarCreated_ != 0) {
    self->AddIcon();  // Explorer restarted; re-add the icon
    return 0;
  }

  switch (msg) {
    case kTrayCallbackMsg: {
      // With NOTIFYICON_VERSION_4 the event is LOWORD(lParam) and the anchor is
      // in (x, y) = GET_X/Y_LPARAM(wParam).
      const WORD event = LOWORD(lParam);
      POINT anchor{GET_X_LPARAM(wParam), GET_Y_LPARAM(wParam)};
      if (event == NIN_SELECT || event == NIN_KEYSELECT) {
        if (self->cb_.onLeftClick) self->cb_.onLeftClick(anchor);
      } else if (event == WM_CONTEXTMENU) {
        self->ShowContextMenu(anchor);
      }
      return 0;
    }
    case WM_SETTINGCHANGE:
      if (lParam && wcscmp(reinterpret_cast<const wchar_t*>(lParam), L"ImmersiveColorSet") == 0)
        self->OnThemeChanged();
      return 0;
    default:
      return ::DefWindowProcW(hwnd, msg, wParam, lParam);
  }
}

}  // namespace urnw
