// The provider globe: a dark sphere with white land, a graticule, and one dot
// per plottable provider colored by its country. A port of the android
// ProviderGlobe.kt (Compose Canvas) onto XAML Shapes, which is what this app
// already draws with (TransferChart) -- there is no Win2D/Direct2D dependency in
// the solution and adding one for a 100 KB wireframe would not pay for itself.
//
// Renders into a host Grid, like TransferChart: a Canvas carrying the sphere
// Ellipse, one Path for all land (fill + hairline border), one Path for the
// graticule, and a child Canvas of provider dots. Land and graticule are
// rebuilt as PathGeometry on each redraw, but only when the rotation actually
// changes -- each ring becomes a single PathFigure whose PolyLineSegment points
// are set with one ReplaceAll, so a redraw is a few hundred WinRT calls rather
// than ~10,500.
//
// Interaction (android parity): with providers on the globe a horizontal drag
// or the mouse wheel is a SCROLL WHEEL over the providers ordered by longitude,
// stepping past a hysteresis threshold and recentering on each step; free
// rotation would fight that animation, so it is enabled only when there is
// nothing plottable to traverse. Clicking a dot selects it.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.Shapes.h>

#include "GlobeGeometry.h"
#include "ProviderLocations.h"
#include "WorldTopology.h"

namespace urnw {

class ProviderGlobe {
 public:
  // `host` receives the globe canvas; it should be square-ish and is fit-center
  // scaled, so a wider-than-tall host still shows the whole globe, centered.
  explicit ProviderGlobe(winrt::Microsoft::UI::Xaml::Controls::Grid const& host);
  ~ProviderGlobe();

  // Replace the plotted providers and the selection. Cheap enough to call on
  // every SDK push; a selection change starts the recenter animation.
  void SetProviders(std::vector<ProviderLocationRow> rows, std::string selectedClientId);

  // Selecting a dot. The sheet keeps the single selection and calls
  // SetProviders back, so this only reports the intent.
  void SetOnSelect(std::function<void(std::string)> onSelect) { onSelect_ = std::move(onSelect); }

  // Advance the recenter animation; called from the window's shared ~10 fps
  // drawer clock (MainWindow::OnChartTick). A no-op when nothing is in flight.
  void Tick();

 private:
  void BuildVisuals(winrt::Microsoft::UI::Xaml::Controls::Grid const& host);
  void Redraw();
  void RedrawDots(float width, float height, float unit);
  winrt::Microsoft::UI::Xaml::Media::PathGeometry BuildLandGeometry(
      const std::shared_ptr<const WorldTopology>& world, float width, float height) const;
  winrt::Microsoft::UI::Xaml::Media::PathGeometry BuildGraticuleGeometry(float width,
                                                                         float height) const;
  // Kick off the one-time background TopoJSON decode; redraws when it lands.
  void EnsureTopology();
  // Start the spin to the given provider (shortest way round). No-op if it is
  // already centered there.
  void CenterOn(const ProviderLocationRow& row);
  void StepWheel(int steps);
  void OnPointerPressed(winrt::Windows::Foundation::IInspectable const& sender,
                        winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args);
  void OnPointerMoved(winrt::Windows::Foundation::IInspectable const& sender,
                      winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args);
  void EndDrag(winrt::Windows::Foundation::IInspectable const& sender,
               winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args);
  void OnPointerWheel(winrt::Windows::Foundation::IInspectable const& sender,
                      winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args);
  void OnTapped(float x, float y);

  winrt::Microsoft::UI::Xaml::Controls::Canvas canvas_{nullptr};
  winrt::Microsoft::UI::Xaml::Shapes::Ellipse sphere_{nullptr};
  winrt::Microsoft::UI::Xaml::Shapes::Path land_{nullptr};
  winrt::Microsoft::UI::Xaml::Shapes::Path graticule_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Canvas dots_{nullptr};

  // Auto-revoked so a pointer event delivered to a canvas that briefly outlives
  // this object (XAML holds the closing dialog's tree) can never reach a
  // dangling `this`.
  winrt::Microsoft::UI::Xaml::FrameworkElement::SizeChanged_revoker sizeChangedRevoker_;
  winrt::Microsoft::UI::Xaml::UIElement::PointerPressed_revoker pointerPressedRevoker_;
  winrt::Microsoft::UI::Xaml::UIElement::PointerMoved_revoker pointerMovedRevoker_;
  winrt::Microsoft::UI::Xaml::UIElement::PointerReleased_revoker pointerReleasedRevoker_;
  winrt::Microsoft::UI::Xaml::UIElement::PointerCanceled_revoker pointerCanceledRevoker_;
  winrt::Microsoft::UI::Xaml::UIElement::PointerCaptureLost_revoker pointerCaptureLostRevoker_;
  winrt::Microsoft::UI::Xaml::UIElement::PointerWheelChanged_revoker pointerWheelRevoker_;
  winrt::Microsoft::UI::Xaml::UIElement::Tapped_revoker tappedRevoker_;

  std::vector<ProviderLocationRow> rows_;
  std::vector<ProviderLocationRow> plottable_;
  std::vector<ProviderLocationRow> wheel_;  // plottable, west to east
  std::string selectedClientId_;
  std::function<void(std::string)> onSelect_;

  GlobeRotation rotation_{};
  // recenter animation
  bool animating_ = false;
  GlobeRotation animFrom_{};
  GlobeRotation animTo_{};
  double animStartSeconds_ = 0;
  // the globe centers once on the first provider that appears and thereafter
  // only on an explicit selection -- recentering on every window turnover would
  // fight the user
  bool centeredOnce_ = false;
  // whether the last redraw had the land: the topology arrives from a
  // background decode, so the first frames legitimately draw without it
  bool landLoaded_ = false;

  // pointer state
  bool dragging_ = false;
  float lastPointerX_ = 0;
  float lastPointerY_ = 0;
  float dragTravel_ = 0;   // horizontal travel toward the next wheel step
  float wheelTravel_ = 0;  // accumulated mouse-wheel delta

  bool dirty_ = true;
};

}  // namespace urnw
