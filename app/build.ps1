# Windows-host build: fetch deps, build the app + service + driver for x64 and
# ARM64, sign, and produce the per-arch MSIs. Invoked by build/all/run.sh over
# ssh on the ARM64 Windows build VM (native ARM64 VS 2022 cross-builds both
# arches — see build/DESKTOP_BUILD.md). Run from windows/app.
#
# SPDX-License-Identifier: MPL-2.0
[CmdletBinding()]
param(
  [Parameter(Mandatory = $true)][string]$Version,
  # sdk/cgo/build/URnetworkSdkWindows.zip cross-built on the macOS host
  [Parameter(Mandatory = $true)][string]$SdkZip,
  [string[]]$Platforms = @("x64", "ARM64"),
  [string]$Configuration = "Release",
  # signing (optional): Authenticode cert thumbprint for the app installer + PEs
  [string]$SignThumbprint = $env:URN_SIGN_THUMBPRINT,
  [string]$OutDir = "$PSScriptRoot\build\out"
)

$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

# Resolve tools (Developer environment must be on PATH, or launch from a
# Developer PowerShell). msbuild, lib.exe, signtool, dotnet (WiX), vcpkg.
function Require($name) {
  if (-not (Get-Command $name -ErrorAction SilentlyContinue)) {
    throw "$name not found on PATH (open a Developer PowerShell for VS 2022)"
  }
}
Require msbuild
Require dotnet

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

# 1. Fetch wintun (pinned) + unzip the SDK + build the per-arch import libs.
& "$PSScriptRoot\tools\fetch-deps.ps1" -SdkZip $SdkZip

# 2. Set the version into the app + installer (single source of truth).
#    (The SDK Version is baked into the DLL at cross-build time via -ldflags.)
$env:URN_VERSION = $Version

foreach ($platform in $Platforms) {
  Write-Host "== building $platform $Configuration =="

  # 3. Build the solution (Common, Service, App, SplitTunnel driver).
  msbuild URnetwork.sln `
    /p:Configuration=$Configuration /p:Platform=$platform `
    /p:Version=$Version /m /nologo /v:minimal

  $bin = "$PSScriptRoot\build\$platform\$Configuration"

  # 4. Sign the PE files (app + service + SDK dll + driver), if a cert is set.
  if ($SignThumbprint) {
    Get-ChildItem $bin -Include *.exe, *.dll, *.sys -Recurse | ForEach-Object {
      signtool sign /sha1 $SignThumbprint /fd SHA256 `
        /tr http://timestamp.digicert.com /td SHA256 $_.FullName
    }
    # The driver additionally needs Microsoft attestation signing — see
    # SIGNING.md; that .cab submission is a separate offline step and its signed
    # .sys/.cat replace the dev-signed ones in $bin before packaging.
  }

  # 5. Build the MSI for this arch (WiX v5), staging from $bin.
  $wixPlatform = if ($platform -eq "ARM64") { "arm64" } else { "x64" }
  dotnet build installer\Installer.wixproj -c $Configuration `
    -p:Platform=$platform -p:BinDir=$bin -p:Version=$Version

  $msi = Get-ChildItem "installer\bin\$platform\$Configuration\*.msi" | Select-Object -First 1
  if (-not $msi) { throw "MSI not produced for $platform" }

  # 6. Sign the MSI.
  if ($SignThumbprint) {
    signtool sign /sha1 $SignThumbprint /fd SHA256 `
      /tr http://timestamp.digicert.com /td SHA256 $msi.FullName
  }

  $dest = Join-Path $OutDir "URnetwork-$Version-$wixPlatform.msi"
  Copy-Item $msi.FullName $dest -Force
  Write-Host "== produced $dest =="
}

Write-Host "Desktop MSIs in $OutDir"
