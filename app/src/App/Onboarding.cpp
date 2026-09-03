// SPDX-License-Identifier: MPL-2.0
#include "pch.h"

#include "Onboarding.h"

#include <algorithm>
#include <cwctype>
#include <random>

#include <winrt/Microsoft.UI.Xaml.Automation.Peers.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Windows.UI.ViewManagement.h>

#include "Localization.h"
#include "PageContext.h"
#include "ReferralCard.h"
#include "SdkHost.h"
#include "SettingsSheets.h"
#include "StatsFormat.h"
#include "Strings.h"
#include "UrColors.h"
#include "UsageBar.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::UI::Xaml::Media;
using namespace winrt::Microsoft::UI::Xaml::Media::Animation;
using winrt::Microsoft::UI::Xaml::Media::Imaging::BitmapImage;
using winrt::Windows::Foundation::IInspectable;
using winrt::Windows::Foundation::Point;
using winrt::Windows::Foundation::Rect;
using winrt::Windows::Foundation::TimeSpan;
using winrt::Windows::Foundation::Uri;
using ShapeEllipse = winrt::Microsoft::UI::Xaml::Shapes::Ellipse;
using ShapeLine = winrt::Microsoft::UI::Xaml::Shapes::Line;
using ShapeRectangle = winrt::Microsoft::UI::Xaml::Shapes::Rectangle;
using urnw::pages::Balance;
using urnw::pages::H;
using urnw::pages::Loc;
using urnw::pages::Sdk;

namespace urnw {
namespace {

// the walk: 1% -> 94% of the line over 6.5 s, visible from 4% to 88% (the docs
// traveller, react/src/pages/Docs.jsx RouteLine)
constexpr int kTripMillis = 6500;
constexpr double kWalkerDip = 40;
constexpr double kRouteConnectorDip = 72;
constexpr double kHeaderConnectorDip = 34;
constexpr double kRouteHeightDip = 76;
constexpr int kFlightMillis = 520;

constexpr const wchar_t* kPeople[] = {L"ms-appx:///Assets/UrPerson1.png",
                                      L"ms-appx:///Assets/UrPerson2.png",
                                      L"ms-appx:///Assets/UrPerson3.png"};
constexpr const wchar_t* kConnector = L"ms-appx:///Assets/app.ico";

constexpr winrt::Windows::UI::Color kTransparent{0, 0, 0, 0};

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

// The page title: the brand display face, as on every Android page.
TextBlock MakeTitle(hstring const& text) {
  auto title = MakeText(text, 32, colors::TextBrush(), true);
  title.FontFamily(FontResource(L"UrHeadingFontFamily"));
  title.FontWeight(winrt::Windows::UI::Text::FontWeights::Bold());
  return title;
}

// The pixel face (PP NeueBit) the flow uses for its lead lines and bullets.
TextBlock MakeLead(hstring const& text, double size = 22) {
  auto lead = MakeText(text, size, colors::TextBrush(), true);
  lead.FontFamily(FontResource(L"UrWordmarkFontFamily"));
  lead.FontWeight(winrt::Windows::UI::Text::FontWeights::Bold());
  return lead;
}

// A green-dot bullet in the referral page's style: the dot sits on the first
// line, a wrapped bullet stays left-aligned.
StackPanel MakeBullet(hstring const& text) {
  StackPanel row;
  row.Orientation(Orientation::Horizontal);
  row.Spacing(8);
  ShapeEllipse dot;
  dot.Width(8);
  dot.Height(8);
  dot.Fill(colors::MakeBrush(colors::kUrGreen));
  dot.VerticalAlignment(VerticalAlignment::Top);
  dot.Margin(Thickness{0, 9, 0, 0});
  row.Children().Append(dot);
  auto line = MakeLead(text, 20);
  line.MaxWidth(520);
  row.Children().Append(line);
  return row;
}

Button MakeTextButton(hstring const& text, Brush const& foreground) {
  Button button;
  button.Content(winrt::box_value(text));
  button.Background(colors::MakeBrush(kTransparent));
  button.BorderThickness(Thickness{0, 0, 0, 0});
  button.Foreground(foreground);
  button.FontSize(14);
  button.Padding(Thickness{10, 6, 10, 6});
  return button;
}

Button MakePrimaryButton(hstring const& text) {
  Button button;
  button.Content(winrt::box_value(text));
  button.HorizontalAlignment(HorizontalAlignment::Stretch);
  if (auto style = rows::Lookup(L"UrPrimaryButtonStyle")) button.Style(style);
  return button;
}

Button MakeSecondaryButton(hstring const& text) {
  Button button;
  button.Content(winrt::box_value(text));
  button.HorizontalAlignment(HorizontalAlignment::Stretch);
  if (auto style = rows::Lookup(L"UrSecondaryButtonStyle")) button.Style(style);
  return button;
}

hstring Upper(hstring const& text) {
  std::wstring upper{text};
  for (auto& ch : upper) ch = static_cast<wchar_t>(std::towupper(ch));
  return hstring{upper};
}

// The whole gibibytes for prose ("30 GiB"); anything else in the compact form.
std::wstring FormatDailyAllowance(int64_t byteCount) {
  constexpr int64_t kGib = 1024LL * 1024LL * 1024LL;
  if (0 < byteCount && byteCount % kGib == 0) {
    return std::to_wstring(byteCount / kGib) + L" GiB";
  }
  return Widen(FormatByteCountCompact(byteCount));
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

std::shared_ptr<Onboarding> Onboarding::Create(Grid host, Actions actions) {
  return std::shared_ptr<Onboarding>(new Onboarding(host, std::move(actions)));
}

// ---- lifecycle ---------------------------------------------------------------

void Onboarding::Show() {
  if (!built_) Build();
  visible_ = true;
  balance_ = Balance().Current();
  host_.Visibility(Visibility::Visible);
  ShowStep(1);
}

void Onboarding::Hide() {
  visible_ = false;
  StopTrip();
  if (flight_) flight_.Stop();
  if (haloStoryboard_) haloStoryboard_.Stop();
  host_.Visibility(Visibility::Collapsed);
}

void Onboarding::OnBalance(BalanceSnapshot const& snapshot) {
  balance_ = snapshot;
  if (!built_) return;
  ApplyBandwidth();
  ApplyReferral();
}

// ---- shell -------------------------------------------------------------------

void Onboarding::Build() {
  built_ = true;
  animations_ = winrt::Windows::UI::ViewManagement::UISettings().AnimationsEnabled();

  host_.Children().Clear();
  host_.RowDefinitions().Clear();
  RowDefinition topRow;
  topRow.Height(GridLength{0, GridUnitType::Auto});
  host_.RowDefinitions().Append(topRow);
  RowDefinition pageRow;
  pageRow.Height(GridLength{1, GridUnitType::Star});
  host_.RowDefinitions().Append(pageRow);

  // the top bar: back | [connector] bubbles | Skip
  Grid topBar;
  topBar.Height(56);
  topBar.Padding(Thickness{8, 0, 8, 0});
  ColumnDefinition left, centre, right;
  left.Width(GridLength{48, GridUnitType::Pixel});
  centre.Width(GridLength{1, GridUnitType::Star});
  right.Width(GridLength{0, GridUnitType::Auto});
  topBar.ColumnDefinitions().Append(left);
  topBar.ColumnDefinitions().Append(centre);
  topBar.ColumnDefinitions().Append(right);

  backButton_ = Button();
  backButton_.Background(colors::MakeBrush(kTransparent));
  backButton_.BorderThickness(Thickness{0, 0, 0, 0});
  backButton_.Width(40);
  backButton_.Height(40);
  backButton_.VerticalAlignment(VerticalAlignment::Center);
  FontIcon backIcon;
  backIcon.Glyph(L"\xE76B");
  backIcon.FontSize(14);
  backButton_.Content(backIcon);
  winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(backButton_, Loc("back"));
  backButton_.Click([weak = weak_from_this()](auto const&, auto const&) {
    if (auto self = weak.lock()) {
      if (1 < self->step_) self->ShowStep(self->step_ - 1);
    }
  });
  topBar.Children().Append(backButton_);

  StackPanel middle;
  middle.Orientation(Orientation::Horizontal);
  middle.Spacing(10);
  middle.HorizontalAlignment(HorizontalAlignment::Center);
  middle.VerticalAlignment(VerticalAlignment::Center);
  headerSlot_ = Border();
  headerSlot_.Width(kHeaderConnectorDip);
  headerSlot_.Height(kHeaderConnectorDip);
  middle.Children().Append(headerSlot_);
  bubbles_ = StackPanel();
  bubbles_.Orientation(Orientation::Horizontal);
  bubbles_.Spacing(6);
  bubbles_.VerticalAlignment(VerticalAlignment::Center);
  middle.Children().Append(bubbles_);
  Grid::SetColumn(middle, 1);
  topBar.Children().Append(middle);

  // a small muted Skip: leaves the whole flow, so nobody feels captive
  skipButton_ = MakeTextButton(Loc("skip"), colors::MutedBrush());
  skipButton_.VerticalAlignment(VerticalAlignment::Center);
  skipButton_.Click([weak = weak_from_this()](auto const&, auto const&) {
    if (auto self = weak.lock()) {
      if (self->actions_.finish) self->actions_.finish();
    }
  });
  Grid::SetColumn(skipButton_, 2);
  topBar.Children().Append(skipButton_);
  host_.Children().Append(topBar);

  // the page column
  scroll_ = ScrollViewer();
  scroll_.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
  Grid::SetRow(scroll_, 1);
  pageHost_ = ContentControl();
  pageHost_.HorizontalContentAlignment(HorizontalAlignment::Stretch);
  pageHost_.VerticalContentAlignment(VerticalAlignment::Stretch);
  pageHost_.HorizontalAlignment(HorizontalAlignment::Center);
  pageHost_.MaxWidth(560);
  pageHost_.Margin(Thickness{24, 8, 24, 24});
  scroll_.Content(pageHost_);
  host_.Children().Append(scroll_);

  // the connector in flight, above everything
  flightCanvas_ = Canvas();
  flightCanvas_.IsHitTestVisible(false);
  Grid::SetRowSpan(flightCanvas_, 2);
  flyer_ = Image();
  flyer_.Source(BitmapImage{Uri{kConnector}});
  flyer_.Visibility(Visibility::Collapsed);
  flightCanvas_.Children().Append(flyer_);
  host_.Children().Append(flightCanvas_);
  host_.SizeChanged([weak = weak_from_this()](auto const&, auto const&) {
    if (auto self = weak.lock()) self->ParkFlyer();
  });

  welcome_ = BuildWelcome();
  bandwidth_ = BuildBandwidth();
  providing_ = BuildProviding();
  referral_ = BuildReferral();
}

void Onboarding::ShowStep(int step) {
  const int previous = step_;
  step_ = std::clamp(step, 1, kStepCount);

  // the connector: measure where it is leaving from before the page changes
  const bool leavingRoute = previous == 1 && step_ != 1 && visible_;
  const bool returningToRoute = previous != 1 && step_ == 1 && visible_ && flying_;
  Rect from{};
  if (leavingRoute && heroSlot_) from = SlotRect(heroSlot_);
  if (returningToRoute && headerSlot_) from = SlotRect(headerSlot_);

  if (previous == 1 && step_ != 1) StopTrip();

  StackPanel page{nullptr};
  switch (step_) {
    case 1: page = welcome_; break;
    case 2: page = bandwidth_; break;
    case 3: page = providing_; break;
    default: page = referral_; break;
  }
  pageHost_.Content(page);
  scroll_.ChangeView(nullptr, winrt::Windows::Foundation::IReference<double>{0.0}, nullptr, true);
  ApplyTopBar();
  if (step_ == 2) ApplyBandwidth();
  if (step_ == 4) ApplyReferral();

  if (animations_) {
    page.Opacity(0);
    DoubleAnimation fade;
    fade.From(0.0);
    fade.To(1.0);
    fade.Duration(MillisDuration(220));
    Storyboard::SetTarget(fade, page);
    Storyboard::SetTargetProperty(fade, L"Opacity");
    Storyboard storyboard;
    storyboard.Children().Append(fade);
    storyboard.Begin();
  } else {
    page.Opacity(1);
  }

  host_.UpdateLayout();
  if (leavingRoute) {
    PlaceFlyer(from);
    FlyConnector(/*toHeader=*/true);
  } else if (returningToRoute) {
    PlaceFlyer(from);
    FlyConnector(/*toHeader=*/false);
  } else if (step_ == 1) {
    if (routeConnector_) routeConnector_.Visibility(Visibility::Visible);
    flyer_.Visibility(Visibility::Collapsed);
    flying_ = false;
  } else {
    ParkFlyer();
  }
  if (step_ == 1) StartTrip();
}

void Onboarding::ApplyTopBar() {
  backButton_.Visibility(1 < step_ ? Visibility::Visible : Visibility::Collapsed);
  // the header slot only exists after page 1: page 1 keeps the connector large
  // in its route line
  headerSlot_.Visibility(1 < step_ ? Visibility::Visible : Visibility::Collapsed);
  ApplyBubbles();
}

void Onboarding::ApplyBubbles() {
  bubbles_.Children().Clear();
  for (int index = 1; index <= kStepCount; ++index) {
    ShapeRectangle bubble;
    const bool current = index == step_;
    bubble.Width(current ? 22 : 8);
    bubble.Height(8);
    bubble.RadiusX(4);
    bubble.RadiusY(4);
    bubble.Fill(current   ? colors::MakeBrush(colors::kOffWhite)
                : index < step_ ? colors::MakeBrush(colors::WithAlpha(colors::kOffWhite, 0x8C))
                                : colors::MakeBrush(colors::kTextFaint));
    bubbles_.Children().Append(bubble);
  }
  winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(
      bubbles_, hstring{Format("onboarding_step_of", static_cast<int64_t>(step_),
                               static_cast<int64_t>(kStepCount))});
}

// ---- the connector's flight ----------------------------------------------------

Rect Onboarding::SlotRect(FrameworkElement const& slot) {
  try {
    const auto origin = slot.TransformToVisual(host_).TransformPoint(Point{0, 0});
    return Rect{origin.X, origin.Y, static_cast<float>(slot.ActualWidth()),
                static_cast<float>(slot.ActualHeight())};
  } catch (...) {
    return Rect{};
  }
}

void Onboarding::PlaceFlyer(Rect const& rect) {
  Canvas::SetLeft(flyer_, rect.X);
  Canvas::SetTop(flyer_, rect.Y);
  flyer_.Width(rect.Width);
  flyer_.Height(rect.Height);
  flyer_.Visibility(Visibility::Visible);
  flying_ = true;
}

// After a resize, or a page change without a flight: the connector sits on
// the header slot, no animation.
void Onboarding::ParkFlyer() {
  if (!visible_ || step_ == 1 || !headerSlot_) return;
  if (flight_) flight_.Stop();
  const auto target = SlotRect(headerSlot_);
  if (target.Width <= 0) return;
  PlaceFlyer(target);
}

void Onboarding::FlyConnector(bool toHeader) {
  if (routeConnector_) routeConnector_.Visibility(Visibility::Collapsed);
  const auto target = toHeader ? SlotRect(headerSlot_) : SlotRect(heroSlot_);
  if (target.Width <= 0 || !animations_) {
    if (toHeader) {
      PlaceFlyer(target);
    } else {
      flyer_.Visibility(Visibility::Collapsed);
      flying_ = false;
      if (routeConnector_) routeConnector_.Visibility(Visibility::Visible);
    }
    return;
  }

  if (flight_) flight_.Stop();
  flight_ = Storyboard();
  auto animate = [&](hstring const& property, double to) {
    DoubleAnimation animation;
    animation.To(to);
    animation.Duration(MillisDuration(kFlightMillis));
    animation.EnableDependentAnimation(true);
    CubicEase ease;
    ease.EasingMode(EasingMode::EaseInOut);
    animation.EasingFunction(ease);
    Storyboard::SetTarget(animation, flyer_);
    Storyboard::SetTargetProperty(animation, property);
    flight_.Children().Append(animation);
  };
  animate(L"(Canvas.Left)", target.X);
  animate(L"(Canvas.Top)", target.Y);
  animate(L"Width", target.Width);
  animate(L"Height", target.Height);
  flight_.Completed([weak = weak_from_this(), toHeader](auto const&, auto const&) {
    auto self = weak.lock();
    if (!self) return;
    if (toHeader) {
      self->ParkFlyer();
    } else {
      // landed in the route: hand the mark back to the page, under the walker
      self->flyer_.Visibility(Visibility::Collapsed);
      self->flying_ = false;
      if (self->routeConnector_) self->routeConnector_.Visibility(Visibility::Visible);
    }
  });
  flight_.Begin();
}

// ---- page 1: welcome ----------------------------------------------------------

FrameworkElement Onboarding::BuildRoute() {
  // You - - - [connector] - - - Internet, with the walker above it all
  Grid route;
  route.Height(kRouteHeightDip);
  ColumnDefinition you, lineA, mark, lineB, internet;
  you.Width(GridLength{0, GridUnitType::Auto});
  lineA.Width(GridLength{1, GridUnitType::Star});
  mark.Width(GridLength{0, GridUnitType::Auto});
  lineB.Width(GridLength{1, GridUnitType::Star});
  internet.Width(GridLength{0, GridUnitType::Auto});
  route.ColumnDefinitions().Append(you);
  route.ColumnDefinitions().Append(lineA);
  route.ColumnDefinitions().Append(mark);
  route.ColumnDefinitions().Append(lineB);
  route.ColumnDefinitions().Append(internet);

  auto stop = [&](hstring const& text, winrt::Windows::UI::Color color, int column) {
    auto label = MakeText(text, 13, colors::MakeBrush(colors::WithAlpha(color, 0xE6)));
    label.FontFamily(FontFamily{L"Cascadia Mono, Consolas"});
    label.VerticalAlignment(VerticalAlignment::Center);
    label.Padding(
        Thickness{column == 0 ? 0.0 : 8.0, 2.0, column == 4 ? 0.0 : 8.0, 2.0});
    Grid::SetColumn(label, column);
    route.Children().Append(label);
  };
  auto dash = [&](int column) {
    ShapeLine line;
    line.X1(0);
    line.X2(1);
    line.Y1(0);
    line.Y2(0);
    line.Stretch(Stretch::Fill);
    line.StrokeThickness(1);
    line.Stroke(colors::MakeBrush(colors::WithAlpha(colors::kUrPink, 0x66)));
    DoubleCollection dashes;
    dashes.Append(6);
    dashes.Append(5);
    line.StrokeDashArray(dashes);
    line.VerticalAlignment(VerticalAlignment::Center);
    line.HorizontalAlignment(HorizontalAlignment::Stretch);
    line.Margin(Thickness{6, 0, 6, 0});
    Grid::SetColumn(line, column);
    route.Children().Append(line);
  };
  stop(Loc("route_you"), colors::kUrLightBlue, 0);
  dash(1);
  dash(3);
  stop(Loc("route_internet"), colors::kOffWhite, 4);

  // the connector: the page draws it here; the host flies it into the header
  // on leaving this page
  heroSlot_ = Border();
  heroSlot_.Width(kRouteConnectorDip);
  heroSlot_.Height(kRouteConnectorDip);
  heroSlot_.Background(colors::MakeBrush(colors::kBackground));  // masks the line
  routeConnector_ = Image();
  routeConnector_.Source(BitmapImage{Uri{kConnector}});
  routeConnector_.Width(kRouteConnectorDip);
  routeConnector_.Height(kRouteConnectorDip);
  heroSlot_.Child(routeConnector_);
  heroSlot_.VerticalAlignment(VerticalAlignment::Center);
  Grid::SetColumn(heroSlot_, 2);
  route.Children().Append(heroSlot_);

  // the walker rides a canvas over the whole route
  walkerCanvas_ = Canvas();
  walkerCanvas_.Height(kRouteHeightDip);
  walkerCanvas_.IsHitTestVisible(false);
  Grid::SetColumnSpan(walkerCanvas_, 5);
  walker_ = Grid();
  walker_.Width(kWalkerDip);
  walker_.Height(kWalkerDip);
  walker_.Opacity(0);
  // a soft white glow, then the tile on top
  ShapeEllipse glow;
  glow.Margin(Thickness{-8, -8, -8, -8});
  RadialGradientBrush glowBrush;
  GradientStop glowCentre;
  glowCentre.Color(colors::WithAlpha(colors::kOffWhite, 0x40));
  glowCentre.Offset(0.45);
  glowBrush.GradientStops().Append(glowCentre);
  GradientStop glowEdge;
  glowEdge.Color(colors::WithAlpha(colors::kOffWhite, 0));
  glowEdge.Offset(1);
  glowBrush.GradientStops().Append(glowEdge);
  glow.Fill(glowBrush);
  walker_.Children().Append(glow);
  walkerImage_ = Image();
  walkerImage_.Width(kWalkerDip);
  walkerImage_.Height(kWalkerDip);
  walkerImage_.Source(BitmapImage{Uri{kPeople[0]}});
  walker_.Children().Append(walkerImage_);
  Canvas::SetTop(walker_, (kRouteHeightDip - kWalkerDip) / 2);
  walkerCanvas_.Children().Append(walker_);
  walkerCanvas_.SizeChanged([weak = weak_from_this()](auto const&, auto const&) {
    if (auto self = weak.lock()) {
      const double before = self->travel_;
      self->travel_ = std::max(0.0, self->walkerCanvas_.ActualWidth() - kWalkerDip);
      if (!self->animations_) {
        self->PlaceWalker(0.85);
      } else if (before <= 0 && 0 < self->travel_ && self->trip_) {
        // the first trip started before the route had a width: walk it now
        self->StartTrip();
      }
    }
  });
  route.Children().Append(walkerCanvas_);

  winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::SetAccessibilityView(
      route, winrt::Microsoft::UI::Xaml::Automation::Peers::AccessibilityView::Raw);
  return route;
}

void Onboarding::PlaceWalker(double fraction) {
  Canvas::SetLeft(walker_, travel_ * fraction);
  walker_.Opacity(1);
}

// One trip per storyboard, restarted on completion with the next member of a
// shuffled deck, so the swap happens while the walker is faded out and the
// clock never drifts from the walk.
void Onboarding::StartTrip() {
  if (!walker_ || !visible_ || step_ != 1) return;
  if (!animations_) {
    // rests near the internet end, wearing person 0
    walkerImage_.Source(BitmapImage{Uri{kPeople[0]}});
    PlaceWalker(0.85);
    return;
  }
  if (trip_) trip_.Stop();
  trip_ = Storyboard();

  DoubleAnimation walk;
  walk.From(travel_ * 0.01);
  walk.To(travel_ * 0.94);
  walk.Duration(MillisDuration(kTripMillis));
  walk.EnableDependentAnimation(true);
  Storyboard::SetTarget(walk, walker_);
  Storyboard::SetTargetProperty(walk, L"(Canvas.Left)");
  trip_.Children().Append(walk);

  DoubleAnimationUsingKeyFrames fade;
  auto keyFrame = [&](double at, double value) {
    LinearDoubleKeyFrame frame;
    frame.KeyTime(KeyTime{Millis(static_cast<int>(kTripMillis * at))});
    frame.Value(value);
    fade.KeyFrames().Append(frame);
  };
  keyFrame(0.0, 0);
  keyFrame(0.04, 1);
  keyFrame(0.88, 1);
  keyFrame(0.94, 0);
  keyFrame(1.0, 0);
  Storyboard::SetTarget(fade, walker_);
  Storyboard::SetTargetProperty(fade, L"Opacity");
  trip_.Children().Append(fade);

  trip_.Completed([weak = weak_from_this()](auto const&, auto const&) {
    auto self = weak.lock();
    if (!self || !self->visible_ || self->step_ != 1) return;
    // the deck: every member once, in a random order, before any repeats
    if (self->deck_.empty()) {
      for (int i = 0; i < static_cast<int>(std::size(kPeople)); ++i) {
        if (i != self->person_) self->deck_.push_back(i);
      }
      std::shuffle(self->deck_.begin(), self->deck_.end(), std::mt19937{std::random_device{}()});
    }
    self->person_ = self->deck_.back();
    self->deck_.pop_back();
    self->walkerImage_.Source(BitmapImage{Uri{kPeople[self->person_]}});
    self->StartTrip();
  });
  trip_.Begin();
}

void Onboarding::StopTrip() {
  if (trip_) {
    trip_.Stop();
    trip_ = nullptr;
  }
  if (walker_) walker_.Opacity(0);
}

// only the yearly plan carries the trial: the button says what the click does
void Onboarding::ApplyPlanCta(bool yearly) {
  if (checkoutButton_) checkoutButton_.Content(winrt::box_value(PlanPicker::CtaLabel(yearly)));
}

StackPanel Onboarding::BuildWelcome() {
  StackPanel page;
  page.Spacing(0);

  page.Children().Append(BuildRoute());
  auto routeGap = Border();
  routeGap.Height(20);
  page.Children().Append(routeGap);

  page.Children().Append(MakeTitle(Loc("welcome_to_urnetwork")));
  auto tagline = MakeLead(Loc("intro_verifiable_encryption"));
  tagline.Margin(Thickness{0, 16, 0, 0});
  page.Children().Append(tagline);

  // the plans (the shared picker): room for the halo and the pill, and air
  // after the tagline
  auto plans = plans_.Build();
  plans.Margin(Thickness{0, 52, 0, 0});
  page.Children().Append(plans);
  plans_.onSelect = [weak = weak_from_this()](bool yearly) {
    if (auto self = weak.lock()) self->ApplyPlanCta(yearly);
  };

  checkoutButton_ = MakePrimaryButton(PlanPicker::CtaLabel(plans_.Yearly()));
  checkoutButton_.Margin(Thickness{0, 16, 0, 0});
  checkoutButton_.Click([weak = weak_from_this()](auto const&, auto const&) {
    if (auto self = weak.lock()) {
      if (self->actions_.startCheckout) self->actions_.startCheckout(self->plans_.Yearly());
    }
  });
  page.Children().Append(checkoutButton_);

  // the other ways in, as quiet links at the bottom: the screen is about
  // starting the free trial
  StackPanel links;
  links.Spacing(4);
  links.HorizontalAlignment(HorizontalAlignment::Center);
  links.Margin(Thickness{0, 40, 0, 0});
  auto community = MakeTextButton(Loc("community_edition"), colors::MutedBrush());
  community.HorizontalAlignment(HorizontalAlignment::Center);
  community.Click([weak = weak_from_this()](auto const&, auto const&) {
    if (auto self = weak.lock()) self->ShowStep(2);
  });
  links.Children().Append(community);
  auto redeem = MakeTextButton(Loc("redeem_balance_code"), colors::MutedBrush());
  redeem.HorizontalAlignment(HorizontalAlignment::Center);
  redeem.Click([weak = weak_from_this()](auto const&, auto const&) {
    if (auto self = weak.lock()) {
      if (self->actions_.redeemCode) self->actions_.redeemCode();
    }
  });
  links.Children().Append(redeem);
  page.Children().Append(links);

  if (animations_) {
    haloStoryboard_ = PulseOpacity(plans_.Halo(), 0.6, 1.0, 2200);
    haloStoryboard_.Begin();
  }
  return page;
}

// ---- page 2: your bandwidth ------------------------------------------------------

StackPanel Onboarding::BuildBandwidth() {
  StackPanel page;
  page.Spacing(16);
  page.Children().Append(MakeTitle(Loc("your_bandwidth")));
  page.Children().Append(MakeLead(Loc("you_get_free_data_every_day")));

  StackPanel bar;
  bar.Spacing(4);
  bar.Margin(Thickness{0, 16, 0, 0});
  Grid barHost;
  barHost.Height(12);
  bar.Children().Append(barHost);
  StackPanel legend;
  legend.Orientation(Orientation::Horizontal);
  legend.Spacing(8);
  bar.Children().Append(legend);
  usageBar_ = std::make_unique<UsageBar>(barHost, legend);

  Grid daily;
  ColumnDefinition label, value;
  label.Width(GridLength{1, GridUnitType::Star});
  value.Width(GridLength{0, GridUnitType::Auto});
  daily.ColumnDefinitions().Append(label);
  daily.ColumnDefinitions().Append(value);
  daily.Margin(Thickness{0, 8, 0, 0});
  daily.Children().Append(MakeText(Loc("daily_data_balance_label"), 14, colors::MutedBrush()));
  dailyValue_ = MakeText(hstring{L""}, 14, colors::MutedBrush());
  Grid::SetColumn(dailyValue_, 1);
  daily.Children().Append(dailyValue_);
  bar.Children().Append(daily);
  page.Children().Append(bar);

  // the free allowance the server grants (pro.yml free.data per data_period),
  // never a number typed into the app; hidden until the balance has loaded
  dailyLine_ = MakeText(hstring{L""}, 16, colors::TextBrush(), true);
  dailyLine_.Margin(Thickness{0, 16, 0, 0});
  dailyLine_.Visibility(Visibility::Collapsed);
  page.Children().Append(dailyLine_);

  auto next = MakePrimaryButton(Loc("next"));
  next.Margin(Thickness{0, 32, 0, 0});
  next.Click([weak = weak_from_this()](auto const&, auto const&) {
    if (auto self = weak.lock()) self->ShowStep(3);
  });
  page.Children().Append(next);
  return page;
}

void Onboarding::ApplyBandwidth() {
  if (!usageBar_) return;
  usageBar_->Update(balance_.usedByteCount, balance_.pendingByteCount,
                    balance_.availableByteCount);
  dailyValue_.Text(H(FormatByteCountCompact(balance_.startBalanceByteCount)));
  if (0 < balance_.startBalanceByteCount) {
    const std::wstring amount = FormatDailyAllowance(balance_.startBalanceByteCount);
    dailyLine_.Text(hstring{Format("default_daily_data", amount)});
    dailyLine_.Visibility(Visibility::Visible);
  } else {
    dailyLine_.Visibility(Visibility::Collapsed);
  }
}

// ---- page 3: contribute bandwidth ---------------------------------------------------

StackPanel Onboarding::BuildProviding() {
  StackPanel page;
  page.Spacing(16);
  page.Children().Append(MakeTitle(Loc("reliability_settings")));
  page.Children().Append(MakeLead(Loc("provide_intro_lead")));
  page.Children().Append(MakeBullet(Loc("provide_intro_bullet_devices")));
  page.Children().Append(MakeBullet(Loc("provide_intro_bullet_people")));

  // the provide mode picker, with one line under each option saying what it
  // does; Never needs none
  Border card;
  card.CornerRadius(CornerRadius{12, 12, 12, 12});
  card.Background(colors::CardBrush());
  card.Padding(Thickness{16, 16, 16, 16});
  card.Margin(Thickness{0, 16, 0, 0});
  StackPanel body;
  body.Spacing(8);
  StackPanel headingRow;
  headingRow.Orientation(Orientation::Horizontal);
  headingRow.Spacing(8);
  ShapeEllipse indicator;
  indicator.Width(8);
  indicator.Height(8);
  indicator.Fill(colors::MakeBrush(colors::kUrCoral));
  indicator.VerticalAlignment(VerticalAlignment::Center);
  headingRow.Children().Append(indicator);
  headingRow.Children().Append(MakeText(Loc("provide_mode"), 14, colors::TextBrush()));
  body.Children().Append(headingRow);

  struct Mode {
    const char* value;
    const char* label;
    const char* description;  // nullptr: no line
  };
  static constexpr Mode kModes[] = {
      {"auto", "auto", "provide_mode_auto_description"},
      {"always", "always", "provide_mode_always_description"},
      {"network", "network", "provide_mode_network_description"},
      {"never", "never", nullptr},
  };
  const std::string current = Sdk().CurrentProvideControlMode();
  RadioButtons radios;
  int selected = 3;
  for (int i = 0; i < 4; ++i) {
    StackPanel item;
    item.Spacing(2);
    item.Children().Append(MakeText(Loc(kModes[i].label), 14, colors::TextBrush()));
    if (kModes[i].description) {
      item.Children().Append(MakeText(Loc(kModes[i].description), 12, colors::MutedBrush(), true));
    }
    radios.Items().Append(item);
    if (current == kModes[i].value) selected = i;
  }
  radios.SelectedIndex(selected);
  radios.SelectionChanged([](IInspectable const& sender, SelectionChangedEventArgs const&) {
    auto picker = sender.try_as<RadioButtons>();
    if (!picker) return;
    const int index = picker.SelectedIndex();
    if (index < 0 || 4 <= index) return;
    Sdk().SetProvideControlMode(kModes[index].value);
  });
  body.Children().Append(radios);
  card.Child(body);
  page.Children().Append(card);

  auto next = MakePrimaryButton(Loc("next"));
  next.Margin(Thickness{0, 32, 0, 0});
  next.Click([weak = weak_from_this()](auto const&, auto const&) {
    if (auto self = weak.lock()) self->ShowStep(4);
  });
  page.Children().Append(next);
  return page;
}

// ---- page 4: refer friends -----------------------------------------------------------

StackPanel Onboarding::BuildReferral() {
  StackPanel page;
  page.Spacing(16);
  page.Children().Append(MakeTitle(Loc("refer_friends_header")));
  page.Children().Append(MakeLead(Loc("when_you_refer_a_friend")));

  const auto terms = Balance().ReferralTerms();
  const std::wstring bonus = std::to_wstring(terms.bonusGibPerDay);
  const std::wstring referred = std::to_wstring(terms.referredBonusGibPerDay);
  page.Children().Append(MakeBullet(hstring{Format("refer_friends_perks", bonus)}));
  page.Children().Append(MakeBullet(hstring{Format("refer_friends_they_get_data", referred)}));

  // the progress box + the gold king-frog panel: the SHARED referral pieces
  // (the Account "Refer and earn" page shows the same two)
  referralCard_.Build(page, animations_);

  auto done = MakePrimaryButton(Loc("get_connected"));
  done.Margin(Thickness{0, 24, 0, 0});
  done.Click([weak = weak_from_this()](auto const&, auto const&) {
    if (auto self = weak.lock()) {
      if (self->actions_.finish) self->actions_.finish();
    }
  });
  page.Children().Append(done);
  return page;
}

void Onboarding::ApplyReferral() { referralCard_.Apply(); }

}  // namespace urnw
