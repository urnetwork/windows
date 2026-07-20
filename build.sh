#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0
#
# Smoketest build for the windows app — regenerates the .resw string resources
# from the localization store (../localizations/keys) like the pipeline does,
# then runs the same build the pipeline runs: build/all/build-windows.sh
# (cgo SDK DLLs + x64/arm64 MSIs inside the local QEMU/HVF ARM Windows VM).
# One-time VM setup: build/all/windows/setup.sh — see build/BUILD-PLATFORMS.md.
#
# Usage:
#   ./build.sh
#   EXTERNAL_WARP_VERSION=<v>   version stamp (default 0.0.0-0 for local builds)
#   OUT_DIR=<dir>               MSI output (default <this repo>/out/smoketest;
#                               existing *.msi there are cleared)
#   URNETWORK_ROOT=<dir>        sibling-repo root (default: parent of this repo)
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
root="${URNETWORK_ROOT:-$(dirname "$here")}"

echo "== sync localizations (store -> app/src/App/Strings/*/Resources.resw)"
(cd "$root/localizations" &&
    { [ -d node_modules ] || npm ci --no-audit --no-fund; } &&
    npm run gen:windows)

echo "== pipeline windows build (QEMU VM: cgo SDK + MSIs)"
SRC_HOME="$root" \
EXTERNAL_WARP_VERSION="${EXTERNAL_WARP_VERSION:-0.0.0-0}" \
OUT_DIR="${OUT_DIR:-$here/out/smoketest}" \
    "$root/build/all/build-windows.sh"

echo "== windows build OK"
