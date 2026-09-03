// The Earnings destination (rail item "wallet"): points first, the subnet
// layer once a Bittensor coldkey is connected, and the leaderboard beside it.
//
//   pane A  the net points figure and its breakdown; the protocol note; the
//           Bittensor wallet (connected through the ur.io wallet bridge with
//           purpose "connect", or a pasted address that is still signed); the
//           unclaimed SN25a tile and the claim dialog; the Top 200 head-spot
//           tile
//   pane B  the per-epoch history (points; the alpha column only with a
//           wallet) and the leaderboard, one at a time
//   pane C  own ranking, the Seeker multiplier (points only), reliability
//
// Points are URnetwork's own system and always the headline. Alpha accrues
// from the first epoch after the wallet was attached, never retroactively.
// Claims are the SDK's on this device, straight to the settlement vault; no
// URnetwork API is in that path, and the app only ever sees the gas key's
// address and its ss58 mirror.
//
// One unit of the per-page split of MainWindow. It owns the destination's
// state and the fetches; MainWindow's XAML event handlers forward here.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>

#include "EarningsSheets.h"
#include "SdkHost.h"
#include "UrComponents.h"

namespace winrt::URnetwork::implementation {
struct MainWindow;
}

namespace urnw {

class EmojiTagSheet;

class WalletPage {
 public:
  explicit WalletPage(winrt::URnetwork::implementation::MainWindow& window);
  ~WalletPage();

  void Initialize();  // the address-validation debounce timer
  void ApplyStrings();

  // Every Earnings fetch: points, the Seeker flag, reliability, the epoch
  // history, the coldkey, the head-spot status - and, once the coldkey is
  // known, the claims and the gas key from the chain. Each settles its own
  // panel independently, so one failing source does not blank the others.
  void LoadWallet();
  void LoadLeaderboard();

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
  winrt::fire_and_forget OnClaimAlpha(winrt::Windows::Foundation::IInspectable const&,
                                      winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnClaimTop200(winrt::Windows::Foundation::IInspectable const&,
                     winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnLearnUrXyz(winrt::Windows::Foundation::IInspectable const&,
                    winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  winrt::fire_and_forget OnVerifySeeker(winrt::Windows::Foundation::IInspectable const&,
                                        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnEarningsTableChanged(
      winrt::Microsoft::UI::Xaml::Controls::SelectorBar const&,
      winrt::Microsoft::UI::Xaml::Controls::SelectorBarSelectionChangedEventArgs const&);
  void OnLeaderboardPublicToggled(winrt::Windows::Foundation::IInspectable const&,
                                  winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);

  void RefreshAfterWalletChange();

  // ---- the points board (android/POINTSLEADERBOARD.md) ----------------------
  // The leaderboard is two boards behind one switch: Data (the last-4-payments
  // board above) and Points (the all-time points board). The Points board is
  // the SDK's PointsLeaderboardViewController rendered as it is: rows, ranks,
  // sort and pages all come from the controller; nothing here sorts, ranks or
  // pages. ShowPointsBoard flips the switch; the controller is opened the first
  // time the Points board shows and closed with the page.
  void ShowPointsBoard(bool points);
  winrt::fire_and_forget OnEditEmoji();

  // ---- the points board's row (public: built by a free helper in the .cpp)
  // One ranked network as the list renders it: a value copy of the SDK row
  // (the controller re-emits fresh rows on every event) with the texts the
  // SDK preformats. `displayName` is empty when the row is anonymous; the
  // list then shows the localized "Anonymous". `emojiTag` shows either way.
  struct PointsRow {
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
  // ACTION here (connect, claim, verify, the leaderboard switch) has to pass
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
  // the head-spot status (the SDK's SnHeadResult)
  struct HeadInfo {
    bool eligible = false;
    double score = 0;
    double floor = 0;
    int64_t rankEstimate = 0;
    int64_t cutoff = 200;
    bool bound = false;
    std::string hotkey;
    int64_t uid = 0;
    int64_t rank = 0;
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
  void LoadSeeker();
  void LoadReliability();
  void LoadEpochs();
  void LoadSnWallet();
  void LoadHead();
  void LoadClaims();
  void LoadGas();

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

  // ---- the manual address (still signed)
  void ValidateWalletAddress();
  // Runs the unauthenticated validate call for `address`; `done` gets the
  // verdict on the UI thread, or nullopt when the call itself failed.
  void ValidateAddressRemote(std::string const& address,
                             std::function<void(std::optional<AddressVerdict>)> done);
  void ApplyManualVerdict(uint32_t generation, std::optional<AddressVerdict> verdict);
  void ShowManualPanel(bool show);

  // ---- history, claims, gas, head
  void ApplyEpochs(std::vector<EpochRow> const& epochs, Fetch state);
  void RebuildHistory();
  // `error` is the SDK's SnError (or one built from a transport error) when
  // the fetch failed; its stable code picks the store's sentence.
  void ApplyClaims(std::vector<EpochClaim> const& claims, int64_t totalClaimableRao,
                   Fetch state, std::optional<urnet::SnError> const& error);
  // The default chain settings ship without the vault, coordinator and
  // operator id, so the first vault read waits for one GET /sn/epoch through
  // the device, which stores them. `then` runs on the UI thread either way.
  void EnsureChainSettings(std::function<void()> then);
  void ApplyGas(std::optional<GasKeyInfo> gas);
  void ApplyHead(std::optional<HeadInfo> head, Fetch state);
  void ApplyLedgerMeta();
  // The SDK claim, wrapped for the dialog (Device.snClaim).
  Claimer MakeClaimer();
  std::string ExplorerTxUrl() const;
  const EpochClaim* ClaimForEpoch(int64_t epoch) const;

  // ---- reliability
  void ApplyReliability(std::optional<urnet::ReliabilityWindow> window, Fetch state);

  // ---- the Seeker multiplier (points only)
  void ApplySeekerState();
  void ApplySeekerResult(uint32_t generation, bool ok, std::string const& serverError);

  // ---- leaderboard
  void ApplyLeaderboard(urnet::LeaderboardEarnersList const& earners, Fetch state);
  void ApplyRanking(urnet::NetworkRanking const& ranking, bool ok);
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
  PointsBreakdown accountPoints_;
  bool seekerHolder_ = false;
  bool verifyingSeeker_ = false;
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
  std::optional<GasKeyInfo> gas_;
  std::optional<HeadInfo> head_;
  bool chainSynced_ = false;

  int64_t leaderboardRank_ = 0;
  int64_t leaderboardCount_ = 0;
  bool rankingPublic_ = false;
  bool applyingRankingToggle_ = false;
  bool settingRankingPublic_ = false;

  Flow connectFlow_;
  Flow seekerFlow_;
  Flow rankingFlow_;

  std::shared_ptr<urnw::ClaimAlphaSheet> claimSheet_;

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
  void RenderPointsRows();
  void RenderPointsHeader();
  void RenderPointsFooter();
  void OnPointsSortChanged(std::string const& sort);
  void OnPointsScroll();
  void OnPointsRetry();
  void OnPointsPublicToggled();
  void SetPointsToggle(bool isPublic);
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
  bool pointsBoardShowing_ = false;
  std::vector<PointsRow> pointsRows_;
  std::string pointsSort_ = urnet::PointsLeaderboardSortPoints;
  std::string pointsRenderedSort_;
  bool pointsLoading_ = false;
  bool pointsEnd_ = false;
  bool pointsHasLoaded_ = false;  // the first page landed (rows, an empty end, or an error)
  std::string pointsError_;
  int64_t pointsTotalRanked_ = 0;
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

  // pane C's block (BuildPointsNetworkHost)
  winrt::Microsoft::UI::Xaml::Controls::TextBlock pointsGroupMeta_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock pointsEmojiText_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock pointsNameText_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Button editEmojiButton_{nullptr};
  PointsStatTile pointsTiles_[3];
  winrt::Microsoft::UI::Xaml::Controls::TextBlock pointsLongestText_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::ToggleSwitch pointsPublicToggle_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock pointsPrivateHint_{nullptr};
};

}  // namespace urnw
