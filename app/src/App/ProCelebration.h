// The Pro celebration (android ProFlightClock.kt / ProSunglassesFlight.kt): a
// 15 s confetti stream of pixel sunglasses, eye covers and face discs racing
// left to right over the whole window, while the window under them softens —
// the veil fades in over the first 5 s, holds while the confetti flies, and
// fades out over the 5 s after the last sprite has left, so the window is
// sharp again at 20 s. One clock drives both; the overlay takes no input;
// nothing plays when the system has animations off.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <vector>

#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Shapes.h>

namespace urnw {

// The flight timeline, in seconds.
inline constexpr double kProFlightTotalSeconds = 20.0;
inline constexpr double kProFlightConfettiSeconds = 15.0;
inline constexpr double kProFlightPixelateInSeconds = 5.0;
inline constexpr double kProFlightPixelateOutStartSeconds = 15.0;
inline constexpr double kProFlightPixelateOutSeconds = 5.0;

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

// The 0..1 strength of the veil at a moment of the flight: ramp in, hold,
// ramp out (the same envelope the android mosaic cell follows).
double ProFlightVeilStrength(double seconds);

// The flight: drives the sprites on the window's celebration canvas and the
// veil's opacity from one 60 Hz timer. Both elements sit at the bottom of
// MainWindow.xaml above everything else, hit-test invisible.
class ProCelebrationFlight {
 public:
  ProCelebrationFlight(winrt::Microsoft::UI::Xaml::Controls::Canvas const& canvas,
                       winrt::Microsoft::UI::Xaml::Shapes::Rectangle const& veil);
  ~ProCelebrationFlight();

  // Starts a flight (a new seeded burst). Does nothing when the system has
  // animations off, or while a flight is already in the air.
  void Launch();
  bool Active() const { return active_; }

 private:
  struct Live;  // one sprite in the air: its XAML paths and transforms

  void Tick();
  void Finish();
  double Seconds() const;
  void Retire(Live& live);

  winrt::Microsoft::UI::Xaml::Controls::Canvas canvas_;
  winrt::Microsoft::UI::Xaml::Shapes::Rectangle veil_;
  winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer timer_{nullptr};
  std::vector<ProFlightSprite> sprites_;
  std::vector<Live> live_;
  size_t nextSprite_ = 0;
  uint64_t sequence_ = 0;
  int64_t startTicks_ = 0;
  bool active_ = false;
};

}  // namespace urnw
