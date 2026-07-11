// URnetwork tray app entry point. Single-instance, registers the AppUserModelId
// (for toasts + tray grouping), then hands off to the WinUI 3 Application, which
// creates the tray + SDK host in OnLaunched. The window is opened from the tray.
//
// SPDX-License-Identifier: MPL-2.0
#include "pch.h"

#include <shobjidl_core.h>

#include "App.xaml.h"
#include "Ids.h"

int __stdcall wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  // Single instance: a second launch just exits (a full impl would signal the
  // first instance to open its window; tracked as a follow-up).
  HANDLE mutex = ::CreateMutexW(nullptr, TRUE, L"URnetwork.Desktop.SingleInstance");
  if (mutex && ::GetLastError() == ERROR_ALREADY_EXISTS) {
    return 0;
  }

  ::SetCurrentProcessExplicitAppUserModelID(urnw::ids::kAppUserModelId);

  winrt::init_apartment(winrt::apartment_type::single_threaded);
  winrt::Microsoft::UI::Xaml::Application::Start([](auto&&) {
    winrt::make<winrt::URnetwork::implementation::App>();
  });
  return 0;
}
