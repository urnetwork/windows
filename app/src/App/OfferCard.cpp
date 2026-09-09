// SPDX-License-Identifier: MPL-2.0
#include "pch.h"

#include "OfferCard.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <optional>

#include <urnetwork_sdk.hpp>
#include <winrt/Windows.Globalization.DateTimeFormatting.h>

#include "Localization.h"
#include "PageContext.h"
#include "PlanPicker.h"  // kFreeTrialDays
#include "Strings.h"
#include "UrColors.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::UI::Xaml::Media;
using ShapeEllipse = winrt::Microsoft::UI::Xaml::Shapes::Ellipse;
using urnw::pages::Loc;

namespace urnw {
namespace {

// the reminder lands the day before the first charge (PLAN.md: day-before
// reminders outperform day-of, and a third of day-of sends are read late)
constexpr int64_t kReminderDaysBeforeCharge = 2;

TextBlock MakeLine(hstring const& text, Brush const& brush) {
  TextBlock block;
  block.Text(text);
  block.FontSize(13);
  block.Foreground(brush);
  block.TextWrapping(TextWrapping::Wrap);
  return block;
}

Grid MakeTimelineRow(hstring const& when, TextBlock& what) {
  Grid row;
  ColumnDefinition c0, c1, c2;
  c0.Width(GridLength{0, GridUnitType::Auto});
  c1.Width(GridLength{64, GridUnitType::Pixel});
  c2.Width(GridLength{1, GridUnitType::Star});
  row.ColumnDefinitions().Append(c0);
  row.ColumnDefinitions().Append(c1);
  row.ColumnDefinitions().Append(c2);
  row.ColumnSpacing(12);
  ShapeEllipse dot;
  dot.Width(8);
  dot.Height(8);
  dot.Fill(colors::ProGoldBrush());
  dot.VerticalAlignment(VerticalAlignment::Top);
  dot.Margin(Thickness{0, 5, 0, 0});
  row.Children().Append(dot);
  auto whenBlock = MakeLine(when, colors::MakeBrush(colors::kProGoldLight));
  whenBlock.TextWrapping(TextWrapping::NoWrap);
  Grid::SetColumn(whenBlock, 1);
  row.Children().Append(whenBlock);
  what = MakeLine(hstring{}, colors::MutedBrush());
  Grid::SetColumn(what, 2);
  row.Children().Append(what);
  return row;
}

hstring Money(double amount, std::string const& currency) {
  return winrt::to_hstring(FormatMoney(amount, currency));
}

// RFC 3339 ("2026-09-14T12:49:00Z", "...+02:00", fractional seconds allowed)
// to a system time point. Nullopt when the text is not a timestamp.
std::optional<std::chrono::sys_seconds> ParseRfc3339(std::string const& text) {
  int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
  if (std::sscanf(text.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d", &year, &month, &day, &hour, &minute,
                  &second) != 6) {
    return std::nullopt;
  }
  // the offset after the seconds (and any fraction)
  int offsetMinutes = 0;
  const size_t tPos = text.find('T');
  size_t at = tPos == std::string::npos ? std::string::npos : text.find_first_of("Zz+-", tPos + 9);
  if (at != std::string::npos && text[at] != 'Z' && text[at] != 'z') {
    int oh = 0, om = 0;
    if (std::sscanf(text.c_str() + at + 1, "%2d:%2d", &oh, &om) >= 1) {
      offsetMinutes = oh * 60 + om;
      if (text[at] == '-') offsetMinutes = -offsetMinutes;
    }
  }
  using namespace std::chrono;
  const year_month_day ymd{std::chrono::year{year}, std::chrono::month{static_cast<unsigned>(month)},
                           std::chrono::day{static_cast<unsigned>(day)}};
  if (!ymd.ok()) return std::nullopt;
  sys_seconds when = sys_days{ymd} + hours{hour} + minutes{minute} + seconds{second};
  when -= minutes{offsetMinutes};
  return when;
}

}  // namespace

PlanCardTexts ComposePlanCardTexts(const PriceTierView& tier, const OfferView& offer,
                                   int64_t trialDays) {
  PlanCardTexts texts;
  PriceEquivalentView eq;
  if (auto computed = urnet::computePriceEquivalent(tier.yearly, tier.monthly, 2)) {
    eq.monthlyEquivalent = computed->monthly_equivalent;
    eq.showEquivalent = computed->show_equivalent;
    eq.savingPercent = computed->saving_percent;
  }
  const std::wstring yearly = std::wstring{Money(tier.yearly, tier.currency)};
  const std::wstring monthly = std::wstring{Money(tier.monthly, tier.currency)};
  if (offer.active) {
    texts.yearlyPrice = hstring{Format("offer_first_year_price",
                                       std::wstring{Money(offer.firstYear, offer.currency)})};
    texts.yearlySecondary = hstring{Format("offer_then_regular_price",
                                           std::wstring{Money(offer.regularYear, offer.currency)})};
  } else {
    texts.yearlyPrice = hstring{Format("plan_price_per_year", yearly)};
    texts.yearlySecondary =
        ShowSaving(eq) ? hstring{Format("plan_save_percent", eq.savingPercent)} : hstring{};
  }
  texts.yearlyEquivalent =
      ShowMonthlyEquivalent(tier, eq)
          ? hstring{Format("plan_monthly_equivalent_line",
                           std::wstring{Money(eq.monthlyEquivalent, tier.currency)})}
          : hstring{};
  texts.yearlyTrial = hstring{Format("includes_free_trial_days", trialDays)};
  texts.monthlyPrice = hstring{Format("plan_price_per_month", monthly)};
  texts.monthlyLine = Loc("plan_billed_monthly_cancel_anytime");
  return texts;
}

hstring OfferDeadlineText(const OfferView& offer) {
  if (offer.expiresAt.empty()) return hstring{};
  const auto when = ParseRfc3339(offer.expiresAt);
  if (!when) return hstring{};
  try {
    // the user's locale and local time zone: "9/14/2026 3:04 PM"
    winrt::Windows::Globalization::DateTimeFormatting::DateTimeFormatter formatter(
        L"shortdate shorttime");
    const hstring stamp = formatter.Format(winrt::clock::from_sys(*when));
    return hstring{Format("offer_available_until", std::wstring{stamp})};
  } catch (...) {
    return hstring{};
  }
}

int64_t OfferExpiresInSeconds(const OfferView& offer) {
  if (offer.expiresAt.empty()) return 0;
  const auto when = ParseRfc3339(offer.expiresAt);
  if (!when) return 0;
  const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
  const auto delta = std::chrono::duration_cast<std::chrono::seconds>(*when - now).count();
  return delta < 0 ? 0 : static_cast<int64_t>(delta);
}

StackPanel OfferLines::Build(bool compact) {
  root_ = StackPanel();
  root_.Spacing(8);
  deadline_ = MakeLine(hstring{}, colors::MakeBrush(colors::kProGoldLight));
  root_.Children().Append(deadline_);
  if (!compact) {
    StackPanel timeline;
    timeline.Spacing(8);
    timeline.Margin(Thickness{0, 4, 0, 0});
    TextBlock today{nullptr};
    timeline.Children().Append(MakeTimelineRow(Loc("offer_timeline_today"), today));
    today.Text(Loc("offer_timeline_trial_starts"));
    TextBlock reminder{nullptr};
    timeline.Children().Append(MakeTimelineRow(
        hstring{Format("offer_timeline_day", kFreeTrialDays - kReminderDaysBeforeCharge)},
        reminder));
    reminder.Text(Loc("offer_timeline_reminder"));
    timeline.Children().Append(
        MakeTimelineRow(hstring{Format("offer_timeline_day", kFreeTrialDays)}, chargeLine_));
    root_.Children().Append(timeline);
  }
  terms_ = MakeLine(hstring{}, colors::MutedBrush());
  terms_.Margin(Thickness{0, 4, 0, 0});
  root_.Children().Append(terms_);
  return root_;
}

void OfferLines::Update(const OfferView& offer, const PriceTierView& tier, int64_t trialDays) {
  if (!root_) return;
  const std::wstring first = std::wstring{Money(offer.firstYear, offer.currency)};
  const std::wstring regular = std::wstring{
      Money(0 < offer.regularYear ? offer.regularYear : tier.yearly,
            offer.currency.empty() ? tier.currency : offer.currency)};
  const hstring deadline = OfferDeadlineText(offer);
  deadline_.Text(deadline);
  deadline_.Visibility(deadline.empty() ? Visibility::Collapsed : Visibility::Visible);
  if (chargeLine_) chargeLine_.Text(hstring{Format("offer_timeline_first_charge", first)});
  terms_.Text(hstring{Format("offer_terms_first_year", trialDays, first, regular)});
}

}  // namespace urnw
