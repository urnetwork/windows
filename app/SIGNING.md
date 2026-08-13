# Code signing pipelines (R10)

Two separate signing tracks. Both are release infrastructure that needs certs +
Partner Center accounts (can't run from the build host); the scripts below are
the shape of the pipeline to stand up on the signing box.

## 1. App installer + binaries — Authenticode (OV)

The MSI, and every PE inside it (URnetwork.exe, urnetworkd.exe, URnetworkSdk.dll),
are Authenticode-signed with a standard/OV code-signing cert. EV is NOT required
for the app; Microsoft does not re-sign EXE/MSI, so our signature is what ships.
Register the cert to the Partner Center account for Store submission.

```powershell
# sign-app.ps1 — run on the signing box (cert in the cert store or a token)
param([string]$BinDir, [string]$Thumbprint, [string]$TimestampUrl = "http://timestamp.digicert.com")
$sign = { param($f) signtool sign /sha1 $Thumbprint /fd SHA256 /tr $TimestampUrl /td SHA256 $f }
# sign the PE files first, then the MSI that contains them
Get-ChildItem $BinDir -Include *.exe,*.dll -Recurse | ForEach-Object { & $sign $_.FullName }
& $sign "$BinDir\URnetwork.msi"
signtool verify /pa /all "$BinDir\URnetwork.msi"
```

Keep the signer identity stable across releases: the tray icon GUID registration
is bound to the exe path + signer (plan R5), and Store update continuity expects
a consistent publisher.

## 2. Split-tunnel kernel driver — attestation signing (EV + Partner Center)

`SplitTunnel.sys` is a kernel driver: Windows 10 1607+ only loads kernel drivers
signed by Microsoft. We use **attestation signing** (no HLK tests, Windows 10/11
client, our target) via the Partner Center **Hardware Dev Center** dashboard.

> **⚠ Policy risk — verify before committing to this track (flagged 2026-08-08).**
> Microsoft's [Driver Signing Options](https://learn.microsoft.com/en-us/windows-hardware/drivers/dashboard/driver-signing-offerings)
> page (revised 2026-03-23) retitled this section **"Attestation signed drivers for
> testing scenarios"** and states *"For testing purposes only."* Separately, from
> April 2026 only WHCP-signed drivers (or an allowlist entry) load by default on
> Windows 11 24H2/25H2/26H1.
>
> **Verified:** that April change targets *cross-signed* drivers, not attestation, and
> attestation-signed drivers still load today. Nothing in the enumerated restrictions
> blocks us (Win10+ client, we ship the `.sys` in our MSI).
>
> **Unverified, and the reason to be nervous:** no primary source says attestation
> survives as a *retail* path. Two independent research passes reached this same
> conclusion. **The next action on the driver is not code — it is one person spending
> ~1 week getting a written answer from Microsoft.** That de-risks a 10–16 eng-week
> milestone. Do not start driver work before that answer. Tracked as task #23.
>
> Note this affects **only** the split-tunnel driver. The WFP leak-prevention and
> kill-switch work is user-mode `fwpuclnt` and needs no driver, no WDK and no
> attestation — Mullvad adds even its boot-time/persistent filters from user mode.

Requirements (start in M0 — lead times):
- A **Partner Center account with an EV code-signing certificate** attached to
  the Hardware Dev Center (the EV cert is required to access attestation
  signing, not to sign the driver yourself).
- Note: attestation-signed retail drivers are **not** distributed via Windows
  Update — that's fine, we ship the `.sys` inside our MSI.

Pipeline:

```powershell
# attest-driver.ps1 — build the CAB, submit for attestation, retrieve the signed sys
param([string]$SysPath, [string]$Arch = "amd64")
# 1. EV-sign the driver + build a CAB with the .sys + .inf (+ .cat placeholder)
$ddf = @"
.OPTION EXPLICIT
.Set CabinetNameTemplate=SplitTunnel.cab
.Set DiskDirectory1=out
$SysPath
$(Split-Path $SysPath)\SplitTunnel.inf
"@
$ddf | Set-Content driver.ddf
makecab /f driver.ddf
signtool sign /fd SHA256 /a /tr http://timestamp.digicert.com /td SHA256 out\SplitTunnel.cab
# 2. Upload out\SplitTunnel.cab to Partner Center > Hardware > "Drivers" as a new
#    shipping label, target "Windows 10/11 Client (attestation)", per-arch.
#    (Manual/API step; see the hardware dashboard.)
# 3. Download the Microsoft-signed driver package; extract the signed .sys/.cat
#    into third_party or the installer stage for that arch.
```

Do this per architecture (amd64, arm64). The signed `.sys` + `.cat` then go into
the MSI's driver feature (installer/Package.wxs `SplitTunnelDriver` component).

## Verification

- `signtool verify /pa /all <file>` for the app track.
- For the driver, confirm the returned package is Microsoft-signed and loads
  without test-signing mode on a clean Windows 10 + Windows 11 VM before ship.
