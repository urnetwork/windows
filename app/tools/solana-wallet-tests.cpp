// Executable spec for the Solana wallet bridge's pure logic, which signing in
// with Phantom or Solflare and adding either as a sign-in method use: the words
// for the bridge page's failure codes (App/SolanaWalletPresentation.h), where
// the wallet bridge's returns go and when a late challenge may still open the
// bridge (App/WalletBridgeRoute.h) - run against the SAME sources the app
// compiles, on any host with a C++20 compiler.
//
// The WinUI halves (WalletConnect.cpp, SdkHost's wallet flows) cannot be built
// off Windows at all; what is verified here is every decision they make on
// plain values.
//
//   c++ -std=c++20 -I ../src/App solana-wallet-tests.cpp ../src/App/SolanaWalletPresentation.cpp -o /tmp/solana-wallet-tests && /tmp/solana-wallet-tests
//
// SPDX-License-Identifier: MPL-2.0

#include <cstdint>
#include <iostream>
#include <string>

#include "SolanaWalletPresentation.h"
#include "WalletBridgeRoute.h"

using namespace urnw::solana;

namespace {

int gFailures = 0;
int gCases = 0;
std::string gCurrentCase;

void Fail(const std::string& message) {
  ++gFailures;
  std::cout << "  FAIL [" << gCurrentCase << "] " << message << "\n";
}

void Check(bool condition, const std::string& message) {
  if (!condition) Fail(message);
}

void CheckEq(const std::string& expected, const std::string& actual,
             const std::string& what) {
  if (expected != actual) {
    Fail(what + ": expected \"" + expected + "\", got \"" + actual + "\"");
  }
}

void CheckEq(long long expected, long long actual, const std::string& what) {
  if (expected != actual) {
    Fail(what + ": expected " + std::to_string(expected) + ", got " +
         std::to_string(actual));
  }
}

struct Case {
  explicit Case(const char* name) {
    gCurrentCase = name;
    ++gCases;
  }
};
#define TEST_CASE(name) Case case_##__LINE__(name)

// ---- the bridge page's failure codes ---------------------------------------

void BridgeErrorTests() {
  {
    TEST_CASE("theBridgePagesCodesReadInTheAppsWords");
    // the page's code (urnet::SolanaWalletBridgeError*), the store key, and
    // whether the string names the wallet (Phantom or Solflare)
    struct Expected {
      const char* code;
      const char* key;
      bool takesWalletName;
    };
    for (const Expected& c : {
             Expected{"extension_not_found", "bittensor_error_extension_not_found", true},
             Expected{"no_account", "bittensor_error_no_account", true},
             Expected{"session_not_found", "solana_wallet_error_session_not_found", false},
             Expected{"user_rejected", "bittensor_error_user_rejected", false},
         }) {
      const BridgeErrorText text = BridgeErrorTextFor(c.code);
      CheckEq(c.key, text.key, std::string("the key for ") + c.code);
      Check(text.takesWalletName == c.takesWalletName,
            std::string("the wallet's name for ") + c.code);
    }
  }
  {
    TEST_CASE("anyOtherCodeShowsThePagesText");
    // the page's other failures, a code this app does not know, a page before
    // the codes (-1), and a wallet's own numeric code
    for (const char* code : {"invalid_request", "wallet_error", "wallet_locked", "-1", "4001", ""}) {
      CheckEq("", BridgeErrorTextFor(code).key, std::string("no key for \"") + code + "\"");
    }
  }
}

// ---- where a wallet-bridge return goes (WalletBridgeRoute.h) -----------------

std::string RouteName(urnw::bridge::PublicKeyRoute route) {
  switch (route) {
    case urnw::bridge::PublicKeyRoute::Drop: return "Drop";
    case urnw::bridge::PublicKeyRoute::SignForRequest: return "SignForRequest";
    case urnw::bridge::PublicKeyRoute::SignIn: return "SignIn";
  }
  return "?";
}

std::string RouteName(urnw::bridge::SignatureRoute route) {
  switch (route) {
    case urnw::bridge::SignatureRoute::Drop: return "Drop";
    case urnw::bridge::SignatureRoute::AnswerRequest: return "AnswerRequest";
    case urnw::bridge::SignatureRoute::SignIn: return "SignIn";
  }
  return "?";
}

void BridgeRouteTests() {
  using urnw::bridge::IsSuperseded;
  using urnw::bridge::RoutePublicKey;
  using urnw::bridge::RouteSignature;
  {
    TEST_CASE("aConnectReturnNobodyWaitsForIsDropped");
    // the bridge page's "Return to URnetwork" after its flow already ended
    CheckEq("Drop", RouteName(RoutePublicKey(false, false, false)), "no flow in flight");
  }
  {
    TEST_CASE("aSignatureRequestSignsBeforeASignIn");
    CheckEq("SignForRequest", RouteName(RoutePublicKey(false, true, true)),
            "an add-sign-in request's own message is signed");
  }
  {
    TEST_CASE("aWalletSignInStillSignsIn");
    CheckEq("SignIn", RouteName(RoutePublicKey(false, false, true)),
            "the login page's Solana sign-in fetches its challenge");
  }
  {
    TEST_CASE("aBittensorConnectReturnChainsNothing");
    CheckEq("Drop", RouteName(RoutePublicKey(true, true, true)),
            "Bittensor has no connect hop, whatever is waiting");
  }
  {
    TEST_CASE("aSignatureNobodyWaitsForNeverSignsIn");
    CheckEq("Drop", RouteName(RouteSignature(false, false)),
            "a superseded tab's signature is dropped");
    CheckEq("AnswerRequest", RouteName(RouteSignature(true, true)),
            "a waiting request is answered before a sign-in");
    CheckEq("SignIn", RouteName(RouteSignature(false, true)), "a wallet sign-in logs in");
  }
  {
    TEST_CASE("everyPublicKeyCombinationRoutesByPriority");
    for (int bits = 0; bits < 8; ++bits) {
      const bool bittensor = (bits & 1) != 0;
      const bool sign = (bits & 2) != 0;
      const bool signIn = (bits & 4) != 0;
      const std::string expected = bittensor ? "Drop"
                                   : sign    ? "SignForRequest"
                                   : signIn  ? "SignIn"
                                             : "Drop";
      CheckEq(expected, RouteName(RoutePublicKey(bittensor, sign, signIn)),
              "combination " + std::to_string(bits));
    }
  }
  {
    TEST_CASE("supersededIsTheHostsOwnWord");
    Check(!IsSuperseded("User rejected the request."), "the bridge's own error is not");
    Check(!IsSuperseded(""), "an empty error is not");
    // reasons SdkHost::CancelPendingWalletFlows passes (TestWalletBridgeReturnsAreRouted
    // holds every one of them to the prefix)
    for (const char* reason :
         {"superseded by a wallet sign-in", "superseded by a wallet signature request",
          "superseded by wallet network creation", "superseded by a sign-in"}) {
      Check(IsSuperseded(reason), std::string("\"") + reason + "\"");
    }
    Check(!IsSuperseded("Superseded by a sign-in"), "the prefix is exact");
  }
  {
    TEST_CASE("aSupersededFlowsChallengeNeverOpensTheBridge");
    urnw::bridge::FlowSerial flows;
    // the page's Bittensor connect starts and fetches its challenge
    const uint64_t bittensor = flows.Start();
    Check(flows.IsCurrent(bittensor), "its challenge may open the bridge while nothing else started");
    // meanwhile adding a Solana sign-in method takes the bridge
    const uint64_t solana = flows.Start();
    Check(!flows.IsCurrent(bittensor),
          "the late Bittensor challenge is stale: no tab, no message, no session reset");
    Check(flows.IsCurrent(solana), "the Solana signature request still owns the bridge");
    CheckEq(static_cast<long long>(solana), static_cast<long long>(flows.Current()),
            "Current is the flow that started last");
  }
  {
    TEST_CASE("aSignInsChallengeBelongsToTheFlowItWasRoutedUnder");
    urnw::bridge::FlowSerial flows;
    flows.Start();  // SignInWithSolana starts and opens Phantom
    const uint64_t routed = flows.Current();  // on_public_key routes to SignIn, fetches the challenge
    Check(flows.IsCurrent(routed), "the challenge may ask for a signature while the sign-in owns the bridge");
    flows.Start();  // a Google sign-in starts meanwhile
    Check(!flows.IsCurrent(routed), "the late Solana challenge must not open a signing tab");
  }
  {
    TEST_CASE("flowNumbersNeverRepeat");
    urnw::bridge::FlowSerial flows;
    uint64_t last = flows.Current();
    bool increasing = true;
    for (int i = 0; i < 1000; ++i) {
      const uint64_t next = flows.Start();
      if (next <= last) increasing = false;
      last = next;
    }
    Check(increasing, "every start takes a new, larger number");
    Check(flows.IsCurrent(last) && !flows.IsCurrent(last - 1), "only the last one is current");
  }
}

}  // namespace

int main() {
  std::cout << "the bridge page's failure codes\n";
  BridgeErrorTests();
  std::cout << "wallet bridge routing\n";
  BridgeRouteTests();

  std::cout << "\n" << gCases << " cases, " << gFailures << " failures\n";
  return gFailures == 0 ? 0 : 1;
}
