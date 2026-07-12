// SPDX-License-Identifier: MPL-2.0
#include "pch.h"

#include "Localization.h"

#include <winrt/Microsoft.Windows.ApplicationModel.Resources.h>

#include <algorithm>
#include <optional>

#include "../Common/Strings.h"

namespace urnw {
namespace {

using winrt::Microsoft::Windows::ApplicationModel::Resources::ResourceLoader;
using winrt::Microsoft::Windows::ApplicationModel::Resources::ResourceManager;

// The app's default resource map ("Resources" == Strings/<lang>/Resources.resw),
// out of the resources.pri MakePri writes next to the exe (App.vcxproj indexes
// the resw as PRIResource; the MSI installs the pri).
//
// This is MRT Core (Microsoft.Windows.ApplicationModel.Resources), *not* the OS
// ResourceLoader (Windows.ApplicationModel.Resources): the OS one resolves
// through the package's resource view and has no default view in a process
// without package identity, which this app is (WindowsPackageType=None). MRT
// Core reads the pri file directly and works packaged or not.
//
// Null when there is no readable resources.pri, in which case every lookup falls
// back to the key id — visibly wrong rather than silently empty.
std::optional<ResourceLoader>& Loader() {
  static std::optional<ResourceLoader> loader = []() -> std::optional<ResourceLoader> {
    try {
      return ResourceLoader{ResourceLoader::GetDefaultResourceFilePath(), L"Resources"};
    } catch (...) {
      return std::nullopt;  // never let a missing pri take the app down
    }
  }();
  return loader;
}

// The language MRT resolved the resources with, e.g. "pt-BR". Read from MRT's own
// context so the plural rule always matches the .resw that is actually loaded.
std::string PrimaryLanguage() {
  static const std::string lang = [] {
    try {
      auto context = ResourceManager().CreateResourceContext();
      winrt::hstring value = context.QualifierValues().Lookup(L"Language");
      if (!value.empty()) return Narrow(std::wstring{value});
    } catch (...) {
      // no pri / no Language qualifier: fall back to the user's locale
    }
    wchar_t name[LOCALE_NAME_MAX_LENGTH]{};
    if (0 < ::GetUserDefaultLocaleName(name, LOCALE_NAME_MAX_LENGTH)) return Narrow(name);
    return std::string{"en"};
  }();
  return lang;
}

// CLDR cardinal categories, for the languages the store ships. Keep in step with
// CLDR_CATEGORIES in localizations/gen/store.mjs.
std::string_view PluralCategory(std::string_view lang, int64_t n) {
  const auto is = [&](std::string_view p) {
    return lang.size() >= p.size() && lang.compare(0, p.size(), p) == 0;
  };
  const int64_t i = n < 0 ? -n : n;
  const int64_t mod10 = i % 10, mod100 = i % 100;

  // one category only
  if (is("ja") || is("ko") || is("zh") || is("th") || is("vi") || is("id"))
    return "other";

  if (is("ar")) {
    if (i == 0) return "zero";
    if (i == 1) return "one";
    if (i == 2) return "two";
    if (mod100 >= 3 && mod100 <= 10) return "few";
    if (mod100 >= 11) return "many";
    return "other";
  }
  if (is("he")) {
    if (i == 1) return "one";
    if (i == 2) return "two";
    if (mod10 == 0 && i != 0) return "many";
    return "other";
  }
  if (is("ru") || is("uk")) {
    if (mod10 == 1 && mod100 != 11) return "one";
    if (mod10 >= 2 && mod10 <= 4 && (mod100 < 12 || mod100 > 14)) return "few";
    return "many";
  }
  if (is("pl")) {
    if (i == 1) return "one";
    if (mod10 >= 2 && mod10 <= 4 && (mod100 < 12 || mod100 > 14)) return "few";
    return "many";
  }
  if (is("cs")) {
    if (i == 1) return "one";
    if (i >= 2 && i <= 4) return "few";
    return "other";
  }
  if (is("fr") || is("pt")) return i > 1 ? "other" : "one";

  // en, de, el, es, it, nl, sv, sw, hi, ...
  return i == 1 ? "one" : "other";
}

}  // namespace

std::wstring Localized(std::string_view key) {
  auto& loader = Loader();
  if (loader) {
    // MakePri indexes a '.' in a .resw name as a resource-map subtree separator
    // ("host_count.one" -> Resources/host_count/one) and GetString takes the path
    // form, so the dots have to become slashes. Store ids are snake_case; the only
    // segmented names are the "<id>.<category>" composites Plural() builds.
    std::string name{key};
    std::replace(name.begin(), name.end(), '.', '/');
    try {
      winrt::hstring value = loader->GetString(winrt::to_hstring(name));
      if (!value.empty()) return std::wstring{value};
    } catch (...) {
      // missing resource: fall through to the key id
    }
  }
  return Widen(key);
}

std::wstring Plural(std::string_view key, int64_t count) {
  const std::string name =
      std::string{key} + "." + std::string{PluralCategory(PrimaryLanguage(), count)};
  std::wstring fmt = Localized(name);
  if (fmt == Widen(name)) fmt = Localized(std::string{key} + ".other");  // fallback
  return std::vformat(fmt, std::make_wformat_args(count));
}

}  // namespace urnw
