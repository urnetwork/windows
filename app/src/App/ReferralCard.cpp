// SPDX-License-Identifier: MPL-2.0
#include "pch.h"
#include "ReferralCard.h"

#include <algorithm>
#include <cwctype>

#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>

#include "Localization.h"
#include "PageContext.h"
#include "SettingsSheets.h"
#include "Strings.h"
#include "SubscriptionBalance.h"
#include "UrColors.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::UI::Xaml::Media;
using namespace winrt::Microsoft::UI::Xaml::Media::Animation;
using winrt::Microsoft::UI::Xaml::Media::Imaging::BitmapImage;
using winrt::Windows::Foundation::Point;
using winrt::Windows::Foundation::TimeSpan;
using winrt::Windows::Foundation::Uri;
using ShapeRectangle = winrt::Microsoft::UI::Xaml::Shapes::Rectangle;
using urnw::pages::Balance;
using urnw::pages::H;
using urnw::pages::Loc;

namespace urnw {
namespace {

// The onboarding flow's text/gold helpers, carried here so the card is
// self-contained (Onboarding.cpp keeps its own copies for its other pages).
TimeSpan Millis(int millis) {
  return std::chrono::duration_cast<TimeSpan>(std::chrono::milliseconds(millis));
}

Duration MillisDuration(int millis) {
  return Duration{Millis(millis), DurationType::TimeSpan};
}

// A font family from App.xaml by key, or the default when it is missing.
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

// The pixel face (PP NeueBit) the flow uses for its lead lines and bullets.
TextBlock MakeLead(hstring const& text, double size = 22) {
  auto lead = MakeText(text, size, colors::TextBrush(), true);
  lead.FontFamily(FontResource(L"UrWordmarkFontFamily"));
  lead.FontWeight(winrt::Windows::UI::Text::FontWeights::Bold());
  return lead;
}

hstring Upper(hstring const& text) {
  std::wstring upper{text};
  for (auto& ch : upper) ch = static_cast<wchar_t>(std::towupper(ch));
  return hstring{upper};
}

// Pro gold, top to bottom light -> gold (the Best value pill, the copy button).
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

Storyboard PulseOpacity(DependencyObject const& target, double from, double to, int millis) {
  DoubleAnimation pulse;
  pulse.From(from);
  pulse.To(to);
  pulse.Duration(MillisDuration(millis));
  pulse.AutoReverse(true);
  pulse.RepeatBehavior(RepeatBehavior{.Count = 0, .Duration = {}, .Type = RepeatBehaviorType::Forever});
  Storyboard::SetTarget(pulse, target);
  Storyboard::SetTargetProperty(pulse, L"Opacity");
  Storyboard storyboard;
  storyboard.Children().Append(pulse);
  return storyboard;
}

}  // namespace

void ReferralCard::Build(Panel const& host, bool animations) {
  animations_ = animations;
  // the gold king-frog panel, rebuilt from the store's referral state; it is
  // the only referral progress bar on the onboarding step and the Referrals
  // page alike
  referralPanelHost_ = StackPanel();
  host.Children().Append(referralPanelHost_);
}

void ReferralCard::Apply() {
  if (!referralPanelHost_) return;
  const auto terms = Balance().ReferralTerms();
  const int64_t total = Balance().TotalReferrals();
  const auto code = Balance().ReferralCode();
  const std::string codeText = code ? *code : std::string();

  // "n/max" — or, once the code's cap is reached, the sentence that says so
  const bool capped = 0 < terms.maxReferrals && terms.maxReferrals <= total;

  if (codeText == shownReferralCode_ && total == shownReferralTotal_ &&
      terms.maxReferrals == shownReferralMax_ && referralPanelHost_.Children().Size() != 0) {
    return;
  }
  shownReferralCode_ = codeText;
  shownReferralTotal_ = total;
  shownReferralMax_ = terms.maxReferrals;
  referralPanelHost_.Children().Clear();

  const bool crowned = 0 < total;

  Grid panel;
  auto halo = MakeHalo(colors::kReferralGold, crowned ? 0x66 : 0x4D, 28);
  panel.Children().Append(halo);

  Border box;
  box.CornerRadius(CornerRadius{20, 20, 20, 20});
  box.BorderThickness(Thickness{1, 1, 1, 1});
  box.BorderBrush(colors::MakeBrush(colors::WithAlpha(colors::kReferralGold, crowned ? 0xBF : 0x66)));
  box.Background(colors::MakeBrush(colors::kBackground));
  box.Padding(Thickness{22, 28, 22, 28});
  Grid boxContent;
  Border wash;
  wash.CornerRadius(CornerRadius{19, 19, 19, 19});
  wash.Margin(Thickness{-22, -28, -22, -28});
  wash.Background(colors::MakeBrush(colors::WithAlpha(colors::kReferralGold, 0x1A)));
  wash.IsHitTestVisible(false);
  boxContent.Children().Append(wash);

  StackPanel content;
  content.Spacing(6);
  content.HorizontalAlignment(HorizontalAlignment::Stretch);

  Image frog;
  frog.Source(BitmapImage{Uri{L"ms-appx:///Assets/ReferralFrog.png"}});
  frog.Width(108);
  frog.Height(108);
  frog.HorizontalAlignment(HorizontalAlignment::Center);
  frog.Margin(Thickness{0, 0, 0, 12});
  content.Children().Append(frog);

  auto kicker = MakeText(Upper(Loc("referrals")), 11, colors::ReferralGoldBrush());
  kicker.FontWeight(winrt::Windows::UI::Text::FontWeights::Bold());
  kicker.CharacterSpacing(140);
  kicker.HorizontalAlignment(HorizontalAlignment::Center);
  content.Children().Append(kicker);

  auto heading = MakeLead(crowned ? Loc("referral_royalty") : Loc("referral_panel_heading"), 24);
  heading.TextAlignment(TextAlignment::Center);
  heading.HorizontalAlignment(HorizontalAlignment::Center);
  content.Children().Append(heading);

  auto detail = MakeText(hstring{Format("referral_panel_detail", terms.bonusGibPerDay)}, 14,
                         colors::MakeBrush(colors::WithAlpha(colors::kUrLightBlue, 0xD9)), true);
  detail.TextAlignment(TextAlignment::Center);
  content.Children().Append(detail);

  if (!codeText.empty()) {
    auto label = MakeText(Upper(Loc("your_referral_code")), 11,
                          colors::MakeBrush(colors::WithAlpha(colors::kUrLightBlue, 0x99)));
    label.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
    label.CharacterSpacing(80);
    label.HorizontalAlignment(HorizontalAlignment::Center);
    label.Margin(Thickness{0, 12, 0, 0});
    content.Children().Append(label);

    // the code in a dark pill with a dashed gold border and the gold copy
    // button inside it
    Grid pill;
    pill.HorizontalAlignment(HorizontalAlignment::Center);
    ShapeRectangle dashed;
    dashed.RadiusX(24);
    dashed.RadiusY(24);
    dashed.StrokeThickness(1);
    dashed.Stroke(colors::MakeBrush(colors::WithAlpha(colors::kReferralGold, 0x8C)));
    DoubleCollection dashes;
    dashes.Append(4);
    dashes.Append(4);
    dashed.StrokeDashArray(dashes);
    dashed.Fill(colors::MakeBrush(colors::WithAlpha(colors::kBackground, 0x59)));
    pill.Children().Append(dashed);
    StackPanel pillRow;
    pillRow.Orientation(Orientation::Horizontal);
    pillRow.Spacing(10);
    pillRow.Padding(Thickness{18, 8, 8, 8});
    auto codeBlock = MakeText(H(codeText), 18, colors::ReferralGoldLightBrush());
    codeBlock.FontWeight(winrt::Windows::UI::Text::FontWeights::Bold());
    codeBlock.VerticalAlignment(VerticalAlignment::Center);
    pillRow.Children().Append(codeBlock);
    Button copy;
    copy.Content(winrt::box_value(Loc("copy")));
    copy.CornerRadius(CornerRadius{18, 18, 18, 18});
    copy.Padding(Thickness{16, 8, 16, 8});
    copy.BorderThickness(Thickness{0, 0, 0, 0});
    copy.Background(GoldGradient(colors::kReferralGoldPale, colors::kReferralGold));
    copy.Foreground(colors::ReferralGoldInkBrush());
    copy.FontWeight(winrt::Windows::UI::Text::FontWeights::Bold());
    copy.Click([codeText, copy](auto const&, auto const&) {
      rows::CopyToClipboard(codeText);
      copy.Content(winrt::box_value(Loc("copied")));
    });
    pillRow.Children().Append(copy);
    pill.Children().Append(pillRow);
    content.Children().Append(pill);

    // windows has no share sheet: "share" copies the invite message, like the
    // account menu's share item
    Button share;
    share.Content(winrt::box_value(Loc("share")));
    share.HorizontalAlignment(HorizontalAlignment::Stretch);
    share.CornerRadius(CornerRadius{22, 22, 22, 22});
    share.Padding(Thickness{0, 12, 0, 12});
    share.BorderThickness(Thickness{0, 0, 0, 0});
    share.Background(GoldGradient(colors::kReferralGoldPale, colors::kReferralGold));
    share.Foreground(colors::ReferralGoldInkBrush());
    share.FontWeight(winrt::Windows::UI::Text::FontWeights::Bold());
    share.Margin(Thickness{0, 6, 0, 0});
    share.Click([codeText, share](auto const&, auto const&) {
      rows::CopyToClipboard(
          Narrow(Format("referral_share_message", Widen(codeText))));
      share.Content(winrt::box_value(Loc("copied")));
    });
    content.Children().Append(share);
  } else {
    ProgressRing ring;
    ring.Width(24);
    ring.Height(24);
    ring.IsActive(true);
    ring.Margin(Thickness{0, 12, 0, 0});
    ring.HorizontalAlignment(HorizontalAlignment::Center);
    content.Children().Append(ring);
  }

  // android ReferralProgressBar: 16 under the share button, a 6px gold track
  // that fills per friend joined out of the code's cap, "joined / cap" 6
  // under it (or the used-up sentence once the cap is reached), 12 to the
  // status line. The content stack's 6 spacing supplies the rest.
  {
    const int64_t cap = std::max<int64_t>(1, terms.maxReferrals);
    const int64_t joined = std::clamp<int64_t>(total, 0, cap);
    Grid track;
    track.Height(6);
    track.Margin(Thickness{0, 10, 0, 0});
    track.CornerRadius(CornerRadius{3, 3, 3, 3});
    track.Background(colors::MakeBrush(colors::WithAlpha(colors::kReferralGold, 0x2E)));
    ColumnDefinition joinedColumn, freeColumn;
    joinedColumn.Width(GridLength{std::max(0.0001, static_cast<double>(joined)), GridUnitType::Star});
    freeColumn.Width(GridLength{std::max(0.0001, static_cast<double>(cap - joined)), GridUnitType::Star});
    track.ColumnDefinitions().Append(joinedColumn);
    track.ColumnDefinitions().Append(freeColumn);
    if (0 < joined) {
      Border fill;
      fill.CornerRadius(CornerRadius{3, 3, 3, 3});
      fill.MinWidth(6);
      LinearGradientBrush gradient;
      gradient.StartPoint(Point{0, 0});
      gradient.EndPoint(Point{1, 0});
      GradientStop from;
      from.Color(colors::kReferralGold);
      from.Offset(0);
      gradient.GradientStops().Append(from);
      GradientStop to;
      to.Color(colors::kReferralGoldLight);
      to.Offset(1);
      gradient.GradientStops().Append(to);
      fill.Background(gradient);
      track.Children().Append(fill);
    }
    content.Children().Append(track);

    auto count = MakeText(
        capped ? Loc("referral_code_capped")
               : hstring{std::to_wstring(joined) + L" / " + std::to_wstring(cap)},
        12,
        capped ? colors::ReferralGoldLightBrush()
               : colors::MakeBrush(colors::WithAlpha(colors::kUrLightBlue, 0xB3)),
        true);
    count.TextAlignment(TextAlignment::Center);
    content.Children().Append(count);
  }

  TextBlock status;
  if (crowned) {
    status.Text(hstring{L"\U0001F451 " + PluralFormat("referral_crowned_congrats", total, total,
                                                       terms.EarnedGibPerDay(total))});
    status.Foreground(colors::ReferralGoldLightBrush());
    status.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
    status.FontSize(14);
  } else {
    status.Text(Loc("referral_panel_none"));
    status.Foreground(colors::MakeBrush(colors::WithAlpha(colors::kUrLightBlue, 0xB3)));
    status.FontSize(13);
  }
  status.TextWrapping(TextWrapping::Wrap);
  status.TextAlignment(TextAlignment::Center);
  status.Margin(Thickness{0, 6, 0, 0});
  content.Children().Append(status);

  boxContent.Children().Append(content);
  box.Child(boxContent);
  panel.Children().Append(box);
  referralPanelHost_.Children().Append(panel);

  if (animations_) {
    auto pulse = PulseOpacity(halo, 0.55, 0.9, crowned ? 1700 : 2500);
    pulse.Begin();
  }
}

}  // namespace urnw
