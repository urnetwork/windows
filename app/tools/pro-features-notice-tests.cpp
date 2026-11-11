// Executable spec for what the app does with the sdk's Pro features decisions
// (App/ProFeaturesNotice.h): the words for each prompt, the device list's
// notice and its Get Pro, and when a balance read is the purchase that reads the
// network's proxies again. The decisions themselves (the prompt, when the main
// window's notice shows, the dismissal) are the sdk's, tested there. Run on any
// host with a C++20 compiler:
//
//   c++ -std=c++20 -Wall -Wextra -Werror -I ../src/App pro-features-notice-tests.cpp -o /tmp/pro-features-notice-tests && /tmp/pro-features-notice-tests
//
// SPDX-License-Identifier: MPL-2.0

#include <iostream>
#include <optional>
#include <string>

#include "ProFeaturesNotice.h"

using namespace urnw::profeatures;

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

// The keys a prompt reads, as "title/body", or "" for no words.
std::string KeysOf(const std::optional<Words>& words) {
  if (!words) return std::string();
  return std::string(words->titleKey) + "/" + words->messageKey;
}

void TestTheWords() {
  Check(KeysOf(WordsFor(kPromptUpgrade)) == "pro_features_in_use/pro_features_upgrade_message",
        "before enforcement: Pro features in use, and an upgrade keeps them working");
  Check(KeysOf(WordsFor(kPromptStopped)) == "pro_features_stopped/pro_features_stopped_message",
        "after enforcement: SOCKS and WireGuard stopped working, and an upgrade turns them back on");
  Check(!WordsFor(kPromptNone), "no prompt: nothing shows");
  Check(!WordsFor("removed_later"), "a prompt this app has no words for: nothing shows");
  Check(!WordsFor("Upgrade") && !WordsFor(" upgrade"), "the sdk's spelling only");
}

void TestTheDeviceList() {
  const std::optional<ListNotice> upgrade = ListNoticeFor(kPromptUpgrade, false);
  Check(upgrade && KeysOf(upgrade->words) == "pro_features_in_use/pro_features_upgrade_message" &&
            upgrade->offersUpgrade,
        "the account's list: the same words, with Get Pro");
  const std::optional<ListNotice> stopped = ListNoticeFor(kPromptStopped, false);
  Check(stopped && KeysOf(stopped->words) == "pro_features_stopped/pro_features_stopped_message" &&
            stopped->offersUpgrade,
        "after enforcement too");
  const std::optional<ListNotice> signIn = ListNoticeFor(kPromptUpgrade, true);
  Check(signIn && KeysOf(signIn->words) == "pro_features_in_use/pro_features_upgrade_message" &&
            !signIn->offersUpgrade,
        "a refused sign-in's list says it, with nothing to buy with");
  Check(!ListNoticeFor(kPromptNone, false) && !ListNoticeFor(kPromptNone, true),
        "no prompt: no notice above the list");
  Check(!ListNoticeFor("removed_later", false), "no words: no notice above the list");
}

void TestAPurchaseReadsTheProxiesAgain() {
  Check(PlanTurnedPro(true, false, true, true), "free, then Pro: read the proxies again");
  Check(!PlanTurnedPro(false, false, true, true),
        "the first read of a sign-in finding Pro: the controller read them at its start");
  Check(!PlanTurnedPro(true, true, true, true), "Pro all along");
  Check(!PlanTurnedPro(true, false, true, false), "still free");
  Check(!PlanTurnedPro(true, true, true, false), "a lapse is no purchase");
  Check(!PlanTurnedPro(true, false, false, true), "a read that did not load says nothing");
}

}  // namespace

int main() {
  TestTheWords();
  TestTheDeviceList();
  TestAPurchaseReadsTheProxiesAgain();
  std::cout << (gCases - gFailures) << "/" << gCases << " pro features notice checks passed\n";
  return gFailures == 0 ? 0 : 1;
}
