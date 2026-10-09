// SPDX-License-Identifier: MPL-2.0
#include "pch.h"

#include "SettingsSheets.h"

#include <algorithm>
#include <cctype>

#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Automation.Peers.h>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include <winrt/Windows.Storage.Streams.h>

#include "AddSignIn.h"
#include "BittensorWalletFlow.h"
#include "DeleteAccountOutcome.h"
#include "Localization.h"
#include "Log.h"
#include "PageContext.h"
#include "SheetFit.h"  // sheetfit: sheets clamp to the window at open time
#include "SubscriptionBalance.h"
#include "Strings.h"
#include "UrColors.h"
#include "UrComponents.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Windows::Foundation;

// NOTE on captures: control event handlers capture the owning sheet weakly, and
// SDK callbacks capture the dialog's DispatcherQueue plus a weak_from_this and
// marshal before touching anything (StatsSheets.cpp / AuthSheets.cpp set the
// pattern). The window holds the sheet's shared_ptr while the dialog is
// showing, so lock() succeeds throughout an interaction and returns null
// afterwards instead of running into a freed sheet.

namespace urnw {
namespace {

hstring H(std::string const& s) { return winrt::to_hstring(s); }
hstring Loc(std::string_view key) { return hstring{Localized(key)}; }

std::string Trim(std::string const& s) {
  const auto b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return {};
  return s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1);
}

std::string LowerAscii(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// The two error channels every Api call has: the transport `err` and the
// server's own `result->error->message`. Collapsing them here keeps each call
// site from forgetting the second one, which is the channel that carries every
// interesting refusal (the auth-code limit, an invalid referral code, a name
// already taken).
// Error structs are not uniform: most carry `std::string message`, but
// AuthCodeCreateError carries `std::optional<std::string> message`. Two
// overloads rather than a constexpr branch, so a third shape fails to compile
// here instead of silently returning nothing.
inline std::string AsMessage(std::string const& message) { return message; }
inline std::string AsMessage(std::optional<std::string> const& message) {
  return message ? *message : std::string();
}

template <typename Result>
std::string ServerError(std::optional<Result> const& result,
                        std::optional<std::string> const& err) {
  if (result && result->error) return AsMessage(result->error->message);
  if (err) return *err;
  if (!result) return Narrow(Localized("something_went_wrong"));
  return {};
}

}  // namespace

// ---- the row kit ----------------------------------------------------------

namespace rows {

// ---- pane mode (R4) --------------------------------------------------------
// See the note in SettingsSheets.h. UI thread only, set around a section build.
bool g_paneMode = false;

void SetPaneMode(bool on) { g_paneMode = on; }
bool PaneMode() { return g_paneMode; }

Style Lookup(std::wstring_view key) {
  auto app = Application::Current();
  if (!app) return nullptr;
  auto res = app.Resources();
  auto boxed = winrt::box_value(hstring{key});
  if (!res.HasKey(boxed)) return nullptr;
  return res.Lookup(boxed).try_as<Style>();
}

StackPanel Card(Panel const& host, double spacing) {
  if (g_paneMode) {
    // A pane has no islands in it: the group's rows go straight into the pane's
    // column, separated by their own hairlines. Spacing 0 for the same reason -
    // a gap between two rows is the card model's separator.
    StackPanel plain;
    host.Children().Append(plain);
    return plain;
  }
  Border card;
  card.Style(Lookup(L"UrCardStyle"));
  StackPanel inner;
  inner.Spacing(spacing);
  card.Child(inner);
  host.Children().Append(card);
  return inner;
}

void Heading(Panel const& host, hstring const& text, hstring const& glyph) {
  if (g_paneMode) {
    // The 28px group strip. The glyph is dropped: a pane group header is chrome
    // naming a column, and the R3 vocabulary deliberately does not put marks in
    // it (Home's twelve group headers carry none).
    host.Children().Append(kit::MakePaneGroupHeader(text).root);
    return;
  }
  TextBlock block;
  block.Text(text);
  block.FontSize(18);
  block.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
  block.VerticalAlignment(VerticalAlignment::Center);
  auto app = Application::Current();
  if (app) {
    auto boxed = winrt::box_value(hstring{L"UrHeadingFontFamily"});
    if (app.Resources().HasKey(boxed)) {
      if (auto family = app.Resources().Lookup(boxed).try_as<Media::FontFamily>()) {
        block.FontFamily(family);
      }
    }
  }
  if (glyph.empty()) {
    host.Children().Append(block);
    return;
  }
  StackPanel row;
  row.Orientation(Orientation::Horizontal);
  row.Spacing(8);
  FontIcon icon;
  // Segoe Fluent Icons by name: FontIcon otherwise defaults to the older Segoe
  // MDL2 Assets and the screen ends up with two icon weights on it.
  icon.FontFamily(Media::FontFamily(L"Segoe Fluent Icons"));
  icon.Glyph(glyph);
  icon.FontSize(16);
  icon.Foreground(urnw::colors::MutedBrush());
  icon.VerticalAlignment(VerticalAlignment::Center);
  Automation::AutomationProperties::SetAccessibilityView(
      icon, Automation::Peers::AccessibilityView::Raw);
  row.Children().Append(icon);
  row.Children().Append(block);
  host.Children().Append(row);
}

TextBlock Supporting(Panel const& host, hstring const& text) {
  TextBlock block;
  block.Text(text);
  block.FontSize(12);
  block.TextWrapping(TextWrapping::Wrap);
  block.Foreground(colors::MutedBrush());
  if (g_paneMode) {
    // Prose, not a list row: it is allowed to wrap and therefore to be taller
    // than 44, but it still carries the row hairline and the pane's 12px inset
    // so it does not read as text floating between two rows.
    Border box;
    box.Padding(ThicknessHelper::FromLengths(12, 10, 12, 10));
    box.BorderBrush(colors::BorderBrush());
    box.BorderThickness(ThicknessHelper::FromLengths(0, 0, 0, 1));
    box.Child(block);
    host.Children().Append(box);
    return block;
  }
  host.Children().Append(block);
  return block;
}

// Accessibility for a row's trailing control. Its own content is a VERB
// ("Copy", "Save"), which does not say what it acts on - the UIA tree showed two
// bare "Copy" buttons with nothing to tell them apart.
//
// FullDescription, NOT LabeledBy. LabeledBy REPLACES the name, which cost more
// than it bought: on a ValueRow the trailing element IS the value TextBlock, so
// its name became the row label and the device spec, the version and every
// field's state became unreadable to a screen reader. FullDescription appends
// instead, so the button keeps "Copy" and gains "Client ID", and a value
// TextBlock is never touched at all.
//
// Shared by the card row and the pane row (R4) so the two cannot drift.
void DescribeTrailing(FrameworkElement const& trailing, hstring const& label) {
  auto describe = [&label](FrameworkElement const& element) {
    // A TextBlock's name is its text, which is exactly what a screen-reader
    // user needs to hear - never touch it. That was the ValueRow regression.
    if (!element.try_as<Control>()) return;
    if (element.try_as<ToggleSwitch>()) {
      // A UrSwitchToggleStyle switch has empty On/Off content, so it has NO
      // name of its own - FullDescription supplements a name that is not there,
      // and the toggle vanished from the tree as an unnamed control. Verified
      // live: "Send me product updates" had no TogglePattern target to drive.
      // It needs the label AS its name.
      Automation::AutomationProperties::SetName(element, label);
      return;
    }
    // Buttons carry a VERB. FullDescription appends the object without
    // destroying the verb, which is what LabeledBy did.
    Automation::AutomationProperties::SetFullDescription(element, label);
  };
  describe(trailing);
  if (auto panel = trailing.try_as<Panel>()) {
    // ValueActionRow's trailing is a value + button stack: describe the button,
    // leave the value readable.
    for (auto const& child : panel.Children()) {
      if (auto element = child.try_as<FrameworkElement>()) describe(element);
    }
  }
}

Grid Row(Panel const& host, hstring const& label, hstring const& note,
         FrameworkElement const& trailing) {
  if (g_paneMode) {
    auto pane = kit::MakePaneTwoLineRow(label, note);
    if (trailing) {
      trailing.VerticalAlignment(VerticalAlignment::Center);
      DescribeTrailing(trailing, label);
      pane.trailing.Children().Append(trailing);
    }
    host.Children().Append(pane.root);
    // The caller gets the row's inner Grid, as it always has. Nothing in the
    // app reads it, but returning the Border would silently change the type.
    return pane.trailing;
  }
  Grid row;
  ColumnDefinition left, right;
  left.Width(GridLength{1, GridUnitType::Star});
  right.Width(GridLength{0, GridUnitType::Auto});
  row.ColumnDefinitions().Append(left);
  row.ColumnDefinitions().Append(right);
  row.ColumnSpacing(12);

  StackPanel text;
  text.VerticalAlignment(VerticalAlignment::Center);
  text.Spacing(2);
  TextBlock labelBlock;
  labelBlock.Text(label);
  labelBlock.FontSize(14);
  labelBlock.TextWrapping(TextWrapping::Wrap);
  text.Children().Append(labelBlock);
  if (!note.empty()) {
    TextBlock noteBlock;
    noteBlock.Text(note);
    noteBlock.FontSize(12);
    noteBlock.TextWrapping(TextWrapping::Wrap);
    noteBlock.Foreground(colors::MutedBrush());
    text.Children().Append(noteBlock);
  }
  row.Children().Append(text);

  if (trailing) {
    Grid::SetColumn(trailing, 1);
    trailing.VerticalAlignment(VerticalAlignment::Center);
    DescribeTrailing(trailing, label);
    row.Children().Append(trailing);
  }
  host.Children().Append(row);
  return row;
}

ToggleSwitch ToggleRow(Panel const& host, hstring const& label, hstring const& note) {
  ToggleSwitch toggle;
  toggle.Style(Lookup(L"UrSwitchToggleStyle"));
  Row(host, label, note, toggle);
  return toggle;
}

Button ButtonRow(Panel const& host, hstring const& label, hstring const& note,
                 hstring const& action, bool danger) {
  Button button;
  button.Content(winrt::box_value(action));
  if (danger) button.Foreground(colors::DangerBrush());
  Row(host, label, note, button);
  return button;
}

TextBlock ValueRow(Panel const& host, hstring const& label) {
  TextBlock value;
  value.FontSize(14);
  value.Foreground(colors::MutedBrush());
  value.TextTrimming(TextTrimming::CharacterEllipsis);
  // wide enough for the longest FieldState line, not just for a value
  value.MaxWidth(260);
  Row(host, label, hstring{}, value);
  return value;
}

TextBlock ValueActionRow(Panel const& host, hstring const& label, hstring const& action,
                         Button& outButton) {
  StackPanel trailing;
  trailing.Orientation(Orientation::Horizontal);
  trailing.Spacing(8);

  TextBlock value;
  value.FontSize(14);
  value.Foreground(colors::MutedBrush());
  value.TextTrimming(TextTrimming::CharacterEllipsis);
  value.MaxWidth(220);
  value.VerticalAlignment(VerticalAlignment::Center);
  trailing.Children().Append(value);

  outButton = Button();
  outButton.Content(winrt::box_value(action));
  trailing.Children().Append(outButton);

  Row(host, label, hstring{}, trailing);
  return value;
}

Button NavRow(Panel const& host, hstring const& label, TextBlock& outValue) {
  if (g_paneMode) {
    auto pane = kit::MakePaneTwoLineRowButton(label);
    outValue = pane.value;
    host.Children().Append(pane.root);
    return pane.root;
  }
  Button button;
  button.Style(Lookup(L"UrCardRowButtonStyle"));
  button.HorizontalAlignment(HorizontalAlignment::Stretch);
  button.HorizontalContentAlignment(HorizontalAlignment::Stretch);
  // N8: UrCardRowButtonStyle pads 12px, Row starts at 0, so a NavRow and a Row
  // in the SAME card were visibly out of line. Negative margin of the same 12
  // puts the content back on Row's left edge while leaving the hover/pressed
  // surface full-bleed, which is what a tappable row should look like.
  button.Margin(ThicknessHelper::FromLengths(-12, 0, -12, 0));

  Grid content;
  ColumnDefinition c0, c1, c2;
  c0.Width(GridLength{1, GridUnitType::Star});
  c1.Width(GridLength{0, GridUnitType::Auto});
  c2.Width(GridLength{0, GridUnitType::Auto});
  content.ColumnDefinitions().Append(c0);
  content.ColumnDefinitions().Append(c1);
  content.ColumnDefinitions().Append(c2);
  content.ColumnSpacing(8);

  TextBlock labelBlock;
  labelBlock.Text(label);
  labelBlock.FontSize(14);
  labelBlock.VerticalAlignment(VerticalAlignment::Center);
  content.Children().Append(labelBlock);

  outValue = TextBlock();
  outValue.FontSize(14);
  outValue.Foreground(colors::MutedBrush());
  outValue.VerticalAlignment(VerticalAlignment::Center);
  outValue.TextTrimming(TextTrimming::CharacterEllipsis);
  outValue.MaxWidth(200);
  Grid::SetColumn(outValue, 1);
  content.Children().Append(outValue);

  FontIcon chevron;
  chevron.Glyph(L"\uE76C");  // ChevronRight, as the markup rows use
  chevron.FontSize(12);
  chevron.VerticalAlignment(VerticalAlignment::Center);
  chevron.Foreground(colors::MutedBrush());
  Grid::SetColumn(chevron, 2);
  content.Children().Append(chevron);

  button.Content(content);
  // A Button whose Content is a Grid has NO accessible name - it was absent
  // from the UIA tree as a button entirely, so a screen-reader user could not
  // reach Referral network, Blocked locations, Provider Identities or Manage
  // Subscription at all. Name it with the label it already shows.
  Automation::AutomationProperties::SetName(button, label);
  // ...and take the label and the chevron OUT of the tree, or the button's name
  // is read and then its own child repeats it. Per-element, not a subtree
  // sweep: outValue must stay readable, it is the row's data.
  Automation::AutomationProperties::SetAccessibilityView(
      labelBlock, Automation::Peers::AccessibilityView::Raw);
  Automation::AutomationProperties::SetAccessibilityView(
      chevron, Automation::Peers::AccessibilityView::Raw);
  host.Children().Append(button);
  return button;
}

void ApplyFieldState(TextBlock const& value, FieldState state, hstring const& loadedText) {
  if (!value) return;
  switch (state) {
    case FieldState::Loaded:
      value.Text(loadedText);
      value.Foreground(colors::MutedBrush());
      return;
    case FieldState::Loading:
      value.Text(Loc("loading"));
      value.Foreground(colors::FaintBrush());
      return;
    case FieldState::Empty:
      value.Text(Loc("none"));
      value.Foreground(colors::FaintBrush());
      return;
    case FieldState::NoSession:
      value.Text(Loc("please_login_to_urnetwork"));
      value.Foreground(colors::FaintBrush());
      return;
    case FieldState::NoDevice:
      // signed in, but the device controls are not attached yet
      value.Text(Loc("site_app_device_attaching"));
      value.Foreground(colors::FaintBrush());
      return;
    case FieldState::Failed:
      value.Text(Loc("something_went_wrong"));
      value.Foreground(colors::DangerBrush());
      return;
  }
}

void Divider(Panel const& host) {
  // Every pane row already ends in a hairline; a second one here draws a 2px
  // rule under one row in a list of otherwise identical rows.
  if (g_paneMode) return;
  Border line;
  line.Height(1);
  line.Background(colors::BorderBrush());
  host.Children().Append(line);
}

void CopyToClipboard(std::string const& text) {
  winrt::Windows::ApplicationModel::DataTransfer::DataPackage package;
  package.SetText(H(text));
  winrt::Windows::ApplicationModel::DataTransfer::Clipboard::SetContent(package);
}

// The royal-welcome panel: the crowned frog in gold plus confirmation copy
// (the referral king-frog moment, matching the ur.io referral panel and the
// android/apple sheets).
StackPanel MakeRoyalWelcomePanel(double minWidth) {
  StackPanel panel;
  panel.Spacing(12);
  panel.MinWidth(minWidth);

  Image frog;
  winrt::Microsoft::UI::Xaml::Media::Imaging::BitmapImage bitmap{
      winrt::Windows::Foundation::Uri{L"ms-appx:///Assets/ReferralFrog.png"}};
  frog.Source(bitmap);
  frog.Width(108);
  frog.Height(108);
  frog.HorizontalAlignment(HorizontalAlignment::Center);
  panel.Children().Append(frog);

  TextBlock title;
  title.Text(Loc("referral_royal_welcome"));
  title.FontSize(24);
  title.FontWeight(winrt::Windows::UI::Text::FontWeights::Bold());
  title.TextWrapping(TextWrapping::Wrap);
  title.TextAlignment(TextAlignment::Center);
  title.Foreground(colors::ReferralGoldLightBrush());
  panel.Children().Append(title);

  TextBlock detail;
  detail.Text(hstring{urnw::Format("referral_royal_welcome_detail",
                                   urnw::pages::Balance().ReferralTerms().bonusGibPerDay)});
  detail.TextWrapping(TextWrapping::Wrap);
  detail.TextAlignment(TextAlignment::Center);
  panel.Children().Append(detail);

  return panel;
}

ContentDialog MakeSheet(XamlRoot const& root, hstring const& title) {
  ContentDialog dialog;
  dialog.XamlRoot(root);
  dialog.Title(winrt::box_value(title));
  dialog.CloseButtonText(Loc("close"));
  dialog.Background(colors::SheetBrush());
  return dialog;
}

}  // namespace rows

using namespace rows;

// ---- DeviceNameSheet -------------------------------------------------------

std::shared_ptr<DeviceNameSheet> DeviceNameSheet::Create(
    XamlRoot const& root, SdkHost& sdk, std::string const& current,
    std::function<void(std::string)> onSaved) {
  auto sheet = std::shared_ptr<DeviceNameSheet>(new DeviceNameSheet(sdk, std::move(onSaved)));
  sheet->Build(root, current);
  return sheet;
}

void DeviceNameSheet::Build(XamlRoot const& root, std::string const& current) {
  dialog_ = MakeSheet(root, Loc("edit_device_name"));
  dialog_.PrimaryButtonText(Loc("save"));
  dialog_.CloseButtonText(Loc("cancel"));
  dialog_.DefaultButton(ContentDialogButton::Primary);

  StackPanel content;
  content.MinWidth(360);
  content.Spacing(8);

  nameBox_ = TextBox();
  nameBox_.Style(Lookup(L"UrTextInputStyle"));
  nameBox_.Header(winrt::box_value(Loc("device_name")));
  nameBox_.Text(H(current));
  content.Children().Append(nameBox_);

  errorText_ = TextBlock();
  errorText_.FontSize(12);
  errorText_.TextWrapping(TextWrapping::Wrap);
  errorText_.Foreground(colors::DangerBrush());
  errorText_.Visibility(Visibility::Collapsed);
  content.Children().Append(errorText_);

  dialog_.Content(content);
  dialog_.PrimaryButtonClick(
      [weak = weak_from_this()](auto const&, ContentDialogButtonClickEventArgs const& args) {
        args.Cancel(true);  // Submit decides whether the sheet closes
        if (auto self = weak.lock()) self->Submit();
      });

  if (!sdk_.IsLoggedIn()) {
    nameBox_.IsEnabled(false);
    dialog_.IsPrimaryButtonEnabled(false);
    ApplyFieldState(errorText_, FieldState::NoSession);
    errorText_.Visibility(Visibility::Visible);
  }
}

void DeviceNameSheet::Submit() {
  if (saving_ || !sdk_.IsLoggedIn()) return;
  const std::string name = Trim(Narrow(nameBox_.Text().c_str()));
  if (name.empty()) return;  // the save button is the affordance; an empty name is a no-op

  saving_ = true;
  dialog_.IsPrimaryButtonEnabled(false);
  errorText_.Visibility(Visibility::Collapsed);

  urnet::DeviceSetNameArgs args;
  args.device_name = name;
  // device_id stays unset: the server names the client this JWT was issued to,
  // which is the row's meaning ("this device"). iOS passes the id it resolved
  // from getNetworkClients; we do the same when we could resolve one, and the
  // settings page passes it in through `current` only for display.
  auto queue = dialog_.DispatcherQueue();
  auto weak = weak_from_this();
  sdk_.api().deviceSetName(
      args, [queue, weak, name](std::optional<urnet::DeviceSetNameResult> result,
                                std::optional<std::string> err) {
        const std::string error = ServerError(result, err);
        queue.TryEnqueue([weak, error, name] {
          if (auto self = weak.lock()) self->ApplyResult(error.empty(), error, name);
        });
      });
}

void DeviceNameSheet::ApplyResult(bool ok, std::string const& error, std::string const& name) {
  saving_ = false;
  dialog_.IsPrimaryButtonEnabled(true);
  if (ok) {
    if (onSaved_) onSaved_(name);
    dialog_.Hide();
    return;
  }
  // A server message is not localizable; show it when there is one, and fall
  // back to the shipped "error updating the device name" line when there is not.
  errorText_.Text(error.empty() ? Loc("error_updating_device_name") : H(error));
  errorText_.Visibility(Visibility::Visible);
}

// ---- AuthCodeSheet ---------------------------------------------------------

std::shared_ptr<AuthCodeSheet> AuthCodeSheet::Create(XamlRoot const& root, SdkHost& sdk) {
  auto sheet = std::shared_ptr<AuthCodeSheet>(new AuthCodeSheet(sdk));
  sheet->Build(root);
  return sheet;
}

void AuthCodeSheet::Build(XamlRoot const& root) {
  dialog_ = MakeSheet(root, Loc("auth_code_create"));

  StackPanel content;
  content.MinWidth(380);
  content.Spacing(12);

  Supporting(content, Loc("created_auth_codes_expire_after_5_minutes"));

  StackPanel actionRow;
  actionRow.Orientation(Orientation::Horizontal);
  actionRow.Spacing(8);
  createButton_ = Button();
  createButton_.Content(winrt::box_value(Loc("site_app_create_auth_code")));
  createButton_.Style(Lookup(L"UrPrimaryButtonStyle"));
  createButton_.Click([weak = weak_from_this()](auto const&, auto const&) {
    if (auto self = weak.lock()) self->Create();
  });
  actionRow.Children().Append(createButton_);
  ring_ = ProgressRing();
  ring_.Width(16);
  ring_.Height(16);
  ring_.IsActive(false);
  ring_.Visibility(Visibility::Collapsed);
  ring_.VerticalAlignment(VerticalAlignment::Center);
  actionRow.Children().Append(ring_);
  content.Children().Append(actionRow);

  // The code itself, revealed only after a successful create.
  codePanel_ = StackPanel();
  codePanel_.Spacing(8);
  codePanel_.Visibility(Visibility::Collapsed);
  codeText_ = TextBlock();
  codeText_.FontSize(14);
  codeText_.FontFamily(Media::FontFamily(L"Consolas"));
  codeText_.TextWrapping(TextWrapping::Wrap);
  codeText_.IsTextSelectionEnabled(true);
  codePanel_.Children().Append(codeText_);
  Button copyButton;
  copyButton.Content(winrt::box_value(Loc("copy_auth_code")));
  copyButton.HorizontalAlignment(HorizontalAlignment::Left);
  copyButton.Click([weak = weak_from_this()](auto const&, auto const&) {
    auto self = weak.lock();
    if (!self || self->code_.empty()) return;
    // the WHOLE code, not the abbreviated form on screen (apple AuthCodeCreate)
    CopyToClipboard(self->code_);
    self->statusText_.Text(Loc("site_app_copied"));
    self->statusText_.Foreground(colors::MutedBrush());
    self->statusText_.Visibility(Visibility::Visible);
  });
  codePanel_.Children().Append(copyButton);
  content.Children().Append(codePanel_);

  // One line carrying "created" / "copied" / the failure. iOS has no failure
  // surface here at all  -  createAuthCode() only prints  -  so a rejected create
  // is invisible there. It is not here.
  statusText_ = TextBlock();
  statusText_.FontSize(12);
  statusText_.TextWrapping(TextWrapping::Wrap);
  statusText_.Visibility(Visibility::Collapsed);
  content.Children().Append(statusText_);

  dialog_.Content(content);

  // With no session the create would 401 and, before this, did so silently:
  // the button was live, the click produced nothing at all.
  if (!sdk_.IsLoggedIn()) {
    createButton_.IsEnabled(false);
    ApplyFieldState(statusText_, FieldState::NoSession);
    statusText_.Visibility(Visibility::Visible);
  }
}

void AuthCodeSheet::Create() {
  if (creating_ || !sdk_.IsLoggedIn()) return;
  creating_ = true;
  createButton_.IsEnabled(false);
  ring_.IsActive(true);
  ring_.Visibility(Visibility::Visible);
  statusText_.Visibility(Visibility::Collapsed);

  urnet::AuthCodeCreateArgs args;
  args.duration_minutes = 5;  // matches the "expire after 5 minutes" caption
  args.uses = 1;

  auto queue = dialog_.DispatcherQueue();
  auto weak = weak_from_this();
  sdk_.api().authCodeCreate(args, [queue, weak](std::optional<urnet::AuthCodeCreateResult> result,
                                                std::optional<std::string> err) {
    const std::string error = err ? *err : std::string();
    queue.TryEnqueue([weak, result, error] {
      if (auto self = weak.lock()) self->ApplyResult(result, error);
    });
  });
}

void AuthCodeSheet::ApplyResult(std::optional<urnet::AuthCodeCreateResult> result,
                                std::string const& err) {
  creating_ = false;
  createButton_.IsEnabled(true);
  ring_.IsActive(false);
  ring_.Visibility(Visibility::Collapsed);

  // The limit is its own field on the error, and its own shipped string: "you
  // asked too often" is a different situation from "the call failed".
  if (result && result->error && result->error->auth_code_limit_exceeded &&
      *result->error->auth_code_limit_exceeded) {
    statusText_.Text(Loc("site_app_auth_code_limit"));
    statusText_.Foreground(colors::DangerBrush());
    statusText_.Visibility(Visibility::Visible);
    return;
  }
  const std::string message = ServerError(result, err.empty() ? std::nullopt
                                                              : std::optional<std::string>{err});
  if (!message.empty() || !result || !result->auth_code || result->auth_code->empty()) {
    statusText_.Text(message.empty() ? Loc("auth_code_error") : H(message));
    statusText_.Foreground(colors::DangerBrush());
    statusText_.Visibility(Visibility::Visible);
    return;
  }

  code_ = *result->auth_code;
  // Abbreviated on screen (first 6 ... last 6), whole on copy  -  apple's
  // AuthCodeCreate confirmation dialog shows exactly this shape, so a shoulder
  // surfer near the screen does not get a usable credential.
  std::string shown = code_;
  if (code_.size() > 14) {
    shown = code_.substr(0, 6) + "..." + code_.substr(code_.size() - 6);
  }
  codeText_.Text(H(shown));
  codePanel_.Visibility(Visibility::Visible);
  statusText_.Text(Loc("auth_code_created"));
  statusText_.Foreground(colors::MutedBrush());
  statusText_.Visibility(Visibility::Visible);
}

namespace {

// The SDK side of the conversion. Every answer is marshalled onto the dialog's
// queue; one that arrives after the sheet is gone is dropped by `alive_`.
class SdkGuestConversionSession : public GuestConversionSession {
 public:
  // `balance` is null for Settings' AddAuthSheet: that network was never a
  // guest, so there is no guest state to lift after the add.
  SdkGuestConversionSession(SdkHost& sdk, SubscriptionBalanceStore* balance,
                            winrt::Microsoft::UI::Dispatching::DispatcherQueue queue)
      : sdk_(sdk), balance_(balance), queue_(std::move(queue)) {}
  ~SdkGuestConversionSession() override { *alive_ = false; }

  void AddSignIn(std::string const& userAuth, std::string const& password,
                 std::function<void(std::string error)> done) override {
    urnet::AddAuthArgs args;
    args.user_auth = userAuth;
    args.password = password;
    sdk_.api().addAuth(args, [queue = queue_, alive = alive_, done = std::move(done)](
                                 std::optional<urnet::AddAuthResult> result,
                                 std::optional<std::string> err) {
      std::string error = ServerError(result, err);
      queue.TryEnqueue([alive, done, error] {
        if (*alive) done(error);
      });
    });
  }

  void RefreshJwt() override {
    if (balance_) sdk_.RefreshJwt();
  }

  void RefreshBalance() override {
    if (balance_) balance_->Refresh();
  }

  void SendCode(std::string const& userAuth,
                std::function<void(VerifySendNotice notice)> done) override {
    sdk_.ResendVerifyCode(userAuth, [queue = queue_, alive = alive_,
                                     done = std::move(done)](VerifySendNotice notice) {
      queue.TryEnqueue([alive, done, notice] {
        if (*alive) done(notice);
      });
    });
  }

  // authVerify without SdkHost::VerifyCode's sign-in: the returned jwt is for
  // this same network, and the session already holds one.
  void VerifyCode(std::string const& userAuth, std::string const& code,
                  std::function<void(std::string error)> done) override {
    urnet::AuthVerifyArgs args;
    args.user_auth = userAuth;
    args.verify_code = code;
    sdk_.api().authVerify(args, [queue = queue_, alive = alive_, done = std::move(done)](
                                    std::optional<urnet::AuthVerifyResult> result,
                                    std::optional<std::string> err) {
      std::string error = ServerError(result, err);
      queue.TryEnqueue([alive, done, error] {
        if (*alive) done(error);
      });
    });
  }

 private:
  SdkHost& sdk_;
  SubscriptionBalanceStore* balance_;
  winrt::Microsoft::UI::Dispatching::DispatcherQueue queue_;
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

// The SDK side of adding Apple, Google or a wallet (AddSignIn.h). SdkHost
// produces the credential under an add-owned flow; this posts it to addAuth on
// the current network. Nothing here signs in, refreshes or replaces the jwt.
// Every answer is marshalled onto the dialog's queue; one that arrives after
// the sheet is gone is dropped by `alive_`.
class SdkAddSignInSession : public add_sign_in::AddSignInSession {
 public:
  SdkAddSignInSession(SdkHost& sdk, winrt::Microsoft::UI::Dispatching::DispatcherQueue queue,
                      std::function<void(SdkHost::BittensorManualRequest)> manualHandler)
      : sdk_(sdk), queue_(std::move(queue)), manualHandler_(std::move(manualHandler)) {}
  ~SdkAddSignInSession() override { *alive_ = false; }

  void ProviderToken(std::string_view provider,
                     std::function<void(std::string, std::string)> done) override {
    sdk_.SsoTokenForAdd(std::string(provider), [queue = queue_, alive = alive_, done = std::move(done)](
                                                   std::string idToken, std::string error) {
      queue.TryEnqueue([alive, done, idToken, error] {
        if (*alive) done(idToken, error);
      });
    });
  }

  void SignWallet(add_sign_in::WalletChain chain, std::string_view walletId,
                  std::function<void(add_sign_in::WalletSignature, std::string)> done) override {
    auto answer = [queue = queue_, alive = alive_, done = std::move(done)](
                      std::string address, std::string signature, std::string message,
                      std::string error) {
      queue.TryEnqueue([alive, done, address, signature, message, error] {
        if (*alive) done(add_sign_in::WalletSignature{address, signature, message}, error);
      });
    };
    if (chain == add_sign_in::WalletChain::Solana) {
      const auto provider = walletId == add_sign_in::kSolanaSolflare
                                ? WalletConnect::Provider::Solflare
                                : WalletConnect::Provider::Phantom;
      sdk_.SignSolanaForAdd(provider, std::move(answer));
      return;
    }
    sdk_.SignBittensorForAdd(std::string(walletId), manualHandler_, std::move(answer));
  }

  void AddAuth(add_sign_in::AddAuthBody const& body,
               std::function<void(std::string, std::string)> done) override {
    urnet::AddAuthArgs args;
    args.user_auth = body.user_auth;
    args.password = body.password;
    args.auth_jwt = body.auth_jwt;
    args.auth_jwt_type = body.auth_jwt_type;
    if (body.wallet_auth) {
      urnet::WalletAuthArgs wallet;
      wallet.blockchain = body.wallet_auth->blockchain;
      wallet.wallet_address = body.wallet_auth->address;
      wallet.wallet_signature = body.wallet_auth->signature;
      wallet.wallet_message = body.wallet_auth->message;
      args.wallet_auth = wallet;
    }
    // the identity token and the signature are credentials; nothing logs args
    sdk_.api().addAuth(args, [queue = queue_, alive = alive_, done = std::move(done)](
                                 std::optional<urnet::AddAuthResult> result,
                                 std::optional<std::string> err) {
      std::string error = ServerError(result, err);
      // signature_mismatch: a pasted signature from another account
      std::string code = result && result->error ? result->error->code.value_or(std::string())
                                                 : std::string();
      queue.TryEnqueue([alive, done, error, code] {
        if (*alive) done(error, code);
      });
    });
  }

  void Cancel() override { sdk_.CancelAddSignIn(); }

 private:
  SdkHost& sdk_;
  winrt::Microsoft::UI::Dispatching::DispatcherQueue queue_;
  std::function<void(SdkHost::BittensorManualRequest)> manualHandler_;
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};


// The last code send's outcome under the code field: a sent code muted, a
// failure or a counting-down rate limit as an error, nothing for none.
void ShowSendNotice(TextBlock const& text, std::optional<VerifySendNotice> const& notice) {
  if (!notice) {
    text.Text(hstring{});
    return;
  }
  hstring message;
  switch (notice->kind) {
    case VerifySendNoticeKind::Sent:
    case VerifySendNoticeKind::SendFailed:
      message = Loc(VerifySendNoticeKey(*notice));
      break;
    case VerifySendNoticeKind::RateLimited:
      message = hstring{Plural(VerifySendNoticeKey(*notice), notice->minutes)};
      break;
    case VerifySendNoticeKind::ServerMessage:
      message = H(notice->message);
      break;
  }
  text.Text(message);
  text.Foreground(notice->kind == VerifySendNoticeKind::Sent ? colors::MutedBrush()
                                                             : colors::DangerBrush());
}

// A one-second repeating timer that re-renders while a rate limit counts down
// (RunCooldownTimer starts and stops it).
winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer MakeCooldownTimer(
    winrt::Microsoft::UI::Dispatching::DispatcherQueue const& queue, std::function<void()> render) {
  auto timer = queue.CreateTimer();
  timer.Interval(std::chrono::seconds(1));
  timer.IsRepeating(true);
  timer.Tick([render = std::move(render)](auto const&, auto const&) { render(); });
  return timer;
}

void RunCooldownTimer(winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer const& timer,
                      GuestConversion const& conversion) {
  if (!timer) return;
  if (conversion.CoolingDown()) {
    if (!timer.IsRunning()) timer.Start();
  } else if (timer.IsRunning()) {
    timer.Stop();
  }
}

}  // namespace

// ---- AddAuthSheet ----------------------------------------------------------

std::shared_ptr<AddAuthSheet> AddAuthSheet::Create(XamlRoot const& root, SdkHost& sdk,
                                                   std::function<void()> onChanged) {
  auto sheet = std::shared_ptr<AddAuthSheet>(new AddAuthSheet(sdk, std::move(onChanged)));
  sheet->Build(root);
  return sheet;
}

AddAuthSheet::~AddAuthSheet() {
  if (cooldownTimer_) cooldownTimer_.Stop();
  // a browser or wallet step still open is answered and dropped
  if (add_ && add_->Busy()) add_->Cancel();
  // the flows go first: they drop their answers before the sessions go
  add_.reset();
  addSession_.reset();
  conversion_.reset();
  session_.reset();
}

void AddAuthSheet::Build(XamlRoot const& root) {
  dialog_ = MakeSheet(root, Loc("add_a_sign_in_method"));
  dialog_.CloseButtonText(Loc("cancel"));
  dialog_.DefaultButton(ContentDialogButton::Primary);
  session_ = std::make_unique<SdkGuestConversionSession>(sdk_, nullptr, dialog_.DispatcherQueue());
  conversion_ = std::make_unique<GuestConversion>(*session_);
  // TAO.com's manual form shows inside this sheet (a second ContentDialog
  // cannot open over it); the request comes in on an SDK thread
  auto manualHandler = [weak = weak_from_this(), queue = dialog_.DispatcherQueue()](
                           SdkHost::BittensorManualRequest request) {
    queue.TryEnqueue([weak, request] {
      if (auto self = weak.lock()) self->ShowManual(request);
    });
  };
  addSession_ = std::make_unique<SdkAddSignInSession>(sdk_, dialog_.DispatcherQueue(),
                                                      std::move(manualHandler));
  add_ = std::make_unique<add_sign_in::AddSignInFlow>(*addSession_);

  StackPanel content;
  content.MinWidth(380);
  content.Spacing(12);

  // ---- the method: the same options and order as every app and ur.io ----
  methodPicker_ = RadioButtons();
  methodPicker_.Header(winrt::box_value(Loc("method")));
  methodPicker_.MaxColumns(add_sign_in::kMethodCount);
  int selected = 0;
  for (int i = 0; i < add_sign_in::kMethodCount; ++i) {
    methodPicker_.Items().Append(
        winrt::box_value(Loc(std::string(add_sign_in::MethodLabelKey(add_sign_in::kMethods[i])))));
    if (add_sign_in::kMethods[i] == method_) selected = i;
  }
  methodPicker_.SelectedIndex(selected);
  methodPicker_.SelectionChanged([weak = weak_from_this()](IInspectable const&,
                                                           SelectionChangedEventArgs const&) {
    auto self = weak.lock();
    if (!self) return;
    const int index = self->methodPicker_.SelectedIndex();
    if (index < 0 || add_sign_in::kMethodCount <= index) return;
    self->SelectMethod(add_sign_in::kMethods[index]);
  });
  content.Children().Append(methodPicker_);

  // ---- Apple / Google: the provider's web flow in the browser ----
  providerPanel_ = StackPanel();
  providerPanel_.Spacing(12);
  providerHint_ = Supporting(providerPanel_, hstring{});
  providerButton_ = Button();
  providerButton_.Style(Lookup(L"UrPrimaryButtonStyle"));
  providerButton_.HorizontalAlignment(HorizontalAlignment::Stretch);
  providerButton_.Click([weak = weak_from_this()](auto const&, auto const&) {
    auto self = weak.lock();
    if (!self || !self->sdk_.IsLoggedIn()) return;
    self->browserHint_ = hstring{};
    self->add_->StartProvider(self->method_);
  });
  providerPanel_.Children().Append(providerButton_);
  content.Children().Append(providerPanel_);

  // ---- a wallet: Solana (Phantom / Solflare) or Bittensor (the chooser) ----
  walletPanel_ = StackPanel();
  walletPanel_.Spacing(8);
  for (int c = 0; c < add_sign_in::kWalletChainCount; ++c) {
    const add_sign_in::WalletChain chain = add_sign_in::kWalletChains[c];
    TextBlock heading;
    heading.Text(Loc(std::string(add_sign_in::WalletChainLabelKey(chain))));
    heading.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
    walletPanel_.Children().Append(heading);
    Supporting(walletPanel_, Loc(std::string(add_sign_in::WalletChainHintKey(chain))));
    // Solana: the login page's two wallets; Bittensor: the shared chooser's
    std::vector<std::pair<std::string, hstring>> wallets;
    if (chain == add_sign_in::WalletChain::Solana) {
      wallets.emplace_back(std::string(add_sign_in::kSolanaPhantom), Loc("phantom"));
      wallets.emplace_back(std::string(add_sign_in::kSolanaSolflare), Loc("solflare"));
    } else {
      for (int i = 0; i < bittensor::kChooserWalletCount; ++i) {
        const std::string walletId(bittensor::kChooserWallets[i]);
        wallets.emplace_back(walletId, H(urnet::bittensorWalletDisplayName(walletId)));
      }
    }
    for (auto const& [walletId, name] : wallets) {
      StackPanel label;
      TextBlock nameText;
      nameText.Text(name);
      label.Children().Append(nameText);
      const std::string hintKey =
          chain == add_sign_in::WalletChain::Bittensor ? bittensor::ChooserHintKey(walletId) : std::string();
      if (!hintKey.empty()) {
        TextBlock hint;
        hint.Text(Loc(hintKey));
        hint.Opacity(0.7);
        hint.FontSize(12);
        hint.TextWrapping(TextWrapping::Wrap);
        label.Children().Append(hint);
      }
      Button button;
      button.HorizontalAlignment(HorizontalAlignment::Stretch);
      button.HorizontalContentAlignment(HorizontalAlignment::Left);
      button.Content(label);
      button.Click([weak = weak_from_this(), chain, walletId = walletId](auto const&, auto const&) {
        if (auto self = weak.lock()) self->StartWallet(chain, walletId);
      });
      walletButtons_.push_back(button);
      walletPanel_.Children().Append(button);
    }
  }
  content.Children().Append(walletPanel_);

  // ---- TAO.com: sign the message in the wallet, paste the signature ----
  manualPanel_ = StackPanel();
  manualPanel_.Spacing(8);
  manualInstructions_ = TextBlock();
  manualInstructions_.TextWrapping(TextWrapping::Wrap);
  manualPanel_.Children().Append(manualInstructions_);
  manualMessageBox_ = TextBox();
  manualMessageBox_.Header(winrt::box_value(Loc("bittensor_message_to_sign")));
  manualMessageBox_.IsReadOnly(true);
  manualMessageBox_.AcceptsReturn(true);
  manualMessageBox_.TextWrapping(TextWrapping::Wrap);
  manualPanel_.Children().Append(manualMessageBox_);
  Button copyButton;
  copyButton.Content(winrt::box_value(Loc("copy")));
  copyButton.Click([weak = weak_from_this()](auto const&, auto const&) {
    auto self = weak.lock();
    if (!self) return;
    namespace dt = winrt::Windows::ApplicationModel::DataTransfer;
    try {
      dt::DataPackage package;
      package.SetText(self->manualMessageBox_.Text());
      dt::Clipboard::SetContent(package);
    } catch (...) {
      LogWarn("add sign-in: the clipboard refused the message");
    }
  });
  manualPanel_.Children().Append(copyButton);
  manualAddressBox_ = TextBox();
  manualAddressBox_.PlaceholderText(Loc("earnings_address_placeholder"));
  manualPanel_.Children().Append(manualAddressBox_);
  manualSignatureBox_ = TextBox();
  manualSignatureBox_.Header(winrt::box_value(Loc("bittensor_signature_label")));
  manualSignatureBox_.PlaceholderText(Loc("bittensor_signature_placeholder"));
  manualPanel_.Children().Append(manualSignatureBox_);
  manualErrorText_ = TextBlock();
  manualErrorText_.FontSize(12);
  manualErrorText_.TextWrapping(TextWrapping::Wrap);
  manualErrorText_.Foreground(colors::DangerBrush());
  manualErrorText_.Visibility(Visibility::Collapsed);
  manualPanel_.Children().Append(manualErrorText_);
  StackPanel manualButtons;
  manualButtons.Orientation(Orientation::Horizontal);
  manualButtons.Spacing(8);
  Button manualContinue;
  manualContinue.Style(Lookup(L"UrPrimaryButtonStyle"));
  manualContinue.Content(winrt::box_value(Loc("continue_txt")));
  manualContinue.Click([weak = weak_from_this()](auto const&, auto const&) {
    auto self = weak.lock();
    if (!self || !self->manualRequest_) return;
    const auto answer = self->sdk_.SubmitBittensorManual(
        Narrow(self->manualAddressBox_.Text().c_str()), Narrow(self->manualSignatureBox_.Text().c_str()));
    if (answer.closed) {
      // the proof has its answer; the flow continues to addAuth
      self->manualRequest_.reset();
    } else {
      self->manualErrorText_.Text(H(answer.error));
      self->manualErrorText_.Visibility(Visibility::Visible);
    }
    self->Render();
  });
  manualButtons.Children().Append(manualContinue);
  Button manualCancel;
  manualCancel.Content(winrt::box_value(Loc("cancel")));
  manualCancel.Click([weak = weak_from_this()](auto const&, auto const&) {
    auto self = weak.lock();
    if (!self || !self->manualRequest_) return;
    self->manualRequest_.reset();
    // answered as a quiet cancel: no error, back to the wallets
    self->sdk_.CancelBittensorProof();
    self->Render();
  });
  manualButtons.Children().Append(manualCancel);
  manualPanel_.Children().Append(manualButtons);
  content.Children().Append(manualPanel_);

  statusText_ = TextBlock();
  statusText_.FontSize(12);
  statusText_.TextWrapping(TextWrapping::Wrap);
  statusText_.Foreground(colors::MutedBrush());
  content.Children().Append(statusText_);

  // ---- email or phone: the sign-in to add ----
  signInPanel_ = StackPanel();
  signInPanel_.Spacing(12);
  authBox_ = TextBox();
  authBox_.Style(Lookup(L"UrTextInputStyle"));
  authBox_.Header(winrt::box_value(Loc("your_email")));
  authBox_.TextChanged([weak = weak_from_this()](auto const&, auto const&) {
    if (auto self = weak.lock()) self->Render();
  });
  signInPanel_.Children().Append(authBox_);
  passwordBox_ = PasswordBox();
  passwordBox_.Style(Lookup(L"UrPasswordInputStyle"));
  passwordBox_.Header(winrt::box_value(Loc("password_label")));
  passwordBox_.PasswordChanged([weak = weak_from_this()](auto const&, auto const&) {
    if (auto self = weak.lock()) self->Render();
  });
  signInPanel_.Children().Append(passwordBox_);
  Supporting(signInPanel_, Loc("password_must_be_at_least_12_characters_long"));
  content.Children().Append(signInPanel_);

  // ---- page 2: verify the added email or phone ----
  codePanel_ = StackPanel();
  codePanel_.Spacing(12);
  Supporting(codePanel_, Loc("verify_explanation"));
  codeBox_ = TextBox();
  codeBox_.Style(Lookup(L"UrTextInputStyle"));
  codeBox_.Header(winrt::box_value(Loc("verify_input_label")));
  codeBox_.TextChanged([weak = weak_from_this()](auto const&, auto const&) {
    if (auto self = weak.lock()) self->Render();
  });
  codePanel_.Children().Append(codeBox_);
  noticeText_ = TextBlock();
  noticeText_.FontSize(12);
  noticeText_.TextWrapping(TextWrapping::Wrap);
  codePanel_.Children().Append(noticeText_);
  content.Children().Append(codePanel_);

  errorText_ = TextBlock();
  errorText_.FontSize(12);
  errorText_.TextWrapping(TextWrapping::Wrap);
  errorText_.Foreground(colors::DangerBrush());
  errorText_.Visibility(Visibility::Collapsed);
  content.Children().Append(errorText_);

  ScrollViewer scroller;
  scroller.Content(content);
  dialog_.Content(scroller);
  dialog_.PrimaryButtonClick(
      [weak = weak_from_this()](auto const&, ContentDialogButtonClickEventArgs const& args) {
        args.Cancel(true);  // the flow closes the dialog once the code is verified
        auto self = weak.lock();
        if (!self) return;
        if (self->conversion_->Step() == GuestConversionStep::EnterSignIn) {
          // only the email method has the Add button
          if (self->method_ != add_sign_in::Method::Email || !self->sdk_.IsLoggedIn()) return;
          self->conversion_->SubmitSignIn(Narrow(self->authBox_.Text().c_str()),
                                          Narrow(self->passwordBox_.Password().c_str()));
        } else {
          self->conversion_->SubmitCode(Narrow(self->codeBox_.Text().c_str()));
        }
      });
  dialog_.SecondaryButtonClick(
      [weak = weak_from_this()](auto const&, ContentDialogButtonClickEventArgs const& args) {
        args.Cancel(true);
        if (auto self = weak.lock()) self->conversion_->Resend();
      });
  cooldownTimer_ = MakeCooldownTimer(dialog_.DispatcherQueue(), [weak = weak_from_this()] {
    if (auto self = weak.lock()) self->Render();
  });
  conversion_->on_changed = [weak = weak_from_this()] {
    if (auto self = weak.lock()) self->Render();
  };
  add_->on_changed = [weak = weak_from_this()] {
    auto self = weak.lock();
    if (!self) return;
    // the browser or the wallet answered: its status line and form are done
    if (!self->add_->Busy()) {
      self->browserHint_ = hstring{};
      self->manualRequest_.reset();
    }
    self->Render();
  };
  Render();
}

void AddAuthSheet::SelectMethod(add_sign_in::Method method) {
  if (method == method_) return;
  // a browser or wallet step for the previous method is abandoned
  if (add_->Busy()) add_->Cancel();
  manualRequest_.reset();
  browserHint_ = hstring{};
  method_ = method;
  Render();
}

void AddAuthSheet::StartWallet(add_sign_in::WalletChain chain, std::string const& walletId) {
  if (!sdk_.IsLoggedIn() || add_->Busy()) return;
  // Talisman asks for the extension's approval, WalletConnect for a scan; a
  // Solana wallet continues in the browser too
  browserHint_ = hstring{};
  if (chain == add_sign_in::WalletChain::Bittensor) {
    const std::string hintKey = bittensor::BrowserHintKey(walletId);
    if (hintKey == "bittensor_continue_in_browser") {
      browserHint_ = hstring{
          Format("bittensor_continue_in_browser", Widen(urnet::bittensorWalletDisplayName(walletId)))};
    } else if (!hintKey.empty()) {
      browserHint_ = Loc(hintKey);
    }
  }
  add_->StartWallet(chain, walletId);
}

void AddAuthSheet::ShowManual(SdkHost::BittensorManualRequest request) {
  // only while this sheet's Bittensor step waits for it
  if (!add_->Busy() || method_ != add_sign_in::Method::Wallet) {
    sdk_.CancelBittensorProof();
    return;
  }
  manualInstructions_.Text(
      hstring{Format("bittensor_manual_sign_instructions", Widen(request.walletName))});
  manualMessageBox_.Text(H(request.message));
  manualAddressBox_.Text(H(request.address));
  manualSignatureBox_.Text(hstring{});
  manualErrorText_.Visibility(Visibility::Collapsed);
  manualRequest_ = std::move(request);
  Render();
}

void AddAuthSheet::Render() {
  // Apple, Google and a wallet are added once addAuth accepts them (no code);
  // an email or phone once its code is verified
  const GuestConversionStep step =
      add_->Added() ? GuestConversionStep::Done : conversion_->Step();
  const bool busy = conversion_->Busy() || add_->Busy();
  const bool signIn =
      step == GuestConversionStep::EnterSignIn || step == GuestConversionStep::AddingSignIn;
  const bool email = method_ == add_sign_in::Method::Email;
  const bool provider = !add_sign_in::SsoProvider(method_).empty();
  const bool wallet = method_ == add_sign_in::Method::Wallet;
  if (step == GuestConversionStep::Done) {
    // added only now that the code was accepted, or addAuth took the credential
    if (done_) return;
    done_ = true;
    if (cooldownTimer_) cooldownTimer_.Stop();
    addedMessageKey_ = std::string(add_sign_in::AddedMessageKey(add_->Added() ? *add_->Added() : method_));
    if (onChanged_) onChanged_();
    dialog_.Hide();
    return;
  }
  methodPicker_.Visibility(signIn ? Visibility::Visible : Visibility::Collapsed);
  // the method can change while a browser step waits (it is abandoned), not
  // while an email is being added
  methodPicker_.IsEnabled(!conversion_->Busy());
  signInPanel_.Visibility(signIn && email ? Visibility::Visible : Visibility::Collapsed);
  codePanel_.Visibility(signIn ? Visibility::Collapsed : Visibility::Visible);
  providerPanel_.Visibility(signIn && provider ? Visibility::Visible : Visibility::Collapsed);
  walletPanel_.Visibility(signIn && wallet && !manualRequest_ ? Visibility::Visible
                                                              : Visibility::Collapsed);
  manualPanel_.Visibility(signIn && wallet && manualRequest_ ? Visibility::Visible
                                                             : Visibility::Collapsed);
  if (provider) {
    providerHint_.Text(Loc(std::string(add_sign_in::MethodHintKey(method_))));
    providerButton_.Content(winrt::box_value(Loc(std::string(add_sign_in::ProviderButtonKey(method_)))));
    providerButton_.IsEnabled(!busy && sdk_.IsLoggedIn());
  }
  for (auto const& button : walletButtons_) button.IsEnabled(!busy && sdk_.IsLoggedIn());
  const bool showHint = signIn && !email && add_->Busy() && !manualRequest_ && !browserHint_.empty();
  statusText_.Text(showHint ? browserHint_ : hstring{});
  statusText_.Visibility(showHint ? Visibility::Visible : Visibility::Collapsed);

  if (signIn && !sdk_.IsLoggedIn()) {
    // Adding a sign-in method to no account is not a thing; say so rather than
    // offering a form whose submit would 401 in silence.
    ApplyFieldState(errorText_, FieldState::NoSession);
    errorText_.Visibility(Visibility::Visible);
  } else {
    hstring error;
    if (email || !signIn) {
      error = H(conversion_->Error());
    } else if (!add_->Error().empty()) {
      error = H(WalletProofRefusalText(add_->ErrorCode(), add_->Error(), add_->ErrorWalletId()));
    } else if (!add_->ErrorKey().empty()) {
      error = Loc(add_->ErrorKey());
    }
    errorText_.Foreground(colors::DangerBrush());
    errorText_.Text(error);
    errorText_.Visibility(error.empty() ? Visibility::Collapsed : Visibility::Visible);
  }
  if (signIn) {
    // apple AddAuthSheet formValid for the email leg: an auth AND a 12-char
    // password. The server is the real validator; this only gates the button.
    // Apple, Google and the wallets have their own buttons and no Add.
    dialog_.PrimaryButtonText(email ? Loc("add") : hstring{});
    dialog_.SecondaryButtonText(hstring{});
    authBox_.IsEnabled(!busy && sdk_.IsLoggedIn());
    passwordBox_.IsEnabled(!busy && sdk_.IsLoggedIn());
    dialog_.IsPrimaryButtonEnabled(
        email && !busy && sdk_.IsLoggedIn() &&
        GuestConversion::CanSubmitSignIn(Narrow(authBox_.Text().c_str()),
                                         Narrow(passwordBox_.Password().c_str())));
    return;
  }
  dialog_.PrimaryButtonText(Loc("verify"));
  dialog_.SecondaryButtonText(Loc("resend_verify_code"));
  dialog_.IsSecondaryButtonEnabled(conversion_->CanResend());
  dialog_.IsPrimaryButtonEnabled(
      !busy && !GuestConversion::Trim(Narrow(codeBox_.Text().c_str())).empty());
  ShowSendNotice(noticeText_, conversion_->ShownNotice());
  RunCooldownTimer(cooldownTimer_, *conversion_);
}

// ---- GuestConversionSheet --------------------------------------------------


std::shared_ptr<GuestConversionSheet> GuestConversionSheet::Create(
    XamlRoot const& root, SdkHost& sdk, SubscriptionBalanceStore& balance,
    std::function<void()> onDone) {
  auto sheet =
      std::shared_ptr<GuestConversionSheet>(new GuestConversionSheet(sdk, std::move(onDone)));
  sheet->Build(root, balance);
  return sheet;
}

GuestConversionSheet::~GuestConversionSheet() {
  if (cooldownTimer_) cooldownTimer_.Stop();
  // the conversion goes first: it drops its answers before the session goes
  conversion_.reset();
  session_.reset();
}

void GuestConversionSheet::Build(XamlRoot const& root, SubscriptionBalanceStore& balance) {
  dialog_ = MakeSheet(root, Loc("create_an_account"));
  dialog_.CloseButtonText(Loc("cancel"));
  dialog_.DefaultButton(ContentDialogButton::Primary);
  session_ = std::make_unique<SdkGuestConversionSession>(sdk_, &balance, dialog_.DispatcherQueue());
  conversion_ = std::make_unique<GuestConversion>(*session_);

  StackPanel content;
  content.MinWidth(380);
  content.Spacing(12);

  // ---- page 1: the sign-in to add ----
  signInPanel_ = StackPanel();
  signInPanel_.Spacing(12);
  Supporting(signInPanel_, Loc("guest_convert_explanation"));
  authBox_ = TextBox();
  authBox_.Style(Lookup(L"UrTextInputStyle"));
  authBox_.Header(winrt::box_value(Loc("your_email")));
  authBox_.TextChanged([weak = weak_from_this()](auto const&, auto const&) {
    if (auto self = weak.lock()) self->Render();
  });
  signInPanel_.Children().Append(authBox_);
  passwordBox_ = PasswordBox();
  passwordBox_.Style(Lookup(L"UrPasswordInputStyle"));
  passwordBox_.Header(winrt::box_value(Loc("password_label")));
  passwordBox_.PasswordChanged([weak = weak_from_this()](auto const&, auto const&) {
    if (auto self = weak.lock()) self->Render();
  });
  signInPanel_.Children().Append(passwordBox_);
  Supporting(signInPanel_, Loc("password_must_be_at_least_12_characters_long"));
  content.Children().Append(signInPanel_);

  // ---- page 2: verify the added sign-in ----
  codePanel_ = StackPanel();
  codePanel_.Spacing(12);
  Supporting(codePanel_, Loc("verify_explanation"));
  codeBox_ = TextBox();
  codeBox_.Style(Lookup(L"UrTextInputStyle"));
  codeBox_.Header(winrt::box_value(Loc("verify_input_label")));
  codeBox_.TextChanged([weak = weak_from_this()](auto const&, auto const&) {
    if (auto self = weak.lock()) self->Render();
  });
  codePanel_.Children().Append(codeBox_);
  noticeText_ = TextBlock();
  noticeText_.FontSize(12);
  noticeText_.TextWrapping(TextWrapping::Wrap);
  codePanel_.Children().Append(noticeText_);
  content.Children().Append(codePanel_);

  errorText_ = TextBlock();
  errorText_.FontSize(12);
  errorText_.TextWrapping(TextWrapping::Wrap);
  errorText_.Foreground(colors::DangerBrush());
  content.Children().Append(errorText_);

  dialog_.Content(content);
  dialog_.PrimaryButtonClick(
      [weak = weak_from_this()](auto const&, ContentDialogButtonClickEventArgs const& args) {
        args.Cancel(true);  // the conversion closes the dialog when it is done
        auto self = weak.lock();
        if (!self) return;
        if (self->conversion_->Step() == GuestConversionStep::EnterSignIn) {
          self->conversion_->SubmitSignIn(Narrow(self->authBox_.Text().c_str()),
                                          Narrow(self->passwordBox_.Password().c_str()));
        } else {
          self->conversion_->SubmitCode(Narrow(self->codeBox_.Text().c_str()));
        }
      });
  dialog_.SecondaryButtonClick(
      [weak = weak_from_this()](auto const&, ContentDialogButtonClickEventArgs const& args) {
        args.Cancel(true);
        if (auto self = weak.lock()) self->conversion_->Resend();
      });
  cooldownTimer_ = MakeCooldownTimer(dialog_.DispatcherQueue(), [weak = weak_from_this()] {
    if (auto self = weak.lock()) self->Render();
  });
  conversion_->on_changed = [weak = weak_from_this()] {
    if (auto self = weak.lock()) self->Render();
  };
  Render();
}

void GuestConversionSheet::Render() {
  const GuestConversionStep step = conversion_->Step();
  const bool busy = conversion_->Busy();
  const bool signIn =
      step == GuestConversionStep::EnterSignIn || step == GuestConversionStep::AddingSignIn;
  signInPanel_.Visibility(signIn ? Visibility::Visible : Visibility::Collapsed);
  codePanel_.Visibility(signIn ? Visibility::Collapsed : Visibility::Visible);
  errorText_.Text(H(conversion_->Error()));
  errorText_.Visibility(conversion_->Error().empty() ? Visibility::Collapsed
                                                     : Visibility::Visible);
  if (step == GuestConversionStep::Done) {
    if (done_) return;
    done_ = true;
    if (cooldownTimer_) cooldownTimer_.Stop();
    if (onDone_) onDone_();
    dialog_.Hide();
    return;
  }
  if (signIn) {
    dialog_.PrimaryButtonText(Loc("add"));
    dialog_.SecondaryButtonText(hstring{});
    authBox_.IsEnabled(!busy);
    passwordBox_.IsEnabled(!busy);
    dialog_.IsPrimaryButtonEnabled(
        !busy && sdk_.IsLoggedIn() &&
        GuestConversion::CanSubmitSignIn(Narrow(authBox_.Text().c_str()),
                                         Narrow(passwordBox_.Password().c_str())));
    return;
  }
  dialog_.PrimaryButtonText(Loc("verify"));
  dialog_.SecondaryButtonText(Loc("resend_verify_code"));
  dialog_.IsSecondaryButtonEnabled(conversion_->CanResend());
  dialog_.IsPrimaryButtonEnabled(
      !busy && !GuestConversion::Trim(Narrow(codeBox_.Text().c_str())).empty());
  ShowSendNotice(noticeText_, conversion_->ShownNotice());
  RunCooldownTimer(cooldownTimer_, *conversion_);
}

// ---- ReferralNetworkSheet --------------------------------------------------

std::shared_ptr<ReferralNetworkSheet> ReferralNetworkSheet::Create(
    XamlRoot const& root, SdkHost& sdk, std::function<void()> onChanged) {
  auto sheet =
      std::shared_ptr<ReferralNetworkSheet>(new ReferralNetworkSheet(sdk, std::move(onChanged)));
  sheet->Build(root);
  sheet->Load();
  return sheet;
}

void ReferralNetworkSheet::Build(XamlRoot const& root) {
  dialog_ = MakeSheet(root, Loc("update_referral_network"));
  dialog_.PrimaryButtonText(Loc("update"));
  dialog_.IsPrimaryButtonEnabled(false);

  StackPanel content;
  content.MinWidth(sheetfit::Width(root, 400));
  content.Spacing(12);

  // What it is now, before offering to change it.
  StackPanel currentBlock;
  currentBlock.Spacing(2);
  TextBlock currentLabel;
  currentLabel.Text(Loc("current_referral_network"));
  currentLabel.Style(Lookup(L"UrLabelStyle"));
  currentBlock.Children().Append(currentLabel);
  currentText_ = TextBlock();
  currentText_.FontSize(14);
  currentText_.Text(Loc("loading"));
  currentBlock.Children().Append(currentText_);
  content.Children().Append(currentBlock);

  codeBox_ = TextBox();
  codeBox_.Style(Lookup(L"UrTextInputStyle"));
  codeBox_.Header(winrt::box_value(Loc("enter_network_referral_code")));
  codeBox_.TextChanged([weak = weak_from_this()](auto const&, auto const&) {
    auto self = weak.lock();
    if (!self) return;
    // apple UpdateReferralNetworkSheet gates Update on 6+ characters
    const std::string code = Trim(Narrow(self->codeBox_.Text().c_str()));
    // ...and a session. Without this the button armed at 6 characters with no
    // token, and pressing it produced no request and no message whatsoever.
    self->dialog_.IsPrimaryButtonEnabled(6 <= code.size() && !self->busy_ &&
                                         self->sdk_.IsLoggedIn());
    if (self->sdk_.IsLoggedIn()) self->errorText_.Visibility(Visibility::Collapsed);
  });
  content.Children().Append(codeBox_);

  // Unlink lives below the update field and only appears when there is
  // something to unlink.
  unlinkButton_ = Button();
  unlinkButton_.Content(winrt::box_value(Loc("unlink_referral_network")));
  unlinkButton_.Foreground(colors::DangerBrush());
  unlinkButton_.HorizontalAlignment(HorizontalAlignment::Left);
  unlinkButton_.Visibility(Visibility::Collapsed);
  unlinkButton_.Click([weak = weak_from_this()](auto const&, auto const&) {
    if (auto self = weak.lock()) self->ArmUnlink();
  });
  content.Children().Append(unlinkButton_);

  errorText_ = TextBlock();
  errorText_.FontSize(12);
  errorText_.TextWrapping(TextWrapping::Wrap);
  errorText_.Foreground(colors::DangerBrush());
  errorText_.Visibility(Visibility::Collapsed);
  content.Children().Append(errorText_);

  dialog_.Content(content);
  dialog_.PrimaryButtonClick(
      [weak = weak_from_this()](auto const&, ContentDialogButtonClickEventArgs const& args) {
        args.Cancel(true);
        auto self = weak.lock();
        if (!self) return;
        // In confirm mode the primary IS the destructive commit; otherwise it
        // is the ordinary update.
        if (self->unlinkArmed_) self->Unlink();
        else self->Submit();
      });

  if (!sdk_.IsLoggedIn()) {
    codeBox_.IsEnabled(false);
    unlinkButton_.IsEnabled(false);
    ApplyFieldState(errorText_, FieldState::NoSession);
    errorText_.Visibility(Visibility::Visible);
  }
}

void ReferralNetworkSheet::Load() {
  // apiReady() is set at SDK INIT, not at login - it is not a session check.
  if (!sdk_.IsLoggedIn()) {
    ApplyCurrent(FieldState::NoSession, {});
    return;
  }
  ApplyCurrent(FieldState::Loading, {});
  auto queue = dialog_.DispatcherQueue();
  auto weak = weak_from_this();
  sdk_.api().getReferralNetwork([queue, weak](std::optional<urnet::GetReferralNetworkResult> result,
                                              std::optional<std::string> err) {
    // "No referral network found" is how the server says NONE - it answers on
    // the error channel of a lookup that succeeded. Verified against the beta
    // network: an account with no referral network returns exactly that, and
    // rendering it as "Something went wrong." was wrong on screen. Only a
    // TRANSPORT failure (err) is a real failure here; a structured response,
    // error or not, means the server answered.
    const bool failed = !result || err.has_value();
    if (failed) {
      LogWarn("settings: getReferralNetwork (sheet) failed: {}",
              err ? *err : std::string("no result"));
    }
    std::string name;
    if (!failed && result->network) name = result->network->name;
    queue.TryEnqueue([weak, failed, name] {
      auto self = weak.lock();
      if (!self) return;
      // "You have no referral network" and "we could not find out" are
      // different answers, and unlink must only appear for the first: offering
      // to unlink something we failed to read is a destructive act on a guess.
      self->ApplyCurrent(failed ? FieldState::Failed
                                : (name.empty() ? FieldState::Empty : FieldState::Loaded),
                         name);
    });
  });
}

void ReferralNetworkSheet::ApplyCurrent(rows::FieldState state, std::string const& name) {
  currentName_ = state == FieldState::Loaded ? name : std::string();
  ApplyFieldState(currentText_, state, H(name));
  unlinkButton_.Visibility(currentName_.empty() ? Visibility::Collapsed : Visibility::Visible);
  if (unlinkArmed_) DisarmUnlink();
  unlinkButton_.Content(winrt::box_value(Loc("unlink_referral_network")));
}


void ReferralNetworkSheet::Submit() {
  if (busy_ || !sdk_.IsLoggedIn()) return;
  const std::string code = Trim(Narrow(codeBox_.Text().c_str()));
  if (code.size() < 6) return;
  busy_ = true;
  dialog_.IsPrimaryButtonEnabled(false);
  errorText_.Visibility(Visibility::Collapsed);

  urnet::SetNetworkReferralArgs args;
  args.referral_code = code;
  auto queue = dialog_.DispatcherQueue();
  auto weak = weak_from_this();
  sdk_.api().setNetworkReferral(
      args, [queue, weak](std::optional<urnet::SetNetworkReferralResult> result,
                          std::optional<std::string> err) {
        const std::string error = ServerError(result, err);
        queue.TryEnqueue([weak, error] {
          auto self = weak.lock();
          if (!self) return;
          self->busy_ = false;
          if (error.empty()) {
            self->codeBox_.Text(L"");
            if (self->onChanged_) self->onChanged_();
            // linking a referral network is the royal-welcome moment; the
            // sheet dismisses itself after the beat (reopening re-Loads)
            self->ShowRoyalWelcome();
            return;
          }
          // A rejected code is the common failure and has its own string; a
          // transport failure falls back to the generic one.
          self->ShowError(Loc("invalid_referral_code_please_try_again"));
        });
      });
}

void ReferralNetworkSheet::ShowRoyalWelcome() {
  dialog_.Title(winrt::box_value(hstring{}));
  dialog_.PrimaryButtonText(hstring{});
  dialog_.IsPrimaryButtonEnabled(false);
  // the welcome swaps into the same dialog, so it obeys the sheet's clamped
  // width too -- read off the live root (sheetfit), not re-fixed at 400
  dialog_.Content(MakeRoyalWelcomePanel(sheetfit::Width(dialog_.XamlRoot(), 400)));

  royalTimer_ = dialog_.DispatcherQueue().CreateTimer();
  royalTimer_.Interval(std::chrono::milliseconds(2000));
  royalTimer_.IsRepeating(false);
  royalTimer_.Tick([weak = weak_from_this()](auto const&, auto const&) {
    if (auto self = weak.lock()) {
      if (self->royalTimer_) self->royalTimer_.Stop();
      self->dialog_.Hide();
    }
  });
  royalTimer_.Start();
}

// Arm the confirm. A ContentDialog cannot open a second ContentDialog, so the
// confirmation cannot be the modal apple uses - but it must still be a separate
// deliberate act on a DIFFERENT control, or a double-click arms and commits in
// one gesture. So arming switches the SHEET into confirm mode: the warning that
// names what is being forfeited, the primary relabelled to the destructive
// action, and the dialog's own close button as the explicit Cancel.
void ReferralNetworkSheet::ArmUnlink() {
  if (busy_ || currentName_.empty() || unlinkArmed_) return;
  unlinkArmed_ = true;
  ShowError(hstring{urnw::Format("when_unlinking_your_referral_network_you_will_no",
                                 urnw::Widen(currentName_))});
  errorText_.Foreground(colors::DangerBrush());
  // The update field has nothing to do with the pending decision.
  codeBox_.IsEnabled(false);
  unlinkButton_.IsEnabled(false);
  dialog_.PrimaryButtonText(Loc("unlink_referral_network"));
  dialog_.IsPrimaryButtonEnabled(true);
  dialog_.DefaultButton(ContentDialogButton::Close);  // Enter must not commit
}

void ReferralNetworkSheet::DisarmUnlink() {
  unlinkArmed_ = false;
  errorText_.Visibility(Visibility::Collapsed);
  codeBox_.IsEnabled(sdk_.IsLoggedIn());
  unlinkButton_.IsEnabled(true);
  dialog_.PrimaryButtonText(Loc("update"));
  dialog_.DefaultButton(ContentDialogButton::None);
  const std::string code = Trim(Narrow(codeBox_.Text().c_str()));
  dialog_.IsPrimaryButtonEnabled(6 <= code.size() && !busy_ && sdk_.IsLoggedIn());
}

void ReferralNetworkSheet::Unlink() {
  if (busy_ || currentName_.empty() || !unlinkArmed_ || !sdk_.IsLoggedIn()) return;
  busy_ = true;
  dialog_.IsPrimaryButtonEnabled(false);
  unlinkButton_.IsEnabled(false);

  auto queue = dialog_.DispatcherQueue();
  auto weak = weak_from_this();
  sdk_.api().unlinkReferralNetwork(
      [queue, weak](std::optional<urnet::UnlinkReferralNetworkResult> result,
                    std::optional<std::string> err) {
        // UnlinkReferralNetworkResult has no error field, so "a result arrived
        // and the transport did not fail" is the whole success test.
        const bool ok = !err && result.has_value();
        const std::string error = err ? *err : std::string();
        queue.TryEnqueue([weak, ok, error] {
          auto self = weak.lock();
          if (!self) return;
          self->busy_ = false;
          self->DisarmUnlink();  // back to the normal sheet either way
          if (ok) {
            self->errorText_.Visibility(Visibility::Collapsed);
            self->Load();
            if (self->onChanged_) self->onChanged_();
            return;
          }
          self->ShowError(error.empty() ? Loc("something_went_wrong") : H(error));
        });
      });
}

void ReferralNetworkSheet::ShowError(hstring const& message) {
  errorText_.Text(message);
  errorText_.Foreground(colors::DangerBrush());
  errorText_.Visibility(Visibility::Visible);
}

// ---- BlockedLocationsSheet -------------------------------------------------

std::shared_ptr<BlockedLocationsSheet> BlockedLocationsSheet::Create(XamlRoot const& root,
                                                                     SdkHost& sdk) {
  auto sheet = std::shared_ptr<BlockedLocationsSheet>(new BlockedLocationsSheet(sdk));
  sheet->Build(root);
  sheet->LoadBlocked();
  sheet->LoadCountries();
  return sheet;
}

void BlockedLocationsSheet::Build(XamlRoot const& root) {
  dialog_ = MakeSheet(root, Loc("blocked_locations"));

  StackPanel content;
  content.MinWidth(sheetfit::Width(root, 420));
  content.Spacing(12);

  // What is blocked now.
  blockedPanel_ = StackPanel();
  blockedPanel_.Spacing(4);
  content.Children().Append(blockedPanel_);
  blockedEmpty_ = TextBlock();
  blockedEmpty_.Text(Loc("no_blocked_locations"));
  blockedEmpty_.FontSize(12);
  blockedEmpty_.Foreground(colors::FaintBrush());
  content.Children().Append(blockedEmpty_);

  Divider(content);

  // Add: the country picker, searched client-side over the provider countries.
  TextBlock addLabel;
  addLabel.Text(Loc("select_country_to_block"));
  addLabel.Style(Lookup(L"UrLabelStyle"));
  content.Children().Append(addLabel);

  search_ = TextBox();
  search_.Style(Lookup(L"UrTextInputStyle"));
  search_.PlaceholderText(Loc("search_placeholder"));
  search_.TextChanged([weak = weak_from_this()](auto const&, auto const&) {
    if (auto self = weak.lock()) self->RenderCountries();
  });
  content.Children().Append(search_);

  ScrollViewer scroll;
  scroll.MaxHeight(sheetfit::Height(root, 240));
  scroll.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
  countryPanel_ = StackPanel();
  countryPanel_.Spacing(2);
  scroll.Content(countryPanel_);
  content.Children().Append(scroll);

  // iOS assigns a message on a block/unblock failure and never renders it, so
  // the only sign is the row silently reappearing. This line is that sign.
  errorText_ = TextBlock();
  errorText_.FontSize(12);
  errorText_.TextWrapping(TextWrapping::Wrap);
  errorText_.Foreground(colors::DangerBrush());
  errorText_.Visibility(Visibility::Collapsed);
  content.Children().Append(errorText_);

  dialog_.Content(content);
}

void BlockedLocationsSheet::LoadBlocked() {
  // apiReady() is NOT a session check - it is api_.has_value(), set at SDK
  // INIT, not at login (Startup.h says so about the preview switch, and this
  // sheet proved it: opened with no session it fired a real unauthenticated
  // request at production and rendered the 401 as "No blocked locations").
  if (!sdk_.IsLoggedIn()) {
    blockedState_ = FieldState::NoSession;
    RenderBlocked();
    return;
  }
  blockedState_ = FieldState::Loading;
  RenderBlocked();
  auto queue = dialog_.DispatcherQueue();
  auto weak = weak_from_this();
  sdk_.api().getNetworkBlockedLocations(
      [queue, weak](std::optional<urnet::GetNetworkBlockedLocationsResult> result,
                    std::optional<std::string> err) {
        // The server never sets GetNetworkBlockedLocationsResult.error, so a
        // missing result or a transport error is the only failure signal there
        // is - and without this check a 401 arrived as an empty list and
        // rendered as the reassuring "No blocked locations".
        const bool failed = !result || err.has_value();
        if (failed) {
          LogWarn("settings: getNetworkBlockedLocations failed: {}",
                  err ? *err : std::string("no result"));
        }
        std::vector<urnet::BlockedLocation> list;
        if (!failed && result->blocked_locations) list = *result->blocked_locations;
        std::sort(list.begin(), list.end(),
                  [](urnet::BlockedLocation const& a, urnet::BlockedLocation const& b) {
                    return a.location_name < b.location_name;
                  });
        queue.TryEnqueue([weak, failed, list = std::move(list)]() mutable {
          auto self = weak.lock();
          if (!self) return;
          self->blocked_ = std::move(list);
          self->blockedState_ = failed ? FieldState::Failed
                                       : (self->blocked_.empty() ? FieldState::Empty
                                                                 : FieldState::Loaded);
          self->RenderBlocked();
          self->RenderCountries();  // already-blocked countries drop out of the picker
        });
      });
}

void BlockedLocationsSheet::LoadCountries() {
  // Same session gate as the blocked list. getProviderLocations happens to be
  // an UNAUTHENTICATED endpoint - opened with no session it really did return
  // the full country list - but a development switch must not talk to
  // production either way, so it is gated with the rest.
  if (!sdk_.IsLoggedIn()) {
    countriesState_ = FieldState::NoSession;
    RenderCountries();
    return;
  }
  if (loadingCountries_) return;
  loadingCountries_ = true;
  countriesState_ = FieldState::Loading;
  auto queue = dialog_.DispatcherQueue();
  auto weak = weak_from_this();
  // getProviderLocations is the provider-country list iOS's add sheet is handed
  // (it passes `providerCountries` in). Countries only: the sheet blocks
  // countries, not cities or regions.
  sdk_.api().getProviderLocations([queue, weak](std::optional<urnet::FindLocationsResult> result,
                                                std::optional<std::string> err) {
    const bool failed = !result || err.has_value();
    if (failed) {
      LogWarn("settings: getProviderLocations failed: {}",
              err ? *err : std::string("no result"));
    }
    std::vector<std::pair<std::string, std::string>> countries;
    if (result && result->locations) {
      for (auto const& location : *result->locations) {
        if (location.location_type != urnet::LocationTypeCountry) continue;
        const std::string id =
            location.location_id ? *location.location_id
                                 : (location.country_location_id ? *location.country_location_id
                                                                 : std::string());
        if (id.empty() || location.name.empty()) continue;
        countries.emplace_back(id, location.name);
      }
    }
    std::sort(countries.begin(), countries.end(),
              [](auto const& a, auto const& b) { return a.second < b.second; });
    queue.TryEnqueue([weak, failed, countries = std::move(countries)]() mutable {
      auto self = weak.lock();
      if (!self) return;
      self->loadingCountries_ = false;
      self->countries_ = std::move(countries);
      self->countriesState_ = failed ? FieldState::Failed
                                     : (self->countries_.empty() ? FieldState::Empty
                                                                 : FieldState::Loaded);
      self->RenderCountries();
    });
  });
}

void BlockedLocationsSheet::RenderBlocked() {
  blockedPanel_.Children().Clear();
  // Four outcomes, four different lines. "No blocked locations" is reserved for
  // the one case where the server actually said so.
  if (blockedState_ != FieldState::Loaded || blocked_.empty()) {
    blockedEmpty_.Visibility(Visibility::Visible);
    ApplyFieldState(blockedEmpty_,
                    blockedState_ == FieldState::Loaded ? FieldState::Empty : blockedState_);
    if (blockedState_ == FieldState::Empty ||
        (blockedState_ == FieldState::Loaded && blocked_.empty())) {
      // the shipped, specific empty line beats the generic "None"
      blockedEmpty_.Text(Loc("no_blocked_locations"));
    }
    if (blocked_.empty()) return;
  } else {
    blockedEmpty_.Visibility(Visibility::Collapsed);
  }
  for (auto const& location : blocked_) {
    const std::string id = location.location_id ? *location.location_id : std::string();
    Button remove;
    remove.Content(winrt::box_value(Loc("remove")));
    remove.Foreground(colors::DangerBrush());
    remove.IsEnabled(!id.empty());
    remove.Click([weak = weak_from_this(), id](auto const&, auto const&) {
      if (auto self = weak.lock()) self->Unblock(id);
    });
    Row(blockedPanel_, H(location.location_name), hstring{}, remove);
  }
}

void BlockedLocationsSheet::RenderCountries() {
  countryPanel_.Children().Clear();
  const std::string query = LowerAscii(Trim(Narrow(search_.Text().c_str())));
  int shown = 0;
  for (auto const& [id, name] : countries_) {
    if (!query.empty() && LowerAscii(name).find(query) == std::string::npos) continue;
    // already blocked: not offered again (iOS dedupes in blockLocation)
    const bool alreadyBlocked =
        std::any_of(blocked_.begin(), blocked_.end(), [&](urnet::BlockedLocation const& b) {
          return b.location_id && *b.location_id == id;
        });
    if (alreadyBlocked) continue;

    Button row;
    row.Style(Lookup(L"UrCardRowButtonStyle"));
    row.HorizontalAlignment(HorizontalAlignment::Stretch);
    row.HorizontalContentAlignment(HorizontalAlignment::Left);
    row.Content(winrt::box_value(H(name)));
    row.Click([weak = weak_from_this(), id](auto const&, auto const&) {
      if (auto self = weak.lock()) self->Block(id);
    });
    countryPanel_.Children().Append(row);
    ++shown;
  }
  if (shown == 0) {
    // Distinguish "still loading", "no session", "the fetch failed" and "your
    // search matched nothing" - one blank panel for all four is the bug the
    // leaderboard already had on this project.
    TextBlock note;
    note.FontSize(12);
    note.TextWrapping(TextWrapping::Wrap);
    if (countriesState_ == FieldState::Loaded && !countries_.empty()) {
      note.Text(Loc("no_locations_found"));  // loaded, but the search matched none
      note.Foreground(colors::FaintBrush());
    } else {
      ApplyFieldState(note, countriesState_ == FieldState::Loaded ? FieldState::Empty
                                                                 : countriesState_);
    }
    countryPanel_.Children().Append(note);
  }
}

void BlockedLocationsSheet::Block(std::string const& locationId) {
  if (!sdk_.IsLoggedIn() || locationId.empty()) return;
  errorText_.Visibility(Visibility::Collapsed);
  urnet::NetworkBlockLocationArgs args;
  args.location_id = locationId;
  auto queue = dialog_.DispatcherQueue();
  auto weak = weak_from_this();
  sdk_.api().networkBlockLocation(
      args, [queue, weak](std::optional<urnet::NetworkBlockLocationResult> result,
                          std::optional<std::string> err) {
        const std::string error = ServerError(result, err);
        queue.TryEnqueue([weak, error] {
          auto self = weak.lock();
          if (!self) return;
          if (!error.empty()) {
            self->ShowError(Loc("blocked_location_could_not_be_added_please_try"));
            return;
          }
          // Re-read rather than inserting a locally-built row: the server owns
          // the name and type, and a refetch cannot drift from it.
          self->LoadBlocked();
        });
      });
}

void BlockedLocationsSheet::Unblock(std::string const& locationId) {
  if (!sdk_.IsLoggedIn() || locationId.empty()) return;
  errorText_.Visibility(Visibility::Collapsed);
  urnet::NetworkUnblockLocationArgs args;
  args.location_id = locationId;
  auto queue = dialog_.DispatcherQueue();
  auto weak = weak_from_this();
  sdk_.api().networkUnblockLocation(
      args, [queue, weak](std::optional<urnet::NetworkUnblockLocationResult> result,
                          std::optional<std::string> err) {
        const std::string error = ServerError(result, err);
        queue.TryEnqueue([weak, error] {
          auto self = weak.lock();
          if (!self) return;
          if (!error.empty()) {
            self->ShowError(Loc("blocked_location_could_not_be_removed_please_try"));
          }
          self->LoadBlocked();  // re-sync either way
        });
      });
}

void BlockedLocationsSheet::ShowError(hstring const& message) {
  errorText_.Text(message);
  errorText_.Visibility(Visibility::Visible);
}

// ---- PostQuantumIdentitySheet ----------------------------------------------

namespace {

// The canonical display form the apple client uses for a 52-char identity key
// hash: 4-char groups, space joined, and elided in the middle past 6 groups.
// Copy always takes the raw hash, never this.
std::string GroupHash(std::string const& hash, bool elide) {
  std::vector<std::string> groups;
  for (size_t i = 0; i < hash.size(); i += 4) groups.push_back(hash.substr(i, 4));
  std::string out;
  auto append = [&out](std::string const& part) {
    if (!out.empty()) out += ' ';
    out += part;
  };
  if (elide && 6 < groups.size()) {
    for (size_t i = 0; i < 4; ++i) append(groups[i]);
    append("...");  // middle elision
    append(groups[groups.size() - 2]);
    append(groups[groups.size() - 1]);
    return out;
  }
  for (auto const& group : groups) append(group);
  return out;
}

}  // namespace

std::shared_ptr<PostQuantumIdentitySheet> PostQuantumIdentitySheet::Create(XamlRoot const& root,
                                                                           SdkHost& sdk) {
  auto sheet = std::shared_ptr<PostQuantumIdentitySheet>(new PostQuantumIdentitySheet(sdk));
  sheet->Build(root);
  sheet->Load();
  return sheet;
}

void PostQuantumIdentitySheet::Build(XamlRoot const& root) {
  dialog_ = MakeSheet(root, Loc("post_quantum_identity"));

  StackPanel content;
  content.MinWidth(sheetfit::Width(root, 420));
  content.Spacing(12);

  identicon_ = Image();
  identicon_.Width(80);
  identicon_.Height(80);
  identicon_.HorizontalAlignment(HorizontalAlignment::Center);
  identicon_.Visibility(Visibility::Collapsed);
  content.Children().Append(identicon_);

  hashText_ = TextBlock();
  hashText_.FontSize(13);
  hashText_.FontFamily(Media::FontFamily(L"Consolas"));
  hashText_.TextWrapping(TextWrapping::Wrap);
  hashText_.HorizontalAlignment(HorizontalAlignment::Center);
  hashText_.TextAlignment(TextAlignment::Center);
  hashText_.IsTextSelectionEnabled(true);
  content.Children().Append(hashText_);

  copyHash_ = Button();
  copyHash_.Content(winrt::box_value(Loc("copy")));
  copyHash_.HorizontalAlignment(HorizontalAlignment::Center);
  // Nothing has been read yet, so there is nothing to copy. An enabled Copy
  // over an empty hash is an affordance that lies about what it will do.
  copyHash_.IsEnabled(false);
  copyHash_.Click([weak = weak_from_this()](auto const&, auto const&) {
    auto self = weak.lock();
    if (!self || self->hash_.empty()) return;
    CopyToClipboard(self->hash_);  // the raw hash, never the grouped display form
    self->statusText_.Text(Loc("identity_key_hash_copied"));
    self->statusText_.Foreground(colors::MutedBrush());
    self->statusText_.Visibility(Visibility::Visible);
  });
  content.Children().Append(copyHash_);

  Supporting(content, Loc("post_quantum_identity_explanation"));
  Divider(content);

  TextBlock providersLabel;
  providersLabel.Text(Loc("provider_identities"));
  providersLabel.Style(Lookup(L"UrLabelStyle"));
  content.Children().Append(providersLabel);

  ScrollViewer scroll;
  scroll.MaxHeight(sheetfit::Height(root, 200));
  scroll.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
  providerPanel_ = StackPanel();
  providerPanel_.Spacing(6);
  scroll.Content(providerPanel_);
  content.Children().Append(scroll);

  statusText_ = TextBlock();
  statusText_.FontSize(12);
  statusText_.TextWrapping(TextWrapping::Wrap);
  statusText_.Foreground(colors::FaintBrush());
  content.Children().Append(statusText_);

  dialog_.Content(content);
}

// N3: these are three DeviceRemote calls, i.e. three synchronous RPCs to the
// service, and they used to run on the UI thread BEFORE ShowAsync - so a wedged
// or slow service froze the whole app with no dialog on screen to explain it.
// The dialog now shows first and the reads happen on a background thread.
winrt::fire_and_forget PostQuantumIdentitySheet::Load() {
  // DeviceRemote, so this needs a live service session. With none, say so:
  // an empty provider list would otherwise read as "no peer has an identity",
  // which is a claim about the network rather than about this app.
  if (!sdk_.hasDevice()) {
    // Say it where the hash would be, not only under the provider list, and
    // leave Copy disabled - there is nothing to put on the clipboard.
    const FieldState state =
        sdk_.IsLoggedIn() ? FieldState::NoDevice : FieldState::NoSession;
    ApplyFieldState(hashText_, state);
    ApplyFieldState(statusText_, state);
    co_return;
  }
  ApplyFieldState(hashText_, FieldState::Loading);
  ApplyFieldState(statusText_, FieldState::Loading);

  auto weak = weak_from_this();
  auto queue = dialog_.DispatcherQueue();
  std::string hash;
  std::vector<uint8_t> key;
  std::optional<urnet::ProviderIdentityList> providers;
  bool failed = false;

  co_await winrt::resume_background();
  try {
    auto& device = sdk_.device();
    hash = device.getPublicIdentityKeyHash();
    key = device.getPublicIdentityKey();
    providers = device.getProviderIdentities();
  } catch (const std::exception& e) {
    LogWarn("settings: post quantum identity read failed: {}", e.what());
    failed = true;
  } catch (...) {
    LogWarn("settings: post quantum identity read failed");
    failed = true;
  }
  // Back to the UI thread the way every other callback in this file does it -
  // there is no resume_foreground overload for Microsoft.UI.Dispatching.
  queue.TryEnqueue([weak, failed, hash, key, providers] {
    // The sheet may have been dismissed while the RPCs were in flight.
    if (auto self = weak.lock()) self->ApplyIdentity(failed, hash, key, providers);
  });
}

void PostQuantumIdentitySheet::ApplyIdentity(
    bool failed, std::string const& hash, std::vector<uint8_t> const& key,
    std::optional<urnet::ProviderIdentityList> const& providers) {
  if (failed) {
    ApplyFieldState(hashText_, FieldState::Failed);
    ApplyFieldState(statusText_, FieldState::Failed);
    return;
  }

  hash_ = hash;
  if (hash_.empty()) {
    ApplyFieldState(hashText_, FieldState::Empty);
  } else {
    ApplyFieldState(hashText_, FieldState::Loaded, H(GroupHash(hash, /*elide=*/false)));
    copyHash_.IsEnabled(true);
  }

  // The identicon is the point of the panel: it is byte-identical on every
  // platform for the same key, so two people can compare glyphs out of band.
  // Rendered at 2x the display size, as apple does, so it stays crisp.
  if (!key.empty()) {
    try {
      auto png = urnet::renderIdenticonPng(key, 160);
      if (!png.empty()) SetIdenticon(std::move(png));
    } catch (const std::exception& e) {
      LogWarn("settings: identicon render failed: {}", e.what());
    }
  }

  providerPanel_.Children().Clear();
  const int64_t count = providers ? static_cast<int64_t>(providers->size()) : 0;
  if (providers) {
    for (auto const& identity : *providers) {
      StackPanel row;
      row.Spacing(2);
      TextBlock client;
      client.Text(H(identity.ClientId ? *identity.ClientId : std::string()));
      client.FontSize(12);
      client.FontFamily(Media::FontFamily(L"Consolas"));
      client.TextTrimming(TextTrimming::CharacterEllipsis);
      row.Children().Append(client);
      TextBlock key2;
      key2.Text(H(GroupHash(identity.PublicKey, /*elide=*/true)));
      key2.FontSize(11);
      key2.FontFamily(Media::FontFamily(L"Consolas"));
      key2.Foreground(colors::FaintBrush());
      key2.TextTrimming(TextTrimming::CharacterEllipsis);
      row.Children().Append(key2);
      providerPanel_.Children().Append(row);
    }
  }
  statusText_.Text(hstring{urnw::Plural("connected_provider_count", count)});
  statusText_.Foreground(colors::FaintBrush());
}

// Feeding PNG bytes to a BitmapImage means an IRandomAccessStream, and every
// step of that is async. Doing it with blocking .get() calls would be a
// synchronous wait on the UI thread — the STA-blocking-wait C++/WinRT asserts
// on — so this is a coroutine that starts on the UI thread and resumes there.
winrt::fire_and_forget PostQuantumIdentitySheet::SetIdenticon(std::vector<uint8_t> png) {
  auto weak = weak_from_this();
  try {
    winrt::Windows::Storage::Streams::InMemoryRandomAccessStream stream;
    winrt::Windows::Storage::Streams::DataWriter writer{stream};
    writer.WriteBytes(winrt::array_view<const uint8_t>(png.data(), png.data() + png.size()));
    co_await writer.StoreAsync();
    co_await writer.FlushAsync();
    writer.DetachStream();
    stream.Seek(0);

    Media::Imaging::BitmapImage bitmap;
    co_await bitmap.SetSourceAsync(stream);
    // The sheet may have been dismissed while the decode was in flight.
    if (auto self = weak.lock()) {
      self->identicon_.Source(bitmap);
      self->identicon_.Visibility(Visibility::Visible);
    }
  } catch (const std::exception& e) {
    LogWarn("settings: identicon decode failed: {}", e.what());
  } catch (...) {
    LogWarn("settings: identicon decode failed");
  }
}

// ---- DeleteAccountSheet ----------------------------------------------------

std::shared_ptr<DeleteAccountSheet> DeleteAccountSheet::Create(XamlRoot const& root,
                                                               SdkHost& sdk) {
  auto sheet = std::shared_ptr<DeleteAccountSheet>(new DeleteAccountSheet(sdk));
  sheet->Build(root);
  sheet->LoadNetworkName();  // the gate arms only on a fresh read
  return sheet;
}

// Read the network name of the session that is current RIGHT NOW. Nothing is
// passed in and nothing cached is trusted: see the header for why.
void DeleteAccountSheet::LoadNetworkName() {
  if (!sdk_.IsLoggedIn()) {
    ApplyName(FieldState::NoSession, {});
    return;
  }
  ApplyName(FieldState::Loading, {});
  auto queue = dialog_.DispatcherQueue();
  auto weak = weak_from_this();
  sdk_.api().getNetworkUser([queue, weak](std::optional<urnet::GetNetworkUserResult> result,
                                          std::optional<std::string> err) {
    std::string error;
    if (result && result->error) error = result->error->message;
    else if (err) error = *err;
    const bool failed = !error.empty() || !result || !result->network_user;
    if (failed) LogWarn("settings: delete-account name read failed: {}", error);
    std::string name;
    if (!failed) name = result->network_user->network_name;
    queue.TryEnqueue([weak, failed, name] {
      auto self = weak.lock();
      if (!self) return;
      // A failed read leaves the gate closed. Refusing to offer the action is
      // the only safe answer when we cannot say WHICH network would go.
      self->ApplyName(failed || name.empty() ? FieldState::Failed : FieldState::Loaded, name);
    });
  });
}

void DeleteAccountSheet::ApplyName(rows::FieldState state, std::string const& name) {
  networkName_ = state == FieldState::Loaded ? name : std::string();
  ApplyFieldState(nameText_, state, H(name));
  // The placeholder is the name to type, so it must never show a stale one.
  confirmBox_.PlaceholderText(H(networkName_));
  confirmBox_.IsEnabled(!networkName_.empty());
  UpdateGate();
}

// The single place the primary is allowed to become enabled. Fails closed on an
// empty networkName_, which is the state after any failure and before any read.
void DeleteAccountSheet::UpdateGate() {
  const std::string typed = Trim(Narrow(confirmBox_.Text().c_str()));
  dialog_.IsPrimaryButtonEnabled(!deleting_ && !networkName_.empty() &&
                                 typed == networkName_);
}

void DeleteAccountSheet::Build(XamlRoot const& root) {
  dialog_ = MakeSheet(root, Loc("are_you_sure_delete_account"));
  dialog_.PrimaryButtonText(Loc("delete_account_2"));
  dialog_.CloseButtonText(Loc("cancel"));
  // Cancel is the default so that Enter cannot destroy a network, and the
  // primary starts disabled: nothing about this dialog should be one keystroke.
  dialog_.DefaultButton(ContentDialogButton::Close);
  dialog_.IsPrimaryButtonEnabled(false);

  StackPanel content;
  content.MinWidth(sheetfit::Width(root, 400));
  content.Spacing(12);

  TextBlock warning;
  warning.Text(Loc("site_app_delete_warning"));
  warning.FontSize(13);
  warning.TextWrapping(TextWrapping::Wrap);
  warning.Foreground(colors::DangerBrush());
  content.Children().Append(warning);

  // WHICH network is about to go. Shown as its own line rather than only as a
  // placeholder, because the name is the whole basis of the decision and a
  // placeholder disappears the moment the user starts typing.
  nameText_ = TextBlock();
  nameText_.FontSize(14);
  nameText_.TextWrapping(TextWrapping::Wrap);
  content.Children().Append(nameText_);

  // The typed-name gate. Api::networkDelete takes no arguments and cannot be
  // undone, so this is the only thing between a mis-click and a destroyed
  // network. iOS ships a one-tap destructive confirmation here; this is
  // deliberately stricter, and it spends a string the store already carries.
  confirmBox_ = TextBox();
  confirmBox_.Style(Lookup(L"UrTextInputStyle"));
  confirmBox_.Header(winrt::box_value(Loc("site_app_delete_confirm")));
  confirmBox_.IsEnabled(false);  // until a fresh name lands
  confirmBox_.TextChanged([weak = weak_from_this()](auto const&, auto const&) {
    if (auto self = weak.lock()) self->UpdateGate();
  });
  content.Children().Append(confirmBox_);

  errorText_ = TextBlock();
  errorText_.FontSize(12);
  errorText_.TextWrapping(TextWrapping::Wrap);
  errorText_.Foreground(colors::DangerBrush());
  errorText_.Visibility(Visibility::Collapsed);
  content.Children().Append(errorText_);

  dialog_.Content(content);
  dialog_.PrimaryButtonClick(
      [weak = weak_from_this()](auto const&, ContentDialogButtonClickEventArgs const& args) {
        args.Cancel(true);
        if (auto self = weak.lock()) self->Submit();
      });
}

void DeleteAccountSheet::Submit() {
  if (deleting_ || !sdk_.IsLoggedIn()) return;
  const std::string typed = Trim(Narrow(confirmBox_.Text().c_str()));
  if (networkName_.empty() || typed != networkName_) return;  // belt and braces
  deleting_ = true;
  dialog_.IsPrimaryButtonEnabled(false);
  errorText_.Visibility(Visibility::Collapsed);

  auto queue = dialog_.DispatcherQueue();
  auto weak = weak_from_this();
  auto* sdk = &sdk_;
  sdk_.api().networkDelete([queue, weak, sdk](std::optional<urnet::NetworkDeleteResult> result,
                                              std::optional<std::string> err) {
    // A refused deletion is a result with an error (HTTP 200): the account
    // still exists, so only a result with no error signs out.
    const bool serverError = result && result->error.has_value();
    const auto outcome = account::DecideDeleteAccount(
        err ? &*err : nullptr, result.has_value(), serverError,
        serverError ? result->error->message : std::string());
    const bool ok = outcome.deleted;
    const std::string detail = outcome.detail;
    queue.TryEnqueue([weak, sdk, ok, detail] {
      auto self = weak.lock();
      if (!self) return;
      self->deleting_ = false;
      if (ok) {
        // The network is gone; the session is meaningless. Signing out is what
        // iOS does too, and it is the only coherent next state.
        self->dialog_.Hide();
        sdk->Logout();
        return;
      }
      // Still signed in: the sheet stays open with the primary enabled for a retry.
      self->dialog_.IsPrimaryButtonEnabled(true);
      self->errorText_.Text(H(account::DeleteAccountErrorText(
          winrt::to_string(Loc("error_deleting_account")), detail)));
      self->errorText_.Visibility(Visibility::Visible);
    });
  });
}

}  // namespace urnw
