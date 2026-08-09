# Capture the machine's network state as byte-comparable CSV, so a P7 gate can
# be proved to have left nothing behind.
#
# Usage (UNELEVATED is fine and is what the baseline should be taken with):
#   powershell -NoProfile -File capture-netstate.ps1 -OutDir <dir> [-Label <name>]
#
# Everything here is READ-ONLY. No cmdlet in this file mutates network state.
#
# Design notes — why each field is present or absent:
#
#   * CSV, not Format-Table. Format-Table -AutoSize sizes its columns from the
#     widest value in THAT run, so one long interface alias appearing later
#     re-indents the whole table and the diff lights up everywhere. CSV is
#     field-delimited and therefore positionally stable.
#   * Explicit Select-Object, never `*`. The default property sets carry
#     ValidLifetime / PreferredLifetime, which are DHCP countdown TimeSpans:
#     they change every single second and make a diff worthless. They are
#     excluded here, deliberately.
#   * Explicit Sort-Object on every capture. Get-NetRoute and friends do not
#     guarantee row order between calls.
#   * ServerAddresses is an array; it is joined so one resolver set is one cell.
#
# Fields that still legitimately move between runs (expect these in a diff and
# do not treat them as tunnel residue):
#   * Get-NetAdapter Status / LinkSpeed  — flips on any link event.
#   * IPv6 routes and addresses on fe80::/64 and /128 host routes for temporary
#     privacy addresses — Windows rotates these on its own schedule.
#   * ifIndex on an adapter that was disabled and re-enabled between captures.
#
# SPDX-License-Identifier: MPL-2.0
[CmdletBinding()]
param(
  [Parameter(Mandatory = $true)][string]$OutDir,
  [string]$Label = ''
)

$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

# Advanced function: a plain `function Save($obj,$name)` does NOT accept
# pipeline input — $obj binds to $null and every capture silently writes an
# empty file. That failure looks exactly like "nothing changed", which is the
# one wrong answer this whole script exists to avoid.
function Save {
  [CmdletBinding()]
  param(
    [Parameter(ValueFromPipeline = $true)]$InputRow,
    [Parameter(Mandatory = $true)][string]$Name
  )
  begin { $rows = New-Object System.Collections.ArrayList }
  process { if ($null -ne $InputRow) { [void]$rows.Add($InputRow) } }
  end {
    $path = Join-Path $OutDir $Name
    if ($rows.Count -eq 0) {
      # An explicit empty marker, never a zero-byte file: "no rows" and "the
      # cmdlet was not available on this SKU" must not look the same.
      '"#empty"' | Out-File -FilePath $path -Encoding utf8
    } else {
      $rows | ConvertTo-Csv -NoTypeInformation | Out-File -FilePath $path -Encoding utf8
    }
    Write-Verbose ("{0}: {1} rows" -f $Name, $rows.Count)
  }
}

# --- routes ----------------------------------------------------------------
# The primary artifact. P7 step 6/8 installs 31 IPv4 prefixes on the tun LUID;
# a clean revert means this file is byte-identical to the baseline.
foreach ($fam in 'IPv4', 'IPv6') {
  Get-NetRoute -AddressFamily $fam -ErrorAction SilentlyContinue |
    Select-Object AddressFamily, ifIndex, InterfaceAlias, DestinationPrefix,
                  NextHop, RouteMetric, InterfaceMetric, Protocol, Publish, Store |
    Sort-Object Store, AddressFamily, DestinationPrefix, ifIndex, NextHop |
    Save -name "route-$fam.csv"
}

# --- DNS -------------------------------------------------------------------
# Per-adapter, because that is how Windows resolves and therefore how R6 leaks.
Get-DnsClientServerAddress -ErrorAction SilentlyContinue |
  Select-Object InterfaceIndex, InterfaceAlias, AddressFamily,
                @{n = 'ServerAddresses'; e = { ($_.ServerAddresses -join ';') } } |
  Sort-Object InterfaceIndex, AddressFamily |
  Save -name 'dns-server-address.csv'

# NRPT is one of the two candidate R6 fixes. Capture it so we can prove no rule
# was left behind if it is chosen.
Get-DnsClientNrptRule -ErrorAction SilentlyContinue |
  Select-Object Name, Namespace, NameServers, DnsSecEnable, DisplayName |
  Sort-Object Name |
  Save -name 'dns-nrpt-rule.csv'

Get-DnsClientNrptPolicy -ErrorAction SilentlyContinue |
  Select-Object Namespace, NameServers, QueryPolicy |
  Sort-Object Namespace |
  Save -name 'dns-nrpt-policy.csv'

# --- adapters and addresses -------------------------------------------------
Get-NetAdapter -IncludeHidden -ErrorAction SilentlyContinue |
  Select-Object ifIndex, Name, InterfaceDescription, Status, MacAddress,
                LinkSpeed, MtuSize, Virtual, Hidden |
  Sort-Object ifIndex |
  Save -name 'adapter.csv'

# Deliberately NOT Get-NetIPConfiguration for the diffable copy: it emits a
# formatted view whose shape depends on which fields happen to be populated.
# Its structured equivalents are below; a human-readable copy is written too.
Get-NetIPAddress -ErrorAction SilentlyContinue |
  Select-Object AddressFamily, ifIndex, InterfaceAlias, IPAddress,
                PrefixLength, PrefixOrigin, SuffixOrigin, Type, Store |
  Sort-Object Store, AddressFamily, ifIndex, IPAddress |
  Save -name 'ip-address.csv'

# Per-interface metric and MTU. Step 6/8 sets the TUN's Metric to 1 and
# UseAutomaticMetric to false (NetworkConfig.cpp:154-157); this file proves no
# PHYSICAL interface's metric or MTU was touched.
Get-NetIPInterface -ErrorAction SilentlyContinue |
  Select-Object AddressFamily, ifIndex, InterfaceAlias, NlMtu,
                InterfaceMetric, AutomaticMetric, Forwarding, Dhcp,
                ConnectionState, Store |
  Sort-Object Store, AddressFamily, ifIndex |
  Save -name 'ip-interface.csv'

# --- human-readable companion ----------------------------------------------
# Not for diffing. For the owner to eyeball mid-gate without parsing CSV.
$human = Join-Path $OutDir 'netipconfiguration.txt'
Get-NetIPConfiguration -All -Detailed -ErrorAction SilentlyContinue |
  Out-String -Width 200 | Out-File -FilePath $human -Encoding utf8

# --- manifest ---------------------------------------------------------------
# Timestamp lives HERE and nowhere else, so no diffable file carries a clock.
@(
  "label       : $Label"
  "captured    : $(Get-Date -Format 'o')"
  "host        : $env:COMPUTERNAME"
  "user        : $env:USERNAME"
  "elevated    : $((New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())).IsInRole([Security.Principal.WindowsBuiltinRole]::Administrator))"
  "urnetworkd  : $(@(Get-Process urnetworkd -ErrorAction SilentlyContinue).Count) running"
  "tun present : $([bool](Get-NetAdapter -Name 'URnetwork' -ErrorAction SilentlyContinue))"
  "marker      : $(Test-Path 'C:\ProgramData\URnetwork\service\tunnel_active')"
) | Out-File -FilePath (Join-Path $OutDir 'manifest.txt') -Encoding utf8

Write-Output "captured to $OutDir"
