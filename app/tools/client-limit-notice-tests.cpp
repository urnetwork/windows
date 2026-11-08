// Executable spec for what the app does with the sdk's client limit decisions
// (App/ClientLimitNotice.h): when the connect screen's notice shows and what it
// offers, what the tray does for each of the notice tracker's actions, when an
// upgrade ends the hold, and the words a refused sign-in shows. The decisions
// themselves (the prompt, the tracker) are the sdk's, tested there. Run on any
// host with a C++20 compiler:
//
//   c++ -std=c++20 -Wall -Wextra -Werror -I ../src/App client-limit-notice-tests.cpp -o /tmp/client-limit-notice-tests && /tmp/client-limit-notice-tests
//
// SPDX-License-Identifier: MPL-2.0

#include <iostream>
#include <optional>
#include <string>

#include "ClientLimitNotice.h"

using namespace urnw::clientlimit;

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

Reading HeldWith(const std::string& prompt) {
  return Reading{.status = kStatusExceeded, .retryTimeMillis = 1, .prompt = prompt};
}

void TestTheNoticeFollowsTheHoldAndTheRequest() {
  Check(!NoticeFor(std::nullopt, true), "no reading: no notice");
  Check(!NoticeFor(Reading{}, true), "a device not held: no notice");
  Check(!NoticeFor(HeldWith(kPromptUpgrade), false), "held with no connection requested: no notice");
  Check(NoticeFor(HeldWith(kPromptUpgrade), true) == Notice{true},
        "held while a connection is requested, on a free plan: the notice offers the upgrade");
  Check(NoticeFor(HeldWith(kPromptRemoveClient), true) == Notice{false},
        "on a plan already at its most devices: no upgrade");
  Check(NoticeFor(HeldWith(""), true) == Notice{true},
        "a device process too old to send a prompt reads as free");
}

void TestTheWords() {
  Check(std::string(kTitleKey) == "client_limit_reached", "the title");
  Check(std::string(MessageKey(true)) == "client_limit_upgrade_message", "the free plan's body");
  Check(std::string(MessageKey(false)) == "client_limit_remove_client_message",
        "the full plan's body");
}

void TestTheTray() {
  Check(TrayStepFor(kNoticePost, true) == TrayStep::Alert, "a post in the background alerts");
  Check(TrayStepFor(kNoticeUpdate, true) == TrayStep::Quiet,
        "an update in the background replaces it without a sound");
  Check(TrayStepFor(kNoticePost, false) == TrayStep::None &&
            TrayStepFor(kNoticeUpdate, false) == TrayStep::None,
        "with the window in front, its notice says it and the tray shows nothing");
  Check(TrayStepFor(kNoticeRemove, true) == TrayStep::Withdraw &&
            TrayStepFor(kNoticeRemove, false) == TrayStep::Withdraw,
        "a withdrawal always goes through");
  Check(TrayStepFor("", true) == TrayStep::None, "nothing to do");
}

void TestAnUpgradeEndsTheHold() {
  Check(UpgradeEndsHold(true, false, true, true, true), "free, then Pro, while held: dial again now");
  Check(!UpgradeEndsHold(false, false, true, true, true),
        "the first read of a launch finding Pro is not an upgrade");
  Check(!UpgradeEndsHold(true, true, true, true, true), "Pro all along is not an upgrade");
  Check(!UpgradeEndsHold(true, false, true, true, false), "an upgrade with no hold ends nothing");
  Check(!UpgradeEndsHold(true, false, true, false, true), "still free ends nothing");
}

void TestARefusedSignIn() {
  Check(SignInRefusalToken(std::nullopt) == std::nullopt &&
            SignInRefusalToken(std::string()) == std::nullopt &&
            SignInRefusalToken(std::string("other")) == std::nullopt,
        "any other refusal keeps the server's message");
  const auto upgrade = SignInRefusalToken(std::string(kPromptUpgrade));
  const auto removeClient = SignInRefusalToken(std::string(kPromptRemoveClient));
  Check(upgrade && SignInRefusalMessageKey(*upgrade) == std::string("client_limit_upgrade_message"),
        "a refusal an upgrade lifts says to upgrade");
  Check(removeClient && SignInRefusalMessageKey(*removeClient) ==
                            std::string("client_limit_remove_client_message"),
        "a refusal only a free slot lifts says to free one");
  Check(SignInRefusalMessageKey("402 Payment Required") == nullptr &&
            SignInRefusalMessageKey("") == nullptr,
        "a raw error is not a token");
}

}  // namespace

int main() {
  TestTheNoticeFollowsTheHoldAndTheRequest();
  TestTheWords();
  TestTheTray();
  TestAnUpgradeEndsTheHold();
  TestARefusedSignIn();
  std::cout << (gCases - gFailures) << "/" << gCases << " client limit notice checks passed\n";
  return gFailures == 0 ? 0 : 1;
}
