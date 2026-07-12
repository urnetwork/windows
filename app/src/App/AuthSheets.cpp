// SPDX-License-Identifier: MPL-2.0
#include "pch.h"

#include "AuthSheets.h"

#include "BalanceSheets.h"  // SetTermsMarkerText (the terms/privacy link inlines)
#include "Localization.h"
#include "UrColors.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;

// NOTE on captures: control event handlers capture the owning sheet weakly
// (see StatsSheets.cpp). The window holds the sheet's shared_ptr while the
// dialog is showing, so lock() always succeeds during interaction.

namespace urnw {
namespace {

hstring H(std::string const& s) { return winrt::to_hstring(s); }

// A UI string from the shared localization store, by key id (Localization.h).
hstring Loc(std::string_view key) { return hstring{Localized(key)}; }

}  // namespace

// ---- GuestModeSheet ---------------------------------------------------------

std::shared_ptr<GuestModeSheet> GuestModeSheet::Create(XamlRoot const& root,
                                                       SdkHost& sdk) {
  auto sheet = std::shared_ptr<GuestModeSheet>(new GuestModeSheet(sdk));
  sheet->Build(root);
  return sheet;
}

void GuestModeSheet::Build(XamlRoot const& root) {
  dialog_ = ContentDialog();
  dialog_.XamlRoot(root);
  dialog_.Title(winrt::box_value(Loc("try_guest_mode_2")));
  dialog_.CloseButtonText(Loc("close"));
  // brand sheet surface (macOS sheet background; BalanceSheets::MakeDialog)
  dialog_.Background(colors::BackgroundBrush());
  dialog_.PrimaryButtonText(Loc("enter_urnetwork"));
  dialog_.IsPrimaryButtonEnabled(false);  // gated on the terms consent
  dialog_.DefaultButton(ContentDialogButton::Primary);

  StackPanel content;
  content.MinWidth(400);
  content.Spacing(12);

  // what guest mode is, and that a full account can come later
  TextBlock explainer;
  explainer.Text(Loc("guest_mode_explainer"));
  explainer.FontSize(13);
  explainer.Foreground(colors::MutedBrush());
  explainer.TextWrapping(TextWrapping::Wrap);
  content.Children().Append(explainer);

  // terms consent: checkbox + the tappable terms/privacy links (the same
  // terms_checkbox string the create step renders)
  Grid termsRow;
  ColumnDefinition c0, c1;
  c0.Width(GridLength{0, GridUnitType::Auto});
  c1.Width(GridLength{1, GridUnitType::Star});
  termsRow.ColumnDefinitions().Append(c0);
  termsRow.ColumnDefinitions().Append(c1);
  termsRow.ColumnSpacing(8);

  termsCheck_ = CheckBox();
  termsCheck_.MinWidth(0);
  termsCheck_.VerticalAlignment(VerticalAlignment::Top);
  auto onTermsChanged = [weak = weak_from_this()](auto const&, auto const&) {
    if (auto self = weak.lock()) {
      const bool agreed =
          self->termsCheck_.IsChecked() && self->termsCheck_.IsChecked().Value();
      self->dialog_.IsPrimaryButtonEnabled(agreed && !self->creating_);
      self->errorText_.Visibility(Visibility::Collapsed);
    }
  };
  termsCheck_.Checked(onTermsChanged);
  termsCheck_.Unchecked(onTermsChanged);
  termsRow.Children().Append(termsCheck_);

  TextBlock termsText;
  termsText.VerticalAlignment(VerticalAlignment::Center);
  termsText.Foreground(colors::MutedBrush());
  SetTermsMarkerText(termsText, Localized("terms_checkbox"), 12);
  Grid::SetColumn(termsText, 1);
  termsRow.Children().Append(termsText);
  content.Children().Append(termsRow);

  errorText_ = TextBlock();
  errorText_.FontSize(12);
  errorText_.Foreground(colors::DangerBrush());
  errorText_.TextWrapping(TextWrapping::Wrap);
  errorText_.Visibility(Visibility::Collapsed);
  content.Children().Append(errorText_);

  dialog_.Content(content);

  dialog_.PrimaryButtonClick([weak = weak_from_this()](
                                 auto const&, ContentDialogButtonClickEventArgs const& args) {
    args.Cancel(true);  // keep the dialog open; ApplyResult decides what shows next
    if (auto self = weak.lock()) self->Submit();
  });
}

void GuestModeSheet::Submit() {
  const bool agreed = termsCheck_.IsChecked() && termsCheck_.IsChecked().Value();
  if (creating_ || !agreed || !sdk_.apiReady()) return;
  creating_ = true;
  dialog_.IsPrimaryButtonEnabled(false);
  termsCheck_.IsEnabled(false);
  errorText_.Visibility(Visibility::Collapsed);

  auto queue = dialog_.DispatcherQueue();
  auto weak = weak_from_this();
  sdk_.LoginAsGuest([queue, weak](AuthResult r) {
    queue.TryEnqueue([weak, r] {
      if (auto self = weak.lock()) self->ApplyResult(r.ok, r.error);
    });
  });
}

void GuestModeSheet::ApplyResult(bool ok, std::string const& error) {
  creating_ = false;
  if (ok) {
    // the auth-state relay swaps the login panel for the home view underneath
    dialog_.Hide();
    return;
  }
  termsCheck_.IsEnabled(true);
  dialog_.IsPrimaryButtonEnabled(termsCheck_.IsChecked() &&
                                 termsCheck_.IsChecked().Value());
  // a server error is not localizable; show it when there is one
  errorText_.Text(error.empty() ? Loc("guest_mode_failed") : H(error));
  errorText_.Visibility(Visibility::Visible);
}

}  // namespace urnw
