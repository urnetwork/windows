// SPDX-License-Identifier: MPL-2.0
#include "pch.h"

#include "SettingsSheets.h"

#include <algorithm>
#include <cctype>

#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Windows.Storage.Streams.h>

#include "Localization.h"
#include "Log.h"
#include "PageContext.h"
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

Style Lookup(std::wstring_view key) {
  auto app = Application::Current();
  if (!app) return nullptr;
  auto res = app.Resources();
  auto boxed = winrt::box_value(hstring{key});
  if (!res.HasKey(boxed)) return nullptr;
  return res.Lookup(boxed).try_as<Style>();
}

StackPanel Card(Panel const& host, double spacing) {
  Border card;
  card.Style(Lookup(L"UrCardStyle"));
  StackPanel inner;
  inner.Spacing(spacing);
  card.Child(inner);
  host.Children().Append(card);
  return inner;
}

void Heading(Panel const& host, hstring const& text) {
  TextBlock block;
  block.Text(text);
  block.FontSize(18);
  block.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
  auto app = Application::Current();
  if (app) {
    auto boxed = winrt::box_value(hstring{L"UrHeadingFontFamily"});
    if (app.Resources().HasKey(boxed)) {
      if (auto family = app.Resources().Lookup(boxed).try_as<Media::FontFamily>()) {
        block.FontFamily(family);
      }
    }
  }
  host.Children().Append(block);
}

TextBlock Supporting(Panel const& host, hstring const& text) {
  TextBlock block;
  block.Text(text);
  block.FontSize(12);
  block.TextWrapping(TextWrapping::Wrap);
  block.Foreground(colors::MutedBrush());
  host.Children().Append(block);
  return block;
}

Grid Row(Panel const& host, hstring const& label, hstring const& note,
         FrameworkElement const& trailing) {
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
    // Accessibility: the trailing control's own content is the VERB ("Copy",
    // "Save"), which is not a name - the UIA tree showed two bare "Copy"
    // buttons with nothing to tell them apart. Point them at the row's label so
    // they announce as "Copy, Client ID" / "Copy, Bonus referral code". Uses
    // the label already on screen, so it needs no new string and cannot drift.
    Automation::AutomationProperties::SetLabeledBy(trailing, labelBlock);
    if (auto panel = trailing.try_as<Panel>()) {
      // ValueActionRow's trailing is a value + button stack; label the button.
      for (auto const& child : panel.Children()) {
        if (auto control = child.try_as<Control>()) {
          Automation::AutomationProperties::SetLabeledBy(control, labelBlock);
        }
      }
    }
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
  Button button;
  button.Style(Lookup(L"UrCardRowButtonStyle"));
  button.HorizontalAlignment(HorizontalAlignment::Stretch);
  button.HorizontalContentAlignment(HorizontalAlignment::Stretch);

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
    case FieldState::Failed:
      value.Text(Loc("something_went_wrong"));
      value.Foreground(colors::DangerBrush());
      return;
  }
}

void Divider(Panel const& host) {
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

// ---- AddAuthSheet ----------------------------------------------------------

std::shared_ptr<AddAuthSheet> AddAuthSheet::Create(XamlRoot const& root, SdkHost& sdk,
                                                   std::function<void()> onChanged) {
  auto sheet = std::shared_ptr<AddAuthSheet>(new AddAuthSheet(sdk, std::move(onChanged)));
  sheet->Build(root);
  return sheet;
}

void AddAuthSheet::Build(XamlRoot const& root) {
  dialog_ = MakeSheet(root, Loc("site_app_login_methods"));
  dialog_.PrimaryButtonText(Loc("add"));
  dialog_.CloseButtonText(Loc("cancel"));
  dialog_.IsPrimaryButtonEnabled(false);
  dialog_.DefaultButton(ContentDialogButton::Primary);

  StackPanel content;
  content.MinWidth(380);
  content.Spacing(12);

  authBox_ = TextBox();
  authBox_.Style(Lookup(L"UrTextInputStyle"));
  authBox_.Header(winrt::box_value(Loc("your_email")));
  authBox_.TextChanged([weak = weak_from_this()](auto const&, auto const&) {
    if (auto self = weak.lock()) self->Validate();
  });
  content.Children().Append(authBox_);

  passwordBox_ = PasswordBox();
  passwordBox_.Style(Lookup(L"UrPasswordInputStyle"));
  passwordBox_.Header(winrt::box_value(Loc("password_label")));
  passwordBox_.PasswordChanged([weak = weak_from_this()](auto const&, auto const&) {
    if (auto self = weak.lock()) self->Validate();
  });
  content.Children().Append(passwordBox_);

  Supporting(content, Loc("password_must_be_at_least_12_characters_long"));

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

  if (!sdk_.IsLoggedIn()) {
    // Adding a sign-in method to no account is not a thing; say so rather than
    // offering a form whose submit would 401 in silence.
    authBox_.IsEnabled(false);
    passwordBox_.IsEnabled(false);
    ApplyFieldState(errorText_, FieldState::NoSession);
    errorText_.Visibility(Visibility::Visible);
  }
}

void AddAuthSheet::Validate() {
  const std::string auth = Trim(Narrow(authBox_.Text().c_str()));
  const std::string password = Narrow(passwordBox_.Password().c_str());
  // apple AddAuthSheet formValid for the email leg: an auth AND a 12-char
  // password. The server is the real validator; this only gates the button.
  const bool valid = !auth.empty() && 12 <= password.size();
  dialog_.IsPrimaryButtonEnabled(valid && !submitting_ && sdk_.IsLoggedIn());
  if (sdk_.IsLoggedIn()) errorText_.Visibility(Visibility::Collapsed);
}

void AddAuthSheet::Submit() {
  if (submitting_ || !sdk_.IsLoggedIn()) return;
  const std::string auth = Trim(Narrow(authBox_.Text().c_str()));
  const std::string password = Narrow(passwordBox_.Password().c_str());
  if (auth.empty() || password.size() < 12) return;

  submitting_ = true;
  dialog_.IsPrimaryButtonEnabled(false);
  errorText_.Visibility(Visibility::Collapsed);

  urnet::AddAuthArgs args;
  args.user_auth = auth;
  args.password = password;

  auto queue = dialog_.DispatcherQueue();
  auto weak = weak_from_this();
  sdk_.api().addAuth(args, [queue, weak](std::optional<urnet::AddAuthResult> result,
                                         std::optional<std::string> err) {
    const std::string error = ServerError(result, err);
    queue.TryEnqueue([weak, error] {
      if (auto self = weak.lock()) self->ApplyResult(error.empty(), error);
    });
  });
}

void AddAuthSheet::ApplyResult(bool ok, std::string const& error) {
  submitting_ = false;
  if (ok) {
    if (onChanged_) onChanged_();
    dialog_.Hide();
    return;
  }
  dialog_.IsPrimaryButtonEnabled(true);
  errorText_.Text(error.empty() ? Loc("something_went_wrong") : H(error));
  errorText_.Visibility(Visibility::Visible);
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
  content.MinWidth(400);
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
    self->dialog_.IsPrimaryButtonEnabled(6 <= code.size() && !self->busy_);
    self->errorText_.Visibility(Visibility::Collapsed);
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
    if (auto self = weak.lock()) self->Unlink();
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
        if (auto self = weak.lock()) self->Submit();
      });
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
    std::string error;
    if (result && result->error) error = result->error->message;
    else if (err) error = *err;
    const bool failed = !error.empty() || !result;
    if (failed) LogWarn("settings: getReferralNetwork (sheet) failed: {}", error);
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
  unlinkArmed_ = false;
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
            self->Load();  // re-read the network the code resolved to
            if (self->onChanged_) self->onChanged_();
            return;
          }
          // A rejected code is the common failure and has its own string; a
          // transport failure falls back to the generic one.
          self->ShowError(Loc("invalid_referral_code_please_try_again"));
        });
      });
}

void ReferralNetworkSheet::Unlink() {
  if (busy_ || currentName_.empty()) return;
  // Two clicks, because unlinking silently forfeits future points. The first
  // turns the button into the warning that names what is being given up; the
  // second commits. iOS uses an alert for this; a ContentDialog cannot open a
  // second ContentDialog, so the confirmation is in the row.
  if (!unlinkArmed_) {
    unlinkArmed_ = true;
    unlinkButton_.Content(winrt::box_value(Loc("unlink_referral_network")));
    ShowError(hstring{urnw::Format("when_unlinking_your_referral_network_you_will_no",
                                   urnw::Widen(currentName_))});
    errorText_.Foreground(colors::MutedBrush());
    return;
  }
  if (!sdk_.IsLoggedIn()) return;
  busy_ = true;
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
          self->unlinkButton_.IsEnabled(true);
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
  content.MinWidth(420);
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
  scroll.MaxHeight(240);
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
        // GetNetworkBlockedLocationsResult has NO error field, so a missing
        // result or a transport error is the only failure signal there is - and
        // without this check a 401 arrived as an empty list and rendered as the
        // reassuring "No blocked locations".
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
  content.MinWidth(420);
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
  scroll.MaxHeight(200);
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

void PostQuantumIdentitySheet::Load() {
  // DeviceRemote, so this needs a live service session. With none, say so:
  // an empty provider list would otherwise read as "no peer has an identity",
  // which is a claim about the network rather than about this app.
  if (!sdk_.hasDevice()) {
    // Say it where the hash would be, not only under the provider list, and
    // leave Copy disabled - there is nothing to put on the clipboard.
    ApplyFieldState(hashText_, FieldState::NoSession);
    ApplyFieldState(statusText_, FieldState::NoSession);
    return;
  }
  std::string hash;
  std::vector<uint8_t> key;
  std::optional<urnet::ProviderIdentityList> providers;
  try {
    auto& device = sdk_.device();
    hash = device.getPublicIdentityKeyHash();
    key = device.getPublicIdentityKey();
    providers = device.getProviderIdentities();
  } catch (const std::exception& e) {
    LogWarn("settings: post quantum identity read failed: {}", e.what());
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

std::shared_ptr<DeleteAccountSheet> DeleteAccountSheet::Create(XamlRoot const& root, SdkHost& sdk,
                                                               std::string const& networkName) {
  auto sheet = std::shared_ptr<DeleteAccountSheet>(new DeleteAccountSheet(sdk, networkName));
  sheet->Build(root);
  return sheet;
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
  content.MinWidth(400);
  content.Spacing(12);

  TextBlock warning;
  warning.Text(Loc("site_app_delete_warning"));
  warning.FontSize(13);
  warning.TextWrapping(TextWrapping::Wrap);
  warning.Foreground(colors::DangerBrush());
  content.Children().Append(warning);

  // The typed-name gate. Api::networkDelete takes no arguments and cannot be
  // undone, so this is the only thing between a mis-click and a destroyed
  // network. iOS ships a one-tap destructive confirmation here; this is
  // deliberately stricter, and it spends a string the store already carries.
  confirmBox_ = TextBox();
  confirmBox_.Style(Lookup(L"UrTextInputStyle"));
  confirmBox_.Header(winrt::box_value(Loc("site_app_delete_confirm")));
  confirmBox_.PlaceholderText(H(networkName_));
  confirmBox_.TextChanged([weak = weak_from_this()](auto const&, auto const&) {
    auto self = weak.lock();
    if (!self) return;
    const std::string typed = Trim(Narrow(self->confirmBox_.Text().c_str()));
    self->dialog_.IsPrimaryButtonEnabled(!self->deleting_ && !self->networkName_.empty() &&
                                         typed == self->networkName_);
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
    // NetworkDeleteResult carries no error field, so a result plus no transport
    // error is the whole success test. (iOS does not check even that far.)
    const bool ok = !err && result.has_value();
    const std::string error = err ? *err : std::string();
    queue.TryEnqueue([weak, sdk, ok, error] {
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
      self->dialog_.IsPrimaryButtonEnabled(true);
      self->errorText_.Text(error.empty() ? Loc("error_deleting_account") : H(error));
      self->errorText_.Visibility(Visibility::Visible);
    });
  });
}

}  // namespace urnw
