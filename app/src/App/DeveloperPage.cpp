// SPDX-License-Identifier: MPL-2.0
#include "pch.h"

#include "DeveloperPage.h"

#include <cmath>
#include <thread>

#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Text.h>

#include "Log.h"
#include "MainWindow.xaml.h"
#include "PageContext.h"
#include "Strings.h"
#include "UrColors.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::UI::Xaml::Media;
using namespace urnw::pages;

namespace urnw {
namespace {

using winrt::Windows::Foundation::IInspectable;

// The poll interval, matching iOS ReliabilityStore.pollInterval. Each tick is
// four synchronous rpcs into the service, so it runs on a worker and only while
// the destination is both selected and presenting.
constexpr std::chrono::milliseconds kPollInterval{5000};

// ---- strings ---------------------------------------------------------------
//
// READ THIS BEFORE ADDING A ROW.
//
// The rule for this app is that every user-facing string comes from the shared
// localization store (@urnetwork/localizations) through Localization.h. The
// developer surface is the one place where that is not yet possible: the store
// ships 916 English keys and NOT ONE of them covers this screen. `reliability`
// is the earnings-page word and `reliability_settings` is "Contribute
// bandwidth" — a different feature entirely. iOS is in the same position: its
// DeveloperView passes bare `LocalizedStringKey` literals with no catalog
// entries behind them, so those literals ARE its strings.
//
// So each row below carries BOTH the store key id it should have and the iOS
// literal, and Dev() prefers the store the moment the key lands. Localized()
// already returns the key id itself on a miss (that is how a typo is made
// visible), and Plural() already uses that same equality as its miss test — so
// this is the established idiom here, not a new one.
//
// Adding the keys to @urnetwork/localizations is separate work. There is no
// doc listing them and there should not be one — it would go stale the first
// time a row is added. The list IS this file, and it extracts mechanically:
//
//     grep -oE '"dev_[a-z0-9_]+"' DeveloperPage.cpp | sort -u
//
// When they land, this screen localizes with no code change.
hstring Dev(std::string_view key, const wchar_t* english) {
  std::wstring value = urnw::Localized(key);
  if (value == urnw::Widen(key)) return hstring{english};
  return hstring{value};
}

// The same, as a std::wstring, for the places that compose text.
std::wstring DevW(std::string_view key, const wchar_t* english) {
  return std::wstring{Dev(key, english).c_str()};
}

// ---- small builders --------------------------------------------------------

Brush MutedBrush() { return colors::MutedBrush(); }
Brush FaintBrush() { return colors::FaintBrush(); }

TextBlock MakeText(hstring const& text, double size, Brush const& brush = nullptr,
                   bool wrap = false) {
  TextBlock tb;
  tb.Text(text);
  tb.FontSize(size);
  if (brush) tb.Foreground(brush);
  if (wrap) tb.TextWrapping(TextWrapping::Wrap);
  tb.VerticalAlignment(VerticalAlignment::Center);
  return tb;
}

std::optional<Style> LookupStyle(hstring const& key) {
  auto resources = Application::Current().Resources();
  auto boxed = winrt::box_value(key);
  if (resources.HasKey(boxed)) {
    if (auto style = resources.Lookup(boxed).try_as<Style>()) return style;
  }
  return std::nullopt;
}

// A UrCard (App.xaml UrCardStyle) with a heading and a vertical body.
Border MakeCard(hstring const& heading, StackPanel& body) {
  Border card;
  if (auto style = LookupStyle(L"UrCardStyle")) card.Style(*style);
  StackPanel outer;
  outer.Spacing(12);
  if (!heading.empty()) {
    TextBlock title;
    title.Text(heading);
    if (auto style = LookupStyle(L"UrCardLabelStyle")) title.Style(*style);
    outer.Children().Append(title);
  }
  body = StackPanel();
  body.Spacing(10);
  outer.Children().Append(body);
  card.Child(outer);
  return card;
}

// label + wrapped detail on the left, `trailing` on the right. `outLabel`
// receives the label block so a toggle can be LabeledBy it — the
// UrSwitchToggleStyle comment is explicit that a switch without it is nameless
// to a screen reader, and this screen has twelve of them.
Grid MakeSettingRow(hstring const& label, hstring const& detail,
                    FrameworkElement const& trailing, TextBlock* outLabel = nullptr) {
  Grid row;
  ColumnDefinition left, right;
  left.Width(GridLength{1, GridUnitType::Star});
  right.Width(GridLength{0, GridUnitType::Auto});
  row.ColumnDefinitions().Append(left);
  row.ColumnDefinitions().Append(right);

  StackPanel text;
  text.Spacing(2);
  text.Margin(Thickness{0, 0, 16, 0});
  TextBlock title = MakeText(label, 14, colors::TextBrush(), true);
  text.Children().Append(title);
  if (outLabel) *outLabel = title;
  if (!detail.empty()) text.Children().Append(MakeText(detail, 11, FaintBrush(), true));
  Grid::SetColumn(text, 0);
  row.Children().Append(text);

  Grid::SetColumn(trailing, 1);
  row.Children().Append(trailing);
  return row;
}

// The two kit button roles that fit a dense diagnostic surface: the brand
// accent for the one primary affordance, and UrCardRowButtonStyle (compact,
// card-coloured, real hover/focus states) with accent text for everything else
// — which is what iOS's borderless accent `actionRow` looks like. The 48px /
// 24pt UrPrimary/UrSecondary pills are the sign-in CTA role and would swamp a
// table row.
Button MakeActionButton(hstring const& text, bool primary = false) {
  Button b;
  b.Content(winrt::box_value(text));
  if (primary) {
    if (auto style = LookupStyle(L"AccentButtonStyle")) b.Style(*style);
  } else if (auto style = LookupStyle(L"UrCardRowButtonStyle")) {
    b.Style(*style);
    b.Foreground(colors::MakeBrush(colors::kAccent));
  }
  b.HorizontalAlignment(HorizontalAlignment::Left);
  return b;
}

// A table row: one cell per width, star widths spelled as negative numbers.
Grid MakeTableRow(std::initializer_list<double> widths) {
  Grid row;
  for (double w : widths) {
    ColumnDefinition c;
    c.Width(w < 0 ? GridLength{-w, GridUnitType::Star} : GridLength{w, GridUnitType::Pixel});
    row.ColumnDefinitions().Append(c);
  }
  return row;
}

void PutCell(Grid const& row, int column, FrameworkElement const& element) {
  Grid::SetColumn(element, column);
  row.Children().Append(element);
}

// ---- formatting ------------------------------------------------------------
// iOS formatDurationMillis: 0 is not a duration, it is "the behaviour that
// shipped before this control existed".
std::wstring FormatDurationMillis(int64_t ms) {
  if (ms == 0) return urnw::Localized("off");
  if (ms < 1000) return std::format(L"{}ms", ms);
  if (ms < 60000) {
    const double seconds = static_cast<double>(ms) / 1000.0;
    return ms % 1000 == 0 ? std::format(L"{}s", ms / 1000) : std::format(L"{:.1f}s", seconds);
  }
  return std::format(L"{}m", ms / 60000);
}

std::wstring FormatBlastRadius(double value) { return std::format(L"{:.1f}", value); }

// ULIDs share their leading time bytes, so a prefix renders every exit row
// identically. iOS shows the last 8 characters; do the same.
std::wstring ShortId(std::string const& id) {
  const std::wstring wide = urnw::Widen(id);
  return wide.size() <= 8 ? wide : wide.substr(wide.size() - 8);
}

}  // namespace

// ---------------------------------------------------------------------------

DeveloperPage::DeveloperPage(winrt::URnetwork::implementation::MainWindow& window)
    : w_(window) {
  // The persistent session-mode notice. THREADING: this handler is invoked from
  // the bootstrap thread WITH SdkHost's mutex_ held (and also from
  // RefreshModeNotice, which takes it), so it may do exactly one thing —
  // enqueue and return. TryEnqueue never runs the lambda inline, so nothing
  // here can re-enter SdkHost under its own lock.
  auto queue = w_.DispatcherQueue();
  Sdk().SetModeNoticeHandler([weak = w_.get_weak(), queue](SdkHost::ModeNotice const& notice) {
    queue.TryEnqueue([weak, notice] {
      if (auto self = weak.get()) self->developer().OnModeNotice(notice);
    });
  });
  bridge_ = std::thread([this] { BridgeLoop(); });

  // What a view does when it is constructed. Gated on session existence, so a
  // logged-out launch correctly gets an empty notice rather than a fabricated
  // claim that the service is clamped.
  //
  // ON THE BRIDGE, not here. RefreshModeNotice takes SdkHost::mutex_, and the
  // bootstrap thread holds that across the WHOLE of BootstrapSession — several
  // synchronous service rpcs. This ctor runs inside MainWindow's constructor on
  // the UI thread, so calling it directly makes the first tray click produce a
  // window that does not appear until the service handshake finishes or times
  // out. The notice comes back through the handler above either way.
  Submit([] { Sdk().RefreshModeNotice(); });
}

DeveloperPage::~DeveloperPage() {
  {
    std::scoped_lock lock(bridgeMutex_);
    bridgeStop_ = true;
    bridgeJobs_.clear();  // nothing queued needs to run; we are going away
  }
  bridgeCv_.notify_all();
  // Joining is the point: after this returns, no job can be inside SdkHost or
  // holding a reference to this page. See the note on Submit in the header.
  if (bridge_.joinable()) bridge_.join();
  if (pollTimer_) pollTimer_.Stop();
}

void DeveloperPage::Submit(std::function<void()> job) {
  {
    std::scoped_lock lock(bridgeMutex_);
    if (bridgeStop_) return;
    bridgeJobs_.push_back(std::move(job));
  }
  bridgeCv_.notify_one();
}

void DeveloperPage::BridgeLoop() {
  for (;;) {
    std::function<void()> job;
    {
      std::unique_lock<std::mutex> lock(bridgeMutex_);
      bridgeCv_.wait(lock, [this] { return bridgeStop_ || !bridgeJobs_.empty(); });
      if (bridgeStop_) return;
      job = std::move(bridgeJobs_.front());
      bridgeJobs_.pop_front();
    }
    // Nothing may escape: an uncaught exception on a std::thread is
    // std::terminate, and winrt::hresult_error does NOT derive from
    // std::exception, so the catch-all is not decoration.
    try {
      job();
    } catch (const std::exception& e) {
      LogWarn("developer: bridge job failed: {}", e.what());
    } catch (...) {
      LogWarn("developer: bridge job failed");
    }
  }
}

void DeveloperPage::ApplyStrings() {
  w_.DeveloperNavItem().Content(winrt::box_value(Dev("dev_developer", L"Developer")));
  // The rest of the surface is code-built and carries its own strings; it is
  // built lazily on first selection so a user who never opens this destination
  // pays nothing for its ~40 controls.
}

// ---- the notice ------------------------------------------------------------

void DeveloperPage::OnModeNotice(SdkHost::ModeNotice const& notice) {
  // --preview-ui only. The ctor's RefreshModeNotice() is delivered through
  // TryEnqueue, so it lands AFTER the synchronous preview notice and (correctly,
  // for a real run) clears it — which left the preview bar on screen for about
  // 20ms and nothing to look at. Once a preview notice is raised, it owns the
  // bar for the rest of the process.
  if (previewNotice_) return;
  auto bar = w_.ModeNoticeBar();
  if (!notice.active) {
    bar.IsOpen(false);
    LogInfo("developer: mode notice cleared");
    return;
  }
  // Render `message` verbatim: it is a complete sentence and already
  // self-describing, so a title on top would say the words twice.
  bar.Title(hstring{});
  bar.Message(hstring{urnw::Widen(notice.message)});
  bar.Severity(notice.kind == SdkHost::ModeNotice::Kind::SessionFailed
                   ? InfoBarSeverity::Error
                   : InfoBarSeverity::Warning);
  // No dismiss control, ever: this is a standing property of the session, not
  // an event, and it stays until it is replaced.
  bar.IsClosable(false);
  bar.IsOpen(true);
  LogInfo("developer: mode notice shown (kind={}, requestedTunnel={}): {}",
          notice.kind == SdkHost::ModeNotice::Kind::SessionFailed ? "session_failed"
                                                                  : "rpc_only",
          notice.requestedTunnel, notice.message);
}

void DeveloperPage::ShowPreviewModeNotice() {
  SdkHost::ModeNotice notice;
  notice.active = true;
  notice.kind = SdkHost::ModeNotice::Kind::RpcOnly;
  notice.requestedTunnel = true;
  notice.message =
      "Developer mode: the service is running with --rpc-only, so this app "
      "asked for a tunnel and did not get one. Nothing is connected and no "
      "traffic is carried.";
  LogInfo("preview-ui: raising a synthetic rpc-only mode notice (there is no session)");
  OnModeNotice(notice);
  previewNotice_ = true;
}

void DeveloperPage::ShowPreviewSnapshot() {
  EnsureBuilt();
  ReliabilitySnapshot snap;
  snap.haveDevice = true;
  snap.remoteConnected = true;

  // Values chosen to exercise the formatting rather than to look plausible:
  // a sub-second, a fractional-second and a multi-minute duration, a zero that
  // must render as its zeroLabel, and both toggle positions.
  urnet::ReliabilitySettings s;
  s.SendStallTimeoutMillis = 900;          // -> "900ms"
  s.BusyProbeBudgetMillis = 1500;          // -> "1.5s"
  s.SchedulerPauseToleranceMillis = 5000;  // -> "5s"
  s.HeartbeatIntervalMillis = 120000;      // -> "2m"
  s.MaxFlowsPerExit = 0;                   // -> "Unlimited"
  s.ProbeSampleHostCount = 0;              // -> "All"
  s.EvaluationPoolMultiple = 0;            // -> "1 (min)"
  s.RemovalBudgetCount = 4;
  s.MinBlackholeDestinations = 3;
  s.BusyProbe = true;
  s.SoftVerdictDemote = true;
  s.EffectiveTierSelection = true;
  s.ProviderProbe = true;
  s.QuicRebindOnExitLoss = true;
  snap.settings = s;

  urnet::ReliabilityMetrics m;
  m.FlowsOpened = 1284;
  m.DialFailuresIntercepted = 37;
  m.FlowsReraced = 31;
  m.ProbesSent = 96;
  m.ProbesAnswered = 88;
  m.BusyProbesSent = 12;
  m.BusyProbesAcquitted = 9;
  m.VerdictsHeldUplinkStale = 4;
  m.VerdictsHeldTransportDown = 1;
  m.RemovalsDeferred = 2;
  m.SchedulerPausesDetected = 1;
  m.FlowsRebound = 18;
  m.RebindsAccepted = 15;
  m.RebindsRedialed = 3;
  m.ExitLossEvents = 6;  // unlocks the loss group
  m.MeanFlowsLostPerExitLoss = 3.5;
  m.MaxFlowsLostInOneEvent = 11;
  m.RecoveryMeanMillis = 820;
  m.RecoveryMaxMillis = 4300;
  m.RecoveryMissed = 2;
  m.FlowsLostToExit = 21;
  snap.metrics = m;

  // One healthy exit, one demoted-and-warned, one benched: the three states the
  // row has to tell apart.
  urnet::Exit a;
  a.ClientId = "01J8Z2QK7M9V4CXA1ND3";
  a.WindowType = "auto";
  a.Tier = 1;
  a.EffectiveTier = 1;
  a.FlowCount = 24;
  a.Proven = true;
  urnet::Exit b;
  b.ClientId = "01J8Z2QK7M9V4CXB7WQ2";
  b.WindowType = "";  // -> "auto"
  b.Tier = 1;
  b.EffectiveTier = 3;
  b.FlowCount = 6;
  b.DialFailureCount = 4;
  b.Warning = true;
  b.WarningCause = "probe_silence";
  urnet::Exit c;
  c.ClientId = "01J8Z2QK7M9V4CXCZ0P8";
  c.WindowType = "p2p";
  c.Tier = 2;
  c.EffectiveTier = 2;
  c.FlowCount = 0;
  c.Quarantined = true;
  c.P2pOnly = true;
  c.Done = true;
  snap.exits = {a, b, c};

  urnet::DestinationExit d1;
  d1.DestinationIp = "142.250.72.238";
  d1.ClientId = "01J8Z2QK7M9V4CXA1ND3";
  d1.FlowCount = 9;
  urnet::DestinationExit d2;
  d2.DestinationIp = "151.101.1.140";
  d2.ClientId = "01J8Z2QK7M9V4CXB7WQ2";
  d2.FlowCount = 3;
  urnet::DestinationExit d3;  // no client id: the em-dash cell
  d3.DestinationIp = "2606:4700::6810:85e5";
  d3.FlowCount = 1;
  snap.destinationExits = {d1, d2, d3};

  LogInfo("preview-ui: applying a SYNTHETIC reliability snapshot - none of these "
          "numbers came from a device");
  previewData_ = true;
  ApplySnapshotNow(snap);
  SetLastAction(DevW("dev_requested", L"Requested:") + L" probe all exits");
}

// ---- lifecycle -------------------------------------------------------------

void DeveloperPage::SetSelected(bool selected) {
  if (selected_ == selected) return;
  selected_ = selected;
  if (selected_) EnsureBuilt();
  ReconcilePolling();
}

void DeveloperPage::SetPresentationActive(bool active) {
  if (presentationActive_ == active) return;
  presentationActive_ = active;
  ReconcilePolling();
}

void DeveloperPage::ReconcilePolling() {
  const bool want = selected_ && presentationActive_ && built_;
  if (!pollTimer_) {
    if (!want) return;
    pollTimer_ = w_.DispatcherQueue().CreateTimer();
    pollTimer_.Interval(kPollInterval);
    pollTimer_.Tick([weak = w_.get_weak()](auto const&, auto const&) {
      if (auto self = weak.get()) self->developer().Poll();
    });
  }
  if (want) {
    if (!pollTimer_.IsRunning()) {
      LogInfo("developer: polling started ({}ms)", kPollInterval.count());
      pollTimer_.Start();
    }
    Poll();  // immediate first read, then the interval
  } else if (pollTimer_.IsRunning()) {
    LogInfo("developer: polling stopped (selected={}, presenting={})", selected_,
            presentationActive_);
    pollTimer_.Stop();
  }
}

void DeveloperPage::Poll() {
  if (!built_) return;
  // Polls coalesce: one queued or running at a time. A read slower than the
  // interval would otherwise stack behind SdkHost's lock, and the newest of the
  // pile is the only one anyone wants.
  bool expected = false;
  if (!pollPending_.compare_exchange_strong(expected, true)) return;

  auto queue = w_.DispatcherQueue();
  Submit([this, weak = w_.get_weak(), queue] {
    // Cleared on EVERY path. Leaving it true wedges both the 5s poll and the
    // Refresh button for the life of the process, and the screen would then
    // look merely stale rather than broken. (`this` is safe: the dtor joins the
    // bridge, and it drops queued jobs before it does.)
    struct ClearOnExit {
      std::atomic<bool>& flag;
      ~ClearOnExit() { flag.store(false); }
    } clear{pollPending_};

    ReliabilitySnapshot snap = Sdk().ReadReliability();
    queue.TryEnqueue([weak, snap = std::move(snap)] {
      if (auto self = weak.get()) self->developer().ApplySnapshot(snap);
    });
  });
}

void DeveloperPage::EditSettings(std::function<void(urnet::ReliabilitySettings&)> mutate) {
  auto queue = w_.DispatcherQueue();
  Submit([weak = w_.get_weak(), queue, mutate = std::move(mutate)] {
    // Fresh whole-struct read-modify-write inside SdkHost under its lock. A nil
    // read is a no-op there — never a write of a zeroed struct — and it comes
    // back as nullopt, which is the difference between "applied" and "there was
    // nothing to apply it to".
    const bool applied = Sdk().UpdateReliabilitySettings(mutate).has_value();
    // Read everything back: the device may not have applied what was asked.
    ReliabilitySnapshot snap = Sdk().ReadReliability();
    queue.TryEnqueue([weak, snap = std::move(snap), applied] {
      auto self = weak.get();
      if (!self) return;
      self->developer().ApplySnapshot(snap);
      // A failed write used to be visible only in a log file that a WinUI app
      // never shows anyone, so the control would just spring back with no
      // explanation.
      if (!applied)
        self->developer().SetLastAction(DevW(
            "dev_not_applied",
            L"Not applied: there is no reliability override in force to change."));
    });
  });
}

void DeveloperPage::RunAction(ReliabilityAction action, std::wstring const& described,
                              std::string const& exitClientId) {
  // Report the OUTCOME, after the fact. The first version wrote "Requested: x"
  // unconditionally before the job was even submitted and never corrected it,
  // so with no session the intro card rendered "Requested: sync" directly under
  // "No session. Sign in and connect to use these tools." while SdkHost logged
  // that it had skipped the call. That is this file's own rule about dead
  // buttons, violated by the file itself — and EditSettings two functions up
  // already did the right thing.
  //
  // "Requested", not "done", is still the ceiling even on success: these return
  // void over the C ABI (the DeviceLocal forms return an int64 count; the
  // DeviceRemote exports are void), so the counters and exit rows underneath
  // are where an action is actually confirmed.
  auto queue = w_.DispatcherQueue();
  Submit([weak = w_.get_weak(), queue, action, exitClientId, described] {
    const bool issued = Sdk().RunReliabilityAction(action, exitClientId);
    ReliabilitySnapshot snap = Sdk().ReadReliability();
    queue.TryEnqueue([weak, snap = std::move(snap), issued, described] {
      auto self = weak.get();
      if (!self) return;
      self->developer().ApplySnapshot(snap);
      self->developer().SetLastAction(
          issued ? DevW("dev_requested", L"Requested:") + L" " + described
                 : DevW("dev_not_issued",
                        L"Not issued: there is no live session to act on."));
    });
  });
}

void DeveloperPage::SetLastAction(std::wstring const& text) {
  if (!lastAction_) return;
  lastAction_.Text(hstring{text});
  lastAction_.Visibility(text.empty() ? Visibility::Collapsed : Visibility::Visible);
}

// ---- build -----------------------------------------------------------------

void DeveloperPage::EnsureBuilt() {
  if (built_) return;
  Build();
  built_ = true;
}

void DeveloperPage::Build() {
  root_ = StackPanel();
  root_.Spacing(16);
  root_.MaxWidth(1000);
  root_.HorizontalAlignment(HorizontalAlignment::Left);
  root_.Margin(Thickness{0, 0, 0, 24});

  // ---- intro + the always-available actions --------------------------------
  {
    StackPanel body{nullptr};
    Border card = MakeCard(Dev("dev_developer", L"Developer"), body);
    body.Children().Append(MakeText(
        Dev("dev_intro",
            L"Tools for diagnosing connection freezes. These act on the live connection."),
        13, MutedBrush(), true));

    connectHint_ = MakeText(Dev("dev_connect_to_use", L"Connect to use these tools."), 14,
                            colors::TextBrush(), true);
    body.Children().Append(connectHint_);

    StackPanel actions;
    actions.Orientation(Orientation::Horizontal);
    actions.Spacing(8);
    {
      Button refresh = MakeActionButton(Dev("dev_refresh", L"Refresh"), true);
      refresh.Click([weak = w_.get_weak()](auto const&, auto const&) {
        if (auto self = weak.get()) self->developer().Poll();
      });
      actions.Children().Append(refresh);

      // These two act on the device. Every OTHER action button lives inside
      // liveCards_ and is hidden outright when nothing is in force; these sit
      // in the always-visible intro card, so they are disabled instead (see
      // ApplySettings). Refresh above is not gated: it only re-reads, and it is
      // how a user retries after starting the service.
      simulateButton_ =
          MakeActionButton(Dev("dev_simulate_network_change", L"Simulate network change"));
      simulateButton_.IsEnabled(false);
      simulateButton_.Click([weak = w_.get_weak()](auto const&, auto const&) {
        if (auto self = weak.get())
          self->developer().RunAction(ReliabilityAction::SimulateNetworkChange,
                                      L"simulate network change");
      });
      actions.Children().Append(simulateButton_);

      syncButton_ = MakeActionButton(Dev("dev_sync", L"Sync"));
      syncButton_.IsEnabled(false);
      syncButton_.Click([weak = w_.get_weak()](auto const&, auto const&) {
        if (auto self = weak.get())
          self->developer().RunAction(ReliabilityAction::Sync, L"sync");
      });
      actions.Children().Append(syncButton_);
    }
    body.Children().Append(actions);

    lastAction_ = MakeText(hstring{}, 12, MutedBrush(), true);
    lastAction_.Visibility(Visibility::Collapsed);
    body.Children().Append(lastAction_);
    body.Children().Append(MakeText(
        Dev("dev_actions_are_requests",
            L"Actions are requests: confirm them in the measurements and exit rows, which "
            L"refresh underneath."),
        11, FaintBrush(), true));

    root_.Children().Append(card);
  }

  // A card that only means anything once something is in force.
  auto liveCard = [&](hstring const& heading, StackPanel& body) {
    Border card = MakeCard(heading, body);
    card.Visibility(Visibility::Collapsed);
    liveCards_.push_back(card);
    root_.Children().Append(card);
    return card;
  };

  // ---- measurements --------------------------------------------------------
  {
    StackPanel body{nullptr};
    liveCard(Dev("dev_measurements", L"Measurements"), body);
    body.Children().Append(MakeText(
        Dev("dev_measurements_detail",
            L"What a provider failure costs. Reset, run a test, read back."),
        11, FaintBrush(), true));

    auto metric = [&](std::string_view key, const wchar_t* label, const wchar_t* detail) {
      TextBlock value = MakeText(L"0", 14, colors::TextBrush());
      value.HorizontalAlignment(HorizontalAlignment::Right);
      value.FontFamily(FontFamily{L"Consolas"});
      Grid row = MakeSettingRow(Dev(key, label), hstring{detail}, value);
      body.Children().Append(row);
      metricRows_.push_back(MetricRow{row, value});
    };
    // The order here IS the index order ApplyMetrics uses; do not reorder one
    // without the other.
    metric("dev_flows_opened", L"Flows opened",
           L"Total since reset, so runs of different lengths compare");
    metric("dev_provider_connect_failures", L"Provider connect failures",
           L"Times a provider reported it could not open the upstream connection");
    metric("dev_moved_to_another_exit", L"Moved to another exit",
           L"How many of those failures were quietly moved instead of hanging");
    metric("dev_probes", L"Probes", L"Qualification probes this session");
    metric("dev_busy_probes", L"Busy probes",
           L"Liveness pings fired at stalled exits; acquitted ones answered and were kept");
    metric("dev_verdicts_held", L"Verdicts held",
           L"Convictions withheld because this machine's own uplink, not the provider, was "
           L"silent (uplink / transport)");
    metric("dev_removals_deferred", L"Removals deferred",
           L"Removals the storm breaker held back after a correlated burst");
    metric("dev_suspends_caught", L"Suspends caught",
           L"Host suspends the detector caught, each one a batch of verdicts held instead "
           L"of executed on a just-resumed machine");
    metric("dev_quic_flows_rebound", L"QUIC flows rebound",
           L"Flows moved to a warm exit inside a removal; accepted means the server took "
           L"the path change without a re-dial");

    noFailures_ = MakeText(Dev("dev_no_provider_failures", L"No provider failures yet."), 13,
                           MutedBrush(), true);
    body.Children().Append(noFailures_);

    metric("dev_blast_radius", L"Blast radius",
           L"Connections lost per provider failure. Lower is better");
    metric("dev_worst_single_failure", L"Worst single failure",
           L"The one the user actually feels");
    metric("dev_recovery_time", L"Recovery time",
           L"From an exit dying to that site answering again");
    metric("dev_never_came_back", L"Never came back",
           L"Sites abandoned rather than recovered");

    Button reset = MakeActionButton(Dev("dev_reset_measurements", L"Reset measurements"));
    reset.Click([weak = w_.get_weak()](auto const&, auto const&) {
      if (auto self = weak.get())
        self->developer().RunAction(ReliabilityAction::ResetMetrics, L"reset measurements");
    });
    body.Children().Append(reset);
  }

  // ---- exits ---------------------------------------------------------------
  // The desktop advantage over the phones: a real table with a header rather
  // than a stack of two-line rows.
  {
    StackPanel body{nullptr};
    liveCard(Dev("dev_exits", L"Exits"), body);
    Grid header = MakeTableRow({-2, -2, -1, -1, -1, -3, 90});
    auto head = [&](int column, std::string_view key, const wchar_t* label) {
      auto tb = MakeText(Dev(key, label), 11, FaintBrush());
      PutCell(header, column, tb);
    };
    head(0, "dev_col_exit", L"Exit");
    head(1, "dev_col_window", L"Window");
    head(2, "dev_col_tier", L"Tier");
    head(3, "dev_col_flows", L"Flows");
    head(4, "dev_col_failed_dials", L"Failed dials");
    head(5, "dev_col_state", L"State");
    body.Children().Append(header);
    exitsBody_ = StackPanel();
    exitsBody_.Spacing(6);
    body.Children().Append(exitsBody_);
  }

  // ---- destination exits ---------------------------------------------------
  // Not on iOS, which never renders getDestinationExits. It is the readout that
  // shows a single site split across exits, so a window with room for it should
  // show it.
  {
    StackPanel body{nullptr};
    liveCard(Dev("dev_destinations", L"Destinations"), body);
    body.Children().Append(MakeText(
        Dev("dev_destinations_detail",
            L"Which exit each destination's flows are landing on. A site spread over "
            L"several rows is a site that lost its single egress IP."),
        11, FaintBrush(), true));
    Grid header = MakeTableRow({-3, -2, -1});
    auto head = [&](int column, std::string_view key, const wchar_t* label) {
      PutCell(header, column, MakeText(Dev(key, label), 11, FaintBrush()));
    };
    head(0, "dev_col_destination", L"Destination");
    head(1, "dev_col_exit", L"Exit");
    head(2, "dev_col_flows", L"Flows");
    body.Children().Append(header);
    destinationsBody_ = StackPanel();
    destinationsBody_.Spacing(6);
    body.Children().Append(destinationsBody_);
  }

  // ---- the settings sections ----------------------------------------------
  StackPanel section{nullptr};

  auto boolRow = [&](std::string_view key, const wchar_t* label, const wchar_t* detail,
                     bool urnet::ReliabilitySettings::*field) {
    ToggleSwitch toggle;
    if (auto style = LookupStyle(L"UrSwitchToggleStyle")) toggle.Style(*style);
    const size_t index = boolRows_.size();
    toggle.Toggled([weak = w_.get_weak(), index](IInspectable const&, RoutedEventArgs const&) {
      if (auto self = weak.get()) self->developer().OnBoolToggled(index);
    });
    TextBlock title{nullptr};
    section.Children().Append(
        MakeSettingRow(Dev(key, label), hstring{detail}, toggle, &title));
    // Without this the switch is nameless to a screen reader (see the
    // UrSwitchToggleStyle comment in App.xaml).
    Automation::AutomationProperties::SetLabeledBy(toggle, title);
    boolRows_.push_back(BoolRow{toggle, field});
  };

  auto numRow = [&](std::string_view key, const wchar_t* label, const wchar_t* detail,
                    int64_t urnet::ReliabilitySettings::*i64,
                    int32_t urnet::ReliabilitySettings::*i32, const wchar_t* zeroLabel,
                    bool millis) {
    StackPanel trailing;
    trailing.Orientation(Orientation::Horizontal);
    trailing.Spacing(8);
    trailing.VerticalAlignment(VerticalAlignment::Center);
    TextBlock effective = MakeText(L"0", 12, MutedBrush());
    effective.MinWidth(72);
    effective.TextAlignment(TextAlignment::Right);
    trailing.Children().Append(effective);

    NumberBox box;
    box.Width(120);
    box.Minimum(0);
    // A Maximum as well as a Minimum. Without one, a typed value above
    // INT32_MAX wraps NEGATIVE through the int32 cast and writes a negative
    // MaxFlowsPerExit / RemovalBudgetCount into the live reliability stack —
    // the `raw < 0` check in OnNumChanged catches a typed minus sign, not a
    // wrap. The int64 ceiling is ~35 years in milliseconds, which is past any
    // meaningful value and short of the range where the double->int64 cast is
    // undefined.
    box.Maximum(i32 ? 2147483647.0 : 1.0e12);
    box.SpinButtonPlacementMode(NumberBoxSpinButtonPlacementMode::Compact);
    // A rejected edit snaps back to the value that IS in force rather than
    // leaving an unapplied number on screen claiming to be one.
    box.ValidationMode(NumberBoxValidationMode::InvalidInputOverwritten);
    const size_t index = numRows_.size();
    box.ValueChanged([weak = w_.get_weak(), index](NumberBox const&,
                                                   NumberBoxValueChangedEventArgs const&) {
      if (auto self = weak.get()) self->developer().OnNumChanged(index);
    });
    trailing.Children().Append(box);

    TextBlock title{nullptr};
    section.Children().Append(
        MakeSettingRow(Dev(key, label), hstring{detail}, trailing, &title));
    Automation::AutomationProperties::SetLabeledBy(box, title);
    numRows_.push_back(NumRow{box, effective, i64, i32,
                              zeroLabel ? std::wstring{zeroLabel} : std::wstring{}, millis});
  };
  auto millisRow = [&](std::string_view key, const wchar_t* label, const wchar_t* detail,
                       int64_t urnet::ReliabilitySettings::*field) {
    numRow(key, label, detail, field, nullptr, nullptr, true);
  };
  auto countRow = [&](std::string_view key, const wchar_t* label, const wchar_t* detail,
                      int32_t urnet::ReliabilitySettings::*field, const wchar_t* zeroLabel) {
    numRow(key, label, detail, nullptr, field, zeroLabel, false);
  };

  using RS = urnet::ReliabilitySettings;

  // Detection: how an exit is judged to be failing, and how fast.
  liveCard(Dev("dev_detection", L"Detection"), section);
  millisRow("dev_drop_stalled_exits_fast", L"Drop stalled exits fast",
            L"How long an exit may stop delivering before it is dropped, in ms. Off waits 30s",
            &RS::SendStallTimeoutMillis);
  boolRow("dev_probe_stalled_exits", L"Probe stalled exits before dropping",
          L"When an exit stalls, ping it once before convicting. A congested but alive exit "
          L"answers and keeps its flows; a dead one is still dropped",
          &RS::BusyProbe);
  millisRow("dev_busy_probe_wait", L"Busy probe wait",
            L"How long the stall probe waits for an answer before convicting, in ms. Off "
            L"derives half the stall bar",
            &RS::BusyProbeBudgetMillis);
  millisRow("dev_suspend_detector", L"Suspend detector",
            L"How much timer overshoot reads as the machine being suspended rather than an "
            L"exit stalling, in ms, so a resumed machine does not convict every exit at "
            L"once. Off disables it",
            &RS::SchedulerPauseToleranceMillis);
  millisRow("dev_suspend_recovery_window", L"Suspend recovery window",
            L"How long verdicts stay held after a detected suspend, in ms, giving transports "
            L"time to re-register. Off uses the built-in 5s",
            &RS::SchedulerPauseRecoveryTimeoutMillis);
  millisRow("dev_cut_dead_connects_early", L"Cut dead connects early",
            L"Drop an exit that has established nothing sooner when two sibling exits are "
            L"receiving, in ms. Off waits the full 30s connect bar",
            &RS::BlackholeConnectComparativeTimeoutMillis);
  millisRow("dev_keep_quiet_providers_longer", L"Keep quiet providers longer",
            L"How long a provider still acknowledging traffic may return nothing before it "
            L"is dropped, in ms. Off keeps them until they stop acknowledging",
            &RS::BlackholeReceiveTimeoutMillis);
  boolRow("dev_demote_before_removing", L"Demote before removing",
          L"Ambiguous verdicts bench an exit instead of tearing down its flows; removal "
          L"needs sustained evidence or an empty exit",
          &RS::SoftVerdictDemote);

  // Placement: which exit a flow lands on, and how the pool is shaped.
  liveCard(Dev("dev_placement", L"Placement"), section);
  boolRow("dev_live_tier_demotion", L"Live tier demotion",
          L"Failing dials and survived verdicts push a provider down the ranking within a "
          L"second; promotion back needs clean minutes and a proven connect",
          &RS::EffectiveTierSelection);
  countRow("dev_max_connections_per_exit", L"Max connections per exit",
           L"Losing an exit kills every connection on it. Lower spreads the damage; a site "
           L"may then use more than one exit IP",
           &RS::MaxFlowsPerExit, L"Unlimited");
  boolRow("dev_sticky_site_affinity", L"Sticky site affinity",
          L"A site's new connections stay on the exit its earlier ones already use, even "
          L"past the flow cap, so a busy site keeps one egress IP",
          &RS::AffinityStickyPastCap);
  boolRow("dev_follow_benched_exits", L"Follow benched exits",
          L"A quarantined exit keeps its own sites' new connections through the early bench, "
          L"when the verdict is least proven. New sites still avoid it",
          &RS::QuarantineGroupFollow);
  millisRow("dev_follow_window", L"Follow window",
            L"How long into a bench a site's new connections keep following their exit, in "
            L"ms. Off scatters immediately",
            &RS::GroupFollowWindowMillis);
  countRow("dev_removal_storm_limit", L"Removal storm limit",
           L"How many verdict removals are allowed per window before the rest are deferred; "
           L"a burst is more likely one local cause than many failures",
           &RS::RemovalBudgetCount, L"Off");
  millisRow("dev_removal_storm_window", L"Removal storm window",
            L"The window the removal limit is counted over, in ms. Off (like a limit of 0) "
            L"turns the breaker off",
            &RS::RemovalBudgetWindowMillis);
  boolRow("dev_keep_a_spare_exit_warm", L"Keep a spare exit warm",
          L"Size each window one exit beyond its target so a replacement is already "
          L"connected. Off waits until a loss to backfill",
          &RS::StandingReserve);
  countRow("dev_load_corroboration", L"Load corroboration",
           L"Extra silent destinations required per this many flows before a busy exit can "
           L"be benched on soft evidence. Off keeps the flat minimum",
           &RS::BlackholeLoadCorroboration, L"Off");
  countRow("dev_corroborate_silent_exits", L"Corroborate silent exits",
           L"How many distinct destinations must be silent before an exit is convicted on "
           L"no-receive, so one dead site cannot remove a working exit",
           &RS::MinBlackholeDestinations, L"Off");
  boolRow("dev_group_ips_by_site", L"Group IPs by site",
          L"Keeps a site on one exit when its hostname is not visible",
          &RS::ClusterAffinityFallback);
  boolRow("dev_converge_late_named_flows", L"Converge late-named flows",
          L"Moves later connections onto the exit the first one already uses",
          &RS::ServerNameAffinityBridge);

  // Recovery: getting a flow moving again after its exit fails.
  liveCard(Dev("dev_recovery", L"Recovery"), section);
  boolRow("dev_rebind_quic_on_exit_loss", L"Rebind QUIC on exit loss",
          L"Re-pin established QUIC flows to a live exit inside the removal instead of "
          L"tearing them down",
          &RS::QuicRebindOnExitLoss);
  boolRow("dev_retry_refused_connects_elsewhere", L"Retry refused connects elsewhere",
          L"When a provider can't reach a site, move the connection to another exit instead "
          L"of letting it hang",
          &RS::DialFailureRerace);
  boolRow("dev_signal_udp_teardown", L"Signal UDP teardown",
          L"Tells DNS and QUIC the path is gone instead of going silent",
          &RS::UdpTeardownSignal);
  millisRow("dev_release_stuck_retransmits", L"Release stuck retransmits",
            L"How long retransmits are held before one is released, in ms. Off waits 30s",
            &RS::TcpCollapseMaxHoldMillis);
  millisRow("dev_longer_tcp_idle_timeout", L"Longer TCP idle timeout",
            L"How long a TCP connection may sit idle, in ms. Off uses the UDP bound",
            &RS::TcpSequenceIdleTimeoutMillis);
  millisRow("dev_udp_idle_timeout", L"UDP idle timeout",
            L"How long a non-TCP flow may sit idle before it is reaped, in ms",
            &RS::SequenceIdleTimeoutMillis);
  millisRow("dev_uplink_silence_gate", L"Uplink silence gate",
            L"How long the whole tunnel may be silent before provider verdicts are held as "
            L"inadmissible, in ms. 0 convicts as before",
            &RS::UplinkStalenessGateMillis);
  millisRow("dev_fast_first_exit_poll", L"Fast first-exit poll",
            L"How often a connecting flow re-checks an empty window, in ms, so the first "
            L"request leaves right after the first exit lands. Off waits the 2s retry pace",
            &RS::FormationPollTimeoutMillis);

  // Probing: proving an exit can actually reach real destinations.
  liveCard(Dev("dev_probing", L"Probing"), section);
  boolRow("dev_probe_providers", L"Probe providers",
          L"Qualify exits by dialing real sites through them. An answer proves the exit; "
          L"silence never counts against it",
          &RS::ProviderProbe);
  millisRow("dev_probe_wait", L"Probe wait",
            L"How long a qualification probe waits for an answer, in ms. Off uses the "
            L"built-in 4s. It only bounds waiting for proof, it never convicts",
            &RS::ProbeTimeoutMillis);
  countRow("dev_probe_hosts_per_pass", L"Probe hosts per pass",
           L"How many health sites one qualification pass dials through an exit. 0 probes "
           L"the entire embedded list; a smaller number rotates through it in blocks",
           &RS::ProbeSampleHostCount, L"All");
  countRow("dev_probe_silence_streak", L"Probe silence streak",
           L"How many consecutive probe passes an exit may answer with total silence before "
           L"it is warned out of new-flow placement. Placement only",
           &RS::ProbeSilenceWarnStreak, L"Off");
  countRow("dev_candidates_per_slot", L"Candidates evaluated per slot",
           L"How many providers a window expansion pings and ranks per slot it needs, "
           L"keeping the best. 1 evaluates exactly what it needs",
           &RS::EvaluationPoolMultiple, L"1 (min)");
  {
    Button probe = MakeActionButton(Dev("dev_probe_all_exits_now", L"Probe all exits now"));
    probe.Click([weak = w_.get_weak()](auto const&, auto const&) {
      if (auto self = weak.get())
        self->developer().RunAction(ReliabilityAction::ProbeAllExits, L"probe all exits");
    });
    section.Children().Append(probe);
  }

  // Observability: what the session writes to the log for later forensics.
  liveCard(Dev("dev_observability", L"Observability"), section);
  millisRow("dev_state_heartbeat", L"State heartbeat",
            L"How often one line summarizing live state is written to the log for later "
            L"forensics, in ms. Off silences it",
            &RS::HeartbeatIntervalMillis);
  {
    Button reset =
        MakeActionButton(Dev("dev_reset_to_shipped_defaults", L"Reset to shipped defaults"));
    reset.Click([weak = w_.get_weak()](auto const&, auto const&) {
      if (auto self = weak.get())
        self->developer().RunAction(ReliabilityAction::ResetSettings,
                                    L"reset to shipped defaults");
    });
    section.Children().Append(reset);
  }

  w_.DeveloperView().Content(root_);
  LogInfo("developer: built {} toggles, {} numeric rows, {} metric rows", boolRows_.size(),
          numRows_.size(), metricRows_.size());
}

// ---- apply -----------------------------------------------------------------

void DeveloperPage::ApplySnapshot(ReliabilitySnapshot const& snap) {
  if (!built_) return;
  // --preview-ui only. Selecting the destination starts the poll, and the poll
  // in a preview run reads an EMPTY snapshot off a nonexistent device; it is
  // delivered asynchronously, so it lands after the synthetic one and hides
  // everything again. (Observed: the screen went back to "Connect to use these
  // tools" ~20ms after the preview data was applied.) Once preview data is in,
  // it owns the surface.
  if (previewData_) return;
  ApplySnapshotNow(snap);
}

void DeveloperPage::ApplySnapshotNow(ReliabilitySnapshot const& snap) {
  ApplySettings(snap);
  ApplyMetrics(snap.metrics);
  ApplyExits(snap);
}

void DeveloperPage::ApplySettings(ReliabilitySnapshot const& snap) {
  auto const& settings = snap.settings;
  // NIL IS "NOTHING IS IN FORCE", NOT "EVERYTHING IS OFF". The controls are
  // hidden rather than shown at zero, because a screen full of zeroed switches
  // reads as a configuration and is not one.
  const bool inForce = settings.has_value();

  // Three different reasons the controls can be absent, and a diagnostic screen
  // is the one place that must not collapse them into one sentence. This is
  // what ReliabilitySnapshot::haveDevice and remoteConnected are for.
  connectHint_.Text(
      !snap.haveDevice
          ? Dev("dev_no_device", L"No session. Sign in and connect to use these tools.")
          : (!snap.remoteConnected
                 ? Dev("dev_service_detached",
                       L"The URnetwork service is not attached, so the live connection "
                       L"cannot be read.")
                 : (inForce ? hstring{}
                            : Dev("dev_nothing_in_force",
                                  L"Connected, but no reliability override is in force "
                                  L"yet. Connect to a provider to use these tools."))));
  connectHint_.Visibility(inForce ? Visibility::Collapsed : Visibility::Visible);
  for (auto const& card : liveCards_)
    card.Visibility(inForce ? Visibility::Visible : Visibility::Collapsed);
  // The two intro-card actions are gated on a DEVICE, not on settings being in
  // force: simulateNetworkChange and sync are meaningful the moment there is a
  // session, and are exactly what a developer reaches for before any override
  // exists.
  if (simulateButton_) simulateButton_.IsEnabled(snap.haveDevice);
  if (syncButton_) syncButton_.IsEnabled(snap.haveDevice);
  if (!inForce) return;

  // RAII, not a bare pair of assignments. There are ~46 WinRT property sets and
  // a dozen std::format calls between the two, and a throw in any of them would
  // leave the flag stuck true — after which OnBoolToggled and OnNumChanged
  // early-return forever and the whole settings surface is silently read-only.
  struct ApplyingGuard {
    bool& flag;
    explicit ApplyingGuard(bool& f) : flag(f) { flag = true; }
    ~ApplyingGuard() { flag = false; }
  } guard{applying_};
  for (auto const& row : boolRows_) row.toggle.IsOn(settings.value().*(row.field));
  for (auto const& row : numRows_) {
    const int64_t value = row.i64 ? settings.value().*(row.i64)
                                  : static_cast<int64_t>(settings.value().*(row.i32));
    // Never write over a box the user is typing in: the poll is 5s and the
    // edit is not finished until it commits.
    if (row.box.FocusState() == FocusState::Unfocused)
      row.box.Value(static_cast<double>(value));
    row.effective.Text(hstring{
        value == 0 && !row.zeroLabel.empty()
            ? row.zeroLabel
            : (row.millis ? FormatDurationMillis(value) : std::format(L"{}", value))});
  }
}

void DeveloperPage::ApplyMetrics(std::optional<urnet::ReliabilityMetrics> const& metrics) {
  if (metricRows_.size() < 13) return;
  const urnet::ReliabilityMetrics m = metrics.value_or(urnet::ReliabilityMetrics{});
  auto set = [&](size_t index, std::wstring const& text) {
    metricRows_[index].value.Text(hstring{text});
    metricRows_[index].root.Visibility(Visibility::Visible);
  };
  auto hide = [&](size_t index) {
    metricRows_[index].root.Visibility(Visibility::Collapsed);
  };

  set(0, std::format(L"{}", m.FlowsOpened));
  set(1, std::format(L"{}", m.DialFailuresIntercepted));
  set(2, std::format(L"{}", m.FlowsReraced));
  set(3, std::format(L"{} sent / {} answered", m.ProbesSent, m.ProbesAnswered));
  set(4, std::format(L"{} sent / {} acquitted", m.BusyProbesSent, m.BusyProbesAcquitted));
  set(5, std::format(L"{} / {}", m.VerdictsHeldUplinkStale, m.VerdictsHeldTransportDown));

  // Three counters that are rare and device-specific: a zero is noise, not a
  // measurement, so the row only appears once it has fired.
  if (0 < m.RemovalsDeferred) set(6, std::format(L"{}", m.RemovalsDeferred)); else hide(6);
  if (0 < m.SchedulerPausesDetected)
    set(7, std::format(L"{}", m.SchedulerPausesDetected));
  else
    hide(7);
  if (0 < m.FlowsRebound)
    set(8, std::format(L"{} ({} accepted / {} re-dialed)", m.FlowsRebound, m.RebindsAccepted,
                       m.RebindsRedialed));
  else
    hide(8);

  // The loss numbers mean nothing until something has failed, and a column of
  // zeros reads as "nothing is wrong" rather than "nothing has been measured".
  const bool anyLoss = 0 < m.ExitLossEvents;
  noFailures_.Visibility(anyLoss ? Visibility::Collapsed : Visibility::Visible);
  if (!anyLoss) {
    for (size_t i = 9; i < 13; ++i) hide(i);
    return;
  }
  set(9, std::format(L"{} per failure", FormatBlastRadius(m.MeanFlowsLostPerExitLoss)));
  set(10, std::format(L"{} connections", m.MaxFlowsLostInOneEvent));
  set(11, std::format(L"avg {}, worst {}", FormatDurationMillis(m.RecoveryMeanMillis),
                      FormatDurationMillis(m.RecoveryMaxMillis)));
  set(12, std::format(L"{} of {}", m.RecoveryMissed, m.FlowsLostToExit));
}

void DeveloperPage::ApplyExits(ReliabilitySnapshot const& snap) {
  // Rebuild the ROWS only when the set of exits changes; update the CELLS every
  // time. The first version keyed the rebuild on a signature that included
  // FlowCount, which moves constantly on a live session — so every 5s poll tore
  // down and rebuilt the table, destroying and recreating each row's Migrate
  // button under the pointer. Identity is the client-id sequence; everything
  // else is a text write into a cell that is already there.
  std::wstring identity;
  for (auto const& e : snap.exits) {
    if (!e.ClientId || e.ClientId->empty()) continue;  // no identity, no row
    identity += urnw::Widen(*e.ClientId) + L";";
  }
  if (!exitsIdentity_ || *exitsIdentity_ != identity) {
    exitsIdentity_ = identity;
    exitRows_.clear();
    exitsBody_.Children().Clear();
    if (identity.empty()) {
      exitsBody_.Children().Append(
          MakeText(Dev("dev_no_exits", L"No exits. Connect first."), 13, MutedBrush(), true));
    }
    for (auto const& e : snap.exits) {
      // No client id means no stable identity, so there is nothing to migrate
      // and nothing to name the row with. Drop it rather than mint a label.
      if (!e.ClientId || e.ClientId->empty()) continue;
      const std::string clientId = *e.ClientId;
      Grid row = MakeTableRow({-2, -2, -1, -1, -1, -3, 90});
      ExitRow cells;
      cells.clientId = clientId;
      cells.root = row;

      auto mono = MakeText(hstring{ShortId(clientId)}, 13, colors::TextBrush());
      mono.FontFamily(FontFamily{L"Consolas"});
      PutCell(row, 0, mono);
      cells.window = MakeText(hstring{}, 12, MutedBrush());
      PutCell(row, 1, cells.window);
      cells.tier = MakeText(hstring{}, 12, MutedBrush());
      PutCell(row, 2, cells.tier);
      cells.flows = MakeText(hstring{}, 12, MutedBrush());
      PutCell(row, 3, cells.flows);
      cells.dials = MakeText(hstring{}, 12, MutedBrush());
      PutCell(row, 4, cells.dials);
      cells.state = MakeText(hstring{}, 12, MutedBrush(), true);
      PutCell(row, 5, cells.state);

      Button migrate = MakeActionButton(Dev("dev_migrate", L"Migrate"));
      migrate.Click([weak = w_.get_weak(), clientId](auto const&, auto const&) {
        if (auto self = weak.get())
          self->developer().RunAction(ReliabilityAction::MigrateExit,
                                      std::format(L"migrate exit {}", ShortId(clientId)),
                                      clientId);
      });
      PutCell(row, 6, migrate);
      exitsBody_.Children().Append(row);
      exitRows_.push_back(std::move(cells));
    }
  }

  // Cell values, every poll. exitRows_ is in the same order as the exits that
  // have an id, which is what identity is built from, so index tracks.
  size_t i = 0;
  for (auto const& e : snap.exits) {
    if (!e.ClientId || e.ClientId->empty()) continue;
    if (i >= exitRows_.size()) break;
    auto const& cells = exitRows_[i++];
    cells.window.Text(hstring{e.WindowType.empty() ? std::wstring{L"auto"}
                                                   : urnw::Widen(e.WindowType)});
    // tier N->M when live demotion has moved it
    cells.tier.Text(hstring{e.Tier < e.EffectiveTier
                                ? std::format(L"{}\u2192{}", e.Tier, e.EffectiveTier)
                                : std::format(L"{}", e.Tier)});
    cells.flows.Text(hstring{std::format(L"{}", e.FlowCount)});
    cells.dials.Text(hstring{std::format(L"{}", e.DialFailureCount)});
    cells.dials.Foreground(0 < e.DialFailureCount ? colors::DangerBrush() : MutedBrush());

    // The state line, joined the way iOS joins it. WarningCause is passed
    // through VERBATIM so a new go-side cause renders without an app update.
    std::vector<std::wstring> state;
    if (e.Quarantined) {
      state.push_back(DevW("dev_state_benched", L"benched"));
    } else if (e.Warning) {
      state.push_back(e.WarningCause.empty() ? DevW("dev_state_warned", L"warned")
                                             : urnw::Widen(e.WarningCause));
    }
    if (e.Done) state.push_back(DevW("dev_state_done", L"done"));
    if (e.P2pOnly) state.push_back(DevW("dev_state_p2p", L"p2p"));
    // absence of `proven` means "not yet proven", never "bad"
    if (e.Proven) state.push_back(DevW("dev_state_proven", L"proven"));
    std::wstring joined;
    for (auto const& part : state) {
      if (!joined.empty()) joined += L" \u00B7 ";  // middle dot
      joined += part;
    }
    cells.state.Text(hstring{joined});
    cells.state.Foreground(e.Quarantined || e.Warning ? colors::DangerBrush() : MutedBrush());
  }

  // ---- destinations ----
  // Same split: the destination ip sequence is the identity, the exit and the
  // flow count are cells. This table carries no buttons, but a live session
  // churns it faster than the exits table and a full rebuild every poll would
  // make it unreadable.
  std::wstring dstIdentity;
  for (auto const& d : snap.destinationExits) dstIdentity += urnw::Widen(d.DestinationIp) + L";";
  if (!destinationsIdentity_ || *destinationsIdentity_ != dstIdentity) {
    destinationsIdentity_ = dstIdentity;
    destinationRows_.clear();
    destinationsBody_.Children().Clear();
    if (snap.destinationExits.empty()) {
      destinationsBody_.Children().Append(MakeText(
          Dev("dev_no_destinations", L"No destinations yet."), 13, MutedBrush(), true));
    }
    for (auto const& d : snap.destinationExits) {
      Grid row = MakeTableRow({-3, -2, -1});
      auto ip = MakeText(hstring{urnw::Widen(d.DestinationIp)}, 13, colors::TextBrush());
      ip.FontFamily(FontFamily{L"Consolas"});
      PutCell(row, 0, ip);
      DestinationRow cells;
      cells.exit = MakeText(hstring{}, 12, MutedBrush());
      cells.exit.FontFamily(FontFamily{L"Consolas"});
      PutCell(row, 1, cells.exit);
      cells.flows = MakeText(hstring{}, 12, MutedBrush());
      PutCell(row, 2, cells.flows);
      destinationsBody_.Children().Append(row);
      destinationRows_.push_back(std::move(cells));
    }
  }
  for (size_t j = 0; j < snap.destinationExits.size() && j < destinationRows_.size(); ++j) {
    auto const& d = snap.destinationExits[j];
    destinationRows_[j].exit.Text(hstring{
        d.ClientId && !d.ClientId->empty() ? ShortId(*d.ClientId) : std::wstring{L"\u2014"}});
    destinationRows_[j].flows.Text(hstring{std::format(L"{}", d.FlowCount)});
  }
}

// ---- edits -----------------------------------------------------------------

void DeveloperPage::OnBoolToggled(size_t index) {
  if (applying_ || index >= boolRows_.size()) return;
  auto field = boolRows_[index].field;
  const bool on = boolRows_[index].toggle.IsOn();
  EditSettings([field, on](urnet::ReliabilitySettings& s) { s.*field = on; });
}

void DeveloperPage::OnNumChanged(size_t index) {
  if (applying_ || index >= numRows_.size()) return;
  const double raw = numRows_[index].box.Value();
  // NumberBox reports NaN for a cleared field. Nothing to write.
  if (std::isnan(raw) || raw < 0) return;
  auto i64 = numRows_[index].i64;
  auto i32 = numRows_[index].i32;
  // NumberBox has a Minimum but no Maximum, and it carries a double: casting a
  // typed 1e30 straight to an integer is undefined behaviour, not a big number.
  // Clamp per destination type. The ceiling is far past any meaningful value
  // (kMaxMillis is ~35 years), so this only ever catches a typo.
  constexpr double kMaxMillis = 1.0e12;
  const double ceiling = i32 ? 2147483647.0 : kMaxMillis;
  const int64_t value = static_cast<int64_t>(raw < ceiling ? raw : ceiling);
  EditSettings([i64, i32, value](urnet::ReliabilitySettings& s) {
    if (i64)
      s.*i64 = value;
    else if (i32)
      s.*i32 = static_cast<int32_t>(value);
  });
}

}  // namespace urnw
