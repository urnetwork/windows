// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "MainWindow.g.h"

#include <string>

#include "Protocol.h"
#include "SdkHost.h"

namespace winrt::URnetwork::implementation {

struct MainWindow : MainWindowT<MainWindow> {
  MainWindow();

  // XAML event handlers
  void OnSignIn(winrt::Windows::Foundation::IInspectable const&,
                winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnUseCode(winrt::Windows::Foundation::IInspectable const&,
                 winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnConnectToggle(winrt::Windows::Foundation::IInspectable const&,
                       winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnNavSelectionChanged(
      winrt::Microsoft::UI::Xaml::Controls::NavigationView const&,
      winrt::Microsoft::UI::Xaml::Controls::NavigationViewSelectionChangedEventArgs const&);
  void OnAddExcludedApp(winrt::Windows::Foundation::IInspectable const&,
                        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnRemoveExcludedApp(winrt::Windows::Foundation::IInspectable const&,
                           winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnSignOut(winrt::Windows::Foundation::IInspectable const&,
                 winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnSaveNetworkName(winrt::Windows::Foundation::IInspectable const&,
                         winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnUpgrade(winrt::Windows::Foundation::IInspectable const&,
                 winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnRedeemCode(winrt::Windows::Foundation::IInspectable const&,
                    winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnSendFeedback(winrt::Windows::Foundation::IInspectable const&,
                      winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);

  // Called by AppController (already marshaled onto the UI thread).
  void OnAuthStateChanged(urnw::AuthState state, std::string const& error);
  void OnTunnelStateChanged(urnw::proto::TunnelStatus const& status);
  void OnStatsChanged(urnw::LiveStats const& stats);

 private:
  void ApplyAuthState(urnw::AuthState state, std::string const& error);
  void SetConnectedUi(bool connected);
  void ApplyStats(urnw::LiveStats const& stats);
  void PushExcludedApps();
  void LoadAccount();
  void LoadWallet();
  void LoadLeaderboard();

  bool connected_ = false;
  std::vector<std::string> excludedApps_;
};

}  // namespace winrt::URnetwork::implementation

namespace winrt::URnetwork::factory_implementation {
struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow> {};
}  // namespace winrt::URnetwork::factory_implementation
