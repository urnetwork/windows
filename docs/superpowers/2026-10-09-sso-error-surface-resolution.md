# RESOLUTION — the SSO error that never surfaced (2026-10-09)

Closes `HANDOFF-2026-10-09-sso-error-surface.md`. Read this before touching the
browser sign-in return path again; the handoff's suspect list is mostly retired
below.

## Outcome

After the browser returns (`urnetwork://oauth/<provider>?...`), the app's window
now comes in front of the browser by itself, with the sign-in result or error on
screen, whether the window was visible, hidden to the tray or minimized. Commits
`f8c3997` (error line, supersede, mapping) and `dec4d14` (foreground); later
commits correct the mechanism wording and add logging and contracts.

State of the providers on Windows today: **Sign in with Apple works end to end**
(a real sign-in, verified live 2026-10-09); **Sign in with Google still cannot
succeed** because production's vault lacks `sign_in_oauth` (server side), and now
fails visibly instead of looking hung.

## Root cause (measured on the live desktop)

Not the callback, not the delivery. **The window never came to the front, and
nothing asked Windows to bring it there.** `Window::Activate()` is
`ShowWindow` + `UpdateWindow` + `SetActiveWindow`: it never requests the
foreground, so it cannot lift a visible window that another process (the browser)
covers, whatever rights this process holds. The error was set, laid out and on
screen *inside a window the browser covered*. (An earlier draft of this note said
Windows' foreground lock "refused" `Activate()` because nobody handed the process
the right. That was wrong: the Windows App SDK's `RedirectActivationToAsync`
already passes the right to the running instance - `AppInstance::QueueRequest`
calls `AllowSetForegroundWindow` - and the WinUI binaries import
`SetActiveWindow` but never `SetForegroundWindow`.)

| flow (500x600, the shell minimum) | before | after |
| --- | --- | --- |
| real browser handoff (owner clicked through) | 0/3 probe points visible for 8 s, until raised by hand | in front at +74 ms |
| `Start-Process` of the uri, browser in front | 0/3 for the whole 6 s | in front at +363 ms |
| callback while hidden to the tray | not measured | in front at +194 ms |
| callback while minimized | not measured | in front at +198 ms |

A second, smaller cause stacks on it only at or near the window minimum: at
500x600 `LoginErrorText` is the last row of a scrolling page and sat below the
fold (fixed by the scroll-into-view tick in `SetInitialLoginError`; the row fits
without scrolling from roughly 700 px of outer height at 125% scale). **The
500x600 in the log is harness-written**: `resize-tour.ps1` hard-codes
`SetWindowPos(60, 40, ...)`, and the log shows `restored placement 1000x800` four
seconds before `saved placement 500x600 at (60,40)`. The owner's last pre-harness
window was 1000x800, where the row fits, so occlusion alone explains the original
report. The 01:16 completions in the handoff are not evidence of what the owner
saw (a maximized browser and a harness-positioned window; who clicked is
unknown); the first provably owner-driven handoffs are the ones measured above.

The fix, pinned by `tests/foreground_handoff_test.go`:

- **`shell::RaiseToFront`, called after `Activate()`, is the operative half.**
  `SetForegroundWindow` first (it succeeds after any launch that held the
  foreground right, which the SDK's redirect passes on); when Windows refuses it,
  a topmost toggle lifts the window above the others without taking focus
  (z-order is not foreground-locked).
- The second launch also calls `AllowSetForegroundWindow(primary.ProcessId())`
  before it redirects. That is **defensive**: the SDK already does it. Keep it for
  its log line - `handed the foreground right` vs `no foreground right (error 5)`
  is the only record of whether the launch held a right.

Both branches were exercised live (500x600): a launch that held a right (a real
browser click, and scripted launches from the agent's own chain, which descend
from the foreground terminal) takes the `SetForegroundWindow` path; a launch with
no foreground ancestry (a one-shot Task Scheduler task, `dl-front-norights.ps1`)
logged `no foreground right to hand ... (error 5)`, then the primary logged
`the foreground lock refused the window`, and the toggle put the window in front
in 184 ms **while the browser kept the focus**.

Caveats: "in front" is not "unobscured" - another process's always-on-top window
or a different virtual desktop can still cover the app, and nothing raises it a
second time. Only Chromium (Comet) was observed as the launcher.

## Why every earlier check said "visible"

`.localstate-verify/resize-tour.ps1` calls `SetForegroundWindow` before each
capture and captures with `PrintWindow`, which draws a covered window exactly as
well as an uncovered one. Every "the error is visible" screenshot came from a
window the harness had itself forced to the front. **Do not use that harness to
claim a window is visible to a user.**

Observe-only probes now live beside it (`.localstate-verify/dl-*.ps1`, gitignored):

- `dl-front-synthetic.ps1` arms a Google attempt (UIA invoke), reads `state=` from
  the browser, fires the callback, and records foreground owner, z-order and
  how many of three probe points show the app (`WindowFromPoint`), plus real
  `CopyFromScreen` pixels. It asserts "in front within 2.5 s, unaided".
- `dl-front-away.ps1 -Mode hide|minimize` does the same with the window away.
- `dl-front-norights.ps1` fires the callback from a one-shot scheduled task (a
  launch with no foreground ancestry), the only way to exercise the fallback.
  A plain `Start-Process` from the agent's chain always holds a transient right.
  Judge every probe on the FIRST sample after `deep link received`: an unrelated
  window (a chat notification) can take the foreground a moment later.
- `dl-watch-real.ps1` is passive: start it, then have the owner do the real
  browser handoff; it records the same signals from the moment the deep link lands.
- `dl-supersede.ps1`, `dl-replay.ps1` cover the two page-level behaviors.

## How the other platforms avoid this

macOS (`apple` repo, `NetworkApp.swift`) calls
`NSApplication.shared.activate(ignoringOtherApps: true)` when it shows its window;
Android's OS brings the activity forward on the `ur://` return. Windows had
neither. The sign-in contract itself is identical across Windows, the macOS
direct-download build (`BrowserSso.swift`), Linux and Android's no-Play-services
flavor: same Google client id, same redirect (`<api>/auth/google/callback`),
same `state` with a `platform` claim (the server maps `windows` to `urnetwork`).
Windows is not doing SSO differently.

## What is still outside the app

- **Google cannot succeed in production yet.** The server answers
  `error=not_configured` while `google.yml` has no `sign_in_oauth` section; the
  server's own ops notes (`server/monitor/SIGNALS.md`, the 2026-09-08 audit) say
  production lacks it, and the real handoffs on 2026-10-08 still came back
  `not_configured`. The owner is raising that with URnetwork. Native Android/iOS and the ur.io
  website sign in with Google without that server secret (the website asks Google
  for the identity token directly), which is why only the desktop code flow hits it.
- **Apple works, end to end (verified live 2026-10-09).** The owner pressed
  Sign in with Apple, Apple's page loaded and asked "Do you want to continue
  using URnetwork with your Apple Account ...?", and after Continue the log shows
  the whole chain: `apple sign-in armed` -> the browser's handoff carrying
  Apple's code and token -> `shell: the window is the foreground window` ->
  `an sso callback matched the apple sign-in in flight` (state and nonce
  verified) -> `signed in; no session started` 2.6 s later -> the login pills
  gone and the home shell showing. So the handoff's "add the return URL
  `https://api.bringyour.com/auth/apple/callback` to the Apple Services ID" item
  is **unnecessary - do not ask URnetwork for it** (it is registered, and
  `/auth/login` accepts the Services-ID audience). This was also the first live
  pass of the SUCCESS path through the window raise.
- **Only Google needs URnetwork.** The error copy ("Provider sign-in isn't
  available on this network yet - use email or browser sign-in") predates this and
  does not mention that Apple works; `not_configured` only ever comes from the
  Google callback.

## Myths retired

- *"One `Start-Process` produced two deliveries."* A real browser handoff delivers
  **once** (measured). The pairs in the log are two separate launches (a script
  and/or a human firing twice).
- *"The armed attempt died silently."* No code path does that: `ssoAttempt_` is
  armed in `SignInWithSso`, consumed on a match, reset by
  `CancelPendingWalletFlows`. The cases seen were an app restart and an already
  consumed state. An attempt armed for 4 min 42 s matched fine.
- *"on_error may not reach the `walletAuthDone_` branch."* It does; the
  "no flow in flight" warning has never been logged.

## Logging added so this is readable from the log alone

`sdkhost: <provider> sign-in armed`; a warning when a pending attempt is dropped
(`CancelPendingWalletFlows`); `login: dropped a superseded sign-in answer`;
`login: sign-in error line shown on the initial step` (never the text); the
refusal line names who owned the foreground; and the startup `executable` line
now carries the exe's write time, because the `build` line is `__DATE__ __TIME__`
of one translation unit that an incremental build does not recompile (it read
`Oct 8 2026 18:13:01` for every build that day).

## Known latent hazard (not fixed, not reproduced)

`ssoAttempt_` is cleared only by a match or by `CancelPendingWalletFlows`; neither
a completed email/instant sign-in nor Logout clears it. Click Google, leave the
tab, sign in another way, then finish the Google tab: the return matches,
`on_error` finds `walletAuthDone_` still set and pushes `AuthState::Error` while
the SDK session is signed in, flipping the UI to the login screen. Today every
Google attempt ends in `not_configured`, so this is reachable. A guard belongs at
the `on_sso` / `on_error` entry points (UI thread), not in `RegisterNetworkClient`
or Logout (SDK threads writing UI-thread members). Needs a throwaway sign-in to
reproduce.

## Cheap follow-ups (not done)

- A muted "Waiting for your browser..." line while an attempt is in flight (today
  the only sign is the greyed pills), via `pages::Adv`.
- Friendly copy for `access_denied` / Apple's `user_cancelled_authorize`
  (Android and iOS show the provider text raw too).
- A stale tab's return (state does not match) is dropped silently, as on iOS and
  Android; the window now comes forward but shows nothing new.
