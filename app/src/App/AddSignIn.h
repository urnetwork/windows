// Adding a sign-in method to the signed-in network (Settings' AddAuthSheet):
// the same options every app and ur.io offer (ur.io AddSignInSheet, apple
// AddAuthSheet), in the same order: Apple, Google, a wallet (Solana or
// Bittensor), and an email or phone.
//
// - Email or phone + password: GuestConversion.h (addAuth, then the emailed
//   code; added once the code is verified).
// - Apple and Google: the provider's web flow in the browser, the same one the
//   login page runs (SdkHost::SignInWithSso), but the attempt is owned by the
//   add sheet (SsoPurpose::Add): its urnetwork://oauth/<provider> return goes
//   to addAuth{auth_jwt, auth_jwt_type}, never to authLogin.
// - Solana: a fresh /auth/wallet-challenge signed through the ur.io wallet
//   bridge (Phantom or Solflare), as a bare signature request, then
//   addAuth{wallet_auth{SOL}}.
// - Bittensor: the shared chooser's wallets (BittensorWalletFlow.h) and the
//   sdk session helper under its "add" purpose, then addAuth{wallet_auth{TAO}}.
//   A login session refuses an "add" hand-back and vice versa.
//
// Adding never signs in: the flow below only ever asks the session for a
// credential and posts it to addAuth on the current network. Nothing here can
// install a jwt, sign out, or move the auth state; Google, Apple and wallets
// are verified by the provider or the signature, so they have no code step.
//
// Header-only and free of WinRT and the SDK, so tools/add-sign-in-tests.cpp
// runs it with a fake session. Not thread safe: the session delivers every
// answer on the UI thread.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace urnw::add_sign_in {

enum class Method { Apple, Google, Wallet, Email };

// The sheet's options, in order (ur.io ADD_SIGN_IN_METHODS).
inline constexpr Method kMethods[] = {Method::Apple, Method::Google, Method::Wallet, Method::Email};
inline constexpr int kMethodCount = 4;
// The sheet opens on email (ur.io DEFAULT_ADD_SIGN_IN_METHOD).
inline constexpr Method kDefaultMethod = Method::Email;

enum class WalletChain { Solana, Bittensor };

// The wallet option's chains, in order (ur.io ADD_SIGN_IN_WALLET_CHAINS).
inline constexpr WalletChain kWalletChains[] = {WalletChain::Solana, WalletChain::Bittensor};
inline constexpr int kWalletChainCount = 2;

// The Solana wallets the bridge drives (the login page's Phantom / Solflare).
inline constexpr std::string_view kSolanaPhantom = "phantom";
inline constexpr std::string_view kSolanaSolflare = "solflare";

// The sdk session helper's add purpose (urnet::BittensorWalletPurposeAdd).
inline constexpr std::string_view kBittensorPurpose = "add";

// the sdk's chain names (urnet::SOL, urnet::TAO)
inline constexpr std::string_view kBlockchainSolana = "SOL";
inline constexpr std::string_view kBlockchainBittensor = "TAO";

inline std::string_view MethodLabelKey(Method method) {
  switch (method) {
    case Method::Apple:
      return "apple";
    case Method::Google:
      return "google";
    case Method::Wallet:
      return "wallet";
    case Method::Email:
      return "site_app_email";
  }
  return "";
}

// The explanation under a provider method ("" for wallet and email).
inline std::string_view MethodHintKey(Method method) {
  switch (method) {
    case Method::Apple:
      return "sign_in_with_your_apple_id_to";
    case Method::Google:
      return "sign_in_with_google_to_add_it";
    default:
      return "";
  }
}

// The provider button's label ("" for wallet and email).
inline std::string_view ProviderButtonKey(Method method) {
  switch (method) {
    case Method::Apple:
      return "sign_in_with_apple";
    case Method::Google:
      return "sign_in_with_google";
    default:
      return "";
  }
}

// The auth_jwt_type and the SdkHost sso provider ("" for wallet and email).
inline std::string_view SsoProvider(Method method) {
  switch (method) {
    case Method::Apple:
      return "apple";
    case Method::Google:
      return "google";
    default:
      return "";
  }
}

inline std::string_view WalletChainLabelKey(WalletChain chain) {
  return chain == WalletChain::Solana ? "solana_wallet" : "bittensor_wallet";
}

inline std::string_view WalletChainHintKey(WalletChain chain) {
  return chain == WalletChain::Solana ? "connect_solana_wallet_to_add_sign_in_method"
                                      : "connect_bittensor_wallet_to_add_sign_in_method";
}

inline std::string_view WalletChainBlockchain(WalletChain chain) {
  return chain == WalletChain::Solana ? kBlockchainSolana : kBlockchainBittensor;
}

// The line shown once the method is added (ur.io addedMessageKey).
inline std::string_view AddedMessageKey(Method method) {
  switch (method) {
    case Method::Apple:
      return "apple_sign_in_method_added";
    case Method::Google:
      return "google_sign_in_method_added";
    case Method::Wallet:
      return "wallet_sign_in_method_added";
    case Method::Email:
      return "sign_in_method_added_successfully";
  }
  return "";
}

// Only an email or phone has a code to verify.
inline bool NeedsVerification(Method method) { return method == Method::Email; }

// Who started a Google / Apple browser attempt, and so who its
// urnetwork://oauth/<provider> return belongs to. A return from an attempt the
// add sheet started must never sign in: it goes to addAuth on the current
// network. A return from a login attempt goes to authLogin, as before.
enum class SsoPurpose { SignIn, Add };
enum class SsoReturnRoute { AuthLogin, AddAuth };

constexpr SsoReturnRoute RouteSsoReturn(SsoPurpose purpose) noexcept {
  return purpose == SsoPurpose::Add ? SsoReturnRoute::AddAuth : SsoReturnRoute::AuthLogin;
}

// The /auth/add-auth body (urnet::AddAuthArgs): exactly one of
// {user_auth, password}, {auth_jwt, auth_jwt_type} or {wallet_auth}.
struct AddAuthBody {
  std::optional<std::string> user_auth;
  std::optional<std::string> password;
  std::optional<std::string> auth_jwt;
  std::optional<std::string> auth_jwt_type;
  struct Wallet {
    std::string blockchain;
    std::string address;
    std::string signature;
    std::string message;
  };
  std::optional<Wallet> wallet_auth;
};

inline AddAuthBody SsoBody(std::string_view provider, std::string idToken) {
  AddAuthBody body;
  body.auth_jwt = std::move(idToken);
  body.auth_jwt_type = std::string(provider);
  return body;
}

inline AddAuthBody WalletBody(WalletChain chain, std::string address, std::string signature,
                              std::string message) {
  AddAuthBody body;
  body.wallet_auth = AddAuthBody::Wallet{std::string(WalletChainBlockchain(chain)),
                                         std::move(address), std::move(signature),
                                         std::move(message)};
  return body;
}

// Whether the server's AddAuth accepts `body` (server model AddAuth); it
// answers anything else with "no auth method supplied".
inline bool SuppliesMethod(AddAuthBody const& body) {
  if (body.user_auth && !body.user_auth->empty() && body.password && !body.password->empty())
    return true;
  if (body.auth_jwt && !body.auth_jwt->empty() && body.auth_jwt_type && !body.auth_jwt_type->empty())
    return true;
  return body.wallet_auth && !body.wallet_auth->address.empty() &&
         !body.wallet_auth->signature.empty() && !body.wallet_auth->message.empty();
}

// A wallet's answer to a fresh challenge.
struct WalletSignature {
  std::string address;
  std::string signature;
  // the challenge text, byte for byte
  std::string message;
};

// What the flow needs from the app. Every `done` runs on the UI thread. An
// error that starts with "superseded by " (WalletBridgeRoute.h) is a quiet
// cancel: another flow took over.
class AddSignInSession {
 public:
  virtual ~AddSignInSession() = default;
  // the provider's identity token from an add-owned browser attempt
  virtual void ProviderToken(std::string_view provider,
                             std::function<void(std::string idToken, std::string error)> done) = 0;
  // a fresh /auth/wallet-challenge for `chain`, signed by `walletId`
  // (phantom / solflare, or a Bittensor chooser id)
  virtual void SignWallet(WalletChain chain, std::string_view walletId,
                          std::function<void(WalletSignature signature, std::string error)> done) = 0;
  // Api addAuth on the current network; "" on success, else the message.
  virtual void AddAuth(AddAuthBody const& body, std::function<void(std::string error)> done) = 0;
  // abandon the provider or wallet step in flight
  virtual void Cancel() = 0;
};

inline bool IsQuietCancel(std::string_view error) {
  return error.starts_with("superseded by ");
}

// Apple, Google and the wallets: credential -> addAuth -> added. The email
// method is GuestConversion's.
class AddSignInFlow {
 public:
  explicit AddSignInFlow(AddSignInSession& session) : session_(session) {}
  ~AddSignInFlow() { *alive_ = false; }
  AddSignInFlow(AddSignInFlow const&) = delete;
  AddSignInFlow& operator=(AddSignInFlow const&) = delete;

 private:
  // Drops an answer for an attempt that was cancelled or replaced, or that
  // arrives after the flow is gone.
  template <typename F>
  auto Guard(F f) {
    return [alive = alive_, attempt = attempt_, this, f = std::move(f)](auto&&... args) mutable {
      if (!*alive || attempt != attempt_) return;
      f(std::forward<decltype(args)>(args)...);
    };
  }

 public:
  std::function<void()> on_changed;

  bool Busy() const { return busy_; }
  // the server's or the wallet's message to show ("" for none)
  std::string const& Error() const { return error_; }
  // a store key to show when there is no message ("" for none)
  std::string const& ErrorKey() const { return errorKey_; }
  // the method that was added (set once addAuth succeeded)
  std::optional<Method> Added() const { return added_; }

  // Apple or Google.
  void StartProvider(Method method) {
    const std::string_view provider = SsoProvider(method);
    if (busy_ || added_ || provider.empty()) return;
    Begin();
    session_.ProviderToken(provider, Guard([this, method, provider = std::string(provider)](
                                               std::string idToken, std::string error) {
      if (!error.empty() || idToken.empty()) {
        Fail(error);
        return;
      }
      Submit(method, SsoBody(provider, std::move(idToken)));
    }));
  }

  void StartWallet(WalletChain chain, std::string_view walletId) {
    if (busy_ || added_ || walletId.empty()) return;
    Begin();
    session_.SignWallet(chain, walletId, Guard([this, chain](WalletSignature signature,
                                                             std::string error) {
      if (!error.empty() || signature.address.empty() || signature.signature.empty()) {
        Fail(error);
        return;
      }
      Submit(Method::Wallet, WalletBody(chain, std::move(signature.address),
                                        std::move(signature.signature),
                                        std::move(signature.message)));
    }));
  }

  // The user switched method or closed the sheet.
  void Cancel() {
    if (!busy_) return;
    ++attempt_;
    busy_ = false;
    session_.Cancel();
    Changed();
  }

 private:
  void Begin() {
    ++attempt_;
    busy_ = true;
    error_.clear();
    errorKey_.clear();
    Changed();
  }

  void Submit(Method method, AddAuthBody const& body) {
    if (!SuppliesMethod(body)) {
      Fail(std::string());
      return;
    }
    session_.AddAuth(body, Guard([this, method](std::string error) {
      if (!error.empty()) {
        Fail(error);
        return;
      }
      busy_ = false;
      added_ = method;
      Changed();
    }));
  }

  // A quiet cancel shows nothing; an empty error shows the generic line.
  void Fail(std::string const& error) {
    busy_ = false;
    error_.clear();
    errorKey_.clear();
    if (!IsQuietCancel(error)) {
      if (error.empty()) {
        errorKey_ = kGenericErrorKey;
      } else {
        error_ = error;
      }
    }
    Changed();
  }

  void Changed() {
    if (on_changed) on_changed();
  }

  AddSignInSession& session_;
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
  unsigned attempt_ = 0;
  bool busy_ = false;
  std::string error_;
  std::string errorKey_;
  std::optional<Method> added_;

 public:
  // shown when a step fails with no message of its own
  static constexpr const char* kGenericErrorKey = "something_went_wrong";
};

}  // namespace urnw::add_sign_in
