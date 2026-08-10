// SPDX-License-Identifier: MPL-2.0
#include "pch.h"

#include "IdenticonImage.h"

// not in the pch: the imaging + stream types this file needs to turn png bytes
// into a BitmapImage (ProviderLocationsSheet.cpp adds its extra winrt headers
// the same way)
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.Shapes.h>

#include <string>
#include <utility>

#include "Sdk.h"

using namespace winrt;
using namespace winrt::Windows::Storage::Streams;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Media;
using namespace winrt::Microsoft::UI::Xaml::Media::Imaging;
using namespace winrt::Microsoft::UI::Xaml::Shapes;

namespace urnw {
namespace {

// Fill a BitmapImage from png bytes. Fire-and-forget: the BitmapImage is
// already assigned to its brush by the caller, so it renders when the async
// decode completes; if the row was rebuilt in the meantime the result is simply
// dropped. Called on the UI thread (Render), and the WinRT awaits resume on the
// same STA, so the UI-affine BitmapImage is only touched there.
winrt::fire_and_forget FillBitmapAsync(BitmapImage bitmap, std::vector<uint8_t> png) {
  // Called on the UI (STA) thread; capture it so we can return here. C++/WinRT
  // co_await resumes on a background thread by default, and BitmapImage is
  // UI-thread-affine, so the stream work can run anywhere but SetSourceAsync
  // must be issued back on the UI thread.
  winrt::apartment_context ui;
  InMemoryRandomAccessStream stream;
  DataWriter writer{stream};
  writer.WriteBytes(winrt::array_view<uint8_t const>(png.data(), png.data() + png.size()));
  co_await writer.StoreAsync();
  writer.DetachStream();
  stream.Seek(0);
  co_await ui;
  co_await bitmap.SetSourceAsync(stream);
}

}  // namespace

BitmapImage IdenticonCache::Get(const std::vector<uint8_t>& key, const std::string& hash,
                                int displaySize) {
  const std::string cacheKey = hash + ":" + std::to_string(displaySize);
  if (auto it = cache_.find(cacheKey); it != cache_.end()) return it->second;
  BitmapImage bitmap;
  // the canonical 2x raster (crisp on high-dpi; the WinUI downscale into the
  // display-sized element is the goidenticons resample step)
  std::vector<uint8_t> png =
      urnet::renderIdenticonPng(key, static_cast<int32_t>(displaySize * 2));
  FillBitmapAsync(bitmap, std::move(png));
  cache_.emplace(cacheKey, bitmap);
  return bitmap;
}

UIElement MakeIdenticonBadge(IdenticonCache& cache, const ProviderIdentityRow& identity,
                             int displaySize) {
  Rectangle rect;
  rect.Width(displaySize);
  rect.Height(displaySize);
  // fixed size, centered in its cell -- a Shape otherwise stretches to the
  // (four-line-tall) row height
  rect.VerticalAlignment(VerticalAlignment::Center);
  rect.HorizontalAlignment(HorizontalAlignment::Left);
  // the standard identicon rounding, radius = size / 6, everywhere
  rect.RadiusX(displaySize / 6.0);
  rect.RadiusY(displaySize / 6.0);
  ImageBrush brush;
  brush.ImageSource(cache.Get(identity.key, identity.hash, displaySize));
  brush.Stretch(Stretch::Fill);
  rect.Fill(brush);
  return rect;
}

}  // namespace urnw
