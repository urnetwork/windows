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
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
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
  void SetLastAction(std::wstring const& text);

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
  // A table row is built once per change of IDENTITY (which exits exist) and
  // its cells are written on every poll. Keying the rebuild on the values
  // instead would tear the table down every 5s on any live session, because
  // FlowCount never stops moving — and it would destroy the Migrate button
  // under the user's pointer.
  struct ExitRow {
    std::string clientId;
    FrameworkElement root{nullptr};
    TextBlock window{nullptr};
    TextBlock tier{nullptr};
    TextBlock flows{nullptr};
    TextBlock dials{nullptr};
    TextBlock state{nullptr};
  };
  struct DestinationRow {
    TextBlock exit{nullptr};
    TextBlock flows{nullptr};
  };

  void Build();
  void EnsureBuilt();
  void ApplySnapshotNow(ReliabilitySnapshot const& snap);
  void ApplyMetrics(std::optional<urnet::ReliabilityMetrics> const& metrics);
  // Takes the whole snapshot, not just the settings: "no device", "device but
  // the service is detached" and "connected but nothing in force" are three
  // different states and a diagnostic screen has to tell them apart.
  void ApplySettings(ReliabilitySnapshot const& snap);
  void ApplyExits(ReliabilitySnapshot const& snap);

  void ReconcilePolling();
  // Run `mutate` over a FRESH whole-struct read on the bridge, then re-read.
  void EditSettings(std::function<void(urnet::ReliabilitySettings&)> mutate);

  // ---- the bridge --------------------------------------------------------
  // ONE serial worker thread carries every rpc this screen makes. It is the
  // port of iOS ReliabilityStore.bridgeQueue, and it is load-bearing three
  // times over — the first version used a detached std::thread per call and got
  // all three wrong:
  //
  //   ORDER. Every settings edit is a read-modify-write that writes an
  //   ABSOLUTE value for its field. Two edits in flight at once (two clicks on
  //   a NumberBox spinner, a switch toggled twice) are serialised by SdkHost's
  //   lock but NOT ordered, so the device could end up holding the older value
  //   while the screen shows the newer one. FIFO on one thread fixes it.
  //
  //   STALE PAINT. A poll that read before an edit committed could be delivered
  //   to the UI after the edit's own snapshot and repaint the pre-edit value.
  //   One queue means results reach the UI in the order they were produced.
  //
  //   LIFETIME. A detached worker tested an `alive` flag and then called into
  //   the process-wide SdkHost — a TOCTOU that, at tray-Quit, can land inside a
  //   half-destroyed AppController. The dtor now joins this thread, so no job
  //   can be inside SdkHost once ~DeveloperPage has returned.
  //
  // The join's cost, stated honestly: it is NOT bounded by one rpc. A job can
  // be blocked on SdkHost::mutex_, which BootstrapSession holds across several
  // synchronous service rpcs, so against a hung or unreachable service a
  // tray-Quit can freeze for multiple seconds. That is still the right trade
  // against a use-after-free in a half-torn-down process, and in practice the
  // bridge is idle at quit — polling stops whenever the window is not
  // presenting — but it is a real worst case, not a theoretical one.
  void Submit(std::function<void()> job);
  void BridgeLoop();

  std::thread bridge_;
  std::mutex bridgeMutex_;
  std::condition_variable bridgeCv_;
  std::deque<std::function<void()>> bridgeJobs_;
  bool bridgeStop_ = false;
  // A poll is queued or running. Polls coalesce (a slow read must not stack
  // more behind it); edits and actions never do.
  std::atomic<bool> pollPending_{false};

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

  winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer pollTimer_{nullptr};

  // ---- the built tree ----
  StackPanel root_{nullptr};
  TextBlock connectHint_{nullptr};
  TextBlock lastAction_{nullptr};
  // The two intro-card actions that touch the device. Held because they are the
  // only action buttons OUTSIDE liveCards_, so they cannot be hidden wholesale
  // and are enabled/disabled instead.
  winrt::Microsoft::UI::Xaml::Controls::Button simulateButton_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Button syncButton_{nullptr};
  // Two gates, not one, because the things behind them fail independently.
  //
  //   deviceCards_   measurements, exits, destinations. They need a SESSION and
  //                  nothing more, and they are what a developer opens this
  //                  screen for during a freeze.
  //   settingsCards_ the five override sections. They need a live
  //                  ReliabilitySettings, because rendering a zeroed struct
  //                  would present defaults as if they were in force.
  //
  // The first version gated all of them on settings.has_value(). Observed live
  // against a real rpc-only session: getReliabilitySettings() returns null
  // whenever there is no multi client override, which is the ORDINARY state —
  // so the measurements and the exit table, which were perfectly readable, were
  // hidden because an unrelated getter returned nil. The screen was blank
  // exactly when it was wanted.
  std::vector<Border> deviceCards_;
  std::vector<Border> settingsCards_;
  std::vector<MetricRow> metricRows_;
  TextBlock noFailures_{nullptr};
  StackPanel exitsBody_{nullptr};
  StackPanel destinationsBody_{nullptr};
  // The client-id / destination-ip sequences the current rows were built from.
  // OPTIONAL, not a bare string: an empty table has an empty identity, so with
  // a plain std::wstring the very first apply of an empty exit list compares
  // equal to the initial value, the rebuild is skipped, and the "No exits.
  // Connect first." row is never created. That state is reachable — connected,
  // settings in force, no exits yet.
  std::optional<std::wstring> exitsIdentity_;
  std::optional<std::wstring> destinationsIdentity_;
  std::vector<ExitRow> exitRows_;
  std::vector<DestinationRow> destinationRows_;
  std::vector<BoolRow> boolRows_;
  std::vector<NumRow> numRows_;
};

}  // namespace urnw
