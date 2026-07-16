# Build the URnetwork Windows cgo SDK (URnetworkSdk.dll for x64 + ARM64) NATIVELY
# inside the Windows ARM64 build VM, using Go + llvm-mingw provisioned into the
# image (build/all/windows/packer/scripts/provision.ps1). This replaces the old
# macOS cross-build (sdk/cgo `make build_windows`): build/all/windows/build.sh
# runs it over ssh right after rsync'ing the build home in, then runs the app
# build.ps1. Mirrors sdk/cgo/Makefile's build_windows recipe.
#
# Produces, under $SdkDir\build:
#   windows\amd64\URnetworkSdk.dll (+ urnetwork_sdk.h/.hpp/.def)
#   windows\arm64\URnetworkSdk.dll (+ urnetwork_sdk.h/.hpp/.def)
#   URnetworkSdkWindows.zip        (the layout fetch-deps.ps1 consumes + run.sh uploads)
#
# SPDX-License-Identifier: MPL-2.0
[CmdletBinding()]
param(
  # WARP_VERSION (internal, e.g. 2026.7.6+985989570) - baked into the DLL via -ldflags.
  [Parameter(Mandatory = $true)][string]$Version,
  [string]$SdkDir = "C:\build\urnetwork\sdk\cgo"
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest
function Log($m) { Write-Host "[build-sdk] $m" }
function Require($name) {
  if (-not (Get-Command $name -ErrorAction SilentlyContinue)) {
    throw "$name not found on PATH - is the SDK toolchain provisioned into the image? (Go + llvm-mingw; see all/windows/packer/scripts/provision.ps1)"
  }
}

Require 'go'
Require 'x86_64-w64-mingw32-clang'
Require 'aarch64-w64-mingw32-clang'

if (-not (Test-Path $SdkDir)) { throw "SDK dir not found: $SdkDir (was the build home synced in?)" }
Set-Location $SdkDir

# sdk/cgo/go.sum is git-ignored and generated (run.sh regenerates it at version
# staging via `go mod tidy`). A build from main hasn't staged, so generate it if
# absent - else the cgo build fails "missing go.sum entry". `go mod download all`
# is build-complete and leaves the tracked go.mod untouched.
if (-not (Test-Path (Join-Path $SdkDir "go.sum"))) {
  Log "go.sum missing - generating it (go mod download all)"
  & go mod download all
  if ($LASTEXITCODE -ne 0) { throw "go mod download all failed" }
}

# Match the Makefile: green tea GC, trimpath, c-shared, version stamped in.
$env:GOEXPERIMENT = "greenteagc"
$env:CGO_ENABLED  = "1"
$env:GOOS         = "windows"

$cc = @{ "amd64" = "x86_64-w64-mingw32-clang"; "arm64" = "aarch64-w64-mingw32-clang" }
$buildDir = Join-Path $SdkDir "build"

# Start from a clean windows/ tree so a partial/previous run can't leak stale
# DLLs into the zip.
Remove-Item (Join-Path $buildDir "windows") -Recurse -Force -ErrorAction SilentlyContinue

foreach ($arch in @("amd64", "arm64")) {
  Log "building windows/$arch (CC=$($cc[$arch]))"
  $env:GOARCH = $arch
  $env:CC     = $cc[$arch]
  $outDir = Join-Path $buildDir "windows\$arch"
  New-Item -ItemType Directory -Force -Path $outDir | Out-Null

  & go build -trimpath -buildvcs=false -buildmode=c-shared `
      -ldflags "-s -w -X github.com/urnetwork/sdk.Version=$Version -buildid=" `
      -o (Join-Path $outDir "URnetworkSdk.dll") .
  if ($LASTEXITCODE -ne 0) { throw "go build failed for windows/$arch" }

  # cgo writes a .h next to the dll; drop it and ship the checked-in ABI headers
  # (the .def drives the import-lib generation in the app's fetch-deps.ps1).
  Remove-Item (Join-Path $outDir "URnetworkSdk.h") -ErrorAction SilentlyContinue
  foreach ($h in @("urnetwork_sdk.h", "urnetwork_sdk.hpp", "urnetwork_sdk.def")) {
    Copy-Item (Join-Path $SdkDir "include\$h") $outDir -Force
  }
  if (-not (Test-Path (Join-Path $outDir "URnetworkSdk.dll"))) {
    throw "URnetworkSdk.dll missing for windows/$arch after build"
  }
}

# Zip windows\{amd64,arm64}\ -> URnetworkSdkWindows.zip (same layout the mac
# `make build_windows` produced; the archive root holds the `windows` folder).
$zip = Join-Path $buildDir "URnetworkSdkWindows.zip"
Remove-Item $zip -Force -ErrorAction SilentlyContinue
Compress-Archive -Path (Join-Path $buildDir "windows") -DestinationPath $zip
Log "SDK zip -> $zip"
