# Stages and zips the portable distribution for one architecture.
#
# The portable zip IS the beta artifact (docs/superpowers/specs/
# 2026-08-09-windows-beta-distribution-design.md, section 2): the build output
# as a plain folder -- app, service, SDK dll, wintun, resources.pri, the
# self-contained WindowsAppRuntime -- plus the third-party notices, the wintun
# license, and a README. No installer, and no wrapper folder inside the zip:
# the in-app update checker downloads one of these and swaps files in place,
# so the payload must sit at the zip ROOT under the exact name grammar
#
#   URnetwork-v<version>-windows-<x64|arm64>-portable.zip
#
# Runs locally against whatever build-local.ps1 produced, and in CI against
# the same layout:
#   powershell -NoProfile -File app\tools\package-portable.ps1 -Platform x64 -Version 2026.8.9-1024096560-beta
#
# ASCII on purpose: this file must parse identically under Windows PowerShell
# 5.1 (which reads unmarked files as cp1252) and pwsh.
#
# SPDX-License-Identifier: MPL-2.0
[CmdletBinding()]
param(
  [ValidateSet("x64", "ARM64")]
  [string]$Platform = "x64",
  # the android-format version this build was stamped with; local dev builds
  # default to the sentinel the version contract reserves for "not a release"
  [string]$Version = "0.0.0-dev"
)

$ErrorActionPreference = "Stop"

$app = (Resolve-Path "$PSScriptRoot\..").Path
$srcDir = Join-Path $app "build\$Platform\Release"
# releases spell the arch in lowercase, msbuild platforms do not
$arch = $Platform.ToLowerInvariant()
# tolerate a v-prefixed version so a release tag pastes straight in
$Version = $Version -replace '^v', ''

if (-not (Test-Path $srcDir)) {
  throw "no build output at $srcDir -- run app\tools\build-local.ps1 first"
}

# Fail on the files that matter instead of zipping whatever happens to exist:
# a zip missing the service exe would look fine until a user ran it. (CI
# learned this lesson with the SDK job asserting on dirs instead of the DLL.)
$required = @("URnetwork.exe", "urnetworkd.exe", "URnetworkSdk.dll", "wintun.dll", "resources.pri")
foreach ($name in $required) {
  if (-not (Test-Path (Join-Path $srcDir $name))) {
    throw "required payload file missing from ${srcDir}: $name"
  }
}

# Without the self-contained WindowsAppRuntime payload the zip only runs where
# the runtime is preinstalled. A RELEASE-versioned package fails outright: the
# spec's zip contract is that a clean machine needs no preinstalled runtime,
# and a warning in a green CI log ships exactly the broken zip it warns about
# (SHA256SUMS would even verify it). Dev-versioned runs still only warn --
# builds made before the self-contained switch are worth packaging for a look
# inside, and code 0 can never be offered to anyone by the update checker.
if (-not (Test-Path (Join-Path $srcDir "Microsoft.ui.xaml.dll"))) {
  if ($Version -ne "0.0.0-dev") {
    throw "Microsoft.ui.xaml.dll is not in the build output -- a release-versioned zip must be self-contained (did WindowsAppSDKSelfContained stop applying?)"
  }
  Write-Warning "Microsoft.ui.xaml.dll is not in the build output -- this build is NOT self-contained, so the zip will require the Windows App Runtime to be installed"
}

# --- stage --------------------------------------------------------------------
$stage = Join-Path $app "build\$Platform\portable-stage"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Path $stage | Out-Null

# Copy the whole tree minus debug/link junk, rather than an allowlist: an
# allowlist already dropped Assets\Fonts from the CI artifact once, and would
# silently drop the next asset added too.
$excludeExt = @(".pdb", ".lib", ".exp", ".ilk")
$srcFull = (Get-Item $srcDir).FullName
$payload = Get-ChildItem $srcDir -Recurse -File | Where-Object {
  $rel = $_.FullName.Substring($srcFull.Length + 1)
  $inObj = $false
  foreach ($part in ($rel -split '\\')) {
    if ($part -eq "obj") { $inObj = $true }
  }
  (-not $inObj) -and ($excludeExt -notcontains $_.Extension.ToLowerInvariant())
}
foreach ($f in $payload) {
  $rel = $f.FullName.Substring($srcFull.Length + 1)
  $dest = Join-Path $stage $rel
  $destDir = Split-Path $dest -Parent
  if (-not (Test-Path $destDir)) {
    New-Item -ItemType Directory -Path $destDir -Force | Out-Null
  }
  Copy-Item $f.FullName $dest
}

# Attributions ride along in the zip root, same as the MSI installs them.
Copy-Item (Join-Path $app "THIRD-PARTY-NOTICES.txt") $stage
Copy-Item (Join-Path $app "third_party\wintun\wintun-license.txt") $stage

# --- README -------------------------------------------------------------------
# One page, ASCII, and honest -- especially about what SHA256SUMS does and
# does not protect against, and about how to un-break the network by hand.
$readme = @"
URnetwork (beta) -- portable build
version $Version, windows $arch
==================================

What this is
------------
The URnetwork app and its VPN service as a plain folder. There is no
installer: keep this folder anywhere you can write to and run it from
there. "Portable" means no installer -- your data does not travel with
the folder. App data lives in %LOCALAPPDATA%\URnetwork and service data
in %ProgramData%\URnetwork.

First run
---------
Open URnetwork.exe. The app offers to set up the VPN service -- one
click, one administrator prompt. The service (urnetworkd) is what moves
the traffic; the app is the window onto it. Then log in and connect.

Updating
--------
The app checks this project's releases and offers updates. Applying one
downloads the new zip, verifies it against the release's SHA256SUMS, and
swaps the files here in place; the app then shows "Update the VPN
service" -- one click restarts the service onto the new files. You can
also unzip a newer release over this folder yourself (quit the app
first). Note: SHA256SUMS protects the download against corruption, not
against a compromised download source. Signed installers come in a later
milestone.

Uninstalling
------------
In the app: Settings -> Uninstall VPN service. Or from an elevated
prompt: urnetworkd uninstall. Then delete this folder. If you want the
data gone too: %LOCALAPPDATA%\URnetwork and %ProgramData%\URnetwork.

Recovery
--------
If the internet ever breaks: stop urnetworkd, then elevated: urnetworkd
revert. That removes the service's network filters and routes and puts
the system back the way it was.
"@
Set-Content -Path (Join-Path $stage "README.txt") -Value $readme -Encoding Ascii

# --- zip ----------------------------------------------------------------------
$zipName = "URnetwork-v$Version-windows-$arch-portable.zip"
$zipPath = Join-Path $app "build\$Platform\$zipName"
if (Test-Path $zipPath) { Remove-Item $zipPath -Force }

# Zip by hand instead of Compress-Archive: PowerShell 5.1's Compress-Archive
# writes entry names with backslashes, which non-Windows tools extract as
# literal files named "Assets\Fonts\x". Entry names here are always
# forward-slashed, so the same zip is correct everywhere.
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$stageFull = (Get-Item $stage).FullName
$entries = Get-ChildItem $stage -Recurse -File | Sort-Object FullName
$zipStream = [System.IO.File]::Open($zipPath, [System.IO.FileMode]::CreateNew)
try {
  $archive = New-Object System.IO.Compression.ZipArchive($zipStream, [System.IO.Compression.ZipArchiveMode]::Create)
  try {
    foreach ($f in $entries) {
      $rel = $f.FullName.Substring($stageFull.Length + 1).Replace("\", "/")
      [void][System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
        $archive, $f.FullName, $rel, [System.IO.Compression.CompressionLevel]::Optimal)
    }
  } finally {
    $archive.Dispose()
  }
} finally {
  $zipStream.Dispose()
}

$mb = [math]::Round((Get-Item $zipPath).Length / 1MB, 1)
Write-Host "packaged $($entries.Count) files -> $zipPath ($mb MB)" -ForegroundColor Green
