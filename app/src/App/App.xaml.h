// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "App.xaml.g.h"

namespace winrt::URnetwork::implementation {

struct App : AppT<App> {
  App();
  void OnLaunched(winrt::Microsoft::UI::Xaml::LaunchActivatedEventArgs const&);
};

}  // namespace winrt::URnetwork::implementation
