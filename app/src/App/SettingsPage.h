// The Settings destination (app split rules, sign out, the protocol link) and
// the Support destination (the feedback form), which iOS carries inside the
// same settings surface.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <memory>

#include <winrt/Microsoft.UI.Xaml.h>

#include "StatsSheets.h"
#include "UrComponents.h"

namespace winrt::URnetwork::implementation {
struct MainWindow;
}

namespace urnw {

class SettingsPage {
 public:
  explicit SettingsPage(winrt::URnetwork::implementation::MainWindow& window);

  void ApplyStrings();

  void OnManageAppSplitTunnel(winrt::Windows::Foundation::IInspectable const&,
                              winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnSignOut(winrt::Windows::Foundation::IInspectable const&,
                 winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnSendFeedback(winrt::Windows::Foundation::IInspectable const&,
                      winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);

  // --preview-ui only (Startup.h): raise the feedback acknowledgement, the
  // severity that SHOULD time out, so both snackbar behaviours are visible.
  void ShowPreviewSnackbar();

 private:
  winrt::fire_and_forget ShowAppRulesSheet();

  winrt::URnetwork::implementation::MainWindow& w_;
  // "Thanks for the feedback": a transient acknowledgement (iOS UrSnackBar)
  urnw::kit::Snackbar snackbar_;
  std::shared_ptr<urnw::AppRulesSheet> appRulesSheet_;
};

}  // namespace urnw
