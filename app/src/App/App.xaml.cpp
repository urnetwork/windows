// SPDX-License-Identifier: MPL-2.0
#include "pch.h"

#include "App.xaml.h"

#include "AppController.h"
#include "Log.h"
#include "Paths.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;

// cppwinrt's generated module.g.cpp (the activation-factory dispatcher) links against the
// global ::winrt_make_URnetwork_App(). Unlike MainWindow (Microsoft.UI.Xaml.Window), cppwinrt
// emits NO factory_implementation::App / composition ctor for App (a Microsoft.UI.Xaml.
// Application subclass) - App.g.cpp references them but App.g.h / URnetwork.2.h never declare
// them, so App.g.cpp cannot be compiled. That is correct: App is a singleton created via
// Application::Start -> winrt::make<implementation::App> (main.cpp) and is NEVER activated by
// name (the winrt::URnetwork::App projection is never constructed), so this maker is never
// invoked. Returning null is the honest answer - App has no activation factory - and lets
// URnetwork.exe link. Global namespace to match module.g.cpp's extern declaration.
void* winrt_make_URnetwork_App() { return nullptr; }

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

  // urnetwork:// protocol activation — the ur.io/wallet-connect bridge returning
  // through the browser (see main.cpp). A launch while the app is already running
  // is redirected to this instance by AppInstance and lands on Activated, which
  // fires on a background thread: marshal to the UI thread before touching the app.
  namespace lifecycle = winrt::Microsoft::Windows::AppLifecycle;
  auto instance = lifecycle::AppInstance::GetCurrent();
  auto queue = winrt::Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();
  instance.Activated([queue](winrt::Windows::Foundation::IInspectable const&,
                             lifecycle::AppActivationArguments const& args) {
    const std::string url = urnw::DeepLinkFromActivation(args);
    if (url.empty()) return;  // a plain relaunch: nothing to route
    queue.TryEnqueue([url] { urnw::App().HandleDeepLink(url); });
  });

  // ...and the activation this instance was cold-launched with, if any (the app
  // wasn't running when the wallet returned). We are on the UI thread here.
  std::string url = urnw::DeepLinkFromActivation(instance.GetActivatedEventArgs());
  if (url.empty()) url = urnw::LaunchDeepLink();  // fallback: our own command line
  if (!url.empty()) urnw::App().HandleDeepLink(url);
}

}  // namespace winrt::URnetwork::implementation
