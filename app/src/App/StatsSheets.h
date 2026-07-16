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

// ---- Client contracts ------------------------------------------------------
// One row per peer client aggregating the contract (egress, green) and
// companion (ingress, pink) transfer, visualized as two usage circles with
// directional transfer lines between them.
class ClientContractsSheet : public std::enable_shared_from_this<ClientContractsSheet> {
 public:
  static std::shared_ptr<ClientContractsSheet> Create(
      winrt::Microsoft::UI::Xaml::XamlRoot const& root);

  winrt::Microsoft::UI::Xaml::Controls::ContentDialog Dialog() const { return dialog_; }
  void Update(const std::vector<ContractClientRow>& rows);
  // ~10 fps while the dialog is open: eases the inner disc sizes and plays the
  // contract-swap fade/slide (macOS parity: 0.5s ease-out transitions).
  void Tick();

 private:
  struct CircleUi {
    winrt::Microsoft::UI::Xaml::Shapes::Ellipse inner{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock used{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock total{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Grid ring{nullptr};
    winrt::Microsoft::UI::Xaml::Media::TranslateTransform ringShift{nullptr};
    // eased disc size (from -> to starting at start, cubic ease-out 0.5s)
    double sizeFrom = 0;
    double sizeTo = 0;
    double sizeStart = 0;
    // a replaced contract (new id signature) fades/slides the circle back in
    double swapStart = 0;
    bool swapFromLeft = false;
    std::string signature;
  };
  struct LineUi {
    winrt::Microsoft::UI::Xaml::Controls::TextBlock rate{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Grid line{nullptr};
  };
  struct RowUi {
    winrt::Microsoft::UI::Xaml::Controls::StackPanel root{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock pairCount{nullptr};
    CircleUi contract;
    CircleUi companion;
    LineUi contractLine;
    LineUi companionLine;
  };

  void Build(winrt::Microsoft::UI::Xaml::XamlRoot const& root);
  RowUi BuildRow(const std::string& clientId);
  void UpdateRow(RowUi& ui, const ContractClientRow& row);
  static void AnimateCircle(CircleUi& circle, double now);
  void CopyClientId(const std::string& clientId);

  winrt::Microsoft::UI::Xaml::Controls::ContentDialog dialog_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::ScrollViewer scroll_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::StackPanel list_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::StackPanel empty_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock copiedNote_{nullptr};
  std::unordered_map<std::string, RowUi> rowUis_;
  std::vector<std::string> renderedIds_;
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
