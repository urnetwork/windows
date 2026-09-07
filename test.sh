#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0
#
# Run host-portable Windows and Windows-build contract tests. Guest PowerShell
# helper tests remain owned by the ordinary/default Windows release build because
# their native process and path assertions require Windows.

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="${URNETWORK_ROOT:-$(dirname "$here")}"
readonly root
network_test_gate="$root/tests/network-intensive-suite-lock.sh"
if [ ! -x "$network_test_gate" ]; then
  echo "Windows host test suite gate is missing or not executable: $network_test_gate" >&2
  exit 127
fi
if [ "${URNETWORK_NETWORK_TEST_LOCK_HELD:-}" != 1 ]; then
  exec "$network_test_gate" run-all run-all-windows-host -- \
    "$here/test.sh" "$@"
fi
if ! "$network_test_gate" --verify-held run-all; then
  echo "Windows host test suite inherited an invalid network-intensive lock" >&2
  exit 70
fi

(cd "$here" && go test "$@" ./tests)
(cd "$root/build/all/windows" && go test "$@" ./...)
