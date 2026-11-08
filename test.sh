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
offline_admission="${URNETWORK_WINDOWS_HOST_OFFLINE_ADMISSION:-}"
if [ -n "$offline_admission" ]; then
  if [ "$offline_admission" != windows-host-darwin-network-denied-v1 ] ||
     [ "$(uname -s)" != Darwin ] || [ "${GOPROXY:-}" != off ] ||
     [ "${GOSUMDB:-}" != off ] || [ "${GOVCS:-}" != '*:off' ] ||
     [ "${GOTOOLCHAIN:-}" != local ] || [ "${GOWORK:-}" != off ]; then
    echo 'Windows offline-host admission scope or offline dependency policy is invalid' >&2
    exit 70
  fi
  # The marker selects the lane; only actual parent+child OS denial admits it.
  python3 "$root/tests/windows-host-network-deny.py"
else
  if [ "${URNETWORK_NETWORK_TEST_LOCK_HELD:-}" != 1 ]; then
    exec "$network_test_gate" run-all run-all-windows-host -- \
      "$here/test.sh" "$@"
  fi
  if ! "$network_test_gate" --verify-held run-all; then
    echo "Windows host test suite inherited an invalid network-intensive lock" >&2
    exit 70
  fi
fi

# The complete C++ specification and mutation census shares one package alarm.
# Declare its aggregate budget and two independent compiler slots. The host
# owner grants two cores; inherited -parallel=1 would serialize every
# mutation table. Caller flags still override both defaults.
(cd "$here" && go test -timeout 30m -parallel 2 "$@" ./tests)
# the insufficient-balance acceptance driver (test-insufficient-balance-driver)
(cd "$here" && go test "$@" ./tests/insufficientbalance)
(cd "$root/build/all/windows" && go test "$@" ./...)

# License drift: the Go modules and vendored libraries (sdk/licenses/extra.yml)
# the windows app ships must match sdk/license.yml, which the SDK embeds and the
# app's Settings -> Licenses page shows. It runs here, on the macOS build host,
# where run.sh runs this script with the sdk sibling checked out beside this
# repo and the SDK's Go module cache already warm. Not in app/build.ps1: the
# app build inside the Windows VM uses no Go (only build-sdk.ps1 does), and a
# drift found there would cost a VM boot, a sync and an SDK build first. -check
# compares entry identities (origin, name, version, apps), never the license
# texts, so it needs no network.
(cd "$here" && go -C "$root/sdk" run ./licenses -check windows)
