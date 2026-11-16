# Fetches vendored dependencies for the URnetwork Windows solution:
#   - Wintun (pinned, upstream-signed) from wintun.net
#   - the URnetwork SDK Windows zip built by sdk/cgo, and generates import libs
#   - zxing-cpp (pinned source release), built headless as a static lib: the QR
#     DECODER the extender import sheet reads a chosen image with. The ENCODER
#     is not here - third_party/qrcodegen is two committed files (EXTENDER.md
#     K7, K8)
#
# Run from a Developer PowerShell (needs lib.exe on PATH for the import libs).
#
# SPDX-License-Identifier: MPL-2.0
[CmdletBinding()]
param(
  # path to sdk/cgo/build/URnetworkSdkWindows.zip (built on the macOS build server)
  [string]$SdkZip = "$PSScriptRoot\..\..\..\sdk\cgo\build\URnetworkSdkWindows.zip",
  [string]$WintunVersion = "0.14.1",
  [string]$ZXingVersion = "2.2.1",
  [ValidateSet("x64", "ARM64")][string[]]$Platforms = @("x64", "ARM64"),
  # zxing-cpp is a cmake build of a few hundred files; skipping it is for an
  # inner loop that is not touching the extender import sheet
  [switch]$SkipZXing
)

$ErrorActionPreference = "Stop"
$thirdParty = Join-Path $PSScriptRoot "..\third_party"

# --- Wintun (pinned like Mullvad: verify SHA256 + Authenticode signer) ---------
# Pins (verify against wintun.net before bumping the version):
$WintunSha256 = "07C256185D6EE3652E09FA55C0B673E2624B565E02C4B9091C79CA7D2F24EF51"
# Each architecture's wintun.dll inside that zip, as Get-FileHash writes them
# (QD-823). ur tun (sidecar/cli/tun_wintun.go, defaultUrWintunPins) holds the
# DLL it loads to the same two values, so bump them together.
$WintunDllSha256Amd64 = "E5DA8447DC2C320EDC0FC52FA01885C103DE8C118481F683643CACC3220DAFCE"
$WintunDllSha256Arm64 = "F7BA89005544BE9D85231A9E0D5F23B2D15B3311667E2DAD0DEBD344918A3F80"
$WintunSignerThumbprint = "DF98E075A012ED8C86FBCF14854B8F9555CB3D45"

$wintunDir = Join-Path $thirdParty "wintun"
# Cache the pinned zip in the VM rather than re-fetching every build. This used
# to be an unconditional download with no retry, so a momentary DNS blip inside
# the VM ("The remote name could not be resolved: 'www.wintun.net'") killed a
# build that had already compiled the whole cgo SDK. Caching is safe precisely
# because the artifact is pinned: the SHA256 below is verified on every run, so
# a stale, truncated or tampered cache entry fails exactly as a bad download
# would. The version is in the filename, so bumping $WintunVersion misses the
# cache and re-downloads.
$wintunCache = Join-Path $thirdParty "cache"
New-Item -ItemType Directory -Force -Path $wintunCache | Out-Null
$wintunZip = Join-Path $wintunCache "wintun-$WintunVersion.zip"

function Test-WintunZip {
  if (-not (Test-Path $wintunZip)) { return $false }
  return (Get-FileHash -Algorithm SHA256 $wintunZip).Hash -eq $WintunSha256
}

if (Test-WintunZip) {
  Write-Host "Wintun $WintunVersion already cached (sha256 verified)"
} else {
  Remove-Item -Force $wintunZip -ErrorAction SilentlyContinue
  # Three attempts: the failure mode seen in practice is transient DNS inside
  # the VM, which usually clears within seconds.
  $downloaded = $false
  foreach ($attempt in 1..3) {
    try {
      Write-Host "Downloading Wintun $WintunVersion (attempt $attempt/3) ..."
      Invoke-WebRequest -Uri "https://www.wintun.net/builds/wintun-$WintunVersion.zip" -OutFile $wintunZip
      $downloaded = $true
      break
    } catch {
      Write-Host "  download failed: $($_.Exception.Message)"
      Remove-Item -Force $wintunZip -ErrorAction SilentlyContinue
      if ($attempt -lt 3) { Start-Sleep -Seconds (5 * $attempt) }
    }
  }
  if (-not $downloaded) {
    throw "Wintun $WintunVersion download failed after 3 attempts (last error above). The VM could not reach www.wintun.net."
  }
}

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

New-Item -ItemType Directory -Force -Path $wintunDir | Out-Null
Copy-Item "$wintunExtract\wintun\include\wintun.h" "$wintunDir\wintun.h" -Force
$architectures = foreach ($platform in $Platforms) {
  if ($platform -eq "ARM64") { "arm64" } else { "amd64" }
}
foreach ($architecture in $architectures) {
  $wintunDll = "$wintunExtract\wintun\bin\$architecture\wintun.dll"
  $expectedDllHash = if ($architecture -eq "arm64") { $WintunDllSha256Arm64 } else { $WintunDllSha256Amd64 }
  $actualDllHash = (Get-FileHash -Algorithm SHA256 $wintunDll).Hash
  if ($actualDllHash -ne $expectedDllHash) {
    throw "Wintun $architecture dll SHA256 mismatch: expected $expectedDllHash, got $actualDllHash"
  }
  $wintunArchitectureDir = Join-Path $wintunDir "bin\$architecture"
  New-Item -ItemType Directory -Force -Path $wintunArchitectureDir | Out-Null
  Copy-Item $wintunDll (Join-Path $wintunArchitectureDir "wintun.dll") -Force
}

# Preserve Wintun's license next to the vendored DLL for third-party attribution
# (the prebuilt binaries are permissively licensed for redistribution; see
# THIRD-PARTY-NOTICES.txt, which the MSI ships). Prefer the copy inside the zip;
# if this version's zip doesn't carry one, fetch the pinned upstream text.
$wintunLicense = Get-ChildItem "$wintunExtract\wintun" -Recurse -File -ErrorAction SilentlyContinue |
  Where-Object { $_.Name -match '(?i)licen[cs]e' } | Select-Object -First 1
if ($wintunLicense) {
  Copy-Item $wintunLicense.FullName "$wintunDir\wintun-license.txt" -Force
} else {
  Invoke-WebRequest -Uri "https://raw.githubusercontent.com/WireGuard/wintun/$WintunVersion/prebuilt-binaries-license.txt" -OutFile "$wintunDir\wintun-license.txt"
}
Write-Host "Wintun OK (signer, zip hash and dll hashes verified)."

# --- zxing-cpp (QR decode for the extender import sheet) -----------------------
# Source release, pinned by sha256 and built HERE rather than pulled as a
# binary: upstream publishes no windows binaries, and vcpkg's MSBuild
# integration collides with the Windows App SDK (see the nlohmann note below).
# BUILD_WRITERS is OFF - the app never encodes with this; encoding is
# third_party/qrcodegen, two committed files (EXTENDER.md K7, K8). Examples are
# off too, which is what keeps the FetchContent of stb out of this build, so the
# configure step needs no network beyond the zip above.
#
# The pin is the sha256 of github's tag archive. If a future re-roll of that
# archive changes the bytes, update the pin in a reviewed change - never relax
# the check.
$ZXingSha256 = "71D9288F0637D321EE6823D8C27E684E9A00D4FFB92F18AFF95FB5342CB6521D"
$zxingDir = Join-Path $thirdParty "zxing-cpp"
$zxingBuilt = (Test-Path (Join-Path $zxingDir "include\ZXing\ReadBarcode.h")) -and
  -not ($Platforms | Where-Object { -not (Test-Path (Join-Path $zxingDir "lib\$_\ZXing.lib")) })
if ($SkipZXing) {
  Write-Host "zxing-cpp skipped (-SkipZXing)."
} elseif ($zxingBuilt) {
  Write-Host "zxing-cpp $ZXingVersion already built."
} else {
  if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    throw "cmake not found on PATH; zxing-cpp needs it (or re-run with -SkipZXing)"
  }
  $zxingZip = Join-Path $wintunCache "zxing-cpp-$ZXingVersion.zip"
  if (-not (Test-Path $zxingZip)) {
    Write-Host "Downloading zxing-cpp $ZXingVersion ..."
    Invoke-WebRequest -Uri "https://github.com/zxing-cpp/zxing-cpp/archive/refs/tags/v$ZXingVersion.zip" -OutFile $zxingZip
  }
  $zxingActual = (Get-FileHash -Algorithm SHA256 $zxingZip).Hash
  if ($zxingActual -ne $ZXingSha256) {
    Remove-Item -Force $zxingZip -ErrorAction SilentlyContinue
    throw "zxing-cpp SHA256 mismatch: expected $ZXingSha256, got $zxingActual"
  }
  $zxingExtract = Join-Path $env:TEMP "zxing-cpp-extract"
  Remove-Item -Recurse -Force $zxingExtract -ErrorAction SilentlyContinue
  Expand-Archive -Path $zxingZip -DestinationPath $zxingExtract
  $zxingSrc = Join-Path $zxingExtract "zxing-cpp-$ZXingVersion"

  New-Item -ItemType Directory -Force -Path (Join-Path $zxingDir "include") | Out-Null
  foreach ($platform in $Platforms) {
    $buildDir = Join-Path $env:TEMP "zxing-build-$platform"
    $installDir = Join-Path $buildDir "install"
    Remove-Item -Recurse -Force $buildDir -ErrorAction SilentlyContinue
    & cmake -S $zxingSrc -B $buildDir -A $platform `
      -DBUILD_SHARED_LIBS=OFF `
      -DBUILD_WRITERS=OFF `
      -DBUILD_READERS=ON `
      -DBUILD_EXAMPLES=OFF `
      -DBUILD_BLACKBOX_TESTS=OFF `
      -DBUILD_UNIT_TESTS=OFF `
      -DBUILD_PYTHON_MODULE=OFF `
      -DCMAKE_INSTALL_PREFIX="$installDir" | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "zxing-cpp cmake configure failed for $platform" }
    & cmake --build $buildDir --config Release --target install | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "zxing-cpp build failed for $platform" }

    $libDst = Join-Path $zxingDir "lib\$platform"
    New-Item -ItemType Directory -Force -Path $libDst | Out-Null
    $lib = Get-ChildItem "$installDir\lib" -Filter "ZXing*.lib" | Select-Object -First 1
    if (-not $lib) { throw "zxing-cpp produced no import/static lib for $platform" }
    Copy-Item $lib.FullName (Join-Path $libDst "ZXing.lib") -Force
    # The headers are arch independent, so one copy serves both; copying on
    # every pass is what keeps a partial earlier run from leaving a stale tree.
    Copy-Item "$installDir\include\*" (Join-Path $zxingDir "include") -Recurse -Force
  }
  Copy-Item (Join-Path $zxingSrc "LICENSE") (Join-Path $zxingDir "zxing-cpp-license.txt") -Force
  Write-Host "zxing-cpp $ZXingVersion OK."
}

# --- URnetwork SDK: unzip per-arch and build import libs ------------------------
if (-not (Test-Path $SdkZip)) {
  Write-Warning "SDK zip not found at $SdkZip. Build it with 'make -C sdk/cgo build_windows' on the build server, then re-run."
  return
}
$sdkExtract = Join-Path $env:TEMP "urnetwork-sdk-extract"
Remove-Item -Recurse -Force $sdkExtract -ErrorAction SilentlyContinue
Expand-Archive -Path $SdkZip -DestinationPath $sdkExtract

foreach ($arch in $architectures) {
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

# --- nlohmann/json (single header) --------------------------------------------
# Vendored instead of via vcpkg: the app is the only vcpkg consumer and vcpkg's
# MSBuild integration collides with the Windows App SDK. json.hpp is header-only,
# so a pinned single-header drop-in is simpler + reproducible. wil was declared in
# the old vcpkg manifest but is never #included, so it's dropped entirely.
$nlohmannDir = Join-Path $thirdParty "vendor-include\nlohmann"
if (-not (Test-Path (Join-Path $nlohmannDir "json.hpp"))) {
  New-Item -ItemType Directory -Force -Path $nlohmannDir | Out-Null
  Write-Host "Downloading nlohmann/json 3.12.0 (single header) ..."
  Invoke-WebRequest -Uri "https://github.com/nlohmann/json/releases/download/v3.12.0/json.hpp" -OutFile (Join-Path $nlohmannDir "json.hpp")
}
Write-Host "nlohmann/json OK."

Write-Host "Dependencies fetched."
