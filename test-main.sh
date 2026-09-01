#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0
#
# Product acceptance test for the LOCAL Windows app/service against production
# (main). It installs the freshly built ARM64 MSI in the repository's isolated
# Windows 11 QEMU VM, verifies the service's local SDK marker, performs instant-
# account login/logout/secret-key login, password login, a real provider tunnel
# with changed public egress, disconnect, and clean MSI removal.
#
# Usage:
#   ./test-main.sh                 build and run once
#   ./test-main.sh --repeat=5      repeat the full account+tunnel case five times
#   ./test-main.sh --skip-build    reuse UR_ACCEPT_WINDOWS_OUT artifacts
#   ./test-main.sh --headless      accepted for cross-platform runner parity
#   ./test-main.sh --keep-fixture  retain the recoverable account for another app
#
# Environment:
#   UR_ACCEPT_VAULT=<path>         alternate main acceptance credentials
#   UR_ACCEPT_FIXTURE=<path>       persistent private instant-account fixture
#   UR_ACCEPT_REPEAT=<n>           repetition count
#   UR_ACCEPT_KEEP_FIXTURE=1       retain the account after a successful run
#   UR_ACCEPT_WINDOWS_OUT=<path>   MSI build output cache
#   UR_ACCEPT_WINDOWS_BUILD_TIMEOUT=<seconds> local build deadline (default 7200)
#   EXTERNAL_WARP_VERSION=<v>      local artifact version (default 0.0.0-0)
#   WARP_VERSION=<v>               local SDK marker (derived when omitted)
set -euo pipefail
umask 077

here="$(cd "$(dirname "$0")" && pwd)"
root="${URNETWORK_ROOT:-$(dirname "$here")}"
vault="${UR_ACCEPT_VAULT:-$root/vault/main/tests.yml}"
fixture="${UR_ACCEPT_FIXTURE:-$here/tests/__acceptance__/fixtures/windows-main.secret}"
repeat_count="${UR_ACCEPT_REPEAT:-1}"
skip_build="${SKIP_BUILD:-0}"
keep_fixture="${UR_ACCEPT_KEEP_FIXTURE:-0}"
result_matrix="${UR_ACCEPT_RESULT_FILE:-}"
version="${EXTERNAL_WARP_VERSION:-0.0.0-0}"
out_dir="${UR_ACCEPT_WINDOWS_OUT:-$here/out/acceptance}"
# shellcheck source=test-main-config.sh
source "$here/test-main-config.sh"
build_timeout="$(windows_acceptance_build_timeout)" || exit $?

case "$version" in
  ''|*[!A-Za-z0-9.+-]*) echo "EXTERNAL_WARP_VERSION contains unsupported characters" >&2; exit 2 ;;
esac

for arg in "$@"; do
  case "$arg" in
    --repeat=*) repeat_count="${arg#*=}" ;;
    --skip-build) skip_build=1 ;;
    --headless) ;;
    --keep-fixture) keep_fixture=1 ;;
    -h|--help) grep '^#' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown argument: $arg" >&2; exit 2 ;;
  esac
done
case "$repeat_count" in
  ''|*[!0-9]*) echo "--repeat must be a positive integer" >&2; exit 2 ;;
  0) echo "--repeat must be at least 1" >&2; exit 2 ;;
esac

if [ -n "${WARP_VERSION:-}" ]; then
  sdk_version="$WARP_VERSION"
else
  case "$version" in
    *-*) sdk_version="${version%-*}+${version##*-}" ;;
    *) sdk_version="$version" ;;
  esac
fi
case "$sdk_version" in
  ''|*[!A-Za-z0-9.+-]*) echo "WARP_VERSION contains unsupported characters" >&2; exit 2 ;;
esac

die() { echo "[windows acceptance] ERROR: $*" >&2; exit 1; }
command -v timeout >/dev/null 2>&1 || die "GNU timeout is required (brew install coreutils)"
node "$root/build/all/acceptance/preflight-main.mjs" || exit 1
python3 "$here/tests/build-contract.test.py"
[ -f "$vault" ] || die "no acceptance vault at $vault"
config_reader="$root/tests/read-tests-config.sh"
[ -x "$config_reader" ] || die "test config reader is missing: $config_reader"
UR_ACCEPT_VAULT="$vault" "$config_reader" --ready validate
acc_user="$(UR_ACCEPT_VAULT="$vault" "$config_reader" get data_plane_account.email)"
acc_pass="$(UR_ACCEPT_VAULT="$vault" "$config_reader" get data_plane_account.password)"

timestamp="$(date +%Y%m%d-%H%M%S)"
artifacts="$here/tests/__acceptance__/$timestamp"
run_dir="$(mktemp -d "${TMPDIR:-/tmp}/urnetwork-windows-acceptance.XXXXXX")"
mkdir -p "$artifacts" "$(dirname "$fixture")" "$out_dir"
chmod 700 "$run_dir" "$(dirname "$fixture")"
credentials="$run_dir/credentials"
printf '%s\n%s\n' "$acc_user" "$acc_pass" >"$credentials"
chmod 600 "$credentials"
tests_json="$run_dir/tests.json"
UR_ACCEPT_VAULT="$vault" "$config_reader" write-json "$tests_json"
unset acc_pass

# Use the same VM lifecycle implementation as the product build.
# shellcheck source=../build/all/windows/lib.sh
source "$root/build/all/windows/lib.sh"
win_init
win_ensure_ssh_key
[ -f "$IMAGE" ] || die "Windows VM image is missing; run $root/build/all/windows/setup.sh"

acceptance_scp_to() {
  local source="$1" destination="$2"
  timeout --signal=TERM --kill-after=10s 300 \
    scp -i "$SSH_KEY" -P "$SSH_PORT" "${WIN_SSH_OPTS[@]}" \
    "$source" "builder@127.0.0.1:$destination"
}

acceptance_scp_from() {
  local source="$1" destination="$2"
  timeout --signal=TERM --kill-after=10s 300 \
    scp -i "$SSH_KEY" -P "$SSH_PORT" "${WIN_SSH_OPTS[@]}" \
    "builder@127.0.0.1:$source" "$destination"
}

release_retained_client() {
  local active_client="${1:-$artifacts/active-client-id}"
  [ -f "$active_client" ] || return 0
  echo "[windows acceptance] releasing retained network client"
  UR_ACCEPT_CREDENTIALS_FILE="$credentials" \
    timeout 90 node "$root/build/all/acceptance/client-cleanup.mjs" "$active_client"
}

shutdown_acceptance_vm() {
  local vm_pid="${WIN_QEMU_PID:-}" vm_run_dir="${WIN_RUN_DIR:-}" vm_failed=0
  win_shutdown_vm
  if [ -n "$vm_pid" ]; then
    for _ in $(seq 1 50); do
      kill -0 "$vm_pid" 2>/dev/null || break
      sleep 0.2
    done
    if kill -0 "$vm_pid" 2>/dev/null; then
      kill -KILL "$vm_pid" 2>/dev/null || true
      vm_failed=1
    fi
    wait "$vm_pid" 2>/dev/null || true
  fi
  if [ -n "$vm_run_dir" ] && [ -e "$vm_run_dir" ]; then
    vm_failed=1
  fi
  return "$vm_failed"
}

cleanup() {
  exit_status=$?
  if ! shutdown_acceptance_vm; then
    echo "[windows acceptance] could not stop the acceptance VM" >&2
    exit_status=1
  fi
  if ! release_retained_client; then
    echo "[windows acceptance] could not release the retained network client" >&2
    exit_status=1
  fi
  if ! rm -rf "$run_dir"; then
    echo "[windows acceptance] could not remove $run_dir" >&2
    exit_status=1
  fi
  if [ -n "$result_matrix" ]; then
    mkdir -p "$(dirname "$result_matrix")"
    matrix_status=PASS
    matrix_detail="Windows SDK/service acceptance completed"
    if [ "$exit_status" -ne 0 ]; then matrix_status=FAIL; matrix_detail="Windows acceptance runner failed; see artifacts"; fi
    for matrix_case in email phone solana bittensor instant password data-plane peer-to-peer; do
      printf 'windows\t%s\t%s\t%s\n' "$matrix_case" "$matrix_status" "$matrix_detail" >>"$result_matrix"
    done
    chmod 600 "$result_matrix"
  fi
  echo
  if [ "$exit_status" -eq 0 ]; then
    echo "[windows acceptance] ✓ ACCEPTANCE PASSED (artifacts: $artifacts)"
  else
    echo "[windows acceptance] ✗ ACCEPTANCE FAILED (artifacts: $artifacts)"
  fi
  exit "$exit_status"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

if [ "$skip_build" -ne 1 ]; then
  echo "[windows acceptance] building local Windows artifacts"
  SRC_HOME="$root" EXTERNAL_WARP_VERSION="$version" OUT_DIR="$out_dir" \
    timeout "$build_timeout" "$root/build/all/build-windows.sh" 2>&1 | tee "$artifacts/build.log"
else
  echo "[windows acceptance] reusing $out_dir"
fi

msi="$out_dir/URnetwork-${version}-arm64.msi"
[ -f "$msi" ] || die "missing locally built ARM64 MSI $msi"
msi_sha256="$(shasum -a 256 "$msi" | awk '{print toupper($1)}')"

echo "[windows acceptance] building the local SDK control agent"
(cd "$root/build/all/acceptance" && CGO_ENABLED=0 GOOS=windows GOARCH=arm64 \
  timeout 600 go build -trimpath -o "$run_dir/agent.exe" .)

echo "[windows acceptance] booting the isolated Windows ARM64 VM"
win_boot_vm
if ! win_wait_ssh; then
  win_mon "$WIN_MON_SOCK" "screendump $artifacts/vm-boot-failure.ppm"
  die "Windows VM did not reach SSH"
fi

remote=C:/acceptance
win_ssh_probe 60 "powershell -NoProfile -Command \"New-Item -ItemType Directory -Force -Path '$remote' | Out-Null\""
acceptance_scp_to "$msi" "$remote/urnetwork.msi"
acceptance_scp_to "$run_dir/agent.exe" "$remote/agent.exe"
acceptance_scp_to "$credentials" "$remote/credentials"
acceptance_scp_to "$tests_json" "$remote/tests.json"
acceptance_scp_to "$root/build/all/acceptance/run-windows.ps1" "$remote/run.ps1"
acceptance_scp_to "$root/build/all/acceptance/run-windows-lib.ps1" "$remote/run-windows-lib.ps1"
acceptance_scp_to "$root/build/all/acceptance/run-windows-lib.test.ps1" "$remote/run-windows-lib.test.ps1"
if [ -f "$fixture" ]; then
  acceptance_scp_to "$fixture" "$remote/guest-secret-key"
fi

win_ssh_probe 60 "powershell -NoProfile -ExecutionPolicy Bypass -File $remote/run-windows-lib.test.ps1 -Fixture $remote/urnetwork.msi"
echo "[windows acceptance] running $repeat_count complete repetition(s)"
set +e
win_ssh_probe "$((900 + repeat_count * 900))" "powershell -NoProfile -ExecutionPolicy Bypass -File $remote/run.ps1 -Msi $remote/urnetwork.msi -ExpectedMsiSha256 $msi_sha256 -AppVersion $version -SdkVersion $sdk_version -Repeat $repeat_count -Agent $remote/agent.exe -Credentials $remote/credentials -Tests $remote/tests.json -Fixture $remote/guest-secret-key -WorkDir $remote/results" \
  2>&1 | tee "$artifacts/run.log"
acceptance_status=${PIPESTATUS[0]}
set -e

if [ "$acceptance_status" -ne 0 ]; then
  win_ssh_probe 30 "powershell -NoProfile -Command \"Get-Process -Name agent -ErrorAction SilentlyContinue | Stop-Process -Force; Start-Sleep -Seconds 5\"" \
    >/dev/null 2>&1 || true
fi

for name in result.json agent.log selftest.log selftest.err.log peer-provider.log peer-provider.stdout.log peer-provider/result.json install.log uninstall.log urnetworkd.log failure.txt app-restore-failure.txt uninstall-failure.txt private-input-cleanup-failure.txt active-client-id peer-provider/active-client-id; do
  if win_ssh_probe 30 "powershell -NoProfile -Command \"if (Test-Path -LiteralPath '$remote/results/$name') { exit 0 } else { exit 1 }\"" >/dev/null 2>&1; then
    destination="$artifacts/$name"
    mkdir -p "$(dirname "$destination")"
    case "$name" in active-client-id|peer-provider/active-client-id)
      if ! acceptance_scp_from "$remote/results/$name" "$destination"; then
        echo "could not retrieve the retained network client ID" >&2
        acceptance_status=1
      fi
      ;;
    *) acceptance_scp_from "$remote/results/$name" "$destination" || true ;;
    esac
  fi
done
if win_ssh_probe 30 "powershell -NoProfile -Command \"if (Test-Path -LiteralPath '$remote/guest-secret-key') { exit 0 } else { exit 1 }\"" >/dev/null 2>&1; then
  acceptance_scp_from "$remote/guest-secret-key" "$run_dir/guest-secret-key" || true
  if [ "$(wc -w <"$run_dir/guest-secret-key" | tr -d ' ')" = 24 ]; then
    mv "$run_dir/guest-secret-key" "$fixture"
    chmod 600 "$fixture"
  fi
fi

if ! shutdown_acceptance_vm; then
  acceptance_status=1
fi
if ! release_retained_client; then
  acceptance_status=1
fi
if ! release_retained_client "$artifacts/peer-provider/active-client-id"; then
  acceptance_status=1
fi
if [ "$acceptance_status" -eq 0 ] && [ -f "$fixture" ] && [ "$keep_fixture" -ne 1 ]; then
  if timeout 90 node "$root/build/all/acceptance/fixture.mjs" delete "$fixture"; then
    rm -f "$fixture"
  else
    echo "could not delete instant-account fixture; retained at $fixture" >&2
    acceptance_status=1
  fi
fi

exit "$acceptance_status"
