// Executable spec for a value seeded by a read off the UI thread and pushed
// by its listener (App/SeedOrder.h, owner report 2026-10-16): a push that
// lands after the read was asked for wins over the read's answer, and a read
// asked after the push applies. Run on any host with a C++20 compiler:
//
//   c++ -std=c++20 -I ../src/App seed-order-tests.cpp -o /tmp/so && /tmp/so
//
// The connect page's wiring is pinned by tests/seed_order_test.go.
//
// SPDX-License-Identifier: MPL-2.0

#include <iostream>
#include <string>

#include "SeedOrder.h"

using urnw::SeedOrder;

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
  {
    SeedOrder order;
    order.Asked();
    Check(order.Admits(), "a read with no push since it was asked applies");
  }
  {
    SeedOrder order;
    order.Asked();
    order.Pushed();
    Check(!order.Admits(), "a push after the request is newer than the read: the read is dropped");
  }
  {
    SeedOrder order;
    order.Pushed();
    order.Asked();
    Check(order.Admits(), "a push before the request is in the read: the read applies");
  }
  {
    SeedOrder order;
    order.Asked();
    order.Pushed();
    order.Asked();
    Check(order.Admits(), "coalesced requests: the read answers the last of them");
  }
  {
    SeedOrder order;
    Check(order.Admits(), "nothing asked and nothing pushed: a read applies");
  }
  std::cout << (gCases - gFailures) << "/" << gCases << " seed order checks passed\n";
  return gFailures == 0 ? 0 : 1;
}
