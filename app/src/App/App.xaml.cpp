// SPDX-License-Identifier: MPL-2.0
#include "pch.h"

#include "App.xaml.h"

#include "AppController.h"
#include "Log.h"
#include "Paths.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;

namespace winrt::URnetwork::implementation {

App::App() {
  // XAML diagnostics: surface unhandled exceptions in debug.
#if defined(_DEBUG)
  UnhandledException([](IInspectable const&, UnhandledExceptionEventArgs const& e) {
    if (IsDebuggerPresent()) {
      auto message = e.Message();
      __debugbreak();
    }
  });
#endif
}

// A tray app: OnLaunched brings up the controller (tray + SDK) but does not open
// a window — the user opens it from the tray.
void App::OnLaunched(LaunchActivatedEventArgs const&) {
  urnw::LogInit(urnw::LogDir(/*isService=*/false) / L"urnetwork-app.log", "app");
  urnw::SdkInit(/*isService=*/false, 64ll * 1024 * 1024);

  auto controller = std::make_unique<urnw::AppController>();
  controller->Start();
  urnw::SetApp(std::move(controller));
}

}  // namespace winrt::URnetwork::implementation
