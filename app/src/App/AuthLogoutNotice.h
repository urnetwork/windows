// What the sign-in page says after the server ended this app's sign-in, and
// the one sign-out each rejection takes (server session/REVOKE-UI-FINAL.md §5).
//
// The sdk reports a confirmed rejection of the stored sign-in as an
// AuthLogout, with no arguments, on the Api (SdkHost::BindApiLocked) and, while
// a session is up, on the DeviceRemote too: the device is built on the same
// Api and reports the same rejection after it. Each listener reads the sdk's
// cause before it marshals anything (AuthLogoutCause.h): "session_revoked"
// when the server's structured refusal said another device signed this
// session out, "" for every other rejection and for any sign-out this app
// made itself (the sdk withholds the cause from a revoke of this session from
// the Sessions page, and the app's own Sign out rejects nothing).
//
// On the UI thread AppController asks SignedOutNotice whether a report signs
// the app out. Only a signed-in app does, so the second report of one
// rejection, or a report behind the user's own sign-out, finds the app
// signed out already and changes nothing. The sign-in page takes the notice
// when it shows, once; a sign-in forgets one it never showed.
//
// Pure, for the reason SessionsPresentation.h gives: tools/sessions-tests.cpp
// verifies it on any host.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <string_view>
#include <utility>

namespace urnw::authlogout {

// urnet::AuthLogoutCauseSessionRevoked (URNET_AUTH_LOGOUT_CAUSE_SESSION_REVOKED)
inline constexpr std::string_view kCauseSessionRevoked = "session_revoked";

enum class Notice {
  None,               // any other cause, "" among them: nothing new is said
  SignedOutRemotely,  // "This session was signed out from another device."
};

constexpr Notice NoticeFor(std::string_view cause) {
  return cause == kCauseSessionRevoked ? Notice::SignedOutRemotely : Notice::None;
}

// The store key of a notice, "" for none.
constexpr const char* NoticeKey(Notice notice) {
  return notice == Notice::SignedOutRemotely ? "sessions_signed_out_remotely" : "";
}

// UI thread only.
class SignedOutNotice {
 public:
  // A report of the server's rejection, with the cause its listener read.
  // True when the app signs out for it now; false when the app is signed out
  // already, and then the report leaves no notice either.
  bool Rejected(bool signedIn, std::string_view cause) {
    if (!signedIn) return false;
    pending_ = NoticeFor(cause);
    return true;
  }
  // The sign-in page is showing: what it says, once.
  Notice Take() { return std::exchange(pending_, Notice::None); }
  // Signed in again: a sign-out the page never showed is not explained later.
  void Clear() { pending_ = Notice::None; }

 private:
  Notice pending_ = Notice::None;
};

}  // namespace urnw::authlogout
