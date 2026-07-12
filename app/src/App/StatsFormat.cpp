// SPDX-License-Identifier: MPL-2.0
#include "pch.h"

#include "StatsFormat.h"

#include <ws2tcpip.h>

#include <algorithm>
#include <cstdio>
#include <set>

#include "Localization.h"
#include "Sdk.h"
#include "Strings.h"  // Narrow: the wide localized strings into these utf-8 values

namespace urnw {
namespace {

// >=100 -> %.0f, >=10 -> %.1f, else %.2f (matches the macOS formatter)
std::string FormatMagnitude(double value, const char* unit) {
  char buf[48];
  if (value >= 100) {
    std::snprintf(buf, sizeof(buf), "%.0f %s", value, unit);
  } else if (value >= 10) {
    std::snprintf(buf, sizeof(buf), "%.1f %s", value, unit);
  } else {
    std::snprintf(buf, sizeof(buf), "%.2f %s", value, unit);
  }
  return buf;
}

// shows all values when 20 or fewer, else the first, middle, and last 7 in
// alphanumeric order (21 max) with the omitted count
std::string CompactValueList(const std::vector<std::string>& values) {
  auto join = [](auto first, auto last) {
    std::string out;
    for (auto it = first; it != last; ++it) {
      if (!out.empty()) out += ", ";
      out += *it;
    }
    return out;
  };

  if (values.size() <= 20) return join(values.begin(), values.end());

  std::vector<std::string> sorted = values;
  std::sort(sorted.begin(), sorted.end());
  const size_t n = sorted.size();
  const size_t middleStart = (n - 7) / 2;
  std::string text = join(sorted.begin(), sorted.begin() + 7) + ", …, " +
                     join(sorted.begin() + middleStart, sorted.begin() + middleStart + 7) +
                     ", …, " + join(sorted.begin() + (n - 7), sorted.end());
  const size_t omitted = n - 21;
  // plural rules live in the resources ("plus_n_more")
  if (0 < omitted) text += " " + Narrow(Plural("plus_n_more", static_cast<int64_t>(omitted)));
  return text;
}

}  // namespace

std::string FormatByteCountCompact(int64_t byteCount) {
  const double kib = 1024.0;
  const double mib = kib * 1024;
  const double gib = mib * 1024;
  const double tib = gib * 1024;
  const double v = static_cast<double>(byteCount);
  if (v < kib) return std::to_string(byteCount) + " B";
  if (v < mib) return FormatMagnitude(v / kib, "KiB");
  if (v < gib) return FormatMagnitude(v / mib, "MiB");
  if (v < tib) return FormatMagnitude(v / gib, "GiB");
  return FormatMagnitude(v / tib, "TiB");
}

std::string FormatByteRate(int64_t bytesPerSecond) {
  return FormatByteCountCompact(bytesPerSecond) + "/s";
}

std::string FormatCountCompact(int64_t count) {
  const double v = static_cast<double>(count);
  char buf[32];
  if (count < 1000) return std::to_string(count);
  if (v < 1e6) {
    std::snprintf(buf, sizeof(buf), v < 10000 ? "%.1fk" : "%.0fk", v / 1000);
    return buf;
  }
  std::snprintf(buf, sizeof(buf), "%.1fM", v / 1e6);
  return buf;
}

std::string FormatPacketRate(int64_t packetsPerSecond) {
  return FormatCountCompact(packetsPerSecond) + " pkt/s";
}

std::string FormatBitRate(int64_t bitsPerSecond) {
  const double v = static_cast<double>(bitsPerSecond);
  if (v < 1000) return std::to_string(bitsPerSecond) + " bps";
  if (v < 1e6) return FormatMagnitude(v / 1e3, "Kbps");
  if (v < 1e9) return FormatMagnitude(v / 1e6, "Mbps");
  return FormatMagnitude(v / 1e9, "Gbps");
}

std::string FormatHostClusterText(const std::vector<std::string>& hosts,
                                  const std::vector<std::string>& ips) {
  // host names collapse to public-suffix base names when there are more than 10
  std::vector<std::string> displayHosts = hosts;
  if (hosts.size() > 10) {
    std::set<std::string> seen;
    displayHosts.clear();
    for (const auto& host : hosts) {
      std::string display = "*." + urnet::hostBaseName(host);
      if (seen.insert(display).second) displayHosts.push_back(display);
    }
  }
  std::vector<std::string> items = displayHosts;
  items.insert(items.end(), ips.begin(), ips.end());
  if (items.empty()) return Narrow(Localized("unknown"));
  return CompactValueList(items);
}

bool IsIpAddressValue(const std::string& value) {
  // inet_pton wants winsock initialized; do it lazily once for this process
  static const int wsaInit = [] {
    WSADATA data{};
    return ::WSAStartup(MAKEWORD(2, 2), &data);
  }();
  (void)wsaInit;
  in_addr addr4{};
  if (::inet_pton(AF_INET, value.c_str(), &addr4) == 1) return true;
  in6_addr addr6{};
  return ::inet_pton(AF_INET6, value.c_str(), &addr6) == 1;
}

bool IsValidDohUrl(const std::string& value) {
  // scheme must be https and the authority must contain a host
  static constexpr const char kPrefix[] = "https://";
  constexpr size_t kPrefixLen = sizeof(kPrefix) - 1;
  if (value.size() <= kPrefixLen) return false;
  for (size_t i = 0; i < kPrefixLen; ++i) {
    if (std::tolower(static_cast<unsigned char>(value[i])) != kPrefix[i]) return false;
  }
  std::string authority = value.substr(kPrefixLen);
  if (size_t end = authority.find_first_of("/?#"); end != std::string::npos) {
    authority = authority.substr(0, end);
  }
  if (size_t at = authority.rfind('@'); at != std::string::npos) {
    authority = authority.substr(at + 1);  // strip userinfo
  }
  std::string host;
  if (!authority.empty() && authority[0] == '[') {  // bracketed ipv6
    size_t close = authority.find(']');
    host = close != std::string::npos ? authority.substr(1, close - 1) : "";
  } else if (size_t colon = authority.find(':'); colon != std::string::npos) {
    host = authority.substr(0, colon);  // strip port
  } else {
    host = authority;
  }
  return !host.empty();
}

std::string RelativeTime(int64_t thenMillis, int64_t nowMillis) {
  const int64_t seconds = std::max<int64_t>(0, (nowMillis - thenMillis) / 1000);
  if (seconds < 5) return Narrow(Localized("now"));
  if (seconds < 60) return Narrow(Format("seconds_ago_abbrev", seconds));
  if (seconds < 3600) return Narrow(Format("minutes_ago_abbrev", seconds / 60));
  return Narrow(Format("hours_ago_abbrev", seconds / 3600));
}

}  // namespace urnw
