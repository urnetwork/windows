// SPDX-License-Identifier: MPL-2.0
#include "pch.h"

#include "WindowShell.h"

#include <algorithm>
#include <optional>

#include <winrt/Microsoft.UI.Composition.SystemBackdrops.h>
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

// HKCU\Software\URnetwork\Window — four DWORDs in PHYSICAL pixels. The registry
// rather than a file: window placement is exactly what it is for, each value is
// written atomically, and there is nothing to parse (so there is no
// half-written state to defend against).
constexpr wchar_t kPlacementKey[] = L"Software\\URnetwork\\Window";

std::optional<uint32_t> ReadDword(wchar_t const* name) {
  DWORD value = 0;
  DWORD size = sizeof(value);
  if (::RegGetValueW(HKEY_CURRENT_USER, kPlacementKey, name, RRF_RT_REG_DWORD, nullptr,
                     &value, &size) != ERROR_SUCCESS) {
    return std::nullopt;
  }
  return value;
}

void WriteDword(HKEY key, wchar_t const* name, int32_t value) {
  const DWORD stored = static_cast<DWORD>(value);
  ::RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&stored),
                   sizeof(stored));
}

// Position is stored as an offset from a sentinel so a legitimately negative
// coordinate (a monitor to the left of the primary) round-trips through a DWORD.
constexpr int32_t kPositionBias = 100000;

struct Placement {
  int32_t x = 0;
  int32_t y = 0;
  int32_t width = 0;
  int32_t height = 0;
};

std::optional<Placement> LoadPlacement() {
  auto w = ReadDword(L"Width");
  auto h = ReadDword(L"Height");
  auto x = ReadDword(L"X");
  auto y = ReadDword(L"Y");
  if (!w || !h || !x || !y) return std::nullopt;
  Placement p;
  p.width = static_cast<int32_t>(*w);
  p.height = static_cast<int32_t>(*h);
  p.x = static_cast<int32_t>(*x) - kPositionBias;
  p.y = static_cast<int32_t>(*y) - kPositionBias;
  // A saved size of zero (a minimized or otherwise degenerate window) must not
  // become the size we restore to.
  if (p.width <= 0 || p.height <= 0) return std::nullopt;
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

// Mica is a Windows 11 feature. MicaController::IsSupported() is the OS's own
// answer, so this needs no version check of its own and no build-number table.
bool ApplyBackdrop(winrtx::Window const& window) {
  namespace backdrops = winrt::Microsoft::UI::Composition::SystemBackdrops;
  try {
    if (!backdrops::MicaController::IsSupported()) {
      LogInfo("shell: mica not supported by this OS - keeping the solid #101010 background");
      return false;
    }
    window.SystemBackdrop(winrtx::Media::MicaBackdrop{});
  } catch (winrt::hresult_error const& e) {
    // Not fatal: the solid background is a complete look on its own.
    LogError("shell: mica backdrop refused ({}) - keeping the solid background",
             urnw::Narrow(std::wstring{e.message()}));
    return false;
  }
  LogInfo("shell: mica backdrop applied");
  return true;
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

void ApplyNativeShell(winrtx::Window const& window, HWND hwnd) {
  if (!window || !hwnd) return;

  const bool mica = ApplyBackdrop(window);
  if (mica) {
    // Mica is drawn BEHIND the XAML content. The root grid paints an opaque
    // #101010 over the whole window, so without this the backdrop is applied,
    // costs its composition work, and is invisible.
    if (auto root = window.Content().try_as<winrtx::Controls::Panel>()) {
      root.Background(nullptr);
    } else {
      LogError("shell: mica applied but the root element is not a Panel - "
               "its background could not be cleared, so the backdrop will not show");
    }
  }

  auto windowId = winrt::Microsoft::UI::GetWindowIdFromWindow(hwnd);
  auto appWindow = windowing::AppWindow::GetFromWindowId(windowId);
  if (!appWindow) {
    LogError("shell: no AppWindow for hwnd {} - size and chrome left at their defaults",
             reinterpret_cast<uintptr_t>(hwnd));
    return;
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
  if (auto saved = LoadPlacement()) {
    p = ClampToWorkArea(*saved);
    LogInfo("shell: restored placement {}x{} at ({},{})", p.width, p.height, p.x, p.y);
  } else {
    // No saved placement: the compact default, centred on the monitor the
    // window was created on so the first run is not in a corner.
    p.width = defaultW;
    p.height = defaultH;
    auto area = appWindow.Position();
    p.x = area.X;
    p.y = area.Y;
    p = ClampToWorkArea(p);
    LogInfo("shell: no saved placement - compact default {}x{} (dpi scale {:.2f})",
            p.width, p.height, scale);
  }
  appWindow.MoveAndResize({p.x, p.y, p.width, p.height});
}

void SaveWindowPlacement(HWND hwnd) {
  if (!hwnd) return;
  // A minimized window reports a bogus rect; saving it would restore to it.
  if (::IsIconic(hwnd)) return;
  RECT rc{};
  if (!::GetWindowRect(hwnd, &rc)) return;
  const int32_t width = rc.right - rc.left;
  const int32_t height = rc.bottom - rc.top;
  if (width <= 0 || height <= 0) return;

  HKEY key = nullptr;
  if (::RegCreateKeyExW(HKEY_CURRENT_USER, kPlacementKey, 0, nullptr, 0, KEY_SET_VALUE,
                        nullptr, &key, nullptr) != ERROR_SUCCESS) {
    LogError("shell: could not open {} for writing - placement not saved",
             urnw::Narrow(std::wstring{kPlacementKey}));
    return;
  }
  WriteDword(key, L"X", rc.left + kPositionBias);
  WriteDword(key, L"Y", rc.top + kPositionBias);
  WriteDword(key, L"Width", width);
  WriteDword(key, L"Height", height);
  ::RegCloseKey(key);
  LogInfo("shell: saved placement {}x{} at ({},{})", width, height,
          static_cast<int32_t>(rc.left), static_cast<int32_t>(rc.top));
}

}  // namespace urnw::shell
