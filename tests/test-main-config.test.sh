#!/usr/bin/env bash
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=../test-main-config.sh
source "$here/../test-main-config.sh"

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

[ "$(windows_acceptance_build_timeout)" = 7200 ] || fail "default build timeout is not two hours"
[ "$(UR_ACCEPT_WINDOWS_BUILD_TIMEOUT=9000 windows_acceptance_build_timeout)" = 9000 ] || fail "override was not honored"
if UR_ACCEPT_WINDOWS_BUILD_TIMEOUT=invalid windows_acceptance_build_timeout >/dev/null 2>&1; then
  fail "invalid build timeout was accepted"
fi
if UR_ACCEPT_WINDOWS_BUILD_TIMEOUT=0 windows_acceptance_build_timeout >/dev/null 2>&1; then
  fail "zero build timeout was accepted"
fi

echo "PASS: Windows acceptance build timeout"
