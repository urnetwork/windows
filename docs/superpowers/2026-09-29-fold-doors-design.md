# Fold doors: the monetization panes that fold away with no second door

2026-09-29. Design only, no code. The fold rule (MainWindow.xaml.cpp:625-628): a foldable pane
must not own content with no second door. Four surfaces break it today. Gates live in
ApplyBreakpoint: earnings pane C <1500 (MainWindow.xaml.cpp:574-576), earnings pane A <900
(577-579), account codes <1900 / extenders <1500 / plan <900 (609-617).

## The two shipped precedents, and the third option they imply

- BUILD-TWICE: Support's pane B is built twice (SettingsPage::BuildSupportContactSection,
  SettingsPage.cpp:221-222), once into the pane, once into SupportContactInline under Send
  (MainWindow.xaml:3100-3104); ApplyBreakpoint flips the inline copy below the gate
  (MainWindow.xaml.cpp:657-660). Safe there only because the section keeps no per-row state.
- ACCEPT THE FOLD: Developer's overrides fold with no second door because the rows' indexed
  wiring is single-instance (boolRows_/numRows_, MainWindow.xaml.cpp:676-682). Accepted because
  the surviving half is what the task runs from; not acceptable for monetization actions.
- COMMAND-DOOR DUPLICATION (what the precedents imply): when the folded content is only a DOOR
  to a sheet or a navigation, duplicate the door, not the surface - one row button or menu item
  calling the same member handler. No element is built twice; single-instance wiring is
  untouched.

## Single-instance check (recon's warning, verified in-file)

No builder in scope is build-twice-safe; every one keeps handler/render state in members:

- WalletPage::BuildPointsNetworkHost (WalletPage.cpp:2765-2882): pointsTiles_[3],
  pointsPublicToggle_, editEmojiButton_, pointsNameText_ etc. (WalletPage.h:521-529).
- AccountPage::BuildExtenderPane (AccountPage.cpp:614-710): extenderBuilt_ build-once guard
  (615-616), extenderDnsBox_/Gossip/Hosts, advancedPanel_/advancedOpen_, private*/share*/import*
  fields, and the extenderLabels_ re-localization registry (619-639).
- SettingsPage::BuildSubscriptionSection (SettingsPage.cpp:613-620) keeps manageSubscription_.
- Wallet/account pane A and C controls are XAML x:Name elements - single-instance by construction.

REPARENT-EXISTING-ROWS is therefore the only duplication-free move for whole surfaces. A named
element keeps every member-field reference and handler when moved between parents; the codebase
had exactly this helper before R3 ("no Reparent", MainWindow.xaml.cpp:496-498). Reference-based
writers keep working across a reparent: ShowPointsBoard's visibility flips
(WalletPage.cpp:2886-2889) and the extenderLabels_ registry hold elements, not tree paths.

## Earnings pane A (wallet rail), folds <900dip - survivor: the ledger pane

Content (MainWindow.xaml:2303-2585): points figure (2326-2338), Bittensor connect/change/overflow
(2379-2429), Solana overflow (2484-2490), Claim SN25a (2526-2530), Top-200 claim (2560-2563),
Upgrade (2577-2579). Doors that already exist: every action is a sheet or flow reached through a
member handler - ClaimAlphaSheet (OnClaimAlpha), the bridge/manual connect (OnConnectWallet,
OnChangeWallet, OnWalletMore), the Solana menu (OnSolanaWalletMore), UpgradeSheet
(MainWindow::OnOpenUpgrade -> ShowUpgradeSheet, MainWindow.xaml.cpp:2149-2172). Strings exist
(WalletPage.cpp:595-604: claim, claim_your_spot, upgrade_with_stripe, top200).

Proposal: COMMAND-DOOR DUPLICATION. One "Earnings actions" overflow (MenuFlyout) in the ledger
pane's header beside EarningsTableBar (MainWindow.xaml:2605-2610), built once in code, items
calling the same member handlers. The points figure is a reading, not a control - fold it per
the codes-table precedent (MainWindow.xaml.cpp:590-591, 603-607). Upgrade's other door (Account
pane A, MainWindow.xaml:1946-1948) folds at the same 900 gate, so it does not count as a second
door here.

## Earnings pane C, folds <1500dip - survivor: panes A+B

Content (MainWindow.xaml:2708-2956): points network block (PointsNetworkHost, 2727), own-rank +
leaderboard public toggle (DataRankingHost, 2737-2785), VerifySeeker (2805-2807), provider
transport row (2946-2948), plus read-only reliability/stats/provide-mode/extender rows.

- The two toggles are direct API writes with echo guards (OnLeaderboardPublicToggled,
  WalletPage.cpp:2556-2570; the points opt-in, 2854-2866) - not sheet doors, so the rows
  themselves must move. REPARENT: PointsNetworkHost and DataRankingHost are each ONE named
  StackPanel; move them into a new WalletPaneCFold host at the foot of pane A's scroll (before
  WalletInfo, MainWindow.xaml:2580-2582), moved back when earningsThree re-opens.
- Provider transport: the sheet's only door is this row (ShowProviderTransportSettingsSheet,
  WalletPage.cpp:3978-3995; Connect's only transport door is Client-kind, ConnectPage.cpp:1325).
  Reparent WalletProviderTransportBarRow with the toggles, or duplicate the door into the <900
  overflow above. Same for VerifySeeker (a command: OnVerifySeeker).
- Reliability/stats/provide-mode/extender rows: readings plus rows whose door is Connect
  navigation (MainWindow.xaml.cpp:2563-2570) - fold accepted, precedent as above.
- At <900 pane A folds too: the reparented rows move with the fold host into a 44px-minimal foot
  strip of the ledger pane (option i, preferred - wiring untouched), or the two Toggled handlers
  are refactored to take an explicit `requested` and ride the overflow as checkable items
  (option ii - only if fill-to-floor is held absolute, MainWindow.xaml.cpp:566-572).

## Account plan pane (pane A), folds <900dip - survivor: account pane B

Content (MainWindow.xaml:1905-2068): plan value (1931), usage bar + figures (1940-2046), Upgrade
(1946-1948), Redeem (2049-2063), Manage Subscription (AccountPlanExtraHost, 2066).
Doors: UpgradeSheet and RedeemCodeSheet (MainWindow.xaml.cpp:2163-2192), the Stripe portal
(SettingsPage.cpp:618-619); plan/usage READINGS already have a second door - the status strip
and tray (MainWindow.xaml.cpp:592-594).

Proposal: REPARENT the three action rows (AccountUpgradeButton, RedeemRowButton, the
manageSubscription_ row) into a fold host at the foot of AccountPaneB's scroll, flipped with
accountTwo. Figures and usage bar fold as readings. No new strings: upgrade_with_stripe, the
Redeem row's key, and site_app_manage_subscription all ship (SettingsPage.cpp:618;
WalletPage.cpp:604).

## Account extenders pane (pane D), folds <1500dip - survivor: panes A+B

Content: one named host, AccountExtenderHost (MainWindow.xaml:2195), built once by
AccountPage::BuildExtenderPane with all state in members (above). Share/import sheets exist
(AccountPage.cpp:942-963) but their only door is this pane; the dns/gossip/hosts fields have no
sheet at all.

Proposal: REPARENT AccountExtenderHost whole into a fold host in AccountPaneB (which survives to
the smallest width, so <900 needs nothing more). One element moves; every field, the save
handlers, the advanced disclosure (advancedOpen_) and the re-localization registry stay valid.
Visibility flipped with accountThree (MainWindow.xaml.cpp:612-614).

## Fold-gated host placement rule (from the Support precedent)

The host lives in the pane that survives the gate, as a named inline StackPanel whose Visibility
ApplyBreakpoint flips in the same applied-state pass as the gate (MainWindow.xaml.cpp:657-660);
the move must happen inside that pass, not on SizeChanged, or the early-out (476-480) can strand
rows. Host sits after the surviving pane's content, inside its ScrollViewer - never displacing
the table/list that owns the floor.

## Verification checklist

- Drag 1600 -> 800 on Earnings and Account: at 1500 pane C/D content appears in the fold host;
  at 900 every pane-A action is reachable; widening restores the original placement with no
  duplicate or orphaned rows.
- Each moved control still works: provider transport sheet opens and applies; both public
  toggles write and follow a server-side flip (echo guards intact); emoji edit opens
  EmojiTagSheet; extender save persists; share/import sheets open; Upgrade/Redeem/Manage open.
- ShowPointsBoard flip while folded: PointsNetworkHost/DataRankingHost visibility still tracks
  the board switch (WalletPage.cpp:2886-2889).
- Language change while folded: extender labels re-text via the registry
  (AccountPage.cpp:619-639); fold-host headers use existing keys or the Adv() fallback.
- Sign out and back in while folded (with this wave's wallet_->ResetForSignOut wired): no stale
  leaderboard, rank, points or history; ResetForSignOut writes named elements, which is
  parent-agnostic.
- UIA: accessible names appear exactly once at every width; screen reader finds the fold host
  inside its pane's landmark.
