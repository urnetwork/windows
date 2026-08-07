// The developer / reliability destination (spec P2), and the persistent
// session-mode notice.
//
// iOS parity: Main/Account/Settings/Developer/DeveloperView.swift (742 LOC) +
// Shared/ViewModels/ReliabilityStore.swift (543). Everything it drives hangs
// off DeviceRemote, so the whole screen works over the rpc with NO tunnel —
// which is what `urnetworkd console --rpc-only` exists for.
//
// Three properties this screen must keep, each of which has already cost a
// platform real time:
//
//   1. A settings edit is a read-modify-write of the WHOLE struct from a FRESH
//      read (SdkHost::UpdateReliabilitySettings), never from the snapshot on
//      screen — the poll's copy is always one interval stale and would revert
//      whatever changed underneath it.
//   2. A NIL settings read means "nothing is in force", NOT "everything is
//      off". Writing back a zeroed struct disables the entire reliability
//      stack and sync() latches it. That bug shipped once.
//   3. The poll runs only while this destination is BOTH selected and the
//      window is presenting, and it never runs on the UI thread: every getter
//      here is a synchronous rpc to the service.
//
// Deliberately absent: dropExit, stallExit, shuffleExits and the probe-suite
// getters are DeviceLocal-only with no DeviceRemote equivalent. Android offers
// them; this app cannot reach them, and a button that calls nothing is worse
// than no button.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>

#include "SdkHost.h"

namespace winrt::URnetwork::implementation {
struct MainWindow;
}

namespace urnw {

class DeveloperPage {
 public:
  explicit DeveloperPage(winrt::URnetwork::implementation::MainWindow& window);
  ~DeveloperPage();

  // Builds the whole surface. It is code-built rather than markup because
  // MainWindow.xaml is shared by every parallel phase and this screen is ~40
  // controls; the XAML side is one empty ScrollViewer host and one nav item.
  void ApplyStrings();

  // The nav destination was selected / deselected.
  void SetSelected(bool selected);
  // The window became visible+active, or stopped being so (AppController's
  // presentation lifecycle). Polling needs BOTH this and SetSelected.
  void SetPresentationActive(bool active);

  // ---- the persistent session-mode notice ----------------------------------
  // SdkHost::SetModeNoticeHandler is bound here, in the ctor. Already marshaled
  // onto the UI thread by the time this runs; see the threading note on
  // SdkHost::ModeNotice — the handler itself may not touch SdkHost.
  //
  // The bar it drives lives at window level (MainWindow.xaml, ModeNoticeBar) so
  // it is visible from every destination, not just this one: an rpc-only
  // session carries no traffic whichever screen the user is looking at.
  void OnModeNotice(SdkHost::ModeNotice const& notice);

  // --preview-ui only (Startup.h): there is no session in a preview run, so the
  // notice can never fire on its own and the bar would never be drawn. Raise a
  // synthetic one — the same pattern SettingsPage/WalletPage use for their
  // snackbars, and for the same reason.
  void ShowPreviewModeNotice();

  // --preview-ui only. Everything below the intro card is gated on a live
  // ReliabilitySettings read, so a preview run shows the EMPTY state and the
  // ~50 controls, the metric rows and both tables are never drawn — which is
  // exactly the "written, reviewed, merged, never rendered" hole the
  // --preview-ui note in Startup.h exists to close. Feed one synthetic snapshot
  // so the populated state can be looked at.
  //
  // It writes nothing: there is no device in a preview run, so an edit lands on
  // SdkHost::UpdateReliabilitySettings, which no-ops without one.
  void ShowPreviewSnapshot();

  // ---- callbacks -----------------------------------------------------------
  // Public because every control's handler resolves the window's weak reference
  // and then reaches back through MainWindow::developer(), which is the pattern
  // the other page units use. Not part of the surface anyone else should drive.
  void Poll();
  void ApplySnapshot(ReliabilitySnapshot const& snap);
  void RunAction(ReliabilityAction action, std::wstring const& described,
                 std::string const& exitClientId = {});
  void OnBoolToggled(size_t index);
  void OnNumChanged(size_t index);

 private:
  using ToggleSwitch = winrt::Microsoft::UI::Xaml::Controls::ToggleSwitch;
  using NumberBox = winrt::Microsoft::UI::Xaml::Controls::NumberBox;
  using TextBlock = winrt::Microsoft::UI::Xaml::Controls::TextBlock;
  using StackPanel = winrt::Microsoft::UI::Xaml::Controls::StackPanel;
  using Border = winrt::Microsoft::UI::Xaml::Controls::Border;
  using FrameworkElement = winrt::Microsoft::UI::Xaml::FrameworkElement;

  // A bool setting, bound to its ReliabilitySettings field by pointer-to-member
  // so the 34 rows are a table rather than 34 hand-written closures.
  struct BoolRow {
    ToggleSwitch toggle{nullptr};
    bool urnet::ReliabilitySettings::*field = nullptr;
  };
  // A numeric setting (millis or a count).
  struct NumRow {
    NumberBox box{nullptr};
    TextBlock effective{nullptr};
    // exactly one of these is set
    int64_t urnet::ReliabilitySettings::*i64 = nullptr;
    int32_t urnet::ReliabilitySettings::*i32 = nullptr;
    std::wstring zeroLabel;  // what a 0 actually does, when it is not "0"
    bool millis = false;
  };
  struct MetricRow {
    FrameworkElement root{nullptr};
    TextBlock value{nullptr};
  };

  void Build();
  void EnsureBuilt();
  void ApplySnapshotNow(ReliabilitySnapshot const& snap);
  void ApplyMetrics(std::optional<urnet::ReliabilityMetrics> const& metrics);
  void ApplySettings(std::optional<urnet::ReliabilitySettings> const& settings);
  void ApplyExits(ReliabilitySnapshot const& snap);

  void ReconcilePolling();
  // Run `mutate` over a FRESH whole-struct read on a worker, then re-poll.
  void EditSettings(std::function<void(urnet::ReliabilitySettings&)> mutate);
  void SetLastAction(std::wstring const& text);

  winrt::URnetwork::implementation::MainWindow& w_;

  bool built_ = false;
  bool selected_ = false;
  bool presentationActive_ = false;
  // Set while a poll result is being written into the controls, so the
  // ValueChanged/Toggled handlers can tell a user edit from a refresh. Without
  // it every refresh would write the value back to the device it just read it
  // from — 40 pointless rpcs per poll, and a race with a user edit.
  bool applying_ = false;
  // --preview-ui raised a synthetic notice; real (empty) pushes must not clear
  // it. Never set in a normal run.
  bool previewNotice_ = false;
  // --preview-ui put synthetic data on screen; a real (empty) poll result must
  // not wipe it. Never set in a normal run.
  bool previewData_ = false;

  // Cleared by the dtor. A detached poll thread checks it before touching the
  // process-wide SdkHost, which is torn down at shutdown.
  std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
  std::shared_ptr<std::atomic<bool>> inFlight_ = std::make_shared<std::atomic<bool>>(false);

  winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer pollTimer_{nullptr};

  // ---- the built tree ----
  StackPanel root_{nullptr};
  TextBlock connectHint_{nullptr};
  TextBlock lastAction_{nullptr};
  // every card except the intro: hidden while there is nothing in force
  std::vector<Border> liveCards_;
  std::vector<MetricRow> metricRows_;
  TextBlock noFailures_{nullptr};
  StackPanel exitsBody_{nullptr};
  StackPanel destinationsBody_{nullptr};
  // last rendered table content, so a 5s poll does not rebuild (and drop the
  // pointer out of) rows that have not changed
  std::wstring exitsSignature_;
  std::wstring destinationsSignature_;
  std::vector<BoolRow> boolRows_;
  std::vector<NumRow> numRows_;
};

}  // namespace urnw
