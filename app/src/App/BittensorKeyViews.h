// The one-tap Bittensor key's parts of the Earnings page (apple/BTEASY.md, the
// approved mocks' flows 1, 3 and 4): the payout card's key bodies, the backup
// banner, the funds disclaimer as every screen shows it, and the claim relay's
// rows under the settlement v2 tile. Each draws what the sdk's flow card
// (urnet::BittensorLocalKeyCard) or claim status (urnet::SnClaimStatusResult)
// says, in the words BittensorKeyPresentation.h picks; none decides anything.
//
// Plain functions over code-built XAML, like the rest of the Earnings page:
// WalletPage puts what they build into the named hosts of MainWindow.xaml.
// Every user-facing string is a store key (Loc / Format). UI thread only.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <functional>
#include <string>
#include <string_view>

#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>

#include "BittensorKeyPresentation.h"
#include "Sdk.h"

namespace urnw::btkeyviews {

// What the card's controls do: the page's handlers, which go to the sdk's flow
// (or, for Use a wallet and Change wallet, to the wallet consent sheet).
struct CardActions {
  std::function<void()> setUp;          // Set up payouts (SetUpPayouts, the warning first when it replaces)
  std::function<void()> saveFirst;      // Save the recovery phrase first (SaveFirst)
  std::function<void()> useWallet;      // Use a wallet I already have: the consent sheet, unchanged
  std::function<void()> finishSetup;    // Finish setup (FinishSetup)
  std::function<void()> openKeyScreen;  // Back up your key, View key (OpenScreen)
  std::function<void()> reload;         // Retry, after a store that couldn't answer (Reload)
  std::function<void()> changeWallet;   // Change wallet: the consent sheet with the key offer
};

// Draws the key's part of the payout card for `layout` into `host`, which it
// clears first: a key body (the mocks' 1a, 1b, 1c, 1i, 1l, 2h, 3a, 3d, 3i), or
// the lines under the wallet card's own (another consent in force, or a store
// that can't answer). Nothing for the wallet card alone. The funds disclaimer
// is not drawn here: it closes the card whatever its body (MakeFundsDisclaimer).
void DrawCard(winrt::Microsoft::UI::Xaml::Controls::StackPanel const& host, btkey::CardLayout const& layout,
              urnet::BittensorLocalKeyCard const& card, CardActions const& actions);

// The backup banner that leads Earnings while the key's phrase isn't known to
// be saved (the sdk's BackupReminder): Back up your key, and why. Opens the Key
// screen.
winrt::Microsoft::UI::Xaml::FrameworkElement MakeBackupBanner(std::function<void()> open);

// The owner's funds disclaimer as a screen shows it, for the sdk's id
// (urnet::BittensorFundsDisclaimer*): the whole text, its last sentence, or
// nothing (nullptr) for none. `boxed` in its own tinted box, at the foot of a
// screen of its own.
winrt::Microsoft::UI::Xaml::FrameworkElement MakeFundsDisclaimer(std::string_view disclaimer, bool boxed);

// Fills `host` (cleared first) with the funds disclaimer, and collapses it for none.
void ShowFundsDisclaimer(winrt::Microsoft::UI::Xaml::Controls::StackPanel const& host,
                         std::string_view disclaimer, bool boxed);

// The claim relay's rows under the settlement v2 tile's balances, and in the
// claim sheet (`inSheet`, which has its own Claim now): who claims, the
// pending pill and why it waits, and on the tile Claim now with its note, or
// the device's own claim while URnetwork isn't claiming. Clears `host` first.
void DrawRelaySection(winrt::Microsoft::UI::Xaml::Controls::StackPanel const& host,
                      urnet::SnClaimStatusResult const& status, bool inSheet,
                      std::function<void()> claimNow, std::function<void()> selfClaim);

// The line under Claim now (btkey::ClaimNowNoteFor), "" for none.
winrt::hstring ClaimNowNoteText(btkey::ClaimNowNote note, int64_t periodDays, int64_t floorTaoRao,
                                int64_t nextAtMillis);

// A failure the flow reports, in the store's words (btkey::FailureTextFor), ""
// for none.
winrt::hstring FailureText(std::string const& code, std::string const& message);

}  // namespace urnw::btkeyviews
