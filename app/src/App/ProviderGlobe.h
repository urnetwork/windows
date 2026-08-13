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
// or the mouse wheel is a SCROLL WHEEL over the providers, stepping past a
// hysteresis threshold and recentering on each step; free rotation would fight
// that animation, so it is enabled only when there is nothing plottable to
// traverse. Clicking a dot selects it. Where a step LANDS is the SDK's shared
// ProviderLocationsViewController (the centroid-relative west-to-east order,
// clamped at both ends), not this widget's -- it only reports step counts.
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

// The rotation the globe is animating toward, identified by the provider it
// belongs to. Comparing the coordinates as well as the id is what makes a
// provider whose position changes under the selection recenter the globe.
struct GlobeCenterTarget {
  std::string clientId;
  double lat = 0;
  double lon = 0;

  bool operator==(const GlobeCenterTarget&) const = default;
};

class ProviderGlobe {
 public:
  // `host` receives the globe canvas; it should be square-ish and is fit-center
  // scaled, so a wider-than-tall host still shows the whole globe, centered.
  explicit ProviderGlobe(winrt::Microsoft::UI::Xaml::Controls::Grid const& host);
  ~ProviderGlobe();

  // Replace the plotted providers and the selection. Cheap enough to call on
  // every SDK push; a change to the selected provider's position -- a new
  // selection, or coordinates arriving under the one already selected --
  // retargets the recenter spring.
  void SetProviders(std::vector<ProviderLocationRow> rows, std::string selectedClientId);

  // Selecting a dot. The sheet keeps the single selection and calls
  // SetProviders back, so this only reports the intent.
  void SetOnSelect(std::function<void(std::string)> onSelect) { onSelect_ = std::move(onSelect); }

  // One wheel step, positive east, once a drag or the mouse wheel crosses the
  // hysteresis threshold. The sheet forwards it to the SDK view controller,
  // which decides where it lands and sticks at the wheel's ends.
  void SetOnStep(std::function<void(int)> onStep) { onStep_ = std::move(onStep); }

  // Advance the recenter animation; called from the window's shared ~10 fps
  // drawer clock (ConnectPage::OnChartTick). A no-op when nothing is in flight.
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
  std::string selectedClientId_;
  std::function<void(std::string)> onSelect_;
  std::function<void(int)> onStep_;

  GlobeRotation rotation_{};
  // Recenter spring: the target rotation, the current angular velocity per axis
  // (degrees per second, carried ACROSS retargets so overlapping recenters stay
  // continuous), and the tick it was last advanced at.
  bool animating_ = false;
  GlobeRotation animTo_{};
  GlobeRotation animVelocity_{};
  double animLastSeconds_ = 0;
  // The provider the globe is centered on. Empty until the globe has been
  // placed once: before that an unplottable selection falls back to the first
  // provider on the globe, after it the globe follows the selection only, since
  // chasing the first row as the window turns over would fight the user.
  GlobeCenterTarget centerTarget_{};
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
