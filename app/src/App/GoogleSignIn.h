// "Sign in with Google" through the SYSTEM BROWSER — the Windows peer of the
// macOS/iOS GoogleSignIn SDK path, without a native SSO SDK and without an
// embedded webview.
//
// OAuth 2.0 authorization code + PKCE with a LOOPBACK redirect, which is what
// RFC 8252 ("OAuth 2.0 for Native Apps") prescribes and the only redirect style
// Google accepts for a Desktop client:
//
//   1. bind 127.0.0.1:0 and keep the ephemeral port
//   2. open https://accounts.google.com/o/oauth2/v2/auth?... in the default
//      browser, with redirect_uri=http://127.0.0.1:<port>
//   3. accept ONE request on that socket, read `code` and `state`
//   4. POST the code + verifier to https://oauth2.googleapis.com/token
//   5. hand the resulting id_token to authLogin{auth_jwt_type:"google"}
//
// Why loopback and not the app's own urnetwork:// scheme: Google issues custom
// scheme redirects to iOS/Android client types only. A Desktop client that
// tries one is rejected at the authorization endpoint with invalid_request, so
// routing this through AppController::HandleDeepLink the way WalletConnect does
// would never receive a callback. The loopback socket is self-contained: no
// second process, no protocol registration, no bridge service.
//
// NO CLIENT ID IS COMPILED IN BY DEFAULT. Configured() is false then, the whole
// flow is unreachable, and the caller HIDES the button rather than offering one
// that cannot work. Inject it on the build the way the WalletConnect project id
// is injected (see Config.h):
//   msbuild ... /p:UrnGoogleOAuthClientId=<id> /p:UrnGoogleOAuthClientSecret=<secret>
// Google's "Desktop app" clients ship a client secret that is explicitly NOT
// confidential (it is in every copy of the binary); PKCE is what actually binds
// the code to this process.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>

namespace urnw {

class GoogleSignIn {
 public:
  ~GoogleSignIn();

  // Whether an OAuth client id was compiled into this build. False means the
  // flow cannot run and the sign-in affordance must not be offered.
  static bool Configured();

  // Run the round trip. `done` fires exactly once, on a BACKGROUND thread, with
  // either a non-empty id token or a non-empty error. Calling Start again while
  // one is in flight cancels the first.
  using Done = std::function<void(std::string idToken, std::string error)>;
  void Start(Done done);

  // Abandon an in-flight round trip: signals the attempt's cancel event, which
  // wakes the worker out of its wait, and drops the callback.
  //
  // It does NOT close the loopback socket. It used to, from this thread, while
  // the worker held the same SOCKET by value — and Windows recycles socket
  // descriptors, so the closed value could already be the NEXT attempt's
  // listener by the time the stale worker used it. See the note on Attempt.
  void Cancel();

 private:
  // One attempt's state, held by both the UI thread and the worker through a
  // shared_ptr. The listening socket is NOT in here: it belongs to the worker
  // alone, which is what keeps its lifetime unraceable.
  struct Attempt;
  std::shared_ptr<Attempt> attempt_;
};

}  // namespace urnw
