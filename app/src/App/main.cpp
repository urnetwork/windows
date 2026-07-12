// URnetwork tray app entry point. Single-instance, registers the AppUserModelId
// (for toasts + tray grouping), then hands off to the WinUI 3 Application, which
// creates the tray + SDK host in OnLaunched. The window is opened from the tray.
//
// Single-instancing is the Windows App SDK's (AppInstance), not a bare mutex,
// because a second launch is not always a no-op: the MSI registers the
// urnetwork:// scheme (installer/Package.wxs), so the browser returning from the
// ur.io/wallet-connect bridge launches the app with the wallet callback uri. That
// launch is redirected to the running instance, which receives it on
// AppInstance::Activated (see App::OnLaunched) — the common case, since the user
// started the sign-in from the running app.
//
// SPDX-License-Identifier: MPL-2.0
#include "pch.h"

#include <objbase.h>  // CoWaitForMultipleObjects
#include <shobjidl_core.h>

#include <thread>

#include "App.xaml.h"
#include "Ids.h"

using namespace winrt::Microsoft::Windows::AppLifecycle;

namespace {

// Key for the single instance. Stable, like the other app identities (Ids.h).
constexpr wchar_t kInstanceKey[] = L"URnetwork.Desktop";

// Hand this launch's activation to the instance that owns the key, then exit.
// RedirectActivationToAsync must not be waited on directly from this STA thread
// (it would deadlock), so it runs on a worker while this thread keeps pumping —
// the pattern from the Windows App SDK instancing sample.
void RedirectActivation(AppInstance const& primary, AppActivationArguments const& args) {
  winrt::handle redirected{::CreateEventW(nullptr, TRUE, FALSE, nullptr)};
  if (!redirected) return;
  std::thread worker([&] {
    primary.RedirectActivationToAsync(args).get();
    ::SetEvent(redirected.get());
  });
  HANDLE handles[] = {redirected.get()};
  DWORD index = 0;
  ::CoWaitForMultipleObjects(CWMO_DEFAULT, INFINITE, 1, handles, &index);
  worker.join();
}

}  // namespace

int __stdcall wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  winrt::init_apartment(winrt::apartment_type::single_threaded);

  // The first launch owns the key; every later launch redirects its activation
  // (a urnetwork:// wallet callback, or a plain relaunch) to it and exits.
  AppActivationArguments args = AppInstance::GetCurrent().GetActivatedEventArgs();
  AppInstance primary = AppInstance::FindOrRegisterForKey(kInstanceKey);
  if (!primary.IsCurrent()) {
    RedirectActivation(primary, args);
    return 0;
  }

  ::SetCurrentProcessExplicitAppUserModelID(urnw::ids::kAppUserModelId);

  winrt::Microsoft::UI::Xaml::Application::Start([](auto&&) {
    winrt::make<winrt::URnetwork::implementation::App>();
  });
  return 0;
}
