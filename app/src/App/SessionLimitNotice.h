// A sign-in the account's cap of active sessions refused, as the sign-in
// screens show it. A route that would start one more session refuses at the
// cap, and the sdk reads the server's refusal as the route's RESULT, its error
// code SessionErrorCodeLimitReached and its message the sdk's English (sdk
// api_session_limit.go): authLogin (Google, Apple, a wallet, a recovery
// phrase, and the account discovery), authLoginWithPassword, authVerify,
// networkCreate (a new network and an instant account), authCodeLogin (an auth
// code and the browser sign-in's approval), and the device registration's
// authNetworkClient. This device is not signed in, so nothing on these screens
// frees a session: the line sends the user to a device that is
// (sessions_limit_reached_sign_in), in place of the server's message, a
// generic error or a wrong one. Nothing retries and nothing signs out.
//
// SdkHost reads each of those results' errors through SignInErrorOf, which
// hands the screens a token in place of the server's message, the way the
// client limit's refusal reaches them (ClientLimitNotice.h); the screens show
// the words for either token through SignInRefusalKey (LoginPage's
// MapAuthErrorForDisplay and its verify step, the browser sign-in sheet, and
// the device list a refused sign-in opens). Pure, with no WinRT or sdk header,
// so tools/sessions-tests.cpp runs it on any host, and over the generated
// header's own result structs when it has one.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <optional>
#include <string>

#include "ClientLimitNotice.h"

namespace urnw::sessionlimit {

// The sdk's spelling (sdk SessionErrorCodeLimitReached), here so this header
// needs no sdk header; tests/sessions_test.go holds it to urnetwork_sdk.hpp.
inline constexpr const char* kErrorCodeLimitReached = "session_limit_reached";

// The token SdkHost hands the sign-in screens in place of the server's
// message, through a sign-in's own answer and through the auth state relay.
// Never shown.
inline constexpr const char* kSignInRefused = "session-limit:sign-in";

// The words for it.
inline constexpr const char* kSignInRefusedMessageKey = "sessions_limit_reached_sign_in";

// The error code of a route's result is the session cap's refusal.
inline bool LimitReached(const std::optional<std::string>& code) {
  return code.has_value() && *code == kErrorCodeLimitReached;
}

// The error of a sign-in route's result, for each of the sdk's result errors
// (ApiError, AuthLoginResultError, NetworkCreateResultError,
// AuthNetworkClientError: an optional code and a message): the token for the
// session cap's refusal, else the server's message. "" for no error, or for
// one with no message, which every caller reads as no error, as before.
template <typename ResultError>
std::string SignInErrorOf(const std::optional<ResultError>& error) {
  if (!error) return std::string();
  if (LimitReached(error->code)) return std::string(kSignInRefused);
  return error->message;
}

// The store key for the session cap's token, or nullptr for any other error.
inline const char* SignInRefusalMessageKey(const std::string& error) {
  return error == kSignInRefused ? kSignInRefusedMessageKey : nullptr;
}

// The store key for either refusal token a sign-in can end with, the session
// cap's or the client limit's (clientlimit::SignInRefusalToken), or nullptr
// for any other error: what every sign-in surface shows in place of a token.
inline const char* SignInRefusalKey(const std::string& error) {
  if (const char* key = SignInRefusalMessageKey(error)) return key;
  return clientlimit::SignInRefusalMessageKey(error);
}

}  // namespace urnw::sessionlimit
