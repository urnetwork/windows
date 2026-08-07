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
    : bar_(std::move(bar)),
      defaultDurationMs_(durationMs),
      self_(std::make_shared<Snackbar*>(this)) {
  if (!bar_ || !queue) {
    // A snackbar with no bar is a message that goes nowhere. Say so once here
    // rather than let every later Show() silently do nothing.
    urnw::LogError("kit: snackbar constructed without {}",
                   bar_ ? "a dispatcher queue" : "an InfoBar");
    return;
  }
  timer_ = queue.CreateTimer();
  timer_.IsRepeating(false);
  // `self_` rather than a raw `this`: the destructor nulls it, so a tick that
  // outlives its Snackbar does nothing instead of writing through a dangling
  // pointer. The interval is set per Show(), since it depends on severity.
  timer_.Tick([self = self_](auto const&, auto const&) {
    if (auto* snackbar = *self) snackbar->Hide();
  });
}

Snackbar::~Snackbar() {
  if (timer_) timer_.Stop();
  if (self_) *self_ = nullptr;
}

void Snackbar::Show(winrt::hstring const& message, InfoBarSeverity severity,
                    std::optional<int> durationMs) {
  if (!bar_) return;
  bar_.Severity(severity);
  bar_.Message(message);
  bar_.IsOpen(true);
  if (!timer_) return;
  timer_.Stop();  // a second message restarts the window rather than inheriting it

  // An error is usually the only diagnostic the user gets; it waits for them.
  const bool safeToMiss = (severity == InfoBarSeverity::Informational ||
                           severity == InfoBarSeverity::Success);
  const int duration =
      durationMs.value_or(safeToMiss ? defaultDurationMs_ : kPersistent);
  if (duration <= kPersistent) return;
  timer_.Interval(std::chrono::milliseconds(duration));
  timer_.Start();
}

void Snackbar::Hide() {
  if (timer_) timer_.Stop();
  if (bar_) bar_.IsOpen(false);
}

}  // namespace urnw::kit
