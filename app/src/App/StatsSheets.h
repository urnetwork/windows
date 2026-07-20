// Detail sheets opened from the connect drawer stats cards, as ContentDialogs
// (macOS parity: client contracts, split rules + rule editor, custom DNS
// editor). Plain C++ helpers (no runtime classes); all methods run on the UI
// thread. The window forwards live store pushes into the open sheet.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.Shapes.h>

#include "InstalledApps.h"
#include "SdkHost.h"

namespace urnw {

// ---- Client / provider contracts -------------------------------------------
// Which traffic feed the sheet renders (macOS ContractDetailsMode parity).
enum class ContractDetailsMode { Client, Provider };

// Live per-contract details: one row per peer client, each row two independent
// newest-first stacks -- send (green) and receive (pink). Every circle is ONE
// contract: the outer ring is its total (area-proportional to the largest
// contract in that stack), the inner disc is the used fraction, and a contract
// moving bytes brightens its ring. Contracts slide off / drop in as they close
// and open. Grouping, the FINAL row ordering (the at-top activity sort and the
// scrolled-away freeze), the activity signal, the closing lifecycle, and the
// pending "N new" count all live in the shared SDK view controller (macOS
// ContractDetailsView / ContractDetailsStore parity); the sheet holds no
// ordering state -- it renders the rows in the order given, reports its scroll
// position via SdkHost::SetContractsAtTop, and reads SdkHost::ContractsPendingCount
// for the chip.
class ClientContractsSheet : public std::enable_shared_from_this<ClientContractsSheet> {
 public:
  static std::shared_ptr<ClientContractsSheet> Create(
      winrt::Microsoft::UI::Xaml::XamlRoot const& root, SdkHost& sdk,
      ContractDetailsMode mode = ContractDetailsMode::Client);

  winrt::Microsoft::UI::Xaml::Controls::ContentDialog Dialog() const { return dialog_; }
  // The SDK's already-ordered rows. Renders them in order as-is and refreshes
  // the "N new" chip; the SDK owns the ordering and the scrolled-away freeze.
  void Update(const std::vector<ContractPeerRow>& rows);
  // ~10 fps while the dialog is open: eases ring/disc sizes and advances the
  // contract slide-off / drop-in animations.
  void Tick();

 private:
  explicit ClientContractsSheet(SdkHost& sdk, ContractDetailsMode mode)
      : sdk_(sdk), mode_(mode) {}

  // One contract in a stack: a fixed-height slot with the contract circle and
  // the used/total counts beside it. Keyed by contract id for its whole life.
  struct BlockUi {
    std::string contractId;
    // horizontal [stats,circle] (send) / [circle,stats] (receive), fixed height
    winrt::Microsoft::UI::Xaml::Controls::StackPanel root{nullptr};
    winrt::Microsoft::UI::Xaml::Media::TranslateTransform shift{nullptr};  // slide in/out
    winrt::Microsoft::UI::Xaml::Shapes::Ellipse ring{nullptr};   // outer total ring
    // stream contracts only: a 2nd concentric ring just outside the main ring
    // (null for direct contracts); AnimateStack keeps it in step with the ring
    winrt::Microsoft::UI::Xaml::Shapes::Ellipse streamRing{nullptr};
    winrt::Microsoft::UI::Xaml::Shapes::Ellipse inner{nullptr};  // used-fraction disc
    winrt::Microsoft::UI::Xaml::Controls::TextBlock used{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock total{nullptr};
    int64_t usedByteCount = 0;
    int64_t totalByteCount = 0;
    int64_t bitRate = 0;
    // eased outer diameter (rescales when the stack max changes) and inner disc
    double diaFrom = 0, diaTo = 0, diaStart = 0;
    double innerFrom = 0, innerTo = 0, innerStart = 0;
    bool entering = false;  // dropping in at the top (fade + slide down)
    bool leaving = false;   // sliding off to the edge + fading, then removed
    double animStart = 0;
  };

  // One direction's stack: the header (title, arrow, summed bit rate), the pile
  // of contract circles (newest on top), and the "max N" scale anchor below.
  struct StackUi {
    winrt::Microsoft::UI::Xaml::Controls::StackPanel root{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock rate{nullptr};   // header bit rate
    winrt::Microsoft::UI::Xaml::Controls::StackPanel pile{nullptr};  // the circles
    winrt::Microsoft::UI::Xaml::Controls::TextBlock maxLabel{nullptr};
    winrt::Windows::UI::Color color{};
    bool mirrored = false;        // send: stats outside, circle toward the row center
    bool removalLeading = false;  // slide-off direction (send=left, receive=right)
    std::vector<BlockUi> blocks;  // on-screen, newest first; leavers hold their slot
  };

  struct RowUi {
    winrt::Microsoft::UI::Xaml::Controls::StackPanel root{nullptr};
    StackUi send;
    StackUi receive;
  };

  void Build(winrt::Microsoft::UI::Xaml::XamlRoot const& root);
  RowUi BuildRow(const std::string& clientId);
  StackUi BuildStack(bool mirrored, winrt::hstring const& title,
                     winrt::Windows::UI::Color color);
  BlockUi BuildBlock(StackUi const& stack, const ContractEntry& entry);
  // reconcile a stack's circles with the truth: update values live, admit
  // arrivals at the top, mark departures leaving
  void SyncStack(StackUi& stack, const std::vector<ContractEntry>& entries,
                 int64_t byteCount, double now);
  // advance eases + slide/fade animations; drop finished leavers (the survivors
  // settle via the pile's RepositionThemeTransition)
  static void AnimateStack(StackUi& stack, double now);
  void CopyClientId(const std::string& clientId);

  // ---- scroll reporting + "N new" chip -------------------------------------
  // the SDK VC owns the ordering, the freeze, and the pending count; the sheet
  // only reports at-top and renders/refreshes from what the VC hands back
  void OnScrollViewChanged();  // report at-top to the SDK VC
  void RenderList();           // reconcile list_ children to the SDK row order
  void UpdateChip();           // drive from SdkHost::ContractsPendingCount
  void ScrollToTop();          // chip tap: report at-top + scroll to the top

  SdkHost& sdk_;
  ContractDetailsMode mode_;
  winrt::Microsoft::UI::Xaml::Controls::ContentDialog dialog_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::ScrollViewer scroll_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::StackPanel list_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::StackPanel empty_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Button chip_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock chipText_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock copiedNote_{nullptr};

  std::unordered_map<std::string, RowUi> rowUis_;
  std::vector<std::string> renderedIds_;    // current list_ child order
  std::vector<ContractPeerRow> rows_;       // latest rows, already ordered by the SDK
  bool atTop_ = true;                       // last scroll state reported to the SDK VC
};

// ---- Split rules -----------------------------------------------------------
// Live routing decisions with the split rules pinned on top; tapping a row
// opens the in-dialog rule editor (checklist of host values routed locally).
class SplitRulesSheet : public std::enable_shared_from_this<SplitRulesSheet> {
 public:
  static std::shared_ptr<SplitRulesSheet> Create(
      winrt::Microsoft::UI::Xaml::XamlRoot const& root, SdkHost& sdk);

  winrt::Microsoft::UI::Xaml::Controls::ContentDialog Dialog() const { return dialog_; }
  void Update(std::vector<SplitRule> rules, std::vector<BlockActionItem> actions,
              int64_t allowed, int64_t blocked);
  void RefreshTimes();  // 1s cadence: relative "Ns ago" labels

 private:
  explicit SplitRulesSheet(SdkHost& sdk) : sdk_(sdk) {}

  void Build(winrt::Microsoft::UI::Xaml::XamlRoot const& root);
  void RenderRules();
  void RenderActivity();
  void OpenEditorForRule(const SplitRule& rule);
  void OpenEditorForAction(const BlockActionItem& action);
  void OpenEditor(std::string ruleId, std::vector<std::string> candidates,
                  std::set<std::string> selected);
  void ShowList();
  void ApplyEditor();

  SdkHost& sdk_;
  winrt::Microsoft::UI::Xaml::Controls::ContentDialog dialog_{nullptr};
  // list page
  winrt::Microsoft::UI::Xaml::Controls::ScrollViewer listPage_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::StackPanel rulesList_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::StackPanel activityList_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock countsText_{nullptr};
  // editor page
  winrt::Microsoft::UI::Xaml::Controls::StackPanel editorPage_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::StackPanel checklist_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Button applyButton_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Button removeButton_{nullptr};

  std::vector<SplitRule> rules_;
  std::vector<BlockActionItem> actions_;
  int64_t allowed_ = 0;
  int64_t blocked_ = 0;
  // activity rows are rebuilt only when the data changes; the time labels
  // refresh in place between pushes
  std::vector<std::pair<int64_t, winrt::Microsoft::UI::Xaml::Controls::TextBlock>>
      actionTimeLabels_;
  // editor state
  bool editing_ = false;
  std::string editRuleId_;
  std::vector<std::string> candidates_;
  std::set<std::string> selected_;
};

// ---- Per-app split tunnel (Android parity) ---------------------------------
// Lists installed apps (+ any existing per-app rules) and lets the user route each
// through the tunnel, bypass it, or leave it at the default. Each change calls
// SdkHost::SetAppRule / RemoveAppRule, which persists a BlockActionOverride and
// re-drives the split-tunnel driver from getLocalOverrideAppIds.
class AppRulesSheet : public std::enable_shared_from_this<AppRulesSheet> {
 public:
  static std::shared_ptr<AppRulesSheet> Create(
      winrt::Microsoft::UI::Xaml::XamlRoot const& root, SdkHost& sdk);
  winrt::Microsoft::UI::Xaml::Controls::ContentDialog Dialog() const { return dialog_; }

 private:
  explicit AppRulesSheet(SdkHost& sdk) : sdk_(sdk) {}
  void Build(winrt::Microsoft::UI::Xaml::XamlRoot const& root);
  void RenderList();

  SdkHost& sdk_;
  winrt::Microsoft::UI::Xaml::Controls::ContentDialog dialog_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::StackPanel appsList_{nullptr};
  std::vector<InstalledApp> installed_;
};

// ---- Custom DNS editor -----------------------------------------------------
// Draft copy of the device dns resolver settings; Update applies the draft via
// setDnsResolverSettings. Includes the regional recommendation / secure default
// panel, the four resolver switches, the fallback switch, regional server
// suggestions, and the four editable server-list sections (IPv4 + IPv6).
class DnsEditorSheet : public std::enable_shared_from_this<DnsEditorSheet> {
 public:
  static std::shared_ptr<DnsEditorSheet> Create(
      winrt::Microsoft::UI::Xaml::XamlRoot const& root, SdkHost& sdk,
      std::optional<urnet::DnsResolverSettings> const& current, std::string countryCode,
      std::string countryName);

  winrt::Microsoft::UI::Xaml::Controls::ContentDialog Dialog() const { return dialog_; }

 private:
  struct Draft {
    bool enableRemoteDoh = false;
    bool enableLocalDoh = false;
    bool enableRemoteDns = false;
    bool enableLocalDns = false;
    bool enableFallback = false;
    std::vector<std::string> remoteDohUrlsIpv4;
    std::vector<std::string> remoteDohUrlsIpv6;
    std::vector<std::string> localDohUrlsIpv4;
    std::vector<std::string> localDohUrlsIpv6;
    std::vector<std::string> remoteDnsIpv4;
    std::vector<std::string> remoteDnsIpv6;
    std::vector<std::string> localDnsIpv4;
    std::vector<std::string> localDnsIpv6;
  };
  friend bool operator==(const Draft& a, const Draft& b);

  // one editable value sublist (IPv4 or IPv6 of a section)
  struct ListUi {
    std::vector<std::string> Draft::*member = nullptr;
    bool doh = false;  // validation: https URL vs IP address
    winrt::Microsoft::UI::Xaml::Controls::StackPanel rows{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBox input{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Button add{nullptr};
  };
  struct SuggestionUi {
    urnet::RegionalDnsServer server;
    winrt::Microsoft::UI::Xaml::Controls::ToggleSwitch toggle{nullptr};
  };

  explicit DnsEditorSheet(SdkHost& sdk) : sdk_(sdk) {}

  static Draft FromSettings(std::optional<urnet::DnsResolverSettings> const& settings);
  static urnet::DnsResolverSettings ToSettings(const Draft& draft);

  void Build(winrt::Microsoft::UI::Xaml::XamlRoot const& root);
  void BuildResolverSection(winrt::Microsoft::UI::Xaml::Controls::StackPanel const& parent);
  void BuildSuggestionSection(winrt::Microsoft::UI::Xaml::Controls::StackPanel const& parent);
  // titles/labels/placeholders are localized strings (Localization.h), not literals
  void BuildListSection(winrt::Microsoft::UI::Xaml::Controls::StackPanel const& parent,
                        winrt::hstring const& title, winrt::hstring const& placeholder,
                        bool doh, std::vector<std::string> Draft::*ipv4,
                        std::vector<std::string> Draft::*ipv6);
  size_t AddSublist(winrt::Microsoft::UI::Xaml::Controls::StackPanel const& parent,
                    winrt::hstring const& label, winrt::hstring const& placeholder, bool doh,
                    std::vector<std::string> Draft::*member);
  void RenderRecommendationPanel();
  void RenderListRows(ListUi& list);
  void UpdateAddEnabled(ListUi& list);
  void AddValue(ListUi& list);
  void SyncFromDraft();     // draft -> all controls (guarded against re-entry)
  void OnDraftChanged();    // dirty state + recommendation panel

  SdkHost& sdk_;
  winrt::Microsoft::UI::Xaml::Controls::ContentDialog dialog_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::StackPanel recPanel_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::ToggleSwitch remoteDohToggle_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::ToggleSwitch localDohToggle_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::ToggleSwitch remoteDnsToggle_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::ToggleSwitch localDnsToggle_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::ToggleSwitch fallbackToggle_{nullptr};
  std::vector<SuggestionUi> suggestions_;
  std::vector<std::unique_ptr<ListUi>> lists_;

  Draft draft_;
  Draft original_;
  std::optional<Draft> recommendation_;  // for the connected country, when any
  std::optional<Draft> defaults_;        // most secure defaults
  std::string countryCode_;              // lowercased
  std::string countryName_;
  bool updating_ = false;
};

}  // namespace urnw
