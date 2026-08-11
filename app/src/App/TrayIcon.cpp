// SPDX-License-Identifier: MPL-2.0
// the project compiles with /Yu"pch.h" (App.vcxproj), so every translation unit
// must include it first
#include "pch.h"

#include "TrayIcon.h"

#include <windowsx.h>  // GET_X_LPARAM / GET_Y_LPARAM

#include <exception>

#include "Ids.h"
#include "Localization.h"
#include "Log.h"
#include "PageContext.h"  // pages::AdvW — the two recovery items' labels
#include "Startup.h"  // FailVisible, at the window-procedure boundary
#include "Strings.h"
#include "resource.h"

namespace urnw {
namespace {

constexpr UINT kTrayCallbackMsg = WM_APP + 1;
// Icon id for the fallback (non-GUID) identity — see TrayIcon::useGuid_. Any
// value works; it is only unique within our own window.
constexpr UINT kTrayIconId = 1;
constexpr UINT kMenuOpen = 1;
constexpr UINT kMenuConnect = 2;
constexpr UINT kMenuQuit = 3;
constexpr UINT kMenuStopTunnel = 4;
constexpr UINT kMenuLiftKillSwitch = 5;
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

TrayIcon::~TrayIcon() { Destroy(); }

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
  if (!::RegisterClassExW(&wc)) {
    const DWORD err = ::GetLastError();
    // Already registered is the normal second call; anything else means no
    // window, which means no icon and no app.
    if (err != ERROR_CLASS_ALREADY_EXISTS) {
      LogError("tray: RegisterClass failed: {}", err);
      return false;
    }
  }

  // A hidden top-level window receives the tray callback + broadcast messages
  // (a message-only window would miss TaskbarCreated). Its title is never shown,
  // so it stays an internal identifier.
  hwnd_ = ::CreateWindowExW(0, kWindowClass, L"URnetwork", 0, 0, 0, 0, 0, nullptr,
                            nullptr, instance, this);
  if (!hwnd_) {
    LogError("tray: CreateWindow failed: {}", ::GetLastError());
    return false;
  }

  return AddIcon();
}

void TrayIcon::FillIdentity(NOTIFYICONDATAW& nid) const {
  nid.hWnd = hwnd_;
  if (useGuid_) {
    nid.uFlags |= NIF_GUID;
    nid.guidItem = ids::kTrayIconGuid;
  } else {
    nid.uID = kTrayIconId;
  }
}

bool TrayIcon::AddIcon() {
  // Two attempts at most: the stable GUID first, then the classic hwnd+uID.
  for (;;) {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_SHOWTIP;
    nid.uCallbackMessage = kTrayCallbackMsg;
    nid.hIcon = CurrentIcon();
    wcsncpy_s(nid.szTip, Localized("app_name").c_str(), _TRUNCATE);
    FillIdentity(nid);

    // If a stale registration from a previous run lingers, clear it.
    ::Shell_NotifyIconW(NIM_DELETE, &nid);
    if (::Shell_NotifyIconW(NIM_ADD, &nid)) {
      nid.uVersion = NOTIFYICON_VERSION_4;
      if (!::Shell_NotifyIconW(NIM_SETVERSION, &nid)) {
        // NOT cosmetic. Without NOTIFYICON_VERSION_4 the shell sends the v0
        // callback convention — lParam is the mouse message and wParam is the
        // icon id — so WndProc's LOWORD(lParam) test never matches and the
        // anchor read out of wParam is garbage. Both clicks would be dead: an
        // icon that is visibly there, does nothing, and offers no way into or
        // out of the app. Reporting that as "icon added" would be the same lie
        // this work package exists to remove, so take the icon back down and
        // fail.
        LogError("tray: Shell_NotifyIcon(SETVERSION) failed — the icon would not "
                 "respond to clicks; removing it");
        ::Shell_NotifyIconW(NIM_DELETE, &nid);
        return false;
      }
      LogInfo("tray: icon added ({} identity)", useGuid_ ? "guid" : "hwnd+id");
      return true;
    }

    // Shell_NotifyIcon does not document setting the last error, so the code is
    // a hint, not a diagnosis.
    const DWORD err = ::GetLastError();
    if (useGuid_) {
      // Expected whenever the exe has moved since the GUID was first
      // registered (a build copied out of the output dir, then installed by the
      // MSI). Not fatal — the icon just loses the user's show/hide preference.
      LogWarn("tray: Shell_NotifyIcon(ADD) with the stable guid failed (last error {}); "
              "retrying with an hwnd+id identity", err);
      useGuid_ = false;
      continue;
    }
    LogError("tray: Shell_NotifyIcon(ADD) failed (last error {}) — there is no icon "
             "in the notification area", err);
    return false;
  }
}

void TrayIcon::Destroy() {
  if (!hwnd_) return;
  NOTIFYICONDATAW nid{};
  nid.cbSize = sizeof(nid);
  FillIdentity(nid);
  if (!::Shell_NotifyIconW(NIM_DELETE, &nid))
    LogWarn("tray: Shell_NotifyIcon(DELETE) failed — the icon may linger until hover");
  // Cut the back-pointer BEFORE destroying the window: DestroyWindow dispatches
  // WM_DESTROY/WM_NCDESTROY synchronously, and anything still queued for this
  // window must find a null `self` and fall through to DefWindowProc rather
  // than call into an object that is going away.
  ::SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
  ::DestroyWindow(hwnd_);
  hwnd_ = nullptr;
  LogInfo("tray: icon removed");
}

void TrayIcon::SetState(TrayState state) {
  state_ = state;
  UpdateIcon();
}

void TrayIcon::SetTooltip(const std::wstring& tip) {
  NOTIFYICONDATAW nid{};
  nid.cbSize = sizeof(nid);
  nid.uFlags = NIF_TIP | NIF_SHOWTIP;
  FillIdentity(nid);
  wcsncpy_s(nid.szTip, tip.c_str(), _TRUNCATE);
  if (!::Shell_NotifyIconW(NIM_MODIFY, &nid))
    LogDebug("tray: Shell_NotifyIcon(MODIFY tip) failed");
}

void TrayIcon::ShowBalloon(const std::wstring& title, const std::wstring& text) {
  NOTIFYICONDATAW nid{};
  nid.cbSize = sizeof(nid);
  nid.uFlags = NIF_INFO;
  FillIdentity(nid);
  wcsncpy_s(nid.szInfoTitle, title.c_str(), _TRUNCATE);
  wcsncpy_s(nid.szInfo, text.c_str(), _TRUNCATE);
  nid.dwInfoFlags = NIIF_USER;
  if (!::Shell_NotifyIconW(NIM_MODIFY, &nid))
    LogDebug("tray: Shell_NotifyIcon(MODIFY balloon) failed");
}

HICON TrayIcon::CurrentIcon() {
  // Resource ids: light and dark variants of each of the four states.
  const int base = darkTaskbar_ ? IDI_TRAY_DARK_BASE : IDI_TRAY_LIGHT_BASE;
  const int id = base + static_cast<int>(state_);
  // LoadImage at the small-icon metric, not LoadIcon: the notification area is
  // a SM_CXSMICON slot, and LoadIcon hands back the large image for the shell to
  // downscale, which is the usual cause of a fuzzy tray icon (and is wrong again
  // at every non-96dpi scale). LR_SHARED keeps the module's own shared handle —
  // the icon must stay valid for as long as it is displayed, and a per-call
  // handle here would leak one icon per state change.
  HICON icon = static_cast<HICON>(::LoadImageW(instance_, MAKEINTRESOURCEW(id), IMAGE_ICON,
                                               ::GetSystemMetrics(SM_CXSMICON),
                                               ::GetSystemMetrics(SM_CYSMICON),
                                               LR_DEFAULTCOLOR | LR_SHARED));
  if (!icon) {
    // The icon resources are compiled into the exe (App.rc), so this means the
    // binary is not the one it was built as. Without an icon the shell shows a
    // blank slot, which reads as "nothing happened".
    LogError("tray: icon resource {} could not be loaded: {}", id, ::GetLastError());
  }
  return icon;
}

void TrayIcon::UpdateIcon() {
  NOTIFYICONDATAW nid{};
  nid.cbSize = sizeof(nid);
  nid.uFlags = NIF_ICON;
  FillIdentity(nid);
  nid.hIcon = CurrentIcon();
  if (!::Shell_NotifyIconW(NIM_MODIFY, &nid))
    LogDebug("tray: Shell_NotifyIcon(MODIFY icon) failed — the icon may be stale");
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

  // The escapes, each shown only while it is the answer to something. See the
  // note on Callbacks: this menu is the only surface that exists when the window
  // does not, so it is where a machine blocked by a tunnel has to be reachable
  // from. Separated from the everyday items above so they read as recovery
  // rather than as more settings.
  const bool stoppable = cb_.canStopTunnel && cb_.canStopTunnel();
  const bool liftable = cb_.canLiftKillSwitch && cb_.canLiftKillSwitch();
  if (stoppable || liftable) ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  if (stoppable) {
    // NOT the word "Disconnect": that is the item above, and it means something
    // narrower (stop connecting). This one takes the whole tunnel down —
    // routes, DNS and the firewall policy — which is what someone with no
    // internet is actually looking for.
    ::AppendMenuW(menu, MF_STRING, kMenuStopTunnel,
                  pages::AdvW("conn_tray_turn_tunnel_off",
                              L"Turn the tunnel off (restore my internet)")
                      .c_str());
  }
  if (liftable) {
    ::AppendMenuW(menu, MF_STRING, kMenuLiftKillSwitch,
                  pages::AdvW("conn_tray_lift_kill_switch",
                              L"Turn off the kill switch (unblock this machine)")
                      .c_str());
  }

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
    case kMenuStopTunnel: if (cb_.onStopTunnel) cb_.onStopTunnel(); break;
    case kMenuLiftKillSwitch:
      if (cb_.onLiftKillSwitch) cb_.onLiftKillSwitch();
      break;
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

  // Everything below runs the app's own callbacks — opening the window, talking
  // to the SDK, quitting — and any of them can throw. Unwinding a C++ exception
  // out of a window procedure is unsupported by Windows (the exception crosses
  // the kernel's dispatch frame), so the outcome is a crash with no explanation
  // rather than the error. Catch it here, at the boundary, and say what
  // happened.
  try {
    if (msg == self->wmTaskbarCreated_ && self->wmTaskbarCreated_ != 0) {
      LogInfo("tray: TaskbarCreated — re-adding the icon");
      // Try the stable GUID identity again: the fallback may have been taken
      // because Explorer was mid-restart, and staying on hwnd+uID forever would
      // permanently lose the user's "always show this icon" pin.
      self->useGuid_ = true;
      self->AddIcon();  // Explorer restarted; re-add the icon
      return 0;
    }

    switch (msg) {
      case kTrayCallbackMsg: {
        // With NOTIFYICON_VERSION_4 the event is LOWORD(lParam) and the anchor is
        // in (x, y) = GET_X/Y_LPARAM(wParam). AddIcon fails the whole icon if
        // that version could not be set, so this convention is the only one that
        // can reach here.
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
  } catch (const std::exception& e) {
    LogError("tray: exception from a tray action (msg {}): {}", msg, e.what());
    FailVisible(L"URnetwork could not complete that action.", Widen(e.what()));
    return 0;
  } catch (...) {
    LogError("tray: unknown exception from a tray action (msg {})", msg);
    FailVisible(L"URnetwork could not complete that action.",
                L"An unknown exception reached the tray icon's window procedure.");
    return 0;
  }
}

}  // namespace urnw
