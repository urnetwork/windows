// Executable spec for adding a sign-in method (App/AddSignIn.h): Settings'
// add sheet offers the same options as ur.io and the other apps (Apple,
// Google, a Solana or Bittensor wallet, email or phone), and adding one only
// ever posts the credential to addAuth on the current network; it never signs
// in as the added identity. Run against the SAME header the app compiles, on
// any host with a C++20 compiler, with a fake session that holds each answer
// until the test releases it.
//
//   c++ -std=c++20 -I ../src/App add-sign-in-tests.cpp -o /tmp/add-sign-in-tests && /tmp/add-sign-in-tests
//
// SPDX-License-Identifier: MPL-2.0

#include <functional>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "AddSignIn.h"
#include "BittensorWalletFlow.h"

namespace asi = urnw::add_sign_in;

namespace {

int gFailures = 0;
int gCases = 0;

void Expect(const char* name, bool ok) {
  ++gCases;
  if (!ok) {
    ++gFailures;
    std::cerr << "FAIL " << name << "\n";
  }
}

// Records every call. The session has no way to sign in or to install a jwt,
// so `calls` is the whole of what an add can do.
class FakeSession : public asi::AddSignInSession {
 public:
  std::vector<std::string> calls;
  std::function<void(std::string, std::string)> pendingToken;
  std::function<void(asi::WalletSignature, std::string)> pendingWallet;
  std::function<void(std::string)> pendingAdd;
  asi::AddAuthBody lastBody;

  void ProviderToken(std::string_view provider,
                     std::function<void(std::string, std::string)> done) override {
    calls.push_back("providerToken " + std::string(provider));
    pendingToken = std::move(done);
  }
  void SignWallet(asi::WalletChain chain, std::string_view walletId,
                  std::function<void(asi::WalletSignature, std::string)> done) override {
    calls.push_back(std::string("signWallet ") +
                    std::string(asi::WalletChainBlockchain(chain)) + " " + std::string(walletId));
    pendingWallet = std::move(done);
  }
  void AddAuth(asi::AddAuthBody const& body, std::function<void(std::string)> done) override {
    calls.push_back("addAuth");
    lastBody = body;
    pendingAdd = std::move(done);
  }
  void Cancel() override { calls.push_back("cancel"); }
};

void TestOptions() {
  // every app's option set, in ur.io's order
  Expect("four methods", asi::kMethodCount == 4);
  Expect("apple first", asi::kMethods[0] == asi::Method::Apple);
  Expect("google second", asi::kMethods[1] == asi::Method::Google);
  Expect("wallet third", asi::kMethods[2] == asi::Method::Wallet);
  Expect("email last", asi::kMethods[3] == asi::Method::Email);
  Expect("opens on email", asi::kDefaultMethod == asi::Method::Email);
  Expect("two wallet chains", asi::kWalletChainCount == 2);
  Expect("solana first", asi::kWalletChains[0] == asi::WalletChain::Solana);
  Expect("bittensor second", asi::kWalletChains[1] == asi::WalletChain::Bittensor);
  Expect("labels", asi::MethodLabelKey(asi::Method::Apple) == "apple" &&
                       asi::MethodLabelKey(asi::Method::Google) == "google" &&
                       asi::MethodLabelKey(asi::Method::Wallet) == "wallet" &&
                       asi::MethodLabelKey(asi::Method::Email) == "site_app_email");
  Expect("chain labels", asi::WalletChainLabelKey(asi::WalletChain::Solana) == "solana_wallet" &&
                             asi::WalletChainLabelKey(asi::WalletChain::Bittensor) == "bittensor_wallet");
  Expect("chain hints",
         asi::WalletChainHintKey(asi::WalletChain::Solana) == "connect_solana_wallet_to_add_sign_in_method" &&
             asi::WalletChainHintKey(asi::WalletChain::Bittensor) ==
                 "connect_bittensor_wallet_to_add_sign_in_method");
  Expect("provider hints", asi::MethodHintKey(asi::Method::Apple) == "sign_in_with_your_apple_id_to" &&
                               asi::MethodHintKey(asi::Method::Google) == "sign_in_with_google_to_add_it");
  Expect("only email has a code", asi::NeedsVerification(asi::Method::Email) &&
                                      !asi::NeedsVerification(asi::Method::Apple) &&
                                      !asi::NeedsVerification(asi::Method::Google) &&
                                      !asi::NeedsVerification(asi::Method::Wallet));
  Expect("added lines", asi::AddedMessageKey(asi::Method::Apple) == "apple_sign_in_method_added" &&
                            asi::AddedMessageKey(asi::Method::Google) == "google_sign_in_method_added" &&
                            asi::AddedMessageKey(asi::Method::Wallet) == "wallet_sign_in_method_added");
  // the shared chooser's wallets are the Bittensor option's wallets
  Expect("bittensor add purpose", asi::kBittensorPurpose == "add" &&
                                      urnw::bittensor::kPurposeAdd == asi::kBittensorPurpose);
}

// A return from an add-owned browser attempt goes to addAuth, never authLogin.
void TestSsoReturnRoute() {
  Expect("add return adds", asi::RouteSsoReturn(asi::SsoPurpose::Add) == asi::SsoReturnRoute::AddAuth);
  Expect("login return logs in",
         asi::RouteSsoReturn(asi::SsoPurpose::SignIn) == asi::SsoReturnRoute::AuthLogin);
}

void TestProvider(asi::Method method, std::string_view provider) {
  FakeSession session;
  asi::AddSignInFlow flow(session);
  flow.StartProvider(method);
  Expect("provider: asks for the token", session.calls.size() == 1 &&
                                             session.calls[0] == "providerToken " + std::string(provider));
  Expect("provider: busy", flow.Busy());
  session.pendingToken("synthetic.id.token", "");
  Expect("provider: token goes to addAuth", session.calls.size() == 2 && session.calls[1] == "addAuth");
  Expect("provider: body", session.lastBody.auth_jwt == std::string("synthetic.id.token") &&
                               session.lastBody.auth_jwt_type == std::string(provider) &&
                               !session.lastBody.user_auth && !session.lastBody.wallet_auth);
  Expect("provider: not added before addAuth answers", !flow.Added());
  session.pendingAdd("");
  Expect("provider: added", flow.Added() == method && !flow.Busy());
  // nothing else: no sign-in, no session swap
  Expect("provider: only token and addAuth", session.calls.size() == 2);
}

void TestWallet(asi::WalletChain chain, std::string_view walletId, std::string_view blockchain) {
  FakeSession session;
  asi::AddSignInFlow flow(session);
  flow.StartWallet(chain, walletId);
  Expect("wallet: signs a challenge",
         session.calls.size() == 1 &&
             session.calls[0] == "signWallet " + std::string(blockchain) + " " + std::string(walletId));
  session.pendingWallet({"synthetic-address", "0xsynthetic-signature", "Sign in to URnetwork\nChallenge: c\nTimestamp: 1"}, "");
  Expect("wallet: body", session.lastBody.wallet_auth &&
                             session.lastBody.wallet_auth->blockchain == blockchain &&
                             session.lastBody.wallet_auth->address == "synthetic-address" &&
                             session.lastBody.wallet_auth->signature == "0xsynthetic-signature" &&
                             session.lastBody.wallet_auth->message ==
                                 "Sign in to URnetwork\nChallenge: c\nTimestamp: 1" &&
                             !session.lastBody.auth_jwt && !session.lastBody.user_auth);
  session.pendingAdd("");
  Expect("wallet: added", flow.Added() == asi::Method::Wallet);
  Expect("wallet: only sign and addAuth", session.calls.size() == 2);
}

void TestErrors() {
  {
    FakeSession session;
    asi::AddSignInFlow flow(session);
    flow.StartProvider(asi::Method::Google);
    session.pendingToken("", "superseded by the add sheet closing");
    Expect("quiet cancel shows nothing", flow.Error().empty() && flow.ErrorKey().empty() && !flow.Busy());
    Expect("quiet cancel adds nothing", session.calls.size() == 1 && !flow.Added());
  }
  {
    FakeSession session;
    asi::AddSignInFlow flow(session);
    flow.StartWallet(asi::WalletChain::Bittensor, "talisman");
    session.pendingWallet({}, "Rejected in the wallet");
    Expect("wallet error shown", flow.Error() == "Rejected in the wallet" && !flow.Added());
  }
  {
    FakeSession session;
    asi::AddSignInFlow flow(session);
    flow.StartProvider(asi::Method::Apple);
    session.pendingToken("", "");
    Expect("no token: generic line", flow.Error().empty() &&
                                         flow.ErrorKey() == asi::AddSignInFlow::kGenericErrorKey);
  }
  {
    FakeSession session;
    asi::AddSignInFlow flow(session);
    flow.StartProvider(asi::Method::Google);
    session.pendingToken("synthetic.id.token", "");
    session.pendingAdd("This sign-in is already used by another network");
    Expect("addAuth refusal shown, not added",
           flow.Error() == "This sign-in is already used by another network" && !flow.Added() && !flow.Busy());
  }
  {
    // cancelled while the browser is open: the late token is dropped
    FakeSession session;
    asi::AddSignInFlow flow(session);
    flow.StartProvider(asi::Method::Google);
    auto late = session.pendingToken;
    flow.Cancel();
    late("synthetic.id.token", "");
    Expect("cancel tells the session", session.calls.size() == 2 && session.calls[1] == "cancel");
    Expect("late token after cancel is dropped", !flow.Added() && !flow.Busy());
  }
  {
    // one at a time
    FakeSession session;
    asi::AddSignInFlow flow(session);
    flow.StartProvider(asi::Method::Google);
    flow.StartWallet(asi::WalletChain::Solana, asi::kSolanaPhantom);
    Expect("second start while busy ignored", session.calls.size() == 1);
  }
}

void TestBodies() {
  Expect("sso body supplies", asi::SuppliesMethod(asi::SsoBody("google", "t")));
  Expect("empty token does not", !asi::SuppliesMethod(asi::SsoBody("google", "")));
  Expect("wallet body supplies",
         asi::SuppliesMethod(asi::WalletBody(asi::WalletChain::Solana, "a", "s", "m")));
  Expect("wallet with no signature does not",
         !asi::SuppliesMethod(asi::WalletBody(asi::WalletChain::Solana, "a", "", "m")));
  Expect("empty body does not", !asi::SuppliesMethod(asi::AddAuthBody{}));
}

}  // namespace

int main() {
  TestOptions();
  TestSsoReturnRoute();
  TestProvider(asi::Method::Apple, "apple");
  TestProvider(asi::Method::Google, "google");
  TestWallet(asi::WalletChain::Solana, asi::kSolanaPhantom, "SOL");
  TestWallet(asi::WalletChain::Solana, asi::kSolanaSolflare, "SOL");
  for (int i = 0; i < urnw::bittensor::kChooserWalletCount; ++i) {
    TestWallet(asi::WalletChain::Bittensor, urnw::bittensor::kChooserWallets[i], "TAO");
  }
  TestErrors();
  TestBodies();
  if (gFailures) {
    std::cerr << gFailures << " of " << gCases << " add sign-in checks failed\n";
    return 1;
  }
  std::cout << gCases << " add sign-in checks passed\n";
  return 0;
}
