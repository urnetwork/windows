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

    // --- the two escapes, shown only when they are the answer to something ---
    //
    // THE TRAY IS THE ONLY SURFACE THAT ALWAYS EXISTS. Closing the window hides
    // to tray and the tunnel keeps running (the service owns it), so when a
    // tunnel stops carrying traffic the user may have no window to press
    // anything in — which is the "kill the app and my internet stays blocked"
    // half of the owner's report. Both items below therefore have to work with
    // no main window and without opening one.
    //
    // Each is a PAIR: a predicate that decides whether the item appears at all,
    // and the action. An item that is always present but usually inert teaches
    // people to ignore it; an item that appears exactly when it is the fix does
    // not. Both predicates are read at menu-build time, i.e. at the click.

    // "The service still has routes installed" — a tunnel is carrying (or
    // failing to carry) this machine's traffic right now.
    std::function<bool()> canStopTunnel;
    std::function<void()> onStopTunnel;

    // "A firewall policy is in force with no tunnel up" — the kill switch is
    // holding this machine blocked. Turning it off is the one click that lifts
    // it immediately rather than at the next transition.
    std::function<bool()> canLiftKillSwitch;
    std::function<void()> onLiftKillSwitch;
  };

  TrayIcon() = default;
  // The hidden window holds a pointer to this object in GWLP_USERDATA, so the
  // window must never outlive it: a click or a TaskbarCreated broadcast
  // afterwards would re-enter WndProc on freed memory. Not hypothetical — a
  // failure anywhere after Create() shows a MODAL message box, and a modal box
  // pumps the message queue while the owning AppController is being destroyed.
  ~TrayIcon();

  TrayIcon(const TrayIcon&) = delete;
  TrayIcon& operator=(const TrayIcon&) = delete;

  bool Create(HINSTANCE instance, Callbacks callbacks);
  void Destroy();

  void SetState(TrayState state);
  void SetTooltip(const std::wstring& tip);
  void ShowBalloon(const std::wstring& title, const std::wstring& text);

  HWND MessageWindow() const { return hwnd_; }

 private:
  static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
  // (re)registers the notify icon; also on Explorer restart. False means there
  // is no icon — the app has no UI at all then, so the caller must say so.
  bool AddIcon();
  // Fills in whichever identity this icon is registered under (see useGuid_).
  void FillIdentity(NOTIFYICONDATAW& nid) const;
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
  // A NIF_GUID registration is bound to the executable's PATH: run the same
  // build from a different folder and Shell_NotifyIcon(NIM_ADD) fails, leaving
  // the app with no icon and no way in. Cleared when that happens, which falls
  // the whole icon back to the classic hwnd+uID identity.
  bool useGuid_ = true;
};

}  // namespace urnw
