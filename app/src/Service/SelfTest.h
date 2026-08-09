// `urnetworkd selftest` — the unit tests that can run on a machine where the
// thing under test cannot.
//
// The WFP leak-prevention layer needs elevation to install a single filter, so
// on an unprivileged box there is no way to prove that a filter blocks
// anything. What CAN be proved without elevation, and is proved here, is
// everything upstream of the syscall: that the route set and the firewall's LAN
// permit are the same decision (NetPolicy.h), that BuildFilterSet emits the
// right filters in the right sublayers at the right weights for each state, and
// that the states differ from each other only where they are supposed to.
//
// Nothing in here opens the filter engine, creates an adapter, writes a route
// or contacts BFE. It is safe to run unelevated, and it is structurally unable
// to change the machine — which is the only reason it is wired into the service
// binary rather than a separate harness.
//
// A gate this does not cover is not a pass: see the leak-validation section of
// docs/superpowers/reports/p7-baseline/p7-gates.ps1 for the half that needs the
// owner's elevated session.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

namespace urnw {

// Returns 0 when every check passes, 1 otherwise. Prints one line per check.
int RunSelfTest();

}  // namespace urnw
