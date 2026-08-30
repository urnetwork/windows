#!/usr/bin/env bash

windows_acceptance_build_timeout() {
  local value="${UR_ACCEPT_WINDOWS_BUILD_TIMEOUT:-7200}"
  case "$value" in
    ''|*[!0-9]*|0)
      echo "UR_ACCEPT_WINDOWS_BUILD_TIMEOUT must be a positive integer" >&2
      return 2
      ;;
  esac
  printf '%s\n' "$value"
}
