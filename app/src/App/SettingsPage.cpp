// SPDX-License-Identifier: MPL-2.0
#include "pch.h"

#include "SettingsPage.h"

#include "MainWindow.xaml.h"
#include "PageContext.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace urnw::pages;

namespace urnw {

// winrt::implements makes IInspectable a member typedef of every C++/WinRT
// implementation type, which is why MainWindow could name it unqualified. A
// plain class outside that hierarchy has to bring it in.
using winrt::Windows::Foundation::IInspectable;

SettingsPage::SettingsPage(winrt::URnetwork::implementation::MainWindow& window)
    : w_(window), snackbar_(window.SupportInfo(), window.DispatcherQueue()) {}

void SettingsPage::ApplyStrings() {
  // support
  w_.FeedbackHeading().Text(Loc("feedback"));
  w_.FeedbackRating().Caption(Loc("how_are_we_doing"));
  w_.FeedbackText().Header(LocBox("anything_else"));
  w_.SendFeedbackButton().Content(LocBox("send"));

  // settings
  w_.SplitTunnelHeading().Text(Loc("app_split_rules"));
  w_.SplitTunnelDescription().Text(Loc("apps_listed_bypass_vpn"));
  w_.ManageAppSplitButton().Content(LocBox("manage_apps"));
  w_.SettingsAccountHeading().Text(Loc("account"));
  w_.SignOutButton().Content(LocBox("sign_out"));
  w_.ProtocolLink().Content(LocBox("uses_ur_protocol"));
}

// ---- split tunnel --------------------------------------------------------

void SettingsPage::OnManageAppSplitTunnel(IInspectable const&, RoutedEventArgs const&) {
  ShowAppRulesSheet();
}

winrt::fire_and_forget SettingsPage::ShowAppRulesSheet() {
  if (w_.sheetOpen()) co_return;  // only one ContentDialog can show at a time
  auto self = w_.get_strong();
  w_.SetSheetOpen(true);
  try {
    appRulesSheet_ = urnw::AppRulesSheet::Create(self->Content().XamlRoot(), Sdk());
    co_await appRulesSheet_->Dialog().ShowAsync();
  } catch (...) {
  }
  appRulesSheet_.reset();
  w_.SetSheetOpen(false);
}

// ---- account -------------------------------------------------------------

void SettingsPage::OnSignOut(IInspectable const&, RoutedEventArgs const&) {
  Sdk().Logout();
}

// ---- support -------------------------------------------------------------

void SettingsPage::OnSendFeedback(IInspectable const&, RoutedEventArgs const&) {
  urnet::FeedbackSendArgs args;
  args.star_count = static_cast<int64_t>(w_.FeedbackRating().Value());
  const std::string text = urnw::Narrow(w_.FeedbackText().Text().c_str());
  if (!text.empty()) {
    urnet::FeedbackSendNeeds needs;
    needs.other = text;
    args.needs = needs;
  }
  Sdk().api().sendFeedback(
      args, [this](std::optional<urnet::FeedbackSendResult>, std::optional<std::string>) {
        w_.DispatcherQueue().TryEnqueue([this] {
          snackbar_.Show(Loc("thanks_for_the_feedback"), InfoBarSeverity::Success);
          w_.FeedbackText().Text(L"");
        });
      });
}

}  // namespace urnw
