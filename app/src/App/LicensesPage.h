// The Licenses page (LicensesView in MainWindow.xaml), reached from Settings'
// Licenses row and shown in Settings' place, the way the Refer and earn page
// stands in for Account. android/apple Account -> Settings -> Licenses parity:
//
//   list    the intro, then "Data attributions" (kind "data"), then "Open
//           source software" (everything else), in the SDK's own order. A row
//           is the name over "version · spdx"; an entry with a notice shows
//           the notice in its row, because the notice is what that entry's
//           license requires to be SHOWN (GeoLite2's MaxMind attribution).
//   detail  the selected entry: the notice (emphasized), the copyright lines,
//           a "Project page" link, and the full license text in a monospace,
//           selectable block.
//
// The list comes from the SDK, not from a file in this repo: sdk/license.yml is
// embedded in URnetworkSdk.dll and filtered by app, so this page, the other
// apps and ur.io cannot disagree about what ships. The first read parses the
// whole embedded list (~570 KB of YAML, ~10 ms), so it runs off the UI thread,
// once per process; the list is a compile-time constant of the dll.
//
// No session is needed and none is asked for: nothing here is account data.
//
// Every decision that is a function of plain values (sections, the secondary
// line, which links open) is in LicensePresentation.h and runs in
// tools/license-tests.cpp on any host.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <optional>
#include <vector>

#include <winrt/Microsoft.UI.Xaml.Controls.h>

#include "LicensePresentation.h"
#include "SettingsSheets.h"  // rows::FieldState + the row kit
#include "UrComponents.h"

namespace winrt::URnetwork::implementation {
struct MainWindow;
}

namespace urnw {

class LicensesPage {
 public:
  explicit LicensesPage(winrt::URnetwork::implementation::MainWindow& window);

  // The pane titles, the intro and the two back affordances; builds once.
  void ApplyStrings();
  // Opening the page: the first open reads the list (off the UI thread) and
  // renders it; every later open keeps the list and the selection it had.
  void Load();
  // The window's one responsive switch (MainWindow::ApplyBreakpoint) says
  // whether list and detail fit side by side. Below that, one pane shows at a
  // time and selecting a row swaps the list for the detail.
  void ApplyBreakpoint(bool twoPanes);
  // "‹ Settings" or a rail navigation: the next open starts on the list again
  // (in the one-pane reading), which is where the back affordance leads.
  void OnClosed();

 private:
  // one row of the list: the SAME selectable pane row the connections table
  // uses (UrPaneRowButtonStyle + the 2px accent marker), so selection reads the
  // same everywhere, but two lines tall - and taller again for a notice
  struct Row {
    urnw::kit::PaneListRowButton parts;
    winrt::hstring name;  // the automation name, before the selected suffix
    std::size_t index = 0;
  };

  void Build();  // idempotent
  winrt::fire_and_forget LoadEntries();
  void ApplyEntries(bool failed, std::vector<LicenseView> entries);
  void RenderList();
  void AppendRow(winrt::Microsoft::UI::Xaml::Controls::Panel const& host, std::size_t index);
  void Select(std::size_t index, bool showDetail);
  void RenderDetail();
  void ApplyLayout();

  winrt::URnetwork::implementation::MainWindow& w_;
  bool built_ = false;
  bool loadStarted_ = false;

  std::vector<LicenseView> entries_;
  std::vector<Row> rows_;
  std::optional<std::size_t> selected_;

  // the list's state line while the read is in flight, or when it failed
  winrt::Microsoft::UI::Xaml::Controls::TextBlock listState_{nullptr};
  // the rows go here, under the intro, once the read lands
  winrt::Microsoft::UI::Xaml::Controls::StackPanel sectionsHost_{nullptr};

  bool twoPanes_ = true;
  // one-pane reading only: the detail is on screen in the list's place
  bool detailShown_ = false;
};

}  // namespace urnw
