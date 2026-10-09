# Website approval sign-in for the Windows app: plan

Plan of 2026-10-09. Status: proposal, nothing described here is built. Audience: the repo owner and the URnetwork team (server, website, apps).

Evidence tags: **[V]** read in source by the plan author at the cited line; **[L]** observed live on 2026-10-09 with plain unauthenticated GET requests; **[R]** reported by the research notes behind this plan and not re-read; **[A]** assumption, or not verified. Windows lines are from the branch as it was just before the 2026-10-09 merge of upstream main (`b6e0d6a`; the merge, `a18ab6c`, changed `LoginPage`, `AuthSheets`, `MainWindow`, `AccountPage` and `Config.h`), and must be re-pinned before work starts. Server lines are urnetwork/server main of 2026-10-08; Android and iOS lines come from shallow clones of their public repos. The website repository is internal, so this plan says what the site should do and never how it does it today. Findings about production systems outside this repository belong at https://ur.io/vdp or security@ur.io, not in this file.

## 1. The short answer

**Does SSO code already exist?** Pieces do; the thing the owner described does not. Nothing lets a person who is signed in at ur.io approve a sign-in that the desktop app asked for. Everything such an approval needs already runs in production (mint a one-use code, redeem it, accept it in the app) except the approval page and the app flow that starts it. So we build a small first-party page at `ur.io/app/approve` and a "Continue with ur.io" action in the Windows app. Shipping it needs no server change; making it the default needs one (4.4).

| Piece | Where | State | Usable today? |
| --- | --- | --- | --- |
| "Create auth code" button, code box and Copy (the owner's screenshot); the account menu has "Copy Auth Code" | ur.io, Account tools | Live. 1 use and 5 minutes from Settings, 1 minute from the menu [R] | Yes, by hand: copy, then paste in the app |
| `POST /auth/code-create` | server `api/api.go:106`, `model/auth_model.go:1706-1819` | Live [V]. Needs a signed-in network credential. Default 1 use and 1 minute, max 100 uses and 24 hours, 500 active codes per network (`:1677-1681`). The code is 512 random bytes, 684 URL-safe characters (`:1769-1774`) | Yes. The page will call it |
| `POST /auth/code-login` | `api/api.go:107`, `model/auth_model.go:1973-2145` | Live [V]. No auth. Returns the creator's 30-day network credential (`jwt/by_jwt.go:66`) with `guest_mode` false (`:2130-2137`). Every failure is the same message, "Invalid auth code." (`:2006`, `:2067`), returned as HTTP 200 [R] | Yes |
| "Sign in with browser" sheet | Windows `LoginPage.cpp:1065-1139` | Built [V]. Opens bare `https://ur.io`, asks for a pasted code, discards the launch result (`:1115-1116`) | Yes. It stays as the fallback |
| `urnetwork://auth?code=` deep link | `AppController.cpp:60-67`, `:826-861` | Built [V]. A consumer only: no state, no attempt, ignored when signed in. Nothing in this repo or the Android and iOS clones produces it [R] | Unsafe as written (below). Fixed first, Phase 0 |
| `urnetwork://` handler registration | `app/installer/Package.wxs:218-229` | Per-machine MSI only [V]. The portable zip, the supported artifact (`README.md:17-18`), registers nothing, and no code in `app/src` writes the scheme | Not for portable users until 5.3 |
| Google and Apple return `urnetwork://oauth/<provider>` | `SdkHost.cpp:1775-1814`, `:1508-1548` | Built [V]: per-attempt state and nonce, a mismatch is refused. Apple works end to end; Google waits on an upstream vault change (`docs/superpowers/2026-10-09-sso-error-surface-resolution.md:15-18`) | The model to copy |
| A site page that hands a result back to the app | the wallet-connect page returns through a `urnetwork://<host>` link | Live [R] | Precedent only |
| Sign in with URnetwork (OAuth 2.1 and PKCE) | server `oauth/` (routed at `api/api.go:364`); consent page `ur.io/authorize` | The server and the consent page answer [V][L]; the issuer host `auth.bringyour.com` has no address record [L]. Tokens are scoped to MCP and signed with keys kept apart from the platform session keys; no route accepts them as a session; the exchange to a platform credential is "Planned" in the maintainers' docs [R] | No |
| Device adopt flow `/device/*` | `api/api.go:281-290` | Live [V]. Returns a client token only; no shipped app calls it [R] | No (Option C, later) |
| Approval page | none | Does not exist; `/app/approve` is not served [L] | No |

**At a glance.**
- **Recommendation:** Option A, a first-party approval page at `ur.io/app/approve` plus *Continue with ur.io* in the Windows app, on the existing code-create and code-login routes.
- **Order:** Phase 0 first (close the stateless deep link and the command-line logging); then the page and the app flow in parallel; then the real-desktop matrix; server hardening and the other apps follow.
- **Effort:** about 15-20 engineer-days across the site team and the Windows lane, roughly three calendar weeks, plus the site's deploy cadence (estimates, section 6).
- **Default-on gates (4.4):** edge headers on the page, a server "sign out everywhere", the sealed return, Google and Apple sign-in working on production ur.io, and the matrix passing.

The one existing piece that is not safe to lean on is the deep link. The app accepts `urnetwork://auth?code=` with no state, so on a signed-out client any web page or local script can sign the app into the page owner's account (login CSRF; `AppController.cpp:843-857` [V]). Nothing produces that link today. Phase 0 (about two days, Windows only, no site work) closes the hole before anything else is built.

**Verified:** the server routes, limits and error behavior; the Windows deep-link, sheet, registration and logging code; that `/app/approve` is not served while `/authorize` is; that the issuer host has no address record. **Not verified:** browser behavior when a page launches a custom scheme (first-use prompt, per-browser quirks, enterprise policy); anything end to end, because nothing was run; whether the live site matches the website's main branch; the items in 8.3.

**Decisions needed before work starts** (8.1, items 1-3): whether to stop accepting the stateless link, who ships the page, and whether the app may register `urnetwork://` for the current user at runtime.

## 2. The idea, and why it fits

ur.io already issues auth codes, so add a first-party approval step on `ur.io/app`: a person who is already signed in on the site approves the desktop client's sign-in, with no typing and no pasting.

- **The pieces exist in production.** The site mints codes from the signed-in session (the Settings button in the screenshot, the account menu, and a plain REST call [R]), and the app redeems one (`LoginPage.cpp:1030-1051`, `SdkHost.cpp:686-713` [V]).
- **The browser holds what the app cannot:** the person's session, password manager, passkeys and working Google and Apple sign-in.
- **It is short.** About seven steps today (open ur.io, sign in, Account, Create auth code, Copy, switch window, Paste) become two clicks.
- **The shape has a precedent.** The api's own Apple and Google callbacks hand a result back through a per-platform scheme table and a constant path (`controller/auth_apple_oauth_controller.go:42-54` [V]), and the app already checks a per-attempt state for them (`SdkHost.cpp:1508-1548` [V]).
- **Google-account users get a desktop path** that does not wait for the api's Google vault change, provided Google sign-in on ur.io itself completes in production (a launch gate, 4.4).

Three ways to build it were compared. The efforts are rough estimates, not measurements.

| Option | What it needs | Works today? | Effort | Verdict |
| --- | --- | --- | --- | --- |
| **A. Approval bridge on ur.io/app** (this plan) | A site page and an app flow on the existing code-create and code-login | Yes. No server change to ship | 15-20 engineer-days across two teams (section 6) | Build |
| B. Sign in with URnetwork (OAuth 2.1 + PKCE), the app as a client | A first-party client and scope; an exchange from an OAuth token to a platform credential (network credential or only a client token is undecided); grant revocation; issuer DNS, TLS and routing; PKCE and a token client in the app | No. The tokens are not accepted as a session, and the app stores a network credential plus a client credential (`SdkHost.cpp:1404-1405` [V]) | About 7-9 engineer-weeks across four repos [A] | The long-term home, not v1. Keep A's attempt and state code so the swap changes only the request builder |
| C. Device code (RFC 8628 style): the app asks, any signed-in device approves | New endpoints that return a network credential. Today's adopt flow returns a client token only and skips the client-limit checks | Partly (`/device/*`) | About 3-5 engineer-weeks [A] | The later answer for headless and other-device sign-in. Worse against phishing: the credential goes to whoever polls |

## 3. Recommended design (Option A)

### 3.1 Flow

1. **Start (app).** Signed out, the person chooses *Continue with ur.io* in the existing Sign in with browser sheet. The action exists only when the active network space's link host is the official `ur.io` (`SdkHost.cpp:413`, `:1250` [V]). The app supersedes every other pending sign-in (`CancelPendingWalletFlows`, `SdkHost.cpp:1584-1609` [V]), mints a fresh `state` and a fresh X25519 keypair from the OS random source, keeps both in one in-memory attempt, makes sure the per-user `urnetwork://` handler points at the running exe (5.3), and opens the browser.
2. **Open (app to browser).** `https://ur.io/app/approve#v=1&platform=windows&state=<S>&pk=<PK>&ts=<T>`. The request is in the URL fragment, which no server, proxy or Referer header ever receives. The app shows a waiting sheet (Open again, Copy link, Cancel, a 6-character check code) and says to keep URnetwork open.
3. **Validate (page).** A boot script reads the fragment once, rejects any unknown, duplicate or reserved key before the site's sign-in logic mounts, keeps the request in one slot of the tab's session storage and rewrites the address to `/app/approve`. Nothing is interactive unless this is a top-level window with no opener.
4. **Sign in if needed (page).** The page waits until the stored session has been read and refreshed once. Signed out: a sign-in card (email and password, Google, Apple, wallet, or create a free account) using the site's existing dialog, mounted only in this phase; after sign-in the page reloads, so the consent card renders in a clean document. Then it asks the server whether the account is a guest (`GET /subscription/balance`, field `guest`, computed from the live sign-in methods rather than the token: `api/api.go:244`, `controller/subscription_controller.go:164-171`, `:240` [V]) and refuses guests. A failed lookup means "try again", never "not a guest".
5. **Consent (page).** The card names the app and platform, "this computer", the network that will be signed in, and the same check code the app shows (first 6 base32 characters of SHA-256 of `state`). Cancel comes first in tab order. Approve is not autofocused and ignores activation for about 800 ms after the card is visible.
6. **Approve (page).** On a trusted click in a top-level window, single-flight, the page re-reads the stored session and compares it with the identity the card displayed; on any difference it mints nothing and re-renders. Otherwise it calls `POST /auth/code-create {uses: 1, duration_minutes: 2}` with exactly that session, seals `{"c": code, "s": state}` to `pk` with an ephemeral key (NaCl box) and navigates to `urnetwork://auth?state=<S>&epk=<..>&n=<..>&d=<..>`. The code never reaches the DOM, web storage, the URL, logs or analytics. Other tabs holding the same request are told "approved" for 60 seconds through a marker keyed by a hash of the state, never the code.
7. **Launch (OS).** On first use the browser may ask "Open URnetwork?". Windows starts `URnetwork.exe "<url>"`; the second process hands the activation to the running instance (15 s timeout, `main.cpp:54`; `App.xaml.cpp:105-126` [V]) and exits.
8. **Gate (app).** In this order: parse strictly; signed in or authenticating, ignore; no live attempt, stray; different state, stray; equal state, consume the attempt, raise the window, then show `access_denied` or unseal. A stray (a return the app is not waiting for) never raises the window, never resets the screen and never disturbs a live attempt; signed out with no live attempt it sets one neutral line (5.3). A failed unseal, or an inner state that differs, is a stray that leaves the attempt alive.
9. **Confirm and register (app).** `code-login` returns the network credential. Before registering the device or persisting anything, the app reads `network_name` from it and asks "Sign in to {network}?". No: discard the credential and register nothing. Yes: register (`/network/auth-client`), persist, and say "Signed in as {network}".
10. **Failures.** Code expired or used: "That approval expired or was already used. Try again." Registration refused after the code is spent (client limit or plan gate; the code is consumed first, `SdkHost.cpp:1349-1436` [V]): show the server text and approve again. Timer expiry: "Browser sign-in timed out", attempt cleared, and a late approval becomes a stray.

### 3.2 Sequence

```text
Person                  App (Windows)               Page: ur.io/app/approve               ur.io API
  |                           |                               |                               |
  |-- Continue with ur.io --->|                               |                               |
  |                           | mint state S + keypair (sk, PK); arm attempt (5 min)          |
  |                           | ensure the per-user urnetwork:// handler                      |
  |                           |-- open browser (#fragment) -->|                               |
  |                           | "Waiting for approval..."     |                               |
  |                           |                               | validate, stash, strip URL    |
  |                           |                               |-- GET balance (guest?) ------>|
  |<- sign-in card if signed out; consent card ---------------|                               |
  |-- Approve (trusted click) |------------------------------>|                               |
  |                           |                               |-- POST /auth/code-create ---->|
  |                           |                               |<- auth_code C ----------------|
  |                           |                               | seal C to PK -> epk, n, d     |
  |                           |<- urnetwork://auth?... -------|                               |
  |                           | gate: live attempt, state == S; unseal; consume; raise window |
  |                           |-- POST /auth/code-login {C} --|------------------------------>|
  |                           |<- by_jwt (network credential) |-------------------------------|
  |<- Sign in to N? ----------|                               |                               |
  |-- Yes ------------------->|                               |                               |
  |                           |-- POST /network/auth-client --|------------------------------>|
  |<- Signed in as N ---------|                               |                               |
```

S = state. PK and sk = the attempt's public and secret key. C = auth code. N = network name. epk, n, d = the page's ephemeral public key, the nonce and the ciphertext.

### 3.3 URL contract (v1)

**Request**, app to page, opened in the default browser: `https://ur.io/app/approve#v=1&platform=windows&state=<S>&pk=<PK>&ts=<T>`

| Key | Set by | Rule |
| --- | --- | --- |
| `v` | app, constant `1` | Any other value: the page says "update the app" and offers no Approve. |
| `platform` | app, constant per build | Looked up in a closed table in the page. `windows` maps to the label "URnetwork for Windows" and the return base `urnetwork://auth`. An unknown value is refused. Approve is enabled only when the browser's own OS matches the row; otherwise "Open this link on the PC where you started it." |
| `state` | app, per attempt | 32 bytes from the OS random source, base64url, 43 characters, kept in one in-memory slot. A secret until consumed. The page checks its shape and echoes it unchanged; the app compares the whole string for equality. |
| `pk` | app, per attempt | The attempt's X25519 public key in base58, the encoding the wallet bridge already uses (`WalletConnect.cpp:169-182` [V]). Optional in the first beta behind the flag, required before the flag defaults on. A request with `pk` is answered sealed; one without is answered with a plain `code`. |
| `ts` | app | Unix seconds at attempt start. The page uses it only to refuse a request older than 4 minutes or more than 2 minutes ahead. The app uses its own clock. |
| anything else | none | An unknown, duplicate or reserved key (any name the site's own sign-in landing logic consumes, in any case or percent-encoded spelling) makes the whole request invalid. `redirect_link`, `redirect_uri`, `return` and `callback` are never read. |

Cut from v1 after review: a device label (attacker-controlled text on a trusted card; the card says "this computer") and `lang` (translated copy needs keys in a shared store owned by another repo; v1 is English, hard-coded). If a target browser mishandles fragments, the fallback is the same keys in the query string, stripped by the first script; edge logs would then hold `state` for its 5-minute life, which the team must accept explicitly.

**Return**, page to app, always built by the page from constants plus URL-encoded values. Scheme `urnetwork`, host `auth`, no path, port, userinfo or fragment.

| Case | URL |
| --- | --- |
| Approve, sealed | `urnetwork://auth?state=<S>&epk=<b58>&n=<b58>&d=<b58>`. `d` is a NaCl box of `{"c":"<code>","s":"<S>"}` to `pk`, about 1,050 characters |
| Approve, plain (request without `pk`, first beta only) | `urnetwork://auth?state=<S>&code=<code>`. The app refuses a plain return for an attempt that carried `pk` |
| Cancel | `urnetwork://auth?state=<S>&error=access_denied`. The only error value; anything else is a stray |

`code` comes from `POST /auth/code-create {uses: 1, duration_minutes: 2}` made with the person's own session: 684 URL-safe base64 characters from 512 random bytes (`model/auth_model.go:1769-1774` [V]). The app caps `d` at 2,048 characters and requires `state` to be exactly 43.

**Versioning.** `v=2` is reserved for a server-checked code challenge (5.1 item 4). A page and an app that disagree on `v` fail closed.

### 3.4 Allowed redirect schemes and lifetimes

The return goes to exactly one place per platform, taken from the closed table: today one row, `windows` to `urnetwork://auth`. Later rows (`macos` and `linux` to `urnetwork://auth`, `android` to `ur://auth`) are added to the page only when that app enforces the same attempt gate and the sealed return. Never a caller-supplied URL; never `https:`, `javascript:`, `data:`, `file:`, `intent:`, another host or another path. This mirrors the api's own closed table (`controller/auth_apple_oauth_controller.go:42-54` [V]).

| What | Value | Why |
| --- | --- | --- |
| Auth code | 1 use, 2 minutes | Covers the browser's first-use prompt. The server default is 1 minute and the Settings button uses 5 (`model/auth_model.go:1677-1681` [V]) |
| App attempt | 5 minutes, one slot, not extended by "Open again", checked against both the steady and the wall clock | Bounds how long a leaked `state` is usable; iOS uses 5 (`BrowserSso.swift:61` [V]). Windows' Google and Apple attempt has no timeout today (`SdkHost.h:2099-2108` [V]) |
| Page staleness | 4 minutes from `ts`, checked at load and again at Approve | The page always gives up before the app does |
| Page's copy of the sealed URL | dropped 60 seconds after the first launch attempt | A further launch needs a fresh Approve |
| Mints | at most 3 per state per 10 minutes, per tab | Spares the 500-code cap, which counts expired rows |

### 3.5 Consent screen wording (English, hard-coded in v1)

- **Title:** *Sign in to URnetwork for Windows?*
- **Body:** *URnetwork for Windows on this computer is asking to sign in to your network {network}. Check code: {K}. Approve only if it matches the code shown in URnetwork.*
- **What approval grants:** *If you approve, that app gets full access to this account, the same as signing in with your password: your balance, plans, payout wallet and settings.*
- **Limits:** *Signing out of ur.io later does not sign that computer out, and there is no per-computer disconnect yet. If you approve by mistake, reset your password to end the access.* The final text waits for the server's sign-out-everywhere (4.4); until then the card states the limitation in these words and promises nothing else.
- **Actions:** *Cancel*, *Approve*, then "To use another account without signing out here, open this page in a private window (URnetwork's Copy link does that)" and *Not {network}? Use a different account* (it says that it signs you out of ur.io in this browser). No auto-approve and no remembered consent.

Other page strings:

| Situation | Text |
| --- | --- |
| Working | Opening URnetwork... |
| After launch | Approved. Return to URnetwork to finish signing in. If it did not open, choose Open URnetwork, or start again from the app. |
| Cancelled | Cancelled. Nothing was shared. You can close this tab. |
| Invalid request | This sign-in link isn't valid. Start again from the URnetwork app. |
| Stale request | This request has expired. Start again from the URnetwork app. |
| Unsupported version | This version of URnetwork isn't supported yet. Update the app and try again. |
| Guest | This is a guest account, so it can't sign in other apps. Add a sign-in method, or use a different account. |
| Wrong OS | Open this link on the PC where you started it. |
| Mint failed | We couldn't approve this right now. Try again. |
| Code limit | Too many approvals are pending. Try again later, or use Settings, Create auth code. |
| Second tab | Already approved in another tab. |

App strings, through `pages::Adv('adv_*', English)` with no new `.resw` keys, per `docs/superpowers/HANDOFF-2026-10-09-sso-error-surface.md:158-159` [V]:

| Situation | Text |
| --- | --- |
| Action | Continue with ur.io |
| Waiting | Waiting for approval in your browser... Approve the sign-in on ur.io, then come back here. Keep URnetwork open. (With the check code, and Open browser again, Copy link and Cancel.) |
| Fallback | Have a code instead? Paste it here. |
| Timeout | Browser sign-in timed out. Choose Continue with ur.io to try again. |
| Denied | Sign-in was cancelled in the browser. |
| Spent code | That approval expired or was already used. Try again. |
| Neutral line (a stray while signed out) | That sign-in link is no longer active. Choose Continue with ur.io to start again. |
| Browser failed | Couldn't open your browser. Copy the link and open it in any browser where you're signed in to ur.io. |
| Confirm | Sign in to {network}? |
| Success | Signed in as {network}. |
| Registration refused | The approval worked, but this device couldn't be added: {server message}. Approve again after fixing that. |
| Custom space | Browser approval isn't available on this network. |

### 3.6 Variants and failure branches

- **Signed out at ur.io:** sign-in card, reload, consent card. A person with no account can create one on the card and approve as the new account.
- **Several accounts:** the site keeps one session per browser profile, so the card names the network and leads with the private-window route. "Use a different account" signs out of every ur.io tab in this browser and says so.
- **Guest:** refused on the server's answer, not on a flag in the stored token: refreshed tokens no longer carry the flag (`controller/subscription_controller.go:244-247` [V]) and `code-login` stamps `guest_mode` false (`model/auth_model.go:2130-2137` [V]). This is a UX refusal, not a control (4.2).
- **The browser cannot launch the app** (no handler, policy block, dismissed prompt): the page keeps *Open URnetwork* for 60 seconds and offers *Copy code*, a clipboard write only (the code is never shown), which pairs with the app's paste field. Pressing *Continue with ur.io* twice does not start a second attempt: the same control becomes *Open browser again* with the same state.
- **App closed (cold start):** unsupported in v1. A link that reaches a freshly started process has no attempt, so it is a stray: the app shows the neutral line and never signs in. Rule for every later change: never relax the gate for cold start. Attempts are not persisted, because that would mean persisting the private key. For the same reason the page never says "all set".
- **Different browser profile:** *Copy link* in the app opens the same request in a private window or another profile on the same PC.
- **Already signed in, or another sign-in running:** the link is ignored (existing behavior for signed in, `AppController.cpp:843-847` [V], extended to Authenticating).
- **Custom or beta network space:** the action is hidden. A ur.io code is valid only on the api behind ur.io.

## 4. Security model

### 4.1 What the design rests on

The credential is delivered only to a handler on the approver's own machine, and the address it goes to is built from constants, never from the request. So an attacker who runs the flow on their own PC and sends a victim the genuine link gets nothing: the victim's app rejects the foreign `state`, and the sealed code never leaves the victim's machine. That argument is specific to a same-device, custom-scheme return. Any later https, App Link, loopback or cross-device return reopens the phishing analysis (it is the weak point of Option C) and needs its own review. Approval hands over a full-account network credential that lasts 30 days (`jwt/by_jwt.go:66` [V]), and nothing can revoke it for one device (row 16).

### 4.2 What each control does and does not stop

- `state` stops pages and scripts that never saw the request. It does not stop anyone who saw the request URL during its 5 minutes (browser history, a shared screen, Copy link, an extension that reads tab URLs). Hence the fragment, the stash-and-strip, and the short life.
- Sealing to `pk` stops anyone who sees only the return (a process command line, a scheme squatter, a synced tab). It does not stop injection by someone who also saw `pk` in the request.
- A server-side verifier (optional, 5.1 item 4) binds a code to the app instance, but its challenge travels in the request, so it does not stop that injection either. Neither it nor sealing replaces keeping the request confidential.
- The check code and the "Sign in to {network}?" confirmation are the backstop for whatever gets through: the person sees which network the PC would join before the device registers.
- Guest refusal is advisory, not a control: a guest can call the api directly today, because `code-create` has no guest check (`model/auth_model.go:1706-1734` [V]) and `code-login` stamps `guest_mode` false.

### 4.3 Threat table

| # | Threat | Mitigation |
| --- | --- | --- |
| 1 | **Login CSRF:** a web page or local script fires `urnetwork://auth?code=<attacker's code>` at a signed-out app. Possible today: the auth branch checks no state and no attempt (`AppController.cpp:843-857` [V]), and a valid foreign code registers this PC in the attacker's network and persists their credentials (`SdkHost.cpp:1349-1436` [V]). | The app acts only on a live attempt whose state matches exactly: one slot, consumed on the first match, 5-minute life; everything else is a stray. Ships first as Phase 0 and is inert until Phase 1b creates attempts. |
| 2 | **Injection by someone who has seen the request** (`state` and `pk` are in the URL the person opens). | The request is in the fragment (never sent to a server or in Referer), stashed in the tab and stripped by the first script, with a 5-minute life and one slot. Residual: browser history, screen sharing, extensions that read tab URLs and Copy link still expose it during that window. Backstops: the check code and "Sign in to {network}?" before registering. |
| 3 | **Interception of the return:** a scheme squatter, command-line or process-creation logs, a synced tab. | The return is sealed to a per-attempt key held in memory (wiped on consume, timeout, supersede, Logout and server switch); the code lives 2 minutes and works once. The page never writes the code anywhere. |
| 4 | **The app logs the secret itself.** Every launch, including the second process the handler starts and a cold start, logs the whole command line, which holds the return URL (`Startup.cpp:216`, `:238-240`; `main.cpp:209-211` [V]). The log can be exported (`SettingsPage.cpp:1326-1347`) and attached to feedback (`:1358-1376`) [V]. | Redact in `CollectDiagnostics`: when the command line holds a `urnetwork://` URL, log scheme, host and length only. Ships in Phase 0 and closes today's exposure of Google and Apple `id_token` returns too. Source pins: no log call receives a raw command line or deep-link URL, and the gate logs closed enums only. Check whether the SDK logs `/auth/code-login` bodies [A]. |
| 5 | **Open redirect or exfiltration through the page.** | No redirect parameter exists. The return is built from the closed table and constants. Unknown, duplicate or reserved keys invalidate the request before the sign-in logic mounts. Table tests. The only outbound path for the sealed URL is one navigation to the constant-built address. |
| 6 | **Approval phishing:** the attacker starts the flow on their own PC and sends the victim the genuine link. | Delivery is local (4.1): the victim's app rejects the foreign state, and the check code will not match. The card says to approve only if you just started this; Cancel returns `access_denied`. |
| 7 | **Consent skipped or automated; accidental Enter.** A queued Enter can land in the freshly focused tab, and keyboard activation is also a trusted click. | Trusted click only, top-level window only, single-flight, no auto-approve, no remembered consent; no autofocus on Approve; about 800 ms input delay; Cancel first in tab order. |
| 8 | **Clickjacking and opener or parent leakage.** | Launch requirements for `/app/approve` (4.4): `frame-ancestors 'none'`, a Cross-Origin-Opener-Policy, `Referrer-Policy: no-referrer` and a Content-Security-Policy. In the page: nothing is interactive unless `window.top === window.self` and `window.opener === null`; no `postMessage`, opener, parent or embedded-webview bridge in this component (source-pinned). |
| 9 | **The card and the mint disagree:** a second tab switches account, a sign-in lands in another tab, a background refresh swaps the session. | Capture the session and identity the card rendered; re-read at click and mint nothing on a mismatch; listen for storage changes and refocus; Approve stays disabled until the session is read, refreshed once and decoded; the cross-tab marker carries only "approved at T". |
| 10 | **Wrong account in the browser.** | The card names the network and leads with the private-window route; "Use a different account" says it signs out of ur.io here; the app asks "Sign in to {network}?" before registering and then shows "Signed in as {network}". |
| 11 | **A guest session approves.** | Ask the server (3.1 step 4) and refuse guests; do not offer the action inside the app's guest-upgrade flow; server follow-up in 5.1 item 2. |
| 12 | **Markup or label injection.** | No free-form label in v1. Request values appear only as text. No HTML sinks, no `href` or `src` built from request values, no template interpolation with request data. Payload tests (`<img onerror>`, `$&`, `{x}`, bidi controls, 10,000 characters) and source pins. |
| 13 | **Stale-tab approval, replay, stray or hostile links.** Today's parser accepts a path or fragment after `auth`, takes the first of duplicate keys and decodes `%00` (`AppController.cpp:60-109` [V]), and every link raises the window first (`:831`). | The page gives up at 4 minutes and the app at 5; the first match consumes the attempt. Strict parser: exact prefix `urnetwork://auth?`, no path, fragment, port or userinfo, no duplicate or unknown keys, no control characters, caps measured before decoding, the error value mapped from a closed enum and never shown raw. A stray never raises the window, resets a step or disturbs a live attempt. |
| 14 | **Leftover state:** a late return after another sign-in, Logout or a server switch (the documented `ssoAttempt_` hazard, `docs/superpowers/2026-10-09-sso-error-surface-resolution.md:160-170` [V]). | One slot, cleared by a match, Cancel, timeout, any other sign-in starting or succeeding, Logout, a server switch and the sheet closing, each at a named UI-thread hook (5.3). The same guard fixes the Google and Apple attempt. |
| 15 | **Scheme-registration abuse:** a per-user key shadows the MSI's; a dangling key hands links to whatever lands on that path later; runtime registration resembles a persistence technique to endpoint security. | 5.3: an app-owned marker, overwrite only an absent or marked key, write before opening the browser and read back, removal in Settings and in the README. A same-user process that can write the key could probably read the app's stored credentials too [A], so it sits outside this boundary; sealing makes an intercepted return useless. |
| 16 | **No way to undo an approval.** The only writer of `network_user.credential_change_time` is the password-reset path (`model/auth_model.go:1653-1663` [V]). Removing the device ends only its client credential [R], while the app also stores the network credential (`SdkHost.cpp:1404-1405` [V]). | Launch gate (4.4): server "sign out everywhere". Until then the card promises nothing and says how to recover (3.5). The app's Logout should also remove its own client (to check [A]). Ask for a "new sign-in" notice. |
| 17 | **Code-slot exhaustion and no rate limits.** 500 active codes per network, counting expired rows until they are reaped, about a day later (`model/auth_model.go:1677`, `:1736-1764` [V]; reaping time [R]); no application-level limit on `code-create` or `code-login` [R]; invalid codes return HTTP 200 [R]. | Each code needs a human click and the person's own session, and the page caps mints (3.4). Ask the server owner for limits, a cap that ignores expired rows and a non-200 for invalid codes (5.1 item 3). |
| 18 | **Third-party script on the consent page.** The sign-in dialog loads provider scripts. | Mount it only while signed out, reload after sign-in, list the provider origins in the page's Content-Security-Policy, and load no other third-party script. |
| 19 | **Clipboard leakage.** Copy link and Copy code put secrets on the clipboard, where history and sync can keep them; the paste field shows a 684-character code in clear (`LoginPage.cpp:1086-1094` [V]). | App: set the clipboard with history and roaming disabled and clear it after 60 seconds if unchanged; use a password box for the paste field. Page: Copy code only after a failed launch, and never display the code. |
| 20 | **Wrong network space or wrong OS.** | The action appears only for the official link host; the page enables Approve only when the browser's OS matches the platform row. |

### 4.4 Launch gates

Before the flag defaults on. The first beta can ship earlier with the paste fallback and without `pk`.

1. **Edge headers on `/app/approve`**, checked on the deployed path (a meta tag cannot carry `frame-ancestors`): `frame-ancestors 'none'`; a Cross-Origin-Opener-Policy (`same-origin`, or `same-origin-allow-popups` if provider sign-in popups need it; test every provider); `Referrer-Policy: no-referrer`; a Content-Security-Policy with `object-src 'none'` and `base-uri 'none'` whose script sources are the site plus the sign-in providers.
2. **Server:** "sign out everywhere" exists (5.1 item 1) and the consent text in 3.5 is finalized.
3. **Sealed return on:** `pk` is required.
4. **Sign-in methods:** every method the dialog offers, Google and Apple included, completes on production ur.io after the deploy that carries this page. Until then nothing claims that Google-account users benefit.
5. **Matrix:** the live-desktop matrix in 7.5 passes with the owner's real clicks.

## 5. Work breakdown

Public repositories get file-level pointers; the internal website repository gets behavior only.

### 5.1 Server (urnetwork/server, the protocol team's lane)

Nothing is needed to ship v1: `code-create` and `code-login` already do what the page and the app need, and the ur.io session holds the network credential that `code-create` requires (server main classes the route "App admin" in `AUTHZ1.md` and plans to make that class network-credential-only later; this plan is unaffected [R]). Requests, in priority order:

| # | Request | Pointers | Needed for |
| --- | --- | --- | --- |
| 1 | "Sign out everywhere": an endpoint and an account-page button that set `network_user.credential_change_time` to now. Today only the password-reset path writes it, and a network credential is rejected only when it is older than that time. Check whether accounts that sign in only with Google, Apple or a wallet have any rotation path [A] | `model/auth_model.go:1653-1663`, `jwt/by_jwt.go:563-566` [V] | Default-on (a gate) |
| 2 | `code-create` refuses an account with no sign-in method; restore the client-credential check that is commented out; decide whether `code-login` should keep stamping `guest_mode` false | `model/auth_model.go:1706-1734`, `:1711-1724`, `:2130-2137` [V] | Recommended |
| 3 | Per-network and per-address limits on `code-create` and on failed `code-login`; a cap that ignores expired rows, or reaping them within minutes; a non-200 status for an invalid code | `model/auth_model.go:1677`, `:1756`, `:2006`, `:2067` [V]; reaping time [R] | Recommended |
| 4 | Optional contract v2: a nullable `code_challenge` on `auth_code`, an optional field on create and a `code_verifier` on login, S256 checked inside the login transaction before the code is consumed, and a verifier sent for a code that has no challenge rejected. The SDK argument structs, the cgo header and `SdkHost.cpp:689-690` must gain the field | `model/auth_model.go:1796-1819` [V], `:2018-2071` [R] | Optional hardening |
| 5 | Tests for expiry, multi-use, the cap and concurrency (the model has two code tests [R]); a "new sign-in" notice; a connected-devices list with per-device revoke | none | Later |

### 5.2 Website (internal repository: behavior only)

Owner: the site team [A: not yet confirmed]. What the site should do:

- **One standalone, no-index page at `/app/approve`:** a static shell with no account host and no app chrome, `Referrer-Policy: no-referrer`, and no third-party script except the sign-in providers while signed out. An explicit page beats the site's catch-all route (the `/app/pay-sheet` precedent [R]); everything under `/app` is already no-index.
- **A pure request module:** strict fragment parse, the closed platform table, rejection of reserved, duplicate and unknown keys, and the return builders. It runs in the boot script before the site's sign-in logic mounts.
- **Session handling:** wait for the stored session to be read and refreshed once; make one authenticated call (the guest lookup) before Approve is enabled; a 401 logs out and shows the sign-in card; react to storage changes and to refocus; open the sign-in dialog on its explicit login view; render nothing actionable while a logout is in flight.
- **Consent card and copy (3.5):** identity capture and compare at click, the check code, the input delay, tab order, the private-window route.
- **Approve:** `POST /auth/code-create {uses: 1, duration_minutes: 2}`; seal with the page's existing NaCl and base58 pieces (the wallet return already seals to an app-supplied key [R]); one navigation to the constant-built URL; the 60-second retention and the Copy code fallback; the cross-tab marker; the mint cap.
- **Edge requirements** (4.4), requested from whoever owns the edge and checked after deploy.
- **Tests** (7.3) and English-only strings (no shared-store keys in v1).
- **Deploy:** the live site lags the website's main branch, so nothing can be tested against ur.io before a deploy that carries this page [R]. Phase 2 chores: correct the developer doc that still says the authorization server is not live, and confirm Google sign-in on production ur.io.

### 5.3 Windows app (this repository)

Line numbers are from the safety ref; re-pin after the merge. All new copy goes through `pages::Adv` with English fallbacks, with no new `.resw` keys.

| Area | Change and pointers |
| --- | --- |
| Phase 0: gate and parser | One strict parser for the `auth` host (exact prefix `urnetwork://auth?`; today's check also accepts a path or fragment, takes the first duplicate key, turns `+` into a space and decodes `%00`: `AppController.cpp:60-109`). The gate of 3.1 step 8 runs before `SignInWithAuthCode`, which resets the current step first (`LoginPage.cpp:1033`), and before `ShowWindow` (`AppController.cpp:831`). The pin at `tests/sso_error_surface_test.go:68-76` is updated to cover the non-auth path, not deleted. Fix the comment that says the site builds such links (`AppController.cpp:839-842`). |
| Phase 0: log redaction | When the command line holds a `urnetwork://` URL, log scheme, host and length only. `Startup.cpp:216` and `:238-240` log the whole command line on every launch (`main.cpp:209-211`), today including Google and Apple `id_token` returns. |
| Attempt model | A pure header `App/UrIoApproval.h` in the style of `WalletBridgeRoute.h:1-60`, listed in `App.vcxproj` near `:398-399`. `Begin` replaces any attempt; `Check(return, now)` answers Accept, Denied, Stray or Expired; `Clear`. UI thread only. `state` and the keypair come from the OS random source (BCrypt), because the entropy of `urnet::generateNonce` is not documented in this repo. Key helpers exist: `urnet::generateWalletKeyPair`, `generateSharedSecret`, `decryptData` (`WalletConnect.cpp:169-182`, `:356`). The percent-encoder `Esc` (`:51-67`) is file-local: export or copy it. |
| Clear hooks | `AppController::OnAuthState` for LoggedIn and LoggedOut (marshaled to the UI thread at `AppController.cpp:208-210`, handler at `:325`); `SdkHost::ApplyNetworkServer` (`SdkHost.cpp:1203`), which resets other pendings but not `ssoAttempt_`; the sheet's Closing event; `CancelPendingWalletFlows` (`:1584-1609`). The same guard fixes the documented Google and Apple hazard. |
| Sheet | A new class-based `BrowserApprovalSheet`, shaped like `GuestModeSheet` (`AuthSheets.h:25-49`, `Hide()` at `AuthSheets.cpp:211-217`) and `UpgradeSheet::LaunchHosted`, which awaits the launcher and shows the URL if it fails (`BalanceSheets.cpp:983-1010`). The current sheet is a coroutine with a local dialog that nothing can hide (`LoginPage.cpp:1065-1139`), its body text still tells people to paste (`:1077-1080`), a `ContentDialog` closes on Esc (`AuthSheets.h:56-64`) and three command buttons clip (`LoginPage.cpp:1101-1103`), so this is a refactor. States: waiting (Open again, Copy link, Cancel, check code, 5-minute timer), confirm, signing in, timed out, denied. Initial focus on Cancel; status announcements for waiting, timed out and denied; every action reachable by keyboard; closing for any reason cancels. The greyed buttons cannot carry the waiting state because `OnWindowReactivated` re-enables them (`LoginPage.cpp:1309-1313`). |
| Neutral line | Signed out with no live attempt: one non-modal, dismissible, rate-limited line on the login screen; it never raises the window or resets the step. Ships with Phase 1b; Phase 0 only logs. |
| Sign-in tail | A new `SdkHost` entry that calls `authCodeLogin`, passes `network_name` to a confirm callback and only then runs `RegisterNetworkClient` (`JwtClaimString` is file-local, `SdkHost.cpp:38-73`: add an accessor). Clear the Api's credential when registration fails: `setByJwt` runs first (`:1367`) and the error branches (`:1375-1386`) leave it in memory. Say "Signed in as {network}" only for this flow: the tray text shows on every LoggedIn transition, guest-upgrade re-registration included (`AppController.cpp:331-349`). |
| Gating and kill switch | Offer the action only for the official link host (`SdkHost.cpp:413`, `:1250`) and only after a cheap probe finds the page live (cached per run). The probe is the runtime kill switch; a `Config.h` flag keeps it off while in development. |
| Clipboard and paste | Copy link and Copy code with history and roaming off, cleared after 60 seconds if unchanged; the paste field becomes a password box (it shows a 684-character code today, `LoginPage.cpp:1086-1094`). |
| Logs | Closed-enum lines only: armed, matched, denied, stray (live or none), expired, timed out, cancelled, launch result, handler written, unchanged or failed. Never a URL, code, state, pk or raw error text. |
| Quick wins (optional, Phase 0) | Check `LaunchUriAsync`'s result in the current sheet (discarded at `LoginPage.cpp:1115-1116`). Point *Open ur.io* at `https://ur.io/?auth&next=/app/account/settings` [R]: signed out it opens the sign-in dialog, signed in it lands on Settings, where Create auth code is. |
| Docs | The zip README text (`app/tools/package-portable.ps1:121-139`) gets three lines: the browser asks "Open URnetwork?", what to do if nothing happens, and how to remove the protocol key. |

**`EnsureProtocolHandler()`** runs once at app start and again when an attempt begins, so the Google, Apple, wallet and email-link returns, which share the dependency, benefit too. Today the key exists only if the MSI ran or someone added it by hand (`docs/superpowers/HANDOFF-2026-10-09-sso-error-surface.md:140-141` [V]). It writes `HKCU\Software\Classes\urnetwork` with the MSI's exact values and quoting (`Package.wxs:221-226`: the quoted exe path, then a quoted `%1`; the activation scan stops at the first space, tab or quote, `AppController.cpp:53`) plus an app-owned marker (exe path and version). It overwrites only a key that is absent or carries the marker; otherwise it refuses and keeps the paste fallback. It writes before opening the browser, reads back, and aborts the attempt on failure. The direct write is preferred to `ActivationRegistrationManager` because that API's behavior for an unpackaged exe is unverified, while the plain Launch command the MSI registers is the form already exercised on the real desktop. The README says plainly that this takes the scheme from a per-machine MSI install for this user and how to remove it; Settings gets *Remove link handler*. Budget 1.5-2 days.

### 5.4 Android (follower, not part of v1)

Pointers: `app/app/src/main/AndroidManifest.xml:129-143`, the `https://ur.io/c` link and the `ur` scheme [V]; `app/app/src/google/java/com/bringyour/network/LoginActivity.kt:235-350`, auth-code intake from links [R]; `app/app/src/main/java/com/bringyour/network/ui/login/LoginUtils.kt:205` and `:304-360`, a persisted attempt store with a 10-minute age, the model to extend [V]. Work: add the `auth` host under this contract only with a persisted attempt, the same state gate and the sealed return; take `code` and `state` rather than `auth_code` on that host; add the `android` row to the page last.

### 5.5 iOS and macOS (follower)

`app/URnetwork-Info.plist:20-33` registers `urnetwork://` [V]. The attempt model to mirror is `app/network/Shared/BrowserSso.swift`: the contract at `:5-26`, the five-minute timeout at `:61`, the check at `:200-237`, and the single-slot store where a stray never disturbs a live attempt at `:314-370` [V]. It is compiled for the macOS direct-download build only (`app/network/NetworkApp.swift:689-691`, `#if DIRECT_DOWNLOAD` [V]); iOS has no `auth` deep link (`app/network/Shared/ViewModels/DeepLinkRouter.swift:30-49` [V]). Open: does `ASWebAuthenticationSession` share Safari's ur.io session [A]?

### 5.6 Linux

Not inspected. Expect an `x-scheme-handler/urnetwork` registration and one more row in the page's table, under the same gate.

## 6. Phases and effort

| Phase | Owner | Estimate | Scope | Exit |
| --- | --- | --- | --- | --- |
| 0. Close the hole | Windows | 1.5-2 days | Strict parser and gate (inert until attempts exist: strays are logged and ignored, with no UI), command-line redaction, the comment fix, Go pins and the header test, plus the optional quick wins in 5.3. Precondition: the lines are re-pinned to the merged tree (the merge itself is done: `a18ab6c`) and the owner has answered 8.1 item 1. | The paste flow still signs in on a real desktop; a hand-fired `urnetwork://auth?code=...` does nothing visible and does not raise the window; the log holds no deep-link URL |
| 1a. Approval page | Site team | 5-7 days, plus their deploy | Section 5.2 | Hand-made request URLs behave per section 3 on a local or staging build; tests green; the 4.4 headers verified on the deployed path |
| 1b. App flow | Windows, parallel with 1a | 7-9 days | Attempt model, class-based sheet, sealed return, handler registration, confirm step, link-host check and probe, neutral line, clipboard, logs, tests; behind a flag | Header tests and Go pins pass; flag off by default |
| 1c. Integration | Windows drives, the owner clicks | 1.5 days | The matrix in 7.5 | The matrix passes, or the kill criterion in section 9 fires |
| 2. Release | Both | 0.5 day | Flip the flag once the page is live and the 4.4 gates are met; keep the paste flow; publish the contract; correct the stale developer doc; confirm Google sign-in on production | Default-on with the fallback intact |
| 3. Server | Server owner | their estimate | 5.1 items 1-5. Item 1 is small in code (one update, an endpoint, a button, tests) but needs a decision for accounts without a password; the optional verifier is about a week across server, SDK, cgo and C++ [A] | Item 1 gates default-on |
| 4. Followers | App teams | not estimated | Android, iOS and macOS, Linux (5.4-5.6), each only with its own gate and the sealed return | A platform row added to the page per app |
| 5. Later | URnetwork | not estimated | Option C for headless and other-device sign-in; Option B when a first-party grant exists, reusing the attempt and state code so only the request builder changes; connected devices | none |

Windows totals 8.5-11 engineer-days (phases 0 and 1b), the site 5-7, integration 1.5: about 15-20 engineer-days across two teams, roughly three calendar weeks if both start together, plus the site's deploy cadence and the server gate. These are estimates, not measurements. Earlier verbal figures (8-11 days, one and a half weeks) predate review, which added the sealed return, the confirm step, log redaction, a class-based sheet, handler hardening, CI wiring and the matrix. Calendar gates: (1) the lines re-pinned to the merged tree; (2) a site deploy that carries `/app/approve`, with Google sign-in working on production; (3) one session of the owner's real clicks; (4) sign out everywhere on the server, for default-on.

## 7. Test plan

- **7.1 Windows unit tests.** A c++20 executable in `app/tools` built against the app's own header, like `solana-wallet-tests.cpp:1-18` [V]. Cases: a fresh state per attempt (43 characters, unique); replay; a stray with no attempt; a wrong state leaves a live attempt untouched; expiry on both clocks; supersede; each clear hook; signed in and Authenticating ignored; duplicate, unknown, path, fragment, port and userinfo variants; control characters including NUL; caps measured before decoding; the closed error value; a plain return refused when `pk` was sent; a bad unseal and an inner-state mismatch are strays that keep the attempt; the neutral line's rate limit.
- **7.2 Go source pins in `tests/`.** These are the tests that run without the app: `go test ./tests`, on an LF copy of the tree (see `docs/superpowers/2026-10-09-upstream-merge-pr4.md` for the recipe; upstream removed its GitHub workflows, so nothing here runs on push). The C++ header tests need a C++20 compiler, so the Go pins are the part that always runs. Pin that the gate precedes `SignInWithAuthCode` and the step reset; that a stray returns before `ShowWindow`; that `ShowWindow` still precedes `sdk_.HandleDeepLink` for other hosts; that no log call receives a raw command line, deep-link URL, code, state or pk; that the clipboard calls pass the history and roaming options; and that the new header is in the project.
- **7.3 Site tests** (the site team picks the harness). Table tests of the request module: every reserved, duplicate, unknown and percent-encoded spelling; a missing or oversized `state`; an unknown platform; a stale `ts`; payload strings (`<img onerror>`, `$&`, `{x}`, bidi controls, 10,000 characters). A component test with fake hooks: an identity change mints nothing; a double click mints once; two tabs mint once; a 401 shows the sign-in card; a failed guest lookup says "try again"; the top-window and opener checks; no outbound channel other than the one navigation. Source pins: no HTML sinks, no write of the code to web storage, no auto-approve, no redirect parameter, no `postMessage`, opener, parent or embedded-webview bridge.
- **7.4 Cross-implementation vector.** A fixed keypair, nonce and plaintext sealed by the page's library must open with the app's `decryptData`, and the reverse. The vector holds no secret and lives in both repositories' tests.
- **7.5 Live-desktop matrix**, with the owner's real clicks. Method: the Windows team's observe-only probes (`.localstate-verify/dl-*.ps1`, local and gitignored) record the foreground owner, the z-order, how many of three probe points show the app (`WindowFromPoint`) and real `CopyFromScreen` pixels, judged on the first sample after "deep link received"; `dl-watch-real.ps1` is passive while the owner clicks (`docs/superpowers/2026-10-09-sso-error-surface-resolution.md:77-99` [V]). Do not use `PrintWindow` or forced-foreground harnesses to claim visibility (`:77-83`). The page strips its address within milliseconds and the app never logs `state`, so the probes read the request from the clipboard after Copy link.
  - Browsers: Edge, Chrome, Firefox. App: running, hidden to the tray, minimized, and closed (expect the neutral line and no sign-in).
  - Cases: a signed-out browser with inline sign-in, including the Google popup-blocked full-page fallback (after the deploy); wrong account, then switch; guest; expired request; stray and replayed link; double Approve and two tabs; "forgot password" opened in a second tab; session ended in another tab; first-use prompt and "always allow"; dismissed prompt; no handler registered (what each browser does on a top-level navigation to an unhandled `urnetwork://`, and whether the page survives it); a key path with spaces and a non-ASCII profile name; the MSI installed as well (HKLM and HKCU); a managed or antivirus-enabled machine; an elevated primary instance receiving a medium-integrity launch [A].
- **7.6 Deployed-path checks**, after the site deploy: the headers in 4.4 on `/app/approve`; no analytics, error report or injected script receives the page URL; the fragment is never in a request.
- **7.7 Abuse cases.** Each row of the threat table has at least one test or probe; the site author and the app author add the cases in their lane.

## 8. Open questions

### 8.1 For the owner

1. **Stateless link.** OK to stop accepting `urnetwork://auth?code=` without a live attempt? Recommended: yes. No producer of it was found in this repository or the Android and iOS clones [R].
2. **Ownership.** Who on the site team builds and deploys `/app/approve` and chases the deploy? The protocol and website repositories are another lane (`docs/superpowers/HANDOFF-2026-10-09-sso-error-surface.md:154-155` [V]), so this document can carry only the contract.
3. **Portable zip.** May the app register `urnetwork://` for the current user at runtime (5.3, with its guardrails), or should the flow require the MSI?
4. **Confirmation click.** Keep "Sign in to {network}?" (one extra click after approving), or ship without it behind the flag and measure?
5. **Placement.** A link under *Sign in with browser* (v1), or a first-class button beside Google and Apple? The second touches `MainWindow.xaml` and the `SetWalletSignInEnabled` list (`LoginPage.cpp:1244-1261`), files the merge also touches, and costs about half a day more.
6. **Lifetimes.** Code 2 minutes, attempt 5, page 4: confirm. Is 5 enough for inline account creation?
7. **Sealed return.** Required for the first beta, or only for default-on (recommended)?
8. **Custom or beta network spaces.** Hide the action (recommended), or open that space's own site if it implements the contract?
9. **Names.** This contract uses `code`; the apps' app-to-site handoffs use `auth_code`. Align every platform's new `auth` host on one name?
10. **Computer name.** Worth sending later as a verified field? It was cut from v1.

### 8.2 For URnetwork

1. Sign out everywhere (5.1 item 1): who builds it, and when? Do Google-, Apple- and wallet-only accounts have any credential rotation path?
2. Rate limits, the cap and expired rows, a non-200 for invalid codes, and `code-create` for accounts with no sign-in method (5.1 items 2-3).
3. Does the SDK build the Windows app ships renew the network credential (`POST /auth/network-refresh`)? A code-login credential lasts 30 days [V].
4. Edge headers on `/app/approve` (4.4), and a Cross-Origin-Opener-Policy value that keeps every provider sign-in popup working.
5. Does Google sign-in on ur.io complete in production, and when does the next site deploy land?
6. Does the SDK log `/auth/code-login` request bodies, and what does `uploadLogs` send from the shared log directory (`SettingsPage.cpp:1358-1376`)?
7. iOS and macOS: does `ASWebAuthenticationSession` share Safari's ur.io session?
8. Longer term: who owns first-party OAuth (`IDP.md:3-8` says first-party surfaces should use one issuer instead of the ad-hoc code flow [V]), a connected-devices list, and the issuer's DNS?

### 8.3 Not verified

Browser behavior on a custom-scheme launch: the first-use prompt, "always allow", enterprise policy, and what Firefox leaves in the address bar and in sync when a launch fails; only a Chromium launcher has ever been observed for protocol activations here. `ActivationRegistrationManager` for an unpackaged exe. How endpoint security reacts to runtime registration. Behavior when the primary instance is elevated. Whether the live site equals the website's main branch. Whether accounts that sign in only with Google, Apple or a wallet can rotate credentials. The Linux client and the browser extension as producers or consumers of `urnetwork://auth`. The merged Windows files (`AppController`, `LoginPage`, `MainWindow`, `AuthSheets`, `Config.h` and the headers upstream added), which were read only as they stood before the merge: line numbers may have shifted, and upstream removed guest mode, so the guest-upgrade interaction no longer applies. How the SDK protects stored credentials on Windows (row 15 assumes the same user can read them).

## 9. Non-goals

- Cross-device approval, polling, or any server-held pending-request table (Option C, later).
- Making ur.io an OAuth or OIDC login provider for the desktop (Option B), or changing the OAuth server.
- Replacing email and password, Google, Apple or wallet sign-in. The paste flow stays as the fallback.
- Embedding the approval page in WebView2: its profile holds none of the person's ur.io session.
- A loopback-listener return in v1. It is the stated fallback if 7.5 shows the scheme path unreliable: the same attempt and state, an ephemeral port on 127.0.0.1 (RFC 8252) and a constant path, which removes registration, the browser prompt and the command-line exposure at the cost of a port parameter. That is the kill criterion for the custom-scheme return.
- A per-device revoke or a connected-devices screen inside this plan (asked of the server owner as separate work, 5.1).
- Remembered consent, silent re-approval, or any auto-approve path.
- Persisting attempts across restarts, or supporting cold start.
- Changing token formats or lifetimes on the server in v1.
- New `.resw` keys or shared-store translations in v1.
- Describing weaknesses of production systems outside this repository.