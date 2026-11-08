// Executable spec for how the device list reads (App/ManageDevicesPresentation.h):
// a row's title and the line under it, and what the sheet says about the list's
// state. The list, its order, what is removable and the retry after a removal
// are the sdk's (ManageDevicesViewController), tested there. Run on any host with
// a C++20 compiler:
//
//   c++ -std=c++20 -Wall -Wextra -Werror -I ../src/App manage-devices-tests.cpp -o /tmp/manage-devices-tests && /tmp/manage-devices-tests
//
// SPDX-License-Identifier: MPL-2.0

#include <iostream>
#include <string>

#include "ManageDevicesPresentation.h"

using namespace urnw::managedevices;

namespace {

int gFailures = 0;
int gCases = 0;

void Check(bool condition, const std::string& what) {
  ++gCases;
  if (!condition) {
    ++gFailures;
    std::cout << "  FAIL " << what << std::endl;
  }
}

// The store's words stand in as their keys, so a check reads which key says it.
Text KeyText() {
  Text text;
  text.loc = [](const char* key) { return std::string("<") + key + ">"; };
  text.format = [](const char* key, const std::string& arg) {
    return std::string("<") + key + ":" + arg + ">";
  };
  text.relative = [](int64_t unixMillis) { return "t" + std::to_string(unixMillis); };
  return text;
}

void TestARowsTitle() {
  const Text text = KeyText();
  Device named;
  named.name = "Brien's laptop";
  Check(TitleOf(named, text) == "Brien's laptop", "a device with a name: its name");
  Device unnamed;
  unnamed.spec = "10.0.26100 x64";
  Check(TitleOf(unnamed, text) == "<sessions_device_unknown>",
        "a device with no name: Unknown device, its spec underneath");
  Device proxy;
  proxy.kind = kKindProxy;
  Check(TitleOf(proxy, text) == "<manage_devices_proxy>", "a proxy with no name: Hosted proxy");
}

void TestTheLineUnderIt() {
  const Text text = KeyText();
  Device self;
  self.name = "This PC";
  self.spec = "10.0.26100 x64";
  self.thisDevice = true;
  self.connected = true;
  Check(DetailOf(self, text) ==
            "<manage_devices_this_device> · 10.0.26100 x64 · <connected>",
        "this device, its spec, connected");
  Device away;
  away.lastSeenMillis = 42;
  Check(DetailOf(away, text) == "<manage_devices_last_seen:t42>",
        "a device not connected: when it was last seen");
  Device never;
  Check(DetailOf(never, text).empty(), "nothing known: no line");
  Device proxy;
  proxy.kind = kKindProxy;
  proxy.name = "scraper";
  proxy.connected = true;
  Check(DetailOf(proxy, text) == "<manage_devices_proxy> · <connected>",
        "a named proxy says what it is under its name");
  Device unnamedProxy;
  unnamedProxy.kind = kKindProxy;
  unnamedProxy.connected = true;
  Check(DetailOf(unnamedProxy, text) == "<connected>",
        "an unnamed proxy does not say it twice");
}

void TestTheListsState() {
  Snapshot snapshot;
  snapshot.loading = true;
  Status status = StatusOf(snapshot, true);
  Check(status.loading && status.lineKey == nullptr && status.showsRows,
        "the first load: a ring, no line");
  snapshot.loading = false;
  snapshot.loaded = true;
  status = StatusOf(snapshot, true);
  Check(!status.loading && status.lineKey == nullptr && status.removeEnabled, "loaded: the rows");
  snapshot.loading = true;
  Check(!StatusOf(snapshot, true).loading, "a refresh keeps the rows rather than a ring");
  snapshot.loading = false;

  snapshot.errorId = kErrorLoadFailed;
  status = StatusOf(snapshot, true);
  Check(status.lineKey == std::string(kLoadFailedKey) && status.error && status.offersRetry,
        "a failed load says so, with Retry");
  snapshot.errorId = kErrorRemoveFailed;
  status = StatusOf(snapshot, true);
  Check(status.lineKey == std::string(kRemoveFailedKey) && status.error && !status.offersRetry,
        "a failed removal says so; Remove tries again");
  snapshot.errorId = kErrorSignInRequired;
  status = StatusOf(snapshot, true);
  Check(status.lineKey == std::string(kSignInRequiredKey) && !status.showsRows,
        "a refused credential: sign in again, no rows");
  snapshot.errorId.clear();
  Check(StatusOf(snapshot, false).lineKey == std::string(kSignInRequiredKey),
        "no credential at all: the same");

  snapshot.addRetryCount = 1;
  status = StatusOf(snapshot, true);
  Check(status.lineKey == std::string(kConnectingKey) && !status.error,
        "a removal freed a slot: connecting this device");
  snapshot.errorId = kErrorLoadFailed;
  Check(StatusOf(snapshot, true).lineKey == std::string(kLoadFailedKey),
        "a failure says more than the connecting line");
  snapshot.errorId.clear();

  snapshot.removingClientId = "c1";
  Check(!StatusOf(snapshot, true).removeEnabled, "one removal at a time");
}

void TestRemovable() {
  Device self;
  self.clientId = "c0";
  self.thisDevice = true;
  Check(!Removable(self), "never this device: it signs out instead");
  Device other;
  other.clientId = "c1";
  Check(Removable(other), "another device");
  Device anonymous;
  Check(!Removable(anonymous), "nothing to name, nothing to remove");
}

}  // namespace

int main() {
  TestARowsTitle();
  TestTheLineUnderIt();
  TestTheListsState();
  TestRemovable();
  std::cout << (gCases - gFailures) << "/" << gCases << " manage devices checks passed\n";
  return gFailures == 0 ? 0 : 1;
}
