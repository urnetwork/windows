# Windows-host build: fetch deps, build the app + service + driver for x64 and
# ARM64, sign, and produce the per-arch MSIs. Invoked by build/all/run.sh over
# ssh on the ARM64 Windows build VM (native ARM64 VS 2022 cross-builds both
# arches - see build/DESKTOP_BUILD.md). Run from windows/app.
#
# SPDX-License-Identifier: MPL-2.0
[CmdletBinding()]
param(
  [Parameter(Mandatory = $true)][string]$Version,
  # sdk/cgo/build/URnetworkSdkWindows.zip, built in this VM by ..\build-sdk.ps1
  [Parameter(Mandatory = $true)][string]$SdkZip,
  [string[]]$Platforms = @("x64", "ARM64"),
  [string]$Configuration = "Release",
  # Build + package the WFP split-tunnel driver (needs the WDK). Off by default:
  # the MSI ships without split tunneling until the kernel-driver build is wired
  # into the solution (driver\SplitTunnel.vcxproj is not in URnetwork.sln's build).
  [switch]$IncludeDriver,
  # signing (optional): Authenticode cert thumbprint for the app installer + PEs
  [string]$SignThumbprint = $env:URN_SIGN_THUMBPRINT,
  # Default resolved in the body: $PSScriptRoot is EMPTY during -File parameter
  # default binding (it is only populated once the script body runs), so a default
  # of "$PSScriptRoot\build\out" here silently becomes "\build\out" (= C:\build\out)
  # and the MSIs land off build.sh's retrieval path. Leave empty; Join-Path below.
  [string]$OutDir = ""
)

$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot
if (-not $OutDir) { $OutDir = Join-Path $PSScriptRoot "build\out" }

# Resolve tools. This runs headless over ssh (build/all/windows/build.sh), not
# from an interactive Developer PowerShell, so load the VS 2022 build environment
# here (msbuild, cl, INCLUDE/LIB, ...). dotnet (WiX) is already on PATH from
# provisioning.
function Require($name) {
  if (-not (Get-Command $name -ErrorAction SilentlyContinue)) {
    throw "$name not found on PATH (open a Developer PowerShell for VS 2022)"
  }
}

# TEMPORARY diagnostic (remove once XAML codegen is green). The App is a C++/WinRT
# WinUI 3 project: MarkupCompilePass1 (the IN-PROCESS CompileXaml MSBuild task, run
# inside msbuild.exe) must generate App.xaml.g.h / MainWindow.xaml.g.h before ClCompile.
# When they are missing (C1083 for App.xaml.g.h; x:Name identifiers "not found" in
# MainWindow.xaml.cpp) the markup compiler produced nothing. /v:minimal hides WHY
# (WASDK 1.6 swallows the real markup error - microsoft-ui-xaml#9813), so re-run JUST
# the App project at diagnostic verbosity and surface the MarkupCompilePass1 target
# status, the CompileXaml task + parameters (Language/GenXbf), and any task exception.
function Diagnose-XamlCodegen([string]$MsBuild, [string]$Platform, [string]$Configuration) {
  Write-Host "== diagnosing App XAML markup compile (why no .xaml.g.h) =="
  try {
    Get-ChildItem -Path src, build -Recurse -Directory -ErrorAction SilentlyContinue |
      Where-Object { $_.Name -eq 'Generated Files' } | ForEach-Object {
        Write-Host "[xaml] dir $($_.FullName):"
        Get-ChildItem -LiteralPath $_.FullName -ErrorAction SilentlyContinue | Select-Object -First 40 |
          ForEach-Object { Write-Host "[xaml]   $($_.Name)" }
      }
    # Whether the markup output landed anywhere (whole App tree, not just 'Generated
    # Files'): if *.xaml.g.h exist, the passes ran but wrote off the C++ include path;
    # if nowhere, they errored - the [urn-xaml] winmd-ref count and the "XamlCompiler
    # error WMCxxxx" line above now carry the real cause (no slow re-run needed).
    # List every generated XAML unit (markup .xaml.g.h/.hpp + all *.g.cpp impls). At link
    # time the impl .cpp (XamlTypeInfo/XamlMetaDataProvider) must be compiled; this reveals
    # their exact names/locations so UrnCompileGeneratedXamlImpl can target them.
    $gh = Get-ChildItem -Path src\App -Recurse -File -ErrorAction SilentlyContinue |
      Where-Object { $_.Name -like '*.xaml.g.h' -or $_.Name -like '*.xaml.g.hpp' -or $_.Name -like '*.g.cpp' -or $_.Name -eq 'XamlMetaDataProvider.cpp' }
    if ($gh) { $gh | ForEach-Object { Write-Host "[xaml] gen: $($_.FullName)" } }
    else { Write-Host "[xaml] NO generated XAML files under src\App -> the markup passes produced nothing" }
  } catch { Write-Warning "[xaml] diagnostic error: $($_.Exception.Message)" }
}

# Resolve VS + load its build environment (this runs headless over ssh, not from an
# interactive Developer PowerShell, so msbuild/cl/INCLUDE/LIB aren't on PATH).
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "vswhere not found at $vswhere (install VS 2022 Build Tools; see all/windows/packer/scripts/provision.ps1)" }
$vsPath = & $vswhere -latest -products '*' -requires Microsoft.Component.MSBuild -property installationPath
if (-not $vsPath) { throw "no VS 2022 install with MSBuild found via vswhere" }
# CRITICAL (ARM64 build host): the WinUI XAML markup compiler is managed AnyCPU
# (Microsoft.UI.Xaml.Markup.Compiler.dll) and runs IN-PROCESS inside msbuild.exe via
# the CompileXaml task, so the CLR it executes on == msbuild.exe's OWN architecture. On
# .NET Framework's native-arm64 CLR it dies at startup and never emits App.xaml.g.h /
# MainWindow.xaml.g.h (-> C1083, then a cascade of x:Name "identifier not found"). The
# same code runs fine under x64 (or x86) EMULATION on the mature CLR - exactly like a
# native x64 build host. VsDevCmd's -host_arch selects only the C++ TOOLCHAIN arch, NOT
# msbuild's, so we must invoke the x64 msbuild.exe (Bin\amd64) EXPLICITLY rather than
# trust what the dev shell puts on PATH (the native arm64 msbuild here - the one that
# crashes the markup compiler). x86 (Bin\) is an acceptable fallback (also emulated and
# mature); the only architecture to avoid is native arm64.
$msbuild = & $vswhere -latest -products '*' -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\amd64\MSBuild.exe' | Select-Object -First 1
if (-not $msbuild) {
  $msbuild = & $vswhere -latest -products '*' -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
}
if (-not $msbuild) { throw "no x64/x86 msbuild.exe in the VS install (need MSBuild\Current\Bin\amd64 or Bin)" }

# Enter the VS dev shell for the C++ toolchain ENVIRONMENT (INCLUDE/LIB, and PATH to
# cl.exe/rc.exe/mt.exe + the Windows SDK). -host_arch=amd64 picks the x64-hosted VC
# tools to match the x64 msbuild above. This only sets env vars; the build steps below
# still invoke $msbuild (x64) explicitly, which is what fixes the markup-compiler arch.
$devShell = Join-Path $vsPath 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll'
try {
  Import-Module $devShell
  Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation -DevCmdArguments '-arch=amd64 -host_arch=amd64' | Out-Null
} catch {
  Write-Warning "Enter-VsDevShell failed ($_); relying on the project-resolved VC toolset"
}
Write-Host "== msbuild (x64/emulated - hosts the in-process WinUI markup compiler): $msbuild =="
Require dotnet

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

# 1. Fetch wintun (pinned) + unzip the SDK + build the per-arch import libs.
& "$PSScriptRoot\tools\fetch-deps.ps1" -SdkZip $SdkZip

# 2. Set the version into the app + installer (single source of truth).
#    (The SDK Version is baked into the DLL at cross-build time via -ldflags.)
$env:URN_VERSION = $Version

# Restore NuGet packages for the solution. The App project uses PackageReference
# (C++/WinRT, Windows App SDK, WebView2, SDK BuildTools); unlike packages.config its
# restore is an MSBuild target, not `nuget.exe restore`. The build below imports the
# obj\*.nuget.g.props/targets this produces; without it App.idl falls back to classic
# MIDL (MIDL2025 on WinRT `namespace`). Restore is config-agnostic; run it once.
& $msbuild URnetwork.sln /t:restore /nologo /v:minimal
if ($LASTEXITCODE -ne 0) { throw "NuGet (PackageReference) restore failed" }

foreach ($platform in $Platforms) {
  Write-Host "== building $platform $Configuration =="

  # 3. Build the solution (Common, Service, App, SplitTunnel driver).
  & $msbuild URnetwork.sln `
    /p:Configuration=$Configuration /p:Platform=$platform `
    /p:Version=$Version /m /nologo /v:minimal
  if ($LASTEXITCODE -ne 0) {
    Diagnose-XamlCodegen -MsBuild $msbuild -Platform $platform -Configuration $Configuration
    throw "solution build failed for $platform (see errors above)"
  }

  $bin = "$PSScriptRoot\build\$platform\$Configuration"

  # The split-tunnel driver is intentionally excluded from URnetwork.sln's build
  # (kernel driver; needs the WDK). Build it on demand when -IncludeDriver is set,
  # so its .sys lands in $bin for the installer to pick up.
  if ($IncludeDriver) {
    Write-Host "== building split-tunnel driver ($platform) =="
    # The WDK's MSBuild toolset (WindowsKernelModeDriver10.0) is NOT registered by the
    # standalone wdksetup.exe /quiet in the image - its VS integration ships as a VSIX
    # that VSIXInstaller rejects on Build Tools (exit 2001, "not installable on any
    # products"). Install it by copying the VSIX's $MSBuild payload into the VS MSBuild
    # tree (the layout maps 1:1). Idempotent: skip if the toolset is already present.
    $kmToolset = Get-ChildItem "$vsPath\MSBuild\Microsoft\VC" -Recurse -Directory `
      -Filter WindowsKernelModeDriver10.0 -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $kmToolset) {
      $wdkVsix = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\Vsix" -Recurse `
        -Filter WDK.vsix -ErrorAction SilentlyContinue | Select-Object -First 1
      if (-not $wdkVsix) { throw "WDK.vsix not found - cannot build the driver (is the WDK installed?)" }
      Write-Host "== installing WDK MSBuild toolset from $($wdkVsix.FullName) =="
      $ext = "$env:TEMP\wdk-vsix"; if (Test-Path $ext) { Remove-Item -Recurse -Force $ext }
      Copy-Item $wdkVsix.FullName "$env:TEMP\WDK.zip" -Force
      Expand-Archive "$env:TEMP\WDK.zip" $ext -Force
      Copy-Item "$(Join-Path $ext '$MSBuild')\*" "$vsPath\MSBuild" -Recurse -Force
    }
    # SpectreMitigation is forced on by the driver toolset, but the Spectre-mitigated VC
    # runtime libs aren't in the image (MSB8040) -> pass =false on the command line (wins
    # over the toolset). SignMode=Off + the INF as <None> (skips the x86-emulated inf2cat,
    # which exits -2 under ARM64) are set in the vcxproj.
    & $msbuild driver\SplitTunnel.vcxproj `
      /p:Configuration=$Configuration /p:Platform=$platform `
      /p:SpectreMitigation=false /m /nologo /v:minimal
    if ($LASTEXITCODE -ne 0) { throw "driver build failed for $platform" }
    # The driver toolset overrides OutDir, so SplitTunnel.sys lands in driver\<plat>\<cfg>,
    # not $bin. Copy it where the WiX installer harvests it ($(BinDir)\SplitTunnel.sys).
    $drvSys = "driver\$platform\$Configuration\SplitTunnel.sys"
    if (-not (Test-Path $drvSys)) { throw "driver .sys not produced at $drvSys" }
    Copy-Item $drvSys $bin -Force
    Write-Host "== driver -> $bin\SplitTunnel.sys =="
  }

  # 4. Sign the PE files (app + service + SDK dll + driver), if a cert is set.
  if ($SignThumbprint) {
    Get-ChildItem $bin -Include *.exe, *.dll, *.sys -Recurse | ForEach-Object {
      signtool sign /sha1 $SignThumbprint /fd SHA256 `
        /tr http://timestamp.digicert.com /td SHA256 $_.FullName
    }
    # The driver additionally needs Microsoft attestation signing - see
    # SIGNING.md; that .cab submission is a separate offline step and its signed
    # .sys/.cat replace the dev-signed ones in $bin before packaging.
  }

  # 5. Build the MSI for this arch (WiX v5), staging from $bin. The driver payload
  #    is compiled out of the package unless -IncludeDriver produced its .sys above.
  $wixPlatform = if ($platform -eq "ARM64") { "arm64" } else { "x64" }
  $wixArgs = @("build", "installer\Installer.wixproj", "-c", $Configuration,
    "-p:Platform=$platform", "-p:BinDir=$bin", "-p:Version=$Version")
  if ($IncludeDriver) { $wixArgs += "-p:IncludeDriver=true" }
  dotnet @wixArgs
  if ($LASTEXITCODE -ne 0) { throw "MSI build failed for $platform" }

  # The shared Directory.Build.props redirects OutDir to build\<plat>\<cfg>, which
  # also moves the WiX MSI there (next to the staged binaries), not the wixproj's
  # default installer\bin\<plat>\<cfg>. Look in $bin first, with the default as a
  # fallback so either layout works.
  $msi = Get-ChildItem "$bin\*.msi", "installer\bin\$platform\$Configuration\*.msi" `
    -ErrorAction SilentlyContinue | Select-Object -First 1
  if (-not $msi) { throw "MSI not produced for $platform (looked in $bin and installer\bin\$platform\$Configuration)" }

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
