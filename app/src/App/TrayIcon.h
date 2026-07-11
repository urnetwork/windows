// System tray icon — the Windows equivalent of the macOS menu-bar item.
// Classic Win32 Shell_NotifyIcon (there is no WinUI tray API): GUID identity +
// NOTIFYICON_VERSION_4, left-click toggles a flyout, right-click shows a menu.
// Four icon states mirror the macOS connect x provide matrix, switched for the
// light/dark taskbar theme.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <functional>
#include <string>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

namespace urnw {

// Connect x provide, matching the four macOS menu-bar assets.
enum class TrayState { NoProvideNoConnect, NoProvideConnect, ProvideNoConnect, ProvideConnect };

class TrayIcon {
 public:
  struct Callbacks {
    std::function<void(POINT anchor)> onLeftClick;   // toggle flyout at anchor
    std::function<void()> onShowWindow;              // menu: Open
    std::function<void()> onConnectToggle;           // menu: Connect/Disconnect
    std::function<bool()> isConnected;               // for the menu item label
    std::function<void()> onQuit;                    // menu: Quit
  };

  bool Create(HINSTANCE instance, Callbacks callbacks);
  void Destroy();

  void SetState(TrayState state);
  void SetTooltip(const std::wstring& tip);
  void ShowBalloon(const std::wstring& title, const std::wstring& text);

  HWND MessageWindow() const { return hwnd_; }

 private:
  static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
  void AddIcon();  // (re)registers the notify icon; also on Explorer restart
  void OnThemeChanged();
  void UpdateIcon();
  void ShowContextMenu(POINT pt);
  HICON CurrentIcon();

  HWND hwnd_ = nullptr;
  HINSTANCE instance_ = nullptr;
  Callbacks cb_;
  TrayState state_ = TrayState::NoProvideNoConnect;
  bool darkTaskbar_ = false;
  UINT wmTaskbarCreated_ = 0;  // re-add the icon if Explorer restarts
};

}  // namespace urnw
