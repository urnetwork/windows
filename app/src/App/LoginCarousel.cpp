// SPDX-License-Identifier: MPL-2.0
#include "pch.h"

#include "LoginCarousel.h"

#include <algorithm>
#include <array>
#include <chrono>

#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Microsoft.UI.Xaml.Shapes.h>
#include <winrt/Windows.Foundation.h>

#include "Localization.h"
#include "Log.h"
#include "Strings.h"
#include "UrColors.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::UI::Xaml::Media;
using namespace winrt::Microsoft::UI::Xaml::Media::Animation;
using namespace winrt::Microsoft::UI::Xaml::Shapes;

namespace urnw {
namespace {

// The ur globe silhouette (Assets.xcassets/Icons/ur.symbols.globe.svg), in its
// own 32x32 box. Path.Stretch scales it to the slot, and filling it with an
// ImageBrush is how the image gets clipped to the globe — iOS does the same
// thing with .mask(Image("ur.symbols.globe")).
constexpr const wchar_t* kGlobePath =
    L"M30 8C28.8955 8 28 7.10453 28 6C28 4.89547 27.1045 4 26 4C24.8955 4 24 3.10453 24 2C24 "
    L"0.895469 23.1045 0 22 0H10C8.89547 0 8 0.895469 8 2C8 3.10453 7.10453 4 6 4C4.89547 4 4 "
    L"4.89547 4 6C4 7.10453 3.10453 8 2 8C0.895469 8 0 8.89547 0 10V22C0 23.1045 0.895469 24 2 "
    L"24C3.10453 24 4 24.8955 4 26C4 27.1045 4.89547 28 6 28C7.10453 28 8 28.8955 8 30C8 31.1045 "
    L"8.89547 32 10 32H22C23.1045 32 24 31.1045 24 30C24 28.8955 24.8955 28 26 28C27.1045 28 28 "
    L"27.1045 28 26C28 24.8955 28.8955 24 30 24C31.1045 24 32 23.1045 32 22V10C32 8.89547 31.1045 "
    L"8 30 8Z";

// The three slides, in iOS's order. The headline keys are the store's own
// multi-line strings (see_world_content / stay_private / build_right), which is
// why nothing here is an English literal.
struct Slide {
  const char* headlineKey;
  const wchar_t* image;
};
constexpr std::array<Slide, 3> kSlides{{
    {"see_world_content", L"ms-appx:///Assets/LoginCarousel1.jpg"},
    {"stay_private", L"ms-appx:///Assets/LoginCarousel2.jpg"},
    {"build_right", L"ms-appx:///Assets/LoginCarousel3.jpg"},
}};

// The globe never grows past this, and the type is derived from whatever the
// globe actually ends up at (ApplyMetrics), so the headline stays INSIDE the
// mask at every slot size. It used to be a 190px globe with a 26px headline
// clamped to MaxWidth(300) — 110px wider than the thing it was meant to sit in.
constexpr double kGlobeMaxSide = 220;
// headline width as a fraction of the globe, and the type scale that follows
constexpr double kHeadlineWidthRatio = 0.86;
constexpr double kHeadlineSizeDivisor = 9.0;
constexpr double kHeadlineMinSize = 12;
constexpr double kHeadlineMaxSize = 26;

// iOS timings, kept in the same units so the two can be diffed.
constexpr int kSlideIntervalMs = 5000;
constexpr int kCrossfadeMs = 700;
constexpr int kTextOutMs = 500;
constexpr int kTextInMs = 500;
constexpr int kBottomDelayMs = 400;

ImageBrush BrushFor(std::wstring_view uri) {
  ImageBrush brush;
  brush.Stretch(Stretch::UniformToFill);
  Imaging::BitmapImage bitmap{winrt::Windows::Foundation::Uri{winrt::hstring{uri}}};
  // A carousel that silently renders no image is indistinguishable from a
  // carousel that is working — the headline still animates over an empty
  // globe. Say which it is, once per load, on the only channel this app has.
  const std::wstring name{uri};
  bitmap.ImageFailed([name](auto const&, ExceptionRoutedEventArgs const& args) {
    LogError("carousel: image '{}' failed to load: {}", Narrow(name),
             Narrow(std::wstring{args.ErrorMessage()}));
  });
  bitmap.ImageOpened([name](auto const&, auto const&) {
    LogInfo("carousel: image '{}' loaded", Narrow(name));
  });
  brush.ImageSource(bitmap);
  return brush;
}

// One DoubleAnimation on a named dependency property of `target`.
DoubleAnimation Anim(DependencyObject const& target, std::wstring_view property, double from,
                     double to, int durationMs, int beginMs = 0) {
  DoubleAnimation a;
  a.From(from);
  a.To(to);
  a.Duration(Duration{std::chrono::milliseconds(durationMs)});
  a.BeginTime(winrt::Windows::Foundation::TimeSpan{std::chrono::milliseconds(beginMs)});
  a.EnableDependentAnimation(true);  // Opacity/Translate here are not composition-only
  Storyboard::SetTarget(a, target);
  Storyboard::SetTargetProperty(a, winrt::hstring{property});
  return a;
}

}  // namespace

LoginCarousel::LoginCarousel(Grid const& host,
                             winrt::Microsoft::UI::Dispatching::DispatcherQueue const& queue)
    : host_(host), queue_(queue) {
  Build();
  ShowSlide(0);
}

LoginCarousel::~LoginCarousel() {
  if (timer_) timer_.Stop();
  // Every storyboard, not just the text one. Storyboard::Stop() detaches the
  // board from the timing tree and its Completed will not fire afterwards,
  // which is what makes the raw `this` those handlers capture safe: nothing
  // can call back into an object that is being destroyed.
  StopAnimations();
}

void LoginCarousel::StopAnimations() {
  if (running_) {
    running_.Stop();
    running_ = nullptr;
  }
  if (crossfade_) {
    crossfade_.Stop();
    crossfade_ = nullptr;
  }
}

void LoginCarousel::Build() {
  // Two stacked globe-clipped images: `current` at full opacity, `next` at
  // zero. A slide change fades one into the other and then swaps the sources.
  // Path.Data's mini-language ("M30 8C28.89...") is parsed by the XAML markup
  // compiler, not by any runtime API a C++ caller can reach: there is no
  // Geometry.Parse and PathGeometry takes a figure collection, not a string.
  // Loading a one-element document through XamlReader is the way to get the
  // same parser at run time.
  auto makeImage = [] {
    const std::wstring markup =
        L"<Path xmlns='http://schemas.microsoft.com/winfx/2006/xaml/presentation' Data='" +
        std::wstring(kGlobePath) + L"'/>";
    auto path = Markup::XamlReader::Load(winrt::hstring{markup}).as<Path>();
    path.Stretch(Stretch::Uniform);
    path.HorizontalAlignment(HorizontalAlignment::Center);
    path.VerticalAlignment(VerticalAlignment::Center);
    path.Width(kGlobeMaxSide);   // ApplyMetrics re-sizes both to fit the slot
    path.Height(kGlobeMaxSide);
    return path;
  };
  currentImage_ = makeImage();
  nextImage_ = makeImage();
  nextImage_.Opacity(0);
  host_.Children().Append(currentImage_);
  host_.Children().Append(nextImage_);
  // The globe and the type are sized from the slot, not from constants: the
  // slot itself is now elastic (LoginPage::ApplyLoginLayout gives the carousel
  // whatever height is left after the sign-in affordances have theirs), and a
  // fixed 190px globe under a fixed 26px headline clamped at MaxWidth(300) put
  // the words well outside the mask they are supposed to sit inside.
  host_.SizeChanged([this](auto const&, auto const&) { ApplyMetrics(); });

  // The headline sits OVER the globe, as on iOS. Pure white and the display
  // face: these are the only headlines on the signed-out screen.
  StackPanel text;
  text.VerticalAlignment(VerticalAlignment::Center);
  text.HorizontalAlignment(HorizontalAlignment::Center);

  headline_ = TextBlock();
  headline_.TextAlignment(TextAlignment::Center);
  headline_.HorizontalAlignment(HorizontalAlignment::Center);
  // Wrapped and clamped: the store's headlines carry their own newlines but the
  // longest of them ("Stay completely private and anonymous") still needs to
  // wrap, and unclamped it ran well past the edges of the globe behind it.
  headline_.TextWrapping(TextWrapping::Wrap);
  // both re-derived from the globe by ApplyMetrics; these are only a first frame
  headline_.MaxWidth(kGlobeMaxSide * kHeadlineWidthRatio);
  headline_.FontSize(kHeadlineMaxSize);
  headline_.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
  headline_.Foreground(colors::MakeBrush(winrt::Windows::UI::Color{255, 0xFF, 0xFF, 0xFF}));
  if (auto family = Application::Current()
                        .Resources()
                        .TryLookup(winrt::box_value(L"UrHeadingFontFamily"))
                        .try_as<FontFamily>()) {
    headline_.FontFamily(family);
  }
  headlineShift_ = TranslateTransform();
  headline_.RenderTransform(headlineShift_);
  text.Children().Append(headline_);

  bottomLine_ = TextBlock();
  bottomLine_.TextAlignment(TextAlignment::Center);
  bottomLine_.HorizontalAlignment(HorizontalAlignment::Center);
  bottomLine_.TextWrapping(TextWrapping::Wrap);
  bottomLine_.MaxWidth(kGlobeMaxSide * kHeadlineWidthRatio);
  bottomLine_.FontSize(kHeadlineMaxSize);
  bottomLine_.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
  bottomLine_.Foreground(colors::MakeBrush(winrt::Windows::UI::Color{255, 0xFF, 0xFF, 0xFF}));
  if (auto family = Application::Current()
                        .Resources()
                        .TryLookup(winrt::box_value(L"UrHeadingCondensedFontFamily"))
                        .try_as<FontFamily>()) {
    bottomLine_.FontFamily(family);
  }
  bottomShift_ = TranslateTransform();
  bottomLine_.RenderTransform(bottomShift_);
  text.Children().Append(bottomLine_);

  host_.Children().Append(text);

  timer_ = queue_.CreateTimer();
  timer_.Interval(std::chrono::milliseconds(kSlideIntervalMs));
  timer_.IsRepeating(true);
  timer_.Tick([this](auto const&, auto const&) { Advance(); });
}

void LoginCarousel::ApplyStrings() {
  bottomLine_.Text(winrt::hstring{Localized("with_urnetwork")});
  ShowSlide(index_);
}

// Fit the globe to the slot, and the type to the globe. The slot is elastic
// now (the sign-in affordances get their height first), so nothing here can be
// a constant: a headline wider than the mask reads as a layout accident, and a
// globe taller than the slot pushes the buttons below it off the screen.
void LoginCarousel::ApplyMetrics() {
  const double slotH = host_.ActualHeight();
  const double slotW = host_.ActualWidth();
  const double slot = std::min(slotH, slotW);
  if (slot <= 0) return;
  // A Grid does not clip its children, and a headline that turns out one line
  // taller than the slot would render straight over the field below it. Clip
  // the slot so that failure mode is structurally impossible rather than a
  // property of the arithmetic below being right.
  RectangleGeometry clip;
  clip.Rect(winrt::Windows::Foundation::Rect{0, 0, static_cast<float>(slotW),
                                             static_cast<float>(slotH)});
  host_.Clip(clip);
  const double side = std::min(slot, kGlobeMaxSide);
  currentImage_.Width(side);
  currentImage_.Height(side);
  nextImage_.Width(side);
  nextImage_.Height(side);
  const double font =
      std::clamp(side / kHeadlineSizeDivisor, kHeadlineMinSize, kHeadlineMaxSize);
  headline_.FontSize(font);
  headline_.MaxWidth(side * kHeadlineWidthRatio);
  bottomLine_.FontSize(font);
  bottomLine_.MaxWidth(side * kHeadlineWidthRatio);
}

void LoginCarousel::ShowSlide(size_t index) {
  index_ = index % kSlides.size();
  headline_.Text(winrt::hstring{Localized(kSlides[index_].headlineKey)});
  currentImage_.Fill(BrushFor(kSlides[index_].image));
  currentImage_.Opacity(1);
  nextImage_.Opacity(0);
  headlineShift_.Y(0);
  bottomShift_.Y(0);
  headline_.Opacity(1);
  bottomLine_.Opacity(1);
}

void LoginCarousel::SetActive(bool active) {
  if (active_ == active) return;
  active_ = active;
  if (!timer_) return;
  if (active) {
    timer_.Start();
  } else {
    timer_.Stop();
    // BOTH boards, and BEFORE ShowSlide. A running animation holds the
    // property it targets: with the crossfade still live, ShowSlide's
    // Opacity writes were overridden a frame later and the fade carried on to
    // completion, leaving the NEXT slide's image showing under the CURRENT
    // slide's headline for as long as the window stayed on this screen.
    StopAnimations();
    // Land on a clean frame rather than freezing mid-transition: a hidden
    // window that comes back should show a whole slide, not half of one.
    ShowSlide(index_);
  }
}

void LoginCarousel::Advance() {
  if (!active_) return;
  const size_t next = (index_ + 1) % kSlides.size();
  AnimateTextOut();
  CrossfadeTo(next);
}

void LoginCarousel::AnimateTextOut() {
  if (running_) running_.Stop();
  Storyboard board;
  board.Children().Append(Anim(headlineShift_, L"Y", 0, -100, kTextOutMs));
  board.Children().Append(Anim(headline_, L"Opacity", 1, 0, kTextOutMs));
  board.Children().Append(Anim(bottomShift_, L"Y", 0, -70, kTextOutMs, 100));
  board.Children().Append(Anim(bottomLine_, L"Opacity", 1, 0, kTextOutMs, 100));
  running_ = board;
  board.Begin();
}

void LoginCarousel::CrossfadeTo(size_t index) {
  if (crossfade_) crossfade_.Stop();  // never two fades over the same Opacity
  nextImage_.Fill(BrushFor(kSlides[index % kSlides.size()].image));

  Storyboard board;
  board.Children().Append(Anim(currentImage_, L"Opacity", 1, 0, kCrossfadeMs));
  board.Children().Append(Anim(nextImage_, L"Opacity", 0, 1, kCrossfadeMs));
  // Swap the sources once the fade has landed, so the next transition starts
  // from the same state this one did. Completed fires on the UI thread, and
  // only for a board that ran to completion — Stop() (SetActive(false), the
  // destructor, the line above) suppresses it, which is what keeps this raw
  // `this` from outliving the carousel.
  board.Completed([this, index](auto const&, auto const&) {
    if (!active_) return;
    index_ = index % kSlides.size();
    currentImage_.Fill(BrushFor(kSlides[index_].image));
    currentImage_.Opacity(1);
    nextImage_.Opacity(0);
    headline_.Text(winrt::hstring{Localized(kSlides[index_].headlineKey)});
    AnimateTextIn();
  });
  crossfade_ = board;
  board.Begin();
}

void LoginCarousel::AnimateTextIn() {
  if (running_) running_.Stop();
  // Reset before animating: the From values below are where the text starts,
  // and a storyboard that has been Stop()ed leaves the property wherever the
  // last frame put it.
  headlineShift_.Y(100);
  headline_.Opacity(0);
  bottomShift_.Y(40);
  bottomLine_.Opacity(0);

  Storyboard board;
  board.Children().Append(Anim(headlineShift_, L"Y", 100, 0, kTextInMs));
  board.Children().Append(Anim(headline_, L"Opacity", 0, 1, kTextInMs));
  board.Children().Append(Anim(bottomShift_, L"Y", 40, 0, kTextInMs, kBottomDelayMs));
  board.Children().Append(Anim(bottomLine_, L"Opacity", 0, 1, kTextInMs, kBottomDelayMs));
  running_ = board;
  board.Begin();
}

}  // namespace urnw
