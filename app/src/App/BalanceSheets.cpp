// SPDX-License-Identifier: MPL-2.0
#include "pch.h"

#include "BalanceSheets.h"

#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Documents.h>
#include <winrt/Microsoft.Web.WebView2.Core.h>

#include <algorithm>
#include <map>

#include "Localization.h"
#include "Paths.h"
#include "StatsFormat.h"
#include "Strings.h"
#include "UrColors.h"

using namespace winrt;
using namespace winrt::Windows::Foundation;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::UI::Xaml::Documents;
using namespace winrt::Microsoft::UI::Xaml::Media;

// NOTE on captures: control event handlers capture the owning sheet weakly
// (see StatsSheets.cpp). The window holds the sheet's shared_ptr while the
// dialog is showing, so lock() always succeeds during interaction.

namespace urnw {
namespace {

// wingdi.h declares ::Ellipse; alias the XAML shape for unqualified use
using ShapeEllipse = winrt::Microsoft::UI::Xaml::Shapes::Ellipse;

constexpr winrt::Windows::UI::Color kTransparent{0, 0, 0, 0};
// a redeemable balance code is exactly 26 characters (macOS RedeemBalanceCodeSheet)
constexpr size_t kBalanceCodeLength = 26;

// The ur.io bridge page (mmm/ur.io react EmbeddedCheckout.jsx): mounts Stripe's
// Embedded Checkout for the session's client_secret — the card form stays in
// Stripe's iframe, no card data ever touches the app — and hands control back
// by navigating to the redirect_link:
//   done:  urnetwork://checkout?status=complete&session_id=cs_...
//   error: urnetwork://checkout?errorCode=-1&errorMessage=...
// There is no cancel url: Stripe's embedded flow never leaves the page, so the
// checkout header's own close (X) is the only way out. (Linux UpgradeSheet
// parity; the wallet-connect bridge uses the same envelope.)
constexpr const char* kCheckoutPage = "https://ur.io/checkout";
constexpr const char* kCheckoutRedirect = "urnetwork://checkout";
constexpr const char* kCheckoutScheme = "urnetwork://";

hstring H(std::string const& s) { return winrt::to_hstring(s); }

// True when the Evergreen WebView2 runtime (msedgewebview2) is installed. The
// WebView2 SDK ships with the app, but the runtime is a separate install —
// in-box on Windows 11, usual-but-not-guaranteed on Windows 10 — so embedded
// checkout probes before asking the server for an embedded session, and the
// hosted flow covers the miss with no error surfaced.
bool WebView2RuntimeAvailable() {
  try {
    return !winrt::Microsoft::Web::WebView2::Core::CoreWebView2Environment::
                GetAvailableBrowserVersionString()
                .empty();
  } catch (...) {
    return false;
  }
}

// Percent-encode everything except RFC 3986 unreserved characters
// (WalletConnect.cpp builds its bridge urls the same way).
std::string Esc(std::string const& s) {
  static const char* hex = "0123456789ABCDEF";
  std::string out;
  out.reserve(s.size() * 3);
  for (unsigned char c : s) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
        c == '-' || c == '_' || c == '.' || c == '~') {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(hex[c >> 4]);
      out.push_back(hex[c & 0xF]);
    }
  }
  return out;
}

std::string Unesc(std::string const& s) {
  auto hexv = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
  };
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%' && i + 2 < s.size()) {
      const int hi = hexv(s[i + 1]), lo = hexv(s[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
        continue;
      }
    }
    out.push_back(s[i] == '+' ? ' ' : s[i]);
  }
  return out;
}

// The query parameters of a urnetwork:// callback, percent-decoded.
std::map<std::string, std::string> ParseQuery(std::string const& url) {
  std::map<std::string, std::string> out;
  const size_t q = url.find('?');
  if (q == std::string::npos) return out;
  size_t i = q + 1;
  while (i < url.size()) {
    const auto amp = url.find('&', i);
    const std::string pair =
        url.substr(i, amp == std::string::npos ? std::string::npos : amp - i);
    const auto eq = pair.find('=');
    if (eq != std::string::npos) out[pair.substr(0, eq)] = Unesc(pair.substr(eq + 1));
    if (amp == std::string::npos) break;
    i = amp + 1;
  }
  return out;
}

// A UI string from the shared localization store, by key id (Localization.h).
// Every user-facing string in these sheets comes through Loc or urnw::Format.
hstring Loc(std::string_view key) { return hstring{Localized(key)}; }

std::string TrimWhitespace(std::string const& value) {
  const auto begin = value.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) return "";
  const auto end = value.find_last_not_of(" \t\r\n");
  return value.substr(begin, end - begin + 1);
}

TextBlock MakeText(hstring const& text, double fontSize, Brush const& brush = nullptr,
                   bool wrap = false) {
  TextBlock tb;
  tb.Text(text);
  tb.FontSize(fontSize);
  if (brush) tb.Foreground(brush);
  if (wrap) tb.TextWrapping(TextWrapping::Wrap);
  return tb;
}

ContentDialog MakeDialog(XamlRoot const& root, hstring const& title) {
  ContentDialog dialog;
  dialog.XamlRoot(root);
  if (!title.empty()) dialog.Title(winrt::box_value(title));
  dialog.CloseButtonText(Loc("close"));
  // brand sheet surface (macOS sheet background)
  dialog.Background(colors::BackgroundBrush());
  return dialog;
}

std::optional<Style> AccentButtonStyle() {
  auto resources = Application::Current().Resources();
  auto key = winrt::box_value(hstring(L"AccentButtonStyle"));
  if (resources.HasKey(key)) {
    if (auto style = resources.Lookup(key).try_as<Style>()) return style;
  }
  return std::nullopt;
}

// a tappable hyperlink run for `label`, opening `url` in the default browser
Hyperlink MakeLink(std::wstring const& label, std::wstring const& url) {
  Hyperlink link;
  try {
    link.NavigateUri(Uri(hstring{url}));
  } catch (...) {
    // a malformed store url leaves plain (unlinked) styled text
  }
  Run run;
  run.Text(hstring{label});
  link.Inlines().Append(run);
  return link;
}

void AppendRun(TextBlock const& text, std::wstring const& value) {
  if (value.empty()) return;
  Run run;
  run.Text(hstring{value});
  text.Inlines().Append(run);
}

}  // namespace

void SetMarkdownLinkText(TextBlock const& text, std::wstring const& value,
                         double fontSize) {
  text.Inlines().Clear();
  text.FontSize(fontSize);
  text.TextWrapping(TextWrapping::Wrap);
  size_t pos = 0;
  while (pos < value.size()) {
    const size_t open = value.find(L'[', pos);
    const size_t mid = open == std::wstring::npos ? std::wstring::npos
                                                  : value.find(L"](", open + 1);
    const size_t end =
        mid == std::wstring::npos ? std::wstring::npos : value.find(L')', mid + 2);
    if (open == std::wstring::npos || mid == std::wstring::npos ||
        end == std::wstring::npos) {
      AppendRun(text, value.substr(pos));
      return;
    }
    AppendRun(text, value.substr(pos, open - pos));
    text.Inlines().Append(MakeLink(value.substr(open + 1, mid - open - 1),
                                   value.substr(mid + 2, end - mid - 2)));
    pos = end + 1;
  }
}

void SetTermsMarkerText(TextBlock const& text, std::wstring const& value,
                        double fontSize) {
  struct Marker {
    std::wstring_view start;
    std::wstring_view end;
    std::wstring_view url;
  };
  static constexpr Marker kMarkers[] = {
      {L"{terms_start}", L"{terms_end}", L"https://ur.io/terms"},
      {L"{privacy_start}", L"{privacy_end}", L"https://ur.io/privacy"},
  };

  text.Inlines().Clear();
  text.FontSize(fontSize);
  text.TextWrapping(TextWrapping::Wrap);
  size_t pos = 0;
  while (pos < value.size()) {
    // the earliest start marker at/after pos
    const Marker* next = nullptr;
    size_t nextAt = std::wstring::npos;
    for (auto const& marker : kMarkers) {
      const size_t at = value.find(marker.start, pos);
      if (at != std::wstring::npos && at < nextAt) {
        next = &marker;
        nextAt = at;
      }
    }
    if (!next) {
      AppendRun(text, value.substr(pos));
      return;
    }
    const size_t innerBegin = nextAt + next->start.size();
    const size_t innerEnd = value.find(next->end, innerBegin);
    if (innerEnd == std::wstring::npos) {
      AppendRun(text, value.substr(pos));
      return;
    }
    AppendRun(text, value.substr(pos, nextAt - pos));
    text.Inlines().Append(MakeLink(value.substr(innerBegin, innerEnd - innerBegin),
                                   std::wstring{next->url}));
    pos = innerEnd + next->end.size();
  }
}

// ---- RedeemCodeSheet --------------------------------------------------------

std::shared_ptr<RedeemCodeSheet> RedeemCodeSheet::Create(XamlRoot const& root,
                                                         SdkHost& sdk,
                                                         std::function<void()> onRedeemed) {
  auto sheet = std::shared_ptr<RedeemCodeSheet>(
      new RedeemCodeSheet(sdk, std::move(onRedeemed)));
  sheet->Build(root);
  return sheet;
}

void RedeemCodeSheet::Build(XamlRoot const& root) {
  dialog_ = MakeDialog(root, Loc("redeem_code"));
  dialog_.PrimaryButtonText(Loc("redeem"));
  dialog_.IsPrimaryButtonEnabled(false);
  dialog_.DefaultButton(ContentDialogButton::Primary);

  Grid content;
  content.MinWidth(400);

  // form: code entry + inline error + where-to-get-codes note
  formPanel_ = StackPanel();
  formPanel_.Spacing(8);

  codeBox_ = TextBox();
  codeBox_.Header(winrt::box_value(Loc("balance_code")));
  codeBox_.PlaceholderText(Loc("enter_balance_code"));
  codeBox_.MaxLength(static_cast<int32_t>(kBalanceCodeLength));
  codeBox_.TextChanged([weak = weak_from_this()](auto const&, auto const&) {
    if (auto self = weak.lock()) {
      const std::string code = TrimWhitespace(urnw::Narrow(self->codeBox_.Text().c_str()));
      self->dialog_.IsPrimaryButtonEnabled(!self->redeeming_ &&
                                           code.size() == kBalanceCodeLength);
      self->errorText_.Visibility(Visibility::Collapsed);
    }
  });
  formPanel_.Children().Append(codeBox_);

  errorText_ = MakeText(Loc("invalid_balance_code"), 12, colors::DangerBrush(), true);
  errorText_.Visibility(Visibility::Collapsed);
  formPanel_.Children().Append(errorText_);

  // where codes come from: plain text, deliberately no link (macOS parity)
  auto note = MakeText(Loc("balance_code_where_to_buy"), 12, colors::MutedBrush(), true);
  note.Margin(Thickness{0, 4, 0, 0});
  formPanel_.Children().Append(note);

  content.Children().Append(formPanel_);

  // success: green check + confirmation + the amount added
  successPanel_ = StackPanel();
  successPanel_.Spacing(8);
  successPanel_.Visibility(Visibility::Collapsed);
  successPanel_.HorizontalAlignment(HorizontalAlignment::Center);
  successPanel_.Margin(Thickness{0, 16, 0, 16});
  auto check = MakeText(hstring{L"✓"}, 32, colors::MakeBrush(colors::kUrGreen));
  check.HorizontalAlignment(HorizontalAlignment::Center);
  successPanel_.Children().Append(check);
  auto redeemed = MakeText(Loc("balance_code_redeemed"), 15, colors::TextBrush(), true);
  redeemed.HorizontalAlignment(HorizontalAlignment::Center);
  successPanel_.Children().Append(redeemed);
  successAmountText_ = MakeText(hstring{L""}, 22, colors::TextBrush());
  successAmountText_.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
  successAmountText_.HorizontalAlignment(HorizontalAlignment::Center);
  successPanel_.Children().Append(successAmountText_);
  content.Children().Append(successPanel_);

  dialog_.Content(content);

  dialog_.PrimaryButtonClick([weak = weak_from_this()](
                                 auto const&, ContentDialogButtonClickEventArgs const& args) {
    args.Cancel(true);  // keep the dialog open; Submit decides what shows next
    if (auto self = weak.lock()) self->Submit();
  });
}

void RedeemCodeSheet::Submit() {
  const std::string secret = TrimWhitespace(urnw::Narrow(codeBox_.Text().c_str()));
  if (redeeming_ || secret.size() != kBalanceCodeLength || !sdk_.apiReady()) return;
  redeeming_ = true;
  dialog_.IsPrimaryButtonEnabled(false);
  codeBox_.IsEnabled(false);

  urnet::RedeemBalanceCodeArgs args;
  args.secret = secret;
  auto queue = dialog_.DispatcherQueue();
  auto weak = weak_from_this();
  sdk_.api().redeemBalanceCode(
      args, [queue, weak](std::optional<urnet::RedeemBalanceCodeResult> result,
                          std::optional<std::string> err) {
        const bool ok = result && result->transfer_balance.has_value();
        const bool rejected = !ok && result && result->error;  // the server refused the code
        const std::string error =
            ok || rejected ? std::string() : (err ? *err : std::string());
        const int64_t bytes = ok ? result->transfer_balance->balance_byte_count : 0;
        queue.TryEnqueue([weak, ok, rejected, error, bytes] {
          if (auto self = weak.lock()) self->ApplyResult(ok, rejected, error, bytes);
        });
      });
}

void RedeemCodeSheet::ApplyResult(bool ok, bool rejected, std::string const& serverError,
                                  int64_t balanceByteCount) {
  redeeming_ = false;
  if (ok) {
    formPanel_.Visibility(Visibility::Collapsed);
    successPanel_.Visibility(Visibility::Visible);
    successAmountText_.Text(H("+" + FormatByteCountCompact(balanceByteCount)));
    dialog_.PrimaryButtonText(hstring{L""});  // nothing left to submit
    if (onRedeemed_) onRedeemed_();
    return;
  }
  codeBox_.IsEnabled(true);
  dialog_.IsPrimaryButtonEnabled(true);
  // a server error is not localizable; show it when there is one
  errorText_.Text(rejected            ? Loc("invalid_balance_code")
                  : serverError.empty() ? Loc("something_went_wrong")
                                        : H(serverError));
  errorText_.Visibility(Visibility::Visible);
}

// ---- UpgradeSheet -----------------------------------------------------------

std::shared_ptr<UpgradeSheet> UpgradeSheet::Create(XamlRoot const& root, SdkHost& sdk,
                                                   SubscriptionBalanceStore& balance) {
  auto sheet = std::shared_ptr<UpgradeSheet>(new UpgradeSheet(sdk, balance));
  sheet->Build(root);
  return sheet;
}

Border UpgradeSheet::BuildProductCard(bool yearly) {
  Border card;
  card.CornerRadius(CornerRadius{8, 8, 8, 8});
  card.BorderThickness(Thickness{2, 2, 2, 2});
  card.Padding(Thickness{16, 16, 16, 16});
  card.Background(colors::CardBrush());

  Grid row;
  ColumnDefinition c0, c1, c2;
  c0.Width(GridLength{0, GridUnitType::Auto});
  c1.Width(GridLength{1, GridUnitType::Star});
  c2.Width(GridLength{0, GridUnitType::Auto});
  row.ColumnDefinitions().Append(c0);
  row.ColumnDefinitions().Append(c1);
  row.ColumnDefinitions().Append(c2);
  row.ColumnSpacing(14);

  // selection dot
  ShapeEllipse dot;
  dot.Width(14);
  dot.Height(14);
  dot.StrokeThickness(2);
  dot.VerticalAlignment(VerticalAlignment::Center);
  row.Children().Append(dot);

  StackPanel labels;
  labels.Spacing(2);
  labels.VerticalAlignment(VerticalAlignment::Center);
  auto title = MakeText(yearly ? Loc("yearly") : Loc("monthly"), 18, colors::TextBrush());
  title.FontWeight(winrt::Windows::UI::Text::FontWeights::Bold());
  labels.Children().Append(title);
  if (yearly) {
    labels.Children().Append(
        MakeText(Loc("includes_2_week_free_trial"), 13, colors::MutedBrush()));
  }
  Grid::SetColumn(labels, 1);
  row.Children().Append(labels);

  if (yearly) {
    // "Most Popular" capsule (macOS ProductOptionCard badge)
    Border chip;
    chip.CornerRadius(CornerRadius{10, 10, 10, 10});
    chip.Padding(Thickness{10, 4, 10, 4});
    chip.VerticalAlignment(VerticalAlignment::Center);
    chip.Background(colors::MakeBrush(colors::kUrGreen));
    auto chipText = MakeText(Loc("most_popular"), 11,
                             colors::MakeBrush(colors::kInverseText));
    chipText.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
    chip.Child(chipText);
    Grid::SetColumn(chip, 2);
    row.Children().Append(chip);
  }

  card.Child(row);

  if (yearly) {
    yearlyDot_ = dot;
  } else {
    monthlyDot_ = dot;
  }
  card.Tapped([weak = weak_from_this(), yearly](auto const&, auto const&) {
    if (auto self = weak.lock()) {
      self->yearlySelected_ = yearly;
      self->ApplySelection();
    }
  });
  return card;
}

void UpgradeSheet::ApplySelection() {
  auto apply = [](Border const& card, ShapeEllipse const& dot, bool selected) {
    if (!card || !dot) return;
    card.BorderBrush(selected ? colors::AccentBrush() : colors::FaintBrush());
    dot.Stroke(selected ? colors::AccentBrush() : colors::MutedBrush());
    dot.Fill(selected ? colors::AccentBrush() : colors::MakeBrush(kTransparent));
  };
  apply(yearlyCard_, yearlyDot_, yearlySelected_);
  apply(monthlyCard_, monthlyDot_, !yearlySelected_);
}

void UpgradeSheet::Build(XamlRoot const& root) {
  dialog_ = MakeDialog(root, hstring{L""});

  Grid content;
  content.MinWidth(440);

  // ---- products page (macOS UpgradeSubscriptionSheet) ----
  productsPanel_ = StackPanel();
  productsPanel_.Spacing(12);

  productsPanel_.Children().Append(MakeText(Loc("become_a"), 20, colors::TextBrush()));
  auto supporterTitle = MakeText(Loc("urnetwork_supporter"), 28, colors::TextBrush(), true);
  supporterTitle.FontWeight(winrt::Windows::UI::Text::FontWeights::Bold());
  supporterTitle.Margin(Thickness{0, -8, 0, 0});
  productsPanel_.Children().Append(supporterTitle);
  productsPanel_.Children().Append(
      MakeText(Loc("support_us"), 14, colors::MutedBrush(), true));
  productsPanel_.Children().Append(
      MakeText(Loc("unlock_speed"), 14, colors::MutedBrush(), true));

  yearlyCard_ = BuildProductCard(/*yearly=*/true);
  yearlyCard_.Margin(Thickness{0, 8, 0, 0});
  productsPanel_.Children().Append(yearlyCard_);
  monthlyCard_ = BuildProductCard(/*yearly=*/false);
  productsPanel_.Children().Append(monthlyCard_);
  ApplySelection();

  // the app has no price api: the authoritative price appears on the Stripe page
  productsPanel_.Children().Append(
      MakeText(Loc("pricing_shown_at_checkout"), 12, colors::FaintBrush(), true));

  checkoutErrorText_ = MakeText(hstring{L""}, 12, colors::DangerBrush(), true);
  checkoutErrorText_.Visibility(Visibility::Collapsed);
  productsPanel_.Children().Append(checkoutErrorText_);

  // subscribe row: the accent button with a small in-flight ring
  Grid subscribeRow;
  subscribeButton_ = Button();
  subscribeButton_.Content(winrt::box_value(Loc("join_the_movement")));
  subscribeButton_.HorizontalAlignment(HorizontalAlignment::Stretch);
  if (auto style = AccentButtonStyle()) subscribeButton_.Style(*style);
  subscribeButton_.Click([weak = weak_from_this()](auto const&, auto const&) {
    if (auto self = weak.lock()) self->BeginCheckout();
  });
  subscribeRow.Children().Append(subscribeButton_);
  subscribeRing_ = ProgressRing();
  subscribeRing_.Width(16);
  subscribeRing_.Height(16);
  subscribeRing_.IsActive(false);
  subscribeRing_.HorizontalAlignment(HorizontalAlignment::Right);
  subscribeRing_.VerticalAlignment(VerticalAlignment::Center);
  subscribeRing_.Margin(Thickness{0, 0, 12, 0});
  subscribeRow.Children().Append(subscribeRing_);
  subscribeRow.Margin(Thickness{0, 4, 0, 0});
  productsPanel_.Children().Append(subscribeRow);

  auto terms = TextBlock();
  terms.Foreground(colors::MutedBrush());
  SetMarkdownLinkText(
      terms, Localized("by_subscribing_you_agree_to_urnetwork_s_terms"), 12);
  productsPanel_.Children().Append(terms);

  content.Children().Append(productsPanel_);

  // ---- embedded checkout page (the webview swaps in over the products) ----
  // Brand chrome around the ur.io/checkout page: the dialog background is
  // already the #101010 sheet surface the page itself uses.
  checkoutPanel_ = StackPanel();
  checkoutPanel_.Spacing(8);
  checkoutPanel_.Visibility(Visibility::Collapsed);
  Grid checkoutHeader;
  // the product name is never translated (the store marks it so; linux parity)
  auto checkoutTitle = MakeText(hstring{L"UR Pro"}, 18, colors::TextBrush());
  checkoutTitle.FontWeight(winrt::Windows::UI::Text::FontWeights::Bold());
  checkoutTitle.VerticalAlignment(VerticalAlignment::Center);
  checkoutHeader.Children().Append(checkoutTitle);
  Button checkoutClose;
  winrt::Microsoft::UI::Xaml::Controls::FontIcon closeGlyph;
  closeGlyph.Glyph(hstring{L"\uE711"});  // Segoe Fluent/MDL2 "Cancel" (X)
  closeGlyph.FontSize(12);
  checkoutClose.Content(closeGlyph);
  checkoutClose.HorizontalAlignment(HorizontalAlignment::Right);
  checkoutClose.VerticalAlignment(VerticalAlignment::Center);
  ToolTipService::SetToolTip(checkoutClose, winrt::box_value(Loc("close")));
  winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(
      checkoutClose, Loc("close"));
  checkoutClose.Click([weak = weak_from_this()](auto const&, auto const&) {
    // back to the products; the abandoned embedded session just expires
    if (auto self = weak.lock()) self->ShowPage(Page::Products);
  });
  checkoutHeader.Children().Append(checkoutClose);
  checkoutPanel_.Children().Append(checkoutHeader);
  // the webview (inserted per attempt) sits under a loading ring in this slot
  webviewSlot_ = Grid();
  webviewSlot_.MinWidth(440);
  webviewSlot_.Height(480);
  webviewSlot_.Background(colors::BackgroundBrush());
  checkoutRing_ = ProgressRing();
  checkoutRing_.Width(36);
  checkoutRing_.Height(36);
  checkoutRing_.IsActive(false);
  checkoutRing_.HorizontalAlignment(HorizontalAlignment::Center);
  checkoutRing_.VerticalAlignment(VerticalAlignment::Center);
  webviewSlot_.Children().Append(checkoutRing_);
  checkoutPanel_.Children().Append(webviewSlot_);
  content.Children().Append(checkoutPanel_);

  // ---- waiting page (paying in the browser / webhook confirmation) ----
  waitingPanel_ = StackPanel();
  waitingPanel_.Spacing(12);
  waitingPanel_.Visibility(Visibility::Collapsed);
  waitingPanel_.Margin(Thickness{0, 24, 0, 24});
  ProgressRing waitRing;
  waitRing.Width(36);
  waitRing.Height(36);
  waitRing.IsActive(true);
  waitRing.HorizontalAlignment(HorizontalAlignment::Center);
  waitingPanel_.Children().Append(waitRing);
  auto waitTitle = MakeText(Loc("waiting_for_approval"), 18, colors::TextBrush());
  waitTitle.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
  waitTitle.HorizontalAlignment(HorizontalAlignment::Center);
  waitingPanel_.Children().Append(waitTitle);
  // the body names where the payment is finishing: the browser (hosted) or
  // the just-closed embedded form ("Processing payment")
  waitingBodyText_ =
      MakeText(Loc("checkout_opened_in_browser"), 14, colors::MutedBrush(), true);
  waitingBodyText_.HorizontalAlignment(HorizontalAlignment::Center);
  waitingBodyText_.TextAlignment(TextAlignment::Center);
  waitingPanel_.Children().Append(waitingBodyText_);
  content.Children().Append(waitingPanel_);

  // ---- success page (macOS PurchaseSuccessView) ----
  successPanel_ = StackPanel();
  successPanel_.Visibility(Visibility::Collapsed);
  Border successCard;
  successCard.CornerRadius(CornerRadius{12, 12, 12, 12});
  successCard.Padding(Thickness{24, 24, 24, 24});
  successCard.Background(colors::AccentBrush());
  StackPanel successBody;
  successBody.Spacing(8);
  auto premium = MakeText(Loc("you_re_premium"), 22, colors::MakeBrush(colors::kInverseText));
  premium.FontWeight(winrt::Windows::UI::Text::FontWeights::Bold());
  successBody.Children().Append(premium);
  successBody.Children().Append(
      MakeText(Loc("thanks_for_building_the_new_internet_with_us"), 16,
               colors::MakeBrush(colors::kInverseText), true));
  successCard.Child(successBody);
  successPanel_.Children().Append(successCard);
  content.Children().Append(successPanel_);

  // ---- timeout page (the confirmation poll gave up) ----
  timeoutPanel_ = StackPanel();
  timeoutPanel_.Spacing(12);
  timeoutPanel_.Visibility(Visibility::Collapsed);
  timeoutPanel_.Margin(Thickness{0, 24, 0, 24});
  auto clock = MakeText(hstring{L"\uE823"}, 32, colors::MutedBrush());  // Segoe clock glyph
  clock.FontFamily(FontFamily(L"Segoe MDL2 Assets"));
  clock.HorizontalAlignment(HorizontalAlignment::Center);
  timeoutPanel_.Children().Append(clock);
  auto timeoutBody =
      MakeText(Loc("purchase_confirmation_timeout"), 14, colors::MutedBrush(), true);
  timeoutBody.TextAlignment(TextAlignment::Center);
  timeoutPanel_.Children().Append(timeoutBody);
  content.Children().Append(timeoutPanel_);

  ScrollViewer scroll;
  scroll.Content(content);
  scroll.MaxHeight(560);
  dialog_.Content(scroll);

  dialog_.Closed([weak = weak_from_this()](auto const&, auto const&) {
    // dismissed mid-checkout (Close button / Esc): drop the webview so a stale
    // payment page never lingers, and mark the sheet dead so an in-flight
    // session request cannot open a browser or a webview for it
    if (auto self = weak.lock()) {
      self->closed_ = true;
      self->TeardownWebView();
    }
  });
}

void UpgradeSheet::ShowPage(Page page) {
  // leaving the embedded checkout releases its webview; a closed WebView2
  // cannot be revived, so the next attempt builds a fresh one
  if (page != Page::Checkout) TeardownWebView();
  page_ = page;
  productsPanel_.Visibility(page == Page::Products ? Visibility::Visible
                                                   : Visibility::Collapsed);
  checkoutPanel_.Visibility(page == Page::Checkout ? Visibility::Visible
                                                   : Visibility::Collapsed);
  waitingPanel_.Visibility(page == Page::Waiting ? Visibility::Visible
                                                 : Visibility::Collapsed);
  successPanel_.Visibility(page == Page::Success ? Visibility::Visible
                                                 : Visibility::Collapsed);
  timeoutPanel_.Visibility(page == Page::TimedOut ? Visibility::Visible
                                                  : Visibility::Collapsed);
}

void UpgradeSheet::ShowCheckoutError(hstring const& message) {
  checkingOut_ = false;
  subscribeRing_.IsActive(false);
  subscribeButton_.IsEnabled(true);
  checkoutErrorText_.Text(message);
  checkoutErrorText_.Visibility(Visibility::Visible);
}

void UpgradeSheet::BeginCheckout() {
  if (checkingOut_ || !sdk_.apiReady()) return;
  checkingOut_ = true;
  hostedFallbackTried_ = false;
  subscribeButton_.IsEnabled(false);
  subscribeRing_.IsActive(true);
  checkoutErrorText_.Visibility(Visibility::Collapsed);

  // Embedded first (the ur.io/checkout bridge in a WebView2, so the card form
  // never leaves the sheet) when the Evergreen WebView2 runtime is present;
  // otherwise the hosted session in the system browser. The runtime is an
  // OPTIONAL dependency: every miss falls back to hosted, never to an error.
  RequestSession(/*embedded=*/WebView2RuntimeAvailable());
}

void UpgradeSheet::RequestSession(bool embedded) {
  if (!sdk_.apiReady()) {
    ShowCheckoutError(Loc("something_went_wrong"));
    return;
  }
  urnet::StripeCreateCheckoutSessionArgs args;
  args.item_id = yearlySelected_ ? "pro_yearly" : "pro_monthly";
  args.ui_mode = embedded ? "embedded" : "hosted";

  auto queue = dialog_.DispatcherQueue();
  auto weak = weak_from_this();
  sdk_.api().createStripeCheckoutSession(
      args, [queue, weak, embedded](
                std::optional<urnet::StripeCreateCheckoutSessionResult> result,
                std::optional<std::string> err) {
        // the api callback runs on an sdk thread; decide on the ui thread
        const std::string serverError =
            result && result->error ? result->error->message : std::string();
        const std::string url =
            result && result->checkout_url ? *result->checkout_url : std::string();
        const std::string clientSecret =
            result && result->client_secret ? *result->client_secret : std::string();
        const std::string transportError = err ? *err : std::string();
        queue.TryEnqueue([weak, embedded, serverError, url, clientSecret,
                          transportError] {
          auto self = weak.lock();
          if (!self || self->closed_) return;
          if (embedded) {
            // Any embedded failure — transport, server error, or a session
            // without a client_secret — retries once as hosted: nothing has
            // been shown yet, so no payment can be lost by switching. The
            // hosted retry is a SEPARATE session (never created up front);
            // the embedded one just expires server-side.
            if (serverError.empty() && transportError.empty() && !clientSecret.empty()) {
              self->OpenEmbedded(clientSecret);
            } else {
              self->RequestSession(/*embedded=*/false);
            }
            return;
          }
          if (!serverError.empty()) {
            self->ShowCheckoutError(H(serverError));
            return;
          }
          if (url.empty()) {
            self->ShowCheckoutError(transportError.empty() ? Loc("something_went_wrong")
                                                           : H(transportError));
            return;
          }
          try {
            winrt::Windows::System::Launcher::LaunchUriAsync(Uri(H(url)));
          } catch (...) {
            self->ShowCheckoutError(Loc("something_went_wrong"));
            return;
          }
          self->checkingOut_ = false;
          self->subscribeRing_.IsActive(false);
          self->subscribeButton_.IsEnabled(true);
          // bridge the webhook gap: poll until the server confirms Pro
          self->waitingBodyText_.Text(Loc("checkout_opened_in_browser"));
          self->balance_.StartConfirmationPolling();
          self->ShowPage(Page::Waiting);
        });
      });
}

winrt::fire_and_forget UpgradeSheet::OpenEmbedded(std::string clientSecret) {
  namespace wv2 = winrt::Microsoft::Web::WebView2::Core;
  auto weak = weak_from_this();
  auto queue = dialog_.DispatcherQueue();

  // the join affordance resets now: if the user X-closes the checkout, the
  // products page comes back ready for another try
  checkingOut_ = false;
  subscribeRing_.IsActive(false);
  subscribeButton_.IsEnabled(true);

  // fresh control per attempt (a closed WebView2 cannot be revived); the old
  // one, if any, is torn down first
  TeardownWebView();
  const uint32_t generation = ++webviewGeneration_;
  checkoutPageLoaded_ = false;
  webview_ = winrt::Microsoft::UI::Xaml::Controls::WebView2();
  // brand surface while the (dark) page loads — never a white flash
  webview_.DefaultBackgroundColor(colors::kBackground);

  webview_.NavigationStarting([weak, queue](auto const&, auto const& args) {
    const std::string uri = urnw::Narrow(std::wstring_view{args.Uri()});
    if (!uri.starts_with(kCheckoutScheme)) return;
    // the checkout page handing control back — never a real navigation
    args.Cancel(true);
    // deferred: handling flips pages and tears this webview down, which must
    // not happen from inside its own event
    queue.TryEnqueue([weak, uri] {
      if (auto self = weak.lock()) self->HandleCheckoutCallback(uri);
    });
  });
  webview_.NavigationCompleted([weak, queue, generation](auto const&, auto const& args) {
    const bool ok = args.IsSuccess();
    queue.TryEnqueue([weak, generation, ok] {
      auto self = weak.lock();
      if (!self || generation != self->webviewGeneration_) return;
      if (ok) {
        self->checkoutPageLoaded_ = true;
        self->checkoutRing_.IsActive(false);
      } else if (!self->checkoutPageLoaded_) {
        // the page never rendered (offline, dns, tls): nothing was shown, so
        // nothing was paid — the hosted flow can still save the purchase
        self->FallBackToHosted();
      }
    });
  });
  webview_.CoreProcessFailed([weak, queue, generation](auto const&, auto const&) {
    queue.TryEnqueue([weak, generation] {
      auto self = weak.lock();
      if (!self || generation != self->webviewGeneration_) return;
      if (self->page_ != Page::Checkout) return;
      // a dead browser process cannot take a payment: back to the products
      // with an inline error instead of a blank box
      self->ShowPage(Page::Products);
      self->ShowCheckoutError(Loc("something_went_wrong"));
    });
  });

  // under the loading ring (appended in Build, so it stays on top)
  webviewSlot_.Children().InsertAt(0, webview_);
  checkoutRing_.IsActive(true);
  ShowPage(Page::Checkout);

  const std::string url = std::string(kCheckoutPage) +
                          "?client_secret=" + Esc(clientSecret) +
                          "&redirect_link=" + Esc(kCheckoutRedirect);
  try {
    // Explicit user data folder: WebView2's default is next to the exe, which
    // an install under Program Files cannot write. StorageRoot is the app's
    // own per-user dir (%LOCALAPPDATA%\URnetwork\app).
    const std::wstring dataDir = (StorageRoot(/*isService=*/false) / "webview2").wstring();
    auto environment = co_await wv2::CoreWebView2Environment::CreateWithOptionsAsync(
        hstring{}, hstring{dataDir}, wv2::CoreWebView2EnvironmentOptions());
    auto self = weak.lock();
    if (!self || generation != self->webviewGeneration_ || !self->webview_) co_return;
    co_await self->webview_.EnsureCoreWebView2Async(environment);
    // C++/WinRT resumes on the awaiting (UI) apartment; `self` kept the sheet
    // alive across the await, the generation says whether it still wants us
    if (generation != self->webviewGeneration_ || !self->webview_) co_return;
    auto core = self->webview_.CoreWebView2();
    core.NewWindowRequested([](auto const&, auto const& args) {
      // target=_blank links (Stripe's terms/privacy): the system browser
      args.Handled(true);
      try {
        winrt::Windows::System::Launcher::LaunchUriAsync(Uri(args.Uri()));
      } catch (...) {
      }
    });
    core.Navigate(H(url));
  } catch (...) {
    // WebView2 init failed even though the runtime probe passed (disk, group
    // policy, a mid-flight runtime uninstall): the hosted flow saves the sale
    auto self = weak.lock();
    if (!self || generation != self->webviewGeneration_) co_return;
    self->FallBackToHosted();
  }
}

void UpgradeSheet::HandleCheckoutCallback(std::string const& uri) {
  const auto params = ParseQuery(uri);
  const auto status = params.find("status");
  if (status != params.end() && status->second == "complete") {
    // Paid inside the webview. The server only believes the Stripe payment
    // webhook — a client saying "I paid" is not evidence — so bridge the gap
    // with the same confirmation poll as hosted checkout.
    waitingBodyText_.Text(Loc("processing_payment"));
    balance_.StartConfirmationPolling();
    ShowPage(Page::Waiting);
    return;
  }
  if (page_ != Page::Checkout) return;  // stale error after close
  const auto message = params.find("errorMessage");
  ShowPage(Page::Products);
  ShowCheckoutError(message != params.end() && !message->second.empty()
                        ? H(message->second)
                        : Loc("something_went_wrong"));
}

void UpgradeSheet::FallBackToHosted() {
  // The embedded leg failed BEFORE Stripe's form rendered, so nothing can have
  // been paid — a fresh hosted session in the browser still saves the
  // purchase. Once per Join press: a second failure surfaces as an inline
  // error on the products page instead of looping.
  if (page_ != Page::Checkout) return;
  ShowPage(Page::Products);
  if (hostedFallbackTried_) {
    ShowCheckoutError(Loc("something_went_wrong"));
    return;
  }
  hostedFallbackTried_ = true;
  checkingOut_ = true;
  subscribeButton_.IsEnabled(false);
  subscribeRing_.IsActive(true);
  checkoutErrorText_.Visibility(Visibility::Collapsed);
  RequestSession(/*embedded=*/false);
}

void UpgradeSheet::TeardownWebView() {
  // invalidate every async continuation of the current attempt first: init,
  // navigation results, and the load-failure fallback all check the generation
  ++webviewGeneration_;
  checkoutPageLoaded_ = false;
  checkoutRing_.IsActive(false);
  if (!webview_) return;
  auto view = webview_;
  webview_ = nullptr;
  // Deferred: teardown is reachable from inside the webview's own event
  // handlers (a canceled urnetwork:// navigation flips the page), and closing
  // the control during its own event emission is the same reentrancy the
  // linux sheet defers around.
  auto slot = webviewSlot_;
  dialog_.DispatcherQueue().TryEnqueue([slot, view] {
    uint32_t index = 0;
    if (slot.Children().IndexOf(view, index)) slot.Children().RemoveAt(index);
    try {
      view.Close();
    } catch (...) {
    }
  });
}

void UpgradeSheet::OnBalance(BalanceSnapshot const& snapshot, BalancePollState const& poll) {
  if (page_ != Page::Waiting) return;
  if (snapshot.isPro) {
    ShowPage(Page::Success);
  } else if (poll.timedOut) {
    ShowPage(Page::TimedOut);
  }
}

}  // namespace urnw
