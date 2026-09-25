// SPDX-License-Identifier: MPL-2.0
#include "pch.h"

#include "LicensesPage.h"

#include <string>
#include <utility>

#include <winrt/Microsoft.UI.Xaml.Automation.Peers.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>

#include "Localization.h"
#include "Log.h"
#include "MainWindow.xaml.h"
#include "PageContext.h"
#include "Sdk.h"
#include "UrColors.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using urnw::pages::AdvW;
using urnw::pages::H;
using urnw::pages::Loc;
using urnw::rows::ApplyFieldState;
using urnw::rows::FieldState;

namespace urnw {
namespace {

// The list rows' one height (UrPaneRowTallHeight): a name over a version line.
// A row that carries a notice grows past it, because a notice is prose its
// license requires to be READ, and trimming it to one line would defeat that.
constexpr double kRowHeight = 44;
// The list's fixed rail beside the detail, as Network's detail rail is fixed.
constexpr double kListWidth = 400;

void SetWidth(ColumnDefinition const& column, double dips) {
  if (column) column.Width(GridLengthHelper::FromPixels(dips));
}

void SetStar(ColumnDefinition const& column) {
  if (column) column.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
}

void Raw(UIElement const& element) {
  Automation::AutomationProperties::SetAccessibilityView(
      element, Automation::Peers::AccessibilityView::Raw);
}

// A padded prose block on the pane's inset, selectable: copyright lines and
// license texts are exactly what someone copies out of a screen like this.
TextBlock Prose(Panel const& host, hstring const& text, double size) {
  TextBlock block;
  block.Text(text);
  block.FontSize(size);
  block.TextWrapping(TextWrapping::Wrap);
  block.IsTextSelectionEnabled(true);
  host.Children().Append(block);
  return block;
}

LicenseView ViewOf(urnet::LicenseInfo const& info) {
  LicenseView view;
  view.name = info.Name;
  view.version = info.Version;
  view.kind = info.Kind;
  view.origin = info.Origin;
  view.url = info.Url;
  view.spdx = info.Spdx;
  view.copyright = info.Copyright;
  view.notice = info.Notice;
  view.text = info.Text;
  return view;
}

}  // namespace

LicensesPage::LicensesPage(winrt::URnetwork::implementation::MainWindow& window)
    : w_(window) {}

void LicensesPage::Build() {
  if (built_) return;
  built_ = true;
  auto host = w_.LicensesListHost();

  rows::SetPaneMode(true);
  // Why the page exists, above everything it lists.
  rows::Supporting(host, Loc("licenses_intro"));
  rows::SetPaneMode(false);

  // The state line the read terminates in when it does not produce a list. It
  // sits on the pane grid like RenderAuthMethods' line does, so "loading" and
  // "failed" are rows of this list rather than captions floating above it.
  listState_ = TextBlock();
  listState_.FontSize(12);
  listState_.TextWrapping(TextWrapping::Wrap);
  Border stateBox;
  stateBox.Padding(ThicknessHelper::FromLengths(12, 8, 12, 8));
  stateBox.BorderBrush(colors::BorderBrush());
  stateBox.BorderThickness(ThicknessHelper::FromLengths(0, 0, 0, 1));
  stateBox.Child(listState_);
  host.Children().Append(stateBox);
  ApplyFieldState(listState_, FieldState::Loading);

  sectionsHost_ = StackPanel();
  host.Children().Append(sectionsHost_);

  // Bound here, once, not in ApplyStrings: a second ApplyStrings would
  // otherwise stack a second handler on each button.
  w_.LicensesBackButton().Click([this](auto const&, auto const&) { w_.CloseLicenses(); });
  w_.LicensesDetailBackButton().Click([this](auto const&, auto const&) {
    detailShown_ = false;
    ApplyLayout();
    // back where the user was: on the row they opened
    if (selected_) {
      for (auto const& row : rows_) {
        if (row.index == *selected_) row.parts.root.Focus(FocusState::Programmatic);
      }
    }
  });
}

void LicensesPage::ApplyStrings() {
  Build();  // idempotent
  const hstring title = Loc("licenses");
  w_.LicensesPaneTitle().Text(title);
  Automation::AutomationProperties::SetName(w_.LicensesPaneA(), title);
  // "‹ Settings": the page has no rail item; this is the way back
  w_.LicensesBackButton().Content(
      winrt::box_value(hstring{L"‹ " + std::wstring{Loc("settings")}}));
  Automation::AutomationProperties::SetName(w_.LicensesBackButton(), Loc("settings"));
  // "‹ Licenses": the one-pane reading's way from the detail to the list
  w_.LicensesDetailBackButton().Content(
      winrt::box_value(hstring{L"‹ " + std::wstring{title}}));
  Automation::AutomationProperties::SetName(w_.LicensesDetailBackButton(), title);
}

// ---- lifecycle ---------------------------------------------------------------

void LicensesPage::Load() {
  Build();
  ApplyLayout();
  if (loadStarted_) return;  // in flight, or loaded: the list cannot change
  loadStarted_ = true;
  ApplyFieldState(listState_, FieldState::Loading);
  LoadEntries();
}

void LicensesPage::OnClosed() {
  detailShown_ = false;
  ApplyLayout();
}

// The package-level sdk.GetLicenses, not Device.GetLicenses. They return the
// SAME list (DeviceLocal and DeviceRemote both forward to it), but in this app
// the device is the DeviceRemote, which exists only while a service session is
// up - signed out, or with the service stopped, there is none - and it is a
// std::optional the session teardown resets on the UI thread, so reaching it
// from the background hop below could race its destruction. Licenses are not
// account or device data; nothing about them should wait on a session.
winrt::fire_and_forget LicensesPage::LoadEntries() {
  auto weak = w_.get_weak();
  auto queue = w_.DispatcherQueue();

  std::vector<LicenseView> entries;
  bool failed = false;

  // the first call parses the whole embedded license.yml
  co_await winrt::resume_background();
  try {
    if (auto list = urnet::getLicenses(urnet::LicenseAppWindows)) {
      entries.reserve(list->size());
      for (auto const& info : *list) entries.push_back(ViewOf(info));
    } else {
      LogWarn("licenses: getLicenses returned nothing");
      failed = true;
    }
  } catch (const std::exception& e) {
    LogWarn("licenses: getLicenses failed: {}", e.what());
    failed = true;
  } catch (...) {
    LogWarn("licenses: getLicenses failed");
    failed = true;
  }

  // Back to the UI thread the way every other callback does it - there is no
  // resume_foreground overload for Microsoft.UI.Dispatching.
  queue.TryEnqueue([weak, failed, entries = std::move(entries)]() mutable {
    auto window = weak.get();
    if (!window) return;
    window->licenses().ApplyEntries(failed, std::move(entries));
  });
}

void LicensesPage::ApplyEntries(bool failed, std::vector<LicenseView> entries) {
  if (failed) {
    // the next open tries again; a failed read is not a list
    loadStarted_ = false;
    ApplyFieldState(listState_, FieldState::Failed);
    return;
  }
  entries_ = std::move(entries);
  if (entries_.empty()) {
    ApplyFieldState(listState_, FieldState::Empty);
    return;
  }
  // The state line has said everything it had to; its row goes.
  if (auto box = listState_.Parent().try_as<FrameworkElement>()) {
    box.Visibility(Visibility::Collapsed);
  }
  RenderList();
  // The detail pane is never blank beside a list: the first entry is the
  // selection until the user picks another. It is the MaxMind attribution,
  // which is the one entry whose license requires it to be seen.
  Select(0, /*showDetail=*/false);
}

// ---- the list ----------------------------------------------------------------

void LicensesPage::RenderList() {
  sectionsHost_.Children().Clear();
  rows_.clear();
  const LicenseSections sections = SplitLicenseSections(entries_);
  auto section = [this](std::string_view key, std::vector<std::size_t> const& indices) {
    if (indices.empty()) return;
    // the group strip, with its count at the right as Home's groups carry theirs
    sectionsHost_.Children().Append(
        kit::MakePaneGroupHeader(Loc(key), hstring{std::to_wstring(indices.size())}).root);
    for (const std::size_t index : indices) AppendRow(sectionsHost_, index);
  };
  section("licenses_data_header", sections.data);
  section("licenses_software_header", sections.software);
}

void LicensesPage::AppendRow(Panel const& host, std::size_t index) {
  LicenseView const& entry = entries_[index];
  const bool notice = HasNotice(entry);
  const hstring name = H(entry.name);
  const hstring secondary = H(LicenseSecondaryLine(entry));

  Row row;
  row.index = index;
  row.name = name;
  auto& parts = row.parts;
  parts.root = Button();
  parts.root.Style(rows::Lookup(L"UrPaneRowButtonStyle"));
  if (notice) {
    // grows with its notice, never shorter than its neighbours
    parts.root.MinHeight(kRowHeight);
    parts.root.Padding(ThicknessHelper::FromLengths(12, 8, 12, 8));
  } else {
    // Height, not MinHeight: one row height per list (UrComponents.h)
    parts.root.Height(kRowHeight);
    parts.root.MinHeight(kRowHeight);
  }

  Grid grid;
  grid.ColumnSpacing(10);
  ColumnDefinition markerColumn, textColumn;
  markerColumn.Width(GridLengthHelper::Auto());
  textColumn.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
  grid.ColumnDefinitions().Append(markerColumn);
  grid.ColumnDefinitions().Append(textColumn);

  // The 2px leading accent bar, built exactly as kit::MakePaneListRowButton
  // builds it, so kit::SetPaneListRowSelected paints this row the way it paints
  // every other selectable row: a shape change, not a fill step alone.
  parts.marker = Border();
  parts.marker.Width(2);
  parts.marker.VerticalAlignment(VerticalAlignment::Stretch);
  parts.marker.Margin(ThicknessHelper::FromLengths(-10, 0, 0, 0));  // undo ColumnSpacing
  parts.marker.Opacity(0);
  Raw(parts.marker);
  grid.Children().Append(parts.marker);

  StackPanel text;
  text.Spacing(1);
  text.VerticalAlignment(VerticalAlignment::Center);
  Grid::SetColumn(text, 1);

  parts.title = TextBlock();
  parts.title.Style(rows::Lookup(L"UrRowTitleStyle"));
  parts.title.Text(name);
  Raw(parts.title);
  text.Children().Append(parts.title);

  parts.meta = TextBlock();
  parts.meta.Style(rows::Lookup(L"UrRowNoteStyle"));
  kit::SetTextOrCollapse(parts.meta, secondary);
  Raw(parts.meta);
  text.Children().Append(parts.meta);

  std::wstring description{secondary};
  if (notice) {
    // Verbatim, in full, in the row: the notice is what this entry's license
    // requires the app to SHOW, so it is not left behind a click.
    TextBlock noticeBlock;
    noticeBlock.Style(rows::Lookup(L"UrRowNoteStyle"));
    noticeBlock.Text(H(TrimLicenseBlock(entry.notice)));
    noticeBlock.Foreground(colors::TextBrush());
    noticeBlock.TextTrimming(TextTrimming::None);
    noticeBlock.TextWrapping(TextWrapping::Wrap);
    noticeBlock.Margin(ThicknessHelper::FromLengths(0, 4, 0, 0));
    Raw(noticeBlock);
    text.Children().Append(noticeBlock);
    if (!description.empty()) description += L". ";
    description += std::wstring{noticeBlock.Text()};
  }
  grid.Children().Append(text);
  parts.root.Content(grid);

  // A Button whose Content is a Panel gets NO automatic automation name.
  Automation::AutomationProperties::SetName(parts.root, name);
  if (!description.empty()) {
    Automation::AutomationProperties::SetFullDescription(parts.root, hstring{description});
  }
  parts.root.Click([this, index](auto const&, auto const&) { Select(index, true); });

  host.Children().Append(parts.root);
  rows_.push_back(std::move(row));
}

void LicensesPage::Select(std::size_t index, bool showDetail) {
  if (index >= entries_.size()) return;
  selected_ = index;
  const std::wstring suffix = L", " + AdvW("adv_selected", L"selected");
  for (auto const& row : rows_) {
    const bool selected = row.index == index;
    kit::SetPaneListRowSelected(row.parts, selected);
    // A screen reader is TOLD, not shown (ConnectPage's connections table).
    Automation::AutomationProperties::SetName(
        row.parts.root, selected ? hstring{std::wstring{row.name} + suffix} : row.name);
  }
  RenderDetail();
  if (showDetail && !twoPanes_) {
    detailShown_ = true;
    ApplyLayout();
  }
}

// ---- the detail --------------------------------------------------------------

void LicensesPage::RenderDetail() {
  auto host = w_.LicensesDetailHost();
  host.Children().Clear();
  if (!selected_ || *selected_ >= entries_.size()) {
    w_.LicensesDetailTitle().Text(hstring{});
    return;
  }
  LicenseView const& entry = entries_[*selected_];
  const hstring name = H(entry.name);
  w_.LicensesDetailTitle().Text(name);
  Automation::AutomationProperties::SetName(w_.LicensesPaneB(), name);

  StackPanel body;
  body.Spacing(12);
  body.Padding(ThicknessHelper::FromLengths(12, 16, 12, 24));

  // name, then "version · spdx"
  auto heading = Prose(body, name, 18);
  heading.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
  Automation::AutomationProperties::SetHeadingLevel(
      heading, Automation::Peers::AutomationHeadingLevel::Level1);
  const std::string secondary = LicenseSecondaryLine(entry);
  if (!secondary.empty()) {
    auto line = Prose(body, H(secondary), 13);
    line.Foreground(colors::MutedBrush());
  }

  // The notice, emphasized: the accent bar the selected row carries, on the
  // card fill, in the strong body weight. It is shown verbatim - the text is
  // the licensor's, not ours.
  if (HasNotice(entry)) {
    Border box;
    box.Background(colors::CardBrush());
    box.BorderBrush(colors::AccentBrush());
    box.BorderThickness(ThicknessHelper::FromLengths(2, 0, 0, 0));
    box.Padding(ThicknessHelper::FromLengths(12, 10, 12, 10));
    StackPanel inner;
    auto notice = Prose(inner, H(TrimLicenseBlock(entry.notice)), 14);
    notice.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
    notice.Foreground(colors::TextBrush());
    box.Child(inner);
    body.Children().Append(box);
  }

  // Copyright lines, one per line as the SDK separates them.
  const std::string copyright = TrimLicenseBlock(entry.copyright);
  if (!copyright.empty()) {
    auto block = Prose(body, H(copyright), 13);
    block.Foreground(colors::MutedBrush());
  }

  // "Project page": only for a web URL (IsOpenableLicenseUrl); the shell opens
  // it in the browser, as the protocol link on Settings does.
  if (IsOpenableLicenseUrl(entry.url)) {
    try {
      HyperlinkButton link;
      link.Content(winrt::box_value(Loc("licenses_project_page")));
      link.NavigateUri(winrt::Windows::Foundation::Uri(H(entry.url)));
      link.FontSize(13);
      link.Padding(ThicknessHelper::FromLengths(0, 0, 0, 0));
      ToolTipService::SetToolTip(link, winrt::box_value(H(entry.url)));
      Automation::AutomationProperties::SetFullDescription(link, H(entry.url));
      body.Children().Append(link);
    } catch (...) {
      LogWarn("licenses: unusable project url for {}", entry.name);
    }
  }

  // The full license text: monospace, selectable, in a block of its own on the
  // sheet surface. It scrolls with the pane (LicensesDetailScroll); wrapped
  // rather than scrolled sideways, because a second scroller nested in the
  // first traps the mouse wheel.
  const std::string licenseText = TrimLicenseBlock(entry.text);
  if (!licenseText.empty()) {
    Border box;
    box.Background(colors::SheetBrush());
    box.BorderBrush(colors::BorderBrush());
    box.BorderThickness(ThicknessHelper::FromUniformLength(1));
    box.Padding(ThicknessHelper::FromLengths(12, 10, 12, 10));
    TextBlock text;
    text.Text(H(licenseText));
    text.FontFamily(Media::FontFamily(L"Cascadia Mono, Consolas"));
    text.FontSize(12);
    text.TextWrapping(TextWrapping::Wrap);
    text.IsTextSelectionEnabled(true);
    box.Child(text);
    body.Children().Append(box);
  }

  host.Children().Append(body);
  w_.LicensesDetailScroll().ChangeView(
      nullptr, winrt::Windows::Foundation::IReference<double>{0.0}, nullptr, true);
}

// ---- layout --------------------------------------------------------------------

void LicensesPage::ApplyBreakpoint(bool twoPanes) {
  twoPanes_ = twoPanes;
  if (twoPanes_) detailShown_ = false;  // both are on screen; nothing is swapped
  ApplyLayout();
}

void LicensesPage::ApplyLayout() {
  //   two panes            list(400) | detail(*)
  //   one pane, list       list(*)
  //   one pane, detail     detail(*), with "‹ Licenses" in its header
  const bool listVisible = twoPanes_ || !detailShown_;
  const bool detailVisible = twoPanes_ || detailShown_;
  if (twoPanes_) {
    SetWidth(w_.LicensesPaneAColumn(), kListWidth);
  } else if (listVisible) {
    SetStar(w_.LicensesPaneAColumn());
  } else {
    SetWidth(w_.LicensesPaneAColumn(), 0);
  }
  if (detailVisible) {
    SetStar(w_.LicensesPaneBColumn());
  } else {
    SetWidth(w_.LicensesPaneBColumn(), 0);
  }
  w_.LicensesPaneA().Visibility(listVisible ? Visibility::Visible : Visibility::Collapsed);
  w_.LicensesPaneB().Visibility(detailVisible ? Visibility::Visible : Visibility::Collapsed);
  w_.LicensesPaneBRule().Visibility(twoPanes_ ? Visibility::Visible : Visibility::Collapsed);
  w_.LicensesDetailBackButton().Visibility(twoPanes_ ? Visibility::Collapsed
                                                     : Visibility::Visible);
}

}  // namespace urnw
