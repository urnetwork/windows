// SPDX-License-Identifier: MPL-2.0
#include "pch.h"

#include "ManageDevicesSheet.h"

#include <chrono>
#include <string>
#include <utility>

#include <winrt/Microsoft.UI.Xaml.Automation.h>

#include "Localization.h"
#include "Log.h"
#include "SheetFit.h"  // sheetfit: sheets clamp to the window at open time
#include "StatsFormat.h"  // RelativeTime, the sessions screen's "Last used" words
#include "Strings.h"
#include "UrColors.h"
#include "UrComponents.h"  // kit::SanitizeExternalDisplayText

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::UI::Xaml::Media;

namespace urnw {
namespace {

// A UI string from the shared localization store, by key id (Localization.h).
hstring Loc(std::string_view key) { return hstring{Localized(key)}; }

TextBlock MakeLine(hstring const& text, double fontSize, Brush const& brush) {
  TextBlock line;
  line.Text(text);
  line.FontSize(fontSize);
  if (brush) line.Foreground(brush);
  line.TextWrapping(TextWrapping::Wrap);
  return line;
}

int64_t NowUnixMillis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// The store's words, for ManageDevicesPresentation.h, against the time now.
managedevices::Text StoreText() {
  managedevices::Text text;
  text.loc = [](const char* key) { return Narrow(Localized(key)); };
  text.format = [](const char* key, const std::string& arg) {
    return Narrow(Format(key, Widen(arg)));
  };
  const int64_t now = NowUnixMillis();
  text.relative = [now](int64_t unixMillis) { return RelativeTime(unixMillis, now); };
  return text;
}

// The sdk's snapshot in the plain values the presentation reads. A device's
// name and spec are external text (a user named the device), filtered as the
// window filters the network's name.
managedevices::Snapshot PlainOf(urnet::ManageDevicesSnapshot const& snapshot) {
  managedevices::Snapshot plain;
  if (snapshot.Devices) {
    for (const urnet::ManagedDevice& device : *snapshot.Devices) {
      managedevices::Device row;
      row.clientId = device.ClientId.value_or("");
      row.kind = device.Kind;
      row.name = kit::SanitizeExternalDisplayText(device.Name);
      row.spec = kit::SanitizeExternalDisplayText(device.Spec);
      row.thisDevice = device.ThisDevice;
      row.connected = device.Connected;
      row.lastSeenMillis = device.LastSeenTime;
      plain.devices.push_back(std::move(row));
    }
  }
  plain.loaded = snapshot.Loaded;
  plain.loading = snapshot.Loading;
  plain.removingClientId = snapshot.RemovingClientId.value_or("");
  plain.errorId = snapshot.ErrorId;
  plain.addRetryCount = snapshot.AddRetryCount;
  return plain;
}

}  // namespace

std::shared_ptr<ManageDevicesSheet> ManageDevicesSheet::Create(XamlRoot const& root, SdkHost& sdk,
                                                               bool forSignIn,
                                                               hstring const& message) {
  auto sheet = std::shared_ptr<ManageDevicesSheet>(new ManageDevicesSheet(sdk, forSignIn));
  sheet->Build(root, message);
  sheet->Open();
  return sheet;
}

void ManageDevicesSheet::Build(XamlRoot const& root, hstring const& message) {
  dialog_ = ContentDialog();
  dialog_.XamlRoot(root);
  dialog_.Title(winrt::box_value(Loc(managedevices::kTitleKey)));
  dialog_.CloseButtonText(Loc("done"));
  dialog_.Background(colors::SheetBrush());
  winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::SetAutomationId(
      dialog_, L"acceptance.manage-devices");

  StackPanel content;
  content.Spacing(12);
  content.MinWidth(sheetfit::Width(root, 440));

  // why a refused sign-in opened it: what the refusal said
  messageText_ = MakeLine(message, 14, colors::TextBrush());
  messageText_.Visibility(message.empty() ? Visibility::Collapsed : Visibility::Visible);
  content.Children().Append(messageText_);

  listPanel_ = StackPanel();
  listPanel_.Spacing(8);
  listPanel_.Children().Append(MakeLine(Loc(managedevices::kHelpKey), 13, colors::MutedBrush()));

  StackPanel status;
  status.Orientation(Orientation::Horizontal);
  status.Spacing(8);
  statusText_ = MakeLine(hstring{}, 13, colors::MutedBrush());
  statusText_.Visibility(Visibility::Collapsed);
  status.Children().Append(statusText_);
  retryLink_ = HyperlinkButton();
  retryLink_.Content(winrt::box_value(Loc("retry")));
  retryLink_.Visibility(Visibility::Collapsed);
  retryLink_.Click([weak = weak_from_this()](auto const&, auto const&) {
    if (auto self = weak.lock(); self && !self->closed_) self->sdk_.RefreshManageDevices();
  });
  status.Children().Append(retryLink_);
  listPanel_.Children().Append(status);

  loadingRing_ = ProgressRing();
  loadingRing_.IsActive(false);
  loadingRing_.Visibility(Visibility::Collapsed);
  loadingRing_.HorizontalAlignment(HorizontalAlignment::Left);
  listPanel_.Children().Append(loadingRing_);

  rowsPanel_ = StackPanel();
  rowsPanel_.Spacing(4);
  ScrollViewer scroller;
  scroller.MaxHeight(sheetfit::Height(root, 360, 260));
  scroller.Content(rowsPanel_);
  listPanel_.Children().Append(scroller);
  content.Children().Append(listPanel_);

  // the confirmation, in the list's place while it asks
  confirmPanel_ = StackPanel();
  confirmPanel_.Spacing(8);
  confirmPanel_.Visibility(Visibility::Collapsed);
  confirmPanel_.Children().Append(
      MakeLine(Loc(managedevices::kRemoveTitleKey), 16, colors::TextBrush()));
  confirmBody_ = MakeLine(hstring{}, 14, colors::MutedBrush());
  confirmPanel_.Children().Append(confirmBody_);
  StackPanel answers;
  answers.Orientation(Orientation::Horizontal);
  answers.Spacing(8);
  Button remove;
  remove.Content(winrt::box_value(Loc("remove")));
  winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::SetAutomationId(
      remove, L"acceptance.manage-devices.confirm-remove");
  remove.Click([weak = weak_from_this()](auto const&, auto const&) {
    if (auto self = weak.lock()) self->ConfirmRemove();
  });
  answers.Children().Append(remove);
  Button cancel;
  cancel.Content(winrt::box_value(Loc("cancel")));
  cancel.Click([weak = weak_from_this()](auto const&, auto const&) {
    if (auto self = weak.lock()) self->CancelRemove();
  });
  answers.Children().Append(cancel);
  confirmPanel_.Children().Append(answers);
  content.Children().Append(confirmPanel_);

  dialog_.Content(content);
}

void ManageDevicesSheet::Open() {
  auto queue = dialog_.DispatcherQueue();
  std::weak_ptr<ManageDevicesSheet> weak = weak_from_this();
  sdk_.OpenManageDevices(forSignIn_, [queue, weak](SdkHost::ManageDevicesUpdate update) {
    if (!queue) return;
    queue.TryEnqueue([weak, update = std::move(update)] {
      if (auto self = weak.lock()) self->Apply(update);
    });
  });
}

void ManageDevicesSheet::Apply(SdkHost::ManageDevicesUpdate const& update) {
  if (closed_) return;
  snapshot_ = PlainOf(update.snapshot);
  canManage_ = update.canManage;
  // the device being confirmed went (removed elsewhere, or by this sheet):
  // nothing left to ask about
  if (confirming_) {
    bool listed = false;
    for (const auto& device : snapshot_.devices) {
      if (device.clientId == confirming_->clientId) listed = true;
    }
    if (!listed) CancelRemove();
  }
  Render();
}

void ManageDevicesSheet::Render() {
  const managedevices::Status status = managedevices::StatusOf(snapshot_, canManage_);
  if (status.lineKey) {
    statusText_.Text(Loc(status.lineKey));
    statusText_.Foreground(status.error ? colors::DangerBrush() : colors::MutedBrush());
    statusText_.Visibility(Visibility::Visible);
  } else {
    statusText_.Visibility(Visibility::Collapsed);
  }
  retryLink_.Visibility(status.offersRetry ? Visibility::Visible : Visibility::Collapsed);
  loadingRing_.IsActive(status.loading);
  loadingRing_.Visibility(status.loading ? Visibility::Visible : Visibility::Collapsed);

  rowsPanel_.Children().Clear();
  if (!status.showsRows) return;
  const managedevices::Text text = StoreText();
  for (const managedevices::Device& device : snapshot_.devices) {
    Grid row;
    row.Padding(ThicknessHelper::FromLengths(0, 6, 0, 6));
    ColumnDefinition grow;
    grow.Width(GridLength{1, GridUnitType::Star});
    ColumnDefinition fit;
    fit.Width(GridLengthHelper::Auto());
    row.ColumnDefinitions().Append(grow);
    row.ColumnDefinitions().Append(fit);

    StackPanel lines;
    lines.Spacing(2);
    auto title = MakeLine(hstring{Widen(managedevices::TitleOf(device, text))}, 14, colors::TextBrush());
    lines.Children().Append(title);
    const std::string detail = managedevices::DetailOf(device, text);
    if (!detail.empty()) lines.Children().Append(MakeLine(hstring{Widen(detail)}, 12, colors::MutedBrush()));
    Grid::SetColumn(lines, 0);
    row.Children().Append(lines);

    if (managedevices::Removable(device)) {
      if (snapshot_.removingClientId == device.clientId) {
        ProgressRing removing;
        removing.IsActive(true);
        removing.Width(16);
        removing.Height(16);
        removing.VerticalAlignment(VerticalAlignment::Center);
        Grid::SetColumn(removing, 1);
        row.Children().Append(removing);
      } else {
        HyperlinkButton remove;
        remove.Content(winrt::box_value(Loc("remove")));
        remove.Foreground(colors::DangerBrush());
        remove.IsEnabled(status.removeEnabled);
        remove.VerticalAlignment(VerticalAlignment::Center);
        winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::SetAutomationId(
            remove, L"acceptance.manage-devices.remove");
        remove.Click([weak = weak_from_this(), device](auto const&, auto const&) {
          if (auto self = weak.lock()) self->AskRemove(device);
        });
        Grid::SetColumn(remove, 1);
        row.Children().Append(remove);
      }
    }
    rowsPanel_.Children().Append(row);
  }
}

void ManageDevicesSheet::AskRemove(managedevices::Device const& device) {
  if (closed_ || !managedevices::Removable(device)) return;
  confirming_ = device;
  const std::string title = managedevices::TitleOf(device, StoreText());
  confirmBody_.Text(hstring{Format(managedevices::kRemoveBodyKey, Widen(title))});
  listPanel_.Visibility(Visibility::Collapsed);
  confirmPanel_.Visibility(Visibility::Visible);
}

void ManageDevicesSheet::ConfirmRemove() {
  if (closed_ || !confirming_) return;
  const std::string clientId = confirming_->clientId;
  CancelRemove();
  // the sdk removes it, then adds this device again once the slot is free
  sdk_.RemoveManagedDevice(clientId);
}

void ManageDevicesSheet::CancelRemove() {
  confirming_.reset();
  confirmPanel_.Visibility(Visibility::Collapsed);
  listPanel_.Visibility(Visibility::Visible);
}

void ManageDevicesSheet::SetMessage(hstring const& message) {
  if (closed_) return;
  messageText_.Text(message);
  messageText_.Visibility(message.empty() ? Visibility::Collapsed : Visibility::Visible);
}

void ManageDevicesSheet::Dismiss() {
  if (closed_ || !dialog_) return;
  dialog_.Hide();
}

void ManageDevicesSheet::Close() {
  if (closed_) return;
  closed_ = true;
  sdk_.CloseManageDevices();
  LogInfo("app: the device list closed");
}

}  // namespace urnw
