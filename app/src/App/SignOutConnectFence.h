// Which session work may go ahead once a sign-out has started. SdkHost::Logout
// clears the signed-in flag before it waits for the pass in flight, and from
// then no connect of the signed-out account reaches the sdk: a connect request
// is refused, and a pass that took one before the sign-out began skips its
// connect, which would save the account's location, default and provide mode
// in the network space's local state and start its tunnel beside the logout.
// Its disconnects still run, and so do a bare "make sure a session exists"
// and a provider reconcile, which a signed-out app declines by itself
// (BootstrapSession, ReconcileProviderLocked). The flag is the account's: the
// next sign-in sets it, so the fence never holds the next account.
//
// Pure, no Windows dependencies; tools/sign-out-connect-fence-tests.cpp is the
// spec.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

namespace urnw::signoutfence {

// What a session request or a pass asks of the sdk.
enum class Work { Connect, Disconnect, Ensure, Provider };

// Whether `work` may go ahead with the account signed in or not.
inline constexpr bool Admits(Work work, bool signedIn) {
  return signedIn || work != Work::Connect;
}

}  // namespace urnw::signoutfence
