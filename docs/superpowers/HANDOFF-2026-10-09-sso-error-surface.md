# HANDOFF — SSO error never surfaces on the login screen (2026-10-09)

**Task:** make an SSO error answer from the browser flow visibly surface on the
Windows app's login screen at any window size. Today a *matched* error callback is
processed by the SDK host and answered to the login page, yet nothing appears.

**Repo:** `C:\Users\ryanm\Downloads\claude_sandbox_message\urnetwork-windows`
(fork `Ryanmello07/urnetwork-windows`, branch `beta/custom-server`; upstream
`urnetwork/windows`, PR #4). Worktree has ONE intentional uncommitted change to
keep: `LoginPage.cpp` `SetInitialLoginError` gained `StartBringIntoView()`.

> NOTE: an agent in the Kimi session (agent-81) is already working this exact
> task from the same brief. Coordinate before editing the same worktree — two
> agents on one checkout will collide. Either let it finish, or have the Kimi
> session stop it (`Agent(resume="agent-81")` can also continue it later; its
> context survives).

## Symptom and why it matters

Browser SSO against the operator api now works end-to-end locally (see "shipped"
below); the only upstream gap left is the production vault entry, so TODAY every
real Google attempt ends with `error=not_configured` returned to the app. The
owner experienced this as "stuck": the browser's "Open URnetwork?" prompt did its
job, the app received the callback — and showed nothing. An error the user can't
see reads as a hung app.

## The wired path (all correct on paper)

1. `LoginPage::StartSsoSignIn` → `SdkHost::SignInWithSso` (SdkHost.cpp:1758):
   arms `ssoAttempt_` (state+nonce), sets `walletAuthDone_`, opens the browser
   with the authorize URL pinned to the operator api (`ids::kOperatorApiUrl`).
2. Provider → `https://api.bringyour.com/auth/<provider>/callback` → 302 to
   `urnetwork://oauth/<provider>?state=...&id_token=...|&error=...`.
3. Windows launches `URnetwork.exe "<uri>"` (HKCU-registered `urnetwork://`
   handler) → single-instance redirect → `AppController::HandleDeepLink`
   (AppController.cpp) → `SdkHost::HandleDeepLink` →
   `WalletConnect::HandleOAuthReturn` → `wallet_.on_sso` (SdkHost.cpp:1508):
   state/provider must match `ssoAttempt_`, then nonce must match, else the
   error goes to `wallet_.on_error` (1538): answers `walletAuthDone_` with
   `{false,false,err}`; if NO flow is armed it logs
   "a wallet-bridge error arrived with no flow in flight, ignoring it".
4. The answer runs LoginPage's callback (DispatcherQueue hop) →
   `ApplyWalletSignInResult` (LoginPage.cpp:1213): maps exact `not_configured`
   to friendly copy `Adv("adv_sso_not_configured", ...)` →
   `ShowLoginErrorFor(LoginStep::Initial)` (419) → `SetInitialLoginError` (409)
   → `LoginErrorText` (MainWindow.xaml:294, UrInlineErrorTextStyle TextBlock
   directly below `GetStartedButton`) + `StartBringIntoView()`.

## Live evidence (app log `%LOCALAPPDATA%\URnetwork\app\logs\urnetwork-app.log`, 2026-10-09 UTC)

- 01:16:43 + 01:17:00 — the user's two real Google completions arrived with
  `error=not_configured` and matching states; no visible error.
- 01:29:36 — a synthetic link with the then-armed state hit
  "an sso callback arrived with no sign-in in flight, ignoring it": the armed
  attempt died silently in a ~2min gap with NO "superseded" line. Unexplained.
  (`ssoAttempt_` is only set at SdkHost.cpp:1786 and reset at 1523/1578 —
  find what else cleared it or `walletAuthDone_`.)
- 01:32:37.5 + 01:32:38.6 — ONE `Start-Process` of the uri produced TWO
  "deep link received" lines ~1.1s apart (protocol-launch double delivery?).
  First matched (no mismatch warning), second warned "no sign-in in flight".
  Still nothing visible afterward → a MATCHED error callback doesn't reach the
  screen.
- 01:32:31.6 — "a wallet sign-in was superseded (superseded by a sign-in)":
  a fresh click answers the PREVIOUS attempt's callback with that reason, and
  `ApplyWalletSignInResult` shows non-empty errors — so the raw string
  "superseded by a sign-in" can flash as a login error. Second bug to fix:
  supersede answers must not surface as errors. Reference approach: archived
  branch `archive/sso-embed-sheet` (commit e5319fb) has a `settled` flag in
  LoginPage that drops superseded answers — lift the IDEA only, don't merge.

## Repro recipe (~15s, no credentials needed)

```bash
# app on the login screen (launch twice; tray app shows on 2nd):
EXE='C:\Users\ryanm\Downloads\claude_sandbox_message\urnetwork-windows\app\build\x64\Release\URnetwork.exe'
powershell -NoProfile -Command "Start-Process '$EXE'; sleep 4; Start-Process '$EXE'"
# arm an attempt (UIA click opens the real browser at Google's chooser):
powershell -NoProfile -ExecutionPolicy Bypass -File .localstate-verify/uia-click.ps1 -Name "Sign in with Google"
# read state= out of the browser omnibox (Comet/Chromium: focus the bar first):
#   UIA RootElement descendants, ControlType Edit, Name ~ "address|search",
#   SetFocus(), sleep 300ms, ValuePattern value, regex [?&]state=([^&]+)
# fire the error callback:
powershell -NoProfile -Command "Start-Process 'urnetwork://oauth/google?error=not_configured&state=<STATE>'"
# screenshot the app window:
powershell -NoProfile -ExecutionPolicy Bypass -File .localstate-verify/resize-tour.ps1 -Name <n> -PrintWindow
#   -> .localstate-verify/rs-<n>.png
```

Run the cycle TIGHTLY (click → read → fire in seconds); long gaps with focus
changes correlated with the attempt dying silently.

## Suspects (parent's list, unverified)

- The LoginPage callback's `queue.TryEnqueue` + weak self — does it run at all?
  (temp LogInfo in the callback + in the mapping branch; remove after.)
- Step visibility: `LoginErrorText` lives in the Initial step panel; if another
  step is shown or a later pass re-collapses it, nothing shows.
- Double delivery: first arrival answers+shows; does the second (or an
  Activated/focus path calling `SetInitialLoginError(hstring())`) clear it?
  Grep every `SetInitialLoginError` call site; check MainWindow.xaml.cpp's
  Activated handler (~line 278) and `LoginPage::OnWindowReactivated`.
- Does `on_error` even reach the `walletAuthDone_` branch? The "wallet-bridge
  error ... no flow in flight" warning in the log proves the drop.
- `ApplyStrings`/`ApplyBreakpoint` re-runs re-hiding the TextBlock.

## Build + harness traps (all paid for; respect them)

- Build: `cmd //c "C:\Users\ryanm\AppData\Local\Temp\b.cmd"` from the repo root
  (vcvars64 + `app\tools\build-local.ps1 -SkipDeps`). Kill any running
  `app\build\...\URnetwork.exe` first (link lock). Tail errors LNK1104
  `urnetworkd.exe` + MSB3073 `URnetworkSdk.dll` copy are known-benign service
  locks (the `urnetworkd` service runs from that dir); the App project must
  compile with ZERO errors. Retry transient C3859/C1076 OOMs.
- STALE-OBJ TRAP: any `x:Name` add/rename in MainWindow.xaml requires deleting
  `app/src/App/x64/Release/*.obj` (and agent-77 also found a second stale
  intermediate at `app/src/App/App/x64/Release/`) before building, or the app
  crashes at startup with E_INVALIDARG.
- `.localstate-verify/` (gitignored) harness: `capture.ps1` KILLS running
  URnetwork.exe by path and relaunches with isolated `URNETWORK_APP_ROOT`
  (its own log at `.localstate-verify/approot/logs/`) — expected, but it leaves
  a harness instance holding the single-instance key afterward; kill it and
  relaunch the real exe when done. `resize-tour.ps1` resizes/captures the LIVE
  window (-Width/-Height PHYSICAL px = dips × 1.25 on this 125% box;
  -PrintWindow avoids bleed-through). `uia-click.ps1 -Name` invokes by UIA name.
- Display is 2560x1600 @125%; scripts must SetThreadDpiAwarenessContext(-4).

## Shipped state (all pushed to the fork, CI green via beta-build.yml on push)

- `96e7490` login unbrick (window re-activation re-arms buttons).
- `5d14f58` wave 7 fold doors (monetization panes reparent into fold hosts).
- `d3db948` browser sign-in bridge ("Sign in with browser" → ur.io + one-time
  auth code) + `urnetwork://auth?code=` deep link + the `not_configured`→friendly
  copy mapping (the very path this task debugs).
- `fef9ed9` SSO callback pinned to the operator api (`ids::kOperatorApiUrl`,
  Ids.h): bringyour manages provider registrations centrally; the token signs
  into the ACTIVE space via /auth/login (signature+audience). Verified live:
  Google account chooser renders while pointed at beta-test.net.
- `archive/sso-embed-sheet` (e5319fb): working embedded WebView2 SSO sheet,
  shelved in favor of browser-first UX; restore by merge if wanted.
- `urnetwork://` protocol registered under HKCU → dev exe (the MSI would do
  HKLM; shadows it until removed).

## Pending upstream (owner is raising with URnetwork — NOT this task)

1. Google vault `google.yml` → `sign_in_oauth: {client_id, client_secret}` on
   the production api (redirect `https://api.bringyour.com/auth/google/callback`
   is already registered at Google).
2. Apple Developer console: Services ID `network.ur.service` add return URL
   `https://api.bringyour.com/auth/apple/callback`.
3. Nothing at all is needed on beta-test.net after `fef9ed9`.

## Lanes and house rules

- Protocol repos (`connect`, `sdk`, `server`, `mmm`, ...) belong to another
  agent — do not touch. This lane is the Windows app UI/frontend + bugs only.
- Commits: `git -c user.name="Ryanmello07" -c user.email="67509637+Ryanmello07@users.noreply.github.com"`;
  push to origin after verified waves; PR #4 gets a summary comment.
- Comments carry rule+reason; no new .resw keys (existing keys or
  `pages::Adv("adv_*", L"...")` fallback); palette: action blue #638BFC, lime
  #EFF7BB = earnings/premium only, urGreen tunnelled, urCoral blocked, amber
  bypassed/warning.
- CI: beta-build.yml auto-runs on push (publishes the public prerelease);
  build-and-test (workflow id 333873908) does NOT auto-run — dispatch:
  `gh workflow run 333873908 --repo Ryanmello07/urnetwork-windows --ref beta/custom-server`.

## After the fix

Commit + push, dispatch CI, PR #4 comment. Then the queued work: user sign-in
test (email or browser bridge), Wave 7 drag-resize test on Earnings/Account,
and Wave 8 (live traffic view) per `docs/superpowers/2026-10-06-traffic-view-recon.md`.
