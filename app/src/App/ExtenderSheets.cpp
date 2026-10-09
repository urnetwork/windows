// SPDX-License-Identifier: MPL-2.0
#include "pch.h"

#include "ExtenderSheets.h"

#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include <winrt/Microsoft.UI.Xaml.Shapes.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Pickers.h>
#include <winrt/Windows.Storage.Streams.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include <ZXing/ImageView.h>
#include <ZXing/ReadBarcode.h>
#include <ZXing/ReaderOptions.h>
#include <ZXing/Result.h>
#include <qrcodegen.hpp>

#include "ConnectorGlyph.h"
#include "Localization.h"
#include "Log.h"
#include "SheetFit.h"  // sheetfit: sheets clamp to the window at open time
#include "SettingsSheets.h"  // the row kit: MakeSheet / Supporting / Lookup / clipboard
#include "Strings.h"
#include "UrColors.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Windows::Foundation;
using namespace urnw::rows;

namespace urnw {
namespace {

namespace shapes = winrt::Microsoft::UI::Xaml::Shapes;
namespace media = winrt::Microsoft::UI::Xaml::Media;
namespace automation = winrt::Microsoft::UI::Xaml::Automation;
namespace imaging = winrt::Windows::Graphics::Imaging;
namespace storage = winrt::Windows::Storage;

hstring H(std::string const& s) { return winrt::to_hstring(s); }
hstring Loc(std::string_view key) { return hstring{Localized(key)}; }

// The code is drawn into a fixed square. 296 rather than "whatever fits": the
// module size has to be a whole number of pixels (see ExtenderQrLayoutFor), the
// payload is 48 addresses at most so the version tops out around 37..45
// modules, and 296 divides those into 8 and 6 px modules -- big enough for a
// phone camera pointed at the screen, small enough for a dialog.
constexpr double kCodeSide = 296.0;
// The white margin around the code. Four modules is the spec's quiet zone; this
// is the padding of the white frame, in pixels, sized generously because the
// frame sits on a dark sheet and the contrast step is what a decoder looks for.
constexpr double kQuietZone = 16.0;

const winrt::Windows::UI::Color kCodeDark{255, 0x10, 0x10, 0x10};
const winrt::Windows::UI::Color kCodeLight{255, 0xFF, 0xFF, 0xFF};

// Path.Data's mini-language has no runtime parser a C++ caller can reach, so a
// one-element document goes through XamlReader -- ConnectCanvas and
// LoginCarousel reach the same parser the same way.
shapes::Path ConnectorPath() {
  const std::wstring markup =
      L"<Path xmlns='http://schemas.microsoft.com/winfx/2006/xaml/presentation' Data='" +
      std::wstring(glyph::kConnectorPath) + L"'/>";
  auto path = Markup::XamlReader::Load(winrt::hstring{markup}).as<shapes::Path>();
  path.Stretch(media::Stretch::Fill);
  path.IsHitTestVisible(false);
  return path;
}

TextBlock MakeText(hstring const& text, double size, media::Brush const& brush) {
  TextBlock block;
  block.Text(text);
  block.FontSize(size);
  block.TextWrapping(TextWrapping::Wrap);
  if (brush) block.Foreground(brush);
  return block;
}

// A label above a control, the shape every field in these sheets uses.
StackPanel MakeField(Panel const& host, hstring const& label) {
  StackPanel field;
  field.Spacing(4);
  field.Children().Append(MakeText(label, 12, colors::MutedBrush()));
  host.Children().Append(field);
  return field;
}

// Gray8 is what zxing wants and what a decoder can usually produce directly;
// when it cannot (some codecs refuse the conversion), fall back to BGRA and
// reduce here. Returns false only when the image could not be read at all.
bool ReadLuminance(imaging::BitmapDecoder const& decoder, std::vector<uint8_t>& out,
                   uint32_t& width, uint32_t& height) {
  width = decoder.PixelWidth();
  height = decoder.PixelHeight();
  if (width == 0 || height == 0) return false;
  imaging::BitmapTransform transform;
  try {
    auto provider = decoder
                        .GetPixelDataAsync(imaging::BitmapPixelFormat::Gray8,
                                           imaging::BitmapAlphaMode::Ignore, transform,
                                           imaging::ExifOrientationMode::IgnoreExifOrientation,
                                           imaging::ColorManagementMode::DoNotColorManage)
                        .get();
    const auto pixels = provider.DetachPixelData();
    out.assign(pixels.begin(), pixels.end());
    return out.size() >= static_cast<size_t>(width) * height;
  } catch (...) {
    // fall through to BGRA
  }
  try {
    auto provider = decoder
                        .GetPixelDataAsync(imaging::BitmapPixelFormat::Bgra8,
                                           imaging::BitmapAlphaMode::Ignore, transform,
                                           imaging::ExifOrientationMode::IgnoreExifOrientation,
                                           imaging::ColorManagementMode::DoNotColorManage)
                        .get();
    const auto pixels = provider.DetachPixelData();
    const size_t count = static_cast<size_t>(width) * height;
    if (pixels.size() < count * 4) return false;
    out.resize(count);
    for (size_t i = 0; i < count; ++i) {
      out[i] = ZXing::RGBToLum(pixels[i * 4 + 2], pixels[i * 4 + 1], pixels[i * 4 + 0]);
    }
    return true;
  } catch (const std::exception& e) {
    LogWarn("extender: image pixel read failed: {}", e.what());
  } catch (...) {
    LogWarn("extender: image pixel read failed");
  }
  return false;
}

// The one place this app decodes a barcode. QR only and tryHarder/tryRotate on:
// the input is a photo of a screen or a saved screenshot, not a scanner feed,
// so the extra work is paid once per file choice and is exactly what makes a
// slightly rotated phone photo readable.
std::string DecodeQr(std::vector<uint8_t> const& luminance, uint32_t width,
                     uint32_t height) {
  if (luminance.empty() || width == 0 || height == 0) return {};
  try {
    ZXing::ReaderOptions options;
    options.setFormats(ZXing::BarcodeFormat::QRCode);
    options.setTryHarder(true);
    options.setTryRotate(true);
    const ZXing::ImageView view(luminance.data(), static_cast<int>(width),
                                static_cast<int>(height), ZXing::ImageFormat::Lum);
    const auto result = ZXing::ReadBarcode(view, options);
    if (result.isValid()) return result.text();
  } catch (const std::exception& e) {
    LogWarn("extender: qr decode failed: {}", e.what());
  } catch (...) {
    LogWarn("extender: qr decode failed");
  }
  return {};
}

}  // namespace

// ---- ExtenderShareSheet -----------------------------------------------------

std::shared_ptr<ExtenderShareSheet> ExtenderShareSheet::Create(XamlRoot const& root,
                                                               SdkHost& sdk) {
  auto sheet = std::shared_ptr<ExtenderShareSheet>(new ExtenderShareSheet(sdk));
  sheet->Build(root);
  return sheet;
}

void ExtenderShareSheet::Build(XamlRoot const& root) {
  dialog_ = MakeSheet(root, Loc("share_extenders"));

  StackPanel content;
  content.MinWidth(360);
  content.Spacing(12);

  // The code sits on WHITE with a quiet zone, on a dark sheet. A QR drawn in
  // the sheet's own colours does not decode: the contrast polarity and the
  // quiet zone are part of the symbol, not decoration.
  canvas_ = Canvas();
  canvas_.Width(kCodeSide);
  canvas_.Height(kCodeSide);
  canvas_.IsHitTestVisible(false);
  codeFrame_ = Border();
  codeFrame_.Background(colors::MakeBrush(kCodeLight));
  codeFrame_.Padding(ThicknessHelper::FromUniformLength(kQuietZone));
  codeFrame_.CornerRadius(CornerRadiusHelper::FromUniformRadius(8));
  codeFrame_.HorizontalAlignment(HorizontalAlignment::Center);
  codeFrame_.Child(canvas_);
  content.Children().Append(codeFrame_);

  countText_ = MakeText({}, 12, colors::MutedBrush());
  countText_.HorizontalAlignment(HorizontalAlignment::Center);
  content.Children().Append(countText_);

  Supporting(content, Loc("share_extenders_hint"));

  // K7: off by default. A share that silently carried the operator settings
  // would be a way to move someone's trust anchor without their knowing.
  settingsToggle_ = ToggleSwitch();
  settingsToggle_.Header(winrt::box_value(Loc("include_extender_settings")));
  settingsToggle_.OnContent(winrt::box_value(Loc("on")));
  settingsToggle_.OffContent(winrt::box_value(Loc("off")));
  settingsToggle_.IsOn(false);
  settingsToggle_.Style(Lookup(L"UrSwitchToggleStyle"));
  settingsToggle_.Toggled([weak = weak_from_this()](auto const&, auto const&) {
    auto self = weak.lock();
    if (!self) return;
    const bool on = self->settingsToggle_.IsOn();
    if (on == self->includeSettings_) return;
    self->includeSettings_ = on;
    self->Rebuild();
  });
  content.Children().Append(settingsToggle_);

  // "the share screen also shows the payload as copyable text" (K7). Selectable
  // AND copyable: the text is the fallback for every machine that cannot point
  // a camera at this screen, which on a desktop is most of them.
  payloadText_ = MakeText({}, 11, colors::MutedBrush());
  payloadText_.FontFamily(media::FontFamily(L"Consolas"));
  payloadText_.IsTextSelectionEnabled(true);
  payloadText_.MaxHeight(96);
  content.Children().Append(payloadText_);

  copyButton_ = Button();
  copyButton_.Content(winrt::box_value(Loc("copy_share_text")));
  copyButton_.HorizontalAlignment(HorizontalAlignment::Left);
  copyButton_.IsEnabled(false);
  copyButton_.Click([weak = weak_from_this()](auto const&, auto const&) {
    auto self = weak.lock();
    if (!self || self->text_.empty()) return;
    CopyToClipboard(self->text_);
    self->statusText_.Text(Loc("share_text_copied"));
    self->statusText_.Foreground(colors::MutedBrush());
    self->statusText_.Visibility(Visibility::Visible);
  });
  content.Children().Append(copyButton_);

  statusText_ = MakeText({}, 12, colors::MutedBrush());
  statusText_.Visibility(Visibility::Collapsed);
  content.Children().Append(statusText_);

  ScrollViewer scroll;
  scroll.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
  scroll.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);
  scroll.MaxHeight(sheetfit::Height(root, 620));
  scroll.Content(content);
  dialog_.Content(scroll);

  Rebuild();
}

winrt::fire_and_forget ExtenderShareSheet::Rebuild() {
  auto weak = weak_from_this();
  auto queue = dialog_.DispatcherQueue();
  const bool includeSettings = includeSettings_;
  // A SHARED reference, taken before the suspension and held for the whole
  // call: `sdk_` is a member, so reading it after a suspension point means
  // reading `this`, which the dialog may have dropped by then -- and a raw
  // controller pointer could be released by the session teardown between the
  // check and the call.
  const auto controller = sdk_.ExtenderController();

  if (!controller) {
    // No session, so there is nothing to share and nothing to be coy about.
    ApplyFieldState(statusText_,
                    sdk_.IsLoggedIn() ? FieldState::NoDevice : FieldState::NoSession);
    statusText_.Visibility(Visibility::Visible);
    ApplyShare({}, 0, /*failed=*/false);
    co_return;
  }

  std::string text;
  int64_t count = 0;
  bool failed = false;

  co_await winrt::resume_background();
  try {
    // The controller is on the DeviceRemote, so this is an rpc.
    if (const auto result = controller->buildShare(includeSettings)) {
      text = result->Text;
      count = result->Count;
    } else {
      // A call that returned nothing is a failure, not an empty share: a blank
      // frame with no line under it is indistinguishable from a space that
      // genuinely knows no extenders yet, and those are different.
      failed = true;
    }
  } catch (const std::exception& e) {
    LogWarn("extender: build share failed: {}", e.what());
    failed = true;
  } catch (...) {
    LogWarn("extender: build share failed");
    failed = true;
  }

  // Back on the UI thread the way every other callback in these sheets does it;
  // there is no resume_foreground for Microsoft.UI.Dispatching.
  queue.TryEnqueue([weak, text, count, failed] {
    if (auto self = weak.lock()) self->ApplyShare(text, count, failed);
  });
}

void ExtenderShareSheet::ApplyShare(std::string const& text, int64_t count, bool failed) {
  text_ = text;
  payloadText_.Text(H(text));
  copyButton_.IsEnabled(!text.empty());
  countText_.Text(hstring{urnw::Plural("share_extenders_count", count)});
  const bool drew = RenderCode(text);
  // the frame is a white box with nothing in it when the code did not render,
  // so collapse it rather than present an empty one as if it were the code
  codeFrame_.Visibility(drew && !text.empty() ? Visibility::Visible
                                              : Visibility::Collapsed);
  if (failed) {
    ApplyFieldState(statusText_, FieldState::Failed);
    statusText_.Visibility(Visibility::Visible);
  } else if (!drew) {
    // the payload exists and is copyable; only the picture of it does not
    statusText_.Text(Loc("share_extenders_too_large"));
    statusText_.Foreground(colors::MutedBrush());
    statusText_.Visibility(Visibility::Visible);
  } else if (!text.empty()) {
    statusText_.Visibility(Visibility::Collapsed);
  }
}

bool ExtenderShareSheet::RenderCode(std::string const& text) {
  canvas_.Children().Clear();
  if (text.empty()) return true;

  int moduleCount = 0;
  std::vector<bool> dark;
  try {
    // K7: level H. It is also what makes the centred glyph safe -- the glyph
    // occludes a few percent of the modules and H recovers up to ~30%.
    const qrcodegen::QrCode code =
        qrcodegen::QrCode::encodeText(text.c_str(), qrcodegen::QrCode::Ecc::HIGH);
    moduleCount = code.getSize();
    dark.resize(static_cast<size_t>(moduleCount) * moduleCount, false);
    for (int y = 0; y < moduleCount; ++y) {
      for (int x = 0; x < moduleCount; ++x) {
        dark[static_cast<size_t>(y) * moduleCount + x] = code.getModule(x, y);
      }
    }
  } catch (const std::exception& e) {
    // qrcodegen::data_too_long is the real case: a share bigger than even a
    // version-40 level-H symbol. The payload is capped at 48 addresses so it
    // should not happen, but a blank frame with no explanation is the worst
    // possible way to find out that it did -- and the copyable text below is
    // still a complete answer, which is what the store's line says.
    LogWarn("extender: qr encode failed: {}", e.what());
    return false;
  } catch (...) {
    LogWarn("extender: qr encode failed");
    return false;
  }

  const ExtenderQrLayout layout = ExtenderQrLayoutFor(moduleCount, kCodeSide);
  if (layout.moduleCount <= 0) return false;
  canvas_.Width(layout.side);
  canvas_.Height(layout.side);

  auto darkBrush = colors::MakeBrush(kCodeDark);

  // One rectangle per horizontal RUN, not per module: a 37-module code is ~680
  // dark modules and ~200 runs, and the difference is a dialog that opens
  // rather than one that hitches while it does.
  for (const ExtenderQrRun& run : ExtenderQrRunsFor(moduleCount, dark, layout.clearFrom,
                                                    layout.clearTo)) {
    shapes::Rectangle cell;
    cell.Width(run.length * layout.moduleSize);
    cell.Height(layout.moduleSize);
    cell.Fill(darkBrush);
    Canvas::SetLeft(cell, run.x * layout.moduleSize);
    Canvas::SetTop(cell, run.y * layout.moduleSize);
    canvas_.Children().Append(cell);
  }

  // The connector mark at the centre (K7). Three layers on the blanked white
  // patch, outside in:
  //   * a 4 px stroke of the CONNECTOR SHAPE, so the outline is the mark's own
  //     silhouette rather than a circle or a box around it,
  //   * the white patch the runs already left, which is the gap,
  //   * the mark itself, filled.
  // The ring's outer edge lands on the cleared patch's edge, so nothing is
  // drawn over a live module.
  const double outline = layout.outlineThickness;
  if (0 < outline) {
    auto ring = ConnectorPath();
    const double ringSide = layout.glyphSide + outline;
    ring.Width(ringSide);
    ring.Height(ringSide);
    ring.Stroke(darkBrush);
    ring.StrokeThickness(outline);
    Canvas::SetLeft(ring, (layout.side - ringSide) / 2);
    Canvas::SetTop(ring, (layout.side - ringSide) / 2);
    canvas_.Children().Append(ring);
  }
  auto mark = ConnectorPath();
  const double markSide = (std::max)(layout.glyphSide - 2 * outline, layout.glyphSide / 2);
  mark.Width(markSide);
  mark.Height(markSide);
  mark.Fill(darkBrush);
  Canvas::SetLeft(mark, (layout.side - markSide) / 2);
  Canvas::SetTop(mark, (layout.side - markSide) / 2);
  canvas_.Children().Append(mark);

  automation::AutomationProperties::SetName(codeFrame_, Loc("share_extenders"));
  return true;
}

// ---- ExtenderImportSheet ----------------------------------------------------

std::shared_ptr<ExtenderImportSheet> ExtenderImportSheet::Create(
    XamlRoot const& root, HWND owner, SdkHost& sdk, std::function<void()> onImported) {
  auto sheet = std::shared_ptr<ExtenderImportSheet>(
      new ExtenderImportSheet(owner, sdk, std::move(onImported)));
  sheet->Build(root);
  return sheet;
}

void ExtenderImportSheet::Build(XamlRoot const& root) {
  dialog_ = MakeSheet(root, Loc("import_extenders"));

  StackPanel content;
  content.MinWidth(380);
  content.Spacing(12);

  // K8: windows imports from an image file or from pasted text. There is no
  // camera path, and the two affordances are offered together rather than one
  // behind the other, because on a desktop the pasted text is usually the
  // faster of the two.
  StackPanel actionRow;
  actionRow.Orientation(Orientation::Horizontal);
  actionRow.Spacing(8);
  chooseButton_ = Button();
  chooseButton_.Content(winrt::box_value(Loc("choose_image_file")));
  chooseButton_.Click([weak = weak_from_this()](auto const&, auto const&) {
    if (auto self = weak.lock()) self->ChooseImage();
  });
  actionRow.Children().Append(chooseButton_);
  ring_ = ProgressRing();
  ring_.Width(16);
  ring_.Height(16);
  ring_.IsActive(false);
  ring_.Visibility(Visibility::Collapsed);
  ring_.VerticalAlignment(VerticalAlignment::Center);
  actionRow.Children().Append(ring_);
  content.Children().Append(actionRow);

  auto pasteField = MakeField(content, Loc("paste_share_text"));
  pasteBox_ = TextBox();
  pasteBox_.AcceptsReturn(true);
  pasteBox_.TextWrapping(TextWrapping::Wrap);
  pasteBox_.Height(72);
  pasteBox_.Style(Lookup(L"UrTextInputStyle"));
  pasteBox_.TextChanged([weak = weak_from_this()](auto const&, auto const&) {
    auto self = weak.lock();
    if (!self) return;
    // Decoding is local and cheap (the SDK parses a base64url blob), so the
    // sheet answers as the text lands instead of hiding behind a Decode button
    // that would be the only thing standing between a paste and an answer.
    self->Decode(winrt::to_string(self->pasteBox_.Text()));
  });
  pasteField.Children().Append(pasteBox_);

  countText_ = MakeText({}, 13, colors::TextBrush());
  countText_.Visibility(Visibility::Collapsed);
  content.Children().Append(countText_);

  foreignText_ = MakeText({}, 12, colors::DangerBrush());
  foreignText_.Visibility(Visibility::Collapsed);
  content.Children().Append(foreignText_);

  settingsToggle_ = ToggleSwitch();
  settingsToggle_.Header(winrt::box_value(Loc("use_extender_settings")));
  settingsToggle_.OnContent(winrt::box_value(Loc("on")));
  settingsToggle_.OffContent(winrt::box_value(Loc("off")));
  settingsToggle_.IsOn(false);
  settingsToggle_.Visibility(Visibility::Collapsed);
  settingsToggle_.Style(Lookup(L"UrSwitchToggleStyle"));
  settingsToggle_.Toggled([weak = weak_from_this()](auto const&, auto const&) {
    auto self = weak.lock();
    if (!self) return;
    const bool on = self->settingsToggle_.IsOn();
    if (on == self->useSettings_) return;
    self->useSettings_ = on;
    // Changing what would be applied disarms the confirmation: the sentence the
    // user agreed to is no longer the one the button would carry out.
    self->confirmArmed_ = false;
    self->ApplyDecision();
  });
  content.Children().Append(settingsToggle_);

  controlDohText_ = MakeText({}, 12, colors::MutedBrush());
  controlDohText_.Visibility(Visibility::Collapsed);
  content.Children().Append(controlDohText_);

  importButton_ = Button();
  importButton_.Content(winrt::box_value(Loc("import_extenders")));
  importButton_.Style(Lookup(L"UrPrimaryButtonStyle"));
  importButton_.IsEnabled(false);
  importButton_.HorizontalAlignment(HorizontalAlignment::Left);
  importButton_.Click([weak = weak_from_this()](auto const&, auto const&) {
    if (auto self = weak.lock()) self->Import();
  });
  content.Children().Append(importButton_);

  statusText_ = MakeText({}, 12, colors::MutedBrush());
  statusText_.Visibility(Visibility::Collapsed);
  content.Children().Append(statusText_);

  dialog_.Content(content);

  if (!sdk_.ExtenderController()) {
    chooseButton_.IsEnabled(false);
    pasteBox_.IsEnabled(false);
    ApplyFieldState(statusText_,
                    sdk_.IsLoggedIn() ? FieldState::NoDevice : FieldState::NoSession);
    statusText_.Visibility(Visibility::Visible);
  }
}

winrt::fire_and_forget ExtenderImportSheet::ChooseImage() {
  auto weak = weak_from_this();
  auto queue = dialog_.DispatcherQueue();
  const HWND owner = owner_;
  if (busy_) co_return;
  busy_ = true;
  chooseButton_.IsEnabled(false);
  ring_.IsActive(true);
  ring_.Visibility(Visibility::Visible);
  statusText_.Visibility(Visibility::Collapsed);

  std::string decoded;
  bool cancelled = false;
  try {
    winrt::Windows::Storage::Pickers::FileOpenPicker picker;
    // A WinUI 3 desktop picker has no implicit owner window; without this it
    // throws E_ACCESSDENIED rather than opening.
    if (owner) picker.as<::IInitializeWithWindow>()->Initialize(owner);
    picker.ViewMode(winrt::Windows::Storage::Pickers::PickerViewMode::Thumbnail);
    picker.SuggestedStartLocation(
        winrt::Windows::Storage::Pickers::PickerLocationId::PicturesLibrary);
    for (auto const& ext : {L".png", L".jpg", L".jpeg", L".bmp", L".gif", L".tif", L".tiff"}) {
      picker.FileTypeFilter().Append(hstring{ext});
    }
    auto file = co_await picker.PickSingleFileAsync();
    if (!file) {
      cancelled = true;  // dismissing a picker is not a failure
    } else {
      auto stream = co_await file.OpenAsync(storage::FileAccessMode::Read);
      auto decoder = co_await imaging::BitmapDecoder::CreateAsync(stream);
      // The pixel read and the scan are the expensive parts (a photo is
      // megabytes), so they run off the UI thread.
      co_await winrt::resume_background();
      std::vector<uint8_t> luminance;
      uint32_t width = 0;
      uint32_t height = 0;
      if (ReadLuminance(decoder, luminance, width, height)) {
        decoded = DecodeQr(luminance, width, height);
      }
    }
  } catch (const std::exception& e) {
    LogWarn("extender: choose image failed: {}", e.what());
  } catch (...) {
    LogWarn("extender: choose image failed");
  }

  queue.TryEnqueue([weak, decoded, cancelled] {
    auto self = weak.lock();
    if (!self) return;
    self->busy_ = false;
    self->chooseButton_.IsEnabled(true);
    self->ring_.IsActive(false);
    self->ring_.Visibility(Visibility::Collapsed);
    if (cancelled) return;
    if (decoded.empty()) {
      // K8's own failure line: the file was read and held no code. Distinct
      // from "this is not an extender share", which is about the payload.
      self->ShowMessage(Loc("qr_code_not_found"), /*danger=*/true);
      return;
    }
    // Put it in the box, which decodes it through the TextChanged handler and
    // also leaves the payload visible and editable rather than swallowed.
    self->pasteBox_.Text(H(decoded));
  });
}

void ExtenderImportSheet::Decode(std::string const& text) {
  text_ = TrimShareText(text);
  confirmArmed_ = false;
  decoded_ = ExtenderShareDecodeView{};
  if (text_.empty()) {
    ApplyDecision();
    statusText_.Visibility(Visibility::Collapsed);
    return;
  }
  const auto controller = sdk_.ExtenderController();
  if (!controller) {
    ApplyDecision();
    return;
  }
  try {
    // decodeShare is a parse, not a network call: the payload is self
    // contained, which is the whole point of a code you can photograph.
    if (const auto result = controller->decodeShare(text_)) {
      decoded_.ok = result->Ok;
      decoded_.error = result->Error;
      decoded_.networkHost = result->NetworkHost;
      decoded_.foreignHost = result->ForeignHost;
      decoded_.count = result->Count;
      decoded_.hasSettings = result->HasSettings;
      decoded_.settingsHost = result->SettingsHost;
      if (result->ControlDohUrls) decoded_.controlDohUrls = *result->ControlDohUrls;
    }
  } catch (const std::exception& e) {
    LogWarn("extender: decode share failed: {}", e.what());
    decoded_ = ExtenderShareDecodeView{};
  } catch (...) {
    LogWarn("extender: decode share failed");
    decoded_ = ExtenderShareDecodeView{};
  }
  ApplyDecision();
}

void ExtenderImportSheet::ApplyDecision() {
  const ExtenderImportDecision decision = DecideExtenderImport(decoded_, useSettings_);

  const bool haveText = !text_.empty();
  countText_.Visibility(decoded_.ok && haveText ? Visibility::Visible
                                                : Visibility::Collapsed);
  if (decoded_.ok) {
    countText_.Text(hstring{urnw::Plural("share_extenders_count", decoded_.count)});
  }

  settingsToggle_.Visibility(decision.showSettingsToggle ? Visibility::Visible
                                                         : Visibility::Collapsed);

  // The servers the settings would set see URnetwork's lookups, so they are
  // named beside the toggle, before it is turned on. A code whose settings
  // name none leaves this device's own servers alone and says nothing.
  if (haveText && !decision.controlDohUrlsArg.empty()) {
    controlDohText_.Text(hstring{urnw::Format("import_extenders_control_doh_urls",
                                              urnw::Widen(decision.controlDohUrlsArg))});
    controlDohText_.Visibility(Visibility::Visible);
  } else {
    controlDohText_.Visibility(Visibility::Collapsed);
  }

  if (haveText && decision.showForeignHost) {
    foreignText_.Text(hstring{urnw::Format("import_extenders_foreign_host",
                                           urnw::Widen(decision.messageArg))});
    foreignText_.Visibility(Visibility::Visible);
  } else {
    foreignText_.Visibility(Visibility::Collapsed);
  }

  // A payload that did not parse says so; a foreign one says so in the line
  // above, which is its own message and not an error about the text. Anything
  // else CLEARS the line: without the else the verdict on a rejected paste
  // stayed on screen under the next, valid one.
  if (!haveText) {
    statusText_.Visibility(Visibility::Collapsed);
  } else if (!decoded_.ok && !decision.showForeignHost) {
    ShowMessage(Loc(decision.messageKey), /*danger=*/true);
  } else if (!(confirmArmed_ && decision.needsConfirm)) {
    statusText_.Visibility(Visibility::Collapsed);
  }

  importButton_.IsEnabled(haveText && decision.canImport && !busy_);
  // K7's confirmation, on the button itself: the first press on an import that
  // would replace the operator settings turns the label into the question and
  // the second press answers it.
  if (confirmArmed_ && decision.needsConfirm) {
    // The armed label ANSWERS the question the line below asks ("Use the
    // extender settings from this code?"), so it is "Yes", not a second
    // "Import" that would read as the same press again. There is no `confirm`
    // id in the store; `yes` is the shipped answer word.
    importButton_.Content(winrt::box_value(Loc("yes")));
    ShowMessage(hstring{urnw::Format("import_extenders_confirm_settings",
                                     urnw::Widen(decision.confirmArg))},
                /*danger=*/false);
  } else {
    importButton_.Content(winrt::box_value(Loc("import_extenders")));
  }
}

winrt::fire_and_forget ExtenderImportSheet::Import() {
  if (busy_ || text_.empty()) co_return;
  const ExtenderImportDecision decision = DecideExtenderImport(decoded_, useSettings_);
  if (!decision.canImport) co_return;
  if (decision.needsConfirm && !confirmArmed_) {
    // arm: show the sentence and wait for a second, deliberate press
    confirmArmed_ = true;
    ApplyDecision();
    co_return;
  }
  confirmArmed_ = false;

  auto weak = weak_from_this();
  auto queue = dialog_.DispatcherQueue();
  const std::string text = text_;
  const bool useSettings = useSettings_ && decoded_.hasSettings;

  const auto controller = sdk_.ExtenderController();  // see ExtenderShareSheet::Rebuild
  if (!controller) co_return;
  busy_ = true;
  importButton_.IsEnabled(false);
  ring_.IsActive(true);
  ring_.Visibility(Visibility::Visible);

  ExtenderImportResultView result;
  bool callFailed = true;

  co_await winrt::resume_background();
  try {
    if (const auto imported = controller->importShare(text, useSettings)) {
      result.ok = imported->Ok;
      result.error = imported->Error;
      result.importedCount = imported->ImportedCount;
      callFailed = false;
    }
  } catch (const std::exception& e) {
    LogWarn("extender: import share failed: {}", e.what());
  } catch (...) {
    LogWarn("extender: import share failed");
  }

  queue.TryEnqueue([weak, result, callFailed] {
    if (auto self = weak.lock()) self->ApplyImportResult(result, callFailed);
  });
}

void ExtenderImportSheet::ApplyImportResult(ExtenderImportResultView const& result,
                                            bool callFailed) {
  busy_ = false;
  ring_.IsActive(false);
  ring_.Visibility(Visibility::Collapsed);

  if (callFailed) {
    // The call never reached a verdict, so the payload is not what is wrong
    // with this: saying "this code is not an extender share" here would blame
    // a code the SDK never looked at.
    ApplyDecision();
    ShowMessage(Loc("something_went_wrong"), /*danger=*/true);
    return;
  }

  const ExtenderImportOutcome outcome = ExtenderImportOutcomeFor(result);
  if (outcome.ok) {
    ShowMessage(hstring{urnw::Plural(outcome.messageKey, outcome.count)}, /*danger=*/false);
    // the import may have replaced the dns name, gossip url and root keys, so
    // the section behind this sheet re-reads them
    if (onImported_) onImported_();
    importButton_.IsEnabled(false);
    return;
  }
  // ApplyDecision FIRST, then the verdict: it re-enables the button and owns
  // the status line, so showing the failure before it would be showing it to a
  // line that is about to be cleared.
  ApplyDecision();
  if (outcome.messageKey == "import_extenders_foreign_host") {
    ShowMessage(hstring{urnw::Format("import_extenders_foreign_host",
                                     urnw::Widen(decoded_.networkHost))},
                /*danger=*/true);
  } else {
    ShowMessage(Loc(outcome.messageKey), /*danger=*/true);
  }
}

void ExtenderImportSheet::ShowMessage(hstring const& message, bool danger) {
  statusText_.Text(message);
  statusText_.Foreground(danger ? colors::DangerBrush() : colors::MutedBrush());
  statusText_.Visibility(Visibility::Visible);
}

}  // namespace urnw
