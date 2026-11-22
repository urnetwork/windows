// Formatting helpers for the connect drawer statistics (ports of the macOS
// RateFormatUtils/HostFormatUtils, spec "Formatting helpers").
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace urnw {

// IEC base-1024: "996 B", "1.2 KiB", "3.4 MiB", "1.1 GiB"
std::string FormatByteCountCompact(int64_t byteCount);
// FormatByteCountCompact + "/s"
std::string FormatByteRate(int64_t bytesPerSecond);
// decimal base-1000: "996", "1.2k", "34k", "3.4M"
std::string FormatCountCompact(int64_t count);
// FormatCountCompact + " pkt/s"
std::string FormatPacketRate(int64_t packetsPerSecond);
// FormatCountCompact + " " + unit, for a count that is not packets: "340 reads/s"
// on the extender chart, with the unit already localized (reads_per_second)
std::string FormatCountRate(int64_t count, std::string const& unit);
// decimal: "996 bps", "1.2 Kbps", "3.4 Mbps"
std::string FormatBitRate(int64_t bitsPerSecond);

// Compact rendering of a destination cluster's host values: host names collapse
// to "*.<base>" (public-suffix aware) when there are more than 10; the combined
// host+ip list compacts to first/middle/last 7 when longer than 20.
std::string FormatHostClusterText(const std::vector<std::string>& hosts,
                                  const std::vector<std::string>& ips);

// whether the value parses as an IPv4 or IPv6 address
bool IsIpAddressValue(const std::string& value);
// whether the value is an https URL with a host (DoH endpoint)
bool IsValidDohUrl(const std::string& value);

// "now", "12s ago", "3m ago", "2h ago", "6d ago", then from a week on the
// localized date (FormatLocalDate). The buckets are RelativeTimeSpan.h's.
std::string RelativeTime(int64_t thenMillis, int64_t nowMillis);

// A Unix time in milliseconds as the user's short date in the user's time zone
// (GetDateFormatEx DATE_SHORTDATE, as the update banner's dates); the UTC
// YYYY-MM-DD when Windows cannot format it.
std::string FormatLocalDate(int64_t unixMillis);
// The same time as the user's long date and time ("Friday, October 9, 2026
// 6:41 PM"): what a relative time or a short date is read out as.
std::string FormatLocalDateTime(int64_t unixMillis);
// A whole count in the user's digit grouping ("1,204"; "1.204" or "1 204" in
// other formats), with no fraction (GetNumberFormatEx, as the dates); the plain
// digits when Windows cannot format it.
std::string FormatLocalCount(int64_t count);

}  // namespace urnw
