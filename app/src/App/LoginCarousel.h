// The login screen's carousel — iOS Authenticate/LoginInitial/LoginCarousel.swift.
//
// Three brand images crossfading inside the ur globe silhouette, with the
// headline that belongs to each sliding up and out as the next slides in, and
// the "with URnetwork" line trailing it. Five seconds a slide.
//
// It is not decoration: the login screen has no heading of its own (see the
// comment on LoginPanel in MainWindow.xaml — "the carousel above supplies the
// headline"), so without this the top of the signed-out screen is empty and
// unlabelled.
//
// Built entirely from code into a host Grid, like UsageBar and TransferChart:
// there is no user-facing text in the markup, and the slide/fade timings are
// easier to keep in step with iOS's in one place.
//
// The timer runs ONLY while the window is on screen. A tray app spends most of
// its life hidden, and an animation nobody can see is pure wakeups: LoginPage
// forwards MainWindow::SetPresentationActive here (iOS gates the same timer on
// its `presentationActive` environment value).
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <vector>

#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.Media.Animation.h>

namespace urnw {

class LoginCarousel {
 public:
  LoginCarousel(winrt::Microsoft::UI::Xaml::Controls::Grid const& host,
                winrt::Microsoft::UI::Dispatching::DispatcherQueue const& queue);
  ~LoginCarousel();

  LoginCarousel(LoginCarousel const&) = delete;
  LoginCarousel& operator=(LoginCarousel const&) = delete;

  // (Re)paint the current slide's headline from the localization store.
  void ApplyStrings();

  // Start/stop the slide timer. Off while the window is hidden or the login
  // flow has moved past the initial step.
  void SetActive(bool active);

 private:
  void Build();
  // fit the globe to whatever height the slot ended up with, and the headline
  // type to the globe, so the words stay inside the mask at every window size
  void ApplyMetrics();
  void ShowSlide(size_t index);   // paint text + image for a slide, no animation
  void Advance();                 // timer tick: animate out, swap, animate in
  void AnimateTextOut();
  void AnimateTextIn();
  void CrossfadeTo(size_t index);
  // stop and drop every running board, so nothing holds a property or a
  // Completed callback past this point
  void StopAnimations();

  winrt::Microsoft::UI::Xaml::Controls::Grid host_{nullptr};
  winrt::Microsoft::UI::Dispatching::DispatcherQueue queue_{nullptr};
  winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer timer_{nullptr};
  // the two stacked globe-clipped images the crossfade runs between
  winrt::Microsoft::UI::Xaml::Shapes::Path currentImage_{nullptr};
  winrt::Microsoft::UI::Xaml::Shapes::Path nextImage_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock headline_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock bottomLine_{nullptr};
  winrt::Microsoft::UI::Xaml::Media::TranslateTransform headlineShift_{nullptr};
  winrt::Microsoft::UI::Xaml::Media::TranslateTransform bottomShift_{nullptr};
  // one TEXT storyboard at a time; a new phase stops the previous one so a slow
  // machine cannot leave two animations fighting over the same property
  winrt::Microsoft::UI::Xaml::Media::Animation::Storyboard running_{nullptr};
  // The image crossfade, tracked for exactly the same reasons and separately
  // because it overlaps the text animations by design. It used to be a bare
  // local that nothing could stop: while it ran it HELD the two Opacity
  // properties, so SetActive(false)'s ShowSlide() wrote values the animation
  // immediately overrode — the window came back showing slide N+1's image
  // under slide N's headline, and the Completed handler's `if (!active_)
  // return` guaranteed it never corrected itself. Its Completed also captured
  // a raw `this` that outlived nothing in particular for 700ms after quit.
  winrt::Microsoft::UI::Xaml::Media::Animation::Storyboard crossfade_{nullptr};

  size_t index_ = 0;
  bool active_ = false;
};

}  // namespace urnw
