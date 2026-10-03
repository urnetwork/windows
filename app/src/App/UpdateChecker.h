// The in-app update checker (beta-distribution spec §5): finds newer beta
// releases and installs one with a verified download.
//
// The feed is the beta fork's GitHub releases (Config.h kUpdateRepo — the
// fork IS the beta channel): every green build of the beta branch publishes a
// prerelease carrying the per-arch MSIs, and Common/ReleaseSelection.h accepts
// exactly those beta-marked prereleases while drafts and other prereleases
// stay skipped.
// Poll the release list (on launch after ~30s, then every 6 hours, and on the
// two manual triggers), pick the release with Common/ReleaseSelection.h, and when
// it outranks the build's own stamped code, offer ONE click that
//
//   downloads the own-arch MSI to %LOCALAPPDATA%\URnetwork\updates\<tag>\,
//   verifies it against the asset's own SHA-256 digest, stamped by GitHub in
//     the same releases JSON the check parsed (CNG SHA-256 locally),
//   starts it with msiexec (elevated: the package is per-machine), and quits
//     the app so none of its files are held open. The MSI's MajorUpgrade
//     replaces the install and its ServiceControl stops and restarts the
//     service, so there is no second click for the service.
//
// If the installer cannot be started (the elevation prompt was declined, or
// the launch failed), the verified MSI is shown in Explorer for the user to run.
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
    Applying,       // the click fired; `stage` says how far it has got
    ManualInstall,  // downloaded + verified, but the installer could not be
                    // started (elevation declined, launch failed): the MSI
                    // was revealed in Explorer and the user finishes
    Failed,         // the last apply attempt failed; `failure` says where.
                    // Nothing was installed — the click retries from scratch.
  };
  enum class Stage { Idle, Downloading, Verifying, Installing };
  enum class Failure { None, Download, Checksum };

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
    // ManualInstall: where the verified MSI sits, for the banner's wording and
    // its re-reveal action.
    std::wstring installerPath;
    CheckOutcome lastCheck = CheckOutcome::NeverRan;
    // The newest release tag the last completed check parsed, whether or not
    // it outranks this build — the developer line names it either way.
    std::wstring newestVersion;
    std::uint64_t newestCode = 0;
  };

  using Handler = std::function<void(Snapshot const&)>;
  // Fired on the WORKER thread once the installer is running. The receiver
  // quits the app (the ordinary tray-quit teardown) so the MSI finds none of
  // the app's files in use; only the app side knows how to tear itself down.
  using InstallerStartedHandler = std::function<void()>;

  UpdateChecker() = default;
  ~UpdateChecker();
  UpdateChecker(UpdateChecker const&) = delete;
  UpdateChecker& operator=(UpdateChecker const&) = delete;

  // Spawn the worker: stale-file cleanup first (best-effort .old removal and
  // obsolete download dirs), then the launch-delay check and the 6h cadence.
  void Start();
  // Signal and JOIN the worker. A download in flight notices within one read
  // (the fetch loop polls the stop flag), so this is bounded, not "until the
  // whole MSI finishes".
  void Stop();

  Snapshot Current();
  // Store only — never invokes. Bind, then replay Current() yourself: the main
  // window is built on the first tray click, which can be minutes after the
  // launch check already ran.
  void SetHandler(Handler h);
  void SetInstallerStartedHandler(InstallerStartedHandler h);

  // Queue a check now (the developer screen's trigger). Coalesces with a check
  // already queued; ignored only after Stop().
  void CheckNow();
  // Queue the download/verify/install for the currently offered release.
  // Ignored when nothing is offered or an apply is already running.
  void BeginApply();

  // The "Check for updates automatically" preference (Settings): persisted in
  // app_prefs.json beside Advanced Mode, default ON. The static read exists so
  // the Settings row can seed itself without reaching the instance.
  static bool AutoCheckEnabled();
  // Persist + apply. Turning it ON schedules a check right away — the user
  // just asked for updates, so "in six hours" would be a strange answer.
  void SetAutoCheckEnabled(bool on);

  // Open an Explorer window with `file` selected — the ManualInstall banner's
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
    std::wstring msiUrl;   // browser_download_url of the own-arch MSI
    // The MSI asset's expected SHA-256 (lowercase hex), parsed out of the SAME
    // asset object the msiUrl came from — never re-looked-up later, so a repo
    // that changes mid-flight cannot pair this hash with a different download.
    std::string digestHex;
    std::string msiName;   // the exact asset filename, names the file on disk
  };

  void WorkerLoop();
  void RunCheck();
  void RunApply();
  // Best-effort startup hygiene: drop <name>.old / <name>.old-<code> leftovers
  // next to the exe (renamed images from the portable builds' old rename-swap
  // updater) and download dirs whose tag no longer outranks this build.
  void CleanupStaleFiles();

  // Copy the snapshot under the lock, mutate, publish to the handler outside
  // it — the handler is never invoked with mutex_ held.
  void Mutate(std::function<void(Snapshot&)> const& fn);
  Handler HandlerCopy();
  InstallerStartedHandler InstallerStartedCopy();

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
  InstallerStartedHandler installerStarted_;
};

}  // namespace urnw
