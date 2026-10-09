# Merging upstream/main into beta/custom-server (PR #4), 2026-10-09

PR #4 (`beta/custom-server` into `urnetwork/windows` main) had fallen behind: the
merge base was b15f0b6 (2026-09-30) and upstream main was ef87934, 260 commits
ahead. This records how the merge was resolved, what it cost the beta line, how it
was verified, and what is still open. It is written so the next merge can start
from it.

## What upstream changed that mattered

- **The updater was rebuilt.** An elevated `URnetworkUpdate.exe`, an
  official-feed-only release selection (`Common/ReleaseSelection.h`,
  `kOfficialFeed`, addressed by the repository's numeric id, immutable releases
  only), protocol v3 between the app and `urnetworkd`, and an MSI that compares
  file versions on upgrade.
- **Version numbers come from one script.** `app/tools/UrVersion.ps1` derives the
  VERSIONINFO file version, the MSI ProductVersion and the file version the MSI's
  upgrade rule compares, all from the instant the release code names.
- **The GitHub workflows were removed**, and so were guest mode and Seeker
  verification.
- **A localization contract arrived.** Every string id the app looks up must be in
  the generated neutral catalog and tagged `windows` in the store
  (`tests/catalog_lookup_test.go`, `Strings/windows-keys.txt`).

## Decisions

| Area | Decision | Consequence |
| --- | --- | --- |
| Updater feed | Upstream's design wins. The beta fork's prerelease feed (`Config.h kUpdateRepo`, the beta marker in `ReleaseSelection.h`, the beta cases in `update-release-tests.cpp` and `update_release_test.go`) is dropped. | A beta build no longer updates itself from this fork's prereleases; it updates to the official release or not at all. Testers install betas by hand. The README says so. A beta or developer feed can be added back as a second `Feed` row if wanted. |
| `build-and-test.yml` | Deleted, as upstream deleted it. | No test job runs on this fork. The Go suite runs by hand (see below). Restore with `git checkout b6e0d6a -- .github/workflows/build-and-test.yml` if a test job is wanted. |
| `beta-build.yml` | Kept. It now calls `tools/UrVersion.ps1` for the msbuild arguments and the WiX arguments instead of deriving the numbers in bash. | The beta's file version and MSI version follow the same rule as a release, so a beta orders correctly against one. Without `UrFileVersion` the MSI's upgrade rule would have compared against 0.0.0.0. |
| Guest mode, Seeker | Removed with upstream. The Seeker card, its fold door and the guest-upgrade accessors on `LoginPage` are gone. | The Earnings fold door now moves three blocks, not four. |
| Locations | The beta line's key-based list keeps its structure and takes upstream's `PeerConnectLocation` for peer rows. | `NetworkPage::ConnectFromListKey` connects peers through it, so `peer-location-tests.cpp` checks that function instead of `NetworkPage::Render`. |
| Sign-in | The SSO error surface work stays on top of upstream's Bittensor wallet chooser. The foreground right is now handed over in `Launcher::Redirect`. | `tests/foreground_handoff_test.go` pins `Launcher::Redirect`. |
| Settings | The Support pane keeps the beta layout. The protocol row launches through an awaited `OpenProtocolPage`, and the send button label goes through upstream's `ApplyFeedbackSendButton`. | Upstream's "every launch is observed" contract holds. |
| Catalog | `door`, the Connect page's fold-door helper, is registered in `catalogLookupFunctionKinds`. The group row's connection count uses the store's plural key `adv_connection_count`. | See "Open" below for what still needs the store. |

## Verification

- `msbuild URnetwork.sln` Release|x64: 0 errors (Common, urnetworkd, URnetwork,
  URnetworkUpdate). Built to a side output directory so a running instance is not
  touched. The merged tree needs an SDK newer than the one vendored on 2026-09-25;
  the SDK zip from the last green `beta-build.yml` run was used.
- Go contract suite on an LF copy of the tree, with a C++20 compiler (llvm-mingw
  behind a `c++` shim), the vendored SDK headers and nlohmann/json: 484 pass, 3
  fail. Two are the catalog tests (open, below).
  - `TestInstanceHandoverRejectsARelaunchThatDoesNotWaitOutTheUpdate` fails the
    same way on pristine upstream main: clang's `-Werror=unused-variable` rejects a
    mutated copy of `InstanceHandover.h` that GCC accepts.
  - A Windows working tree has CRLF line endings, which the source-text contracts
    do not expect: run them on an LF copy.
- `beta-build.yml` on the merge commit (`a18ab6c`): the SDK job, the x64 and ARM64
  app jobs, the MSI build and its payload check, and the prerelease job all passed.
  The published beta (`v2026.10.9-1067413860-beta`) carries the numbers
  `UrVersion.ps1` derives from its code: numeric FILEVERSION `2026.10.9.18693` on
  `URnetwork.exe`, `urnetworkd.exe` and `URnetworkUpdate.exe`, and MSI
  ProductVersion `26.10.17270`.
- Not yet run: a live launch of the merged build. It needs the merged
  `urnetworkd.exe` swapped in for the running service.

## Open

1. **The localization contract.** Forty-five keys the beta line looks up are not
   tagged `windows` in the store, and sixteen of those are not in the store at all.
   - Twenty-nine exist in the store tagged `linux` (and a few other platforms) with
     all 28 translations and the same English: they need `- windows` under
     `platforms:` and nothing else. The catalogs already carry them, so nothing
     breaks at runtime; only `Strings/windows-keys.txt` has to list them. (The
     group row's connection count was one of them: it now reads the store's plural
     key instead of a two-key English fallback.)
   - Sixteen are new: `adv_sign_in_browser`, `adv_browser_sign_in_title`,
     `adv_browser_sign_in_body`, `adv_open_ur_io`, `adv_sso_not_configured`,
     `adv_app_rules_note`, `adv_configured`, `adv_earnings_actions`,
     `dev_reliability_overrides`, `upd_manual_note`, `upd_state_not_checked`,
     `upd_state_checking`, `upd_state_current`, `upd_state_available`,
     `upd_state_dev_build`, `upd_state_failed`. They need a store entry with all 28
     locales before the catalogs are regenerated.
   - Three lines of `Strings/en/Resources.resw` were edited by hand in this PR
     (the Daily Data Balance label without its colon, and "Windows service" in the
     two service-removal strings). The store still says otherwise, so a regeneration
     reverts them until the store carries the Windows wording.
2. **The beta update path** (see the decisions table).
3. **ARM64** was not built locally; `beta-build.yml` builds it.

## Repeating this

- Build without disturbing a running instance:
  `.localstate-verify/build-sln-verify.ps1` (local, git-ignored) redirects `OutDir`.
- Go suite: copy the tracked files with LF endings into a scratch directory, put a
  `c++` on `PATH`, and point `URNETWORK_SDK_INCLUDE` at
  `app/third_party/urnetwork-sdk/amd64` and `URNETWORK_JSON_INCLUDE` at
  `app/third_party/vendor-include`.
- `git merge upstream/main` (not a rebase, per the earlier merges), then
  `git diff <merge-base> <ours>` and `<merge-base> <theirs>` against the result to
  list every added line either side lost.
