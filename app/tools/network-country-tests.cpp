// Executable spec for the network country (open bug P052): the decision in
// Common/NetworkCountry.h (the mobile country code table, the provider id, the
// default-route election, the mobile broadband adapter, the registration and
// 3GPP rules, and what the service makes of what the pipe brings) and
// App/NetworkCountryWatch.h's thread (the first report, one read per burst, a
// reading reported only when it changes, a read that throws or blocks, cancel,
// and destruction, which waits out a report but not a read past its budget).
// The wire is network-country-protocol-tests.cpp.
// The watch runs on real threads and the real 750 ms window; every ordering a
// case asserts is forced by a barrier (a held read, WaitSettled, a shared
// counter), never by a sleep, and the independent cases run side by side.
//
//   c++ -std=c++20 -pthread -I ../src/Common -I ../src/App network-country-tests.cpp -o /tmp/network-country-tests && /tmp/network-country-tests
//
// SPDX-License-Identifier: MPL-2.0

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <future>
#include <iostream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "NetworkCountry.h"
#include "NetworkCountryWatch.h"

using namespace urnw;
using namespace std::chrono_literals;

namespace {

std::mutex gMutex;
int gFailures = 0;
int gCases = 0;

// Counts a case, and prints `what` when it fails.
void Check(bool condition, const std::string& what) {
  std::scoped_lock lock(gMutex);
  ++gCases;
  if (!condition) {
    ++gFailures;
    // flushed: a negative control may end the process before main returns
    std::cout << "  FAIL " << what << std::endl;
  }
}

// A reading as the failure messages show it.
std::string Show(const netcountry::Reading& r) {
  return "{\"" + r.code + "\", " + r.source + "}";
}

// The reading is exactly {code, source}.
void CheckReading(const netcountry::Reading& got, std::string_view code, std::string_view source,
                  const std::string& what) {
  Check(got.code == code && got.source == source,
        what + " (got " + Show(got) + ", want {\"" + std::string(code) + "\", " +
            std::string(source) + "})");
}

// ---- the table ----------------------------------------------------------------

// The table's shape, known assignments, and codes that name no country.
void TestMccTable() {
  bool ascending = true;
  bool codes = true;
  bool range = true;
  for (std::size_t i = 0; i < std::size(netcountry::kMccCountries); ++i) {
    const auto& entry = netcountry::kMccCountries[i];
    if (i > 0 && netcountry::kMccCountries[i - 1].mcc >= entry.mcc) ascending = false;
    const std::string_view code(entry.code, 2);
    if (entry.code[2] != '\0' || code[0] < 'a' || code[0] > 'z' || code[1] < 'a' || code[1] > 'z')
      codes = false;
    if (entry.mcc < 200 || entry.mcc > 799) range = false;
  }
  Check(ascending, "table: strictly ascending, one entry per code (the lookup is a binary search)");
  Check(codes, "table: every country is two lower-case letters");
  Check(range, "table: every code is a geographic one, 2xx to 7xx");
  Check(std::size(netcountry::kMccCountries) == 241,
        "table: AOSP MccTable's 241 entries, got " + std::to_string(std::size(netcountry::kMccCountries)));

  const std::pair<std::string_view, std::string_view> known[] = {
      {"250", "ru"}, {"255", "ua"}, {"257", "by"}, {"401", "kz"}, {"310", "us"}, {"316", "us"},
      {"234", "gb"}, {"235", "gb"}, {"262", "de"}, {"208", "fr"}, {"460", "cn"}, {"432", "ir"},
      {"286", "tr"}, {"404", "in"}, {"406", "in"}, {"724", "br"}, {"202", "gr"}, {"750", "fk"},
  };
  for (const auto& [mcc, code] : known) {
    Check(netcountry::CountryForMcc(mcc) == code,
          "mcc: " + std::string(mcc) + " is " + std::string(code) + ", got \"" +
              std::string(netcountry::CountryForMcc(mcc)) + "\"");
  }
  for (const std::string_view none : {"901", "001", "999", "317", "200", "201", "799"}) {
    Check(netcountry::CountryForMcc(none).empty(),
          "mcc: " + std::string(none) + " is assigned to no country");
  }
  for (const std::string_view malformed : {"", "25", "2500", "2a0", " 25", "25 ", "-50"}) {
    Check(netcountry::CountryForMcc(malformed).empty(),
          "mcc: \"" + std::string(malformed) + "\" is not a mobile country code");
  }
}

// A 3GPP provider id is a mobile country code and a network code, nothing else.
void TestProviderId() {
  Check(netcountry::CountryForProviderId("25001") == "ru", "provider id: MCC 250 with a two-digit MNC");
  Check(netcountry::CountryForProviderId("250020") == "ru", "provider id: and with a three-digit one");
  Check(netcountry::CountryForProviderId("310260") == "us", "provider id: a US network");
  Check(netcountry::CountryForProviderId("23415") == "gb", "provider id: a UK network");
  for (const std::string_view bad : {"", "2500", "2500123", "25O01", "250 01", "+25001"}) {
    Check(netcountry::CountryForProviderId(bad).empty(),
          "provider id: \"" + std::string(bad) + "\" is not MCC+MNC");
  }
  Check(netcountry::CountryForProviderId("90112").empty(),
        "provider id: an international network (901) is in no country");
}

// ---- the default route -------------------------------------------------------------

// The connected default route with the lowest metric carries the traffic.
void TestElection() {
  using netcountry::DefaultRoute;
  Check(netcountry::ElectDefaultRoute({}) == 0, "election: no default route, no interface");
  Check(netcountry::ElectDefaultRoute({{.ifIndex = 7, .metric = 50, .connected = true},
                                       {.ifIndex = 9, .metric = 25, .connected = true}}) == 9,
        "election: the lowest metric carries the traffic");
  Check(netcountry::ElectDefaultRoute({{.ifIndex = 7, .metric = 10, .connected = false},
                                       {.ifIndex = 9, .metric = 30, .connected = true}}) == 9,
        "election: a link that is down carries nothing, whatever its metric");
  Check(netcountry::ElectDefaultRoute({{.ifIndex = 7, .metric = 10, .connected = false}}) == 0,
        "election: no connected default route, no interface (no link-down fallback)");
  Check(netcountry::ElectDefaultRoute({{.ifIndex = 3, .metric = 20, .connected = true},
                                       {.ifIndex = 4, .metric = 20, .connected = true}}) == 3,
        "election: the first of equals, as the forward table orders them");
  Check(netcountry::ElectDefaultRoute({{.ifIndex = 0, .metric = 1, .connected = true},
                                       {.ifIndex = 5, .metric = 9, .connected = true}}) == 5,
        "election: index 0 is no interface");
}

// The adapter, registration, data class and adapter identity rules.
void TestAdapterRules() {
  Check(netcountry::IsMobileBroadbandInterface(243, 0), "adapter: IF_TYPE_WWANPP");
  Check(netcountry::IsMobileBroadbandInterface(244, 0), "adapter: IF_TYPE_WWANPP2");
  Check(netcountry::IsMobileBroadbandInterface(6, 8), "adapter: a wireless WAN medium");
  Check(!netcountry::IsMobileBroadbandInterface(71, 9), "adapter: Wi-Fi is not mobile broadband");
  Check(!netcountry::IsMobileBroadbandInterface(6, 14),
        "adapter: Ethernet (a USB-tethered phone among them) is not mobile broadband");
  Check(!netcountry::IsMobileBroadbandInterface(53, 0), "adapter: a tunnel is not mobile broadband");

  for (const uint32_t state : {3u, 4u, 5u}) {
    Check(netcountry::IsRegistered(state), "registration: state " + std::to_string(state) + " is registered");
  }
  for (const uint32_t state : {0u, 1u, 2u, 6u}) {
    Check(!netcountry::IsRegistered(state),
          "registration: state " + std::to_string(state) + " is not registered");
  }

  Check(netcountry::Is3gppDataClass(0x20), "data class: LTE is 3GPP");
  Check(netcountry::Is3gppDataClass(0x1), "data class: GPRS is 3GPP");
  Check(netcountry::Is3gppDataClass(0x40) && netcountry::Is3gppDataClass(0x80), "data class: 5G is 3GPP");
  Check(!netcountry::Is3gppDataClass(0), "data class: none is not 3GPP");
  Check(!netcountry::Is3gppDataClass(0x10000), "data class: CDMA 1xRTT is not 3GPP");
  Check(!netcountry::Is3gppDataClass(0x20 | 0x20000), "data class: a mixed 3GPP and CDMA class is not 3GPP");
  Check(!netcountry::Is3gppDataClass(0x80000000u), "data class: a custom class is not 3GPP");

  Check(netcountry::SameInterfaceGuid("{00000000-0000-4000-8000-0000000000AB}",
                                      "00000000-0000-4000-8000-0000000000ab"),
        "guid: braces and case do not matter");
  Check(netcountry::SameInterfaceGuid("{00000000-0000-4000-8000-0000000000AB}",
                                      "{00000000-0000-4000-8000-0000000000AB}"),
        "guid: the same text");
  Check(!netcountry::SameInterfaceGuid("{00000000-0000-4000-8000-0000000000AB}",
                                       "{00000000-0000-4000-8000-0000000000AC}"),
        "guid: another adapter");
  Check(!netcountry::SameInterfaceGuid("", ""), "guid: no id names no adapter");
  Check(!netcountry::SameInterfaceGuid("{}", "{}"), "guid: empty braces name no adapter");
}

// ---- the decision ---------------------------------------------------------------------

// Every branch of the decision, and its source.
void TestReadingFor() {
  using netcountry::MobileBroadbandFacts;
  MobileBroadbandFacts facts;
  CheckReading(netcountry::ReadingFor(facts), "", netcountry::kSourceNoDefaultRoute,
               "decision: no default route, no country");
  facts.defaultRoute = true;
  CheckReading(netcountry::ReadingFor(facts), "", netcountry::kSourceNotMobileBroadband,
               "decision: Wi-Fi or Ethernet, no country (never the locale)");
  facts.mobileBroadband = true;
  CheckReading(netcountry::ReadingFor(facts), "", netcountry::kSourceUnreadable,
               "decision: an adapter MbnApi does not answer for");
  MobileBroadbandFacts lte{.defaultRoute = true,
                           .mobileBroadband = true,
                           .readable = true,
                           .registerState = netcountry::kRegisterStateHome,
                           .dataClass = 0x20,
                           .providerId = "25001"};
  CheckReading(netcountry::ReadingFor(lte), "ru", netcountry::kSourceMobileBroadband,
               "decision: registered at home on LTE, MCC 250");
  MobileBroadbandFacts roaming = lte;
  roaming.registerState = netcountry::kRegisterStateRoaming;
  roaming.providerId = "250990";
  CheckReading(netcountry::ReadingFor(roaming), "ru", netcountry::kSourceMobileBroadband,
               "decision: a foreign SIM roaming on a Russian network reports the network's country");
  MobileBroadbandFacts partner = lte;
  partner.registerState = netcountry::kRegisterStatePartner;
  CheckReading(netcountry::ReadingFor(partner), "ru", netcountry::kSourceMobileBroadband,
               "decision: a partner network is registered too");
  for (const uint32_t state : {0u, 1u, 2u, 6u}) {
    MobileBroadbandFacts unregistered = lte;
    unregistered.registerState = state;
    CheckReading(netcountry::ReadingFor(unregistered), "", netcountry::kSourceNotRegistered,
                 "decision: register state " + std::to_string(state) + " has no network");
  }
  MobileBroadbandFacts cdma = lte;
  cdma.dataClass = 0x20000;
  cdma.providerId = "41830";
  CheckReading(netcountry::ReadingFor(cdma), "", netcountry::kSourceNot3gpp,
               "decision: a CDMA data class is no country (its id is a system id, not an MCC)");
  MobileBroadbandFacts noData = lte;
  noData.dataClass = 0;
  CheckReading(netcountry::ReadingFor(noData), "", netcountry::kSourceNot3gpp,
               "decision: no data class, no way to read the id as an MCC");
  MobileBroadbandFacts international = lte;
  international.providerId = "90112";
  CheckReading(netcountry::ReadingFor(international), "", netcountry::kSourceUnknownMcc,
               "decision: an international network has no country");
  MobileBroadbandFacts empty = lte;
  empty.providerId.clear();
  CheckReading(netcountry::ReadingFor(empty), "", netcountry::kSourceUnknownMcc,
               "decision: no provider id, no country");
}

// What the service takes off the pipe.
void TestNormalized() {
  Check(netcountry::CodeOrEmpty("RU") == "ru", "normalize: upper case is lower-cased");
  Check(netcountry::CodeOrEmpty("ru") == "ru", "normalize: lower case stands");
  for (const std::string_view bad : {"", "r", "rus", "r1", " r", "\xd1\x80\xd1\x83"}) {
    Check(netcountry::CodeOrEmpty(bad).empty(), "normalize: \"" + std::string(bad) + "\" is no country");
  }
  for (const std::string_view source :
       {netcountry::kSourceMobileBroadband, netcountry::kSourceNoDefaultRoute,
        netcountry::kSourceNotMobileBroadband, netcountry::kSourceUnreadable,
        netcountry::kSourceNot3gpp, netcountry::kSourceNotRegistered,
        netcountry::kSourceUnknownMcc}) {
    Check(netcountry::SourceOrUnknown(source) == source,
          "normalize: the source \"" + std::string(source) + "\" stands");
  }
  Check(netcountry::SourceOrUnknown("") == netcountry::kSourceUnknown,
        "normalize: an older app sends no source");
  Check(netcountry::SourceOrUnknown("locale") == netcountry::kSourceUnknown,
        "normalize: a source this build does not know is unknown, never passed on");
  CheckReading(netcountry::Normalized("UA", "mobile-broadband"), "ua", netcountry::kSourceMobileBroadband,
               "normalize: what the service takes off the pipe");
  CheckReading(netcountry::Normalized("ukraine", "x\ny"), "", netcountry::kSourceUnknown,
               "normalize: a peer's garbage is no country and no source");
}

// ---- the watch ------------------------------------------------------------------------------

// A read the cases script: the reading it answers, how often it was entered and
// returned, and a gate that holds it until released. Waits are barriers on
// these counts, never sleeps.
struct ScriptedRead {
  std::mutex mutex;
  std::condition_variable changed;
  bool hold = false;
  bool throwNext = false;
  netcountry::Reading answer{.code = {}, .source = std::string(netcountry::kSourceNotMobileBroadband)};
  int entered = 0;
  int returned = 0;
  // the order the destruction case checks, by a counter shared with the case
  std::atomic<int>* clock = nullptr;
  int returnedAt = 0;

  netcountry::Reading operator()() {
    std::unique_lock lock(mutex);
    ++entered;
    changed.notify_all();
    changed.wait(lock, [this] { return !hold; });
    ++returned;
    if (clock) returnedAt = ++*clock;
    changed.notify_all();
    if (throwNext) {
      throwNext = false;
      throw std::runtime_error("the WWAN service did not answer");
    }
    return answer;
  }
  void Set(netcountry::Reading reading) {
    std::scoped_lock lock(mutex);
    answer = std::move(reading);
  }
  void Hold(bool on) {
    {
      std::scoped_lock lock(mutex);
      hold = on;
    }
    changed.notify_all();
  }
  // Barrier: the read has been entered `count` times; false after a generous
  // budget, which only a broken watch reaches.
  bool WaitEntered(int count) {
    std::unique_lock lock(mutex);
    return changed.wait_for(lock, 10s, [&] { return entered >= count; });
  }
  // Barrier: the read has returned `count` times.
  bool WaitReturned(int count) {
    std::unique_lock lock(mutex);
    return changed.wait_for(lock, 10s, [&] { return returned >= count; });
  }
  int Returned() {
    std::scoped_lock lock(mutex);
    return returned;
  }
};

// The reports a case received, in order.
struct Reports {
  std::mutex mutex;
  std::vector<netcountry::Reading> seen;

  void Add(const netcountry::Reading& reading) {
    std::scoped_lock lock(mutex);
    seen.push_back(reading);
  }
  int Count() {
    std::scoped_lock lock(mutex);
    return static_cast<int>(seen.size());
  }
  netcountry::Reading Last() {
    std::scoped_lock lock(mutex);
    return seen.empty() ? netcountry::Reading{} : seen.back();
  }
};

// The generous budget a barrier gets; only a broken watch runs it out.
constexpr auto kBarrier = 10s;

// The read budget of the cases that expect destruction to join: far beyond
// how long any of them holds a read, so only a broken watch leaves one behind.
constexpr auto kJoinBudget = 60s;

// A report the cases hold: how often it was entered and returned, a gate that
// holds it until released, and when it returned by a counter shared with the
// case.
struct HeldReport {
  std::mutex mutex;
  std::condition_variable changed;
  bool hold = false;
  int entered = 0;
  int returned = 0;
  std::atomic<int>* clock = nullptr;
  int returnedAt = 0;

  void operator()(const netcountry::Reading&) {
    std::unique_lock lock(mutex);
    ++entered;
    changed.notify_all();
    changed.wait(lock, [this] { return !hold; });
    ++returned;
    if (clock) returnedAt = ++*clock;
    changed.notify_all();
  }
  void Hold(bool on) {
    {
      std::scoped_lock lock(mutex);
      hold = on;
    }
    changed.notify_all();
  }
  // Barrier: the report has been entered `count` times.
  bool WaitEntered(int count) {
    std::unique_lock lock(mutex);
    return changed.wait_for(lock, kBarrier, [&] { return entered >= count; });
  }
  // Barrier: the report has returned `count` times; the time it last did.
  int WaitReturnedAt(int count) {
    std::unique_lock lock(mutex);
    changed.wait_for(lock, kBarrier, [&] { return returned >= count; });
    return returned >= count ? returnedAt : 0;
  }
};

// Ended when the last holder of its End lets go: a barrier on a lifetime, not
// on a clock.
struct Lifetime {
  std::mutex mutex;
  std::condition_variable changed;
  bool ended = false;

  // Ends the lifetime when destroyed; shared, so the last holder ends it.
  class End {
   public:
    explicit End(std::shared_ptr<Lifetime> lifetime) : lifetime_(std::move(lifetime)) {}
    ~End() {
      {
        std::scoped_lock lock(lifetime_->mutex);
        lifetime_->ended = true;
      }
      lifetime_->changed.notify_all();
    }
    End(const End&) = delete;
    End& operator=(const End&) = delete;

   private:
    std::shared_ptr<Lifetime> lifetime_;
  };

  // Barrier: the last holder has let go.
  bool WaitEnded() {
    std::unique_lock lock(mutex);
    return changed.wait_for(lock, kBarrier, [this] { return ended; });
  }
};

// When a case's watch was destroyed, by the counter its read or report
// returned by, and a release that waits for it.
struct Destroyed {
  std::mutex mutex;
  std::condition_variable changed;
  int at = 0;

  void Note(std::atomic<int>& clock) {
    {
      std::scoped_lock lock(mutex);
      at = ++clock;
    }
    changed.notify_all();
  }
  // Waits until the watch is gone, or `budget` when destruction is waiting for
  // what the caller holds.
  void WaitOrAfter(std::chrono::milliseconds budget) {
    std::unique_lock lock(mutex);
    changed.wait_for(lock, budget, [this] { return at != 0; });
  }
  int At() {
    std::scoped_lock lock(mutex);
    return at;
  }
};

const netcountry::Reading kRu{.code = "ru", .source = std::string(netcountry::kSourceMobileBroadband)};

// The first reading is read and reported before WaitFirstReport returns, and
// nothing is read again until something is observed.
void TestWatchFirstReport() {
  ScriptedRead read;
  Reports reports;
  NetworkCountryWatch watch([&] { return read(); }, [&](const auto& r) { reports.Add(r); }, kJoinBudget);
  Check(watch.WaitFirstReport(kBarrier), "watch: the first reading is read and reported");
  Check(reports.Count() == 1, "watch: the first report comes before WaitFirstReport returns");
  CheckReading(reports.Last(), "", netcountry::kSourceNotMobileBroadband,
               "watch: the first reading is reported even when it is no country");
  Check(watch.WaitSettled(kBarrier), "watch: the thread settles");
  Check(read.Returned() == 1 && reports.Count() == 1,
        "watch: nothing observed, nothing read again (reads " + std::to_string(read.Returned()) + ")");
}

// A burst observed while a read runs is one more read, not one per observation;
// an unchanged reading is not reported again, a changed one is.
void TestWatchBurst() {
  ScriptedRead read;
  Reports reports;
  read.Hold(true);
  NetworkCountryWatch watch([&] { return read(); }, [&](const auto& r) { reports.Add(r); }, kJoinBudget);
  Check(read.WaitEntered(1), "burst: the first read runs");
  // a roam: twenty observations while the thread is held in its read, so none
  // of them can be taken before the last is recorded
  const auto sink = watch.NetworkEventSink();
  for (int i = 0; i < 20; ++i) sink();
  read.Hold(false);
  Check(watch.WaitSettled(kBarrier), "burst: the thread settles after the burst");
  Check(read.Returned() == 2, "burst: a burst is one read, got " + std::to_string(read.Returned() - 1));
  Check(reports.Count() == 1, "burst: an unchanged reading is not reported again");

  read.Set(kRu);
  sink();
  Check(watch.WaitSettled(kBarrier), "burst: the thread settles after a change");
  Check(reports.Count() == 2, "burst: a changed reading is reported");
  CheckReading(reports.Last(), "ru", netcountry::kSourceMobileBroadband, "burst: with the new country");
}

// A read that throws is a reading of its own, and the thread lives on.
void TestWatchReadThrows() {
  ScriptedRead read;
  Reports reports;
  read.Set(kRu);
  NetworkCountryWatch watch([&] { return read(); }, [&](const auto& r) { reports.Add(r); }, kJoinBudget);
  Check(watch.WaitSettled(kBarrier), "throw: first report");
  {
    std::scoped_lock lock(read.mutex);
    read.throwNext = true;
  }
  watch.NetworkEventSink()();
  Check(watch.WaitSettled(kBarrier) && reports.Count() == 2, "throw: a read that throws is reported");
  CheckReading(reports.Last(), "", netcountry::kSourceUnreadable, "throw: unreadable, no country");
  watch.NetworkEventSink()();
  Check(watch.WaitSettled(kBarrier) && reports.Count() == 3, "throw: the thread lives on and reads the next burst");
  CheckReading(reports.Last(), "ru", netcountry::kSourceMobileBroadband, "throw: and reports it");
}

// A report that throws does not end the thread.
void TestWatchReportThrows() {
  ScriptedRead read;
  std::atomic<int> reports{0};
  NetworkCountryWatch watch([&] { return read(); }, [&](const auto&) {
    ++reports;
    throw std::runtime_error("the owner's report failed");
  }, kJoinBudget);
  Check(watch.WaitSettled(kBarrier) && reports.load() == 1, "report throws: reported");
  read.Set(kRu);
  watch.NetworkEventSink()();
  Check(watch.WaitSettled(kBarrier) && reports.load() == 2,
        "report throws: a report that throws does not end the thread");
}

// A first read that does not answer is not waited out; it is reported when it
// lands.
void TestWatchBlockedFirstRead() {
  ScriptedRead read;
  Reports reports;
  read.Hold(true);
  NetworkCountryWatch watch([&] { return read(); }, [&](const auto& r) { reports.Add(r); }, kJoinBudget);
  Check(read.WaitEntered(1), "blocked: the first read runs");
  Check(!watch.WaitFirstReport(200ms), "blocked: a read that does not answer is not waited out");
  Check(reports.Count() == 0, "blocked: nothing reported while the read hangs");
  read.Hold(false);
  Check(watch.WaitFirstReport(kBarrier), "blocked: the late first reading is reported when it lands");
  Check(reports.Count() == 1, "blocked: once");
}

// A read held across explicit cancellation and destruction still finishes on
// the worker. The channel owns the callbacks until the worker drops the read.
void TestWatchCancel() {
  auto read = std::make_shared<ScriptedRead>();
  auto reports = std::make_shared<Reports>();
  auto channelGone = std::make_shared<Lifetime>();
  std::promise<void> destroyed;
  std::thread release;
  {
    NetworkCountryWatch watch(
        [read] { return (*read)(); },
        [reports, end = std::make_shared<Lifetime::End>(channelGone)](const auto& r) { reports->Add(r); },
        0ms);
    Check(watch.WaitSettled(kBarrier), "cancel: first report");
    read->Hold(true);
    read->Set(kRu);
    watch.NetworkEventSink()();
    Check(read->WaitEntered(2), "cancel: the burst's read runs");
    watch.Cancel();
    // An unbounded-join mutant must also get to finish and report its failure.
    release = std::thread([read, gone = destroyed.get_future()] {
      gone.wait_for(kBarrier);
      read->Hold(false);
    });
  }
  Check(read->Returned() == 1, "cancel: the second read is still held after destruction");
  destroyed.set_value();
  release.join();
  Check(channelGone->WaitEnded(), "cancel: the worker releases the channel after the read");
  Check(read->Returned() == 2, "cancel: the read returned");
  Check(reports->Count() == 1, "cancel: a read that finishes after the cancel is not reported");
}

// Destruction joins a read already running that returns within the read
// budget: it returns only after that read has returned. The order is read off
// a shared counter, so no clock decides it.
void TestWatchDestruction() {
  ScriptedRead read;
  Reports reports;
  std::atomic<int> clock{0};
  read.clock = &clock;
  std::function<void()> sink;
  std::mutex destroyedMutex;
  std::condition_variable destroyedChanged;
  int destroyedAt = 0;
  auto watch = std::make_unique<NetworkCountryWatch>([&] { return read(); },
                                                     [&](const auto& r) { reports.Add(r); }, kJoinBudget);
  Check(watch->WaitSettled(kBarrier), "destruction: first report");
  sink = watch->NetworkEventSink();
  read.Hold(true);
  read.Set(kRu);
  sink();
  Check(read.WaitEntered(2), "destruction: the burst's read runs");
  // released once the destructor has returned, or after the barrier's budget
  // when it waits for the read, as it must
  std::thread release([&] {
    {
      std::unique_lock lock(destroyedMutex);
      destroyedChanged.wait_for(lock, 2s, [&] { return destroyedAt != 0; });
    }
    read.Hold(false);
  });
  watch.reset();
  {
    std::scoped_lock lock(destroyedMutex);
    destroyedAt = ++clock;
  }
  destroyedChanged.notify_all();
  release.join();
  Check(read.WaitReturned(2), "destruction: the read returns");
  Check(read.returnedAt != 0 && read.returnedAt < destroyedAt,
        "join: destruction returns only after the read already running has returned");
  // a sink that outlives the watch records into a cancelled channel
  sink();
  Check(read.Returned() == 2, "destruction: nothing is read once the watch is gone");
}

// A report that is running when the watch is destroyed is waited out, past
// the read budget: the report is the owner's, and must not run against an
// owner being destroyed. What the thread can touch is shared with it, so a
// broken watch that leaves it running fails the check rather than the process.
void TestWatchDestructionWaitsOutAReport() {
  auto clock = std::make_shared<std::atomic<int>>(0);
  auto report = std::make_shared<HeldReport>();
  report->clock = clock.get();
  report->Hold(true);
  Destroyed destroyed;
  auto watch = std::make_unique<NetworkCountryWatch>(
      [] { return kRu; }, [report, clock](const auto& r) { (*report)(r); }, 100ms);
  Check(report->WaitEntered(1), "report: the first report runs");
  // released once destruction has returned, or after two seconds when it waits
  // for the report, as it must
  std::thread release([&destroyed, report] {
    destroyed.WaitOrAfter(2s);
    report->Hold(false);
  });
  watch.reset();
  destroyed.Note(*clock);
  release.join();
  const int returnedAt = report->WaitReturnedAt(1);
  Check(returnedAt != 0 && returnedAt < destroyed.At(),
        "report: destruction returns only after the report already running has returned, past the read budget");
}

// A read that is still running past the read budget does not hold up
// destruction (a WWAN service that never answers must not hold up the app's
// exit): the watch is gone while the read is held, and the thread left to
// finish it reports nothing and then lets go of the channel. Everything that
// thread can touch is shared with it, as the owner's read must be.
void TestWatchAbandonsAHungRead() {
  auto clock = std::make_shared<std::atomic<int>>(0);
  auto read = std::make_shared<ScriptedRead>();
  read->clock = clock.get();
  auto reports = std::make_shared<Reports>();
  auto channelGone = std::make_shared<Lifetime>();
  auto end = std::make_shared<Lifetime::End>(channelGone);
  Destroyed destroyed;
  auto watch = std::make_unique<NetworkCountryWatch>(
      [read, clock] { return (*read)(); }, [reports, end](const auto& r) { reports->Add(r); }, 100ms);
  // the channel's report holds the only End from here on
  end.reset();
  Check(watch->WaitSettled(kBarrier), "hung read: first report");
  read->Hold(true);
  read->Set(kRu);
  watch->NetworkEventSink()();
  Check(read->WaitEntered(2), "hung read: the burst's read runs");
  // released once destruction has returned, or after the barrier's budget when
  // it waits for the read without a bound
  std::thread release([&destroyed, read] {
    destroyed.WaitOrAfter(kBarrier);
    read->Hold(false);
  });
  watch.reset();
  destroyed.Note(*clock);
  release.join();
  Check(read->WaitReturned(2), "hung read: the read returns once it is answered");
  Check(destroyed.At() != 0 && destroyed.At() < read->returnedAt,
        "hung read: destruction returns while a read past the budget is still running");
  Check(channelGone->WaitEnded(), "hung read: the thread left to finish the read lets go of the channel");
  Check(reports->Count() == 1, "hung read: a read that finishes after destruction is not reported");
}

}  // namespace

int main() {
  TestMccTable();
  TestProviderId();
  TestElection();
  TestAdapterRules();
  TestReadingFor();
  TestNormalized();

  // The watch's cases are independent and each waits out windows: side by side.
  std::vector<std::thread> cases;
  for (const auto test : {TestWatchFirstReport, TestWatchBurst, TestWatchReadThrows, TestWatchReportThrows,
                          TestWatchBlockedFirstRead, TestWatchCancel, TestWatchDestruction,
                          TestWatchDestructionWaitsOutAReport, TestWatchAbandonsAHungRead}) {
    cases.emplace_back(test);
  }
  for (auto& thread : cases) thread.join();

  std::cout << (gCases - gFailures) << "/" << gCases << " network country checks passed" << std::endl;
  return gFailures == 0 ? 0 : 1;
}
