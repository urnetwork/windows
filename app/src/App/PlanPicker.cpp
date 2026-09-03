// SPDX-License-Identifier: MPL-2.0
#include "pch.h"
#include "PlanPicker.h"

#include "Localization.h"
#include "PageContext.h"
#include "Strings.h"
#include "UrColors.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::UI::Xaml::Media;
using namespace winrt::Microsoft::UI::Xaml::Media::Animation;
using winrt::Windows::Foundation::Point;
using ShapeEllipse = winrt::Microsoft::UI::Xaml::Shapes::Ellipse;
using ShapeRectangle = winrt::Microsoft::UI::Xaml::Shapes::Rectangle;
using urnw::pages::Loc;

namespace urnw {
namespace {

constexpr winrt::Windows::UI::Color kTransparent{0, 0, 0, 0};

// The onboarding flow's text/gold helpers, carried here so the picker is
// self-contained (Onboarding.cpp keeps its own copies for its other pages).
FontFamily FontResource(std::wstring_view key) {
  try {
    auto resources = Application::Current().Resources();
    auto boxed = winrt::box_value(hstring{key});
    if (resources.HasKey(boxed)) return resources.Lookup(boxed).as<FontFamily>();
  } catch (...) {
  }
  return FontFamily{L"Segoe UI"};
}

TextBlock MakeText(hstring const& text, double size, Brush const& brush = nullptr,
                   bool wrap = false) {
  TextBlock block;
  block.Text(text);
  block.FontSize(size);
  if (brush) block.Foreground(brush);
  if (wrap) block.TextWrapping(TextWrapping::Wrap);
  return block;
}

// The pixel face (PP NeueBit) the flow uses for its lead lines.
TextBlock MakeLead(hstring const& text, double size = 22) {
  auto lead = MakeText(text, size, colors::TextBrush(), true);
  lead.FontFamily(FontResource(L"UrWordmarkFontFamily"));
  lead.FontWeight(winrt::Windows::UI::Text::FontWeights::Bold());
  return lead;
}

// Pro gold, top to bottom light -> gold (the Best value pill).
LinearGradientBrush GoldGradient(winrt::Windows::UI::Color light,
                                 winrt::Windows::UI::Color gold) {
  LinearGradientBrush brush;
  brush.StartPoint(Point{0, 0});
  brush.EndPoint(Point{0, 1});
  GradientStop top;
  top.Color(light);
  top.Offset(0);
  brush.GradientStops().Append(top);
  GradientStop bottom;
  bottom.Color(gold);
  bottom.Offset(1);
  brush.GradientStops().Append(bottom);
  return brush;
}

// A soft halo behind a box: a rounded rectangle filled with a radial gradient
// that reaches zero alpha well inside its own edge, so it never draws a hard
// box (measured on Android: a gradient that stops at the edge reads as a
// brown rectangle).
ShapeRectangle MakeHalo(winrt::Windows::UI::Color color, uint8_t alpha, double spill) {
  ShapeRectangle halo;
  halo.Margin(Thickness{-spill, -spill, -spill, -spill});
  halo.RadiusX(24);
  halo.RadiusY(24);
  halo.IsHitTestVisible(false);
  RadialGradientBrush brush;
  GradientStop centre;
  centre.Color(colors::WithAlpha(color, alpha));
  centre.Offset(0);
  brush.GradientStops().Append(centre);
  GradientStop mid;
  mid.Color(colors::WithAlpha(color, static_cast<uint8_t>(alpha / 3)));
  mid.Offset(0.55);
  brush.GradientStops().Append(mid);
  GradientStop edge;
  edge.Color(colors::WithAlpha(color, 0));
  edge.Offset(0.92);
  brush.GradientStops().Append(edge);
  halo.Fill(brush);
  return halo;
}

}  // namespace

Border PlanPicker::BuildCard(std::shared_ptr<State> const& state, bool yearly) {
  // the recommended plan wears the Pro-gold dress: halo, gold wash, gold
  // border, the Best value pill; the other card is the plain selectable card
  Border card;
  card.CornerRadius(CornerRadius{12, 12, 12, 12});
  card.BorderThickness(Thickness{2, 2, 2, 2});
  card.Padding(Thickness{20, 18, 20, 18});
  card.Background(yearly ? colors::MakeBrush(colors::kBackground) : colors::CardBrush());

  Grid row;
  ColumnDefinition c0, c1;
  c0.Width(GridLength{0, GridUnitType::Auto});
  c1.Width(GridLength{1, GridUnitType::Star});
  row.ColumnDefinitions().Append(c0);
  row.ColumnDefinitions().Append(c1);
  row.ColumnSpacing(14);

  ShapeEllipse dot;
  dot.Width(14);
  dot.Height(14);
  dot.StrokeThickness(2);
  dot.VerticalAlignment(VerticalAlignment::Center);
  row.Children().Append(dot);

  StackPanel labels;
  labels.Spacing(2);
  labels.VerticalAlignment(VerticalAlignment::Center);
  // the Stripe prices, as product literals in the store ($40 a year is a
  // third off twelve months at $5); the trial line is the yearly plan's only
  auto title = MakeLead(yearly ? Loc("plan_yearly_price") : Loc("plan_monthly_price"), 22);
  labels.Children().Append(title);
  if (yearly) {
    labels.Children().Append(MakeText(Loc("save_33_percent"), 13, colors::MutedBrush()));
    labels.Children().Append(
        MakeText(hstring{Format("includes_free_trial_days", kFreeTrialDays)}, 13,
                 colors::MakeBrush(colors::kProGoldLight)));
  }
  Grid::SetColumn(labels, 1);
  row.Children().Append(labels);

  if (yearly) {
    state->yearlyDot = dot;
    // the gold wash inside the black ground, under the row
    Border wash;
    wash.CornerRadius(CornerRadius{10, 10, 10, 10});
    wash.Background(colors::MakeBrush(colors::WithAlpha(colors::kProGold, 0x14)));
    wash.Margin(Thickness{-20, -18, -20, -18});
    wash.IsHitTestVisible(false);
    Grid dressed;
    dressed.Children().Append(wash);
    dressed.Children().Append(row);
    card.Child(dressed);
  } else {
    state->monthlyDot = dot;
    card.Child(row);
  }

  card.Tapped([weak = std::weak_ptr<State>(state), yearly](auto const&, auto const&) {
    if (auto self = weak.lock()) {
      self->yearly = yearly;
      Apply(*self);
      if (self->onSelect && *self->onSelect) (*self->onSelect)(yearly);
    }
  });
  return card;
}

void PlanPicker::Apply(State const& state) {
  auto apply = [](Border const& card, ShapeEllipse const& dot, bool selected, bool gold) {
    if (!card || !dot) return;
    const auto accent = gold ? colors::ProGoldBrush() : colors::MakeBrush(colors::kUrPink);
    card.BorderBrush(selected ? accent
                     : gold  ? colors::MakeBrush(colors::WithAlpha(colors::kProGold, 0x99))
                             : colors::MutedBrush());
    dot.Stroke(selected ? accent : colors::MutedBrush());
    dot.Fill(selected ? accent : colors::MakeBrush(kTransparent));
  };
  apply(state.yearlyCard, state.yearlyDot, state.yearly, true);
  apply(state.monthlyCard, state.monthlyDot, !state.yearly, false);
}

Grid PlanPicker::Build() {
  state_->onSelect = &onSelect;

  // the plans: room for the halo and the pill is the host's margin to give
  Grid plans;
  auto halo = MakeHalo(colors::kProGold, 0x5C, 28);
  halo.VerticalAlignment(VerticalAlignment::Top);
  plans.Children().Append(halo);
  state_->halo = halo;

  StackPanel cards;
  cards.Spacing(16);
  state_->yearlyCard = BuildCard(state_, /*yearly=*/true);
  Grid recommended;
  recommended.Children().Append(state_->yearlyCard);
  Border pill;
  pill.CornerRadius(CornerRadius{16, 16, 16, 16});
  pill.Padding(Thickness{16, 6, 16, 6});
  pill.HorizontalAlignment(HorizontalAlignment::Right);
  pill.VerticalAlignment(VerticalAlignment::Top);
  pill.Margin(Thickness{0, -16, 12, 0});
  pill.Background(GoldGradient(colors::kProGoldLight, colors::kProGold));
  pill.BorderThickness(Thickness{1, 1, 1, 1});
  pill.BorderBrush(colors::MakeBrush(colors::WithAlpha(colors::kOffWhite, 0x73)));
  auto pillText = MakeLead(Loc("best_value"), 20);
  pillText.Foreground(colors::MakeBrush(colors::kInverseText));
  pill.Child(pillText);
  recommended.Children().Append(pill);
  cards.Children().Append(recommended);
  state_->monthlyCard = BuildCard(state_, /*yearly=*/false);
  cards.Children().Append(state_->monthlyCard);
  plans.Children().Append(cards);

  // halo height follows the yearly card
  state_->yearlyCard.SizeChanged([halo](auto const&, SizeChangedEventArgs const& e) {
    halo.Height(e.NewSize().Height + 56);
  });

  Apply(*state_);
  return plans;
}

void PlanPicker::Select(bool yearly) {
  state_->yearly = yearly;
  Apply(*state_);
}

void PlanPicker::SetEnabled(bool enabled) {
  if (state_->yearlyCard) state_->yearlyCard.IsHitTestVisible(enabled);
  if (state_->monthlyCard) state_->monthlyCard.IsHitTestVisible(enabled);
}

hstring PlanPicker::CtaLabel(bool yearly) {
  return yearly ? Loc("start_free_trial") : Loc("subscribe");
}

Storyboard PlanPicker::HaloPulse() const {
  DoubleAnimation pulse;
  pulse.From(0.6);
  pulse.To(1.0);
  pulse.Duration(Duration{std::chrono::duration_cast<winrt::Windows::Foundation::TimeSpan>(
                              std::chrono::milliseconds(2200)),
                          DurationType::TimeSpan});
  pulse.AutoReverse(true);
  pulse.RepeatBehavior(RepeatBehavior{.Count = 0, .Duration = {}, .Type = RepeatBehaviorType::Forever});
  Storyboard::SetTarget(pulse, state_->halo);
  Storyboard::SetTargetProperty(pulse, L"Opacity");
  Storyboard storyboard;
  storyboard.Children().Append(pulse);
  return storyboard;
}

}  // namespace urnw
