// SPDX-License-Identifier: MPL-2.0
#include "pch.h"

#include "UrComponents.h"

#include <chrono>

#include "Log.h"
#include "UrColors.h"

using namespace winrt::Microsoft::UI::Xaml::Controls;

namespace urnw::kit {

void ApplySupportingText(TextBlock const& line, winrt::hstring const& text,
                         ValidationState state) {
  if (!line) return;
  line.Text(text);
  switch (state) {
    case ValidationState::Invalid:
      line.Foreground(urnw::colors::DangerBrush());
      break;
    case ValidationState::Valid:
      line.Foreground(urnw::colors::MakeBrush(urnw::colors::kUrGreen));
      break;
    case ValidationState::NotChecked:
    case ValidationState::Validating:
      line.Foreground(urnw::colors::MutedBrush());
      break;
  }
}

Snackbar::Snackbar(InfoBar bar,
                   winrt::Microsoft::UI::Dispatching::DispatcherQueue const& queue,
                   int durationMs)
    : bar_(std::move(bar)) {
  if (!bar_ || !queue) {
    // A snackbar with no bar is a message that goes nowhere. Say so once here
    // rather than let every later Show() silently do nothing.
    urnw::LogError("kit: snackbar constructed without {}",
                   bar_ ? "a dispatcher queue" : "an InfoBar");
    return;
  }
  timer_ = queue.CreateTimer();
  timer_.Interval(std::chrono::milliseconds(durationMs));
  timer_.IsRepeating(false);
  // The timer outlives no one: it is stopped in the destructor, and both it and
  // the bar are owned by the same page, so `this` is valid for every tick.
  timer_.Tick([this](auto const&, auto const&) { Hide(); });
}

Snackbar::~Snackbar() {
  if (timer_) timer_.Stop();
}

void Snackbar::Show(winrt::hstring const& message, InfoBarSeverity severity) {
  if (!bar_) return;
  bar_.Severity(severity);
  bar_.Message(message);
  bar_.IsOpen(true);
  if (timer_) {
    timer_.Stop();  // a second message restarts the window rather than inheriting it
    timer_.Start();
  }
}

void Snackbar::Hide() {
  if (timer_) timer_.Stop();
  if (bar_) bar_.IsOpen(false);
}

}  // namespace urnw::kit
