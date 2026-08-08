# URnetwork Windows app redesign specification

> ## OWNER RECONCILIATION (2026-08-08) — READ THIS FIRST; IT OVERRIDES THE BODY
>
> This spec was authored by a review agent from screenshots. The owner reviewed
> it and made four decisions that **override the body wherever they conflict.**
> Everything else in the spec is ADOPTED.
>
> 1. **Fonts — brand faces ONLY. The spec's "Segoe UI Variable throughout" is
>    REJECTED.** Use the licensed brand type already in the repo: ABC Gravity
>    (Extended + Extra-Condensed), PP Neue Montreal (body), PP Neue Bit. The
>    spec's real complaint — two clashing heading styles, the heavy display face
>    shouting on every micro-header — is fixed *within the brand*: apply the
>    spec's TYPE RAMP (sizes, weights, sentence case, one hierarchy) but realized
>    in brand faces. ABC Gravity for page titles, the wordmark and the hero;
>    **PP Neue Montreal for card section headers and body**, so section headers
>    stop shouting. Do NOT introduce Segoe UI Variable as a text face.
>
> 2. **Information architecture — FULL restructure to 5 destinations**, per the
>    spec's §3: Home / Network / Earnings / Account / Settings, with
>    Help + Diagnostics + Settings in the nav footer. Wallet + Leaderboard merge
>    into **Earnings** (tabs: Overview / Payouts / Reliability / Leaderboard).
>    **Network** becomes a first-class page (the locations/providers picker that
>    is a drawer/sheet today). Settings is slimmed to preferences; account,
>    subscription, login methods, referral, network name and balance codes move
>    to **Account**. This DELIBERATELY breaks iOS/Android structural parity —
>    sanctioned by the product-tiering decision (desktop is the advanced line;
>    see `2026-08-06-ios-parity-native-shell.md` § "The product tiering").
>    Feature-level SDK calls do not change; only where each surface LIVES changes.
>
> 3. **Connect hero — KEEP the iOS globe (D3), REBALANCE it.** The spec's
>    "shrink the ornament substantially" is softened: the globe stays as the
>    brand hero, but the **disconnected** Home card leads with explicit
>    protection status text and the globe is sized so it never out-shouts the
>    status + Connect action. Do not delete or minimize the globe to an accent.
>
> 4. **The persistent bottom status strip STAYS.** The spec's "remove the bottom
>    status bar" is REJECTED — the owner built it deliberately (ProtonVPN
>    reference) and wants it on every page. Its Advanced-density fields arrive
>    later with Advanced Mode.
>
> **Palette:** keep the existing byte-matched brand palette in `UrColors.h`. The
> spec's hex fallbacks (`#5B7CFA` UR blue, etc.) map to existing tokens — do NOT
> introduce a new palette. The spec's *semantic* discipline (blue=action,
> green=protected, amber=warning, red=danger, lime=earnings only, never
> colour-alone) IS adopted, expressed through the existing tokens.
>
> **Advanced Mode (D5) is still queued.** The restructure must leave room for it:
> the Settings "Advanced" section is where the toggle lives, and the Developer
> destination folds behind it (it is NOT one of the five primary destinations —
> it lives under Settings › Advanced, revealed by Advanced Mode).
>
> ---
>
> ### 5. THE VISUAL MODEL: FULL-BLEED PANES, NOT CARDS (owner, 2026-08-08, after Wave 1)
>
> Owner's verdict on Wave 1: *"the UI has barely even changed... I think you're
> sticking to this style too much. Things need to fit in a fill, I guess go with
> Portmaster looks, less random sized modules and more fit in."*
>
> **This overrides the card-based layout language in this spec's body AND
> everything built through Wave 1.** Rebalancing cards was never going to fix it:
> floating cards with gutters, page padding and a max-width cap ARE the mobile
> model, and no amount of balancing changes that. Adopt Portmaster's structure:
>
> - **NO floating cards.** No page padding gutter, no `MaxWidth` content cap, no
>   card margins, no rounded islands drifting in dark space. Delete
>   `kWideBreakpointDip`/`kUltraWideDip` centring grids as the layout primitive.
> - **Full-bleed panes, edge to edge, floor to ceiling.** A destination is 2–3
>   vertical panes that each stretch the FULL height of the content area and
>   touch the window edges. The window is always completely covered.
> - **Hairline separators, not gaps.** 1px `#1F1F1F`-class rules divide panes and
>   rows. Separation comes from lines and a subtle fill step, never from empty space.
> - **Each pane owns a header strip** (~40px: title, count, actions) and scrolls
>   independently under it.
> - **Uniform rows.** One row height per list (~36–44px), one grid, one rhythm.
>   "Less random sized modules" means every module is the same shape — no card is
>   200px tall because that is what its content happened to need.
> - **Tables and lists FILL.** A list pane grows to the pane's height; empty
>   states are a centred line inside a full-height pane, never a short card.
> - **Density over air.** Portmaster shows dozens of rows per screen. Aim for
>   information density; this is the "advanced desktop" line (see the product
>   tiering), not the phone.
>
> Home in this idiom: left pane = connection state + primary action + options
> (full height); centre = live activity (connections/exits/throughput, filling
> vertically); right = detail/inspector for the selection. The hero globe stays
> (override 3) but lives INSIDE the left pane's flow, sized to the pane, not
> centred in a void.
>
> The bottom status strip (override 4) stays and now reads correctly as a status
> bar under full-bleed panes — the idiom it always belonged to.
>
> Everything below is the adopted design detail, subject to the four overrides
> above.

---

## Document purpose

This document is a screenshot-based UX review and redesign plan for the current URnetwork Windows desktop application.

The screenshots reviewed cover

- Connect
- Settings
- Leaderboard
- Wallet
- Account
- Connect while scrolled

This is a heuristic review rather than a usability test. It identifies visible interface problems, proposes a modern Windows 11 direction, defines a new information architecture, and gives implementation-ready specifications for layout, components, states, interaction, accessibility, and rollout.

## Executive summary

The current product has a solid dark visual foundation and a recognizable connection control, but it behaves more like a large web dashboard placed inside a desktop window than a purpose-built Windows VPN application.

The largest problems are

- The Connect page does not make protection status, route choice, and the connect action compact enough to scan instantly
- Navigation remains icon-only even in a very wide window
- The bottom status bar duplicates state and resembles developer telemetry
- Settings mix account, identity, VPN, device, community, diagnostics, and application preferences
- Several pages use only a small fraction of the available window
- Many cards are empty or oversized for the information they contain
- The product uses several unrelated accent colors and two visibly different heading styles
- Empty, loading, disabled, and error states are not clearly distinguished
- Advanced decentralized-network concepts are exposed before they are translated into user goals
- Core Windows behaviors such as adaptive NavigationView, native title-bar integration, Mica, accessible SettingsCard patterns, keyboard focus, and responsive effective-pixel layouts are not fully reflected

The redesign should make the app feel like a calm privacy utility.

The user should always be able to answer three questions within a glance

- Am I protected
- Where and how am I connected
- What should I do next

The recommended redesign has five top-level destinations

- Home
- Network
- Earnings
- Account
- Settings

Help and Diagnostics should be footer destinations. Wallet and Leaderboard should become sections within Earnings. Account identity and subscription content should move out of Settings. VPN behavior, privacy controls, device behavior, general preferences, and advanced diagnostics should remain in Settings.

The core Home page should use a compact connection card, a route selector, a single primary action, a concise protection summary, and an expandable privacy-options section. Share-bandwidth or provider behavior should be visually separate from the VPN connection state so users do not confuse consuming VPN service with contributing network capacity.

## Screenshot observations

An approximate pixel segmentation of the supplied screenshots found that empty background occupies

- Connect top view around 54 percent
- Connect scrolled view around 67 percent
- Settings around 67 percent
- Leaderboard around 91 percent
- Wallet around 67 percent
- Account around 83 percent

These values are not usability scores. They demonstrate that the current layouts do not use a large desktop window proportionally. Some whitespace is desirable, but the current screens combine very large empty areas with small labels, dense right-side controls, and long vertical scrolling.

## What is working

### A clear dark-mode identity

The app has a coherent dark baseline and restrained use of borders. The dark theme is appropriate for a network and privacy tool.

### A visible primary connection action

The Connect button is full width and visually distinct. The central connection motif communicates that the page is the primary VPN control.

### Useful feature coverage

The screenshots expose many useful capabilities

- Automatic provider selection
- Multiple connection profiles
- Fixed IP
- Enhanced anonymity
- Post-quantum encryption
- Ad and tracker blocking
- Kill switch
- Blocked locations
- Split tunneling
- Custom DNS
- Provider identity
- Wallet and payout information
- Reliability metrics
- Exportable logs

The redesign should preserve these capabilities while reducing their cognitive load.

### A reusable card language

Cards, section headers, switches, rows, and status metrics already exist. The product can evolve through a component-system refactor rather than a complete visual reset.

## Primary design problems

## App shell and navigation

### Icon-only navigation is too ambiguous

The left rail shows seven or more unlabeled icons. Several icons are not self-explanatory. A globe can mean Home, Connect, Locations, or Network. A trend arrow can mean Leaderboard, Performance, Earnings, or Statistics. A bug icon can mean diagnostics, report a problem, or development tools.

At the supplied window width there is enough room to display labels. The Windows NavigationView pattern supports expanded, compact, and minimal modes and is specifically designed to adapt between window sizes. [cit:https://learn.microsoft.com/en-us/windows/apps/develop/ui/controls/navigationview]

### Selected navigation is too subtle

The selected item uses a small pale vertical line plus a slightly lighter square background. This is easy to miss in peripheral vision and does not include a textual page label.

### Help, Settings, and Diagnostics are mixed into the main task list

These are secondary destinations and should be pinned to the bottom of the navigation pane. Settings should use the built-in bottom placement in NavigationView. [cit:https://learn.microsoft.com/en-us/windows/apps/design/app-settings/guidelines-for-app-settings]

### The bottom status strip duplicates information

The persistent bottom bar contains readiness, network name, selected provider, and traffic state. It resembles an IDE status bar rather than a consumer VPN application. The same readiness and provider information is already visible on the Connect page.

The bottom bar also consumes window height on every screen, including Account and Settings where it is not relevant to the immediate task.

### The title area does not feel fully native

The brand, profile avatar, and caption buttons occupy a custom strip, but the layout does not visually integrate with the navigation and content shell. A modern Windows app should use the system title-bar APIs, retain system caption behavior, define a reliable drag region, and use a 48 epx integrated title bar where appropriate. [cit:https://learn.microsoft.com/en-us/windows/apps/develop/title-bar]

## Information architecture

### Product concepts are grouped by implementation rather than user goal

The current structure separates Wallet, Leaderboard, Account, and Settings, but the content inside these pages overlaps.

- Wallet includes points, multipliers, payouts, and reliability
- Leaderboard is another earnings and contribution concept
- Account contains plan usage, balance codes, network name, and referrals
- Settings contains login methods, device information, subscription management, community links, account split rules, and account content

This increases the chance that users will search several destinations before finding a feature.

### Decentralized-network terminology is exposed too early

Terms such as provider identities, peer discovery, provide mode, network reliability, reliability weight, and post-quantum identity are meaningful to expert users but need plain-language framing.

The interface should present the user goal first and the technical term second.

Examples

- Share bandwidth instead of Provide mode
- Enhanced anonymity instead of Strong Anonymization
- Split tunneling instead of App split rules
- Route details instead of Provider details
- Device identity instead of Post Quantum Identity as the primary heading

## Visual hierarchy

### Large cards contain too little information

The connection card uses a very large decorative center illustration. The Wallet and Leaderboard empty-state cards span hundreds of pixels with one line of text. The Client statistics and Local statistics cards consume large vertical areas while displaying zero traffic.

### Important and secondary elements compete

The Connect page gives substantial space to

- The connection ornament
- Provide mode
- Connection profile
- Four privacy toggles
- Plan quota
- Statistics

This makes the core protection decision feel like one dashboard module among many.

### Typography is inconsistent

Page titles use a clean sans serif style. Several card-section headings use a much heavier, display-like face that appears inconsistent with the rest of the application.

Windows recommends Segoe UI Variable throughout the interface, regular weight for most text, semibold for titles, sentence case, and a standard effective-pixel type ramp. [cit:https://learn.microsoft.com/en-us/windows/apps/design/signature-experiences/typography]

### Accent colors do not have stable meaning

The screenshots use

- Electric blue for the central connection control and usage
- Pale lime for the Connect button and selected-navigation indicator
- Cyan for segmented-control underlines
- Purple and green for reliability
- Orange or red for pending and destructive actions

The result feels assembled from several visual systems. One primary action accent and a smaller semantic palette will make the interface calmer and more predictable.

## Interaction and state communication

### Readiness is not the same as protection

The text `joebanana is ready to connect` emphasizes the account name and a backend-ready state. A VPN app should lead with the actual privacy state.

Recommended copy

- Not connected
- Your internet traffic is not protected
- Protected
- Connected through Frankfurt, Germany
- Connection interrupted
- Reconnecting while the kill switch blocks traffic

### Provider selection is visually understated

The selected provider appears as text within the hero card, followed by a chevron. It should look and behave like a clear route or location picker.

### Connection and provider mode are conflated

Provide mode sits close to connection options even though providing capacity is conceptually different from consuming a VPN route. These should have separate state models and separate visual sections.

### Disabled and loading states are unclear

`Attaching device controls...` appears in several Settings rows. It is unclear whether this is

- A loading state
- A permissions state
- A device compatibility problem
- A backend failure
- An unfinished feature

Loading should be time-bounded and use a progress indicator. Disabled controls should explain why they are unavailable. Errors should use explicit error text and recovery actions.

### Empty states do not help the user progress

Examples include

- No networks on the leaderboard yet
- No payouts yet
- No traffic yet
- Empty statistics cards

Each empty state should explain what creates the data and provide a relevant action.

## Proposed product structure

## Primary navigation

### Home

Purpose

- Connect and disconnect
- Confirm protection state
- Select location or route
- See current session information
- Access common privacy options

### Network

Purpose

- Browse locations and providers
- Search and filter routes
- View latency, load, trust, capabilities, and availability
- Manage favorites
- View blocked locations

### Earnings

Purpose

- Connect payout wallet
- View points and estimated earnings
- See payout history
- Understand earning multipliers
- View contribution reliability
- View leaderboard

Suggested internal tabs

- Overview
- Payouts
- Reliability
- Leaderboard

### Account

Purpose

- Plan and usage
- Network name and verification
- Referral code
- Balance codes
- Login and security methods
- Subscription management

### Settings

Purpose

- VPN and privacy defaults
- Device behavior
- General application preferences
- Advanced diagnostics and About

## Footer destinations

- Help
- Diagnostics
- Settings

The avatar in the title bar can open a lightweight account flyout with plan, account name, Account, and Sign out.

## Modern Windows shell specification

Microsoft's current WinUI structure guidance combines Mica, an integrated title bar, a left NavigationView, transparent page backgrounds, and InfoBar status messaging. [cit:https://learn.microsoft.com/en-us/windows/apps/develop/ui/windows-app-sdk-app-structure]

### Window

- Recommended launch size 1180 by 760 epx
- Minimum supported size 720 by 520 epx
- Remember the last non-minimized size and position
- Support Windows snap layouts and arbitrary resizing
- Do not introduce horizontal page scrolling
- Use effective pixels rather than physical screen pixels

### Title bar

- Height 48 epx
- Left content includes a 16 or 20 epx app icon and `URnetwork`
- The full top center remains a draggable region
- Right content may include a compact connection-state button and account avatar
- Leave system minimize, maximize, restore, and close buttons system-managed
- In inactive state reduce title text and custom icon contrast
- Do not put the main Connect action in the title bar

### Backdrop and surfaces

- Use Mica for the primary window backdrop
- Use transparent page roots so Mica remains visible
- Use solid or semi-opaque card fills from Windows theme resources
- Use Acrylic only for transient flyouts and menus
- Provide a solid-color fallback when Mica is unavailable or transparency is disabled

Mica is intended as the foundation layer for primary windows, while Acrylic is intended for transient surfaces such as flyouts. [cit:https://learn.microsoft.com/en-us/windows/apps/develop/ui/system-backdrops]

### Navigation pane

- Expanded width 220 epx
- Compact width 56 epx
- Item height 44 epx
- Icon size 20 epx
- Item horizontal padding 12 epx
- Show icon and text labels in expanded mode
- Show tooltips in compact mode
- Use a 3 epx selected indicator plus selected background
- Put Help, Diagnostics, and Settings in the footer

Adaptive behavior

- Large windows at 1008 epx and above use expanded left navigation
- Medium windows from 641 through 1007 epx use compact icon navigation
- Small windows at 640 epx or below use a menu button and overlay pane

These match the current Windows responsive categories and NavigationView behavior. [cit:https://learn.microsoft.com/en-us/windows/apps/design/layout/screen-sizes-and-breakpoints-for-responsive-design] [cit:https://learn.microsoft.com/en-us/windows/apps/develop/ui/controls/navigationview]

### Page frame

- Page content margin 24 epx on medium and large windows
- Page content margin 12 to 16 epx on small windows
- Maximum general page width 1240 epx
- Maximum Settings content width 1040 epx
- Page title uses Title style at 28 by 36 epx
- Optional page description uses Body style and a maximum reading width of approximately 60 characters
- Gap below page header 24 epx

## Home page specification

## Large-window layout

Use a two-column grid.

- Main column minimum 620 epx and flexible
- Side column 340 to 380 epx
- Column gap 16 epx
- Main content is vertically aligned to the top

The first viewport should contain

- Page header
- Protection state and main action
- Route selector
- Essential session information
- Common connection profile
- A compact quota warning only when relevant

It should not require scrolling to determine whether the VPN is active.

## Connection card

Recommended dimensions

- Minimum height 300 epx while disconnected
- Padding 24 epx
- Corner radius 8 epx
- One subtle card border

Top row

- Status icon 32 epx
- Status label such as `Not connected`
- Optional status badge such as `Kill switch active`

Body

- Primary headline 28 epx semibold
- One-line consequence or confirmation message
- Optional compact shield illustration no larger than 120 epx
- Do not use a decorative illustration larger than the status and controls combined

Route row

- Label `Location`
- Value `Automatic — fastest available`
- Optional secondary value `Best available route`
- Optional latency when known
- Entire row is clickable
- Chevron at the trailing edge
- Minimum height 56 epx

Primary command row

- Primary button minimum width 160 epx
- Button height 40 epx, or 44 epx for a touch-forward build
- Secondary route or details button only if needed
- On narrow screens the primary button expands to the card width

## Connection state model

### Disconnected

- Icon and badge use neutral styling
- Headline `Not connected`
- Supporting text `Your internet traffic is not protected`
- Primary action `Connect`
- Route and options remain editable

### Connecting

- Headline `Securing your connection`
- Supporting text shows the selected destination
- Primary command becomes `Cancel`
- Use a determinate phase label when possible
  - Finding a route
  - Verifying provider
  - Creating encrypted tunnel
- Avoid an endless indeterminate animation with no text

### Connected

- Headline `Protected`
- Supporting text `Connected through Frankfurt, Germany`
- Display duration, latency, and transferred data
- Primary action `Disconnect`
- Show public IP or route details behind a disclosure if privacy policy permits
- Use a check or shield icon plus text rather than green color alone

### Reconnecting

- Headline `Connection interrupted`
- Supporting text `Reconnecting while the kill switch blocks traffic`
- Show a persistent Warning InfoBar
- Provide `Disconnect` and `Troubleshoot`

### Error

- Headline `Couldn’t connect`
- Explain the failure in user language
- Primary action `Try again`
- Secondary action `Choose another location`
- Tertiary link `View diagnostics`

### Offline

- Headline `No internet connection`
- Action `Open Network settings`
- Keep VPN settings visible but disable Connect with an explanation

## Route picker

Open a dedicated Network page or a large flyout depending on window width.

Contents

- Search field
- `Automatic` recommendation at the top
- Favorites
- Recent locations
- All locations
- Optional Providers tab for expert users

Location row

- Country flag or region icon
- City and country
- Latency
- Relative load
- Capability icons for streaming, fixed IP, post-quantum support, and enhanced anonymity
- Favorite command

Default sort

- Recommended score that balances latency, availability, and capability

Never expose a raw provider identifier as the only meaningful selection label.

## Connection profile

Replace the underlined Auto, Web, and Streaming tabs with a labeled control.

Label

- Connection profile

Options

- Automatic
- Browsing
- Streaming

Use a ComboBox on compact layouts or a three-option segmented control when sufficient width is available.

Description examples

- Automatic — balances speed and privacy
- Browsing — optimized for everyday web traffic
- Streaming — prioritizes stable high-throughput routes

## Privacy options

Use a card titled `Privacy options`.

Default visible rows

- Enhanced anonymity
- Block ads and trackers

Expandable advanced rows

- Fixed IP
- Post-quantum encryption
- Custom DNS
- Split tunneling

Each row includes

- 20 epx icon
- Short title
- One-line description
- Toggle or navigation chevron
- Optional `Pro` badge

If a feature is unavailable

- Keep the row visible
- Disable the control
- Explain the reason
- Provide an Upgrade command only where it is relevant

## Share-bandwidth section

Rename `Provide mode` to `Share bandwidth`.

This section must be visually separate from the VPN connection card because it represents a different user goal and may continue when the VPN tunnel is disconnected.

Primary control

- Toggle `Share bandwidth`

Schedule control shown when enabled

- When the app is open
- Only on trusted networks
- Always

Additional information

- Estimated contribution
- Current state such as Standby or Providing
- Data shared today
- Link to privacy and resource-use explanation

Avoid the current Auto, Always, Network, Never set because the nouns do not form a clearly comparable set.

## Session details

When disconnected

- Do not render large zero-value graph cards
- Show one compact empty state
- Copy `Session statistics will appear after you connect`

When connected

- Show Download, Upload, Blocked traffic, and Active route as four compact metrics
- Offer a `View details` disclosure
- Use a small real-time chart only when enough data exists
- Pause chart animation when the window is not active

## Plan and quota

Remove the large permanent Plan card from the Home right rail.

Instead

- Show a compact usage item in the navigation footer or Account flyout
- Show an inline quota warning only when usage exceeds a defined threshold
- Keep full plan, balance, referral, and upgrade information on Account

## Settings redesign

The current Settings page mixes unrelated categories and uses a two-column dashboard layout. Windows guidance recommends a single scrollable column with a constrained width around 1000 to 1100 epx, grouped SettingsCard and SettingsExpander controls, immediate application of simple changes, and About at the bottom. [cit:https://learn.microsoft.com/en-us/windows/apps/design/app-settings/guidelines-for-app-settings]

## Settings landing page

Maximum width

- 1040 epx

Sections

- General
- VPN and privacy
- Device
- Advanced
- About

Use section headings and SettingsCard rows rather than several independent dashboard cards.

## General

Rows

- Start URnetwork when I sign in
- Connect automatically
- Minimize to notification area
- Notifications
- Theme
- Language when localization is supported

Theme choices

- Use system setting
- Light
- Dark

## VPN and privacy

Rows

- Kill switch
- Connection profile
- Enhanced anonymity
- Post-quantum encryption
- Block ads and trackers
- Blocked locations
- Split tunneling
- Custom DNS

Rules

- Toggles apply immediately
- Navigation rows use chevrons
- Advanced dependencies appear in a one-level SettingsExpander
- Do not require a generic Save button for simple toggles

## Device

Rows

- Device name
- Share-bandwidth behavior
- Trusted networks
- Device identity
- Provider identities

`Attaching device controls...` should be replaced by one of the following explicit states

- Skeleton row for no more than a short loading interval
- `Unavailable on this device`
- `Administrator permission required`
- `Couldn’t load device details` with Retry

## Advanced

Rows

- Diagnostics
- Export logs
- Reset network configuration
- Experimental features if any

Export logs should explain

- What information is included
- Whether addresses or identifiers are redacted
- The selected save location

Destructive reset actions require a confirmation dialog with a safe Cancel command and action-specific button text. [cit:https://learn.microsoft.com/en-us/windows/apps/develop/ui/controls/dialogs-and-flyouts/dialogs]

## About

Use a collapsed SettingsExpander at the bottom.

Contents

- App name and icon
- Version and build
- Release notes
- Privacy policy
- Terms
- Community
- Report a problem

Move Discord and project links here rather than keeping them in a large `Stay in touch` card among operational settings.

## Account redesign

Maximum width

- 1040 to 1120 epx

Suggested sections

- Plan and usage
- Profile
- Security
- Referrals
- Balance codes

## Plan and usage

Show

- Plan name
- Renewal or expiration status where relevant
- Used, pending, and available data as exact numbers
- Progress visualization
- Upgrade or manage subscription command

Rules

- Never rely on the bar colors alone
- Use consistent binary units
- Clarify whether daily balance resets, carries over, or expires
- Move `Manage Subscription` from Settings to this section

## Profile

Rows

- Network name
- Verification state
- Client ID with masked default and Copy command

Editing behavior

- Use inline edit mode
- Save appears only while editing
- Show a confirmation message after success
- Show validation beside the field

## Security

Rows

- Login methods
- Password
- Seed phrase
- Solana method
- Temporary authentication code
- Signed-in devices if supported

Destructive `Remove` commands should not appear as bright red buttons in every default row. Use a trailing menu or secondary button. Show red styling only when the user enters a destructive confirmation context.

## Referrals

Show

- Referral code
- Copy
- Share
- Number of successful referrals
- Monthly benefit
- Short explanation

## Balance codes

Use a compact data table with

- Code
- Amount
- Redeemed date
- Expiration
- Status

Move `Redeem Balance Code` into this section as a clear button.

## Earnings redesign

Rename Wallet to `Earnings`.

This name matches the actual page content, which includes wallet setup, points, multipliers, reliability, and payouts.

## Earnings overview

Top summary

- Estimated earnings
- Available for payout
- Points this cycle
- Reliability score

If a wallet is not connected

- Show a single onboarding card
- Explain supported networks
- Provide `Connect wallet`
- Explain that URnetwork does not custody the wallet

Wallet entry should include

- Network selector
- Wallet address field
- Format validation
- Clear warning if the address network is incompatible
- Confirmation summary before final connection

## Points breakdown

Replace the wide card with

- Total points
- Payout points
- Referral points
- Reliability points
- Current payment-cycle date range

Add `How points work` behind a disclosure.

## Multipliers

Show multiplier cards with

- Name
- Current multiplier
- Eligibility status
- Required action
- Expiration if relevant

Do not show a generic verification button without explaining the result.

## Reliability

The existing chart combines values with different units and does not show a clear time axis.

Replace it with

- KPI `Reliability score`
- KPI `Active client count`
- KPI `Uptime`
- One time-series chart for reliability over time
- Optional secondary client-count chart or overlay using a clearly separate axis

Chart requirements

- Visible x-axis time range
- Visible y-axis scale or meaningful normalized label
- Tooltip values
- Legend labels in plain language
- No chart when fewer than two meaningful samples exist
- Use `Not enough data yet` instead of a flat zero line

## Payout history

Use a data table with

- Date
- Amount
- Asset
- Network
- Wallet
- Status
- Transaction command

Empty-state copy

- `No payouts yet`
- `Payouts will appear here after you connect a wallet and complete an earning cycle`
- Action `Learn how payouts work`

## Leaderboard redesign

Move Leaderboard into Earnings while preserving a direct navigation route if it is a major growth feature.

Top controls

- Payment-cycle selector
- Search
- Current ranking summary
- Privacy toggle `Show my network name`

Ranking table

- Rank
- Network
- Data provided
- Reliability
- Change

Behavior

- Highlight the current account row
- Pin the current account summary above the table if it is outside the visible range
- Explain the update cycle in a tooltip or help link
- Replace `.` or blank rank values with `Not ranked`

Empty state

- Illustration no larger than 96 epx
- Headline `No ranked networks yet`
- Supporting copy explaining qualification
- Action `Start sharing bandwidth`

The current page leaves approximately 91 percent of the screenshot as empty background. The redesign should use a constrained, centered content column and a purposeful empty state rather than a narrow card floating at the top.

## Visual design system

## Typography

Use Segoe UI Variable throughout.

Type tokens

- Caption 12 by 16 epx regular
- Body 14 by 20 epx regular
- Body strong 14 by 20 epx semibold
- Body large 18 by 24 epx regular
- Subtitle 20 by 28 epx semibold
- Title 28 by 36 epx semibold
- Title large 40 by 52 epx semibold only for rare marketing or onboarding moments

Rules

- Sentence case for all UI labels
- Semibold rather than bold for emphasis
- Avoid italic body copy
- Left align operational content
- Center text only in intentional empty states
- Keep helper-text line length near 50 to 60 characters where possible

These values follow the Windows type ramp. [cit:https://learn.microsoft.com/en-us/windows/apps/design/signature-experiences/typography]

## Color

Prefer Windows theme resources over hard-coded colors.

Fallback dark tokens

- Window backdrop — Mica Base
- Page layer — transparent
- Card fill — approximately `#1C1C1C` to `#242424` depending on theme resource
- Card border — white at 8 to 10 percent opacity
- Text primary — `#F5F5F5`
- Text secondary — `#C8C8C8`
- Text tertiary — `#9A9A9A`
- Action accent — UR blue near `#5B7CFA`
- Action accent hover — a lighter blue
- Action accent pressed — a darker blue
- Earnings accent — pale lime used only for rewards and plan identity
- Success — green
- Warning — amber
- Danger — red

Color semantics

- Blue means interactive action or selected state
- Green means successfully protected or completed
- Amber means attention, reconnecting, pending, or approaching quota
- Red means connection failure, destructive action, or blocked critical state
- Lime is reserved for rewards, contribution, or premium branding

Do not use blue alone to mean both selected and connected. Every state color must be paired with an icon and text.

## Spacing

Use a 4 epx base scale.

Tokens

- 4
- 8
- 12
- 16
- 24
- 32
- 48

Common uses

- 8 between closely related controls
- 12 between label and control groups
- 16 card padding on compact rows
- 24 card padding on major cards
- 24 page margins on medium and large windows
- 32 between major page sections

Windows guidance uses consistent 8, 12, and 16 epx relationships to create grouping. [cit:https://learn.microsoft.com/en-us/windows/apps/design/basics/content-basics]

## Corners and elevation

- Control corner radius 4 epx
- Card corner radius 8 epx
- Large hero or empty-state corner radius 8 epx
- Pills and status badges use full radius
- Use borders and material separation before adding shadow
- Use shadow only for transient raised content

## Icons

- Use one icon family, preferably Segoe Fluent Icons or Fluent system icons
- Standard row icon 20 epx
- Command icon 16 epx
- Status icon 24 to 32 epx
- Do not mix line weights
- Do not use icon-only navigation in large-window mode
- Every icon-only command requires an accessible name and tooltip

## Buttons

Primary

- One primary action per visual region
- Filled accent background
- Minimum 40 epx height
- Minimum 120 epx width for touchability when practical

Secondary

- Standard neutral fill

Subtle

- Used for low-priority commands such as Details or Learn more

Danger

- Use only in destructive confirmation contexts

## Toggles

- Use for binary settings that apply immediately
- Keep title and description on the left
- Align switch on the right
- Default to explicit On and Off state labels for accessibility when space permits
- If the choice is not truly binary, use RadioButtons, a segmented control, or ComboBox

Windows toggle guidance defines the switch as an immediate binary action. [cit:https://learn.microsoft.com/en-us/windows/apps/develop/ui/controls/toggles]

## Touch and pointer targets

- Minimum target 40 by 40 epx
- A wide control may be 32 epx tall only when at least 120 epx wide
- Touch-optimized mode may use 44 by 44 epx
- Maintain at least 4 epx visible space between touch targets

These values follow current Windows touch guidance. [cit:https://learn.microsoft.com/en-us/windows/apps/develop/input/touch-interactions]

## Status messaging

Use InfoBar for persistent non-modal application or connection state changes.

Examples

- Internet connection lost
- Reconnecting
- Subscription expired
- Update available
- Could not load device controls

Use inline field errors for form validation.

Use dialogs only when the user must acknowledge a severe state or confirm a consequential action.

InfoBar is designed for visible but non-intrusive changed-state messages. [cit:https://learn.microsoft.com/en-us/windows/apps/develop/ui/controls/infobar]

## Motion

Motion should clarify state, not decorate.

Durations

- Hover and press feedback 80 to 120 ms
- Expand and collapse 160 to 200 ms
- Connection state transition 180 to 240 ms
- Page transition 180 to 220 ms

Connection motion

- Disconnected shield remains still
- Connecting uses a subtle progress ring or moving highlight
- Connected resolves into a stable check or shield
- Reconnecting uses warning motion no faster than necessary

Rules

- Respect reduced-motion settings
- Avoid continuous decorative pulsing
- Stop nonessential chart animation while the app is inactive
- Keep animations smooth at 60 frames per second where hardware permits

## Accessibility specification

### Contrast

- Normal visible text minimum 4.5 to 1
- Large text minimum 3 to 1
- Interactive boundaries and meaningful icons minimum 3 to 1 against adjacent colors
- Test the default theme rather than relying on high-contrast mode as a fallback
- Test all custom states in Windows contrast themes

Windows accessibility guidance requires at least 4.5 to 1 for visible text and recommends testing high-contrast themes and avoiding color-only meaning. [cit:https://learn.microsoft.com/en-us/windows/apps/design/accessibility/accessibility-checklist]

### Keyboard

- Every command is reachable by keyboard
- Tab order follows visual order
- Arrow keys operate lists, radio groups, tabs, and segmented controls
- Enter and Space activate the focused command as appropriate
- Escape closes flyouts and non-destructive dialogs
- Alt plus Left navigates back
- Ctrl plus Comma opens Settings
- F1 opens Help
- Provide an optional shortcut for Connect and Disconnect and show it in tooltips or menus

### Focus

- Use a visible 2 epx focus outline or an equivalent area
- Maintain at least a 3 to 1 contrast change between focused and unfocused states
- Do not remove native focus visuals unless an equal or stronger replacement is provided

WCAG focus appearance guidance describes a 2 CSS pixel perimeter-equivalent area and a 3 to 1 contrast change as the benchmark for strong focus visibility. [cit:https://www.w3.org/WAI/WCAG22/Understanding/focus-appearance.html]

### Screen readers

- Set accessible names for every icon-only control
- Associate labels and input fields
- Announce connection-state changes through a live region
- Announce asynchronous failures and retry availability
- Do not put static text into the Tab sequence
- Expose chart summaries as text
- Give progress indicators meaningful labels

### Text scaling and DPI

- Support Windows text-size settings
- Verify layouts at 100, 125, 150, 175, and 200 percent display scale
- Verify text scaling to the maximum supported setting
- Allow rows and cards to grow vertically
- Never clip critical status or action text

### Localization

- Allow at least 30 percent text expansion
- Avoid fixed widths based on English strings
- Support RTL mirroring
- Use locale-aware dates, times, numbers, and units
- Do not concatenate translated UI fragments

## Component inventory

### AppShell

Contains

- TitleBar
- NavigationView
- Page frame
- Global InfoBar host
- Account flyout

### PageHeader

Properties

- Title
- Description
- Primary command
- Secondary commands

### ConnectionHero

Properties

- Connection state
- Destination
- Provider
- Latency
- Duration
- Primary action
- Status badges

### RoutePicker

Properties

- Search
- Filters
- Recommended route
- Favorites
- Recent locations
- Location list
- Provider details

### SettingsCard

Properties

- Icon
- Header
- Description
- Control or chevron
- Enabled state
- Disabled reason
- Pro badge

### MetricCard

Properties

- Label
- Value
- Unit
- Change
- Status

### EmptyState

Properties

- Icon or illustration
- Headline
- Description
- Primary action
- Optional secondary action

### CopyField

Properties

- Label
- Masked value
- Reveal
- Copy
- Copy confirmation

### DataTable

Properties

- Sort
- Filter
- Keyboard row navigation
- Empty state
- Loading state
- Responsive column priority

### ChartCard

Properties

- Title
- Time range
- KPI summary
- Plot
- Legend
- Accessible text summary
- Not-enough-data state

## Responsive behavior

## Large at 1008 epx and above

- Expanded navigation
- Two-column Home
- Tables show all priority columns
- Settings remains one constrained column
- Earnings overview may use a responsive metric grid

## Medium from 641 through 1007 epx

- Compact navigation
- Home side panel stacks below the connection card
- Tables hide low-priority columns behind row details
- Page padding 24 epx

## Small at 640 epx and below

- Minimal navigation with overlay pane
- One-column layout
- Page padding 12 to 16 epx
- Full-width primary buttons
- Route picker opens as a page rather than a narrow flyout
- Charts use simplified legends
- Data tables become list-detail rows

## Content and copy guidelines

### Write the user outcome first

Preferred

- Protected
- Not connected
- Share bandwidth
- Choose a location
- Block ads and trackers
- Split tunneling

Avoid as primary labels

- Ready to connect
- Provide mode
- Provider identities
- App split rules
- Attaching device controls

### Explain technical features in one sentence

Examples

- Enhanced anonymity — routes traffic through additional providers for stronger privacy
- Fixed IP — keeps the same public IP during supported connections
- Post-quantum encryption — uses a tunnel designed to resist future quantum attacks
- Split tunneling — choose which apps bypass URnetwork
- Kill switch — blocks internet traffic if the VPN disconnects unexpectedly

Final technical wording should be validated by engineering and security teams.

### Use precise command labels

Preferred

- Connect
- Disconnect
- Try again
- Choose another location
- Export logs
- Remove login method
- Redeem code

Avoid

- OK
- Save when no editable state exists
- Add without naming what will be added
- Remove without naming the affected item in a confirmation

## Priority backlog

## P0 before visual polish

- Replace readiness copy with explicit protection state
- Make navigation adaptive and labeled on large windows
- Remove or drastically simplify the bottom status strip
- Rebuild the first Home viewport around one status and one primary action
- Separate Share bandwidth from VPN connection options
- Clarify all loading, unavailable, and error states
- Reorganize Settings into a constrained single-column hierarchy
- Move account, subscription, and login content out of general Settings
- Replace oversized zero-data cards with compact empty states
- Standardize typography and remove the alternate heavy display face
- Ensure all interactive targets meet minimum size
- Implement keyboard navigation, focus visuals, and accessible names

## P1

- Merge Wallet and Leaderboard into Earnings
- Build the route and location picker
- Redesign reliability metrics and charts
- Implement Mica, integrated title bar, and native NavigationView
- Add high-contrast, text-scaling, RTL, and reduced-motion support
- Add structured InfoBar state handling
- Add responsive table-to-list behavior
- Add clear Pro feature gating and disabled explanations

## P2

- Add notification-area behavior
- Add connection shortcuts
- Add favorites and recent locations
- Add richer payout and contribution insights
- Add optional onboarding or teaching tips
- Add polished transitions and lightweight illustration

## Delivery plan

## Phase 0 product and technical alignment

Estimated duration

- One week

Deliverables

- Confirm top user journeys
- Inventory every current setting and backend state
- Define connection and provide-mode state machines
- Confirm Windows technology stack
- Define analytics events
- Validate terminology with legal, security, and support

## Phase 1 design system and shell

Estimated duration

- One to two weeks

Deliverables

- Design tokens
- Typography
- Theme resources
- Mica and solid fallback
- Native title bar
- Adaptive NavigationView
- Core cards, buttons, settings rows, InfoBar, dialog, empty state
- Keyboard and focus baseline

## Phase 2 Home and Network

Estimated duration

- Two weeks

Deliverables

- All connection states
- Route picker
- Connection profiles
- Privacy options
- Share-bandwidth section
- Session metrics
- Responsive layouts

This should be the first user-testable release because it contains the core product value.

## Phase 3 Settings and Account

Estimated duration

- One to two weeks

Deliverables

- New Settings information architecture
- Plan and usage
- Profile and network name
- Login methods and security
- Referrals and balance codes
- Diagnostics and About

## Phase 4 Earnings

Estimated duration

- Two weeks

Deliverables

- Wallet onboarding
- Earnings overview
- Points
- Multipliers
- Reliability
- Payout history
- Leaderboard

## Phase 5 validation and release hardening

Estimated duration

- One to two weeks

Deliverables

- Accessibility audit
- Narrator and keyboard test pass
- Contrast-theme test pass
- DPI and text-scaling matrix
- Localization pseudo-language test
- Performance profiling
- Telemetry validation
- Error and recovery test matrix

## Acceptance criteria

### Core usability

- The user can identify connected or disconnected state within one second
- The user can connect from Home with one primary action
- The user can change the automatic destination in no more than two actions
- The user is never shown both a ready state and a protected state as if they mean the same thing
- Share-bandwidth state is visually distinct from VPN state

### Responsive layout

- No horizontal overflow at 720 by 520 epx
- All pages remain usable at 200 percent display scaling
- Text scaling does not clip page titles, status, labels, or primary commands
- Expanded navigation appears on large windows and compact or minimal navigation appears at smaller widths

### Accessibility

- Every workflow is keyboard complete
- Every icon-only control has an accessible name
- Logical Tab order is verified on every page
- Visible body text meets 4.5 to 1 contrast
- Meaningful non-text indicators meet 3 to 1 contrast
- Focus is visible on every interactive element
- Connection and error state changes are announced to Narrator
- Color is never the only state indicator

### State quality

- No loading message remains indefinitely without timeout, error, or retry
- Disabled controls explain why they are disabled
- Empty states explain how data will be created
- Errors provide a recovery command
- Destructive actions use specific confirmation copy

### Visual consistency

- All UI uses Segoe UI Variable or platform fallback
- One primary action accent is used
- Card radius, border, spacing, and typography use shared tokens
- No page uses the alternate heavy display typeface visible in the supplied Settings and Wallet section headings

## Recommended design direction in one sentence

Turn URnetwork from a sparse technical dashboard into a compact, native-feeling Windows privacy utility where protection state is unmistakable, advanced decentralized features are progressively disclosed, and every page uses the window with purpose.

