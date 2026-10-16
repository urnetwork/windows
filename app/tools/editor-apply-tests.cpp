// Executable spec for the settings editors' compare and set
// (App/EditorApply.h, owner report 2026-10-16): the DNS editor takes no Update
// over no reading, and an Update of either editor writes only while the
// settings are still the reading the editor opened on and that fresh reading
// is known; otherwise nothing is written and the editor re-seeds. Run on any
// host with a C++20 compiler:
//
//   c++ -std=c++20 -I ../src/App editor-apply-tests.cpp -o /tmp/ea && /tmp/ea
//
// SdkHost::ApplyDnsSettings and ApplyTransportSettings ask it under mutex_ on
// the write worker; those lines are pinned by tests/editor_apply_test.go.
//
// SPDX-License-Identifier: MPL-2.0

#include <iostream>
#include <optional>
#include <string>

#include "EditorApply.h"

namespace editorapply = urnw::editorapply;
using editorapply::Step;

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

// a resolver or a policy, by the one field that tells two apart here
struct Settings {
  std::string servers;
};

bool Equal(const Settings& a, const Settings& b) { return a.servers == b.servers; }

Step Decide(std::optional<Settings> opened, std::optional<Settings> current, bool known) {
  return editorapply::Decide(opened, current, known, Equal);
}

const Settings kReal{"203.0.113.53"};
const Settings kOther{"198.51.100.53"};

}  // namespace

int main() {
  // the DNS editor: a live device always holds resolver settings
  Check(!editorapply::DnsReadingKnown(std::optional<Settings>{}),
        "no DNS reading takes no Update: the form would overwrite the real resolver");
  Check(editorapply::DnsReadingKnown(std::optional<Settings>{kReal}), "a DNS reading takes an Update");
  Check(Decide(kReal, kReal, true) == Step::Write,
        "still the reading the editor opened on: the draft is written");
  Check(Decide(kReal, kOther, true) == Step::Reseed,
        "changed since the editor opened: nothing is written, the editor re-seeds");
  Check(Decide(kReal, std::nullopt, editorapply::DnsReadingKnown(std::optional<Settings>{})) ==
            Step::Reseed,
        "a fresh DNS read with nothing known writes nothing");
  Check(Decide(kReal, kReal, false) == Step::Reseed,
        "a fresh reading that is not known writes nothing, whatever it equals");

  // the transport editor: none can be the default policy in force
  Check(Decide(std::nullopt, std::nullopt, true) == Step::Write,
        "opened on the default, still the default in force: the draft is written");
  Check(Decide(std::nullopt, std::nullopt, false) == Step::Reseed,
        "none from a detached device is its fallback: nothing is written over it");
  Check(Decide(std::nullopt, kReal, true) == Step::Reseed,
        "opened on the default over a real policy: nothing is written, the editor re-seeds");
  Check(Decide(kReal, std::nullopt, true) == Step::Reseed,
        "the policy went back to the default since the editor opened: nothing is written");

  std::cout << (gCases - gFailures) << "/" << gCases << " editor apply checks passed\n";
  return gFailures == 0 ? 0 : 1;
}
