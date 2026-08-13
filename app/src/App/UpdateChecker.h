// The in-app update checker (beta-distribution spec §5): finds newer GitHub
// releases, and applies one with a verified download and a rename-swap.
//
// The portable zip has no installer, so this component IS the update story:
// poll the release list (on launch after ~30s, then every 6 hours, and on the
// two manual triggers), rank tags with Common/VersionGrammar.h, and when a
// release outranks the build's own stamped code, offer ONE click that
//
//   downloads the own-arch zip to %LOCALAPPDATA%\URnetwork\updates\<tag>\,
//   verifies it against the asset's own SHA-256 digest, stamped by GitHub in
//     the same releases JSON the check parsed (CNG SHA-256 locally),
//   extracts it with the OS tar.exe into a fresh staging dir,
//   takes ONLY allowlisted top-level payload names out of staging
//     (Common/UpdateFormats.h — archive paths are never trusted),
//   rename-swaps them into the app's own directory (<name> -> <name>.old,
//     staged -> <name>; NTFS renames running images fine, INCLUDING the
//     running service exe), and relaunches the app.
//
// After the swap the SERVICE deliberately keeps running the renamed old exe:
// restarting it needs elevation this process must never hold, so ServiceSetup's
// VersionMismatch banner finishes the job with its own one click. Two clicks
// per update, one elevation — that split is the spec's, not an accident.
//
// A dev build (urnw::version::kCode == 0) never self-updates: every release
// would outrank it forever. The periodic checker is fully disabled there; the
// developer screen's manual trigger still RUNS a check and reports what it
// found, because that is the only way to exercise this code path on a dev box.
//
// Threading follows SdkHost's standing-value contract (see CurrentAdvancedMode):
// one worker thread owns every check and every apply, Current() is valid at any
// time including before any view exists, the handler is an optimisation for
// changes after a view binds, and a surface built later binds then replays.
// Handlers are invoked on the WORKER thread and must marshal.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <condition_variable>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace urnw {

class UpdateChecker {
 public:
  // What the banner shows. One phase, not flags: every phase names exactly one
  // banner rendering, and None is "no banner at all".
  enum class Phase {
    None,         // nothing newer is known (or the checker is disabled)
    Available,    // a newer release exists; the one click is offered
    Applying,     // the click fired; `stage` says how far it has got
    ManualUnzip,  // downloaded + verified, but the app dir cannot be swapped
                  // (not writable, or the renames were denied): the zip was
                  // revealed in Explorer and the user finishes
    Failed,       // the last apply attempt failed; `failure` says where.
                  // Nothing was half-swapped (the rollback restored the old
                  // files) — the click retries from scratch. The one exception
                  // is SwapDirty, which says the rollback itself could not
                  // finish; the banner wording owns that honesty.
  };
  enum class Stage { Idle, Downloading, Verifying, Extracting, Swapping };
  // SwapDirty is Swap plus a rollback step that failed: the install dir may
  // hold mixed-version files until a retry succeeds. It exists because the
  // plain Swap banner says "the previous files were put back", and saying that
  // over a directory where it is false teaches the user to distrust the banner.
  enum class Failure { None, Download, Checksum, Extract, Swap, SwapDirty };

  // What the last CHECK concluded — the developer screen's line, separate from
  // the banner phase because "checked and found nothing" must be reportable
  // without putting anything on the connect screen.
  enum class CheckOutcome {
    NeverRan,
    InFlight,
    NoUpdate,     // newest parsed release does not outrank this build
    UpdateFound,  // it does, and the banner phase says so too
    DevBuild,     // a release exists but kCode==0 — dev builds never self-update
    Failed,       // the HTTP fetch or the JSON parse failed; details in the log
  };

  struct Snapshot {
    Phase phase = Phase::None;
    Stage stage = Stage::Idle;        // meaningful while Applying
    Failure failure = Failure::None;  // meaningful while Failed
    // The offered release (v-less grammar, e.g. "2026.8.9-101076420-beta").
    // Empty when phase == None.
    std::wstring version;
    std::uint64_t code = 0;
    // ManualUnzip: where the verified zip sits, for the banner's wording and
    // its re-reveal action.
    std::wstring zipPath;
    CheckOutcome lastCheck = CheckOutcome::NeverRan;
    // The newest release tag the last completed check parsed, whether or not
    // it outranks this build — the developer line names it either way.
    std::wstring newestVersion;
    std::uint64_t newestCode = 0;
  };

  using Handler = std::function<void(Snapshot const&)>;
  // Fired on the WORKER thread after a fully successful swap, with the path of
  // the NEW exe now sitting at the app's own location. The receiver owns the
  // actual handoff (unregister the single-instance key, spawn, exit) because
  // only the app side knows how to tear itself down.
  using RelaunchHandler = std::function<void(std::filesystem::path newExe)>;

  UpdateChecker() = default;
  ~UpdateChecker();
  UpdateChecker(UpdateChecker const&) = delete;
  UpdateChecker& operator=(UpdateChecker const&) = delete;

  // Spawn the worker: stale-file cleanup first (best-effort .old removal and
  // obsolete download dirs), then the launch-delay check and the 6h cadence.
  void Start();
  // Signal and JOIN the worker. A download in flight notices within one read
  // (the fetch loop polls the stop flag), so this is bounded, not "until the
  // 100 MB zip finishes".
  void Stop();

  Snapshot Current();
  // Store only — never invokes. Bind, then replay Current() yourself: the main
  // window is built on the first tray click, which can be minutes after the
  // launch check already ran.
  void SetHandler(Handler h);
  void SetRelaunchHandler(RelaunchHandler h);

  // Queue a check now (the developer screen's trigger). Coalesces with a check
  // already queued; ignored only after Stop().
  void CheckNow();
  // Queue the download/verify/extract/swap for the currently offered release.
  // Ignored when nothing is offered or an apply is already running.
  void BeginApply();

  // The "Check for updates automatically" preference (Settings): persisted in
  // app_prefs.json beside Advanced Mode, default ON. The static read exists so
  // the Settings row can seed itself without reaching the instance.
  static bool AutoCheckEnabled();
  // Persist + apply. Turning it ON schedules a check right away — the user
  // just asked for updates, so "in six hours" would be a strange answer.
  void SetAutoCheckEnabled(bool on);

  // Open an Explorer window with `file` selected — the ManualUnzip banner's
  // re-reveal action. Safe from the UI thread.
  static void RevealInExplorer(std::wstring const& file);

 private:
  // The release a check decided to offer: everything the apply needs, captured
  // at check time so a repo that changes mid-flight cannot redirect an apply
  // the user already clicked.
  struct Offer {
    std::wstring version;  // v-less
    std::uint64_t code = 0;
    std::wstring tag;      // as minted, with the v — names the download dir
    std::wstring zipUrl;   // browser_download_url of the own-arch zip
    // The zip asset's expected SHA-256 (lowercase hex), parsed out of the SAME
    // asset object the zipUrl came from — never re-looked-up later, so a repo
    // that changes mid-flight cannot pair this hash with a different download.
    std::string digestHex;
    std::string zipName;   // the exact asset filename, names the file on disk
  };

  void WorkerLoop();
  void RunCheck();
  void RunApply();
  // Best-effort startup hygiene: drop <name>.old / <name>.old-<code> leftovers
  // next to the exe (the previous update's renamed images, deletable once
  // nothing runs them any more) and download dirs whose tag no longer outranks
  // this build.
  void CleanupStaleFiles();

  // Copy the snapshot under the lock, mutate, publish to the handler outside
  // it — the handler is never invoked with mutex_ held.
  void Mutate(std::function<void(Snapshot&)> const& fn);
  Handler HandlerCopy();
  RelaunchHandler RelaunchCopy();

  std::mutex mutex_;
  std::condition_variable cv_;
  std::thread worker_;
  bool stop_ = false;
  bool checkRequested_ = false;
  bool applyRequested_ = false;
  bool autoCheck_ = true;
  std::chrono::steady_clock::time_point nextAuto_{};
  Snapshot snapshot_;
  Offer offer_;

  // The handlers' own lock, on SdkHost's advancedMutex_ reasoning: never held
  // across an invocation, never taken together with mutex_.
  std::mutex handlerMutex_;
  Handler handler_;
  RelaunchHandler relaunch_;
};

}  // namespace urnw
