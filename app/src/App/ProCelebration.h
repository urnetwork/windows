// The Pro celebration (android ProFlightClock.kt / ProSunglassesFlight.kt): a
// 15 s confetti stream of pixel sunglasses, eye covers and face discs racing
// left to right over the whole window, while the window under them turns to
// a mosaic — the cell grows over the first 5 s, holds while the confetti
// flies, and shrinks back over the 5 s after the last sprite has left, so the
// window is sharp again at 20 s. One clock drives both; the overlay takes no
// input; nothing plays when the system has animations off.
//
// The mosaic is a snapshot, as on iOS (ProScreenSnapshot) and Linux
// (PixelateBin): the window is frozen the instant a flight launches and the
// frozen copy is pixelated while the live UI carries on underneath. There is
// no Win2D in this project, so the nearest-neighbour sampling is done by the
// compositor: a full-resolution surface of the snapshot is drawn into a
// visual surface of (width / cell) x (height / cell) pixels with nearest
// sampling (one source pixel per cell), and that small surface is stretched
// back over the window, again with nearest sampling, which makes the blocks.
// The cell changes every frame by resizing the small surface.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <winrt/Microsoft.UI.Composition.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Windows.Storage.Streams.h>

namespace urnw {

// The flight timeline, in seconds.
inline constexpr double kProFlightTotalSeconds = 20.0;
inline constexpr double kProFlightConfettiSeconds = 15.0;
inline constexpr double kProFlightPixelateInSeconds = 5.0;
inline constexpr double kProFlightPixelateOutStartSeconds = 15.0;
inline constexpr double kProFlightPixelateOutSeconds = 5.0;
// The mosaic cell at full strength, in device-independent pixels (android's
// 24 dp), and the cell under which the mosaic is not drawn at all: a block
// smaller than that is indistinguishable from the live window, so the
// snapshot hands back to it without a pop.
inline constexpr double kProFlightPixelateMaxCell = 24.0;
inline constexpr double kProFlightPixelateMinCell = 2.0;

// One sprite of the burst, fixed for the whole flight.
struct ProFlightSprite {
  enum class Kind { Sunglasses, EyeCover, FaceCover };
  Kind kind = Kind::Sunglasses;
  double delaySeconds = 0;
  double crossingSeconds = 1.5;
  double lane = 0.5;  // fraction of the height
  double bobAmplitude = 24;  // px
  double bobCycles = 3;
  double phaseOffset = 0;
  double scale = 1;
  bool behind = false;
};

// The whole take-off schedule of one flight, computed once at launch.
std::vector<ProFlightSprite> ProFlightBurst(uint64_t seed);

// The 0..1 strength of the mosaic at a moment of the flight: ramp in, hold,
// ramp out (the same envelope the android mosaic cell follows). The cell is
// kProFlightPixelateMaxCell times this.
double ProFlightMosaicStrength(double seconds);

// The flight: drives the sprites on the window's celebration canvas and the
// mosaic's cell from one 60 Hz timer. The canvas and the mosaic host sit at
// the bottom of MainWindow.xaml above everything else, hit-test invisible;
// the snapshot root is the window content the mosaic freezes.
class ProCelebrationFlight {
 public:
  ProCelebrationFlight(winrt::Microsoft::UI::Xaml::Controls::Canvas const& canvas,
                       winrt::Microsoft::UI::Xaml::UIElement const& mosaicHost,
                       winrt::Microsoft::UI::Xaml::UIElement const& snapshotRoot);
  ~ProCelebrationFlight();

  // Starts a flight (a new seeded burst). Does nothing when the system has
  // animations off, or while a flight is already in the air.
  void Launch();
  bool Active() const { return active_; }

 private:
  struct Live;  // one sprite in the air: its XAML paths and transforms

  winrt::fire_and_forget Begin(uint64_t sequence);
  void TakeOff();
  void Tick();
  void Finish();
  double Seconds() const;
  void Retire(Live& live);
  void InstallMosaic(winrt::Windows::Storage::Streams::IRandomAccessStream const& snapshot,
                     float pixelWidth, float pixelHeight);
  void UpdateMosaic(double strength);
  void DropMosaic();

  winrt::Microsoft::UI::Xaml::Controls::Canvas canvas_;
  winrt::Microsoft::UI::Xaml::UIElement mosaicHost_;
  winrt::Microsoft::UI::Xaml::UIElement snapshotRoot_;
  winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer timer_{nullptr};
  std::vector<ProFlightSprite> sprites_;
  std::vector<Live> live_;
  size_t nextSprite_ = 0;
  uint64_t sequence_ = 0;
  int64_t startTicks_ = 0;
  bool active_ = false;

  // The mosaic (see the file comment): the snapshot's full-resolution
  // surface is drawn by the sampler into the blocks surface, which the mosaic
  // visual (a child visual of the host) stretches over the window.
  winrt::Microsoft::UI::Composition::SpriteVisual sampler_{nullptr};
  winrt::Microsoft::UI::Composition::CompositionVisualSurface blocks_{nullptr};
  winrt::Microsoft::UI::Composition::SpriteVisual mosaic_{nullptr};
  winrt::Windows::Storage::Streams::IRandomAccessStream snapshot_{nullptr};
  float snapshotPixelWidth_ = 0;   // the snapshot, in physical pixels
  float snapshotPixelHeight_ = 0;
  float snapshotWidth_ = 0;  // the root's size when it was taken, in DIPs
  float snapshotHeight_ = 0;
  // set false by the destructor so a snapshot still encoding lets go
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

}  // namespace urnw
