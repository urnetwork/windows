// The connected providers and where they are — a port of the android
// ProviderLocationsScreen (ui/connect/providerlocations/), opened from the
// drawer's "Connected to N providers" label. A ContentDialog modeled on
// SplitRulesSheet: a fixed globe above an independently scrolling list of rows,
// built imperatively.
//
// Row: a fixed-size country-color dot on the left (top aligned, gaining a
// selection ring at a 4px gap without changing the column width), then the
// client id in monospace (tap to copy), "City, Region, Country", "lat, lon",
// and the connected duration, plus an inline remove button. Selection is one
// value shared with the globe: clicking a row centers the globe on it, and
// stepping the globe's wheel selects the row.
//
// NOT ported from android: the mock-location toggle and its setup guide.
// Windows has no third-party geolocation injection point —
// GeolocationProvider.SetOverridePosition is a Limited Access Feature needing a
// Microsoft-issued, package-family-bound unlock token, and the writable
// IDefaultLocation is fallback-only. See sdk/PROVIDERLOCATIONS.md "Windows".
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>

#include "IdenticonImage.h"
#include "PostQuantumIdentity.h"
#include "ProviderGlobe.h"
#include "ProviderLocations.h"
#include "SdkHost.h"

namespace urnw {

class ProviderLocationsSheet : public std::enable_shared_from_this<ProviderLocationsSheet> {
 public:
  static std::shared_ptr<ProviderLocationsSheet> Create(
      winrt::Microsoft::UI::Xaml::XamlRoot const& root, SdkHost& sdk);

  winrt::Microsoft::UI::Xaml::Controls::ContentDialog Dialog() const { return dialog_; }

  // Rebuild from the latest rows. `remoteConnected` is SdkHost::RemoteConnected:
  // the window state lives in the service's device, so while the rpc is down an
  // empty list would be a stale claim presented as fact -- the sheet shows the
  // gray discovery-disabled line instead of "no providers" (apple/drawer
  // parity). Cheap enough to run on every SDK push.
  void Update(std::vector<ProviderLocationRow> rows, bool remoteConnected);

  // The providers with a verified e2e session (SdkHost::CurrentProviderIdentities
  // / the identity change feed). Joined by egress client id onto the rows to
  // badge the encrypted providers. Independent of the locations feed -- a
  // session verifying does not change a location row -- so it re-renders on its
  // own push.
  void UpdateIdentities(std::vector<ProviderIdentityRow> identities);

  // Called from the window's shared ~10 fps drawer clock: advances the globe's
  // recenter animation and reticks the connected-duration labels at 1s.
  void Tick();

  // Re-read the selection from the SDK view controller (which owns it and
  // drops one whose provider left the window) and re-render. Called on the UI
  // thread from SdkHost's signal-only selection handler -- a wheel step lands
  // through here.
  void RefreshSelection();

 private:
  explicit ProviderLocationsSheet(SdkHost& sdk) : sdk_(sdk) {}

  void Build(winrt::Microsoft::UI::Xaml::XamlRoot const& root);
  void Render();
  void RefreshDurations();
  void Select(std::string clientId);
  void Remove(const std::string& clientId);
  void CopyClientId(const std::string& clientId);
  winrt::Microsoft::UI::Xaml::Controls::Grid MakeProviderRow(const ProviderLocationRow& row);

  SdkHost& sdk_;
  winrt::Microsoft::UI::Xaml::Controls::ContentDialog dialog_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::StackPanel list_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock status_{nullptr};    // empty / rpc down
  winrt::Microsoft::UI::Xaml::Controls::TextBlock copiedNote_{nullptr};
  std::unique_ptr<ProviderGlobe> globe_;

  std::vector<ProviderLocationRow> rows_;
  // the verified-e2e identity set, keyed by egress client id (the row join
  // key). Membership drives the badge; the entry supplies the identicon key.
  // Value-compared in UpdateIdentities so an identity change re-renders even
  // when the location rows are unchanged.
  std::vector<ProviderIdentityRow> identities_;
  std::map<std::string, const ProviderIdentityRow*> identityByClientId_;
  IdenticonCache identiconCache_;
  std::string selectedClientId_;
  // the selection the list has already been scrolled to, so a re-render for
  // some other reason (a duration tick, an identity badge) does not scroll
  std::string scrolledToClientId_;
  // rows the user removed, trimmed locally so the list does not appear to snap
  // back during the SDK round trip; cleared as the SDK confirms them gone
  std::set<std::string> removing_;
  bool remoteConnected_ = true;
  // connected-since stamps paired with the label that renders them, so the 1s
  // retick does not rebuild the list
  std::vector<std::pair<int64_t, winrt::Microsoft::UI::Xaml::Controls::TextBlock>> durationLabels_;
  uint32_t tickCount_ = 0;
};

}  // namespace urnw
