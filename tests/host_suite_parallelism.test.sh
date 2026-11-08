#!/usr/bin/env bash
# Exercise the actual suite script with an offline Go recorder. No Go tool,
# C++ compiler, network gate or repository source is executed by these controls.
set -euo pipefail

case_name="${1:?control name required}"
suite="${2:?suite script required}"
work="${3:?isolated work directory required}"
mkdir -p "$work/bin" "$work/root/windows/tests/insufficientbalance" "$work/root/build/all/windows" "$work/root/sdk" "$work/root/tests"
cp "$suite" "$work/root/windows/test.sh"
chmod +x "$work/root/windows/test.sh"
cat > "$work/root/tests/network-intensive-suite-lock.sh" <<'SH'
#!/usr/bin/env bash
[[ "$#" == 2 && "$1" == --verify-held && "$2" == run-all ]]
SH
cat > "$work/bin/go" <<'SH'
#!/usr/bin/env bash
set -euo pipefail
printf '%s\n' BEGIN "$@" END >> "$WINDOWS_SUITE_CONTROL_RECORD"
if [[ "${WINDOWS_SUITE_CONTROL_FAIL_FIRST:-}" == 1 ]]; then
  exit 42
fi
SH
chmod +x "$work/bin/go" "$work/root/tests/network-intensive-suite-lock.sh"
export PATH="$work/bin:$PATH"
export URNETWORK_ROOT="$work/root"
export URNETWORK_NETWORK_TEST_LOCK_HELD=1
unset URNETWORK_WINDOWS_HOST_OFFLINE_ADMISSION
export WINDOWS_SUITE_CONTROL_RECORD="$work/argv"
export GOFLAGS='-mod=readonly -p=2 -parallel=1 -count=1'
case "$case_name" in
  inherited-default) wanted_parallel=2; wanted_timeout=30m ;;
  caller-overrides) wanted_parallel=1; wanted_timeout=7m ;;
  first-command-failure)
    export WINDOWS_SUITE_CONTROL_FAIL_FIRST=1
    if "$work/root/windows/test.sh"; then
      echo 'WINDOWS_SUITE_FAILURE_IGNORED' >&2; exit 1
    else
      status=$?
    fi
    [[ "$status" == 42 && "$(awk '$0 == "BEGIN" { n++ } END { print n+0 }' "$work/argv")" == 1 ]]
    echo 'WINDOWS_SUITE_FAIL_FAST_PRESERVED'; exit 0 ;;
  *) echo "unknown control: $case_name" >&2; exit 2 ;;
esac
if [[ "$case_name" == caller-overrides ]]; then
  "$work/root/windows/test.sh" -parallel=1 -timeout=7m
else
  "$work/root/windows/test.sh"
fi

# Resolve repeated Go flags in their real order: GOFLAGS first, explicit argv
# second. The first record is the Windows mutation package; all four original
# suite commands must still be dispatched, in order.
first_args=()
in_first=0
while IFS= read -r arg; do
  if [[ "$arg" == BEGIN ]]; then in_first=1; continue; fi
  [[ "$arg" == END ]] && break
  [[ "$in_first" == 1 ]] && first_args+=("$arg")
done < "$work/argv"
parallel=1
timeout=''
next=''
for arg in "${first_args[@]}"; do
  if [[ -n "$next" ]]; then
    case "$next" in parallel) parallel="$arg" ;; timeout) timeout="$arg" ;; esac
    next=''; continue
  fi
  case "$arg" in
    -parallel) next=parallel ;;
    -parallel=*) parallel="${arg#*=}" ;;
    -timeout) next=timeout ;;
    -timeout=*) timeout="${arg#*=}" ;;
  esac
done
if [[ "$parallel" != "$wanted_parallel" ]]; then
  echo "WINDOWS_COMPILER_DEFAULT_SERIALIZED: got=$parallel want=$wanted_parallel" >&2; exit 1
fi
[[ "$timeout" == "$wanted_timeout" ]]
[[ "$(awk '$0 == "BEGIN" { n++ } END { print n+0 }' "$work/argv")" == 4 ]]
awk '
  $0 == "BEGIN" { n++; next }
  $0 == "./tests" && n == 1 { first=1 }
  $0 == "./tests/insufficientbalance" && n == 2 { second=1 }
  $0 == "./..." && n == 3 { third=1 }
  $0 == "-check" && n == 4 { fourth=1 }
  END { exit !(first && second && third && fourth) }
' "$work/argv"
echo "WINDOWS_COMPILER_BUDGET_CONTROL: case=$case_name parallel=$parallel timeout=$timeout commands=4"
