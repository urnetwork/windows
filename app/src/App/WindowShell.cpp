// SPDX-License-Identifier: MPL-2.0
#include "pch.h"

#include "WindowShell.h"

#include <algorithm>
#include <optional>

#include <winrt/Microsoft.UI.Interop.h>
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>

#include "Log.h"
#include "Strings.h"  // Narrow
#include "UrColors.h"

namespace winrtx = winrt::Microsoft::UI::Xaml;
namespace windowing = winrt::Microsoft::UI::Windowing;

namespace urnw::shell {

namespace {

// HKCU\Software\URnetwork\Window, value "Placement": ONE REG_BINARY blob.
//
// It was five DWORDs. Registry writes are atomic per value, not per set, so a
// failure part-way through left three fresh values and one stale that a reader
// would happily accept and restore to a rect that never existed - and the
// cleanup path that defended against it could itself fail, silently. A single
// blob has no such state: the write either replaces the previous good placement
// or leaves it untouched, and a short/garbage/foreign read is rejected whole.
constexpr wchar_t kPlacementKey[] = L"Software\\URnetwork\\Window";
constexpr wchar_t kPlacementValue[] = L"Placement";

#pragma pack(push, 1)
struct StoredPlacement {
  uint32_t magic;    // kPlacementMagic
  uint32_t version;  // kPlacementVersion
  int32_t x;
  int32_t y;
  int32_t width;
  int32_t height;
  // The DPI the SIZE was measured at. Everything here is physical pixels, so
  // without this a user who changes their display scale gets the old physical
  // size reinterpreted at the new one - a window that grows or shrinks by the
  // ratio for no reason they can see. (Moving to a differently-scaled MONITOR
  // is self-correcting: the window manager sends WM_DPICHANGED with a suggested
  // rect. Rescaling the same monitor does not.)
  uint32_t dpi;
};
#pragma pack(pop)

constexpr uint32_t kPlacementMagic = 0x574E5255;  // 'URNW'
constexpr uint32_t kPlacementVersion = 1;

struct Placement {
  int32_t x = 0;
  int32_t y = 0;
  int32_t width = 0;
  int32_t height = 0;
  uint32_t dpi = 0;  // 0 = not recorded, which skips the rescale
};

std::optional<Placement> LoadPlacement() {
  StoredPlacement stored{};
  DWORD size = sizeof(stored);
  DWORD type = 0;
  if (::RegGetValueW(HKEY_CURRENT_USER, kPlacementKey, kPlacementValue,
                     RRF_RT_REG_BINARY, &type, &stored, &size) != ERROR_SUCCESS) {
    return std::nullopt;
  }
  // A blob of the wrong length, magic or version is not ours (or not this
  // version of ours) and is discarded whole rather than partly believed.
  if (size != sizeof(stored) || stored.magic != kPlacementMagic ||
      stored.version != kPlacementVersion) {
    LogInfo("shell: stored placement is not readable by this build - using the default");
    return std::nullopt;
  }
  // A saved size of zero (a minimized or otherwise degenerate window) must not
  // become the size we restore to.
  if (stored.width <= 0 || stored.height <= 0) return std::nullopt;

  Placement p;
  p.x = stored.x;
  p.y = stored.y;
  p.width = stored.width;
  p.height = stored.height;
  p.dpi = stored.dpi;
  return p;
}

double ScaleFor(HWND hwnd) {
  const UINT dpi = ::GetDpiForWindow(hwnd);
  return (dpi == 0 ? 96u : dpi) / 96.0;
}

// Keep the rect on a monitor that exists, and no larger than that monitor's
// work area. Same failure this codebase already paid for once with the tray
// anchor: an unclamped position put the window at (-1136,-875), off every
// screen, with the tray still saying the app was open.
Placement ClampToWorkArea(Placement p) {
  POINT centre{p.x + p.width / 2, p.y + p.height / 2};
  HMONITOR mon = ::MonitorFromPoint(centre, MONITOR_DEFAULTTONEAREST);
  MONITORINFO mi{};
  mi.cbSize = sizeof(mi);
  if (!mon || !::GetMonitorInfoW(mon, &mi)) return p;

  const int workLeft = static_cast<int>(mi.rcWork.left);
  const int workTop = static_cast<int>(mi.rcWork.top);
  const int workRight = static_cast<int>(mi.rcWork.right);
  const int workBottom = static_cast<int>(mi.rcWork.bottom);

  p.width = (std::min)(p.width, workRight - workLeft);
  p.height = (std::min)(p.height, workBottom - workTop);
  // max() after min() so a window larger than the work area still has its
  // TOP-LEFT on screen rather than its bottom-right
  p.x = (std::max)(workLeft, (std::min)(p.x, workRight - p.width));
  p.y = (std::max)(workTop, (std::min)(p.y, workBottom - p.height));
  return p;
}

// The caption buttons are drawn by the SYSTEM on top of our extended content,
// so they do not inherit anything from XAML. Left alone they render on the
// default light-theme plate over a near-black window.
void ApplyCaptionButtonColors(windowing::AppWindow const& appWindow) {
  if (!windowing::AppWindowTitleBar::IsCustomizationSupported()) {
    LogInfo("shell: title bar customization unsupported - system caption colours kept");
    return;
  }
  auto bar = appWindow.TitleBar();
  const auto transparent = winrt::Windows::UI::Colors::Transparent();
  bar.ButtonBackgroundColor(transparent);
  bar.ButtonInactiveBackgroundColor(transparent);
  bar.ButtonForegroundColor(urnw::colors::kText);
  bar.ButtonInactiveForegroundColor(urnw::colors::kTextMuted);
  // the same hover/press steps the card kit uses, so the chrome and the content
  // move together
  bar.ButtonHoverBackgroundColor(urnw::colors::kCardHover);
  bar.ButtonHoverForegroundColor(urnw::colors::kText);
  bar.ButtonPressedBackgroundColor(urnw::colors::kCardPressed);
  bar.ButtonPressedForegroundColor(urnw::colors::kText);
}

}  // namespace

bool ApplyNativeShell(winrtx::Window const& window, HWND hwnd) {
  if (!window || !hwnd) return false;

  // NO MICA. Removed 2026-08-07 after the owner reported the window rendering
  // bright pink-white while focused and black once it lost focus.
  //
  // The two are mutually exclusive here, and the removed code said so without
  // drawing the conclusion: Mica draws BEHIND the XAML content, so the only way
  // to see it was to clear the root grid's opaque #101010 - and once that is
  // cleared, the DESKTOP WALLPAPER is the app's background. Not a tint over the
  // brand colour; its replacement. How bad it looks is a function of what the
  // window happens to be sitting on top of, which is why it survived review:
  // measured here over a dark region the surface read #242428 against a brand
  // #101010 (already wrong, just not obviously), while over a bright region of
  // the same wallpaper it goes near-white and the login form stops being
  // legible. The focus dependence the owner saw is SystemBackdropConfiguration
  // .IsInputActive doing exactly what it is meant to - Mica dims when
  // deactivated, which is why "hover away and it looks normal".
  //
  // This app is a full-bleed fixed-dark brand surface: RequestedTheme is pinned
  // Dark and the palette is byte-correct against the other clients. It has no
  // empty chrome for a backdrop to fill, so Mica could never be decoration
  // here - only a replacement for the brand colour. The native-shell win the
  // owner asked for comes from the sizing, the placement persistence, the real
  // title bar, the caption colours and the control metrics, none of which need
  // a backdrop. So the root keeps its opaque #101010 from App.xaml.
  //
  // Also note the earlier verification gap that let this ship: the session was
  // locked, so only PrintWindow captures were possible, and PrintWindow asks
  // the window to draw ITSELF - it never composites the backdrop. The API-level
  // check (reading FallbackColor back off the controller) passed and proved
  // nothing about the pixels. A backdrop change is only verified by a real
  // screen capture of an ACTIVE window.

  auto windowId = winrt::Microsoft::UI::GetWindowIdFromWindow(hwnd);
  auto appWindow = windowing::AppWindow::GetFromWindowId(windowId);
  if (!appWindow) {
    LogError("shell: no AppWindow for hwnd {} - size and chrome left at their defaults",
             reinterpret_cast<uintptr_t>(hwnd));
    return false;
  }

  ApplyCaptionButtonColors(appWindow);

  const double scale = ScaleFor(hwnd);
  const int defaultW = static_cast<int>(kDefaultWidthDips * scale);
  const int defaultH = static_cast<int>(kDefaultHeightDips * scale);

  if (auto presenter = appWindow.Presenter().try_as<windowing::OverlappedPresenter>()) {
    // resizable, but not down to nothing
    presenter.PreferredMinimumWidth(static_cast<int32_t>(kMinWidthDips * scale));
    presenter.PreferredMinimumHeight(static_cast<int32_t>(kMinHeightDips * scale));
  }

  Placement p;
  bool restored = false;
  if (auto saved = LoadPlacement()) {
    p = *saved;
    // Move to the saved POSITION first, then read the DPI. GetDpiForWindow
    // answers for the monitor the window is currently on, which at this point
    // is wherever it was created - so on a mixed-DPI desktop, reading it before
    // the move rescales by the wrong monitor's scale. Position is unaffected by
    // scale, so moving first is safe and costs one call.
    appWindow.Move({p.x, p.y});
    const UINT dpi = ::GetDpiForWindow(hwnd);
    if (0 < p.dpi && 0 < dpi && p.dpi != dpi) {
      // the display scale changed under a saved SIZE: keep the LOGICAL size
      const double ratio = static_cast<double>(dpi) / p.dpi;
      const int32_t wasW = p.width;
      const int32_t wasH = p.height;
      p.width = static_cast<int32_t>(p.width * ratio);
      p.height = static_cast<int32_t>(p.height * ratio);
      LogInfo("shell: display scale changed since the placement was saved "
              "({} -> {} dpi): {}x{} rescaled to {}x{}",
              p.dpi, dpi, wasW, wasH, p.width, p.height);
    }
    p = ClampToWorkArea(p);
    restored = true;
    LogInfo("shell: restored placement {}x{} at ({},{})", p.width, p.height, p.x, p.y);
  } else {
    // No saved placement: the compact default, CENTRED on the monitor the
    // window was created on. AppWindow.Position() is the un-sized window's
    // top-left, which on this box is the corner - the comment here used to
    // claim centred while the code took that verbatim.
    p.width = defaultW;
    p.height = defaultH;
    const auto origin = appWindow.Position();
    POINT here{origin.X, origin.Y};
    if (HMONITOR mon = ::MonitorFromPoint(here, MONITOR_DEFAULTTOPRIMARY)) {
      MONITORINFO mi{};
      mi.cbSize = sizeof(mi);
      if (::GetMonitorInfoW(mon, &mi)) {
        p.x = static_cast<int32_t>(mi.rcWork.left) +
              (static_cast<int32_t>(mi.rcWork.right - mi.rcWork.left) - p.width) / 2;
        p.y = static_cast<int32_t>(mi.rcWork.top) +
              (static_cast<int32_t>(mi.rcWork.bottom - mi.rcWork.top) - p.height) / 2;
      }
    }
    p = ClampToWorkArea(p);
    LogInfo("shell: no saved placement - compact default {}x{} centred at ({},{}) "
            "(dpi scale {:.2f})",
            p.width, p.height, p.x, p.y, scale);
  }
  appWindow.MoveAndResize({p.x, p.y, p.width, p.height});
  return restored;
}

bool SaveWindowPlacement(HWND hwnd) {
  if (!hwnd) return false;
  // A minimized window reports a bogus rect; saving it would restore to it.
  if (::IsIconic(hwnd)) return false;
  RECT rc{};
  if (!::GetWindowRect(hwnd, &rc)) return false;
  const int32_t width = rc.right - rc.left;
  const int32_t height = rc.bottom - rc.top;
  if (width <= 0 || height <= 0) return false;

  HKEY key = nullptr;
  if (::RegCreateKeyExW(HKEY_CURRENT_USER, kPlacementKey, 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
    LogError("shell: could not open {} for writing - placement not saved",
             urnw::Narrow(std::wstring{kPlacementKey}));
    return false;
  }
  const UINT dpi = ::GetDpiForWindow(hwnd);
  StoredPlacement stored{};
  stored.magic = kPlacementMagic;
  stored.version = kPlacementVersion;
  stored.x = static_cast<int32_t>(rc.left);
  stored.y = static_cast<int32_t>(rc.top);
  stored.width = width;
  stored.height = height;
  stored.dpi = (dpi == 0 ? 96u : dpi);
  // One value, one write: it either lands whole or leaves the previous good
  // placement in place. There is no partial state, so there is no cleanup path
  // to get wrong.
  const LSTATUS wrote =
      ::RegSetValueExW(key, kPlacementValue, 0, REG_BINARY,
                       reinterpret_cast<const BYTE*>(&stored), sizeof(stored));
  ::RegCloseKey(key);
  if (wrote != ERROR_SUCCESS) {
    LogError("shell: placement write failed ({}) - the previous saved placement "
             "is untouched", static_cast<int32_t>(wrote));
    return false;
  }
  LogInfo("shell: saved placement {}x{} at ({},{}) at {} dpi", width, height,
          static_cast<int32_t>(rc.left), static_cast<int32_t>(rc.top), dpi);
  return true;
}

}  // namespace urnw::shell
