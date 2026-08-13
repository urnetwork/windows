// The in-app service manager (beta-distribution spec §3): what state is the
// urnetworkd Windows service in, and the two elevated verbs that change it.
//
// The portable zip has no installer, so the FIRST run of URnetwork.exe is where
// the service comes from: the app classifies the machine with read-only SCM
// queries (no elevation, ever, in this process) and offers ONE click that fires
// ONE UAC prompt — `urnetworkd install`, which package B made idempotent and
// starting, so the same verb serves first setup, repair after a stop, and
// re-pointing the service at a freshly swapped binary. Everything here is
// therefore a pure observation function or a blocking helper for a background
// thread; the banner state machine, the dispatcher marshalling and the wording
// live with the window (MainWindow::RefreshServiceSetup and friends), following
// the same coroutine idiom as every other off-thread read in the app.
//
// The one rule the classification must keep: NEVER claim more than what was
// observed. A denied config query degrades to status-only rather than
// fabricating a version comparison, and any query failure lands on Unknown,
// which the UI renders as nothing at all — a wrong "update your service" banner
// teaches the user to ignore the banner that will one day be right.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <filesystem>
#include <string>

namespace urnw {

class ServiceSetup {
 public:
  enum class State {
    // The SCM could not be asked, a query failed mid-way, or there is no
    // sibling urnetworkd.exe for the one offered action to run. No evidence,
    // no banner.
    Unknown,
    // No urnetworkd service is registered and nothing serves the control pipe.
    // The fresh-unzip state; the banner offers "Set up".
    NotInstalled,
    // Registered, not running, pipe silent. The banner offers "Start" — the
    // same install verb, which re-points binPath and starts (package B).
    Stopped,
    // Registered and running (or start-pending), versions agree as far as the
    // evidence goes. The healthy state; the UI shows nothing.
    Running,
    // The registered service's exe carries a different ProductVersion string
    // than the urnetworkd.exe sitting next to this app — the state the
    // rename-swap update flow (package D) deliberately parks the machine in.
    // The banner offers "Update"; install re-points binPath at the sibling.
    VersionMismatch,
    // The control pipe is alive but the SCM did not produce a RUNNING service
    // to explain it: a developer's `urnetworkd console` owns the tunnel. Show
    // NOTHING — an install fired now would refuse against the live pipe
    // (InstallVerb.h), and the developer does not need a banner about it.
    ConsoleMode,
  };

  struct Observation {
    State state = State::Unknown;
    // ProductVersion strings, kept as evidence for the mismatch banner's
    // wording. Empty when unread (no config access, no version resource).
    std::wstring installedVersion;
    std::wstring siblingVersion;
  };

  // What the UI needs on top of the raw classification. One standing notice,
  // not a queue: the banner shows the LAST thing that went wrong with the one
  // action it offers, and a refresh that observes the same state keeps it
  // (the UAC dialog closing re-activates the window, which refreshes — a
  // notice wiped there would flash for one frame and vanish).
  enum class Notice {
    None,
    UacDeclined,   // the calm case: the user said no, the banner stays
    ActionFailed,  // launch failed / non-zero exit / never reached the goal
  };

  struct Snapshot {
    Observation observation{};
    bool busy = false;  // an elevated verb is in flight; the click is disabled
    Notice notice = Notice::None;
  };

  // One synchronous, read-only classification. SCM + up to two file-version
  // reads — milliseconds, but still file I/O, so callers run it off the UI
  // thread (winrt::resume_background) like every other blocking read.
  static Observation Classify();

  // Poll Classify until it reports `goal` or the budget runs out; returns the
  // LAST observation either way, so a caller that timed out still knows what
  // the machine looked like when it gave up.
  static Observation AwaitState(State goal, unsigned long budgetMs);

  // ShellExecuteExW + runas on the sibling urnetworkd.exe: the single UAC
  // prompt. Blocking (the UAC dialog itself blocks inside ShellExecuteEx, the
  // verb's run blocks in the wait) — background thread only.
  struct ElevatedResult {
    bool launched = false;       // the elevated process was created
    bool declined = false;       // the UAC prompt was refused (ERROR_CANCELLED)
    bool exited = false;         // it finished within waitMs
    unsigned long exitCode = 1;  // valid only when exited; the verb's whole
                                 // interface is this code (InstallVerb.h)
  };
  static ElevatedResult RunElevatedVerb(const wchar_t* verb,
                                        unsigned long waitMs);

  // The urnetworkd.exe next to this app — both the exe the install verb runs
  // AND the version the installed service is compared against. Empty path
  // checks are the caller's job; Classify refuses to banner without it.
  static std::filesystem::path SiblingServiceExe();

  // The exe inside an SCM binary path: strip the quotes the install verb
  // always writes (InstallVerb.h QuoteServiceBinPath), or take the first
  // space-delimited token of a legacy unquoted value. Pure.
  static std::wstring ExeFromBinPath(std::wstring const& binPath);

  // The ProductVersion STRING of an exe's VERSIONINFO resource — the field
  // both App.rc and Service.rc stamp with the full release grammar, and the
  // one VersionMismatch compares. Empty on any failure (no resource, no
  // translation, unreadable file): absence of evidence, never a claim.
  static std::wstring FileProductVersion(std::wstring const& exePath);
};

}  // namespace urnw
