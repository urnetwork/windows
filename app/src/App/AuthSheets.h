// Sign-in sheets, as ContentDialogs (macOS Authenticate/LoginInitial parity).
// Plain C++ helpers like BalanceSheets/StatsSheets: all methods run on the UI
// thread, and the window holds the sheet's shared_ptr while it is showing.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <memory>
#include <string>

#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>

#include "SdkHost.h"

namespace urnw {

// ---- Try guest mode -----------------------------------------------------------
// macOS GuestModeSheet: a brief explainer, the terms consent (the same
// terms/privacy links the create step uses), and one button that creates a
// throwaway guest network (SdkHost::LoginAsGuest). On success the dialog hides
// itself and the auth-state relay swaps the login panel for the home view;
// errors show inline and leave the sheet open for a retry.
class GuestModeSheet : public std::enable_shared_from_this<GuestModeSheet> {
 public:
  static std::shared_ptr<GuestModeSheet> Create(
      winrt::Microsoft::UI::Xaml::XamlRoot const& root, SdkHost& sdk);

  winrt::Microsoft::UI::Xaml::Controls::ContentDialog Dialog() const { return dialog_; }

 private:
  explicit GuestModeSheet(SdkHost& sdk) : sdk_(sdk) {}

  void Build(winrt::Microsoft::UI::Xaml::XamlRoot const& root);
  void Submit();
  void ApplyResult(bool ok, std::string const& error);

  SdkHost& sdk_;
  winrt::Microsoft::UI::Xaml::Controls::ContentDialog dialog_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::CheckBox termsCheck_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock errorText_{nullptr};
  bool creating_ = false;
};

}  // namespace urnw
