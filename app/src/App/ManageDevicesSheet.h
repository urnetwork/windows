// The device list (owner, 2026-11-08: the windows app at parity with apple and
// android): the network's devices and hosted proxies, each removable but this
// one, so the user frees a slot of the concurrent client limit for this device.
// Opened from the client limit notice's Manage devices, from Settings, and for a
// sign-in the limit refused. The list is the sdk's ManageDevicesViewController,
// which SdkHost opens (OpenManageDevices) on the session's device when there is
// one, so a removal that frees a slot ends the device's hold at once, else on the
// Api, and for a refused sign-in on that sign-in's token, so a removal runs its
// registration again. This sheet renders the snapshots
// (ManageDevicesPresentation.h decides the words) and asks for a refresh or a
// removal; a removal asks first, inside the sheet, since only one ContentDialog
// can show at a time.
//
// Threading is SessionsPage's: the list publishes on its own goroutines, the
// update is marshalled to the UI thread through the dialog's DispatcherQueue,
// resolved through a weak reference, and dropped once the sheet has closed.
// Everything else runs on the UI thread, and every call into SdkHost returns at
// once.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <memory>
#include <optional>
#include <string>

#include <winrt/Microsoft.UI.Xaml.Controls.h>

#include "ManageDevicesPresentation.h"
#include "SdkHost.h"

namespace urnw {

class ManageDevicesSheet : public std::enable_shared_from_this<ManageDevicesSheet> {
 public:
  // `forSignIn`: the list of the sign-in the client limit refused, with
  // `message` (what the refusal said) above it. The caller shows Dialog() and
  // calls Close once it has closed.
  static std::shared_ptr<ManageDevicesSheet> Create(
      winrt::Microsoft::UI::Xaml::XamlRoot const& root, SdkHost& sdk, bool forSignIn,
      winrt::hstring const& message);

  winrt::Microsoft::UI::Xaml::Controls::ContentDialog Dialog() const { return dialog_; }
  bool ForSignIn() const { return forSignIn_; }
  // What a retry of the refused sign-in answered, above the list: refused
  // again, or another failure.
  void SetMessage(winrt::hstring const& message);
  // Takes the sheet down: the sign-in it was opened for went through, or the
  // account signed out. Close follows when the dialog has closed.
  void Dismiss();
  // The dialog closed: the list closes with it, and a snapshot still on its way
  // is dropped.
  void Close();

 private:
  ManageDevicesSheet(SdkHost& sdk, bool forSignIn) : sdk_(sdk), forSignIn_(forSignIn) {}

  void Build(winrt::Microsoft::UI::Xaml::XamlRoot const& root, winrt::hstring const& message);
  // Asks SdkHost for the list; its snapshots arrive through Apply.
  void Open();
  void Apply(SdkHost::ManageDevicesUpdate const& update);
  void Render();
  // The confirmation, in the list's place, and its two answers.
  void AskRemove(managedevices::Device const& device);
  void ConfirmRemove();
  void CancelRemove();

  SdkHost& sdk_;
  const bool forSignIn_;
  winrt::Microsoft::UI::Xaml::Controls::ContentDialog dialog_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock messageText_{nullptr};
  // the list: the help, the state line with its Retry, the ring, the rows
  winrt::Microsoft::UI::Xaml::Controls::StackPanel listPanel_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock statusText_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::HyperlinkButton retryLink_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::ProgressRing loadingRing_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::StackPanel rowsPanel_{nullptr};
  // the confirmation
  winrt::Microsoft::UI::Xaml::Controls::StackPanel confirmPanel_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock confirmBody_{nullptr};
  std::optional<managedevices::Device> confirming_;
  managedevices::Snapshot snapshot_;
  bool canManage_ = true;
  bool closed_ = false;
};

}  // namespace urnw
