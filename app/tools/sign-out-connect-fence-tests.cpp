// Executable spec for the sign-out connect fence (App/SignOutConnectFence.h,
// owner report 2026-10-16): once a sign-out starts no connect of the
// signed-out account reaches the sdk, its disconnects and the work a
// signed-out app declines by itself still run, and the next sign-in is not
// held. Run on any host with a C++20 compiler:
//
//   c++ -std=c++20 -I ../src/App sign-out-connect-fence-tests.cpp -o /tmp/socf && /tmp/socf
//
// SdkHost asks it in RequestSession and before a pass's connect; those lines
// are pinned by tests/sign_out_connect_fence_test.go.
//
// SPDX-License-Identifier: MPL-2.0

#include <iostream>
#include <string>

#include "SignOutConnectFence.h"

using urnw::signoutfence::Admits;
using urnw::signoutfence::Work;

namespace {

int gFailures = 0;
int gCases = 0;

void Check(bool condition, const std::string& what) {
  ++gCases;
  if (!condition) {
    ++gFailures;
    std::cout << "  FAIL " << what << "\n";
  }
}

}  // namespace

int main() {
  Check(!Admits(Work::Connect, /*signedIn=*/false),
        "a connect is refused once the sign-out has started");
  Check(Admits(Work::Disconnect, false), "a disconnect of the signed-out account still runs");
  Check(Admits(Work::Ensure, false),
        "a bare ensure still runs: the bootstrap declines a signed-out app itself");
  Check(Admits(Work::Provider, false),
        "a provider reconcile still runs: it stops a signed-out app's provider");
  for (Work work : {Work::Connect, Work::Disconnect, Work::Ensure, Work::Provider}) {
    Check(Admits(work, /*signedIn=*/true), "the next sign-in is not held by the fence");
  }
  std::cout << (gCases - gFailures) << "/" << gCases << " sign-out connect fence checks passed\n";
  return gFailures == 0 ? 0 : 1;
}
