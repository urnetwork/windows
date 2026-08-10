// Identicon rasterization for WinUI: the canonical SDK png (rendered at 2x the
// display size) decoded into a BitmapImage, cached per (key hash, display
// size), plus a rounded badge element for the provider-locations row. The
// WinUI counterpart of the linux IdenticonCache / apple IdenticonView.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <map>
#include <string>
#include <vector>

#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>

#include "PostQuantumIdentity.h"

namespace urnw {

// Identicon raster cache, keyed by (key hash, display size) -- the raster
// derives from the key, which the hash captures. Renders through the canonical
// SDK png at 2x the display size.
class IdenticonCache {
 public:
  // The BitmapImage for a key at a display size, cached per (hash, size). The
  // BitmapImage is created immediately and filled asynchronously
  // (SetSourceAsync), so a caller assigns it to an ImageBrush/Image now and it
  // appears when decoding completes -- Render() stays synchronous.
  winrt::Microsoft::UI::Xaml::Media::Imaging::BitmapImage Get(
      const std::vector<uint8_t>& key, const std::string& hash, int displaySize);
  void Clear() { cache_.clear(); }

 private:
  std::map<std::string, winrt::Microsoft::UI::Xaml::Media::Imaging::BitmapImage> cache_;
};

// A rounded (radius = size / 6, the standard identicon rounding everywhere) size
// x size element rendering the identity's identicon, for the trailing badge next
// to a provider client id.
winrt::Microsoft::UI::Xaml::UIElement MakeIdenticonBadge(
    IdenticonCache& cache, const ProviderIdentityRow& identity, int displaySize);

}  // namespace urnw
