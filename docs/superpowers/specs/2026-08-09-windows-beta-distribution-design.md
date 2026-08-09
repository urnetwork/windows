# Windows beta distribution: portable zip, release pipeline, auto-update

2026-08-09. Approved in conversation; owner delegated implementation.

## Goal

Ship a **portable zip** as the beta v1 artifact, published as GitHub prereleases
tagged exactly the way `urnetwork/android` tags its releases, with an in-app
service setup (one click, one UAC prompt) and an in-app update checker that
finds new releases and applies them. Prove the **MSI compiles** but do not
polish it — the MSI becomes the end-user artifact in a later milestone, when a
signing cert exists.

## Non-goals (this milestone)

- MSI polish: signing, runtime bundling, Store submission, MSI upgrade-ordering.
- The split-tunnel driver (separate milestone; gated on task #23's written
  answer from Microsoft about attestation signing).
- Delta updates, update channels, rollback UI.
- ARM64 live verification (we build and attach it, labeled untested).

## Context (what already exists)

- `urnetworkd install` / `uninstall` verbs in `app/src/Service/main.cpp`
  (SCM registration, auto-start). `install` does **not** start the service and
  is **not** idempotent — both change here.
- `app/installer/Package.wxs` + `Installer.wixproj` (WiX v5.0.2, `dotnet
  build`), never compiled, excluded from the solution. Installs app + service +
  wintun + `urnetwork://` protocol handler.
- `.github/workflows/beta-build.yml` **already derives the android-format
  version** (`v<YYYY.M.D>-<code>[-beta]`, code = seconds since 2023-05-23
  founding x 10, computed from the run's `created_at` so all jobs agree). It
  currently feeds only the SDK build. No release job, no zips.
- Neither `URnetwork.exe` nor `urnetworkd.exe` carries a VERSIONINFO resource.
- Local inner loop: `app/tools/build-local.ps1` (x64 Release into
  `app/build/x64/Release`), `urnetworkd.exe selftest` (unelevated, 167 checks).

## Design

### 1. Version stamping (foundation for everything else)

CI passes the derived version into the app build:
`/p:UrVersion=2026.8.9-<code>-beta /p:UrVersionCode=<code>`.

- `app/Directory.Build.props` defaults `UrVersion=0.0.0-dev`,
  `UrVersionCode=0` when the properties are absent, and turns them into
  preprocessor definitions for both ClCompile and ResourceCompile.
- Both exes gain a VERSIONINFO resource: numeric `FILEVERSION` from the date
  parts (`2026,8,9,0` — each fits 16 bits), full version string in
  `ProductVersion`/`FileVersion` strings, and the numeric code retrievable via
  `VerQueryValue` (`ProductVersion` string carries the full grammar; the code
  is parsed from it).
- The app shows the version in Settings/About and the developer screen.
- **`0.0.0-dev` (code 0) disables the update checker.** Dev builds never
  self-update.
- Release artifacts build **self-contained** (`WindowsAppSDKSelfContained`)
  so a clean machine needs no preinstalled Windows App Runtime. Set it
  unconditionally in the build for dev/CI consistency.

### 2. Portable zip

Per arch: `URnetwork-v<version>-windows-<x64|arm64>-portable.zip` containing
the build output (app, service, SDK dll, wintun, resources.pri, self-contained
runtime), `THIRD-PARTY-NOTICES.txt`, the wintun license, and a one-page ASCII
`README.txt`: what it is, first run (open `URnetwork.exe`; the app offers to
set up the VPN service), updating, uninstalling (Settings, or elevated
`urnetworkd uninstall`), and recovery ("if the internet ever breaks: stop
urnetworkd, then elevated `urnetworkd revert`").

"Portable" means **no installer** — not roaming data. App data stays in
`%LOCALAPPDATA%\URnetwork`, service data in `ProgramData\URnetwork`, as today.

### 3. In-app service manager

New app-side component (`ServiceSetup`) that classifies the service state via
read-only SCM queries (no elevation):

| State | Meaning | UI |
|---|---|---|
| `NotInstalled` | no `urnetworkd` service registered | Connect banner: "Set up the VPN service" |
| `Stopped` | registered, not running | same banner, "Start" wording |
| `Running` | registered and running | nothing |
| `VersionMismatch` | installed service exe's version != the `urnetworkd.exe` next to the app | banner: "Update the VPN service" |
| `ConsoleMode` | control pipe alive but no SCM service (developer console run) | nothing — do not interfere |

One click fires **one** UAC prompt: `ShellExecuteEx` with `runas` on the
sibling `urnetworkd.exe install`. The `install` verb becomes **idempotent and
starting**: service exists -> stop it, re-point binPath at the invoking exe,
start; not registered -> create + start. The app polls the SCM until Running
(bounded), then refreshes. UAC declined -> banner stays, calm inline notice.
Settings gains "Uninstall VPN service" (elevated `uninstall`, which already
exists). VersionMismatch compares the *file version resource* of the installed
service's binPath exe against the sibling exe's — both stamped by (1).

### 4. Release pipeline (android parity)

Extend `beta-build.yml`:

- The existing derive step's outputs flow to the app build jobs (job outputs)
  and into msbuild as `/p:UrVersion /p:UrVersionCode`.
- Each matrix job stages and uploads the portable zip for its arch.
- A `release` job (after both arches): downloads artifacts, writes
  `SHA256SUMS`, deletes any same-tag release, then
  `gh release create v<version> --prerelease` attaching both zips, SHA256SUMS,
  and the MSI. Notes line mirrors android's ("beta build riding the official
  version numbering...") plus "MSI attached is unsigned and untested — use the
  portable zip".
- Every green build of `beta/custom-server` publishes a prerelease, exactly
  like android.

### 5. Auto-update

New app-side component (`UpdateChecker`):

- On launch and every 6 h, GET
  `https://api.github.com/repos/Ryanmello07/urnetwork-windows/releases?per_page=15`
  (unauthenticated; WinHTTP), parse tags matching the grammar, take the
  highest code. Repo is a constant in `Config.h` (upstream handoff = one-line
  change later).
- Newer than own stamped code -> banner: "Update available: v<version>".
  One click: download own-arch zip to `%LOCALAPPDATA%\URnetwork\updates\`,
  verify against the release's `SHA256SUMS`, extract (`tar.exe`, ships with
  Windows 10 1803+), then **rename-swap**: for each payload file, rename the
  existing file to `<name>.old` (NTFS allows renaming running images —
  including the running service exe), move the new file in, relaunch the app.
  `.old` files are cleaned on next launch.
- The service keeps running the old (renamed) binary until the
  `VersionMismatch` banner's one-click elevated `install` restarts it onto the
  new exe. Two clicks per update, one elevation.
- Fallbacks: app folder not user-writable (someone unzipped into Program
  Files) -> "update downloaded — unzip it yourself", reveal the file.
  Download/hash/extract failure -> banner persists with the error, nothing
  half-swapped (stage fully in `updates\`, swap only after a complete verified
  extract).
- Settings toggle "Check for updates automatically", default on.
- Honest limit, stated in README too: SHA256SUMS from the same origin protects
  download integrity, not against repo compromise. Real signing arrives with
  the MSI milestone.

### 6. MSI compile-proof

CI builds `Installer.wixproj` (x64) with `dotnet build`, `BinDir` at the
staged build output, `-p:` version wired as `yy.M.d` (MSI ProductVersion
fields are 16-bit-bounded; the real upgrade-ordering scheme is the MSI
milestone's problem). Unsigned MSI uploaded and attached to the release,
labeled untested. `Package.wxs` gets the minimal fixes it needs to actually
compile (it has never been through the compiler).

### 7. Verification gates

- **Selftest** stays green; new checks cover the install verb's idempotent
  logic where testable unelevated.
- **Local build gate**: full x64 Release build + selftest after each package.
- **Live gate (owner, elevated)**: unzip a release zip -> open app -> banner ->
  one-click setup (UAC) -> Connect -> real traffic -> cut the next release ->
  checker finds it -> update applies -> service banner -> one-click service
  update -> Connect again.
- **Pipeline gate**: a real prerelease appears on the fork with both zips +
  SHA256SUMS + MSI, tag matching android's grammar.

## Sequencing after this ships

Beta v1 release -> split tunneling (task #24 phase 0 first; task #23 gates any
driver work) -> feature train (post-beta).

## Implementation packages

| Pkg | Scope | Files (primary) |
|---|---|---|
| A | version stamping, VERSIONINFO, self-contained, About | `Directory.Build.props`, `App.rc`, new `Service .rc`, `SettingsPage`, `DeveloperPage` |
| B | idempotent+starting `install` verb, selftest | `Service/main.cpp`, `Service/SelfTest.cpp` |
| C | ServiceSetup component + banners + uninstall | new `App/ServiceSetup.{h,cpp}`, `ConnectPage`, `SettingsPage` |
| D | UpdateChecker + swap + relaunch + toggle | new `App/UpdateChecker.{h,cpp}`, `SettingsPage`, shell banner |
| E | zips, README, release job, version plumbing | `beta-build.yml`, new `app/tools/package-portable.ps1` |
| F | MSI compile-proof | `Installer.wixproj`, `Package.wxs`, `beta-build.yml` (with E) |

A and B are independent. C needs A+B. D needs A. E/F touch only CI/packaging
files and run parallel to C/D; one agent owns `beta-build.yml`.
