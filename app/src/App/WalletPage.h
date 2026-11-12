// The Earnings destination (rail item "wallet"): points first, the subnet
// layer once a Bittensor coldkey is connected, and the leaderboard beside it.
//
//   pane A  the net points figure and its breakdown; the protocol note; the
//           Bittensor wallet (connected through the ur.io wallet bridge with
//           purpose "connect", or a pasted address that is still signed) with
//           its overflow, "Connect Solana wallet" (SolanaWalletSheets); the
//           Solana payout wallet that USDC payouts go to until the Bittensor
//           migration completes, and the USDC still waiting for one; the
//           unclaimed SN25a tile and the claim dialog; beside it settlement
//           v2's section, the coldkey's v2 accounts, and their claim dialog;
//           the Top 200 head-spot tile (GET /sn/top/me, the Protocol board's
//           own answer), and its Sign fleet binding sheet (Top200SignSheet)
//   pane B  the per-epoch history (points; the alpha column only with a
//           wallet) and the leaderboard, one at a time: the Protocol board
//           (ProtocolBoard.h) first and the default, then Points, then Data
//   pane C  the board's own block (the Protocol card, the points network, or
//           the data board's own rank), reliability
//
// Points are URnetwork's own system and always the headline. Alpha accrues
// from the first epoch after the wallet was attached, never retroactively.
// Claims are the SDK's on this device, straight to the settlement vault (v1)
// or vault2 (v2); no URnetwork API is in that path, and the app only ever sees
// the gas key's address and its ss58 mirror.
//
// One unit of the per-page split of MainWindow. It owns the destination's
// state and the fetches; MainWindow's XAML event handlers forward here.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Xaml.Media.Animation.h>
#include <winrt/Microsoft.UI.Xaml.Shapes.h>

#include "BittensorKeySheets.h"
#include "BittensorWalletDialogs.h"
#include "EarningsSheets.h"
#include "ProtocolBoard.h"
#include "ProviderIdleReason.h"
#include "ProviderStatusPresentation.h"
#include "SnConsentPresentation.h"
#include "SnPayoutPresentation.h"
#include "SolanaWalletPresentation.h"
#include "Top200SignSheet.h"

namespace urnw {
struct LiveStats;
}
#include "SdkHost.h"
#include "TransferChart.h"
#include "TransportBar.h"
#include "UrComponents.h"

namespace winrt::URnetwork::implementation {
struct MainWindow;
}

namespace urnw {

class EmojiTagSheet;
class TransportSettingsSheet;
class ConnectSolanaWalletSheet;

class WalletPage {
 public:
  explicit WalletPage(winrt::URnetwork::implementation::MainWindow& window);
  ~WalletPage();

  void Initialize();  // the address-validation debounce timer
  void ApplyStrings();

  // The provide-mode row (the Connect page's indicator + label with the
  // current mode), the idle reason under it (P008) and the providing gate:
  // with providing off the reliability chart hides and the group says so, the
  // same gate and message as the stats widget. MainWindow relays each
  // live-stats update here.
  void ApplyProvideState(urnw::LiveStats const& stats);

  // The read-only extender row under the provide mode row (connect/EXTENDER.md
  // N7), and the extender statistics section's other half, whether the role is
  // running (O8). MainWindow hands every pushed status here and to the Connect
  // page.
  void ApplyExtenderProvideState(urnw::ExtenderProvideStatusView const& view);
  // The provider statistics and the extender series (O5, O8), from SdkHost's
  // feed on every throughput tick.
  void ApplyProviderThroughput(urnw::ProviderThroughputSnapshot const& snapshot);
  // Re-seed both from SdkHost's caches when the page is built and when the
  // destination shows: cache reads, no rpc.
  void ResyncProviderStats();
  // The statistics charts' clock runs only while the window presents, as the
  // Connect page's does, and so does the provider status poll (P008).
  void SetPresentationActive(bool active);
  // Whether the Earnings destination is the one showing: the other half of
  // the provider status poll's gate, from MainWindow's navigation.
  void SetSelected(bool selected);

  // Every Earnings fetch: points, reliability, the epoch history, the coldkey,
  // the head-spot status - and, once the coldkey is known, the claims and the
  // gas key from the chain. Each settles its own panel independently, so one
  // failing source does not blank the others. It also asks the leaderboard's
  // load gate (LoadLeaderboardIfShowing), for a session that starts with the
  // Leaderboard tab already selected.
  void LoadWallet();
  void LoadLeaderboard();
  // The board's one load gate: once per session, signed in, tab selected.
  void LoadLeaderboardIfShowing();

  // Drop everything the leaderboard, the points figure and the history were
  // saying about the account that just signed out, and re-arm the board's
  // first-show fetch. leaderboardLoaded_ is the dangerous one: it gates the
  // board's only load (LoadLeaderboardIfShowing), so a guard left set shows
  // the next sign-in the PREVIOUS account's table and own rank.
  void ResetForSignOut();

  // --preview-ui: settle every panel on its empty state (or, with
  // URNETWORK_PREVIEW_SAMPLE=1, on obviously synthetic rows) instead of
  // "Loading..." forever.
  void ShowPreviewWalletState();
  void ShowPreviewLeaderboardState();
  void ShowPreviewSnackbar();

  // XAML handlers, forwarded from MainWindow
  void OnConnectWallet(winrt::Windows::Foundation::IInspectable const&,
                       winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnChangeWallet(winrt::Windows::Foundation::IInspectable const&,
                      winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnEnterAddressManually(winrt::Windows::Foundation::IInspectable const&,
                              winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnWalletAddressChanged(
      winrt::Windows::Foundation::IInspectable const&,
      winrt::Microsoft::UI::Xaml::Controls::TextChangedEventArgs const&);
  void OnConnectWalletAddress(winrt::Windows::Foundation::IInspectable const&,
                              winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  // the three-dot overflows: beside the Bittensor action in both of its states
  // ("Connect Solana wallet"), and on the Solana payout wallet's card ("Remove")
  void OnWalletMore(winrt::Windows::Foundation::IInspectable const&,
                    winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnSolanaWalletMore(winrt::Windows::Foundation::IInspectable const&,
                          winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  winrt::fire_and_forget OnClaimAlpha(winrt::Windows::Foundation::IInspectable const&,
                                      winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  // settlement v2's Claim: the v2 claim dialog, on the claim in flight while
  // there is one
  winrt::fire_and_forget OnClaimV2(winrt::Windows::Foundation::IInspectable const&,
                                   winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  // the payout consent's button beside it: Sign with wallet, or Change wallet
  void OnSignConsent(winrt::Windows::Foundation::IInspectable const&,
                     winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnClaimTop200(winrt::Windows::Foundation::IInspectable const&,
                     winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  // the Top 200 card's Sign fleet binding (eligible and bound): the sign sheet
  void OnSignTop200(winrt::Windows::Foundation::IInspectable const&,
                    winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  // Opens the sign sheet with `prefill` in its box (Top200SignSheet::Create),
  // or pre-fills the one already open. A urnetwork://top200/sign link lands here
  // through MainWindow::HandleTop200SignLink whatever the Top 200 card shows; it
  // only fills the box, never signs. Dropped while another sheet is open.
  winrt::fire_and_forget ShowTop200Sign(std::string prefill, bool linkInvalid);
  void OpenProtocolSite();  // the "Learn more" link beside the wallet note
  void OnEarningsTableChanged(
      winrt::Microsoft::UI::Xaml::Controls::SelectorBar const&,
      winrt::Microsoft::UI::Xaml::Controls::SelectorBarSelectionChangedEventArgs const&);
  void OnLeaderboardPublicToggled(winrt::Windows::Foundation::IInspectable const&,
                                  winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  // The ledger header's "Earnings actions" overflow, shown while pane A is
  // folded (the fold-doors design note's COMMAND-DOOR DUPLICATION): one menu
  // whose items call the SAME member handlers the rail's buttons call - claim,
  // settlement v2's claim, wallet connect/change, the Solana menu and upgrade -
  // so the door survives the fold without any element being built twice.
  void ShowEarningsActionsMenu(winrt::Microsoft::UI::Xaml::FrameworkElement const& anchor);

  void RefreshAfterWalletChange();

  // ---- the one-tap Bittensor key (apple/BTEASY.md, flows 1 to 4) -------------
  // The sdk's BittensorLocalKeyFlow decides everything; these run its calls and
  // draw what it says (BittensorKeyViews.h, BittensorKeySheets.h).
  //
  // Back up first (Sign out, Delete account) and the reminder's notification:
  // the Key screen, once Earnings has the flow for the signed-in network.
  void OpenKeyScreenWhenReady();
  // The window lost focus or stopped presenting: the words hide.
  void OnWindowDeactivated();
  // The window is hiding to the tray: the Key screen closes, whatever its step.
  void CloseKeyScreenForHide();

  // ---- the boards ------------------------------------------------------------
  // The leaderboard is three boards behind one switch, in the owner's order
  // (owner decision 2026-10-29, LEADERTOP Q14): Protocol (ProtocolBoard.h, the
  // SN25-aligned top 400) first and the default, Points (the all-time points
  // board, android/POINTSLEADERBOARD.md) and Data (the last-4-payments board
  // above). The Points board is the SDK's PointsLeaderboardViewController
  // rendered as it is: rows, ranks, sort and pages all come from the
  // controller; nothing here sorts, ranks or pages. ShowBoard flips the
  // switch; the points controller is opened the first time the Points board
  // shows and closed with the page.
  enum class Board { Protocol, Points, Data };
  void ShowBoard(Board board);
  winrt::fire_and_forget OnEditEmoji();

  // ---- the Protocol board and the Top 200 tile (LEADERTOP) -------------------
  ProtocolBoard& protocol() { return *protocol_; }
  // The tile, from the Protocol board's card answer (GET /sn/top/me): hidden,
  // a network projected inside the top 200, or a claimed one.
  void ApplyTop200(protocolboard::Tile const& tile);
  // "See the Protocol leaderboard": the ledger pane's leaderboard tab, on the
  // Protocol board.
  void ShowProtocolBoard();
  // The network's "Show my network name" switch (points_leaderboard_public,
  // Q12), one value behind two switches: the Points board's and the Protocol
  // card's. Either asks for `requested` here; the answer, or a refusal, is
  // written back to both.
  void RequestNamePublic(bool requested);

  // ---- the points board's row (public: built by a free helper in the .cpp)
  // One ranked network as the list renders it: a value copy of the SDK row
  // (the controller re-emits fresh rows on every event) with the texts the
  // SDK preformats. `displayName` is empty when the row is anonymous; the
  // list then shows the localized "Anonymous". `emojiTag` shows either way.
  struct PointsRow {
    int64_t position = 0;  // the row's 1-based place in the whole ranking (no ties)
    std::string networkId;
    std::string displayName;
    std::string emojiTag;
    bool anonymous = false;
    std::string totalPointsText;
    std::string blocksText;
    std::string streakText;
    std::string longestStreakText;
    std::string rankPointsText;
    std::string rankBlocksText;
    std::string rankStreakText;
    bool operator==(PointsRow const&) const = default;
  };

 private:
  // THE ONE GATE for every server call this destination makes. --preview-ui
  // deliberately has no session, and a guarded LOAD path is not enough: every
  // action here (connect, claim, the leaderboard switch) has to pass
  // through this too, or a preview build puts authenticated-looking requests
  // on the wire with no token.
  bool CanCallApi() const;
  // Claims read and write the chain through the SDK on this device, which in
  // this app lives behind the service's DeviceRemote: no device, no claims.
  bool CanClaim() const;
  void RefuseNoSession();

  enum class Fetch { Loading, Ready, Failed };

  // the coldkey attached to this network's provider, as the page holds it
  struct SnWalletInfo {
    std::string coldkeySs58;
    std::string clientId;
    int64_t setAtMillis = 0;
  };
  // one finalized epoch of the history (the SDK's AccountEpoch)
  struct EpochRow {
    int64_t epoch = 0;
    int64_t startMillis = 0;
    int64_t endMillis = 0;
    double points = 0;
    int64_t shareBps = 0;
  };
  // the unauthenticated address check (POST /sn/wallet/validate)
  struct AddressVerdict {
    bool validSyntax = false;
    bool existsOnChain = true;
    bool banned = false;
    std::string message;
  };

  // ---- fetches (the SDK adapters; every callback marshals to the UI thread)
  void LoadPoints();
  void LoadReliability();
  void LoadEpochs();
  void LoadSnWallet();
  void LoadClaims();
  void LoadGas();
  void LoadV2Accounts();

  // ---- points
  void ApplyPoints(std::vector<urnet::AccountPoint> const& points, Fetch state);
  void RebuildPointsRows();

  // ---- the coldkey
  void ApplySnWallet(std::optional<SnWalletInfo> wallet, Fetch state);
  // The bridge answered with a signed challenge (either path). Validates the
  // address before anything is sent to the account, then attaches it.
  void ApplyWalletSigned(uint32_t generation, bool ok, std::string const& address,
                         std::string const& signature, std::string const& message,
                         std::string const& error, std::string const& expectedAddress);
  void SubmitWalletConnect(uint32_t generation, std::string const& address,
                           std::string const& signature, std::string const& message);
  // `warning` is the SDK's non-blocking warning code (a store key such as
  // wallet_looks_new_warning) on success; `error` the SnError on failure.
  void ApplyWalletConnectResult(uint32_t generation, bool ok,
                                std::optional<urnet::SnError> const& error,
                                std::string const& warning);
  void SetConnectingWallet(bool connecting);
  void StartWalletConnect(std::string const& pinnedAddress);
  // after the chooser: the proof with `walletId` (a bittensor::kChooserWallets id)
  void ConnectWithWallet(std::string const& walletId, std::string const& pinnedAddress);

  // ---- the manual address (still signed)
  void ValidateWalletAddress();
  // Runs the unauthenticated validate call for `address`; `done` gets the
  // verdict on the UI thread, or nullopt when the call itself failed.
  void ValidateAddressRemote(std::string const& address,
                             std::function<void(std::optional<AddressVerdict>)> done);
  void ApplyManualVerdict(uint32_t generation, std::optional<AddressVerdict> verdict);
  void ShowManualPanel(bool show);

  // ---- the Solana payout wallet (legacy USDC payouts until the migration)
  // The account's wallets, the payout wallet id and the payments: three
  // independent reads, committed together by solana::LegacyLoad so the card and
  // the waiting line never render from part of them. `reset` (after a write)
  // hides both until the new answers commit; a plain reload keeps what is on
  // screen meanwhile; another network is cleared at once.
  void LoadLegacyWallets(bool reset = false);
  void ApplyLegacyAnswer(uint32_t generation, solana::LegacyRead which, bool ok,
                         solana::LegacyAnswer answer);
  void RebuildSolanaPanel();
  void ShowWalletMenu(winrt::Microsoft::UI::Xaml::FrameworkElement const& anchor);
  winrt::fire_and_forget OpenConnectSolanaSheet();
  // The sheet linked `walletId`: made the payout wallet here when a fresh read of
  // the payout wallet says it is not yet (ApplyFreshPayoutWallet).
  void OnSolanaConnected(std::string const& walletId);
  void ApplyFreshPayoutWallet(uint32_t generation, std::string const& walletId,
                              std::string const& payoutWalletId);
  void SwitchPayoutWallet(std::string const& walletId);
  // the payout read and the switch share one watchdog (legacyFlow_)
  uint32_t BeginPayoutFlow();
  void ApplyPayoutSwitchResult(uint32_t generation, bool ok, std::string const& error);
  void ShowSolanaCardMenu(winrt::Microsoft::UI::Xaml::FrameworkElement const& anchor);
  winrt::fire_and_forget ConfirmRemoveSolanaWallet();
  void RemoveSolanaWallet(std::string const& walletId);
  void ApplyRemoveResult(uint32_t generation, bool ok, std::string const& error,
                         solana::PayoutRemoval const& removal);
  // the reload after a removal committed: name the wallet the server promoted
  void NotifyPromotedPayoutWallet();
  void SetLegacyBusy(bool busy);

  // ---- history, claims, gas
  void ApplyEpochs(std::vector<EpochRow> const& epochs, Fetch state);
  void RebuildHistory();
  // `error` is the SDK's SnError (or one built from a transport error) when
  // the fetch failed; its stable code picks the store's sentence. `schedule` is
  // the current epoch's, read with the claims (absent when the SDK could not
  // read the coordinator's policy).
  void ApplyClaims(std::vector<EpochClaim> const& claims, int64_t totalClaimableRao,
                   Fetch state, std::optional<urnet::SnError> const& error,
                   std::optional<snpayout::EpochSchedule> const& schedule = std::nullopt);
  // How and when SN payouts happen, under the points figure
  // (snpayout::PayoutLineFor), from the coldkey, the claims and the schedule.
  void RebuildPayoutLine();
  // The default chain settings ship without the vault, coordinator and
  // operator id, so the first vault read waits for one GET /sn/epoch through
  // the device, which stores them. `then` runs on the UI thread either way.
  void EnsureChainSettings(std::function<void()> then);
  void ApplyGas(std::optional<GasKeyInfo> gas);
  void ApplyLedgerMeta();
  // The SDK claim, wrapped for the dialog (Device.snClaim).
  Claimer MakeClaimer();
  std::string ExplorerTxUrl() const;
  const EpochClaim* ClaimForEpoch(int64_t epoch) const;

  // ---- settlement v2
  // The read's answer: its state and, when Ready, the accounts and the pools
  // it could not read (a failed read and a missing device session leave none).
  void ApplyV2Accounts(snpayout::LedgerFetch fetch,
                       std::vector<snpayout::LedgerAccount> const& accounts,
                       std::vector<snpayout::UnavailablePool> const& unavailable);
  // The section beside the v1 tile (snpayout::LedgerSectionFor), from the
  // coldkey, the chain settings, the read and the device.
  void RebuildV2Section();
  // The chain settings name a vault2 (Vault2Address): v2 claims are configured.
  bool Vault2Configured() const;
  // The claim relay's transfer floor in rao of α at its last price, under
  // which the chain refuses a claim; 0 while the relay has not said.
  int64_t TransferFloorRao() const;
  // Capture the flag's won-slot lines (GET /sn/ctf/client, the sdk's
  // SnCtfClient): a line for every win not claimed yet, read with the Earnings
  // reads, and the answer to read number `read`: the lines show the newest
  // read's good answer (snpayout::CtfWinsLines), a network answer beating a
  // client answer (`answer`), and none (std::nullopt) is a read that failed,
  // which keeps them. Drawn with the settlement v2 section's capture the flag
  // account.
  void LoadCtfClient();
  void ApplyCtfClient(uint32_t read, std::optional<std::vector<snpayout::CtfWin>> wins,
                      snpayout::CtfAnswer answer);
  // After a v2 claim's Done, which read the wins: when the claim, started in
  // `session`, paid an account they are read again 30 s, 60 s and 120 s after
  // Done (snpayout::CtfWinsReadsAfterClaim, every app's schedule) through
  // LoadCtfClient, on ctfWinsTimer_, until the page goes or the user signs out.
  void ReadCtfWinsAfterClaim(bool paid, uint32_t session);
  void ArmCtfWinsTimer(snpayout::CtfWinsReadsAfterClaim::Wait wait);
  void OnCtfWinsTimer(uint32_t token);
  // The v2 dialog's Claim (Device.snLedgerClaim): StartV2Claim.
  LedgerClaimer MakeV2Claimer();
  // Starts the SDK's v2 claim on a dialog's `progress` as the session's claim
  // in flight (v2Claim_): false, starting nothing, while one is in flight or
  // when the press would take nothing. Its events move the progress through
  // OnV2ClaimEvent whether or not a dialog shows it; after its Done the page
  // reads the accounts, the gas balance, the relay's status and the wins
  // again, and the wins on the schedule after a claim that paid.
  bool StartV2Claim(std::shared_ptr<snpayout::LedgerClaimProgress> progress);
  // An event of the claim whose progress is `claim`, and its Done (`ticket`):
  // they move that progress while the page or a dialog holds it, an open
  // dialog draws it again, and Done ends the claim in flight
  // (snpayout::LedgerClaimInFlight).
  void OnV2ClaimEvent(std::weak_ptr<snpayout::LedgerClaimProgress> const& claim,
                      std::function<void(snpayout::LedgerClaimProgress&)> const& step);
  void OnV2ClaimDone(std::weak_ptr<snpayout::LedgerClaimProgress> const& claim, uint64_t ticket);

  // ---- the payout consent (SnConsentPresentation.h)
  // The network's consent (Api.snNetworkConsentStatus), and the answer to read
  // number `read`, dropped unless it is the newest read's.
  void LoadConsentStatus();
  void ApplyConsentStatus(uint32_t read, snconsent::Fetch fetch,
                          std::optional<snconsent::Status> const& status);
  // The panel beside settlement v2 (snconsent::PanelFor).
  void RebuildConsentPanel();
  // The server's challenge for `coldkeySs58`, then the signature of
  // `walletId` over it, then the submission.
  void StartConsent(std::string const& coldkeySs58, std::string const& walletId);
  void ApplyConsentChallenge(uint32_t generation, std::string const& walletId,
                             std::optional<urnet::SnNetworkConsentChallengeResult> const& result,
                             std::optional<std::string> const& err);
  // `code` is the refusal's (the session's, or the SDK error that starts with
  // one); `error` its localized text.
  void ApplyConsentSigned(uint32_t generation, snconsent::Challenge const& challenge, bool ok,
                          std::string const& address, std::string const& signature,
                          std::string const& message, std::string const& code,
                          std::string const& error);
  void ApplyConsentSubmitted(uint32_t generation,
                             std::optional<urnet::SnSetWalletResult> const& result,
                             std::optional<std::string> const& err);
  // A refusal: the store's sentence for the three the flow names
  // (snconsent::RefusalKey), else `fallback`, the page's own words for it.
  void FailConsent(std::string const& codeOrError, winrt::hstring const& fallback);
  void EndConsent();

  // ---- the one-tap Bittensor key
  // The flow for the signed-in network: made on the first load of a network
  // and read again (the sdk's Reload) on every later one, closed at sign-out.
  void OpenKeyFlow();
  void CloseKeyFlow();
  // The flow's listener, on the UI thread: the card, the banner, the card's
  // place, the wallet card, the payout line and the Key screen read it again.
  void ReadKeyFlow();
  // The consent read, the epoch, the guest flag and the Top 200 registration,
  // into the flow (SetConsent, SetCurrentEpoch, SetGuest, SetTop200Registered).
  void FeedKeyConsent();
  // The key's part of the payout card (btkey::CardLayoutFor), under the
  // consent panel's own lines or in their place, and its disclaimer.
  void RebuildKeyCard(snconsent::PanelView const& view);
  // The backup banner, while the key's phrase isn't known to be saved.
  void ApplyKeyBanner();
  // The payout card leads Earnings while the sdk shows the key path; it keeps
  // its old place after settlement v2 otherwise.
  void PlaceConsentPanel();
  // The Bittensor wallet card with no wallet read: the key on this device in
  // place of Connect Bittensor wallet.
  void ApplyKeyWalletCard();
  // The sdk's card says the consent pays the key on this device.
  bool KeyPaysThisDevice() const;
  // The card's actions. A replacement of the payout wallet in force asks first.
  enum class KeyAction { SetUp, SaveFirst, FinishSetup };
  void OnKeyAction(KeyAction action);
  winrt::fire_and_forget ConfirmKeyReplacement(KeyAction action);
  void RunKeyAction(KeyAction action, bool replaceConfirmed);
  void OpenKeyScreen();
  // The wallet consent sheet: Change wallet offers a key on this device while
  // the sdk's card offers one; Use a wallet I already have never does.
  void StartWalletConsent(bool offerKey);
  // the key offer the consent sheet starts with, or none (BittensorWalletDialogs.h)
  pages::WalletKeyOffer KeyOfferForConsent(bool offerKey);
  winrt::fire_and_forget ShowKeySheet();
  // The one reminder (the sdk's Reminder), on each Earnings read.
  void ApplyKeyReminder();
  void PostKeyReminder();
  // ---- claims through the relay (sn_claim_relay.go)
  void LoadClaimStatus();
  void ApplyClaimStatus(uint32_t read, std::optional<urnet::SnClaimStatusResult> status);
  // A network with no wallet on this device: the settlement v2 tile shows what
  // the relay claims for its consent.
  bool RelayTileShows() const;
  // The tile's relay rows and its disclaimer, after RebuildV2Section's balances.
  void ApplyRelayToV2Section(snpayout::LedgerSectionView const& view);
  // The claim tiles' funds disclaimer: the first claim tile shown carries the
  // sdk's ClaimFundsDisclaimer (the v1 tile while a wallet is read, else v2's).
  void ApplyClaimDisclaimers();
  winrt::fire_and_forget OpenClaimRelaySheet();
  // The sdk's disclaimer for a place (FundsDisclaimerAt), "" with no flow.
  std::string KeyDisclaimerAt(std::string_view place) const;

  // ---- reliability
  void ApplyReliability(std::optional<urnet::ReliabilityWindow> window, Fetch state);

  // ---- the statistics groups (connect/EXTENDER.md O5, O8)
  void BuildCharts();
  void OnChartTick();
  // the read-only extender row, from extenderProvideView_ (N7)
  void ApplyExtenderProvideRow();
  // The line under the provide mode row (P008): why providing is enabled but
  // idle (provideridle::ProviderIdleReasonFor), from the last live stats and
  // the provider bytes in the window, merged with the server's reason once a
  // status for this device has loaded (providerstatus::LineFor); collapsed
  // when there is nothing to say.
  void ApplyProvideReason();
  // ---- the provider status (P008): the SDK's ProviderStatusViewController,
  // which reads GET /network/provider-status about once a minute. Opened on
  // the device once the destination shows with providing enabled (never while
  // it is off), polling only while the destination shows and the window
  // presents, and closed with the typed close when providing turns off, the
  // device it was opened on goes, or the page goes.
  void ReconcileProviderStatus();
  void OpenProviderStatus(uint64_t device);
  // Releases the controller and forgets its readings; touches no XAML, so the
  // destructor can call it.
  void CloseProviderStatus(bool deviceAlive);
  // Mirrors the controller into the page: loaded, the last poll's error and
  // this device's status (one value copy of the SDK's struct).
  void ReadProviderStatus();
  // With no session the provider is the service's provider-only device, which
  // no controller can be opened on: SdkHost reads its status on the api while
  // this page wants it (SetProviderOnlyStatusWanted), and these take its
  // readings into the same three fields the controller fills.
  void ApplyProviderOnlyStatus(urnw::ProviderOnlyStatus const& status);
  void TakeProviderOnlyReadings(urnw::ProviderOnlyStatus const& status);
  // The readings through providerstatus::ViewFor; neither a controller nor the
  // provider-only source reads as a failed poll.
  providerstatus::View ProviderStatusView() const;
  // The Demand row and Why? under the provider plots' gate, and the line.
  void ApplyProviderStatus();
  void RebuildProviderWhy();
  void ToggleProviderWhy();
  // Both groups' visibility and the provider header's meta label, from
  // ExtenderStatsSectionsFor. Only a changed reading is painted unless `force`.
  void ApplyStatsSections(bool force);
  winrt::fire_and_forget ShowProviderTransportSettingsSheet();

  // ---- leaderboard
  void ApplyLeaderboard(urnet::LeaderboardEarnersList const& earners, Fetch state);
  void ApplyRanking(urnet::NetworkRanking const& ranking, bool ok);
  // the ranking's points_leaderboard_public, read at `askedAt` on the
  // own-flags clock: it loses to a local edit made after it was asked for
  void ApplyRankingNamePublic(bool isPublic, uint64_t askedAt);
  void ApplyRankingPublicResult(uint32_t generation, bool ok, bool requested,
                                std::string const& serverError);
  void SetRankingToggle(bool isPublic);

  void OpenUrl(std::string const& url);

  // A request with a watchdog: BeginFlow arms the timer and returns the
  // generation the request owns; SettleFlow is false when the answer belongs
  // to a request already given up on (or superseded).
  struct Flow {
    winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer timer{nullptr};
    uint32_t generation = 0;
  };
  uint32_t BeginFlow(Flow& flow, int timeoutMs, std::function<void()> onTimeout);
  bool SettleFlow(Flow& flow, uint32_t generation);

  // the snackbar of whichever pane the message belongs to
  void Notify(winrt::hstring const& message,
              winrt::Microsoft::UI::Xaml::Controls::InfoBarSeverity severity);

  winrt::URnetwork::implementation::MainWindow& w_;
  urnw::kit::Snackbar snackbar_;
  urnw::kit::Snackbar leaderboardSnackbar_;

  // ---- state
  std::string ownNetworkId_;
  bool providingEnabled_ = true;  // the reliability chart follows the provide mode
  // ---- the statistics groups (O5, O8)
  std::unique_ptr<urnw::TransferChart> extenderChart_;
  std::unique_ptr<urnw::TransferChart> providerLocalChart_;
  std::unique_ptr<urnw::TransportBar> providerTransportBar_;
  std::unique_ptr<urnw::TransferChart> providerBlockedChart_;
  winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer chartTimer_{nullptr};
  std::shared_ptr<urnw::TransportSettingsSheet> providerTransportSheet_;
  // the last pushed status: the read-only row draws it, and its `enabled` is
  // the extender section's running role
  urnw::ExtenderProvideStatusView extenderProvideView_;
  bool extenderRunning_ = false;
  bool hasProviderStats_ = false;
  // the bar holds a distribution with shares, so it has nothing to wait for
  bool providerDistributionSeen_ = false;
  // the reading last painted; empty before the first
  std::optional<urnw::ExtenderStatsSections> statsSections_;
  // the idle reason's inputs (P008): the control mode and the live provide
  // state from the last live stats, and the provider bytes in the window from
  // the last provider distribution
  provideridle::ProvideControlMode provideControlMode_ = provideridle::ProvideControlMode::Unknown;
  int64_t liveProvideMode_ = 0;
  bool providePaused_ = false;
  int64_t providerWindowBytes_ = 0;
  // ---- the provider status (P008)
  bool selected_ = false;            // the Earnings destination shows
  bool presentationActive_ = false;  // the window presents
  // a live stats reading has set providingEnabled_: until then the gate's
  // default says nothing, and the controller must not open on it
  bool provideStateKnown_ = false;
  std::optional<urnet::ProviderStatusViewController> providerStatusVc_;
  std::optional<urnet::Sub> providerStatusSub_;
  // the device handle the controller was opened (or tried) on; 0 for none
  uint64_t providerStatusVcDevice_ = 0;
  bool providerStatusStarted_ = false;
  bool providerStatusLoaded_ = false;
  std::string providerStatusError_;
  std::optional<urnet::ProviderStatus> providerStatus_;
  // the three readings above are the provider-only device's (no session, no
  // controller), from SdkHost
  bool providerOnlySource_ = false;
  bool providerWhyOpen_ = false;  // Why? starts collapsed
  // the Demand chart's 60 bars, oldest first (BuildCharts)
  std::vector<winrt::Microsoft::UI::Xaml::Shapes::Rectangle> demandBars_;
  PointsBreakdown accountPoints_;
  std::optional<urnet::ReliabilityWindow> reliability_;

  std::optional<SnWalletInfo> snWallet_;
  Fetch walletState_ = Fetch::Loading;
  bool connectingWallet_ = false;
  bool manualPanelOpen_ = false;
  bool manualAddressOk_ = false;
  std::string manualAddress_;
  uint32_t walletValidateGeneration_ = 0;
  winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer walletValidateTimer_{nullptr};

  std::vector<EpochRow> epochs_;
  Fetch epochsState_ = Fetch::Loading;
  std::vector<EpochClaim> claims_;
  int64_t totalClaimableRao_ = 0;
  Fetch claimsState_ = Fetch::Loading;
  std::optional<snpayout::EpochSchedule> schedule_;
  std::optional<GasKeyInfo> gas_;
  bool chainSynced_ = false;
  // settlement v2: the read, the accounts it answered with, and the pools it
  // could not read (capture the flag's or the provider pool's), each shown as
  // unavailable beside the others
  snpayout::LedgerFetch v2Fetch_ = snpayout::LedgerFetch::Loading;
  std::vector<snpayout::LedgerAccount> v2Accounts_;
  std::vector<snpayout::UnavailablePool> v2Unavailable_;
  // the v2 claim in flight, the session's rather than its dialog's: its
  // progress, which a dialog shows while it is up, and the ticket its events
  // carry; only its Done and a sign-out end it
  snpayout::LedgerClaimInFlight v2Claim_;
  // capture the flag: the wins not claimed yet, newest first, as the newest
  // read's good answer gave them (none from a sign-out until the next good
  // read), and the reads' numbers
  snpayout::CtfWinsLines ctfWins_;
  // the wins' reads after a v2 claim that paid an account, and the one-shot
  // timer of the wait for the next one (ReadCtfWinsAfterClaim, a new timer
  // each wait)
  snpayout::CtfWinsReadsAfterClaim ctfWinsAfterClaim_;
  winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer ctfWinsTimer_{nullptr};
  // the payout consent: the read (Idle until a session asks for it), the
  // consent it answered with (none before an answer and after a failed read),
  // and whether a signing is out (the challenge, the wallet, the submission)
  snconsent::Fetch consentFetch_ = snconsent::Fetch::Idle;
  std::optional<snconsent::Status> consent_;
  bool consentSigning_ = false;
  // the newest read's number (LoadConsentStatus); a sign-out moves it on too
  uint32_t consentReadGeneration_ = 0;

  // ---- the one-tap Bittensor key
  // The sdk's flow for keyNetworkId_ (BittensorKeyStore.h NewBittensorKeyFlow),
  // shared with the Key screen while it is up. Null with no network signed in.
  std::shared_ptr<urnet::BittensorLocalKeyFlow> keyFlow_;
  std::optional<urnet::Sub> keySub_;
  std::string keyNetworkId_;
  // the card the flow last said, and the link generation the page last
  // re-read the consent and earnings for
  std::optional<urnet::BittensorLocalKeyCard> keyCard_;
  int64_t keyLinkGeneration_ = 0;
  // the consent panel's last view, for the key's part of the card
  std::optional<snconsent::PanelView> consentView_;
  std::shared_ptr<urnw::BittensorKeySheet> keySheet_;
  // Back up first or the reminder asked for the Key screen before the flow was there
  bool keyScreenPending_ = false;
  // Use a wallet I already have: the consent sheet opens without the key offer
  bool walletOnlyConsent_ = false;
  // the network has a claimed Top 200 fleet (the tile's answer)
  bool keyTop200Registered_ = false;
  // the Bittensor wallet card shows the key on this device
  bool keyWalletCardShown_ = false;
  // the card's BackupReminder as last read: its end takes a reminder back
  bool keyBackupReminder_ = false;
  // the one reminder's moment, waited for while the app runs
  winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer keyReminderTimer_{nullptr};
  // ---- claims through the relay: the newest read's number and answer
  uint32_t claimStatusGeneration_ = 0;
  std::optional<urnet::SnClaimStatusResult> claimStatus_;
  std::shared_ptr<urnw::ClaimRelaySheet> relaySheet_;

  int64_t leaderboardRank_ = 0;
  int64_t leaderboardCount_ = 0;
  bool rankingPublic_ = false;
  bool applyingRankingToggle_ = false;
  bool settingRankingPublic_ = false;

  Flow connectFlow_;
  Flow rankingFlow_;
  Flow consentFlow_;  // the consent signing's three steps, one watchdog each

  std::shared_ptr<urnw::ClaimAlphaSheet> claimSheet_;
  std::shared_ptr<urnw::ClaimLedgerSheet> v2ClaimSheet_;

  // the Solana payout wallet: the view last committed from the three reads, and
  // the load whose answers are still coming in
  solana::LegacyCommitted legacy_;
  solana::LegacyLoad legacyLoad_;
  uint32_t legacyGeneration_ = 0;
  // a payout switch or a removal is out: every door to another Solana write is
  // shut (SetLegacyBusy)
  bool legacyBusy_ = false;
  Flow legacyFlow_;  // the payout read and switch after a link
  Flow removeFlow_;  // the removal, on its own flow: a link must never drop its answer
  // a removal that landed, until the reload after it commits
  std::optional<solana::PayoutRemoval> payoutRemoval_;
  std::shared_ptr<urnw::ConnectSolanaWalletSheet> solanaSheet_;

  struct PointsStatTile {
    winrt::Microsoft::UI::Xaml::Controls::TextBlock value{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Border chip{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock rank{nullptr};
  };

  void InitializePointsBoard();  // wires the switch, the sort bar, the scroll, retry
  void ApplyPointsBoardStrings();
  void BuildPointsNetworkHost();  // pane C's block, built in code (rebuilt on strings)
  // Opens the controller on the current device (or shows why it cannot);
  // safe to call on every look: a controller on a device that is still the
  // device is kept.
  void EnsurePointsBoard();
  void ClosePointsBoard(bool deviceAlive);
  // Mirrors the controller into the page: rows (value-compared, so a no-op
  // event does not re-render the table), sort, loading, end, error, `me`.
  void ReadPointsBoard();
  void RenderPointsRows(size_t fromIndex = 0);
  std::string OwnPointsName();
  void RenderPointsHeader();
  void RenderPointsFooter();
  void OnPointsSortChanged(std::string const& sort);
  void OnPointsScroll();
  // ---- the position indicator and the tab reset (mmm/DESIGNSTYLE.md "Long
  // ranked lists"; the math in LeaderboardIndicator.h)
  void BuildPointsIndicator();   // the overlay over the points scroller: track, thumb, label, slider
  void RefreshPointsPosition();  // the first row in view, from the scroller
  void UpdatePointsIndicator();  // visibility, geometry, the slider's value
  void OnPointsThumbPressed(double y, winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& e);
  void OnPointsThumbMoved(double y);
  void OnPointsThumbReleased();
  void SeekPoints(int64_t rank);  // a rank in the window scrolls there; any other asks the controller
  void ShowPointsDragLabel(int64_t rank);
  void HidePointsDragLabel(bool fade);
  void ScrollPointsToFirstRow();
  void PrependPointsRows(size_t count);
  winrt::Microsoft::UI::Xaml::UIElement MakePointsRow(PointsRow const& row, std::string const& ownId);
  void ResetBoardList(Board board);  // a board tab activated (the active one included)
  void OnPointsRetry();
  void OnPointsPublicToggled();
  void SetPointsToggle(bool isPublic);
  // both name switches from pointsPublic_ and settingPointsPublic_
  void SyncNameSwitches();
  void ApplyPointsPublicResult(uint32_t generation, bool ok, bool requested,
                               std::string const& serverError);
  void SaveEmojiTag(std::string tag, std::function<void(std::string)> done);
  void SettlePointsBoardPreview();

  // false once the page is gone: the controller's listener and the sheet's
  // completions marshal through the window and must not reach a dead page
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
  std::optional<urnet::PointsLeaderboardViewController> pointsVc_;
  std::optional<urnet::Sub> pointsSub_;
  uint64_t pointsVcDevice_ = 0;  // the device handle the controller was opened on
  Board board_ = Board::Protocol;
  bool pointsBoardShowing_ = false;  // board_ is Points
  // the Protocol board: built with the page, after alive_
  std::unique_ptr<ProtocolBoard> protocol_;
  // The Leaderboard's once-per-session fetch guard, read only by
  // LoadLeaderboardIfShowing, which both ways the board starts showing ask
  // (the tab's selection, and LoadWallet for a session that starts with the
  // tab selected). Tab flips must not re-fetch - the board is slower-moving
  // than the ledger - so the load runs once and the ranking toggle's own
  // re-read keeps its explicit call. ResetForSignOut re-arms it: the guard is
  // per-ACCOUNT, not per-run - left set, the next sign-in on this app run would
  // keep the previous account's table and own rank with no fetch ever
  // replacing them.
  bool leaderboardLoaded_ = false;
  std::vector<PointsRow> pointsRows_;
  std::string pointsSort_ = urnet::PointsLeaderboardSortPoints;
  std::string pointsRenderedSort_;
  std::string pointsRenderedOwnId_;   // the own id the rows were drawn with
  size_t pointsRenderedCount_ = 0;    // rows drawn; the next page appends after them
  bool pointsLoading_ = false;
  bool pointsEnd_ = false;
  bool pointsHasLoaded_ = false;  // the first page landed (rows, an empty end, or an error)
  std::string pointsError_;
  int64_t pointsTotalRanked_ = 0;
  int64_t pointsFirstPosition_ = 1;   // the loaded window's first position
  bool pointsHasMoreBefore_ = false;  // rows exist above the window (after a seek)
  int64_t pointsFirstVisible_ = 1;    // the first row in view's position
  // the draggable position indicator: a slider over ranks 1..N at the
  // scroller's right edge, the rank and tier beside the thumb while dragging
  winrt::Microsoft::UI::Xaml::Controls::Canvas pointsIndicator_{nullptr};
  winrt::Microsoft::UI::Xaml::Shapes::Rectangle pointsTrack_{nullptr};
  winrt::Microsoft::UI::Xaml::Shapes::Rectangle pointsThumb_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Slider pointsSlider_{nullptr};  // the keyboard and UIA face of the thumb
  winrt::Microsoft::UI::Xaml::Controls::Border pointsIndicatorLabel_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock pointsIndicatorRank_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock pointsIndicatorTier_{nullptr};
  winrt::Microsoft::UI::Xaml::Media::Animation::Storyboard pointsLabelFade_{nullptr};
  bool pointsDragging_ = false;
  double pointsDragGrab_ = 0;  // where inside the thumb the pointer took it
  double pointsDragTop_ = 0;   // the thumb's top while dragging
  int64_t pointsDragRank_ = 0;
  bool applyingPointsSlider_ = false;  // ECHO GUARD on the slider's value
  bool pointsSeekPending_ = false;     // a seek's window is on its way: scroll to its first row when it lands
  std::optional<PointsRow> pointsMe_;
  bool pointsPublic_ = false;  // this network's opt-in, from `me`, updated locally on toggle
  std::string emojiTag_;       // this network's tag, from `me`, updated locally on save
  bool settingPointsPublic_ = false;
  bool applyingPointsToggle_ = false;  // ECHO GUARD on the opt-in switch
  bool savingEmojiTag_ = false;
  // after a local toggle or save, `me` from an older in-flight page could
  // briefly disagree with what the user just did; the local values win until
  // a response newer than the edit lands
  uint64_t ownFlagsClock_ = 0;
  uint64_t ownFlagsEditedAt_ = 0;
  uint64_t ownFlagsAppliedAt_ = 0;
  Flow pointsPublicFlow_;
  std::shared_ptr<urnw::EmojiTagSheet> emojiSheet_;
  // the Top 200 sign sheet while it is open
  std::shared_ptr<urnw::Top200SignSheet> top200SignSheet_;

  // pane C's block (BuildPointsNetworkHost)
  winrt::Microsoft::UI::Xaml::Controls::TextBlock pointsGroupMeta_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock pointsEmojiText_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock pointsNameText_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock pointsRankedText_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Button editEmojiButton_{nullptr};
  PointsStatTile pointsTiles_[3];
  winrt::Microsoft::UI::Xaml::Controls::TextBlock pointsLongestText_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::ToggleSwitch pointsPublicToggle_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock pointsPrivateHint_{nullptr};
};

}  // namespace urnw
