# Fetches vendored dependencies for the URnetwork Windows solution:
#   - Wintun (pinned, upstream-signed) from wintun.net
#   - the URnetwork SDK Windows zip built by sdk/cgo, and generates import libs
#
# Run from a Developer PowerShell (needs lib.exe on PATH for the import libs).
#
# SPDX-License-Identifier: MPL-2.0
[CmdletBinding()]
param(
  # path to sdk/cgo/build/URnetworkSdkWindows.zip (built on the macOS build server)
  [string]$SdkZip = "$PSScriptRoot\..\..\..\sdk\cgo\build\URnetworkSdkWindows.zip",
  [string]$WintunVersion = "0.14.1"
)

$ErrorActionPreference = "Stop"
$thirdParty = Join-Path $PSScriptRoot "..\third_party"

# --- Wintun (pinned like Mullvad: verify SHA256 + Authenticode signer) ---------
# Pins (verify against wintun.net before bumping the version):
$WintunSha256 = "07C256185D6EE3652E09FA55C0B673E2624B565E02C4B9091C79CA7D2F24EF51"
$WintunSignerThumbprint = "DF98E075A012ED8C86FBCF14854B8F9555CB3D45"

$wintunDir = Join-Path $thirdParty "wintun"
$wintunZip = Join-Path $env:TEMP "wintun-$WintunVersion.zip"
Write-Host "Downloading Wintun $WintunVersion ..."
Invoke-WebRequest -Uri "https://www.wintun.net/builds/wintun-$WintunVersion.zip" -OutFile $wintunZip

$actual = (Get-FileHash -Algorithm SHA256 $wintunZip).Hash
if ($actual -ne $WintunSha256) {
  throw "Wintun SHA256 mismatch: expected $WintunSha256, got $actual"
}

$wintunExtract = Join-Path $env:TEMP "wintun-extract"
Remove-Item -Recurse -Force $wintunExtract -ErrorAction SilentlyContinue
Expand-Archive -Path $wintunZip -DestinationPath $wintunExtract

# verify the upstream Authenticode signer on each dll we ship
Get-ChildItem "$wintunExtract\wintun\bin" -Recurse -Filter wintun.dll | ForEach-Object {
  $sig = Get-AuthenticodeSignature $_.FullName
  if ($sig.Status -ne "Valid") { throw "Wintun dll $($_.FullName) not validly signed: $($sig.Status)" }
  $tp = $sig.SignerCertificate.Thumbprint
  if ($tp -ne $WintunSignerThumbprint) { throw "Wintun signer thumbprint mismatch: $tp" }
}

New-Item -ItemType Directory -Force -Path "$wintunDir\bin\amd64", "$wintunDir\bin\arm64" | Out-Null
Copy-Item "$wintunExtract\wintun\include\wintun.h" "$wintunDir\wintun.h" -Force
Copy-Item "$wintunExtract\wintun\bin\amd64\wintun.dll" "$wintunDir\bin\amd64\wintun.dll" -Force
Copy-Item "$wintunExtract\wintun\bin\arm64\wintun.dll" "$wintunDir\bin\arm64\wintun.dll" -Force
Write-Host "Wintun OK (signer + hash verified)."

# --- URnetwork SDK: unzip per-arch and build import libs ------------------------
if (-not (Test-Path $SdkZip)) {
  Write-Warning "SDK zip not found at $SdkZip. Build it with 'make -C sdk/cgo build_windows' on the build server, then re-run."
  return
}
$sdkExtract = Join-Path $env:TEMP "urnetwork-sdk-extract"
Remove-Item -Recurse -Force $sdkExtract -ErrorAction SilentlyContinue
Expand-Archive -Path $SdkZip -DestinationPath $sdkExtract

foreach ($arch in @("amd64", "arm64")) {
  $src = Join-Path $sdkExtract "windows\$arch"
  $dst = Join-Path $thirdParty "urnetwork-sdk\$arch"
  New-Item -ItemType Directory -Force -Path $dst | Out-Null
  Copy-Item "$src\URnetworkSdk.dll" $dst -Force
  Copy-Item "$src\urnetwork_sdk.h" $dst -Force
  Copy-Item "$src\urnetwork_sdk.hpp" $dst -Force
  Copy-Item "$src\urnetwork_sdk.def" $dst -Force
  # generate the import library from the module-definition file
  $machine = if ($arch -eq "arm64") { "arm64" } else { "x64" }
  & lib.exe "/def:$dst\urnetwork_sdk.def" "/machine:$machine" "/out:$dst\URnetworkSdk.lib" | Out-Null
  Write-Host "SDK $arch OK."
}

Write-Host "Dependencies fetched."
