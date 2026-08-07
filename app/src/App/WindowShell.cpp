// SPDX-License-Identifier: MPL-2.0
#include "pch.h"

#include "WindowShell.h"

#include <algorithm>
#include <optional>

#include <winrt/Microsoft.UI.Composition.h>
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

bool WriteDword(HKEY key, wchar_t const* name, int32_t value) {
  const DWORD stored = static_cast<DWORD>(value);
  return ::RegSetValueExW(key, name, 0, REG_DWORD,
                          reinterpret_cast<const BYTE*>(&stored),
                          sizeof(stored)) == ERROR_SUCCESS;
}

// Position is stored as an offset from a sentinel so a legitimately negative
// coordinate (a monitor to the left of the primary) round-trips through a DWORD.
constexpr int32_t kPositionBias = 100000;

struct Placement {
  int32_t x = 0;
  int32_t y = 0;
  int32_t width = 0;
  int32_t height = 0;
  // The DPI the size was MEASURED at. Everything here is physical pixels, so
  // without this a user who changes their display scale gets the old physical
  // size reinterpreted at the new one - a window that grows or shrinks by the
  // ratio for no reason they can see. (Moving to a differently-scaled MONITOR
  // is self-correcting: the window manager sends WM_DPICHANGED with a suggested
  // rect. Rescaling the same monitor does not.) 0 means "not recorded".
  uint32_t dpi = 96;
};

std::optional<Placement> LoadPlacement() {
  auto w = ReadDword(L"Width");
  auto h = ReadDword(L"Height");
  auto x = ReadDword(L"X");
  auto y = ReadDword(L"Y");
  auto dpi = ReadDword(L"Dpi");
  if (!w || !h || !x || !y) return std::nullopt;
  Placement p;
  p.width = static_cast<int32_t>(*w);
  p.height = static_cast<int32_t>(*h);
  p.x = static_cast<int32_t>(*x) - kPositionBias;
  p.y = static_cast<int32_t>(*y) - kPositionBias;
  // absent (an older build wrote this) or nonsense: 0, which skips the rescale
  p.dpi = (dpi && 0 < *dpi) ? *dpi : 0;
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

namespace backdrops = winrt::Microsoft::UI::Composition::SystemBackdrops;

// The controller and its configuration must outlive this call - releasing the
// controller tears the backdrop down - and there is exactly one main window for
// the life of the process. Deliberately leaked rather than held in a static
// unique_ptr: destroying a WinRT object during static destruction runs after
// COM has been torn down.
struct MicaState {
  backdrops::MicaController controller{nullptr};
  backdrops::SystemBackdropConfiguration config{nullptr};
};
MicaState* g_mica = nullptr;

// Mica is a Windows 11 feature. MicaController::IsSupported() is the OS's own
// answer, so this needs no version check of its own and no build-number table.
//
// This drives MicaController directly rather than handing XAML a MicaBackdrop,
// for one reason: FALLBACK COLOUR. IsSupported() answers "does this OS have
// Mica", NOT "is Mica compositing right now" - transparency effects off,
// battery saver, RDP and several VM/GPU paths all keep it supported while the
// backdrop collapses to its fallback. By then the opaque root background has
// been cleared, so the fallback IS the app background, and MicaController's
// default is the system base fill (measured at #202020 here), not the brand
// #101010. Microsoft.UI.Xaml.Media.MicaBackdrop exposes no FallbackColor at
// all - only Kind - so it cannot express this; the controller can.
bool ApplyBackdrop(winrtx::Window const& window) {
  try {
    if (!backdrops::MicaController::IsSupported()) {
      LogInfo("shell: mica not supported by this OS - keeping the solid #101010 background");
      return false;
    }
    auto target =
        window.try_as<winrt::Microsoft::UI::Composition::ICompositionSupportsSystemBackdrop>();
    if (!target) {
      LogError("shell: this window cannot host a system backdrop - keeping the solid background");
      return false;
    }

    auto state = std::make_unique<MicaState>();
    state->config = backdrops::SystemBackdropConfiguration();
    // the app requests Dark unconditionally (App.xaml RequestedTheme)
    state->config.Theme(backdrops::SystemBackdropTheme::Dark);
    state->config.IsInputActive(true);
    state->controller = backdrops::MicaController();
    state->controller.FallbackColor(urnw::colors::kBackground);
    state->controller.SetSystemBackdropConfiguration(state->config);
    state->controller.AddSystemBackdropTarget(target);

    // Native behaviour: Mica dims while the window is not the active one. The
    // configuration is what carries that, and nothing updates it by itself.
    auto* raw = state.get();
    window.Activated([raw](auto const&, winrtx::WindowActivatedEventArgs const& args) {
      if (raw->config) {
        raw->config.IsInputActive(args.WindowActivationState() !=
                                  winrtx::WindowActivationState::Deactivated);
      }
    });

    // Read the fallback BACK rather than logging what we meant to set. This is
    // the colour the window becomes whenever Mica degrades at runtime while
    // still being "supported", and the opaque root background has been cleared
    // by then - so it is the app background, and a silent default here would be
    // the system fill, not the brand one.
    const auto applied = state->controller.FallbackColor();
    LogInfo("shell: mica backdrop applied (fallback #{:02X}{:02X}{:02X})", applied.R,
            applied.G, applied.B);
    g_mica = state.release();  // see the comment on MicaState
  } catch (winrt::hresult_error const& e) {
    // Not fatal: the solid background is a complete look on its own.
    LogError("shell: mica backdrop refused ({}) - keeping the solid background",
             urnw::Narrow(std::wstring{e.message()}));
    return false;
  }
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

bool ApplyNativeShell(winrtx::Window const& window, HWND hwnd) {
  if (!window || !hwnd) return false;

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
  bool ok = WriteDword(key, L"X", rc.left + kPositionBias);
  ok = WriteDword(key, L"Y", rc.top + kPositionBias) && ok;
  ok = WriteDword(key, L"Width", width) && ok;
  ok = WriteDword(key, L"Height", height) && ok;
  ok = WriteDword(key, L"Dpi", static_cast<int32_t>(dpi == 0 ? 96 : dpi)) && ok;
  if (!ok) {
    // A partial write is worse than none: LoadPlacement would accept three
    // fresh values and one stale one and restore a rect that never existed.
    // Drop the lot so the next run falls back to the compact default.
    for (auto const* name : {L"X", L"Y", L"Width", L"Height", L"Dpi"}) {
      ::RegDeleteValueW(key, name);
    }
    ::RegCloseKey(key);
    LogError("shell: placement write failed part-way - cleared the saved values "
             "rather than leave a mixed set");
    return false;
  }
  ::RegCloseKey(key);
  LogInfo("shell: saved placement {}x{} at ({},{}) at {} dpi", width, height,
          static_cast<int32_t>(rc.left), static_cast<int32_t>(rc.top), dpi);
  return true;
}

}  // namespace urnw::shell
