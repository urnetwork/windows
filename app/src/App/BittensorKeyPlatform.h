// What only Windows can do for the payout key's screens (apple/BTEASY.md,
// Screen privacy and Porting; the port brief's Windows column): device
// authentication with Windows Hello, the clipboard for an explicit Copy, and
// keeping the window out of screen capture while the Key screen is up. The sdk
// decides when each is needed (NeedsAuthentication, ConfirmNeedsAuthentication,
// PhraseToCopy); these only do it.
//
// Nothing here logs or keeps the phrase: the clipboard's clear-after is keyed on
// the clipboard's sequence number, not on a copy of the words.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Microsoft.UI.Dispatching.h>

namespace urnw::btkeyplatform {

// Windows Hello (UserConsentVerifier), over `window`, saying `reason`. True once
// the user verified. A machine with nothing to verify with (no Windows Hello
// set up for this user, no device for it, or Hello turned off by policy) counts
// as verified, as a device with no passcode does on Apple: its owner must still
// be able to save the phrase. A busy device, a cancel and a failure do not.
winrt::Windows::Foundation::IAsyncOperation<bool> Authenticate(HWND window, winrt::hstring reason);

// The explicit Copy: the phrase onto the clipboard, kept out of the clipboard
// history (Win+V) and off the cloud clipboard, and taken off again after
// urnet::BittensorLocalKeyCopySeconds if nothing replaced it. False when the
// clipboard refused: nothing was copied, and there is no fallback that would
// let it into the history.
bool CopyPhrase(std::string const& phrase,
                winrt::Microsoft::UI::Dispatching::DispatcherQueue const& queue);

// Keeps `window` out of screenshots, recordings and screen sharing for as long
// as it lives (SetWindowDisplayAffinity, WDA_EXCLUDEFROMCAPTURE), and puts back
// what the window had. Windows before 10 2004 draw it black in a capture
// instead (WDA_MONITOR). Best effort, as on every platform: a camera, and
// capture that does not honor the affinity, still see the screen.
class CaptureExclusion {
 public:
  explicit CaptureExclusion(HWND window);
  ~CaptureExclusion();
  CaptureExclusion(CaptureExclusion const&) = delete;
  CaptureExclusion& operator=(CaptureExclusion const&) = delete;

 private:
  HWND window_ = nullptr;
  DWORD previous_ = 0;
  bool applied_ = false;
};

}  // namespace urnw::btkeyplatform
