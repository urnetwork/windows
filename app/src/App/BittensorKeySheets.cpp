// SPDX-License-Identifier: MPL-2.0
// the project compiles with /Yu"pch.h" (App.vcxproj), so every translation unit
// must include it first
#include "pch.h"

#include "BittensorKeySheets.h"

#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Text.h>

#include <chrono>

#include "BittensorKeyStore.h"
#include "BittensorKeyViews.h"
#include "EarningsSheets.h"  // ShortAddress, FormatAlphaRao
#include "Localization.h"
#include "Log.h"
#include "MainWindow.xaml.h"
#include "PageContext.h"
#include "SheetFit.h"
#include "Strings.h"
#include "UrColors.h"
#include "UrComponents.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::UI::Xaml::Media;
using namespace urnw::pages;

namespace urnw {
namespace {

constexpr wchar_t kWarningGlyph[] = L"\uE7BA";  // Warning
constexpr wchar_t kInfoGlyph[] = L"\uE946";     // Info
constexpr wchar_t kHiddenGlyph[] = L"\uED1A";   // Hide: the words are covered
constexpr wchar_t kHelloGlyph[] = L"\uE928";    // Fingerprint: Windows Hello asks first
constexpr wchar_t kCopyGlyph[] = L"\uE8C8";     // Copy
constexpr wchar_t kCheckGlyph[] = L"\uE73E";    // CheckMark

int64_t NowMillis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// An app or system style by key, or null (AuthSheets.cpp's idiom).
Style StyleNamed(std::wstring_view key) {
  auto app = Application::Current();
  if (!app) return nullptr;
  return app.Resources().TryLookup(winrt::box_value(hstring{key})).try_as<Style>();
}

TextBlock MakeText(hstring const& text, double fontSize, Brush const& brush, bool semibold = false) {
  TextBlock block;
  block.Text(text);
  block.FontSize(fontSize);
  block.Foreground(brush);
  block.TextWrapping(TextWrapping::Wrap);
  if (semibold) block.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
  return block;
}

TextBlock Line(hstring const& text) { return MakeText(text, 13, colors::TextBrush()); }
TextBlock Muted(hstring const& text) { return MakeText(text, 13, colors::MutedBrush()); }
TextBlock Danger(hstring const& text) { return MakeText(text, 13, colors::DangerBrush()); }

// A glyph; with no brush it takes the foreground of what it sits in.
FontIcon Glyph(wchar_t const* glyph, double size, Brush const& brush) {
  FontIcon icon;
  icon.FontFamily(FontFamily(kit::kIconFontFamily));
  icon.Glyph(glyph);
  icon.FontSize(size);
  if (brush) icon.Foreground(brush);
  return icon;
}

// A notice in a tinted box: an icon or a spinner, then its content.
Border Notice(winrt::Windows::UI::Color tint, wchar_t const* glyph, bool spinner, UIElement const& content) {
  Grid grid;
  grid.ColumnSpacing(10);
  ColumnDefinition lead;
  lead.Width(GridLengthHelper::Auto());
  grid.ColumnDefinitions().Append(lead);
  grid.ColumnDefinitions().Append(ColumnDefinition());
  FrameworkElement mark{nullptr};
  if (spinner) {
    ProgressRing ring;
    ring.Width(16);
    ring.Height(16);
    ring.IsActive(true);
    mark = ring;
  } else {
    mark = Glyph(glyph, 16, colors::MakeBrush(tint));
  }
  mark.VerticalAlignment(VerticalAlignment::Top);
  Grid::SetColumn(mark, 0);
  grid.Children().Append(mark);
  if (auto element = content.try_as<FrameworkElement>()) Grid::SetColumn(element, 1);
  grid.Children().Append(content);
  Border box;
  box.Background(colors::MakeBrush(colors::WithAlpha(tint, 0x1F)));
  box.CornerRadius(CornerRadiusHelper::FromUniformRadius(8));
  box.Padding(Thickness{12, 12, 12, 12});
  box.Child(grid);
  return box;
}

// A plain sheet button, in the accent fill for the one main action.
Button SheetButton(hstring const& text, bool accent) {
  Button button;
  if (accent) {
    if (auto style = StyleNamed(L"AccentButtonStyle")) button.Style(style);
  }
  button.HorizontalAlignment(HorizontalAlignment::Stretch);
  button.MinHeight(40);
  button.Content(winrt::box_value(text));
  winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(button, text);
  return button;
}

// The destructive action: coral with dark text for contrast (white on coral is
// 2.8:1, dark 6.8:1), or a coral outline where it sits beside the safe choice.
// The template's own states read these resources first, so hover and press
// stay coral.
void MakeCoral(Button const& button, bool filled) {
  auto resources = button.Resources();
  auto put = [&resources](wchar_t const* key, winrt::Windows::UI::Color const& color) {
    resources.Insert(winrt::box_value(hstring{key}), SolidColorBrush(color));
  };
  const auto coral = colors::kUrCoral;
  const auto clear = winrt::Windows::UI::Color{0, 0, 0, 0};
  const auto ink = filled ? colors::kInverseText : coral;
  put(L"ButtonBackground", filled ? coral : clear);
  put(L"ButtonBackgroundPointerOver", filled ? colors::WithAlpha(coral, 0xE0) : colors::WithAlpha(coral, 0x1F));
  put(L"ButtonBackgroundPressed", filled ? colors::WithAlpha(coral, 0xC0) : colors::WithAlpha(coral, 0x33));
  put(L"ButtonBackgroundDisabled", filled ? colors::WithAlpha(coral, 0x66) : clear);
  put(L"ButtonForeground", ink);
  put(L"ButtonForegroundPointerOver", ink);
  put(L"ButtonForegroundPressed", ink);
  put(L"ButtonForegroundDisabled", colors::WithAlpha(ink, 0x99));
  put(L"ButtonBorderBrush", coral);
  put(L"ButtonBorderBrushPointerOver", coral);
  put(L"ButtonBorderBrushPressed", coral);
  put(L"ButtonBorderBrushDisabled", colors::WithAlpha(coral, 0x66));
  button.BorderThickness(Thickness{1, 1, 1, 1});
}

// The words of a step's button: a glyph before them when the device asks for
// Windows Hello first, a spinner after them while the flow is busy.
UIElement ButtonContent(hstring const& text, wchar_t const* glyph, ProgressRing* ring) {
  StackPanel content;
  content.Orientation(Orientation::Horizontal);
  content.Spacing(8);
  content.HorizontalAlignment(HorizontalAlignment::Center);
  if (glyph) content.Children().Append(Glyph(glyph, 14, nullptr));
  TextBlock words;
  words.Text(text);
  words.TextWrapping(TextWrapping::Wrap);
  words.TextAlignment(TextAlignment::Center);
  words.VerticalAlignment(VerticalAlignment::Center);
  content.Children().Append(words);
  if (ring) {
    ProgressRing spinner;
    spinner.Width(14);
    spinner.Height(14);
    spinner.IsActive(true);
    spinner.Visibility(Visibility::Collapsed);
    content.Children().Append(spinner);
    *ring = spinner;
  }
  return content;
}

// A field for one word of the recovery phrase that learns nothing and suggests
// nothing: no spell check (which carries autocorrect), no text prediction, and
// the Password input scope, which tells the touch keyboard and the input stack
// not to learn or suggest. The word itself stays visible as it is typed.
TextBox QuietWordField(hstring const& name) {
  TextBox box;
  box.IsSpellCheckEnabled(false);
  box.IsTextPredictionEnabled(false);
  winrt::Microsoft::UI::Xaml::Input::InputScope scope;
  winrt::Microsoft::UI::Xaml::Input::InputScopeName password;
  password.NameValue(winrt::Microsoft::UI::Xaml::Input::InputScopeNameValue::Password);
  scope.Names().Append(password);
  box.InputScope(scope);
  box.FontFamily(FontFamily(L"Consolas"));
  box.PlaceholderText(name);
  winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(box, name);
  return box;
}

}  // namespace

// ---- BittensorKeySheet --------------------------------------------------------------

BittensorKeySheet::BittensorKeySheet(winrt::Microsoft::UI::Dispatching::DispatcherQueue const& queue,
                                     HWND window, std::shared_ptr<urnet::BittensorLocalKeyFlow> flow)
    : queue_(queue), window_(window), flow_(std::move(flow)) {}

std::shared_ptr<BittensorKeySheet> BittensorKeySheet::Create(
    XamlRoot const& root, winrt::Microsoft::UI::Dispatching::DispatcherQueue const& queue, HWND window,
    std::shared_ptr<urnet::BittensorLocalKeyFlow> flow) {
  auto sheet = std::shared_ptr<BittensorKeySheet>(new BittensorKeySheet(queue, window, std::move(flow)));
  sheet->Build(root);
  return sheet;
}

void BittensorKeySheet::Build(XamlRoot const& root) {
  dialog_ = ContentDialog();
  dialog_.XamlRoot(root);
  dialog_.Background(colors::SheetBrush());
  content_ = StackPanel();
  content_.Spacing(12);
  content_.MinWidth(sheetfit::Width(root, 480));
  content_.MaxWidth(sheetfit::Width(root, 560));
  ScrollViewer scroll;
  scroll.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
  scroll.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);
  scroll.MaxHeight(sheetfit::Height(root, 640));
  scroll.Content(content_);
  dialog_.Content(scroll);

  // The whole window stays out of screenshots, recordings and screen sharing
  // while the Key screen is up: the sheet is drawn inside it.
  capture_ = std::make_unique<btkeyplatform::CaptureExclusion>(window_);

  // Close, Cancel and Esc leave the screen from its main step; from what
  // deleting means and from the two words they go back to it. Nothing leaves
  // while the flow is busy signing, unless the window is hiding.
  dialog_.Closing([weak = weak_from_this()](ContentDialog const&, ContentDialogClosingEventArgs const& args) {
    auto self = weak.lock();
    if (!self || self->forceClose_) return;
    if (self->screen_.busy) {
      args.Cancel(true);
      return;
    }
    if (self->step_ == btkey::KeyStep::Confirm) {
      args.Cancel(true);
      self->flow_->cancelConfirmation();
      self->Refresh();
      return;
    }
    if (self->step_ == btkey::KeyStep::DeleteExplained) {
      args.Cancel(true);
      self->deleteExplained_ = false;
      self->Refresh();
    }
  });
  dialog_.Closed([weak = weak_from_this()](ContentDialog const&, ContentDialogClosedEventArgs const&) {
    auto self = weak.lock();
    if (!self) return;
    self->closed_ = true;
    if (self->countdownTimer_) self->countdownTimer_.Stop();
    if (self->copiedTimer_) self->copiedTimer_.Stop();
    self->capture_.reset();
  });

  countdownTimer_ = queue_.CreateTimer();
  countdownTimer_.Interval(std::chrono::seconds(1));
  countdownTimer_.IsRepeating(true);
  countdownTimer_.Tick([weak = weak_from_this()](auto const&, auto const&) {
    if (auto self = weak.lock()) self->UpdateCountdown();
  });
  copiedTimer_ = queue_.CreateTimer();
  copiedTimer_.Interval(std::chrono::seconds(2));
  copiedTimer_.IsRepeating(false);
  copiedTimer_.Tick([weak = weak_from_this()](auto const&, auto const&) {
    auto self = weak.lock();
    if (!self) return;
    self->copied_ = false;
    self->Refresh();
  });

  Refresh();
}

void BittensorKeySheet::Refresh() {
  if (closed_) return;
  std::optional<urnet::BittensorLocalKeyScreen> screen;
  try {
    screen = flow_->getScreen();
  } catch (std::exception const& e) {
    LogWarn("bittensor key: reading the screen failed: {}", e.what());
  }
  if (!screen) return;
  // the screen's work is done (linked, kept or deleted), or the flow closed it
  if (screen->done || !screen->open) {
    CloseNow();
    return;
  }
  screen_ = *screen;
  if (screen_.confirming != btkey::kConfirmNone) deleteExplained_ = false;
  const btkey::KeyStep step = btkey::KeyStepFor(screen_.confirming, deleteExplained_);
  const bool entering = !built_ || step != step_;
  step_ = step;
  built_ = true;
  switch (step) {
    case btkey::KeyStep::Main:
      BuildMain();
      break;
    case btkey::KeyStep::DeleteExplained:
      BuildDeleteExplained();
      break;
    case btkey::KeyStep::Confirm:
      // built once, so the two fields keep what the user typed
      if (entering) BuildConfirm();
      UpdateConfirm();
      break;
  }
  if (screen_.revealed && step == btkey::KeyStep::Main) {
    if (!countdownTimer_.IsRunning()) countdownTimer_.Start();
    UpdateCountdown();
  } else {
    countdownTimer_.Stop();
  }
}

void BittensorKeySheet::HideWords() {
  if (closed_ || !screen_.revealed) return;
  try {
    flow_->hide();
  } catch (std::exception const& e) {
    LogWarn("bittensor key: hiding the words failed: {}", e.what());
  }
  Refresh();
}

void BittensorKeySheet::CloseNow() {
  if (closed_) return;
  forceClose_ = true;
  countdownTimer_.Stop();
  dialog_.Hide();
}

// The screen itself (1d, 1f, 1g, 2b, 2c): the banner, what the key is, the
// words, the warnings, Keep and Delete, and the disclaimer.
void BittensorKeySheet::BuildMain() {
  dialog_.Title(winrt::box_value(Loc("bittensor_key_screen_title")));
  dialog_.CloseButtonText(Loc(screen_.save_now ? "cancel" : "close"));
  auto children = content_.Children();
  children.Clear();
  countdown_ = nullptr;

  const hstring failure = btkeyviews::FailureText(screen_.failure_code, screen_.failure_message);
  switch (btkey::KeyBannerFor(screen_.busy, screen_.offers_retry, screen_.save_now)) {
    case btkey::KeyBanner::Signing:
      children.Append(Notice(colors::kUrLightBlue, nullptr, /*spinner=*/true, Line(Loc("bittensor_key_signing"))));
      break;
    case btkey::KeyBanner::Retry: {
      StackPanel retry;
      retry.Spacing(8);
      if (!failure.empty()) retry.Children().Append(Line(failure));
      auto button = SheetButton(Loc("retry"), /*accent=*/true);
      button.Click([weak = weak_from_this()](auto const&, auto const&) {
        if (auto self = weak.lock()) self->flow_->retrySaveNow();
      });
      retry.Children().Append(button);
      retry.Children().Append(Muted(Loc("bittensor_key_save_now_retry")));
      children.Append(Notice(colors::kDanger, kWarningGlyph, false, retry));
      break;
    }
    case btkey::KeyBanner::SaveNow:
      children.Append(Notice(colors::kUrLightBlue, kInfoGlyph, false, Line(Loc("bittensor_key_save_now_banner"))));
      if (!failure.empty()) children.Append(Danger(failure));
      break;
    case btkey::KeyBanner::None:
      if (!failure.empty()) children.Append(Danger(failure));
      break;
  }

  children.Append(Line(Loc("bittensor_key_wording_not_wallet")));

  StackPanel words;
  words.Spacing(8);
  if (!screen_.address.empty()) {
    words.Children().Append(
        MakeText(hstring{urnw::Format("bittensor_key_words_label", ShortAddress(screen_.address))}, 12,
                 colors::MutedBrush()));
  }
  words.Children().Append(BuildWords());
  children.Append(words);

  if (screen_.hidden_by_capture) {
    children.Append(MakeText(Loc("bittensor_key_capture_hidden"), 13, colors::MakeBrush(colors::kUrAmber)));
  }

  children.Append(Notice(colors::kUrAmber, kWarningGlyph, false, Line(Loc("bittensor_key_wording_control"))));
  children.Append(Muted(Loc("bittensor_key_not_seed_2")));

  if (!screen_.offers_retry) {
    StackPanel saved;
    saved.Spacing(8);
    saved.Children().Append(MakeText(Loc("bittensor_key_saved_question"), 12, colors::MutedBrush()));
    if (screen_.offers_keep) {
      auto keep = SheetButton(Loc("bittensor_key_keep_action"), /*accent=*/false);
      keep.IsEnabled(!screen_.busy);
      keep.Click([weak = weak_from_this()](auto const&, auto const&) {
        auto self = weak.lock();
        if (!self) return;
        // the sdk hides the words before it asks for two of them
        self->flow_->confirmKeep();
        self->Refresh();
      });
      saved.Children().Append(keep);
    }
    Button remove;
    remove.HorizontalAlignment(HorizontalAlignment::Stretch);
    remove.MinHeight(40);
    remove.Content(ButtonContent(Loc("bittensor_key_delete_action"), nullptr, nullptr));
    winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(remove, Loc("bittensor_key_delete_action"));
    MakeCoral(remove, /*filled=*/false);
    remove.IsEnabled(!screen_.busy);
    remove.Click([weak = weak_from_this()](auto const&, auto const&) {
      if (auto self = weak.lock()) self->ExplainDelete();
    });
    saved.Children().Append(remove);
    children.Append(saved);
  }

  if (auto disclaimer = btkeyviews::MakeFundsDisclaimer(screen_.funds_disclaimer, /*boxed=*/true)) {
    children.Append(disclaimer);
  }
}

// The words while they show, in plain text nothing can select, with the
// countdown, Hide, Copy and its warning; the cover with Tap to reveal while
// they don't. Hidden, nothing of them is in the visual tree.
UIElement BittensorKeySheet::BuildWords() {
  if (!screen_.revealed) {
    Button cover;
    cover.HorizontalAlignment(HorizontalAlignment::Stretch);
    cover.HorizontalContentAlignment(HorizontalAlignment::Center);
    cover.MinHeight(160);
    StackPanel label;
    label.Spacing(8);
    label.HorizontalAlignment(HorizontalAlignment::Center);
    label.VerticalAlignment(VerticalAlignment::Center);
    auto icon = Glyph(screen_.needs_authentication ? kHelloGlyph : kHiddenGlyph, 22, colors::MutedBrush());
    icon.HorizontalAlignment(HorizontalAlignment::Center);
    label.Children().Append(icon);
    auto words = MakeText(Loc("bittensor_key_reveal"), 14, colors::TextBrush(), true);
    words.HorizontalAlignment(HorizontalAlignment::Center);
    label.Children().Append(words);
    cover.Content(label);
    winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(cover, Loc("bittensor_key_reveal"));
    cover.IsEnabled(!screen_.hidden_by_capture && !screen_.busy);
    cover.Click([weak = weak_from_this()](auto const&, auto const&) {
      if (auto self = weak.lock()) self->TapToReveal();
    });
    return cover;
  }

  StackPanel shown;
  shown.Spacing(8);
  std::vector<std::string> words;
  try {
    if (auto revealed = flow_->revealedWords()) words = *revealed;
  } catch (std::exception const& e) {
    LogWarn("bittensor key: reading the words failed: {}", e.what());
  }
  Grid grid;
  grid.ColumnSpacing(8);
  grid.RowSpacing(8);
  constexpr int kColumns = 3;
  for (int column = 0; column < kColumns; ++column) grid.ColumnDefinitions().Append(ColumnDefinition());
  const int rows = static_cast<int>((words.size() + kColumns - 1) / kColumns);
  for (int row = 0; row < rows; ++row) {
    RowDefinition definition;
    definition.Height(GridLengthHelper::Auto());
    grid.RowDefinitions().Append(definition);
  }
  for (size_t index = 0; index < words.size(); ++index) {
    Border cell;
    cell.Background(colors::CardBrush());
    cell.CornerRadius(CornerRadiusHelper::FromUniformRadius(6));
    cell.Padding(Thickness{8, 6, 8, 6});
    StackPanel pair;
    pair.Orientation(Orientation::Horizontal);
    pair.Spacing(6);
    TextBlock ordinal;
    ordinal.Text(hstring{std::to_wstring(index + 1) + L"."});
    ordinal.FontFamily(FontFamily(L"Consolas"));
    ordinal.FontSize(12);
    ordinal.Foreground(colors::MutedBrush());
    ordinal.VerticalAlignment(VerticalAlignment::Center);
    pair.Children().Append(ordinal);
    // plain text: IsTextSelectionEnabled stays false, so nothing selects or copies it
    TextBlock word;
    word.Text(winrt::to_hstring(words[index]));
    word.FontFamily(FontFamily(L"Consolas"));
    word.FontSize(14);
    word.Foreground(colors::TextBrush());
    pair.Children().Append(word);
    cell.Child(pair);
    Grid::SetRow(cell, static_cast<int32_t>(index / kColumns));
    Grid::SetColumn(cell, static_cast<int32_t>(index % kColumns));
    grid.Children().Append(cell);
  }
  for (auto& word : words) ::SecureZeroMemory(word.data(), word.size());
  shown.Children().Append(grid);

  countdown_ = MakeText(hstring{}, 12, colors::MutedBrush());
  shown.Children().Append(countdown_);

  Grid buttons;
  buttons.ColumnSpacing(8);
  buttons.ColumnDefinitions().Append(ColumnDefinition());
  buttons.ColumnDefinitions().Append(ColumnDefinition());
  auto hide = SheetButton(Loc("bittensor_key_hide"), /*accent=*/false);
  hide.Content(ButtonContent(Loc("bittensor_key_hide"), kHiddenGlyph, nullptr));
  hide.Click([weak = weak_from_this()](auto const&, auto const&) {
    if (auto self = weak.lock()) self->HideWords();
  });
  Grid::SetColumn(hide, 0);
  buttons.Children().Append(hide);
  auto copy = SheetButton(Loc(copied_ ? "copied" : "copy"), /*accent=*/false);
  copy.Content(ButtonContent(Loc(copied_ ? "copied" : "copy"), copied_ ? kCheckGlyph : kCopyGlyph, nullptr));
  copy.Click([weak = weak_from_this()](auto const&, auto const&) {
    if (auto self = weak.lock()) self->Copy();
  });
  Grid::SetColumn(copy, 1);
  buttons.Children().Append(copy);
  shown.Children().Append(buttons);
  shown.Children().Append(Muted(Loc("bittensor_key_copy_warning_2")));
  return shown;
}

// Tap to reveal: Windows Hello first for a stored key, then the sdk's Reveal,
// which shows nothing to an answer that isn't authenticated.
winrt::fire_and_forget BittensorKeySheet::TapToReveal() {
  auto self = shared_from_this();
  if (authenticating_ || closed_) co_return;
  bool authenticated = false;
  if (screen_.needs_authentication) {
    authenticating_ = true;
    authenticated = co_await btkeyplatform::Authenticate(window_, Loc("bittensor_key_auth_reason"));
    authenticating_ = false;
  }
  if (closed_) co_return;
  try {
    flow_->reveal(authenticated);
  } catch (std::exception const& e) {
    LogWarn("bittensor key: reveal failed: {}", e.what());
  }
  Refresh();
}

// The explicit Copy. The sdk answers only while the words show, so there is no
// Copy while they are hidden.
void BittensorKeySheet::Copy() {
  std::string phrase;
  try {
    phrase = flow_->phraseToCopy();
  } catch (std::exception const& e) {
    LogWarn("bittensor key: reading the phrase to copy failed: {}", e.what());
  }
  if (phrase.empty()) return;
  const bool copied = btkeyplatform::CopyPhrase(phrase, queue_);
  ::SecureZeroMemory(phrase.data(), phrase.size());
  if (!copied) return;
  copied_ = true;
  copiedTimer_.Stop();
  copiedTimer_.Start();
  Refresh();
}

// I have saved the key, it's OK to delete it now: the words hide first, then
// what deleting means shows in place of the screen, so none can be read
// behind it and the disclaimer still shows once.
void BittensorKeySheet::ExplainDelete() {
  try {
    flow_->hide();
  } catch (std::exception const& e) {
    LogWarn("bittensor key: hiding the words failed: {}", e.what());
  }
  deleteExplained_ = true;
  Refresh();
}

// What deleting means (1j, 2d): no way back, where earnings keep going, what
// only the phrase can do, convert ending, the Top 200 claim while it applies,
// then Continue to the two words.
void BittensorKeySheet::BuildDeleteExplained() {
  const btkey::DeleteText text = btkey::DeleteTextFor(screen_.save_now);
  dialog_.Title(winrt::box_value(Loc(text.titleKey)));
  dialog_.CloseButtonText(Loc("cancel"));
  auto children = content_.Children();
  children.Clear();
  countdown_ = nullptr;

  StackPanel bullets;
  bullets.Spacing(6);
  for (auto const& bullet : text.bullets) {
    Grid row;
    row.ColumnSpacing(8);
    ColumnDefinition lead;
    lead.Width(GridLengthHelper::Auto());
    row.ColumnDefinitions().Append(lead);
    row.ColumnDefinitions().Append(ColumnDefinition());
    auto dot = Line(hstring{L"\u2022"});
    Grid::SetColumn(dot, 0);
    row.Children().Append(dot);
    auto words = Line(bullet.namesAddress
                          ? hstring{urnw::Format(bullet.key, ShortAddress(screen_.address))}
                          : Loc(bullet.key));
    Grid::SetColumn(words, 1);
    row.Children().Append(words);
    bullets.Children().Append(row);
  }
  children.Append(bullets);

  if (screen_.warns_top200) {
    children.Append(Notice(colors::kReferralGold, kWarningGlyph, false, Line(Loc("bittensor_key_confirm_top200"))));
  }
  children.Append(Line(Loc("bittensor_key_wording_not_wallet")));
  children.Append(Line(Loc("bittensor_key_wording_control")));
  if (auto disclaimer = btkeyviews::MakeFundsDisclaimer(screen_.funds_disclaimer, /*boxed=*/true)) {
    children.Append(disclaimer);
  }

  Button proceed;
  proceed.HorizontalAlignment(HorizontalAlignment::Stretch);
  proceed.MinHeight(40);
  proceed.Content(ButtonContent(Loc("continue_txt"), nullptr, nullptr));
  winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(proceed, Loc("continue_txt"));
  MakeCoral(proceed, /*filled=*/true);
  proceed.Click([weak = weak_from_this()](auto const&, auto const&) {
    auto self = weak.lock();
    if (!self) return;
    self->deleteExplained_ = false;
    self->flow_->confirmDelete();
    self->Refresh();
  });
  children.Append(proceed);
}

// The two words (1h, 1k, 2e, 2i): Keep's or Delete's, with each field saying
// as it is typed that its word is wrong, and the button only for right ones.
void BittensorKeySheet::BuildConfirm() {
  const btkey::ConfirmText text = btkey::ConfirmTextFor(screen_.confirming, screen_.save_now);
  dialog_.Title(winrt::box_value(Loc(text.titleKey)));
  dialog_.CloseButtonText(Loc("cancel"));
  auto children = content_.Children();
  children.Clear();
  countdown_ = nullptr;
  wordsReady_ = false;

  if (!text.bodyKey.empty()) children.Append(Line(Loc(text.bodyKey)));
  children.Append(Line(Loc(text.promptKey)));

  auto field = [&](int64_t position, TextBox& box, TextBlock& wrong) {
    StackPanel group;
    group.Spacing(4);
    const hstring name{urnw::Format("bittensor_key_word_number", position)};
    group.Children().Append(MakeText(name, 12, colors::MutedBrush()));
    box = QuietWordField(name);
    box.TextChanged([weak = weak_from_this()](auto const&, auto const&) {
      if (auto self = weak.lock()) self->OnWordsTyped();
    });
    group.Children().Append(box);
    wrong = MakeText(hstring{urnw::Format("bittensor_key_word_wrong", position)}, 12, colors::DangerBrush());
    wrong.Visibility(Visibility::Collapsed);
    group.Children().Append(wrong);
    children.Append(group);
  };
  field(screen_.first_word_position, firstBox_, firstWrong_);
  field(screen_.second_word_position, secondBox_, secondWrong_);

  rejected_ = Danger(Loc("bittensor_key_words_rejected"));
  rejected_.Visibility(Visibility::Collapsed);
  children.Append(rejected_);
  confirmFailure_ = Danger(hstring{});
  confirmFailure_.Visibility(Visibility::Collapsed);
  children.Append(confirmFailure_);

  if (auto disclaimer = btkeyviews::MakeFundsDisclaimer(screen_.funds_disclaimer, /*boxed=*/true)) {
    children.Append(disclaimer);
  }

  confirmButton_ = Button();
  confirmButton_.HorizontalAlignment(HorizontalAlignment::Stretch);
  confirmButton_.MinHeight(40);
  confirmRing_ = nullptr;
  confirmButton_.Content(ButtonContent(Loc(text.buttonKey),
                                       screen_.confirm_needs_authentication ? kHelloGlyph : nullptr,
                                       &confirmRing_));
  winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(confirmButton_, Loc(text.buttonKey));
  if (text.destructive) {
    MakeCoral(confirmButton_, /*filled=*/true);
  } else if (auto style = StyleNamed(L"AccentButtonStyle")) {
    confirmButton_.Style(style);
  }
  confirmButton_.Click([weak = weak_from_this()](auto const&, auto const&) {
    if (auto self = weak.lock()) self->Submit();
  });
  children.Append(confirmButton_);
}

void BittensorKeySheet::OnWordsTyped() {
  if (!firstBox_ || !secondBox_) return;
  std::optional<urnet::BittensorLocalKeyWordCheck> check;
  try {
    // changes nothing and publishes nothing: the positions are fixed per key,
    // so this can tell nothing of the phrase but these two words
    check = flow_->checkWords(Narrow(std::wstring_view{firstBox_.Text()}),
                              Narrow(std::wstring_view{secondBox_.Text()}));
  } catch (std::exception const& e) {
    LogWarn("bittensor key: checking the words failed: {}", e.what());
  }
  const bool firstWrong = check && check->first == btkey::kWordWrong;
  const bool secondWrong = check && check->second == btkey::kWordWrong;
  firstWrong_.Visibility(firstWrong ? Visibility::Visible : Visibility::Collapsed);
  secondWrong_.Visibility(secondWrong ? Visibility::Visible : Visibility::Collapsed);
  wordsReady_ = check && check->ready;
  confirmButton_.IsEnabled(wordsReady_ && !screen_.busy && !authenticating_);
}

void BittensorKeySheet::UpdateConfirm() {
  rejected_.Visibility(screen_.words_rejected ? Visibility::Visible : Visibility::Collapsed);
  const hstring failure = btkeyviews::FailureText(screen_.failure_code, screen_.failure_message);
  confirmFailure_.Text(failure);
  confirmFailure_.Visibility(failure.empty() ? Visibility::Collapsed : Visibility::Visible);
  if (confirmRing_) confirmRing_.Visibility(screen_.busy ? Visibility::Visible : Visibility::Collapsed);
  OnWordsTyped();
}

// The button: deleting a stored key asks Windows Hello first, at the button,
// after the words, and stops on anything but a verified answer. Keep, and
// anything on Save now, asks nothing.
winrt::fire_and_forget BittensorKeySheet::Submit() {
  auto self = shared_from_this();
  if (authenticating_ || closed_ || !wordsReady_ || screen_.busy) co_return;
  const std::string first = Narrow(std::wstring_view{firstBox_.Text()});
  const std::string second = Narrow(std::wstring_view{secondBox_.Text()});
  bool authenticated = false;
  if (screen_.confirm_needs_authentication) {
    authenticating_ = true;
    confirmButton_.IsEnabled(false);
    authenticated = co_await btkeyplatform::Authenticate(window_, Loc("bittensor_key_auth_reason"));
    authenticating_ = false;
    if (closed_) co_return;
    if (!authenticated) {
      OnWordsTyped();
      co_return;
    }
  }
  try {
    flow_->submitWords(first, second, authenticated);
  } catch (std::exception const& e) {
    LogWarn("bittensor key: submitting the words failed: {}", e.what());
  }
}

void BittensorKeySheet::UpdateCountdown() {
  if (!countdown_ || !screen_.revealed || screen_.revealed_until_millis <= 0) return;
  countdown_.Text(hstring{urnw::Format(
      "bittensor_key_hides_in", Widen(btkey::Countdown(screen_.revealed_until_millis, NowMillis())))});
}

// ---- ClaimRelaySheet ----------------------------------------------------------------

ClaimRelaySheet::ClaimRelaySheet(std::optional<urnet::SnClaimStatusResult> status, std::string disclaimer,
                                 ClaimNow claimNow)
    : status_(std::move(status)), disclaimer_(std::move(disclaimer)), claimNow_(std::move(claimNow)) {}

std::shared_ptr<ClaimRelaySheet> ClaimRelaySheet::Create(XamlRoot const& root,
                                                         std::optional<urnet::SnClaimStatusResult> status,
                                                         std::string disclaimer, ClaimNow claimNow) {
  auto sheet = std::shared_ptr<ClaimRelaySheet>(
      new ClaimRelaySheet(std::move(status), std::move(disclaimer), std::move(claimNow)));
  sheet->Build(root);
  return sheet;
}

void ClaimRelaySheet::Build(XamlRoot const& root) {
  dialog_ = ContentDialog();
  dialog_.XamlRoot(root);
  dialog_.Title(winrt::box_value(Loc("claim_alpha_title")));
  dialog_.CloseButtonText(Loc("close"));
  dialog_.Background(colors::SheetBrush());

  StackPanel content;
  content.Spacing(12);
  content.MinWidth(sheetfit::Width(root, 460));
  content.Children().Append(MakeText(Loc("sn_ledger_title"), 12, colors::MutedBrush()));

  const bool available = status_ && !status_->error;
  if (!available) {
    // the balances could not be read: unavailable, never zero
    content.Children().Append(MakeText(Loc("sn_ledger_unavailable"), 22, colors::MutedBrush()));
  } else {
    StackPanel figure;
    figure.Spacing(2);
    figure.Children().Append(MakeText(Loc("sn_ledger_claimable"), 12, colors::MutedBrush()));
    figure.Children().Append(MakeText(hstring{FormatAlphaRao(status_->claimable_rao)}, 28, colors::TextBrush()));
    content.Children().Append(figure);
    const std::string coldkey = status_->coldkey_ss58.value_or(std::string());
    if (!coldkey.empty()) {
      content.Children().Append(Line(hstring{urnw::Format("sn_claim_relay_lands_on", ShortAddress(coldkey))}));
    }
    StackPanel section;
    section.Spacing(8);
    btkeyviews::DrawRelaySection(section, *status_, /*inSheet=*/true, nullptr, nullptr);
    content.Children().Append(section);
    actionHost_ = StackPanel();
    actionHost_.Spacing(8);
    content.Children().Append(actionHost_);
  }

  if (auto disclaimer = btkeyviews::MakeFundsDisclaimer(disclaimer_, /*boxed=*/true)) {
    content.Children().Append(disclaimer);
  }

  ScrollViewer scroll;
  scroll.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
  scroll.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);
  scroll.MaxHeight(sheetfit::Height(root, 560));
  scroll.Content(content);
  dialog_.Content(scroll);
  ApplyState();
}

// Claim now and what it answered, or why it can't act, and the device's own
// claim as the fallback.
void ClaimRelaySheet::ApplyState() {
  if (!actionHost_ || !status_) return;
  auto children = actionHost_.Children();
  children.Clear();
  const urnet::SnClaimStatusResult& status = *status_;
  switch (outcome_) {
    case Outcome::Requested: {
      children.Append(Line(Loc("sn_claim_relay_requested")));
      auto queued = SheetButton(Loc("sn_claim_relay_queued_button"), /*accent=*/true);
      queued.IsEnabled(false);
      children.Append(queued);
      break;
    }
    case Outcome::Refused: {
      const hstring note = btkeyviews::ClaimNowNoteText(
          btkey::ClaimNowNoteFor(false, 0, refusedReason_, status.transfer_floor_tao_rao, refusedNextAtMillis_,
                                 /*inSheet=*/true),
          0, status.transfer_floor_tao_rao, refusedNextAtMillis_);
      if (!note.empty()) children.Append(Muted(note));
      break;
    }
    case Outcome::Failed:
      children.Append(Danger(hstring{Widen(failure_)}));
      break;
    case Outcome::None:
      if (btkey::RelayClaims(status.reason)) {
        auto claim = SheetButton(Loc("sn_claim_relay_claim_now"), /*accent=*/true);
        if (claiming_) {
          ProgressRing ring{nullptr};
          claim.Content(ButtonContent(Loc("sn_claim_relay_claim_now"), nullptr, &ring));
          if (ring) ring.Visibility(Visibility::Visible);
        }
        claim.IsEnabled(status.claim_now_available && !claiming_);
        claim.Click([weak = weak_from_this()](auto const&, auto const&) {
          if (auto self = weak.lock()) self->StartClaimNow();
        });
        children.Append(claim);
        const hstring note = btkeyviews::ClaimNowNoteText(
            btkey::ClaimNowNoteFor(status.claim_now_available, status.claim_now_period_days,
                                   status.claim_now_reason.value_or(std::string()), status.transfer_floor_tao_rao,
                                   status.claim_now_next_at_millis.value_or(0), /*inSheet=*/true),
            status.claim_now_period_days, status.transfer_floor_tao_rao,
            status.claim_now_next_at_millis.value_or(0));
        if (!note.empty()) children.Append(Muted(note));
      } else {
        children.Append(Muted(Loc("sn_claim_relay_waits_for_relay")));
      }
      break;
  }
  if (status.self_claim_available) {
    HyperlinkButton self;
    self.Content(winrt::box_value(Loc("sn_claim_relay_self_claim")));
    self.Padding(Thickness{0, 0, 0, 0});
    self.HorizontalAlignment(HorizontalAlignment::Left);
    self.Click([weak = weak_from_this()](auto const&, auto const&) {
      auto sheet = weak.lock();
      if (!sheet) return;
      // the gas-key claim is a sheet of its own: this one closes first
      sheet->selfClaimChosen_ = true;
      sheet->dialog_.Hide();
    });
    children.Append(self);
  }
}

void ClaimRelaySheet::StartClaimNow() {
  if (claiming_ || !status_ || !status_->claim_now_available || !claimNow_) return;
  claiming_ = true;
  ApplyState();
  claimNow_(status_->coldkey_ss58.value_or(std::string()),
            [weak = weak_from_this()](std::optional<urnet::SnClaimNowResult> result, std::string error) {
              auto self = weak.lock();
              if (!self) return;
              self->claiming_ = false;
              self->answered_ = true;
              if (!error.empty() || !result) {
                self->outcome_ = Outcome::Failed;
                self->failure_ = error.empty() ? Narrow(std::wstring_view{Loc("something_went_wrong")}) : error;
              } else if (result->error) {
                self->outcome_ = Outcome::Failed;
                self->failure_ = result->error->message.empty()
                                     ? Narrow(std::wstring_view{Loc("something_went_wrong")})
                                     : result->error->message;
              } else if (result->requested) {
                self->outcome_ = Outcome::Requested;
              } else {
                self->outcome_ = Outcome::Refused;
                self->refusedReason_ = result->reason.value_or(std::string());
                self->refusedNextAtMillis_ = result->next_at_millis.value_or(0);
              }
              self->ApplyState();
            });
}

// ---- the dialogs ----------------------------------------------------------------------

namespace btkeysheets {

winrt::Windows::Foundation::IAsyncOperation<bool> ConfirmReplacePayoutWallet(
    winrt::com_ptr<winrt::URnetwork::implementation::MainWindow> window, std::string oldAddress) {
  if (!window || window->sheetOpen()) co_return false;  // one ContentDialog at a time
  ContentDialog dialog;
  dialog.XamlRoot(window->Content().XamlRoot());
  dialog.Background(colors::SheetBrush());
  dialog.Title(winrt::box_value(Loc("bittensor_key_replace_title")));
  TextBlock message;
  message.Text(hstring{urnw::Format("bittensor_key_replace_message_2", ShortAddress(oldAddress))});
  message.TextWrapping(TextWrapping::Wrap);
  message.MinWidth(320);
  dialog.Content(message);
  dialog.PrimaryButtonText(Loc("bittensor_key_replace_confirm"));
  dialog.CloseButtonText(Loc("cancel"));
  // Enter must not replace everyone's payout wallet
  dialog.DefaultButton(ContentDialogButton::Close);
  bool confirmed = false;
  window->SetSheetOpen(true);
  try {
    confirmed = co_await dialog.ShowAsync() == ContentDialogResult::Primary;
  } catch (...) {
    LogError("bittensor key: the replace warning failed to open");
  }
  window->SetSheetOpen(false);
  co_return confirmed;
}

winrt::fire_and_forget RequestSignOut(winrt::com_ptr<winrt::URnetwork::implementation::MainWindow> window,
                                      std::function<void()> signOut) {
  std::string networkId;
  if (auto jwt = Sdk().ParsedJwt(); jwt && jwt->NetworkId) networkId = *jwt->NetworkId;
  // a sign-out with no unsaved key asks nothing, as it always did
  if (!BittensorKeyNeedsBackup(networkId)) {
    if (signOut) signOut();
    co_return;
  }
  if (!window || window->sheetOpen()) co_return;  // one ContentDialog at a time
  ContentDialog dialog;
  dialog.XamlRoot(window->Content().XamlRoot());
  dialog.Background(colors::SheetBrush());
  dialog.Title(winrt::box_value(Loc("bittensor_key_sign_out_title_2")));
  TextBlock message;
  message.Text(Loc("bittensor_key_sign_out_message_2"));
  message.TextWrapping(TextWrapping::Wrap);
  message.MinWidth(320);
  dialog.Content(message);
  dialog.PrimaryButtonText(Loc("bittensor_key_back_up_first"));
  dialog.SecondaryButtonText(Loc("sign_out"));
  dialog.CloseButtonText(Loc("cancel"));
  // Enter must not sign out past the warning
  dialog.DefaultButton(ContentDialogButton::Close);
  ContentDialogResult result = ContentDialogResult::None;
  window->SetSheetOpen(true);
  try {
    result = co_await dialog.ShowAsync();
  } catch (...) {
    LogError("bittensor key: the sign-out warning failed to open");
  }
  window->SetSheetOpen(false);
  if (result == ContentDialogResult::Primary) {
    // Back up first: Earnings, with the Key screen
    window->OpenEarningsKeyScreen();
  } else if (result == ContentDialogResult::Secondary) {
    // the key stays on this device, and comes back when this network signs in
    if (signOut) signOut();
  }
}

}  // namespace btkeysheets
}  // namespace urnw
