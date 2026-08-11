# Design update super plan — alignment, simple mode, motion, brand

Date: 2026-08-11. Line: `beta/custom-server` (the UI line), parallel to and independent of
`beta/algorithm-dpi`. Owner-initiated from an annotated screenshot plus four asks: fix the
lines that do not match up; make the front page simple when Advanced Mode is off; add
animation everywhere; add a branded animated installer/startup.

Research behind it (full text under the session scratchpad, summarized inline here):
`01-alignment-bugs.md`, `02-simple-mode.md`, `03-motion-system.md`,
`04-installer-startup-motion.md`. Every claim below is code-cited in those files.

## 0. The through-line

Four generations of layout work (D1/D4/R1/R3/R4) each re-derived their own metrics in
place. The result is not a few crooked lines — it is that **no single source decides where
an edge goes**: `UrPaneRowHeight` is overridden inline 14 times with 34/36/44/48/0; the
column widths exist in BOTH `MainWindow.xaml` and `MainWindow.xaml.cpp` (editing the XAML
alone does nothing); `#151515` is spelled as four separate literals; border thickness
appears as 1, 1.5 and 0.5 with one of them drawn on a `Canvas` that does not layout-round.
The owner runs **125% scaling**, so every odd metric lands on a half physical pixel and the
inconsistency becomes visible.

So the alignment work is not three patches. It is: fix the two visible bands, then make the
recurrence impossible, then stop hand-placing edges.

The same theme runs through the other three asks:
- Simple mode is not a new screen — the status strip is ALREADY tiered correctly; what the
  owner photographed was the Advanced reading. The gap is that the three-column shell
  survives into Simple, where it should not exist.
- Motion has no `Frame`: pages are seven sibling Grids toggled by `Visibility`
  (`MainWindow.xaml.cpp:1019-1027`), so the whole `NavigationTransitionInfo` family is
  unreachable and transitions must be hand-built on Composition.
- The installer's problem is not that it lacks animation. It **installs 7 files and omits
  the self-contained WindowsAppRuntime and `Assets\Fonts`**, so an MSI install on a clean
  machine yields an app that cannot start.

## Phase A — make it correct (ship first, smallest diff, highest daily value)

**A1. The two attribute edits that fix "lines don't match up".** (~1 hour, do together)
- `MainWindow.xaml:594` — add `AlwaysShowHeader="False"` to `HomeNav`. Nothing ever sets
  `HomeNav().Header`, but the `HeaderTemplate` (`:610-615`) still instantiates against null
  and `UrTitleTextStyle` pins `LineHeight="36"` + `Margin="4,4,0,0"` (`App.xaml:232`),
  leaving a permanent **40epx empty band** above the pane shell. That band is exactly why
  the hamburger sits at content-top while the three column headers sit one header lower.
- `MainWindow.xaml:2353` — `ModeNoticeBar` carries `Margin="12,0,12,12"` and is only ever
  toggled with `IsOpen` (`DeveloperPage.cpp:346-361`). WinUI collapses an InfoBar's template
  root, not the control, so the margin still measures and root-grid row 2 is permanently
  12epx tall. Set `Margin="0"`, full-bleed it, and collapse it via `Visibility` in
  `DeveloperPage.cpp:347`. The vertical rules (`ConnectPaneBRule`/`CRule`,
  `MainWindow.xaml:1034,1092`) then actually meet the status strip hairline
  (`App.xaml:568`) instead of stopping 15 physical px short.

**A2. The Settings clip.** (~1 hour) A true overlap is impossible as written — all seven
root-grid children were enumerated: no `RowSpan`, no negative margin, no ZIndex, and the
rail has ~734epx of budget for ~348epx of content. The real cause is
`MainWindow.xaml.cpp:808` toggling `DeveloperNavItem().Visibility` at runtime: it is a
**FooterMenuItems child**, and WinUI stamps a cached `MaxHeight` on the footer ScrollViewer
that a visibility change does not refresh — so it clips instead of scrolling. Fix by
inserting/removing the item from `FooterMenuItems` rather than toggling visibility, and add
a `PaneFooter` spacer so the last item is never flush against the strip.

**A3. The MSI actually installs a working app.** (~half a day, beta-blocking)
WiX v5.0.2, `OutputType=Package`, bare MSI with zero UI authored, installing 7 files. Use
WiX v5 `<Files Include>` to harvest the full payload — the self-contained WindowsAppRuntime
and `Assets\Fonts` included — then verify by installing into a clean VM/container, not on a
machine that has already run the portable zip (that is why this looked fine before). Extend
the CI verify step to assert the payload file count/critical names, since today it only
compile-proofs the MSI.

**A4. The layout invariant, so this cannot come back.** (~half a day)
One header-height token; rows 2-3 of the root grid must measure 0 when silent (no margins
permitted there); exactly two divider styles; column widths defined in ONE place (delete the
C++ duplicate at `MainWindow.xaml.cpp:394,400` or make XAML the derived side, not both); and
a debug-build tree assertion that fails on any non-token row height, inset, or border
thickness. Normalize `StrokeThickness` 1.5/0.5 (`MainWindow.xaml:886`,
`StatsSheets.cpp:513`) and make the chart axis layout-round like the Borders do
(`TransferChart.cpp:308-311`).

## Phase B — simple mode (the front page the owner asked for)

**The rule (testable, not taste).** Simple answers three questions: *Am I protected? Through
where? What do I press?* Advanced answers a fourth: *how is it working?* Operationally: an
element is Advanced if the sentence it enables contains a unit (bps/bytes/packets/count), an
identifier (uuid/IP/host/port), a raw pre-clamp value, or a tuning verb
(set/prefer/allow/pin/block) — **unless** it is the sole evidence for one of the three
questions, in which case it is restated without the token.

**The Simple Connect page** — one pane, 480dip cap, centred, six elements:
status block (dot + headline + a new plain line, "Your internet traffic is not protected.")
→ hero canvas enlarged 182→288px → selected-provider row → Connect button → the three
blocking banners → one collapsed `⌄ More options` row containing today's Provide group,
Connect options group, peers line and peers list, unchanged. Panes B (Activity) and C
(Client statistics) **do not exist** in Simple. Status strip reduces to pill + Provider +
traffic-in-words ("Carrying traffic" / "No traffic yet"). Developer already folds out of the
nav rail.

**Implementation stance: disclosure, not relocation.** Provide and Connect options keep
their exact markup positions inside one new contiguous host — two `Visibility` writes, no
reparenting (R3 deliberately deleted reparenting, `MainWindow.xaml:696`), no second writer
of any SDK preference, no relocated store strings. One markup move is required: `PeersLine`
moves below the three InfoBars.

**Empty states, resolved by deletion.** Four of the five blank boxes the owner X'd live on
panes B/C, which Simple does not render — so Simple needs zero empty-state copy. Advanced
keeps them but splits one nothing into three *distinguishable* nothings: **no session /
idle / rpc-only**. The third is a live defect today: under rpc-only the clamped counters
render identically to idle, so "not actually running" is indistinguishable from "running,
nothing happening". Also fix `ConnectPage.cpp:1533`, which prints `Allowed 0` while every
row above it prints an em dash.

**Dead markup found:** `PaneAMeta` on the Connect pane has no writer anywhere — delete it.

## Phase C — motion (a language, not a pile of animations)

**Tokens** (live in `App.xaml` beside `UrPaneHeaderHeight`, plus a new `UrMotion.h` beside
`UrColors.h`), absorbing values already in the code:
durations Micro 90 / Fast 150 / Base 250 / Slow 400 / Hero 500 (= existing `kStateFadeMs`) /
Epic 1000 (= existing `kBlobMs`) / Pulse 1500; stagger 40 (max 6 steps), overlap 60;
easings Standard `(0.10,0.90)(0.20,1.00)`, Exit `(0.70,0.00)(1.00,0.50)`, Soft
`(0.40,0.00)(0.20,1.00)`; one spring (damping 0.75, period 40ms) reserved for the Connect
button; distances 4/8/12/24 dip; hover 1.03, press 0.97. **Exits run one step faster than
entrances.** Full-height panes never translate.

**Priority order**
- **P0 — the cheapest win first:** one `VisualTransition GeneratedDuration` in `App.xaml`
  gives hover/press micro-interaction to every row in the app with zero C++.
- **P0 — page transitions:** a Composition crossfade in `OnNavSelectionChanged` (there is no
  `Frame`, so this is hand-built); delete the one-shot `AnimateDrawerIn`.
- **P0 — the connect moment:** today the canvas takes 500ms while the dot, word, button and
  blobs snap. Make all five one timeline in `ApplyConnectStatus`. This is the emotional
  centre of the product and currently the least coherent thing in it.
- **P1 —** Advanced Mode reveal as reserve-and-fade (never animate `Height`) with a
  clip-wipe on the status strip — this is the showcase for Phase B; list transitions
  (Reposition-only on the big tables); nav rail and dialogs — **build nothing, the framework
  already does these better**.
- **P2 / reject —** connected animations, numeric roll-ups, Lottie (no dependency exists;
  do not add one).

**Two hard gates, one choke point each.** Reduce-motion: a single `motion::ShouldAnimate()`
consulted everywhere — today `UISettings` is read only inside `ConnectCanvas`
(`ConnectCanvas.cpp:202`), while LoginCarousel, AnimateDrawerIn and StatsSheets ignore it.
Presentation gate: hook the first line of `MainWindow::SetPresentationActive`; one-shots
stop-then-settle, only the bounded idle pulse may replay on restore.

**The hazard to design out (found, not hypothetical):** the resume path at
`AppController.cpp:788-797` would re-fire every state animation and every list entrance on
each tray restore — the same shape as the old focus-loss graph-reset bug. Needs a
`SuppressScope` around the replay plus change-detection in `ApplyConnectStatus`.

## Phase D — brand and installer motion (last, and deliberately trimmed)

**D1. Close the asset gap first (~1 hour).** The Windows repo has no SVG and no logo raster
above 256px. The vector master is a C++ string literal (`kGlobePath`) duplicated across two
.cpp files; the real SVG and 1024px PNG live in `claude_sandbox_ios`. Copy them in, de-dup
to one header. Everything branded is blocked on this.

**D2. Static installer branding (cheap, near-zero risk).** WixUI + branded 493×312 and
493×58 bitmaps. Do this with A3 — same file, same test.

**D3. The actual animated installer (1-2 days) — only alongside code signing.** A Burn
bundle with a WixStdBA **custom theme** using thmutil `<Billboard>` gives real motion with
no custom bootstrapper. A custom out-of-proc BA (WiX v5 changed BAs from DLL to EXE) is 3-5
days plus an install-matrix tail for a 5-15 second experience — **rejected**. Pair with
signing so the SmartScreen reputation clock resets once rather than twice; adding a bundle
also needs `wix burn detach/reattach` signing steps in CI, and changes the artifact story
(bundle .exe alongside the portable zip).

**D4. Startup animation — mostly say no, honestly.** Unpackaged apps get no OS splash
(that is an MSIX manifest feature; verified). Measured on this box: `wWinMain`→tray icon
56-176ms, first tray click→window 99-227ms, but whole-process cold start ~2202ms — because
56MB of load-time imports (`URnetworkSdk.dll` alone is 31.6MB) resolve **before our first
instruction**. A splash cannot cover that. Ship instead:
- a 180ms XAML settle + globe draw-in on first window open (covers work already happening);
- **the relaunch splash** (~half a day) — `main.cpp:257` sleeps up to **10 seconds** after an
  update with nothing on screen. That is the only real dead air in the product.
The actual cold-start lever is delay-loading `URnetworkSdk.dll`, which is engineering, not
animation — worth its own ticket.

## Sequencing and why

A → B → C → D, with D1 pulled forward to sit beside A3.

A first because it is the smallest diff with the highest daily value: two attribute edits
delete both misaligned bands, one insert/remove fixes an unusable nav item, and the MSI
stops shipping a non-starting app. B next because Simple mode deletes four of the five
empty-state problems rather than styling them. C after B because the Advanced Mode reveal
is motion's showcase and needs B's structure to exist. D last because it is the only tier
whose payoff is purely brand, and its best item (the relaunch splash) is independent of
everything else.

## What this plan says no to

A custom bootstrapper application; Lottie or Win2D as new dependencies; a startup splash
that would add perceived latency; connected animations and numeric roll-ups; relocating
controls to build Simple mode; and animating `Height` anywhere.

## Risks

Phase A4's tree assertion could be noisy on first run — expect a cleanup pass, and keep it
debug-only. Phase B's single markup move (`PeersLine`) is the one place a mistake changes
behavior rather than looks. Phase C's biggest risk is the resume-replay hazard above; the
second is that hand-built page transitions on a `Visibility`-swapped shell can double-render
during the crossfade — budget for measuring frame time with the live packet-stats UI
running, not on an idle window. Phase D3 is gated on a signing decision that has not been
made yet; do not start it before that decision.
