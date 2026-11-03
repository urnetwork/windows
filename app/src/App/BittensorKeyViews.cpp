// SPDX-License-Identifier: MPL-2.0
// the project compiles with /Yu"pch.h" (App.vcxproj), so every translation unit
// must include it first
#include "pch.h"

#include "BittensorKeyViews.h"

#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Text.h>

#include "EarningsSheets.h"  // ShortAddress, DateFromMillis
#include "Localization.h"
#include "Log.h"
#include "PageContext.h"
#include "Strings.h"
#include "UrColors.h"
#include "UrComponents.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::UI::Xaml::Media;
using namespace urnw::pages;

namespace urnw::btkeyviews {
namespace {

constexpr wchar_t kWarningGlyph[] = L"\uE7BA";  // Warning
constexpr wchar_t kChevronGlyph[] = L"\uE76C";  // ChevronRight
constexpr wchar_t kCopyGlyph[] = L"\uE8C8";     // Copy
constexpr wchar_t kCheckGlyph[] = L"\uE73E";    // CheckMark
constexpr wchar_t kKeyGlyph[] = L"\uE8D7";      // Permissions, a key
constexpr wchar_t kNoKeyGlyph[] = L"\uE711";    // Cancel: the key is no longer here
constexpr wchar_t kShieldGlyph[] = L"\uEA18";   // Shield
constexpr wchar_t kRelayGlyph[] = L"\uE895";    // Sync: URnetwork claims it

// An app style by key, or null: the sheets' own idiom (AuthSheets.cpp).
Style StyleNamed(std::wstring_view key) {
  auto app = Application::Current();
  if (!app) return nullptr;
  return app.Resources().TryLookup(winrt::box_value(hstring{key})).try_as<Style>();
}

TextBlock MakeText(hstring const& text, double fontSize, Brush const& brush, bool semibold = false) {
  TextBlock block;
  block.Text(text);
  block.FontSize(fontSize);
  block.Foreground(brush);
  block.TextWrapping(TextWrapping::Wrap);
  if (semibold) block.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
  return block;
}

TextBlock Title(hstring const& text) { return MakeText(text, 14, colors::TextBrush(), true); }
TextBlock Body(hstring const& text) { return MakeText(text, 12, colors::MutedBrush()); }
TextBlock Danger(hstring const& text) { return MakeText(text, 12, colors::DangerBrush()); }

FontIcon Glyph(wchar_t const* glyph, double size, Brush const& brush) {
  FontIcon icon;
  icon.FontFamily(FontFamily(kit::kIconFontFamily));
  icon.Glyph(glyph);
  icon.FontSize(size);
  icon.Foreground(brush);
  return icon;
}

// A pane action, as the consent panel's own button, with a spinner beside its
// words while the flow is busy.
Button ActionButton(hstring const& text, bool primary, bool busy, bool enabled,
                    std::function<void()> action) {
  Button button;
  if (auto style = StyleNamed(primary ? L"UrPaneActionPrimaryStyle" : L"UrPaneActionSecondaryStyle")) {
    button.Style(style);
  }
  button.HorizontalAlignment(HorizontalAlignment::Stretch);
  if (busy) {
    StackPanel content;
    content.Orientation(Orientation::Horizontal);
    content.Spacing(8);
    ProgressRing ring;
    ring.Width(16);
    ring.Height(16);
    ring.IsActive(true);
    content.Children().Append(ring);
    TextBlock words;
    words.Text(text);
    words.VerticalAlignment(VerticalAlignment::Center);
    content.Children().Append(words);
    button.Content(content);
  } else {
    button.Content(winrt::box_value(text));
  }
  winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(button, text);
  button.IsEnabled(enabled && !busy);
  button.Click([action = std::move(action)](auto const&, auto const&) {
    if (action) action();
  });
  return button;
}

// The card's smaller links: Save first, Use a wallet, View key, Change wallet.
HyperlinkButton Link(hstring const& text, bool enabled, std::function<void()> action) {
  HyperlinkButton link;
  link.Content(winrt::box_value(text));
  link.Padding(Thickness{0, 0, 0, 0});
  link.HorizontalAlignment(HorizontalAlignment::Left);
  link.IsEnabled(enabled);
  link.Click([action = std::move(action)](auto const&, auto const&) {
    if (action) action();
  });
  return link;
}

// The address the consent pays, shortened, with Copy. Not a secret.
StackPanel AddressRow(std::string const& address) {
  StackPanel row;
  row.Orientation(Orientation::Horizontal);
  row.Spacing(8);
  TextBlock text;
  text.Text(hstring{ShortAddress(address)});
  text.FontSize(13);
  text.FontFamily(FontFamily(L"Consolas"));
  text.Foreground(colors::TextBrush());
  text.VerticalAlignment(VerticalAlignment::Center);
  ToolTipService::SetToolTip(text, winrt::box_value(hstring{Widen(address)}));
  winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(text, hstring{Widen(address)});
  row.Children().Append(text);
  Button copy;
  copy.Padding(Thickness{6, 2, 6, 2});
  copy.Background(colors::MakeBrush(winrt::Windows::UI::Color{0, 0, 0, 0}));
  copy.BorderThickness(Thickness{0, 0, 0, 0});
  auto icon = Glyph(kCopyGlyph, 13, colors::MutedBrush());
  copy.Content(icon);
  winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(copy, Loc("copy"));
  ToolTipService::SetToolTip(copy, winrt::box_value(Loc("copy")));
  copy.Click([address, icon](auto const&, auto const&) {
    namespace dt = winrt::Windows::ApplicationModel::DataTransfer;
    try {
      dt::DataPackage package;
      package.SetText(winrt::to_hstring(address));
      dt::Clipboard::SetContent(package);
      icon.Glyph(kCheckGlyph);
    } catch (winrt::hresult_error const&) {
    }
  });
  row.Children().Append(copy);
  return row;
}

// The claim's transaction on the chain's explorer, which the sdk names from the
// device's chain settings: an https page, or nothing.
void OpenTransaction(std::string const& url) {
  if (url.rfind("https://", 0) != 0) return;
  try {
    winrt::Windows::System::Launcher::LaunchUriAsync(winrt::Windows::Foundation::Uri(winrt::to_hstring(url)));
  } catch (winrt::hresult_error const&) {
    LogWarn("earnings: the claim's transaction could not be opened");
  }
}

// Where the key is: a coloured dot, or a glyph, beside the words.
Grid StatusRow(winrt::Windows::UI::Color const* dot, wchar_t const* glyph, hstring const& text) {
  Grid row;
  row.ColumnSpacing(8);
  ColumnDefinition lead;
  lead.Width(GridLengthHelper::Auto());
  row.ColumnDefinitions().Append(lead);
  row.ColumnDefinitions().Append(ColumnDefinition());
  FrameworkElement mark{nullptr};
  if (dot) {
    winrt::Microsoft::UI::Xaml::Shapes::Ellipse circle;
    circle.Width(8);
    circle.Height(8);
    circle.Fill(colors::MakeBrush(*dot));
    mark = circle;
  } else {
    mark = Glyph(glyph, 13, colors::MutedBrush());
  }
  mark.VerticalAlignment(VerticalAlignment::Center);
  Grid::SetColumn(mark, 0);
  row.Children().Append(mark);
  auto words = MakeText(text, 12, colors::TextBrush(), true);
  Grid::SetColumn(words, 1);
  row.Children().Append(words);
  winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(row, text);
  return row;
}

}  // namespace

hstring FailureText(std::string const& code, std::string const& message) {
  const btkey::FailureText text = btkey::FailureTextFor(code, message);
  if (!text.key.empty()) return Loc(text.key);
  return hstring{Widen(text.message)};
}

void DrawCard(StackPanel const& host, btkey::CardLayout const& layout,
              urnet::BittensorLocalKeyCard const& card, CardActions const& actions) {
  host.Children().Clear();
  auto add = [&host](UIElement const& element) { host.Children().Append(element); };
  const hstring failure = FailureText(card.failure_code, card.failure_message);
  auto addFailure = [&] {
    if (!failure.empty()) add(Danger(failure));
  };
  auto addOwnerOnly = [&] {
    // a session without the network credential cannot choose the payout wallet
    if (card.owner_only) add(Body(Loc("sn_consent_owner_only")));
  };
  auto addChangeWallet = [&] {
    if (!card.offers_wallet) return;
    add(ActionButton(Loc("sn_consent_change"), /*primary=*/false, /*busy=*/false, !card.busy,
                     actions.changeWallet));
    add(Body(Loc("sn_consent_change_note")));
  };
  const winrt::Windows::UI::Color green = colors::kUrGreen;
  const winrt::Windows::UI::Color amber = colors::kUrAmber;

  switch (layout.body) {
    case btkey::CardBody::SetUp:
      // 1a, 1b: the card holds the explanation, read before the one tap
      add(Title(Loc("bittensor_key_card_none")));
      add(Body(Loc("bittensor_key_card_short")));
      addFailure();
      add(ActionButton(Loc("bittensor_key_set_up"), /*primary=*/true, card.busy, card.offers_set_up,
                       actions.setUp));
      if (card.busy) add(Body(Loc("bittensor_key_setting_up")));
      if (card.offers_save_first) {
        add(Link(Loc("bittensor_key_save_first"), !card.busy, actions.saveFirst));
      }
      if (card.offers_wallet) add(Link(Loc("bittensor_key_use_wallet"), !card.busy, actions.useWallet));
      return;
    case btkey::CardBody::FinishSetup:
      // 3i: setup stopped between the store and the consent
      add(Title(Loc("bittensor_key_card_not_linked")));
      add(Body(Loc("bittensor_key_finish_body")));
      addFailure();
      if (card.offers_finish_setup) {
        add(ActionButton(Loc("bittensor_key_finish_setup"), /*primary=*/true, card.busy, true,
                         actions.finishSetup));
      }
      if (card.offers_wallet) add(Link(Loc("bittensor_key_use_wallet"), !card.busy, actions.useWallet));
      return;
    case btkey::CardBody::StoreUnavailable:
      add(Body(Loc("bittensor_key_failure_keychain")));
      add(Link(Loc("retry"), true, actions.reload));
      return;
    case btkey::CardBody::Linked:
      // 1c, 1i: the consent pays the key on this device
      add(Title(Loc(btkey::PayoutsTitleKey(card.payouts_start_next_epoch))));
      add(AddressRow(card.address));
      add(Body(Loc("bittensor_key_every_device")));
      if (card.key_saved) {
        add(StatusRow(&green, nullptr, Loc("bittensor_key_status_saved")));
        add(Link(Loc("bittensor_key_view"), true, actions.openKeyScreen));
      } else {
        add(StatusRow(&amber, nullptr, Loc("bittensor_key_status_unsaved")));
        add(ActionButton(Loc("bittensor_key_back_up"), /*primary=*/false, /*busy=*/false, true,
                         actions.openKeyScreen));
      }
      addFailure();
      addOwnerOnly();
      // a small Change wallet, which the frames leave out, so a network can
      // still move from the key to a wallet
      if (card.offers_wallet) add(Link(Loc("sn_consent_change"), !card.busy, actions.changeWallet));
      return;
    case btkey::CardBody::Deleted:
      // 1l, 2h: claims keep working; convert and one-tap Top 200 don't
      add(Title(Loc(btkey::PayoutsTitleKey(card.payouts_start_next_epoch))));
      add(AddressRow(card.address));
      add(Body(Loc("bittensor_key_every_device")));
      add(StatusRow(nullptr, kNoKeyGlyph, Loc("bittensor_key_no_longer_here")));
      add(Body(Loc("bittensor_key_deleted_body")));
      addFailure();
      addOwnerOnly();
      addChangeWallet();
      return;
    case btkey::CardBody::Elsewhere:
      // 3a, 3d: an outside wallet and another device's key are one card
      add(Title(hstring{urnw::Format("sn_consent_active", ShortAddress(card.address))}));
      add(StatusRow(nullptr, kKeyGlyph, Loc("bittensor_key_elsewhere")));
      add(Body(Loc("bittensor_key_elsewhere_body")));
      addFailure();
      addOwnerOnly();
      addChangeWallet();
      return;
    case btkey::CardBody::WalletCard:
      if (layout.otherConsentLines) {
        // a key here that the consent in force doesn't pay
        add(Body(Loc("bittensor_key_card_other_consent")));
        if (card.offers_finish_setup) {
          add(ActionButton(Loc("bittensor_key_finish_setup"), /*primary=*/false, card.busy, true,
                           actions.finishSetup));
        }
        if (card.offers_key_screen) {
          add(Link(Loc(card.key_saved ? "bittensor_key_view" : "bittensor_key_back_up"), true,
                   actions.openKeyScreen));
        }
        addFailure();
      }
      if (layout.storeUnavailableLines) {
        add(Body(Loc("bittensor_key_failure_keychain")));
        add(Link(Loc("retry"), true, actions.reload));
      }
      return;
  }
}

FrameworkElement MakeBackupBanner(std::function<void()> open) {
  Button button;
  button.HorizontalAlignment(HorizontalAlignment::Stretch);
  button.HorizontalContentAlignment(HorizontalAlignment::Stretch);
  button.Padding(Thickness{12, 10, 12, 10});
  button.Margin(Thickness{12, 12, 12, 4});
  button.Background(colors::CardBrush());
  button.BorderBrush(colors::MakeBrush(colors::kUrAmber));
  button.BorderThickness(Thickness{1, 1, 1, 1});
  button.CornerRadius(CornerRadius{8, 8, 8, 8});
  Grid grid;
  grid.ColumnSpacing(10);
  ColumnDefinition lead;
  lead.Width(GridLengthHelper::Auto());
  grid.ColumnDefinitions().Append(lead);
  grid.ColumnDefinitions().Append(ColumnDefinition());
  ColumnDefinition trail;
  trail.Width(GridLengthHelper::Auto());
  grid.ColumnDefinitions().Append(trail);
  auto warning = Glyph(kWarningGlyph, 16, colors::MakeBrush(colors::kUrAmber));
  warning.VerticalAlignment(VerticalAlignment::Top);
  Grid::SetColumn(warning, 0);
  grid.Children().Append(warning);
  StackPanel words;
  words.Spacing(4);
  words.Children().Append(Title(Loc("bittensor_key_back_up")));
  words.Children().Append(Body(Loc("bittensor_key_banner_body")));
  Grid::SetColumn(words, 1);
  grid.Children().Append(words);
  auto chevron = Glyph(kChevronGlyph, 12, colors::MutedBrush());
  chevron.VerticalAlignment(VerticalAlignment::Center);
  Grid::SetColumn(chevron, 2);
  grid.Children().Append(chevron);
  button.Content(grid);
  winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(button, Loc("bittensor_key_back_up"));
  button.Click([open = std::move(open)](auto const&, auto const&) {
    if (open) open();
  });
  return button;
}

FrameworkElement MakeFundsDisclaimer(std::string_view disclaimer, bool boxed) {
  const std::string key = btkey::DisclaimerKey(disclaimer);
  if (key.empty()) return nullptr;
  Grid grid;
  grid.ColumnSpacing(8);
  ColumnDefinition lead;
  lead.Width(GridLengthHelper::Auto());
  grid.ColumnDefinitions().Append(lead);
  grid.ColumnDefinitions().Append(ColumnDefinition());
  auto shield = Glyph(kShieldGlyph, 13, colors::MutedBrush());
  shield.VerticalAlignment(VerticalAlignment::Top);
  shield.Margin(Thickness{0, 2, 0, 0});
  Grid::SetColumn(shield, 0);
  grid.Children().Append(shield);
  // the owner's words, from the store, word for word
  auto words = MakeText(Loc(key), 12, colors::TextBrush());
  Grid::SetColumn(words, 1);
  grid.Children().Append(words);
  winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(grid, Loc(key));
  if (!boxed) return grid;
  Border box;
  box.Background(colors::CardBrush());
  box.CornerRadius(CornerRadius{8, 8, 8, 8});
  box.Padding(Thickness{12, 12, 12, 12});
  box.Child(grid);
  return box;
}

void ShowFundsDisclaimer(StackPanel const& host, std::string_view disclaimer, bool boxed) {
  host.Children().Clear();
  auto element = MakeFundsDisclaimer(disclaimer, boxed);
  if (element) host.Children().Append(element);
  host.Visibility(element ? Visibility::Visible : Visibility::Collapsed);
}

hstring ClaimNowNoteText(btkey::ClaimNowNote note, int64_t periodDays, int64_t floorTaoRao,
                         int64_t nextAtMillis) {
  switch (note) {
    case btkey::ClaimNowNote::Paid:
      return hstring{urnw::Format("sn_claim_relay_claim_now_paid", periodDays)};
    case btkey::ClaimNowNote::PaidEach:
      return hstring{urnw::Format("sn_claim_relay_claim_now_paid_each", periodDays)};
    case btkey::ClaimNowNote::UnderFloor:
      return hstring{urnw::Format("sn_claim_relay_under_floor", Widen(btkey::TaoAmount(floorTaoRao)))};
    case btkey::ClaimNowNote::Requested:
      return Loc("sn_claim_relay_requested");
    case btkey::ClaimNowNote::AgainOn:
      return hstring{urnw::Format("sn_claim_relay_again_on", DateFromMillis(nextAtMillis))};
    case btkey::ClaimNowNote::None:
      break;
  }
  return hstring{};
}

FrameworkElement MakeClaimedBanner(urnet::SnClaimStatusResult const& status) {
  StackPanel words;
  words.Spacing(4);
  const int64_t paid = status.claim_amount_rao.value_or(0);
  const std::string coldkey = status.coldkey_ss58.value_or(std::string());
  // what URnetwork's claim paid, on which key, in one sentence; Claimed without both
  const hstring sentence =
      btkey::ClaimedWordsFor(paid, coldkey) == btkey::ClaimedWords::OnKey
          ? hstring{urnw::Format("sn_claim_relay_claimed", Widen(urnet::formatAlphaAmount(paid)),
                                 ShortAddress(coldkey))}
          : Loc("claim_confirmed");
  words.Children().Append(MakeText(sentence, 13, colors::TextBrush()));
  const std::string url = status.claim_tx_url.value_or(std::string());
  if (!url.empty()) words.Children().Append(Link(Loc("transaction"), true, [url] { OpenTransaction(url); }));
  Grid grid;
  grid.ColumnSpacing(10);
  ColumnDefinition lead;
  lead.Width(GridLengthHelper::Auto());
  grid.ColumnDefinitions().Append(lead);
  grid.ColumnDefinitions().Append(ColumnDefinition());
  auto check = Glyph(kCheckGlyph, 16, colors::MakeBrush(colors::kUrGreen));
  check.VerticalAlignment(VerticalAlignment::Top);
  Grid::SetColumn(check, 0);
  grid.Children().Append(check);
  Grid::SetColumn(words, 1);
  grid.Children().Append(words);
  Border box;
  box.Background(colors::MakeBrush(colors::WithAlpha(colors::kUrGreen, 0x1F)));
  box.CornerRadius(CornerRadius{8, 8, 8, 8});
  box.Padding(Thickness{12, 10, 12, 10});
  box.Child(grid);
  winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(box, sentence);
  return box;
}

void DrawRelaySection(StackPanel const& host, urnet::SnClaimStatusResult const& status, bool inSheet,
                      std::function<void()> claimNow, std::function<void()> selfClaim) {
  host.Children().Clear();
  auto add = [&host](UIElement const& element) { host.Children().Append(element); };
  const bool relayClaims = btkey::RelayClaims(status.reason);
  if (relayClaims) {
    StackPanel row;
    row.Orientation(Orientation::Horizontal);
    row.Spacing(8);
    auto glyph = Glyph(kRelayGlyph, 13, colors::MakeBrush(colors::kUrLightBlue));
    glyph.VerticalAlignment(VerticalAlignment::Center);
    row.Children().Append(glyph);
    row.Children().Append(MakeText(Loc("sn_claim_relay_for_you"), 12, colors::TextBrush(), true));
    add(row);
  }
  // the claimed state (4d): what URnetwork's claim paid, where, and its transaction
  if (status.shows_claimed) add(MakeClaimedBanner(status));
  if (0 < status.claimable_rao) {
    // the pending pill, and why the balance waits
    Grid row;
    row.ColumnSpacing(8);
    ColumnDefinition lead;
    lead.Width(GridLengthHelper::Auto());
    row.ColumnDefinitions().Append(lead);
    row.ColumnDefinitions().Append(ColumnDefinition());
    Border pill;
    pill.Background(colors::BorderBrush());
    pill.CornerRadius(CornerRadius{9, 9, 9, 9});
    pill.Padding(Thickness{8, 2, 8, 2});
    pill.VerticalAlignment(VerticalAlignment::Top);
    pill.Child(MakeText(Loc("pending_data_key"), 12, colors::TextBrush(), true));
    Grid::SetColumn(pill, 0);
    row.Children().Append(pill);
    StackPanel why;
    why.Spacing(2);
    const btkey::ReasonText reason = btkey::ReasonTextFor(
        status.reason, status.automatic_threshold_tao_rao, status.automatic_threshold_rao);
    if (!reason.titleKey.empty()) why.Children().Append(MakeText(Loc(reason.titleKey), 12, colors::TextBrush(), true));
    switch (reason.detail) {
      case btkey::ReasonDetail::Threshold:
        why.Children().Append(Body(hstring{urnw::Format(
            "sn_claim_relay_under_threshold_body", Widen(btkey::TaoAmount(status.automatic_threshold_tao_rao)),
            Widen(btkey::ApproxAlpha(status.automatic_threshold_rao)))}));
        break;
      case btkey::ReasonDetail::ThresholdNoPrice:
        why.Children().Append(Body(hstring{urnw::Format(
            "sn_claim_relay_under_threshold_no_price", Widen(btkey::TaoAmount(status.automatic_threshold_tao_rao)))}));
        break;
      case btkey::ReasonDetail::Waits:
        why.Children().Append(Body(Loc("sn_claim_relay_waits")));
        break;
      case btkey::ReasonDetail::None:
        break;
    }
    Grid::SetColumn(why, 1);
    row.Children().Append(why);
    add(row);
  }
  if (inSheet) return;
  if (relayClaims) {
    // Claim now opens the claim sheet; under it, who pays, or why it can't act,
    // or in the claimed state when it is available again
    add(ActionButton(Loc("sn_claim_relay_claim_now"), /*primary=*/false, /*busy=*/false,
                     status.claim_now_available, std::move(claimNow)));
    const int64_t nextAtMillis = status.claim_now_next_at_millis.value_or(0);
    const btkey::ClaimNowNote kind =
        status.shows_claimed
            ? btkey::ClaimedNoteFor(nextAtMillis)
            : btkey::ClaimNowNoteFor(status.claim_now_available, status.claim_now_period_days,
                                     status.claim_now_reason.value_or(std::string()),
                                     status.transfer_floor_tao_rao, nextAtMillis, /*inSheet=*/false);
    const hstring note =
        ClaimNowNoteText(kind, status.claim_now_period_days, status.transfer_floor_tao_rao, nextAtMillis);
    if (!note.empty()) add(Body(note));
    return;
  }
  // URnetwork isn't claiming: the device's own claim, with its gas key, is the way
  add(ActionButton(Loc("sn_claim_relay_claim_now"), /*primary=*/false, /*busy=*/false, false, nullptr));
  add(Body(Loc("sn_claim_relay_waits_for_relay")));
  if (status.self_claim_available) add(Link(Loc("sn_claim_relay_self_claim"), true, std::move(selfClaim)));
}

}  // namespace urnw::btkeyviews
