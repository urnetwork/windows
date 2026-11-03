// SPDX-License-Identifier: MPL-2.0
// the project compiles with /Yu"pch.h" (App.vcxproj), so every translation unit
// must include it first
#include "pch.h"

#include "BittensorKeyPlatform.h"

#include <UserConsentVerifierInterop.h>
#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include <winrt/Windows.Security.Credentials.UI.h>

#include <chrono>
#include <cstdint>

#include "Log.h"
#include "Sdk.h"

// winuser.h names it from the Windows 10 2004 SDK on; the value is the API's
#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

namespace urnw::btkeyplatform {
namespace {

namespace credui = winrt::Windows::Security::Credentials::UI;
namespace dt = winrt::Windows::ApplicationModel::DataTransfer;

using VerificationOperation =
    winrt::Windows::Foundation::IAsyncOperation<credui::UserConsentVerificationResult>;

// The verification over the app's own window: a desktop app has no window for
// UserConsentVerifier::RequestVerificationAsync to show over, so it asks
// through IUserConsentVerifierInterop. A Windows without the interop asks the
// plain way.
VerificationOperation RequestVerification(HWND window, winrt::hstring const& reason) {
  try {
    auto interop =
        winrt::get_activation_factory<credui::UserConsentVerifier, ::IUserConsentVerifierInterop>();
    VerificationOperation operation{nullptr};
    winrt::check_hresult(interop->RequestVerificationForWindowAsync(
        window, reinterpret_cast<HSTRING>(winrt::get_abi(reason)),
        winrt::guid_of<VerificationOperation>(), winrt::put_abi(operation)));
    return operation;
  } catch (winrt::hresult_error const&) {
    return credui::UserConsentVerifier::RequestVerificationAsync(reason);
  }
}

// After the copy's life, the clipboard is cleared if it still holds what the
// copy put there: the clipboard's sequence number moves with every change, so
// anything copied since is the user's and stays. The words themselves are not
// kept for the comparison.
winrt::fire_and_forget ClearLater(winrt::Microsoft::UI::Dispatching::DispatcherQueue queue,
                                  DWORD sequence) {
  co_await winrt::resume_after(std::chrono::seconds(urnet::BittensorLocalKeyCopySeconds));
  queue.TryEnqueue([sequence] {
    if (::GetClipboardSequenceNumber() != sequence) return;
    try {
      dt::Clipboard::Clear();
    } catch (winrt::hresult_error const&) {
      // the clipboard is busy: the copy stays until something replaces it
      LogWarn("bittensor key: the clipboard could not be cleared");
    }
  });
}

}  // namespace

winrt::Windows::Foundation::IAsyncOperation<bool> Authenticate(HWND window, winrt::hstring reason) {
  bool verified = false;
  try {
    const credui::UserConsentVerifierAvailability availability =
        co_await credui::UserConsentVerifier::CheckAvailabilityAsync();
    switch (availability) {
      case credui::UserConsentVerifierAvailability::Available: {
        const credui::UserConsentVerificationResult result =
            co_await RequestVerification(window, reason);
        verified = result == credui::UserConsentVerificationResult::Verified;
        break;
      }
      case credui::UserConsentVerifierAvailability::DeviceNotPresent:
      case credui::UserConsentVerifierAvailability::NotConfiguredForUser:
      case credui::UserConsentVerifierAvailability::DisabledByPolicy:
        // nothing to verify with: the owner must still be able to save the phrase
        LogInfo("bittensor key: no Windows Hello to verify with; counted as verified");
        verified = true;
        break;
      default:
        // DeviceBusy, and anything a later Windows adds
        verified = false;
        break;
    }
  } catch (winrt::hresult_error const& e) {
    LogWarn("bittensor key: Windows Hello failed: 0x{:08X}",
            static_cast<uint32_t>(static_cast<int32_t>(e.code())));
    verified = false;
  } catch (...) {
    LogWarn("bittensor key: Windows Hello failed");
    verified = false;
  }
  co_return verified;
}

bool CopyPhrase(std::string const& phrase,
                winrt::Microsoft::UI::Dispatching::DispatcherQueue const& queue) {
  if (phrase.empty()) return false;
  try {
    dt::DataPackage package;
    package.SetText(winrt::to_hstring(phrase));
    // Out of the clipboard history (Win+V, which anything can read back) and off
    // the cloud clipboard, which would sync it to the Microsoft account and off
    // this machine: what the seedphrase sheet and password managers set. Plain
    // Clipboard::SetContent opts into both.
    dt::ClipboardContentOptions options;
    options.IsAllowedInHistory(false);
    options.IsRoamable(false);
    if (!dt::Clipboard::SetContentWithOptions(package, options)) {
      // no fallback to SetContent: that is the leak this exists to close
      LogError("bittensor key: the clipboard refused a history-excluded copy");
      return false;
    }
    ClearLater(queue, ::GetClipboardSequenceNumber());
    return true;
  } catch (winrt::hresult_error const&) {
    LogError("bittensor key: the clipboard refused the copy");
  }
  return false;
}

CaptureExclusion::CaptureExclusion(HWND window) : window_(window) {
  if (!window_) return;
  DWORD previous = WDA_NONE;
  if (::GetWindowDisplayAffinity(window_, &previous)) previous_ = previous;
  applied_ = ::SetWindowDisplayAffinity(window_, WDA_EXCLUDEFROMCAPTURE) != FALSE;
  if (!applied_) applied_ = ::SetWindowDisplayAffinity(window_, WDA_MONITOR) != FALSE;
  if (!applied_) LogWarn("bittensor key: the window could not be kept out of screen capture");
}

CaptureExclusion::~CaptureExclusion() {
  if (applied_) ::SetWindowDisplayAffinity(window_, previous_);
}

}  // namespace urnw::btkeyplatform
