# Asserts the built MSI actually carries a working app, not just that it
# compiled. `dotnet build app/installer/Installer.wixproj` is a compile-proof
# only -- it never inspected what landed inside the .msi -- and that gap is
# exactly how the MSI shipped for months installing 7 files (the exe, the
# SDK dll, resources.pri, the service exe, wintun.dll, and two license
# files) while silently omitting the self-contained WindowsAppRuntime and
# Assets\Fonts. An install from that MSI produced an app that could not
# start on any machine that had not already run the portable zip.
#
# Inspection method: the built-in Windows Installer COM object
# (WindowsInstaller.Installer), which ships with every Windows install --
# CI runners included -- so this needs no extra tooling (no msiinfo/msidb/
# lessmsi). It opens the MSI as a database and queries the File table
# directly; no install/extract happens.
#
# Run locally the same way CI does:
#   powershell -NoProfile -File app\tools\verify-msi-payload.ps1 -MsiPath app\installer\dist\URnetwork.msi
#
# SPDX-License-Identifier: MPL-2.0
[CmdletBinding()]
param(
  [Parameter(Mandatory = $true)]
  [string]$MsiPath,

  # 304 files shipped in the portable zip for the 2026.8.11 beta (7
  # hand-authored + ~297 harvested WindowsAppRuntime/WinUI3/Assets files).
  # The floor is set well below that so a routine WindowsAppSDK bump
  # (which changes the exact runtime file count) does not make this flaky,
  # while still catching a regression back to the old 7-8 file MSI by a
  # wide margin.
  [int]$MinFileCount = 250,

  # Long filenames (as recorded in the MSI File table) that must be present
  # for the app to actually start and render in its own brand:
  #   - Microsoft.WindowsAppRuntime.dll / Microsoft.ui.xaml.dll: the
  #     self-contained runtime; their absence is the exact defect this
  #     script exists to catch.
  #   - resources.pri: without it every string renders as its key id.
  #   - pp_neue_montreal_regular.ttf: a brand font under Assets\Fonts; its
  #     absence means the app silently falls back to a system font.
  #   - App.xbf: the app's own compiled XAML for App.xaml. Without it the
  #     app cannot even construct its Application object.
  [string[]]$RequireNames = @(
    "Microsoft.WindowsAppRuntime.dll",
    "Microsoft.ui.xaml.dll",
    "resources.pri",
    "pp_neue_montreal_regular.ttf",
    "App.xbf"
  )
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $MsiPath)) {
  throw "no MSI at $MsiPath"
}
$MsiPath = (Resolve-Path $MsiPath).Path

# --- read the File table via the Windows Installer COM object ----------------
$installer = New-Object -ComObject WindowsInstaller.Installer
# msiOpenDatabaseModeReadOnly = 0
$db = $installer.GetType().InvokeMember(
  "OpenDatabase", "InvokeMethod", $null, $installer, @($MsiPath, 0))
$sql = "SELECT File, FileName, FileSize FROM File"
$view = $db.GetType().InvokeMember("OpenView", "InvokeMethod", $null, $db, @($sql))
$view.GetType().InvokeMember("Execute", "InvokeMethod", $null, $view, $null)

$longNames = @()
$fileCount = 0
while ($true) {
  $record = $view.GetType().InvokeMember("Fetch", "InvokeMethod", $null, $view, $null)
  if ($null -eq $record) { break }
  $fileCount++
  $fileName = $record.GetType().InvokeMember("StringData", "GetProperty", $null, $record, 2)
  # FileName is "shortname|longname" when the two differ (the common case
  # for the harvested payload), or just "shortname" when they are the same
  # (e.g. wintun.dll, already 8.3-safe). Always take the long form.
  $longName = ($fileName -split '\|')[-1]
  $longNames += $longName
}

Write-Host "MSI file table: $fileCount files ($MsiPath)"

# --- assert: floor on total payload count -------------------------------------
$failures = @()
if ($fileCount -lt $MinFileCount) {
  $failures += "file count $fileCount is below the floor of $MinFileCount -- " +
    "this is the exact shape of the defect that shipped a non-starting MSI " +
    "(it had 7 files). Check that installer/Package.wxs's RuntimeFiles " +
    "<Files> harvest still points at a populated `$(var.BinDir) and that " +
    "-p:BinDir was passed to the wixproj build."
}

# --- assert: critical names present -------------------------------------------
foreach ($name in $RequireNames) {
  if ($longNames -notcontains $name) {
    $failures += "required file '$name' is not in the MSI payload"
  }
}

if ($failures.Count -gt 0) {
  Write-Host "MSI payload verification FAILED:" -ForegroundColor Red
  foreach ($f in $failures) { Write-Host "  - $f" -ForegroundColor Red }
  throw "MSI payload verification failed ($($failures.Count) issue(s)); see above"
}

Write-Host "MSI payload verification passed: $fileCount files, all $($RequireNames.Count) required names present." -ForegroundColor Green
