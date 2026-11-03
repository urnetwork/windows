// SPDX-License-Identifier: MPL-2.0
// the project compiles with /Yu"pch.h" (App.vcxproj), so every translation unit
// must include it first
#include "pch.h"

#include "BittensorWalletDialogs.h"

#include <memory>

#include <winrt/Microsoft.UI.Xaml.Automation.h>

#include "BittensorWalletFlow.h"
#include "Localization.h"
#include "Log.h"
#include "MainWindow.xaml.h"
#include "PageContext.h"
#include "SnConsentPresentation.h"
#include "Strings.h"
#include "UrColors.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;

namespace urnw::pages {

winrt::fire_and_forget ChooseBittensorWallet(
    winrt::com_ptr<winrt::URnetwork::implementation::MainWindow> window,
    std::function<void(std::string walletId)> next) {
  if (!window || window->sheetOpen()) co_return;  // one ContentDialog at a time

  // the supported wallets, by their product names, each with the line that
  // says what it is (which WalletConnect wallets)
  ContentDialog dialog;
  dialog.XamlRoot(window->Content().XamlRoot());
  dialog.Title(winrt::box_value(Loc("bittensor_choose_wallet")));
  dialog.CloseButtonText(Loc("cancel"));
  dialog.Background(colors::SheetBrush());

  auto choice = std::make_shared<int>(-1);
  StackPanel list;
  list.MinWidth(320);
  list.Spacing(8);
  for (int i = 0; i < bittensor::kChooserWalletCount; ++i) {
    const std::string walletId(bittensor::kChooserWallets[i]);
    StackPanel label;
    TextBlock name;
    name.Text(H(urnet::bittensorWalletDisplayName(walletId)));
    name.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
    label.Children().Append(name);
    const std::string hintKey = bittensor::ChooserHintKey(walletId);
    if (!hintKey.empty()) {
      TextBlock hint;
      hint.Text(Loc(hintKey));
      hint.Opacity(0.7);
      hint.TextWrapping(TextWrapping::Wrap);
      label.Children().Append(hint);
    }
    Button button;
    button.HorizontalAlignment(HorizontalAlignment::Stretch);
    button.HorizontalContentAlignment(HorizontalAlignment::Left);
    button.Content(label);
    button.Click([dialog, choice, i](auto const&, auto const&) {
      *choice = i;
      dialog.Hide();
    });
    list.Children().Append(button);
  }
  dialog.Content(list);

  window->SetSheetOpen(true);
  try {
    co_await dialog.ShowAsync();
  } catch (std::exception const& e) {
    urnw::LogError("bittensor wallet chooser: {}", e.what());
  } catch (...) {
    urnw::LogError("bittensor wallet chooser failed to open");
  }
  window->SetSheetOpen(false);

  if (next) next(bittensor::WalletForChoice(*choice));
}

winrt::fire_and_forget EnterConsentColdkey(
    winrt::com_ptr<winrt::URnetwork::implementation::MainWindow> window,
    std::function<void(std::string coldkeySs58)> next) {
  if (!window || window->sheetOpen()) co_return;  // one ContentDialog at a time

  StackPanel content;
  content.MinWidth(380);
  content.Spacing(8);

  TextBlock body;
  body.TextWrapping(TextWrapping::Wrap);
  body.Text(Loc("sn_consent_body"));
  content.Children().Append(body);

  // empty on purpose: the coldkey earnings are paid to is the user's to choose
  TextBox addressBox;
  addressBox.PlaceholderText(Loc("earnings_address_placeholder"));
  winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(
      addressBox, Loc("earnings_address_placeholder"));
  content.Children().Append(addressBox);

  TextBlock errorText;
  errorText.FontSize(12);
  errorText.TextWrapping(TextWrapping::Wrap);
  errorText.Foreground(colors::DangerBrush());
  errorText.Visibility(Visibility::Collapsed);
  content.Children().Append(errorText);

  ContentDialog dialog;
  dialog.XamlRoot(window->Content().XamlRoot());
  dialog.Title(winrt::box_value(Loc("sn_consent_title")));
  dialog.Content(content);
  dialog.PrimaryButtonText(Loc("continue_txt"));
  dialog.CloseButtonText(Loc("cancel"));
  dialog.DefaultButton(ContentDialogButton::Primary);
  dialog.Background(colors::SheetBrush());

  // the SDK's own ss58 rule, before any request is made for the address
  auto coldkey = std::make_shared<std::string>();
  dialog.PrimaryButtonClick([addressBox, errorText, coldkey](
                                auto const&, ContentDialogButtonClickEventArgs const& args) {
    const auto valid = snconsent::ColdkeyFrom(
        Narrow(addressBox.Text().c_str()),
        [](std::string const& address) { return urnet::validateSs58(address); });
    if (!valid) {
      args.Cancel(true);  // keep the form, say what is wrong
      errorText.Text(Loc("invalid_ss58_address"));
      errorText.Visibility(Visibility::Visible);
      return;
    }
    *coldkey = *valid;
  });

  window->SetSheetOpen(true);
  try {
    co_await dialog.ShowAsync();
  } catch (...) {
    urnw::LogError("payout consent coldkey form failed to open");
  }
  window->SetSheetOpen(false);
  if (next) next(*coldkey);  // "" when cancelled
}

}  // namespace urnw::pages
