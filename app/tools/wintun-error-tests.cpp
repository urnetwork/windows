// Executable spec for the wintun failure wording (Service/WintunError.h): the
// message a failed tunnel start shows the user carries the Windows error code
// and, where the code is unambiguous, its cause - run against the SAME header
// the service compiles, on any host with a C++20 compiler.
//
//   c++ -std=c++20 -I ../src/Service wintun-error-tests.cpp
//       -o /tmp/wintun-error-tests && /tmp/wintun-error-tests
//
// SPDX-License-Identifier: MPL-2.0

#include <iostream>
#include <string>

#include "WintunError.h"

using namespace urnw::wintun_error;

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

void CheckContains(const std::string& haystack, const std::string& needle,
                   const std::string& what) {
  Check(haystack.find(needle) != std::string::npos,
        what + ": \"" + needle + "\" not in \"" + haystack + "\"");
}

}  // namespace

int main() {
  CheckContains(CodeText(577), "Windows error 577 (0x241)", "code in decimal and hex");

  // ---- adapter creation: the report's case ----
  const std::string denied = AdapterFailure(5);
  CheckContains(denied, "failed to create the wintun adapter", "adapter prefix kept");
  CheckContains(denied, "Windows error 5 (0x5)", "access denied code shown");
  CheckContains(denied, "LocalSystem", "access denied names the service account");

  const std::string hash = AdapterFailure(577);
  CheckContains(hash, "Windows error 577 (0x241)", "invalid image hash code shown");
  CheckContains(hash, "signature", "invalid image hash names the driver signature");

  const std::string blocked = AdapterFailure(1275);
  CheckContains(blocked, "Windows error 1275 (0x4FB)", "driver blocked code shown");
  CheckContains(blocked, "blocked the wintun driver", "driver blocked cause");

  const std::string other = AdapterFailure(0xE0000247u);
  CheckContains(other, "Windows error 3758096967 (0xE0000247)", "an unmapped code is still shown");
  CheckContains(other, "service log", "an unmapped code points at the log");

  // ---- loading wintun.dll ----
  const std::string missing = LoadFailure(126);
  CheckContains(missing, "failed to load wintun.dll", "load prefix kept");
  CheckContains(missing, "Windows error 126 (0x7E)", "module not found code shown");
  CheckContains(missing, "missing next to urnetworkd.exe", "module not found cause");
  CheckContains(LoadFailure(193), "different CPU architecture", "bad exe format cause");
  CheckContains(LoadFailure(127), "Windows error 127 (0x7F)", "missing export code shown");

  std::cout << (gFailures == 0 ? "PASS" : "FAIL") << " wintun-error-tests: " << gCases
            << " checks, " << gFailures << " failures\n";
  return gFailures == 0 ? 0 : 1;
}
