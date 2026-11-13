// SPDX-License-Identifier: MPL-2.0
#include "pch.h"

#include "CryptoPayViews.h"

#include <winrt/Microsoft.UI.Xaml.Automation.Peers.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Windows.Globalization.DateTimeFormatting.h>

#include <algorithm>
#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <utility>

#include <qrcodegen.hpp>

#include "CryptoPaySdk.h"  // cryptopay::TermNoticeDue: the sdk's rule
#include "ExtenderPresentation.h"  // ExtenderQrRunsFor: one rectangle per run of dark modules
#include "Localization.h"
#include "Log.h"
#include "PageContext.h"
#include "PaymentRefusal.h"
#include "SettingsSheets.h"  // rows::CopyToClipboard
#include "Strings.h"
#include "SubscriptionBalance.h"
#include "UrColors.h"
#include "UrComponents.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::UI::Xaml::Media;
using urnw::pages::H;
using urnw::pages::Loc;
namespace automation = winrt::Microsoft::UI::Xaml::Automation;
namespace peers = winrt::Microsoft::UI::Xaml::Automation::Peers;
// wingdi.h declares ::Ellipse and ::Rectangle; alias the XAML shapes
using ShapeEllipse = winrt::Microsoft::UI::Xaml::Shapes::Ellipse;
using ShapeRectangle = winrt::Microsoft::UI::Xaml::Shapes::Rectangle;

namespace urnw {
namespace {

// the QR's ink and its frame: near-black on white, the contrast a camera
// reads best (the frame is the quiet zone, so it has to be light)
constexpr winrt::Windows::UI::Color kQrDark{255, 0x10, 0x10, 0x10};
constexpr winrt::Windows::UI::Color kQrLight{255, 0xFF, 0xFF, 0xFF};
constexpr winrt::Windows::UI::Color kTransparent{0, 0, 0, 0};
// how long a Copy button says Copied
constexpr auto kCopiedFor = std::chrono::seconds(2);
// A coin's mark at full contrast: black on a light theme, white on a dark
// one, so white on this app's (App.xaml RequestedTheme Dark). Never dimmed,
// not even for a coin that cannot be paid with now: Solana's and Bittensor's
// brand rules allow their marks only in black or white (mmm/BTCPAY.md §17.6).
constexpr winrt::Windows::UI::Color kMarkOnDark{255, 0xFF, 0xFF, 0xFF};
// a mark on the More coins tab, and beside a coin in its list
constexpr double kTabMarkSize = 16;
constexpr double kCoinMarkSize = 20;

TextBlock MakeText(hstring const& text, double size, Brush const& brush = nullptr,
                   bool wrap = false) {
  TextBlock block;
  block.Text(text);
  block.FontSize(size);
  if (brush) block.Foreground(brush);
  if (wrap) block.TextWrapping(TextWrapping::Wrap);
  return block;
}

// An address is read and compared character by character: a fixed-width face.
FontFamily MonoFont() { return FontFamily{L"Cascadia Mono, Consolas"}; }

void Show(UIElement const& element, bool visible) {
  if (element) element.Visibility(visible ? Visibility::Visible : Visibility::Collapsed);
}

std::optional<Style> AccentStyle() {
  auto resources = Application::Current().Resources();
  auto key = winrt::box_value(hstring(L"AccentButtonStyle"));
  if (resources.HasKey(key)) {
    if (auto style = resources.Lookup(key).try_as<Style>()) return style;
  }
  return std::nullopt;
}

// The catalog's text for `key`, or `english` while the catalog lacks the key:
// the sdk hands out each tab's and coin's label as a store key with its
// English.
std::wstring PaymentLabel(std::string const& key, std::string const& english) {
  if (key.empty()) return Widen(english);
  std::wstring text = Localized(key);
  if (text == Widen(key)) return Widen(english);
  return text;
}

// A coin's line as the sdk hands it out (cryptopay::CoinLine: its network,
// its warning, TAO's note), looked up as a label is; nothing for a line the
// coin does not have (no key).
std::wstring LineText(cryptopay::CoinLine const& line) {
  if (line.key.empty()) return std::wstring();
  return PaymentLabel(line.key, line.english);
}

// The running exe's folder, which holds Assets; empty when it cannot be read.
// Unbounded, as UpdateChecker's OwnExePath: the portable folder can sit under
// a long path.
std::filesystem::path ExeDir() {
  std::wstring path(MAX_PATH, L'\0');
  for (;;) {
    const DWORD n = ::GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (n == 0) return {};
    if (n < path.size()) {
      path.resize(n);
      return std::filesystem::path(std::move(path)).parent_path();
    }
    if (path.size() >= 0x8000) return {};  // beyond the NT path limit: give up
    path.resize(path.size() * 2);
  }
}

// The path data of the mark `iconKey` names, Assets\<iconKey>.svg beside the
// exe (App.vcxproj copies the four of More coins there), read once per key
// (UI thread only); "" for a key without a file, or a file that is not one
// plain path (cryptopay::SvgPathData).
std::wstring const& MarkPathData(std::string const& iconKey) {
  static std::map<std::string, std::wstring> read;
  if (const auto found = read.find(iconKey); found != read.end()) return found->second;
  std::wstring data;
  if (cryptopay::IsMarkKey(iconKey)) {
    const std::filesystem::path dir = ExeDir();
    if (!dir.empty()) {
      std::ifstream in(dir / L"Assets" / (Widen(iconKey) + L".svg"), std::ios::binary);
      std::ostringstream svg;
      svg << in.rdbuf();
      data = Widen(cryptopay::SvgPathData(svg.str()));
    }
    if (data.empty()) LogWarn("crypto pay: no mark for {}", iconKey);
  }
  return read.emplace(iconKey, std::move(data)).first->second;
}

// The coin's mark, `size` square at full contrast (kMarkOnDark), or nullptr
// when there is none: the label beside it names the coin either way. Path
// data (kit::MakeRowPathIcon fills it nonzero, the svg's rule: XAML's default
// even-odd would hollow out ETH's facets and TAO's stem).
PathIcon CoinMark(std::string const& iconKey, double size) {
  std::wstring const& data = MarkPathData(iconKey);
  if (data.empty()) return nullptr;
  try {
    return kit::MakeRowPathIcon(data, cryptopay::kMarkViewBox, size, colors::MakeBrush(kMarkOnDark));
  } catch (...) {
    // markup the reader refused: no mark
    LogWarn("crypto pay: the mark for {} did not load", iconKey);
    return nullptr;
  }
}

// The status line is a polite live region, so a screen reader hears the
// payment move on (seen, confirming, paid) without going to it. WinUI
// announces a live region only when its peer raises LiveRegionChanged, and a
// peer exists only while assistive technology is asking.
void Announce(FrameworkElement const& line) {
  if (!line) return;
  if (auto peer = peers::FrameworkElementAutomationPeer::FromElement(line)) {
    peer.RaiseAutomationEvent(peers::AutomationEvents::LiveRegionChanged);
  }
}

// The clipboard can be held by another program: a refused copy is logged and
// the invoice goes on.
void CopyText(std::string const& text) {
  if (text.empty()) return;
  try {
    rows::CopyToClipboard(text);
  } catch (...) {
    LogWarn("crypto invoice: the clipboard refused the copy");
  }
}

// A label over a value with its Copy button: the value in the fixed-width
// face, wrapping (an address has no break of its own) and selectable.
StackPanel MakeCopyBlock(hstring const& label, double valueSize, TextBlock& outValue,
                         Button& outCopy) {
  StackPanel block;
  block.Spacing(4);
  block.Children().Append(MakeText(label, 12, colors::MutedBrush(), true));
  Grid line;
  line.ColumnSpacing(8);
  ColumnDefinition valueColumn;
  valueColumn.Width(GridLength{1, GridUnitType::Star});
  ColumnDefinition copyColumn;
  copyColumn.Width(GridLength{0, GridUnitType::Auto});
  line.ColumnDefinitions().Append(valueColumn);
  line.ColumnDefinitions().Append(copyColumn);
  outValue = MakeText(hstring{}, valueSize, colors::TextBrush(), true);
  outValue.FontFamily(MonoFont());
  outValue.IsTextSelectionEnabled(true);
  outValue.VerticalAlignment(VerticalAlignment::Center);
  line.Children().Append(outValue);
  outCopy = Button();
  outCopy.Content(winrt::box_value(Loc("crypto_invoice_copy")));
  outCopy.VerticalAlignment(VerticalAlignment::Top);
  // three buttons say Copy: each says what it copies
  automation::AutomationProperties::SetHelpText(outCopy, label);
  Grid::SetColumn(outCopy, 1);
  line.Children().Append(outCopy);
  block.Children().Append(line);
  return block;
}

}  // namespace

std::wstring PaymentMethodName(std::string const& method) {
  const cryptopay::MethodLabelText label = cryptopay::MethodLabel(method);
  return PaymentLabel(label.key, label.english);
}

// ---- PaymentMethodRow -----------------------------------------------------------

StackPanel PaymentMethodRow::Build() {
  State& state = *state_;
  state.onSelect = &onSelect;
  state.root = StackPanel();
  state.root.Spacing(6);
  state.root.Visibility(Visibility::Collapsed);

  // the segments; "Pay with" names the row for UIA (the row has no visible
  // label: it sits directly above the button it changes)
  state.bar = SelectorBar();
  state.bar.HorizontalAlignment(HorizontalAlignment::Left);
  automation::AutomationProperties::SetName(state.bar, Loc("pay_method_label"));
  automation::AutomationProperties::SetAutomationId(state.bar, L"acceptance.upgrade.payment-method");
  state.bar.SelectionChanged([weak = std::weak_ptr<State>(state_)](
                                 SelectorBar const& sender, SelectorBarSelectionChangedEventArgs const&) {
    auto self = weak.lock();
    if (!self || self->applying) return;
    const SelectorBarItem item = sender.SelectedItem();
    if (!item) return;
    for (size_t index = 0; index < self->items.size(); ++index) {
      if (self->items[index] != item) continue;
      // Card's segment, or a tab: the sdk's coin for the tap (a tab of one
      // coin is that coin; More coins the coin chosen there last, else the
      // remembered one, else its first that can be paid with now)
      std::string method = cryptopay::kMethodCard;
      if (0 < index && index - 1 < self->tabs.size()) {
        cryptopay::PaymentTabView const& tab = self->tabs[index - 1];
        method = cryptopay::TabCoinFor(tab, self->lastChosen.In(tab.id), self->remembered);
      }
      if (method.empty() || method == self->selected) {
        Apply(*self);
        return;
      }
      self->selected = method;
      self->lastChosen.Choose(self->tabs, method);
      Apply(*self);
      if (self->onSelect && *self->onSelect) (*self->onSelect)(self->selected);
      return;
    }
  });
  state.root.Children().Append(state.bar);

  // the coins of More coins, under the bar while it is chosen
  state.coinsPanel = StackPanel();
  state.coinsPanel.Spacing(2);
  state.coinsPanel.Margin(Thickness{12, 0, 0, 0});
  state.coinsPanel.Visibility(Visibility::Collapsed);
  state.root.Children().Append(state.coinsPanel);

  state.unavailable = MakeText(Loc("pay_method_unavailable"), 12, colors::MakeBrush(colors::kUrAmber), true);
  state.unavailable.Visibility(Visibility::Collapsed);
  state.root.Children().Append(state.unavailable);
  return state.root;
}

void PaymentMethodRow::SetTabs(std::vector<cryptopay::PaymentTabView> const& tabs, std::string const& wanted) {
  State& state = *state_;
  if (!state.bar) return;
  if (tabs != state.tabs) {
    state.tabs = tabs;
    Rebuild(state_);
  }
  state.remembered = wanted;
  state.selected = cryptopay::MethodToSelect(state.tabs, wanted);
  state.lastChosen.Choose(state.tabs, state.selected);
  Apply(state);
}

void PaymentMethodRow::Rebuild(std::shared_ptr<State> const& shared) {
  State& state = *shared;
  const auto weak = std::weak_ptr<State>(shared);
  state.applying = true;
  state.bar.Items().Clear();
  state.items.clear();
  state.coinsPanel.Children().Clear();
  state.coins.clear();
  // Card first, then the sdk's tabs in its order; nothing at all without
  // them, which hides the row
  if (cryptopay::MethodRowShows(state.tabs)) {
    SelectorBarItem card;
    card.Text(hstring{PaymentMethodName(cryptopay::kMethodCard)});
    automation::AutomationProperties::SetAutomationId(card, L"acceptance.payment-method-card");
    state.bar.Items().Append(card);
    state.items.push_back(card);
  }
  for (cryptopay::PaymentTabView const& tab : state.tabs) {
    SelectorBarItem item;
    const hstring tabName{PaymentLabel(tab.labelKey, tab.label)};
    automation::AutomationProperties::SetAutomationId(item,
                                                      hstring{L"acceptance.payment-method-" + Widen(tab.id)});
    if (!tab.available && !cryptopay::TabOpensCoins(tab)) {
      // a coin that cannot be paid with now: its name greyed, with
      // "Temporarily unavailable" under it, as the item's Child (no Text, so
      // the template's own line collapses; the padding is the template's)
      StackPanel lines;
      lines.Margin(Thickness{12, 6, 12, 5});
      lines.Children().Append(MakeText(tabName, 14, colors::FaintBrush()));
      lines.Children().Append(MakeText(Loc("pay_method_unavailable"), 11, colors::MutedBrush()));
      item.Child(lines);
      automation::AutomationProperties::SetName(item, tabName);
    } else {
      item.Text(tabName);
    }
    if (cryptopay::TabOpensCoins(tab)) {
      // its coins' marks ahead of its name, each its own with space between
      // them, never merged into one badge (Solana's brand rules). They sit in
      // the item's Child, which its disabled state does not grey, at full
      // contrast whatever the tab's state; the padding is the template's.
      StackPanel marks;
      marks.Orientation(Orientation::Horizontal);
      marks.Spacing(8);
      marks.Margin(Thickness{12, 10, 0, 7});
      marks.VerticalAlignment(VerticalAlignment::Center);
      for (std::string const& iconKey : tab.iconKeys) {
        if (PathIcon mark = CoinMark(iconKey, kTabMarkSize)) marks.Children().Append(mark);
      }
      if (0 < marks.Children().Size()) item.Child(marks);
      // the marks stand for coins the name does not list: a screen reader
      // hears them
      std::vector<std::string> names;
      for (cryptopay::PaymentCoinView const& coin : tab.coins) {
        names.push_back(Narrow(PaymentLabel(coin.labelKey, coin.label)));
      }
      automation::AutomationProperties::SetName(
          item, hstring{Format("pay_method_more_coins_a11y", Widen(cryptopay::JoinNames(names)))});

      // the list under the bar, titled with the tab's name: each coin with
      // its mark, its name over its network, and while it cannot be paid with
      // now its name greyed and "Temporarily unavailable" under it, its mark
      // as it is
      state.coinsPanel.Children().Append(MakeText(tabName, 12, colors::MutedBrush()));
      for (cryptopay::PaymentCoinView const& coin : tab.coins) {
        Grid content;
        content.ColumnSpacing(10);
        ColumnDefinition markColumn;
        markColumn.Width(GridLength{0, GridUnitType::Auto});
        ColumnDefinition textColumn;
        textColumn.Width(GridLength{1, GridUnitType::Star});
        content.ColumnDefinitions().Append(markColumn);
        content.ColumnDefinitions().Append(textColumn);
        if (PathIcon mark = CoinMark(coin.iconKey, kCoinMarkSize)) content.Children().Append(mark);
        StackPanel texts;
        texts.Spacing(1);
        const std::wstring name = PaymentLabel(coin.labelKey, coin.label);
        texts.Children().Append(
            MakeText(hstring{name}, 14, coin.available ? colors::TextBrush() : colors::FaintBrush()));
        std::wstring help;
        const std::wstring network = LineText(coin.networkLine);
        if (!network.empty()) {
          texts.Children().Append(MakeText(hstring{network}, 12, colors::MutedBrush(), true));
          help = network;
        }
        if (!coin.available) {
          const hstring unavailable = Loc("pay_method_unavailable");
          texts.Children().Append(MakeText(unavailable, 12, colors::MutedBrush(), true));
          help += (help.empty() ? L"" : L". ") + std::wstring{unavailable.c_str()};
        }
        Grid::SetColumn(texts, 1);
        content.Children().Append(texts);

        // a radio in this panel's own group (no GroupName: the onboarding has
        // two rows on screen at once)
        RadioButton choice;
        choice.Content(content);
        choice.IsEnabled(state.enabled && coin.available);
        automation::AutomationProperties::SetName(choice, hstring{name});
        automation::AutomationProperties::SetHelpText(choice, hstring{help});
        automation::AutomationProperties::SetAutomationId(
            choice, hstring{L"acceptance.payment-method-" + Widen(coin.asset)});
        choice.Checked([weak, asset = coin.asset](auto const&, auto const&) {
          auto self = weak.lock();
          if (!self || self->applying || asset == self->selected) return;
          self->selected = asset;
          self->lastChosen.Choose(self->tabs, asset);
          Apply(*self);
          if (self->onSelect && *self->onSelect) (*self->onSelect)(self->selected);
        });
        state.coinsPanel.Children().Append(choice);
        state.coins.push_back(Coin{coin.asset, coin.available, choice});
      }
      automation::AutomationProperties::SetName(state.coinsPanel, tabName);
    }
    // a tab none of whose coins can be paid with now cannot be chosen, and its
    // label greys (its Child's, above, by hand)
    item.IsEnabled(tab.available);
    if (!tab.available) automation::AutomationProperties::SetHelpText(item, Loc("pay_method_unavailable"));
    state.bar.Items().Append(item);
    state.items.push_back(item);
  }
  state.applying = false;
}

void PaymentMethodRow::Select(std::string const& method) {
  state_->remembered = method;
  state_->selected = cryptopay::MethodToSelect(state_->tabs, method);
  state_->lastChosen.Choose(state_->tabs, state_->selected);
  if (state_->bar) Apply(*state_);
}

void PaymentMethodRow::SetEnabled(bool enabled) {
  state_->enabled = enabled;
  if (state_->bar) state_->bar.IsEnabled(enabled);
  for (Coin const& coin : state_->coins) coin.button.IsEnabled(enabled && coin.available);
}

void PaymentMethodRow::SetUnavailable(std::string const& method) {
  state_->unavailableMethod = method;
  if (state_->bar) Apply(*state_);
}

void PaymentMethodRow::Apply(State& state) {
  state.root.Visibility(cryptopay::MethodRowShows(state.tabs) ? Visibility::Visible : Visibility::Collapsed);
  // the selection's segment: Card's, or the tab its coin sits in (the sdk's
  // tab id), and More coins' list while that is the one
  const std::string tabId = cryptopay::TabIdFor(state.selected);
  size_t segment = 0;
  for (size_t index = 0; index < state.tabs.size(); ++index) {
    if (!tabId.empty() && state.tabs[index].id == tabId) segment = index + 1;
  }
  const bool coins = 0 < segment && cryptopay::TabOpensCoins(state.tabs[segment - 1]);
  state.applying = true;
  if (segment < state.items.size()) state.bar.SelectedItem(state.items[segment]);
  for (Coin const& coin : state.coins) coin.button.IsChecked(coin.asset == state.selected);
  state.applying = false;
  state.coinsPanel.Visibility(coins ? Visibility::Visible : Visibility::Collapsed);
  const bool unavailable = !state.unavailableMethod.empty() && state.unavailableMethod == state.selected;
  state.unavailable.Visibility(unavailable ? Visibility::Visible : Visibility::Collapsed);
}

// ---- CryptoInvoicePanel -----------------------------------------------------------

StackPanel CryptoInvoicePanel::Build(winrt::Microsoft::UI::Dispatching::DispatcherQueue const& queue,
                                     double qrMaxSide) {
  State& state = *state_;
  state.onOpenWallet = &onOpenWallet;
  state.onRefresh = &onRefresh;
  state.onPayAnotherWay = &onPayAnotherWay;
  state.qrMaxSide = qrMaxSide;
  auto weak = std::weak_ptr<State>(state_);

  StackPanel root;
  root.Spacing(14);

  // 1. the network the coin is sent on, first
  state.networkLine = MakeText(hstring{}, 22, colors::TextBrush(), true);
  state.networkLine.FontWeight(winrt::Windows::UI::Text::FontWeights::Bold());
  automation::AutomationProperties::SetHeadingLevel(state.networkLine,
                                                    peers::AutomationHeadingLevel::Level2);
  root.Children().Append(state.networkLine);

  // a start in flight, or a resumed invoice before its first status: a brief
  // loading line in place of the payment details (nothing is payable yet)
  state.busyLine = StackPanel();
  state.busyLine.Orientation(Orientation::Horizontal);
  state.busyLine.Spacing(10);
  state.busyLine.Margin(Thickness{0, 8, 0, 8});
  state.busyRing = ProgressRing();
  state.busyRing.Width(16);
  state.busyRing.Height(16);
  state.busyRing.IsActive(false);
  state.busyRing.VerticalAlignment(VerticalAlignment::Center);
  state.busyLine.Children().Append(state.busyRing);
  auto loading = MakeText(Loc("loading"), 14, colors::MutedBrush());
  loading.VerticalAlignment(VerticalAlignment::Center);
  state.busyLine.Children().Append(loading);
  state.busyLine.Visibility(Visibility::Collapsed);
  root.Children().Append(state.busyLine);

  // 2-3. what to send, where
  state.instructions = StackPanel();
  state.instructions.Spacing(12);
  // a coin with no payment link (TAO): nothing fills the amount in for the
  // payer, so the invoice says to send exactly this amount to this address
  state.byHandLine = MakeText(Loc("crypto_invoice_send_exactly_to_address"), 14, colors::TextBrush(), true);
  state.byHandLine.Visibility(Visibility::Collapsed);
  state.instructions.Children().Append(state.byHandLine);
  state.qrFrame = Border();
  state.qrFrame.Background(colors::MakeBrush(kQrLight));
  state.qrFrame.CornerRadius(CornerRadius{8, 8, 8, 8});
  state.qrFrame.HorizontalAlignment(HorizontalAlignment::Center);
  state.qrFrame.Visibility(Visibility::Collapsed);
  state.qrCanvas = Canvas();
  state.qrFrame.Child(state.qrCanvas);
  // a picture of the uri or the address: the words below say the same for a
  // screen reader
  automation::AutomationProperties::SetAccessibilityView(state.qrFrame,
                                                         automation::Peers::AccessibilityView::Raw);
  state.instructions.Children().Append(state.qrFrame);
  // under the QR of a coin with no payment link: it holds only the address,
  // and the exact amount is entered in the wallet
  state.noLinkNote = MakeText(hstring{}, 12, colors::MutedBrush(), true);
  state.noLinkNote.HorizontalAlignment(HorizontalAlignment::Center);
  state.noLinkNote.TextAlignment(TextAlignment::Center);
  state.noLinkNote.Visibility(Visibility::Collapsed);
  state.instructions.Children().Append(state.noLinkNote);

  state.openWallet = Button();
  state.openWallet.Content(winrt::box_value(Loc("crypto_invoice_open_wallet")));
  state.openWallet.HorizontalAlignment(HorizontalAlignment::Stretch);
  if (auto style = AccentStyle()) state.openWallet.Style(*style);
  automation::AutomationProperties::SetAutomationId(state.openWallet, L"acceptance.crypto-panel.open-wallet");
  state.openWallet.Click([weak](auto const&, auto const&) {
    if (auto self = weak.lock()) {
      if (self->onOpenWallet && *self->onOpenWallet) (*self->onOpenWallet)();
    }
  });
  state.instructions.Children().Append(state.openWallet);

  state.amountBlock = MakeCopyBlock(Loc("crypto_invoice_send_exactly"), 18, state.amountText,
                                    state.copyAmount);
  state.amountText.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
  state.copyAmount.Click([weak](auto const&, auto const&) {
    if (auto self = weak.lock()) {
      // the bare amount: a wallet's amount field takes no ticker
      CopyText(self->amount);
      ShowCopied(*self, self->copyAmount);
    }
  });
  state.instructions.Children().Append(state.amountBlock);

  state.addressBlock = MakeCopyBlock(Loc("crypto_invoice_to_address"), 13, state.addressText,
                                     state.copyAddress);
  state.copyAddress.Click([weak](auto const&, auto const&) {
    if (auto self = weak.lock()) {
      CopyText(self->address);
      ShowCopied(*self, self->copyAddress);
    }
  });
  state.instructions.Children().Append(state.addressBlock);

  // the coin's network to send on, under the address (a transfer on another
  // is not credited): the sdk's warning for the coin
  state.networkWarning = MakeText(hstring{}, 12, colors::MakeBrush(colors::kUrAmber), true);
  state.networkWarning.Visibility(Visibility::Collapsed);
  state.instructions.Children().Append(state.networkWarning);

  state.memoBlock = MakeCopyBlock(Loc("crypto_invoice_memo"), 13, state.memoText, state.copyMemo);
  state.copyMemo.Click([weak](auto const&, auto const&) {
    if (auto self = weak.lock()) {
      CopyText(self->memo);
      ShowCopied(*self, self->copyMemo);
    }
  });
  state.memoBlock.Visibility(Visibility::Collapsed);
  state.instructions.Children().Append(state.memoBlock);
  state.instructions.Visibility(Visibility::Collapsed);
  root.Children().Append(state.instructions);

  // 4. the quote: its countdown, or that it expired, with Refresh quote
  state.quoteRow = Grid();
  state.quoteRow.ColumnSpacing(8);
  ColumnDefinition quoteColumn;
  quoteColumn.Width(GridLength{1, GridUnitType::Star});
  ColumnDefinition ringColumn;
  ringColumn.Width(GridLength{0, GridUnitType::Auto});
  ColumnDefinition refreshColumn;
  refreshColumn.Width(GridLength{0, GridUnitType::Auto});
  state.quoteRow.ColumnDefinitions().Append(quoteColumn);
  state.quoteRow.ColumnDefinitions().Append(ringColumn);
  state.quoteRow.ColumnDefinitions().Append(refreshColumn);
  state.quoteText = MakeText(hstring{}, 14, colors::MutedBrush(), true);
  state.quoteText.VerticalAlignment(VerticalAlignment::Center);
  // the countdown changes every second: read when asked, never announced
  automation::AutomationProperties::SetLiveSetting(state.quoteText, peers::AutomationLiveSetting::Off);
  state.quoteRow.Children().Append(state.quoteText);
  state.refreshRing = ProgressRing();
  state.refreshRing.Width(16);
  state.refreshRing.Height(16);
  state.refreshRing.IsActive(false);
  state.refreshRing.Visibility(Visibility::Collapsed);
  state.refreshRing.VerticalAlignment(VerticalAlignment::Center);
  Grid::SetColumn(state.refreshRing, 1);
  state.quoteRow.Children().Append(state.refreshRing);
  state.refreshButton = Button();
  state.refreshButton.Content(winrt::box_value(Loc("crypto_invoice_refresh_quote")));
  state.refreshButton.VerticalAlignment(VerticalAlignment::Center);
  automation::AutomationProperties::SetAutomationId(state.refreshButton, L"acceptance.crypto-panel.refresh");
  state.refreshButton.Click([weak](auto const&, auto const&) {
    if (auto self = weak.lock()) {
      if (self->onRefresh && *self->onRefresh) (*self->onRefresh)();
    }
  });
  Grid::SetColumn(state.refreshButton, 2);
  state.quoteRow.Children().Append(state.refreshButton);
  state.quoteRow.Visibility(Visibility::Collapsed);
  root.Children().Append(state.quoteRow);

  // 5. the status: the sdk's steps, waiting -> payment seen -> confirming n of
  // N -> paid (two for USDC), one row each
  state.steps = StackPanel();
  state.steps.Spacing(8);
  for (int index = 0; index < 4; ++index) {
    Step step;
    step.row = StackPanel();
    step.row.Orientation(Orientation::Horizontal);
    step.row.Spacing(10);
    step.dot = ShapeEllipse();
    step.dot.Width(10);
    step.dot.Height(10);
    step.dot.StrokeThickness(2);
    step.dot.VerticalAlignment(VerticalAlignment::Center);
    automation::AutomationProperties::SetAccessibilityView(step.dot,
                                                           automation::Peers::AccessibilityView::Raw);
    step.row.Children().Append(step.dot);
    step.label = MakeText(hstring{}, 14, colors::MutedBrush(), true);
    automation::AutomationProperties::SetLiveSetting(step.label, peers::AutomationLiveSetting::Polite);
    step.row.Children().Append(step.label);
    state.steps.Children().Append(step.row);
    state.stepRows.push_back(step);
  }
  state.steps.Visibility(Visibility::Collapsed);
  root.Children().Append(state.steps);

  state.remainingText = MakeText(hstring{}, 14, colors::MakeBrush(colors::kUrAmber), true);
  state.remainingText.Visibility(Visibility::Collapsed);
  root.Children().Append(state.remainingText);

  state.reviewText = MakeText(Loc("crypto_status_review"), 14, colors::TextBrush(), true);
  automation::AutomationProperties::SetLiveSetting(state.reviewText, peers::AutomationLiveSetting::Polite);
  state.reviewText.Visibility(Visibility::Collapsed);
  root.Children().Append(state.reviewText);

  // 6. help
  state.walletsText = MakeText(hstring{}, 12, colors::MutedBrush(), true);
  state.walletsText.Visibility(Visibility::Collapsed);
  root.Children().Append(state.walletsText);
  state.zcashNote = MakeText(Loc("crypto_invoice_zcash_exchange_note"), 12, colors::MutedBrush(), true);
  state.zcashNote.Visibility(Visibility::Collapsed);
  root.Children().Append(state.zcashNote);

  // the last requote's failure
  state.errorText = MakeText(hstring{}, 12, colors::DangerBrush(), true);
  state.errorText.IsTextSelectionEnabled(true);
  state.errorText.Visibility(Visibility::Collapsed);
  root.Children().Append(state.errorText);

  // 7. another way (Close is the dialog's own)
  state.payAnotherWay = Button();
  state.payAnotherWay.Content(winrt::box_value(Loc("crypto_invoice_pay_another_way")));
  state.payAnotherWay.HorizontalAlignment(HorizontalAlignment::Stretch);
  state.payAnotherWay.Background(colors::MakeBrush(kTransparent));
  automation::AutomationProperties::SetAutomationId(state.payAnotherWay,
                                                    L"acceptance.crypto-panel.pay-another-way");
  state.payAnotherWay.Click([weak](auto const&, auto const&) {
    if (auto self = weak.lock()) {
      if (self->onPayAnotherWay && *self->onPayAnotherWay) (*self->onPayAnotherWay)();
    }
  });
  state.payAnotherWay.Visibility(Visibility::Collapsed);
  root.Children().Append(state.payAnotherWay);

  // "Copied" goes back to the button's own label after a moment
  state.copiedTimer = queue.CreateTimer();
  state.copiedTimer.Interval(std::chrono::duration_cast<winrt::Windows::Foundation::TimeSpan>(kCopiedFor));
  state.copiedTimer.IsRepeating(false);
  state.copiedTimer.Tick([weak](auto const&, auto const&) {
    if (auto self = weak.lock()) LabelCopyButtons(*self);
  });

  automation::AutomationProperties::SetAutomationId(root, L"acceptance.crypto-panel");
  return root;
}

void CryptoInvoicePanel::Render(cryptopay::CryptoInvoiceView const& view, bool uriAllowed,
                                std::vector<std::string> const& wallets) {
  State& state = *state_;
  if (!state.networkLine) return;
  const cryptopay::CryptoInvoiceDisplay display = cryptopay::DisplayFor(view, uriAllowed);

  // 1. the network, first
  state.networkLine.Text(hstring{LineText(display.networkLine)});
  state.busyRing.IsActive(display.busy);
  Show(state.busyLine, display.busy);

  // 2-3. what to send, where (CONTRACT §6.2.1): the amount, the QR and Open in
  // wallet only while payable; the QR is the sdk's qr text (a uri the sdk's
  // check and cryptopay::UriAllowed passed, or a coin with no payment link's
  // address alone), drawn again only when it changes, and Open in wallet only
  // for such a uri; the address also for a top-up, with the coin's network
  // warning
  Show(state.instructions, display.showAmount || display.showQr || display.showAddress);
  Show(state.byHandLine, display.showByHandLine);
  if (display.showQr) {
    if (state.qrText != display.qrText) DrawQr(state, display.qrText);
  } else if (!state.qrText.empty()) {
    state.qrCanvas.Children().Clear();
    state.qrText.clear();
  }
  const bool qrDrawn = display.showQr && !state.qrText.empty();
  Show(state.qrFrame, qrDrawn);
  state.noLinkNote.Text(hstring{LineText(display.noPaymentLinkNote)});
  Show(state.noLinkNote, qrDrawn && !display.noPaymentLinkNote.key.empty());
  Show(state.openWallet, display.showOpenWallet);
  if (display.noPaymentLink != state.copySaysWhat) {
    // a coin paid by hand: Copy amount and Copy address
    state.copySaysWhat = display.noPaymentLink;
    LabelCopyButtons(state);
  }
  state.amount = view.amount;
  state.amountText.Text(H(display.amountText));
  Show(state.amountBlock, display.showAmount);
  state.address = display.address;
  state.addressText.Text(H(display.address));
  Show(state.addressBlock, display.showAddress);
  state.networkWarning.Text(hstring{LineText(display.networkWarning)});
  Show(state.networkWarning, display.showNetworkWarning);
  state.memo = display.memo;
  state.memoText.Text(H(display.memo));
  Show(state.memoBlock, display.showMemo);

  // 4. the quote
  if (display.quote == cryptopay::QuoteLine::Counting) {
    state.quoteText.Text(hstring{Format("crypto_invoice_expires_in", Widen(display.countdown))});
  } else if (display.quote == cryptopay::QuoteLine::Expired) {
    state.quoteText.Text(Loc("crypto_invoice_quote_expired"));
  }
  Show(state.quoteText, display.quote != cryptopay::QuoteLine::Hidden);
  Show(state.refreshButton, display.showRefresh);
  state.refreshButton.IsEnabled(display.refreshEnabled);
  const bool refreshing = display.showRefresh && view.refreshing;
  state.refreshRing.IsActive(refreshing);
  Show(state.refreshRing, refreshing);
  Show(state.quoteRow, display.quote != cryptopay::QuoteLine::Hidden || display.showRefresh);

  // 5. the status steps: the sdk's, each drawn by its progress (a step the
  // payment skipped is done, and once paid every step is), the confirming
  // one with its own counts
  Show(state.steps, display.showSteps);
  const auto pending = colors::MakeBrush(colors::kTextFaint);
  for (size_t index = 0; index < state.stepRows.size(); ++index) {
    Step& row = state.stepRows[index];
    const bool listed = index < display.steps.size();
    Show(row.row, listed);
    if (!listed) continue;
    const cryptopay::PaymentStepView& step = display.steps[index];
    if (step.status == cryptopay::kStatusConfirming) {
      row.label.Text(hstring{Format("crypto_status_confirming", step.confirmations,
                                    step.confirmationsRequired)});
    } else {
      row.label.Text(Loc(cryptopay::StepKey(step.status)));
    }
    const bool done = step.progress == cryptopay::kStepDone;
    const bool current = step.progress == cryptopay::kStepCurrent;
    const auto accent = done ? colors::MakeBrush(colors::kUrGreen) : colors::MakeBrush(colors::kUrPink);
    row.dot.Stroke(done || current ? accent : pending);
    row.dot.Fill(done ? accent : colors::MakeBrush(kTransparent));
    row.label.Foreground(done || current ? colors::TextBrush() : pending);
    row.label.FontWeight(current ? winrt::Windows::UI::Text::FontWeights::SemiBold()
                                 : winrt::Windows::UI::Text::FontWeights::Normal());
  }
  // the one line that says where the payment is, for a screen reader, and
  // announced when the payment moves on: the current step's, or the last
  // once every step is done
  automation::AutomationProperties::SetName(state.steps, Loc(display.statusKey));
  if (display.statusKey != state.statusKey) {
    state.statusKey = display.statusKey;
    const int announced = cryptopay::AnnouncedStep(display.steps);
    if (display.review) {
      Announce(state.reviewText);
    } else if (display.showSteps && 0 <= announced &&
               announced < static_cast<int>(state.stepRows.size())) {
      Announce(state.stepRows[static_cast<size_t>(announced)].label);
    }
  }
  Show(state.remainingText, display.showRemaining);
  if (display.showRemaining) {
    state.remainingText.Text(hstring{Format("crypto_invoice_remaining", Widen(display.remainingText))});
  }
  Show(state.reviewText, display.review);

  // 6. help
  const std::string names = cryptopay::JoinNames(wallets);
  Show(state.walletsText, display.showWallets && !names.empty());
  if (!names.empty()) state.walletsText.Text(hstring{Format("crypto_invoice_wallets", Widen(names))});
  Show(state.zcashNote, display.showZcashNote);

  // a failure in this app's words (PaymentRefusal.h): the last requote's, or
  // an invoice the server no longer knows
  if (display.showUnavailable) {
    state.errorText.Text(hstring{Format("crypto_invoice_unavailable", PaymentMethodName(view.asset))});
  } else if (display.showRefusal) {
    const PaymentRefusalText refusal =
        PaymentRefusalTextFor(view.errorCode, view.errorMessage, "something_went_wrong");
    state.errorText.Text(hstring{PaymentRefusalMessage(Localized(refusal.key), Widen(refusal.detail))});
  }
  Show(state.errorText, display.showUnavailable || display.showRefusal);

  // 7. another way while nothing was received, or after a failure; else only
  // the sheet's Close, which hides the invoice and keeps it saved
  Show(state.payAnotherWay, display.showPayAnotherWay);
}

void CryptoInvoicePanel::Stop() {
  if (state_->copiedTimer) state_->copiedTimer.Stop();
}

void CryptoInvoicePanel::DrawQr(State& state, std::string const& text) {
  state.qrCanvas.Children().Clear();
  state.qrText.clear();
  int moduleCount = 0;
  std::vector<bool> dark;
  try {
    // level M: a payment uri carries a long address (Zcash's unified address
    // is over 200 characters), and M keeps the code at 57-69 modules, four dip
    // each in the frame (tools/crypto-pay-tests.cpp); a bare address is
    // shorter still
    const qrcodegen::QrCode code =
        qrcodegen::QrCode::encodeText(text.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
    moduleCount = code.getSize();
    dark.assign(static_cast<size_t>(moduleCount) * static_cast<size_t>(moduleCount), false);
    for (int y = 0; y < moduleCount; ++y) {
      for (int x = 0; x < moduleCount; ++x) {
        dark[static_cast<size_t>(y) * static_cast<size_t>(moduleCount) + static_cast<size_t>(x)] =
            code.getModule(x, y);
      }
    }
  } catch (const std::exception& e) {
    // qrcodegen::data_too_long: no picture, but the address and Open in
    // wallet still pay
    LogWarn("crypto invoice: qr encode failed: {}", e.what());
    return;
  }
  const cryptopay::QrLayout layout = cryptopay::QrLayoutFor(moduleCount, state.qrMaxSide);
  if (layout.moduleCount <= 0) return;
  state.qrCanvas.Width(layout.codeSide);
  state.qrCanvas.Height(layout.codeSide);
  state.qrFrame.Padding(Thickness{layout.quietZone, layout.quietZone, layout.quietZone, layout.quietZone});
  const auto ink = colors::MakeBrush(kQrDark);
  // one rectangle per horizontal run of dark modules, nothing cleared
  for (const ExtenderQrRun& run : ExtenderQrRunsFor(moduleCount, dark, 0, 0)) {
    ShapeRectangle cell;
    cell.Width(run.length * layout.moduleSize);
    cell.Height(layout.moduleSize);
    cell.Fill(ink);
    Canvas::SetLeft(cell, run.x * layout.moduleSize);
    Canvas::SetTop(cell, run.y * layout.moduleSize);
    state.qrCanvas.Children().Append(cell);
  }
  state.qrText = text;
}

void CryptoInvoicePanel::LabelCopyButtons(State& state) {
  if (!state.copyAmount) return;
  // a coin paid by hand (TAO) has two values to carry over to the wallet:
  // each button names its own
  state.copyAmount.Content(winrt::box_value(
      Loc(state.copySaysWhat ? "crypto_invoice_copy_amount" : "crypto_invoice_copy")));
  state.copyAddress.Content(winrt::box_value(
      Loc(state.copySaysWhat ? "crypto_invoice_copy_address" : "crypto_invoice_copy")));
  state.copyMemo.Content(winrt::box_value(Loc("crypto_invoice_copy")));
}

void CryptoInvoicePanel::ShowCopied(State& state, Button const& copied) {
  LabelCopyButtons(state);
  copied.Content(winrt::box_value(Loc("crypto_invoice_copied")));
  if (state.copiedTimer) {
    state.copiedTimer.Stop();
    state.copiedTimer.Start();
  }
}

// ---- CryptoAccountRows ---------------------------------------------------------

StackPanel CryptoAccountRows::Build() {
  State& state = *state_;
  state.onRenew = &onRenew;
  state.onOpenInvoice = &onOpenInvoice;
  auto weak = std::weak_ptr<State>(state_);
  state.root = StackPanel();

  // a plan paid once in crypto: nothing renews it, nothing cancels it
  state.doesNotRenewRow = Border();
  state.doesNotRenewRow.Padding(Thickness{12, 10, 12, 10});
  state.doesNotRenewRow.BorderBrush(colors::BorderBrush());
  state.doesNotRenewRow.BorderThickness(Thickness{0, 0, 0, 1});
  state.doesNotRenewRow.Child(MakeText(Loc("crypto_does_not_renew"), 13, colors::MutedBrush(), true));
  state.doesNotRenewRow.Visibility(Visibility::Collapsed);
  state.root.Children().Append(state.doesNotRenewRow);

  // the paid term has ended: renew within the grace (an in-app notice, never
  // a system notification)
  state.termNotice = InfoBar();
  state.termNotice.Severity(InfoBarSeverity::Warning);
  state.termNotice.IsClosable(false);
  state.termNotice.IsOpen(false);
  state.termNotice.Margin(Thickness{12, 8, 12, 8});
  Button renew;
  renew.Content(winrt::box_value(Loc("crypto_term_renew")));
  automation::AutomationProperties::SetAutomationId(renew, L"acceptance.account.crypto-renew");
  renew.Click([weak](auto const&, auto const&) {
    if (auto self = weak.lock()) {
      if (self->onRenew && *self->onRenew) (*self->onRenew)();
    }
  });
  state.termNotice.ActionButton(renew);
  state.termNotice.Visibility(Visibility::Collapsed);
  state.root.Children().Append(state.termNotice);

  // a saved invoice not paid yet: the row reopens it
  auto row = kit::MakePaneTwoLineRowButton(Loc("crypto_payment_in_progress"));
  automation::AutomationProperties::SetAutomationId(row.root, L"acceptance.account.crypto-in-progress");
  row.root.Click([weak](auto const&, auto const&) {
    if (auto self = weak.lock()) {
      if (self->onOpenInvoice && *self->onOpenInvoice) (*self->onOpenInvoice)();
    }
  });
  row.root.Visibility(Visibility::Collapsed);
  state.inProgress = row.root;
  state.inProgressNote = row.note;
  state.inProgressValue = row.value;
  state.root.Children().Append(row.root);
  return state.root;
}

void CryptoAccountRows::ApplyBalance(BalanceSnapshot const& balance) {
  State& state = *state_;
  if (!state.root) return;
  // the sdk's rule: from the paid term's end until the grace ends, never before
  const bool notice = !balance.guest && cryptopay::TermNoticeDue(balance.cryptoTerm);
  if (notice) {
    // "Renew by <the grace's last day>", in the user's locale and time zone
    std::wstring date;
    if (const std::optional<int64_t> graceEnd = cryptopay::ParseRfc3339(balance.cryptoTerm.graceEndTime)) {
      try {
        winrt::Windows::Globalization::DateTimeFormatting::DateTimeFormatter formatter(L"shortdate");
        date = std::wstring{formatter.Format(
            winrt::clock::from_sys(std::chrono::sys_seconds{std::chrono::seconds{*graceEnd}}))};
      } catch (...) {
        date = Widen(balance.cryptoTerm.graceEndTime.substr(0, 10));
      }
    }
    state.termNotice.Message(hstring{Format("crypto_term_ended_notice", date)});
  }
  state.termNotice.IsOpen(notice);
  Show(state.termNotice, notice);
  Show(state.doesNotRenewRow,
       !balance.guest && cryptopay::DoesNotRenewShows(balance.subscriptionStoreFamily, notice));
}

void CryptoAccountRows::ApplyInvoice(cryptopay::CryptoInvoiceView const& view) {
  State& state = *state_;
  if (!state.root) return;
  const bool inProgress = cryptopay::ShowsPaymentInProgress(view);
  Show(state.inProgress, inProgress);
  if (!inProgress) return;
  // the status on the second line (cryptopay::InProgressNoteKey), the coin's
  // network at the right
  const std::string noteKey = cryptopay::InProgressNoteKey(view);
  const hstring status = noteKey.empty() ? hstring{}
                         : noteKey == "crypto_status_confirming"
                             ? hstring{Format("crypto_status_confirming", view.confirmations,
                                              view.confirmationsRequired)}
                             : Loc(noteKey);
  kit::SetTextOrCollapse(state.inProgressNote, status);
  state.inProgressValue.Text(hstring{LineText(view.lines.networkLine)});
  automation::AutomationProperties::SetFullDescription(state.inProgress, status);
}

}  // namespace urnw
