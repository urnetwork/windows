// The DNS screen's provider picker (connect ADBLOCK3RD.md F1, F2; sdk
// dns_presets_ui.go): the localization keys the sdk hands the screens, by what
// each one labels. The sdk names the key; the app shows it only when this build
// knows it, and so knows the arguments it takes, and otherwise shows nothing in
// its place (an error id reads as something_went_wrong), never the bare id. The
// lists mirror the store keys the sdk returns (its
// dns_presets_localization_test.go holds the sdk to the same store), and being
// literals of the app they are what tests/catalog_lookup_test.go reads as keys
// this app looks up.
//
// Every rule of the picker -- which picks, their order and copy, the pill, the
// status, what to verify, when the blocked count shows -- is the sdk's; this
// only refuses a key this build cannot show. Pure, with no sdk header and no
// Windows dependency, so tools/dns-settings-tests.cpp runs it on any host.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <string_view>

namespace urnw::dnspresets {

// The ur.io help on DNS providers, the faq's section, behind learn_more. In
// English, as every ur.io link of the app is (ApiKeysPresentation.h kDocsUrl).
inline constexpr const wchar_t* kHelpUrl = L"https://ur.io/docs/faq#dns-providers";
// The same faq's help on personal ids, where each provider shows the id and
// which block response to keep, behind learn_more under the Use my account
// field, as android and apple link it.
inline constexpr const wchar_t* kPersonalIdsHelpUrl = L"https://ur.io/docs/faq#dns-personal-ids";

// The headings over the groups of preset picks (DnsPick.GroupKey): no
// arguments. The ad-blocking group's is the existing block_ads_and_trackers.
inline constexpr const char* kGroupKeys[] = {
    "block_ads_and_trackers", "dns_group_block_malware", "dns_group_family",
    "dns_group_my_account",   "dns_group_no_filtering",
};

// The labels of the default, Custom and Regional picks (DnsPick.LabelKey); a
// preset pick shows its provider's name instead. One argument, the exit's
// country, which only the regional label uses.
inline constexpr const char* kPickLabelKeys[] = {
    "dns_provider_default",
    "dns_provider_custom",
    "dns_provider_regional",
};

// The line under each pick (DnsPick.CopyKey, DnsPreset.CopyKey). One argument:
// the provider's name for a preset (dns_preset_copy_personal uses it) or the
// exit's country for the regional pick (dns_recommendation_message); the
// others use none.
inline constexpr const char* kPickCopyKeys[] = {
    "dns_provider_default_copy",
    "dns_provider_custom_copy",
    "dns_recommendation_message",
    "dns_preset_copy_ads_phishing",
    "dns_preset_copy_ads_malware",
    "dns_preset_copy_ads_malware_phishing",
    "dns_preset_copy_malware",
    "dns_preset_copy_malware_phishing",
    "dns_preset_copy_ads_adult_safe_search",
    "dns_preset_copy_malware_adult",
    "dns_preset_copy_ads_malware_adult_drugs",
    "dns_preset_copy_child",
    "dns_preset_copy_child_ads",
    "dns_preset_copy_nothing",
    "dns_preset_copy_court_orders",
    "dns_preset_copy_personal",
};

// The summary row of the selection (DnsSelectionSummary.SummaryKey). One
// argument, the provider's name, which the default, Custom and Regional rows
// do not use.
inline constexpr const char* kSummaryKeys[] = {
    "dns_summary_default",
    "dns_summary_custom",
    "dns_summary_custom_unencrypted",
    "dns_summary_regional",
    "dns_summary_ad_blocking",
    "dns_summary_malware_blocking",
    "dns_summary_family",
    "dns_summary_no_filtering",
    "dns_summary_personal",
};

// The resolver's status line (DnsStatusSummary.StatusKey): the provider's name.
inline constexpr const char* kStatusKeys[] = {
    "dns_status_provider",
    "dns_status_paused",
    "dns_status_fallback",
    "dns_status_unreachable",
};

// The line under the status line (DnsStatusSummary.StatusDetailKey): no
// arguments.
inline constexpr const char* kStatusDetailKeys[] = {
    "dns_status_paused_detail",
    "dns_status_unreachable_detail",
};

// The verification line (DnsStatusSummary.VerificationKey) and the label of
// its link to the provider's dashboard (VerificationLinkKey): the provider's
// name.
inline constexpr const char* kVerificationKeys[] = {
    "dns_verify_not_filtering",
    "dns_verify_unconfirmed",
    "dns_verify_rejected_controld",
    "dns_verify_malformed_adguard",
};
inline constexpr const char* kVerificationLinkKeys[] = {
    "dns_personal_id_dashboard_link",
};

// Under a Use my account pick's id field: where the provider shows the id
// (DnsPreset.PersonalIdWhereKey) and its free plan's limit (QuotaNoteKey), the
// provider's name; the id's format (PersonalIdFormatKey), the provider's name
// and then its example id (PersonalIdExample).
inline constexpr const char* kPersonalIdWhereKeys[] = {
    "dns_personal_id_where_nextdns",
    "dns_personal_id_where_controld",
    "dns_personal_id_where_adguard",
};
inline constexpr const char* kPersonalIdFormatKeys[] = {
    "dns_personal_id_format_nextdns",
    "dns_personal_id_format_controld",
    "dns_personal_id_format_adguard",
};
inline constexpr const char* kQuotaNoteKeys[] = {
    "dns_personal_id_quota_nextdns",
    "dns_personal_id_quota_adguard",
};

// The error ids of validateDnsPresetPersonalId (urnet::DnsPresetError*), each
// the store key of its message: no arguments.
inline constexpr const char* kPersonalIdErrorKeys[] = {
    "dns_preset_error_unknown",
    "dns_preset_error_personal_id_required",
    "dns_preset_error_personal_id_invalid",
    "dns_preset_error_personal_id_other_provider",
    "dns_preset_error_personal_id_not_accepted",
};

// The message of anything else an error-id call answers: an id this build
// does not know (a newer sdk) and the sdk's internal_error (SdkErrorId.h).
inline constexpr const char* kUnknownErrorKey = "something_went_wrong";

// The pills the sdk puts over the summary (DnsSelectionSummary.PillKey), by
// the sdk's ids. The recommendation pill shows the store's text of its own id
// with the exit's country; the defaults pill's id is android's key for the
// text this app shows from dns_pill_default.
inline constexpr const char* kPillRecommendedId = "dns_pill_recommended";
inline constexpr const char* kPillDefaultsId = "dns_pill_defaults";

enum class Pill {
  None,         // no pill, or one this build does not know
  Recommended,  // dns_pill_recommended, with the exit's country
  Defaults,     // dns_pill_default
};

// `id` when it is one of `known`, else "": the key to show, or nothing.
template <class Keys>
constexpr const char* Known(std::string_view id, Keys const& known) {
  for (const char* candidate : known) {
    if (id == candidate) return candidate;
  }
  return "";
}

// The store key of an error id from validateDnsPresetPersonalId: the id itself
// for the five the sdk names, and something_went_wrong for anything else, so a
// screen never shows a bare id. "" stays "": the id is valid.
constexpr const char* PersonalIdErrorKey(std::string_view id) {
  if (id.empty()) return "";
  const char* known = Known(id, kPersonalIdErrorKeys);
  return *known != '\0' ? known : kUnknownErrorKey;
}

// The pill the sdk asks for.
constexpr Pill PillOf(std::string_view id) {
  if (id == kPillRecommendedId) return Pill::Recommended;
  if (id == kPillDefaultsId) return Pill::Defaults;
  return Pill::None;
}

}  // namespace urnw::dnspresets
