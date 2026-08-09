# P7 elevated-session helpers - gates A..G.
#
#   . .\p7-gates.ps1        (dot-source it; then call the functions below)
#
# NOTHING IN THIS FILE IS DESTRUCTIVE. Every function is read-only except
# Invoke-P7Abort, which runs the documented escape hatch and refuses unless the
# shell is already elevated. The destructive steps are typed BY THE OWNER, by
# hand, and are listed in the gate comments so each one is a deliberate act.
#
# The owner is at the keyboard. Two shells:
#   SHELL 1 (elevated) - runs urnetworkd.
#   SHELL 2 (elevated, ALREADY OPEN before gate D) - the escape hatch. Opening a
#           new elevated shell needs UAC, which needs a responsive desktop; do
#           not plan to open it after something has gone wrong.
#
# SPDX-License-Identifier: MPL-2.0

$script:P7Root     = Split-Path -Parent $MyInvocation.MyCommand.Path
$script:P7Baseline = Join-Path $script:P7Root 'baseline'
$script:P7Exe      = 'C:\Users\ryanm\Downloads\claude_sandbox_windows\app\build\x64\Release\urnetworkd.exe'
$script:P7Log      = 'C:\ProgramData\URnetwork\service\logs\urnetworkd.log'
$script:P7Marker   = 'C:\ProgramData\URnetwork\service\tunnel_active'
$script:P7TunAlias = 'URnetwork'
# net::kTunCaptureV4Count - DERIVED from net::kLocalBypassV4 (Service/NetPolicy.h)
# and static_assert'd at 31. `urnetworkd selftest` re-checks it, so this number
# and the code cannot drift apart silently.
$script:P7RouteCnt = 31

# --- WFP leak-prevention layer (Service/WfpPolicy.cpp) ----------------------
# Fixed forever: the startup purge finds leftovers by provider GUID.
$script:P7WfpProvider           = '{6D2B1C40-9E77-4B58-8F5A-1C0E9D3A7B10}'
$script:P7WfpPersistentProvider = '{6D2B1C41-9E77-4B58-8F5A-1C0E9D3A7B10}'
# Filter counts as built by BuildFilterSet with the shipped defaults (LAN
# permitted, IPv6 blocked, LLMNR/mDNS/NetBIOS blocked).
# `urnetworkd selftest` prints all three; keep them in step.
#
# ARMED and CONNECTING are now DIFFERENT POLICIES (Service/WfpPolicy.h). Armed
# carries NO DNS permit at all - the port-53 hard block is in force with nothing
# lifted through it, so nothing on the machine resolves. Connecting is armed plus
# exactly one filter, urnetwork-permit-dns-host-resolver, and that is the whole
# difference: 39 - 38 = 1.
#
# Both are STABLE numbers however many resolvers the machine has, because filter
# 9b OR's the addresses as consecutive same-field conditions inside ONE filter.
$script:P7WfpCountArmed      = 38
$script:P7WfpCountConnecting = 39
#
# CONNECTED IS NOT A CONSTANT, and pinning it was a real defect in this file.
# Filter 10 emits ONE FILTER PER TUNNEL RESOLVER, so a session with two resolvers
# produces 44 and the old hardcoded 43 failed gate H2 on the count alone - a
# false failure, mid-session, on a policy that was correct.
#
# So it is DERIVED: base + one per tunnel-resolver permit actually in the dump.
# The base is everything except those permits (42 = 43 - 1 with one resolver).
# With NO usable tunnel resolver there is no DNS path, the port-53 block stands
# down by design, and two block filters go with it - hence the -2 below.
$script:P7WfpCountConnectedBase = 42
function Get-P7WfpExpectedConnected {
  param([Parameter(Mandatory=$true)][int]$TunnelResolverFilters)
  if ($TunnelResolverFilters -le 0) { return $script:P7WfpCountConnectedBase - 2 }
  return $script:P7WfpCountConnectedBase + $TunnelResolverFilters
}
# The platform host the service must be able to resolve. Built from
# ids::kNetworkSpaceHostName (Common/Ids.h); the SDK dials it BY NAME, so a
# machine that cannot resolve cannot connect and cannot reconnect.
$script:P7PlatformHost = 'ur.network'

function Test-P7Elevated {
  (New-Object Security.Principal.WindowsPrincipal(
    [Security.Principal.WindowsIdentity]::GetCurrent())
  ).IsInRole([Security.Principal.WindowsBuiltinRole]::Administrator)
}

# ---------------------------------------------------------------------------
# Assert-P7Baseline - the single most important check in this file.
# Captures current network state and diffs it against the pre-flight baseline.
# PASS means the machine is exactly as it was found. Run it after EVERY gate.
# ---------------------------------------------------------------------------
function Assert-P7Baseline {
  param([string]$Label = 'check')
  $tmp = Join-Path $env:TEMP ("p7-{0}-{1}" -f $Label, (Get-Date -Format 'HHmmss'))
  & powershell -NoProfile -ExecutionPolicy Bypass `
    -File (Join-Path $script:P7Root 'capture-netstate.ps1') -OutDir $tmp -Label $Label | Out-Null
  $bad = @()
  Get-ChildItem $script:P7Baseline -Filter *.csv | ForEach-Object {
    $now = Join-Path $tmp $_.Name
    if ((Get-Content $_.FullName -Raw) -cne (Get-Content $now -Raw)) {
      $bad += $_.Name
      Write-Host "  DIFFERS  $($_.Name)" -ForegroundColor Red
      Compare-Object (Get-Content $_.FullName) (Get-Content $now) |
        Select-Object -First 20 | Format-Table -AutoSize | Out-String -Width 190 | Write-Host
    } else {
      Write-Host "  same     $($_.Name)" -ForegroundColor DarkGray
    }
  }
  if ($bad.Count -eq 0) {
    Write-Host "PASS - network state is byte-identical to the baseline" -ForegroundColor Green
    Write-Host "       (capture: $tmp)" -ForegroundColor DarkGray
    return $true
  }
  Write-Host "FAIL - $($bad.Count) capture(s) differ: $($bad -join ', ')" -ForegroundColor Red
  Write-Host "       capture kept at $tmp" -ForegroundColor Yellow
  Write-Host "       ABORT: see Invoke-P7Abort" -ForegroundColor Yellow
  return $false
}

# ---------------------------------------------------------------------------
# Show-P7State - what the tunnel is doing right now. Read-only.
# ---------------------------------------------------------------------------
function Show-P7State {
  $procs = @(Get-Process urnetworkd -ErrorAction SilentlyContinue)
  $ad    = Get-NetAdapter -Name $script:P7TunAlias -ErrorAction SilentlyContinue
  Write-Host "urnetworkd pids : $($procs.Id -join ', ')"
  Write-Host "tun adapter     : $(if ($ad) { "ifIndex=$($ad.ifIndex) status=$($ad.Status)" } else { 'ABSENT' })"
  Write-Host "marker file     : $(Test-Path $script:P7Marker)"
  Write-Host "control pipe    : $([bool](Get-ChildItem '\\.\pipe\' | Where-Object Name -eq 'urnetwork.control'))"
  if ($ad) {
    $r = @(Get-NetRoute -InterfaceAlias $script:P7TunAlias -AddressFamily IPv4 -ErrorAction SilentlyContinue |
             Where-Object { $_.Store -eq 'ActiveStore' })
    Write-Host "tun v4 routes   : $($r.Count)  (expect $($script:P7RouteCnt) + on-link entries once step 6/8 has run)"
    Get-NetIPAddress -InterfaceAlias $script:P7TunAlias -ErrorAction SilentlyContinue |
      Select-Object AddressFamily, IPAddress, PrefixLength | Format-Table -AutoSize | Out-String | Write-Host
    Get-DnsClientServerAddress -InterfaceAlias $script:P7TunAlias -ErrorAction SilentlyContinue |
      Select-Object AddressFamily, @{n='Servers';e={$_.ServerAddresses -join ', '}} |
      Format-Table -AutoSize | Out-String | Write-Host
  }
  Write-Host "--- last 15 log lines ---"
  Get-Content $script:P7Log -Tail 15 | Write-Host
}

# ---------------------------------------------------------------------------
# Assert-P7GateC - R1 socket self-exclusion. THE GATE THAT MATTERS MOST.
#
# Mechanism (connect/egress_windows.go:48-79): IP_UNICAST_IF / IPV6_UNICAST_IF
# setsockopt, i.e. a forced egress INTERFACE INDEX. Not a source-address bind
# and not a route metric. A pinned socket ignores the tun's routes entirely, so
# the observable signature is that every urnetworkd socket carries a LOCAL
# ADDRESS ON THE PHYSICAL NIC and never the tun's 169.254.2.1.
# ---------------------------------------------------------------------------
function Assert-P7GateC {
  param([Parameter(Mandatory=$true)][int]$ServicePid)
  $tun = Get-NetIPAddress -InterfaceAlias $script:P7TunAlias -AddressFamily IPv4 -ErrorAction SilentlyContinue
  $tunAddr = if ($tun) { $tun.IPAddress } else { '169.254.2.1' }

  Write-Host "=== egress: log line (must name the PHYSICAL adapter) ==="
  Select-String -Path $script:P7Log -Pattern 'egress: bound to|\[2/8\]' |
    Select-Object -Last 6 | ForEach-Object { Write-Host "  $($_.Line)" }

  Write-Host "`n=== TCP sockets owned by pid $ServicePid ==="
  $tcp = @(Get-NetTCPConnection -OwningProcess $ServicePid -ErrorAction SilentlyContinue)
  $tcp | Select-Object LocalAddress, LocalPort, RemoteAddress, RemotePort, State |
    Sort-Object RemoteAddress | Format-Table -AutoSize | Out-String -Width 160 | Write-Host

  $leaked = @($tcp | Where-Object { $_.LocalAddress -eq $tunAddr })
  # Loopback is expected and fine: the mTLS device-RPC listener lives on 127.0.0.1.
  $external = @($tcp | Where-Object { $_.RemoteAddress -notmatch '^(127\.|0\.0\.0\.0|::)' })

  Write-Host "tun local address      : $tunAddr"
  Write-Host "external TCP sockets   : $($external.Count)"
  Write-Host "sourced from the tun   : $($leaked.Count)"
  if ($leaked.Count -eq 0 -and $external.Count -gt 0) {
    Write-Host "PASS - no urnetworkd socket is sourced from the tun; R1 holds" -ForegroundColor Green
    return $true
  }
  if ($external.Count -eq 0) {
    Write-Host "INCONCLUSIVE - no external socket yet; wait for the platform connection and re-run" -ForegroundColor Yellow
    return $false
  }
  Write-Host "FAIL - $($leaked.Count) socket(s) sourced from the tun: the service is looping into its own tunnel" -ForegroundColor Red
  Write-Host "       ABORT NOW (Ctrl+C in shell 1), then Invoke-P7Abort" -ForegroundColor Red
  return $false
}

# ---------------------------------------------------------------------------
# Invoke-P7SelfTest - the unelevated half, and the ONLY half an agent can run.
#
# Pure logic: the shared route/firewall table and the WFP filter-set
# construction for every state. It opens no adapter, writes no route and never
# contacts the Base Filtering Engine. It CANNOT prove a filter blocks anything.
# Everything below this function is the half it cannot reach.
# ---------------------------------------------------------------------------
function Invoke-P7SelfTest {
  # Out-Host, not the pipeline: the transcript is the point, and letting the
  # exe's stdout join this function's return value makes a failing run look
  # truthy to `if (Invoke-P7SelfTest)`.
  & $script:P7Exe selftest | Out-Host
  $ok = $LASTEXITCODE -eq 0
  if ($ok) { Write-Host "PASS - selftest" -ForegroundColor Green }
  else     { Write-Host "FAIL - selftest; do not proceed to any gate" -ForegroundColor Red }
  return $ok
}

# ---------------------------------------------------------------------------
# Get-P7WfpDump - `netsh wfp show filters` to a file. NEEDS ELEVATION.
# Read-only. Every WFP assertion below reads one of these dumps rather than
# poking the engine, so the artefact survives for the report.
# ---------------------------------------------------------------------------
function Get-P7WfpDump {
  param([string]$Label = 'wfp')
  if (-not (Test-P7Elevated)) {
    Write-Host "netsh wfp show filters needs an elevated shell." -ForegroundColor Yellow
    return $null
  }
  $out = Join-Path $env:TEMP ("wfp-{0}-{1}.xml" -f $Label, (Get-Date -Format 'HHmmss'))
  & netsh wfp show filters file=$out | Out-Null
  if (-not (Test-Path $out)) {
    Write-Host "FAIL - netsh produced no dump" -ForegroundColor Red
    return $null
  }
  Write-Host "  dump: $out" -ForegroundColor DarkGray
  return $out
}

function Get-P7WfpFilterNames {
  param([Parameter(Mandatory=$true)][string]$Dump)
  # Every filter this policy installs is named 'urnetwork-<something>', and
  # nothing else on the machine uses that prefix. Provider/sublayer display
  # names are capitalised ('URnetwork baseline'), so the lowercase prefix
  # selects filters only.
  $text = Get-Content $Dump -Raw
  [regex]::Matches($text, 'urnetwork-[a-z0-9\-]+') | ForEach-Object { $_.Value }
}

# ---------------------------------------------------------------------------
# GATE H1 - nothing of ours is in the filter engine.
#
# Run BEFORE arming anything and AGAIN after every teardown and every crash
# test. This is the WFP equivalent of Assert-P7Baseline, and it is the check
# that proves the dynamic-session guarantee actually holds on this machine.
# ---------------------------------------------------------------------------
function Assert-P7WfpAbsent {
  param([string]$Label = 'absent')
  $dump = Get-P7WfpDump -Label $Label
  if (-not $dump) { return $false }
  $names = @(Get-P7WfpFilterNames -Dump $dump)
  $prov  = @(Select-String -Path $dump -Pattern ([regex]::Escape($script:P7WfpProvider)) -SimpleMatch)
  $pprov = @(Select-String -Path $dump -Pattern ([regex]::Escape($script:P7WfpPersistentProvider)) -SimpleMatch)
  Write-Host "urnetwork filters      : $($names.Count)"
  Write-Host "provider GUID hits     : $($prov.Count)"
  Write-Host "PERSISTENT provider    : $($pprov.Count)   <-- must ALWAYS be 0; this build never installs it"
  if ($names.Count -eq 0 -and $prov.Count -eq 0 -and $pprov.Count -eq 0) {
    Write-Host "PASS - the filter engine holds nothing of ours" -ForegroundColor Green
    return $true
  }
  Write-Host "FAIL - leftover URnetwork filter-engine objects" -ForegroundColor Red
  Write-Host "       ABORT: stop every urnetworkd (the dynamic session dies with the" -ForegroundColor Yellow
  Write-Host "       process), then from an elevated shell:  & '$($script:P7Exe)' revert" -ForegroundColor Yellow
  Write-Host "       If that does not clear it, the objects are NOT from a dynamic" -ForegroundColor Yellow
  Write-Host "       session and the nuclear option is: net stop bfe ; net start bfe" -ForegroundColor Yellow
  return $false
}

# ---------------------------------------------------------------------------
# GATE H2 - the policy that is in force is the policy that was designed.
#
#   Assert-P7WfpInstalled -Expect connected      (tunnel up)
#   Assert-P7WfpInstalled -Expect connecting     (a connect attempt IN FLIGHT)
#   Assert-P7WfpInstalled -Expect armed          (kill switch on, tunnel down)
#
# The counts come from `urnetworkd selftest`, which builds the same filter set
# in-process. A mismatch means the machine got a different policy than the unit
# test verified - which is exactly the gap between "the code is right" and "the
# machine is protected".
#
# ARMED is the one that changed. It no longer carries a DNS permit of any kind,
# so the check for it is now the INVERSE of what it used to be: the host-resolver
# permit being present while armed is a FAILURE, because that permit is
# address-scoped and therefore machine-wide - every process on the box could
# resolve in plaintext for as long as the kill switch stayed on.
# ---------------------------------------------------------------------------
function Assert-P7WfpInstalled {
  param([Parameter(Mandatory=$true)]
        [ValidateSet('armed','connecting','connected')][string]$Expect)
  $dump = Get-P7WfpDump -Label $Expect
  if (-not $dump) { return $false }
  $names    = @(Get-P7WfpFilterNames -Dump $dump)
  $tunnelResolvers = @($names | Where-Object { $_ -eq 'urnetwork-permit-dns-tunnel-resolver' }).Count
  $expected = switch ($Expect) {
    'connected'  { Get-P7WfpExpectedConnected -TunnelResolverFilters $tunnelResolvers }
    'connecting' { $script:P7WfpCountConnecting }
    default      { $script:P7WfpCountArmed }
  }

  $required = @(
    'urnetwork-permit-service-v4','urnetwork-permit-service-v6',
    'urnetwork-permit-service-dns-v4',
    'urnetwork-permit-loopback-v4','urnetwork-permit-loopback-v6',
    'urnetwork-permit-lan-out','urnetwork-permit-lan-in',
    'urnetwork-permit-dhcpv4-out','urnetwork-permit-dhcpv4-in',
    'urnetwork-permit-ndp-rs-out','urnetwork-permit-ndp-ra-in',
    'urnetwork-lift-dns-v4',
    'urnetwork-block-all-v4-out','urnetwork-block-all-v4-in',
    'urnetwork-block-all-v6-out','urnetwork-block-all-v6-in',
    'urnetwork-block-llmnr-v4','urnetwork-block-mdns-v4','urnetwork-block-netbios-v4'
  )
  # The port-53 hard block is required UNCONDITIONALLY in armed - that is the
  # whole change. In the two connecting states it is coupled to a usable DNS
  # path, so its absence there is reported as DEGRADED rather than as missing
  # (see Assert-P7ServiceDns); it is still required whenever a path exists.
  if ($Expect -eq 'armed') {
    $required += @('urnetwork-block-dns-v4','urnetwork-block-dns-v6')
  } elseif ($names -contains 'urnetwork-permit-dns-host-resolver' -or $tunnelResolvers -gt 0) {
    $required += @('urnetwork-block-dns-v4','urnetwork-block-dns-v6')
  }
  if ($Expect -eq 'connected') {
    $required += @('urnetwork-permit-tun-v4','urnetwork-permit-tun-v6')
  } elseif ($Expect -eq 'connecting') {
    # The DNS path a connection attempt runs on. Its absence is the blocking
    # defect: the port-53 block is in force, the only other DNS permit is scoped
    # on the app id of urnetworkd.exe, and our name resolution is issued by
    # svchost.exe - so nothing matches, the attempt cannot resolve, and arming
    # becomes a one-way door. See Assert-P7ServiceDns.
    $required += @('urnetwork-permit-dns-host-resolver')
  }
  $missing = @($required | Where-Object { $names -notcontains $_ })
  $unexpected = @()
  if ($Expect -eq 'armed') {
    # BOTH are failures here. No tun exists, and no DNS permit may be installed
    # while merely idle - that is the Armed/Connecting split, and this is the
    # only place a machine can prove it.
    $unexpected = @($names | Where-Object { $_ -like 'urnetwork-permit-tun-*' -or
                                            $_ -eq 'urnetwork-permit-dns-tunnel-resolver' -or
                                            $_ -eq 'urnetwork-permit-dns-host-resolver' })
  } elseif ($Expect -eq 'connecting') {
    $unexpected = @($names | Where-Object { $_ -like 'urnetwork-permit-tun-*' -or
                                            $_ -eq 'urnetwork-permit-dns-tunnel-resolver' })
  } else {
    # Connected must NOT carry it: the tunnel's own resolvers are the path
    # there, and permitting the physical adapter's resolvers on top of them is
    # exactly the R6 leak this sublayer exists to close.
    $unexpected = @($names | Where-Object { $_ -eq 'urnetwork-permit-dns-host-resolver' })
  }

  if ($Expect -eq 'connected') {
    Write-Host "tunnel resolvers  : $tunnelResolvers  (filter 10 emits ONE PER resolver)"
  }
  Write-Host "filters installed : $($names.Count)  (expect $expected)"
  Write-Host "missing           : $(if ($missing.Count) { $missing -join ', ' } else { 'none' })"
  if ($unexpected.Count) {
    Write-Host "UNEXPECTED        : $($unexpected -join ', ')" -ForegroundColor Red
    Write-Host "  An armed policy must not permit a tun that does not exist, must not" -ForegroundColor Red
    Write-Host "  carry ANY DNS permit while idle, and a connected policy must not" -ForegroundColor Red
    Write-Host "  permit the physical adapter's resolvers." -ForegroundColor Red
    if ($unexpected -contains 'urnetwork-permit-dns-host-resolver' -and $Expect -eq 'armed') {
      Write-Host "  urnetwork-permit-dns-host-resolver WHILE ARMED means the DNS window" -ForegroundColor Red
      Write-Host "  never closed. It is address-scoped and cannot be scoped to us, so" -ForegroundColor Red
      Write-Host "  EVERY process on this machine can resolve in plaintext right now." -ForegroundColor Red
      Write-Host "  Either a connect attempt is still in flight (use -Expect connecting)" -ForegroundColor Red
      Write-Host "  or the return to armed did not happen - check the log for the" -ForegroundColor Red
      Write-Host "  'wfp: DNS WINDOW CLOSED' line and for the connecting watchdog." -ForegroundColor Red
    }
  }
  if ($missing.Count -eq 0 -and $unexpected.Count -eq 0 -and $names.Count -eq $expected) {
    Write-Host "PASS - the installed policy matches the designed $Expect policy" -ForegroundColor Green
    return $true
  }
  Write-Host "FAIL - the installed policy is not the designed one" -ForegroundColor Red
  Write-Host "       ABORT: Ctrl+C in shell 1, then Assert-P7WfpAbsent." -ForegroundColor Yellow
  return $false
}

# ---------------------------------------------------------------------------
# GATE H3 - THE SERVICE CAN STILL RESOLVE. Run it in EACH of connecting, armed
# and connected.
#
# Why this gate exists. The DNS sublayer hard-blocks remote port 53 and then
# permits our own service back in - scoped on the app id of urnetworkd.exe.
# That permit does not match the traffic. The SDK is Go, and Go on Windows
# resolves through the OS resolver (net/conf.go returns the fallback order
# unconditionally for GOOS=windows, i.e. net/lookup_windows.go's GetAddrInfoW),
# which is an RPC into the DNS Client service. The wire query is issued by
# svchost.exe, not by urnetworkd.exe, so an app-id permit never sees it.
#
# Verified unelevated on this machine, 2026-08-08: a getaddrinfo lookup from an
# arbitrary process opened ZERO UDP endpoints in that process and its result
# landed in the OS-wide DNS Client cache, which only the Dnscache service
# maintains. Dnscache runs as 'svchost.exe -k NetworkService -p'.
#
# The consequence, if this gate fails: kill switch on, tunnel drops, policy
# holds at armed, the service cannot resolve the platform host, so it cannot
# reconnect, so the policy never widens. The machine stays blocked with no way
# back except stopping the service or an elevated revert. That is the rank-1
# unrecoverable state, and it is reached by the ordinary use of the feature.
#
# The fix under test is filter 9b, urnetwork-permit-dns-host-resolver: an
# ADDRESS-scoped permit for the host's own IPv4 resolvers. It is coupled to the
# block in code - in the CONNECTING states BuildFilterSet installs the port-53
# block only when a usable path exists - so a missing permit shows up there as a
# MISSING BLOCK, not as a dead machine. Check both.
#
# ARMED IS NOW THE OPPOSITE ASSERTION, AND THAT IS DELIBERATE. The permit cannot
# be scoped to our process, so leaving it installed while merely idle let every
# process on the machine resolve in plaintext for as long as the kill switch was
# on. The owner's decision (option C, 2026-08-08) was that an idle kill switch
# opens nothing: armed has NO permit and DOES carry the block, and the hole is
# opened per connection attempt instead. So in state 'armed' this gate now
# requires that NOTHING RESOLVES. A successful lookup while armed is the failure.
#
#   Assert-P7ServiceDns -State connecting   (during a connect, window OPEN)
#   Assert-P7ServiceDns -State armed        (kill switch on, tunnel down, CLOSED)
#   Assert-P7ServiceDns -State connected    (tunnel up)
# ---------------------------------------------------------------------------
function Assert-P7ServiceDns {
  param([Parameter(Mandatory=$true)]
        [ValidateSet('connecting','armed','connected')][string]$State)
  $fail = @()

  # --- 0. the premise: our name resolution really does leave another process --
  Write-Host "=== the resolver our own lookups actually use ===" -ForegroundColor Cyan
  $dnscache = Get-CimInstance Win32_Service -Filter "Name='Dnscache'" -ErrorAction SilentlyContinue
  if ($dnscache -and $dnscache.State -eq 'Running') {
    Write-Host "  Dnscache          : Running in pid $($dnscache.ProcessId)"
    Write-Host "  host image        : $($dnscache.PathName)"
    Write-Host "  -> GetAddrInfoW is an RPC into THAT process. The app-id permit"
    Write-Host "     for urnetworkd.exe cannot match the wire query."
  } else {
    Write-Host "  Dnscache is NOT running. With the DNS Client service stopped," -ForegroundColor Yellow
    Write-Host "  GetAddrInfoW resolves in-process and the app-id permit WOULD" -ForegroundColor Yellow
    Write-Host "  match. This gate is then testing a different machine than the" -ForegroundColor Yellow
    Write-Host "  one users have. Start it and re-run." -ForegroundColor Yellow
  }

  # --- 1. the filters that must be present in this state --------------------
  $dump = Get-P7WfpDump -Label ("dns-" + $State)
  if (-not $dump) { return $false }
  $names = @(Get-P7WfpFilterNames -Dump $dump)

  if ($State -eq 'armed') {
    # ---- ARMED: the assertion is that there is NO path ---------------------
    Write-Host "`n=== armed must have NO DNS path at all ===" -ForegroundColor Cyan
    if ($names -contains 'urnetwork-permit-dns-host-resolver') {
      Write-Host "  FAIL  - urnetwork-permit-dns-host-resolver is INSTALLED while" -ForegroundColor Red
      Write-Host "          armed. That permit is address-scoped and cannot be scoped" -ForegroundColor Red
      Write-Host "          to us, so every process on this machine can resolve in" -ForegroundColor Red
      Write-Host "          plaintext right now, with nothing connecting." -ForegroundColor Red
      $fail += 'permit-present-while-idle'
    } else {
      Write-Host "  pass  - no host-resolver permit" -ForegroundColor Green
    }
    if ($names -contains 'urnetwork-permit-dns-tunnel-resolver') {
      Write-Host "  FAIL  - a tunnel-resolver permit is installed with no tunnel." -ForegroundColor Red
      $fail += 'tunnel-permit-while-armed'
    }
    if ($names -contains 'urnetwork-block-dns-v4' -and
        $names -contains 'urnetwork-block-dns-v6') {
      Write-Host "  pass  - the port-53 hard block is in force at both layers" -ForegroundColor Green
    } else {
      # Under the old semantics a missing block while armed was the coupling
      # working. Under these it is a plain hole: armed installs the block
      # unconditionally, so its absence means the policy on this machine is NOT
      # the one BuildFilterSet describes.
      Write-Host "  FAIL  - the port-53 hard block is MISSING while armed. Armed" -ForegroundColor Red
      Write-Host "          installs it unconditionally now; the fail-safe stand-down" -ForegroundColor Red
      Write-Host "          belongs to the connecting states only. Plaintext DNS to" -ForegroundColor Red
      Write-Host "          ANY server is open right now." -ForegroundColor Red
      $fail += 'armed-block-missing'
    }
  } else {
    $needPermit = if ($State -eq 'connected') { 'urnetwork-permit-dns-tunnel-resolver' }
                  else                        { 'urnetwork-permit-dns-host-resolver' }

    Write-Host "`n=== the DNS path this state is supposed to have ===" -ForegroundColor Cyan
    Write-Host "  required permit   : $needPermit"
    if ($names -contains $needPermit) {
      Write-Host "  pass  - it is installed" -ForegroundColor Green
    } else {
      Write-Host "  FAIL  - it is MISSING. Nothing address-scoped permits port 53," -ForegroundColor Red
      Write-Host "          so our own resolution has no path in this state." -ForegroundColor Red
      $fail += 'permit-missing'
    }

    # The block and the permit are coupled in BuildFilterSet. A missing block is
    # not a relief, it is the code telling you it found no resolver to permit -
    # and plaintext DNS to ANY server is open for as long as that lasts.
    if ($names -contains 'urnetwork-block-dns-v4') {
      Write-Host "  pass  - the port-53 hard block is in force alongside it" -ForegroundColor Green
    } else {
      Write-Host "  DEGRADED - the port-53 hard block STOOD DOWN. The service found" -ForegroundColor Yellow
      Write-Host "          no usable IPv4 resolver on this host, so it chose an open" -ForegroundColor Yellow
      Write-Host "          DNS path over an attempt that cannot resolve. Plaintext" -ForegroundColor Yellow
      Write-Host "          DNS to any server is PERMITTED right now; everything else" -ForegroundColor Yellow
      Write-Host "          is still blocked, and it closes on the return to armed." -ForegroundColor Yellow
      Write-Host "          Look for the 'wfp: the port-53 hard block is NOT in" -ForegroundColor Yellow
      Write-Host "          force' warning in the service log." -ForegroundColor Yellow
      $fail += 'block-stood-down'
    }
  }

  # --- 2. the permitted addresses ARE the machine's resolvers ---------------
  # CONNECTING only. Armed permits nothing, so there is no address list to
  # compare; connected permits the tunnel's resolvers, not the host's.
  if ($State -eq 'connecting') {
    Write-Host "`n=== the permitted addresses vs what the OS is configured with ===" -ForegroundColor Cyan
    $tunIf = (Get-NetAdapter -Name $script:P7TunAlias -ErrorAction SilentlyContinue).ifIndex
    $osServers = @(Get-DnsClientServerAddress -AddressFamily IPv4 -ErrorAction SilentlyContinue |
      Where-Object { $_.InterfaceIndex -ne $tunIf } |
      ForEach-Object { $_.ServerAddresses } |
      Where-Object { $_ -and $_ -notmatch '^127\.' } |
      Sort-Object -Unique)
    # The service logs exactly what it read, so this compares the code's view of
    # the machine with the machine.
    $logged = @(Select-String -Path $script:P7Log -Pattern 'connecting-state DNS path' |
                Select-Object -Last 1)
    Write-Host "  OS resolvers (non-tun, non-loopback): $($osServers -join ', ')"
    if ($logged) { Write-Host "  service log: $($logged.Line)" }
    else { Write-Host "  service log: no 'connecting-state DNS path' line found yet" -ForegroundColor Yellow }
    Write-Host "  Every address above must appear in the log line and in the filter."
    Write-Host "  A resolver the OS uses but the filter does not name is a lookup"
    Write-Host "  that fails closed."
    # Adapters that are Up but whose resolver is IPv6 only cannot be permitted:
    # the v6 floor blocks all IPv6 regardless of what the DNS sublayer says.
    $v6only = @(Get-DnsClientServerAddress -AddressFamily IPv6 -ErrorAction SilentlyContinue |
      Where-Object { $_.ServerAddresses.Count -gt 0 -and $_.InterfaceIndex -ne $tunIf })
    if ($v6only.Count -gt 0 -and $osServers.Count -eq 0) {
      Write-Host "  FAIL  - this host has IPv6 resolvers and NO IPv4 ones. The v6" -ForegroundColor Red
      Write-Host "          floor blocks them whatever the DNS sublayer says, so" -ForegroundColor Red
      Write-Host "          there is no path at all. This is a known residual hole." -ForegroundColor Red
      $fail += 'v6-only-resolver'
    }
  }

  # --- 3. the live proof: resolve through the path the service uses ---------
  # This is the assertion. Everything above is the explanation.
  #
  # THE EXPECTED ANSWER DEPENDS ON THE STATE, and in 'armed' it is inverted. An
  # idle kill switch that still resolves is the defect this change removed, and
  # this is the only observable that proves it from outside the filter engine.
  Write-Host "`n=== can a name be resolved RIGHT NOW, through Dnscache? ===" -ForegroundColor Cyan
  Clear-DnsClientCache
  $probe = "p7-dns-" + (Get-Random) + ".example.com"
  $ok = $false
  try { Resolve-DnsName example.com -DnsOnly -ErrorAction Stop | Out-Null; $ok = $true } catch { }
  if ($State -eq 'armed') {
    if ($ok) {
      Write-Host "  FAIL  - example.com RESOLVED while armed. The kill switch is on," -ForegroundColor Red
      Write-Host "          nothing is connecting, and this machine is still able to" -ForegroundColor Red
      Write-Host "          send plaintext DNS. Either the port-53 block is not in" -ForegroundColor Red
      Write-Host "          force, or a DNS permit outlived a connection attempt." -ForegroundColor Red
      Write-Host "          NOTE: rule out the cache first - Clear-DnsClientCache ran," -ForegroundColor Red
      Write-Host "          but a resolver on the LAN reached via a permit that is not" -ForegroundColor Red
      Write-Host "          ours would also show here. Confirm at the wire (gate I)." -ForegroundColor Red
      $fail += 'resolves-while-armed'
    } else {
      Write-Host "  pass  - nothing resolves. That IS the armed policy: no DNS permit," -ForegroundColor Green
      Write-Host "          port-53 blocked, and no process on this machine - including" -ForegroundColor Green
      Write-Host "          the service - can look a name up until a connection attempt" -ForegroundColor Green
      Write-Host "          opens the window." -ForegroundColor Green
    }
  } elseif ($ok) {
    Write-Host "  pass  - example.com resolved" -ForegroundColor Green
  } else {
    Write-Host "  FAIL  - nothing resolves. The service cannot reach its platform" -ForegroundColor Red
    Write-Host "          host either, so it cannot connect and cannot reconnect." -ForegroundColor Red
    $fail += 'no-resolution'
  }
  # Attribute it, so a pass cannot come from something resolving in-process.
  try { [System.Net.Dns]::GetHostAddresses($probe) | Out-Null } catch { }
  $cached = @(Get-DnsClientCache -Entry $probe -ErrorAction SilentlyContinue)
  if ($cached.Count -gt 0) {
    Write-Host "  pass  - the lookup landed in the OS-wide DNS Client cache, so it" -ForegroundColor Green
    Write-Host "          went through Dnscache: the same path the service uses." -ForegroundColor Green
  } else {
    Write-Host "  note  - the probe did not land in the OS cache; attribution is" -ForegroundColor Yellow
    Write-Host "          unproven for this run (some negative answers are not cached)." -ForegroundColor Yellow
  }

  Write-Host "`n=== the platform host itself ===" -ForegroundColor Cyan
  foreach ($h in @($script:P7PlatformHost, ("api." + $script:P7PlatformHost), ("connect." + $script:P7PlatformHost))) {
    $r = $null
    try { $r = Resolve-DnsName $h -DnsOnly -ErrorAction Stop } catch { }
    Write-Host ("  {0,-24} {1}" -f $h, $(if ($r) { 'resolves' } else { 'NO ANSWER' }))
  }
  if ($State -eq 'armed') {
    Write-Host "  NO ANSWER is the CORRECT result here. Armed does not resolve, and"
    Write-Host "  the service does not need it to: it opens the window at the start"
    Write-Host "  of the next connection attempt (state connecting)."
  } else {
    Write-Host "  Not all of these need an A record. At least one must resolve, and"
    Write-Host "  the service log is the arbiter: step 4/8 is where it is used."
  }

  # --- 4. connecting: the log has to show a connect getting past step 4 -----
  if ($State -eq 'connecting') {
    Write-Host "`n=== a connect attempt that started WHILE ARMED got past step 4/8 ===" -ForegroundColor Cyan
    Write-Host "  Step 4/8 constructs DeviceLocal, which is the first thing that"
    Write-Host "  dials the platform by name. Reaching it proves resolution worked"
    Write-Host "  AFTER the policy widened from armed to connecting - which is the"
    Write-Host "  whole regression risk of splitting the two states, because armed"
    Write-Host "  alone cannot resolve at all any more."
    $log = @(Get-Content $script:P7Log -ErrorAction SilentlyContinue)
    $widenIdx = -1
    for ($i = 0; $i -lt $log.Count; $i++) {
      if ($log[$i] -match 'wfp: policy armed -> connecting') { $widenIdx = $i }
    }
    if ($widenIdx -lt 0) {
      Write-Host "  INCONCLUSIVE - no 'wfp: policy armed -> connecting' line in the" -ForegroundColor Yellow
      Write-Host "  log. Arm the kill switch, let the tunnel drop, then retry a" -ForegroundColor Yellow
      Write-Host "  connect. If a connect was attempted and this line never appeared," -ForegroundColor Yellow
      Write-Host "  that IS the regression: the attempt ran under the armed policy." -ForegroundColor Yellow
    } else {
      $after = @($log[$widenIdx..($log.Count - 1)])
      $reached = @($after | Where-Object { $_ -match '\[4/8\] device client_id=' })
      $stuck   = @($after | Where-Object { $_ -match 'start FAILED at step 3/8|start FAILED at step 4/8' })
      if ($reached.Count -gt 0) {
        Write-Host "  pass  - $($reached.Count) start(s) reached step 4/8 after the" -ForegroundColor Green
        Write-Host "          policy widened to connecting" -ForegroundColor Green
      } elseif ($stuck.Count -gt 0) {
        Write-Host "  FAIL  - a start died at step 3/8 or 4/8 after widening. That is" -ForegroundColor Red
        Write-Host "          the signature of name resolution being blocked." -ForegroundColor Red
        $stuck | Select-Object -Last 3 | ForEach-Object { Write-Host "          $_" -ForegroundColor Red }
        $fail += 'connect-blocked'
      } else {
        Write-Host "  INCONCLUSIVE - no start attempt since the policy widened." -ForegroundColor Yellow
      }
    }
  }

  if ($fail.Count -eq 0) {
    if ($State -eq 'armed') {
      Write-Host "`nPASS - armed resolves NOTHING, which is what it is for" -ForegroundColor Green
    } else {
      Write-Host "`nPASS - the service has a working DNS path in state '$State'" -ForegroundColor Green
    }
    return $true
  }
  Write-Host "`nFAIL - $($fail -join ', ')" -ForegroundColor Red
  Write-Host "       If this is 'permit-missing' or 'no-resolution' in CONNECTING," -ForegroundColor Yellow
  Write-Host "       DO NOT leave the machine armed: Ctrl+C in shell 1 (the dynamic" -ForegroundColor Yellow
  Write-Host "       session dies with the process and the network comes back), then" -ForegroundColor Yellow
  Write-Host "       Assert-P7WfpAbsent and Assert-P7Baseline." -ForegroundColor Yellow
  Write-Host "       'permit-present-while-idle' or 'resolves-while-armed' is the" -ForegroundColor Yellow
  Write-Host "       opposite failure: not a stuck machine, a leaking one. The kill" -ForegroundColor Yellow
  Write-Host "       switch is on and DNS is still leaving. Nothing needs aborting," -ForegroundColor Yellow
  Write-Host "       but the layer is not doing what the UI says it does." -ForegroundColor Yellow
  return $false
}

# ---------------------------------------------------------------------------
# GATE H4 - THE WINDOW OPENS FOR AN ATTEMPT AND CLOSES AFTER IT.  <<< new
#
# H2 and H3 photograph one state at a time. This one is about the EDGE, which is
# the only thing the Armed/Connecting split actually added and the only place it
# can go wrong in a way the static checks cannot see:
#
#   * it never opens          -> automatic reconnect is dead (steps 3-5 cannot
#                                resolve, every retry fails at 3/8 or 4/8)
#   * it never closes         -> the kill switch leaks DNS machine-wide forever,
#                                which is the defect the split was made to fix
#
# Run it AROUND a connect attempt made from the armed state:
#
#   1. kill switch on, tunnel down.        Assert-P7WfpInstalled -Expect armed
#   2. start the capture:                  $w = Start-P7ConnectWindow
#   3. press connect in the app (or let the app re-bootstrap).
#   4. after it settles either way:        Assert-P7ConnectWindow -Window $w
#
# Read-only: it reads the service log and takes netsh dumps. It never asks the
# service to do anything.
# ---------------------------------------------------------------------------
function Start-P7ConnectWindow {
  $log = @(Get-Content $script:P7Log -ErrorAction SilentlyContinue)
  $dump = Get-P7WfpDump -Label 'window-before'
  $names = if ($dump) { @(Get-P7WfpFilterNames -Dump $dump) } else { @() }
  $openBefore = $names -contains 'urnetwork-permit-dns-host-resolver'
  if ($openBefore) {
    Write-Host "NOTE - the DNS permit is ALREADY installed before the attempt." -ForegroundColor Yellow
    Write-Host "       Either a connect is already in flight, or the window from a" -ForegroundColor Yellow
    Write-Host "       previous one never closed. Settle the machine and re-run." -ForegroundColor Yellow
  }
  Write-Host "captured log position $($log.Count); now make ONE connect attempt." -ForegroundColor Cyan
  return [pscustomobject]@{ LogLines = $log.Count; PermitBefore = $openBefore }
}

function Assert-P7ConnectWindow {
  param([Parameter(Mandatory=$true)][psobject]$Window)
  $fail = @()
  $log = @(Get-Content $script:P7Log -ErrorAction SilentlyContinue)
  if ($log.Count -le $Window.LogLines) {
    Write-Host "INCONCLUSIVE - the service log did not grow. No attempt was made." -ForegroundColor Yellow
    return $false
  }
  $new = @($log[$Window.LogLines..($log.Count - 1)])

  Write-Host "=== 1. the window OPENED for the attempt ===" -ForegroundColor Cyan
  $opened = @($new | Where-Object { $_ -match 'wfp: policy armed -> connecting' })
  if ($opened.Count -gt 0) {
    Write-Host "  pass  - $($opened.Count) armed -> connecting transition(s)" -ForegroundColor Green
    $opened | Select-Object -Last 2 | ForEach-Object { Write-Host "          $_" -ForegroundColor DarkGray }
  } else {
    Write-Host "  FAIL  - the policy never widened. The attempt ran under ARMED," -ForegroundColor Red
    Write-Host "          which carries no DNS permit, so it could only have resolved" -ForegroundColor Red
    Write-Host "          from the OS cache. This is the reconnect regression." -ForegroundColor Red
    $fail += 'never-opened'
  }
  $window = @($new | Where-Object { $_ -match 'wfp: DNS WINDOW OPEN' })
  if ($window.Count -gt 0) {
    Write-Host "  note  - the service named the exposure itself:" -ForegroundColor DarkGray
    $window | Select-Object -Last 1 | ForEach-Object { Write-Host "          $_" -ForegroundColor DarkGray }
  }

  Write-Host "`n=== 2. the window CLOSED after it ===" -ForegroundColor Cyan
  $closed = @($new | Where-Object { $_ -match 'wfp: policy connecting -> (armed|connected)' })
  if ($closed.Count -gt 0) {
    Write-Host "  pass  - $($closed.Count) transition(s) out of connecting" -ForegroundColor Green
    $closed | Select-Object -Last 2 | ForEach-Object { Write-Host "          $_" -ForegroundColor DarkGray }
  } else {
    Write-Host "  FAIL  - nothing left the connecting state. Either the attempt is" -ForegroundColor Red
    Write-Host "          still in flight (wait, then re-run) or it is WEDGED, in" -ForegroundColor Red
    Write-Host "          which case the connecting watchdog should narrow it back" -ForegroundColor Red
    Write-Host "          to armed within 60s - look for 'held the DNS window open'." -ForegroundColor Red
    $fail += 'never-closed'
  }
  $watchdog = @($new | Where-Object { $_ -match 'held the DNS window open' })
  if ($watchdog.Count -gt 0) {
    Write-Host "  note  - the WATCHDOG fired. The attempt did not finish on its own;" -ForegroundColor Yellow
    Write-Host "          the window was force-closed. That is the backstop working," -ForegroundColor Yellow
    Write-Host "          and it means the attempt was wedged, not slow." -ForegroundColor Yellow
  }

  Write-Host "`n=== 3. the filter engine agrees, right now ===" -ForegroundColor Cyan
  $dump = Get-P7WfpDump -Label 'window-after'
  if (-not $dump) { return $false }
  $names = @(Get-P7WfpFilterNames -Dump $dump)
  $permit = $names -contains 'urnetwork-permit-dns-host-resolver'
  $tunUp  = @($names | Where-Object { $_ -like 'urnetwork-permit-tun-*' }).Count -gt 0
  if ($tunUp) {
    Write-Host "  the tunnel came UP, so the end state is connected." -ForegroundColor DarkGray
    if ($permit) {
      Write-Host "  FAIL  - the host-resolver permit survived into CONNECTED. That is" -ForegroundColor Red
      Write-Host "          the R6 leak: the physical adapter's resolvers are permitted" -ForegroundColor Red
      Write-Host "          while the tunnel is up." -ForegroundColor Red
      $fail += 'permit-survived-into-connected'
    } else {
      Write-Host "  pass  - no host-resolver permit while connected" -ForegroundColor Green
    }
  } else {
    Write-Host "  the attempt FAILED, so the end state should be armed." -ForegroundColor DarkGray
    if ($permit) {
      Write-Host "  FAIL  - the host-resolver permit is STILL INSTALLED with no tunnel" -ForegroundColor Red
      Write-Host "          and no attempt in flight. Machine-wide plaintext DNS is" -ForegroundColor Red
      Write-Host "          open and nothing is going to close it." -ForegroundColor Red
      $fail += 'permit-stuck-open'
    } else {
      Write-Host "  pass  - the permit is gone; armed is closed again" -ForegroundColor Green
    }
  }

  if ($fail.Count -eq 0) {
    Write-Host "`nPASS - the DNS window opened for the attempt and closed after it" -ForegroundColor Green
    return $true
  }
  Write-Host "`nFAIL - $($fail -join ', ')" -ForegroundColor Red
  return $false
}

# ---------------------------------------------------------------------------
# GATE I - THE LEAK TESTS. These are the assertions no unelevated agent could
# run, and they are the only evidence that the layer works.
#
# Test at the WIRE, not with a website. A browser doing its own DoH shows a
# clean result while the OS leaks. Have pktmon running on the PHYSICAL adapter
# for the whole of this:
#
#   pktmon start --capture --comp nics -f C:\temp\cap.etl
#   ... run Assert-P7NoLeakHard ...
#   pktmon stop ; pktmon etl2txt C:\temp\cap.etl
#
# PASS at the wire = on the physical adapter you see only (a) our provider
# transport, (b) DHCP, (c) ARP/NDP, (d) LAN traffic. ZERO port-53 packets to
# anything but our resolver. ZERO IPv6 to a global address.
# ---------------------------------------------------------------------------
function Assert-P7NoLeakHard {
  param([string]$Router = '')
  $fail = @()

  Write-Host "=== R6.1 an app with its OWN resolver must be BLOCKED ===" -ForegroundColor Cyan
  Write-Host "  (this is the test NRPT alone fails and a WFP block passes - the"
  Write-Host "   whole argument for a firewall floor rather than steering)"
  $t0 = Get-Date
  $r = $null
  try { $r = Resolve-DnsName leak-probe.example.com -Server 8.8.8.8 -DnsOnly -ErrorAction Stop } catch { }
  $dt = ((Get-Date) - $t0).TotalSeconds
  if ($r) { Write-Host "  FAIL - 8.8.8.8 answered; port 53 is NOT blocked" -ForegroundColor Red; $fail += 'dns-external' }
  else    { Write-Host ("  pass  - query to 8.8.8.8 failed after {0:N1}s" -f $dt) -ForegroundColor Green }

  if ($Router) {
    Write-Host "=== R6.2 the LAN router's resolver must be BLOCKED ===" -ForegroundColor Cyan
    Write-Host "  (the LAN itself is PERMITTED, so this only passes because the DNS"
    Write-Host "   lifting rule moved port 53 into the DNS sublayer - filter"
    Write-Host "   urnetwork-lift-dns-v4. It is the subtlest filter in the set.)"
    $r2 = $null
    try { $r2 = Resolve-DnsName leak-probe.example.com -Server $Router -DnsOnly -ErrorAction Stop } catch { }
    if ($r2) { Write-Host "  FAIL - the router's resolver answered: SILENT DNS LEAK" -ForegroundColor Red; $fail += 'dns-router' }
    else     { Write-Host "  pass  - the router's resolver did not answer" -ForegroundColor Green }

    Write-Host "=== exemption: the router itself must still be reachable ===" -ForegroundColor Cyan
    if (Test-Connection -ComputerName $Router -Count 2 -Quiet) {
      Write-Host "  pass  - the LAN permit is in force" -ForegroundColor Green
    } else {
      Write-Host "  FAIL - the LAN is blocked; the firewall and the route table disagree" -ForegroundColor Red
      $fail += 'lan'
    }
  } else {
    Write-Host "  (skipped R6.2 / LAN reachability: pass -Router <gateway ip>)" -ForegroundColor DarkGray
  }

  Write-Host "=== R6.3 resolution THROUGH the tunnel must still work ===" -ForegroundColor Cyan
  Clear-DnsClientCache
  $r3 = $null
  try { $r3 = Resolve-DnsName example.com -DnsOnly -ErrorAction Stop } catch { }
  if ($r3) { Write-Host "  pass  - the default resolver answered (via the tun)" -ForegroundColor Green }
  else {
    Write-Host "  FAIL - no DNS at all. Check TunnelStatus.dns_applied: if it is" -ForegroundColor Red
    Write-Host "         false the tun's resolvers never took, and the port-53 block" -ForegroundColor Red
    Write-Host "         then fails resolution CLOSED rather than leaking it." -ForegroundColor Red
    $fail += 'dns-tunnel'
  }

  Write-Host "=== R7.1 IPv6 to a global address must fail, and fail FAST ===" -ForegroundColor Cyan
  Write-Host "  (fast matters: a route blackhole hangs to TCP timeout and defeats"
  Write-Host "   Happy Eyeballs; a WFP block fails instantly so v4 fallback happens"
  Write-Host "   in milliseconds. Anything over ~3s means this is NOT the WFP block.)"
  $t1 = Get-Date
  $v6 = Test-NetConnection -ComputerName '2606:4700:4700::1111' -Port 443 -WarningAction SilentlyContinue
  $dt6 = ((Get-Date) - $t1).TotalSeconds
  if ($v6.TcpTestSucceeded) { Write-Host "  FAIL - IPv6 connected: R7 IS STILL OPEN" -ForegroundColor Red; $fail += 'ipv6' }
  elseif ($dt6 -gt 3)       { Write-Host ("  FAIL - blocked but took {0:N1}s: that is a timeout, not a block" -f $dt6) -ForegroundColor Red; $fail += 'ipv6-slow' }
  else                      { Write-Host ("  pass  - IPv6 refused in {0:N1}s" -f $dt6) -ForegroundColor Green }

  Write-Host "=== R7.2 Happy Eyeballs must still land on IPv4 ===" -ForegroundColor Cyan
  $t2 = Get-Date
  $code = (& curl.exe -s -o NUL -w '%{http_code}' -m 15 https://www.google.com) 2>$null
  $dt2 = ((Get-Date) - $t2).TotalSeconds
  if ($code -match '^\d{3}$' -and $code -ne '000') {
    Write-Host ("  pass  - dual-stack host reached over IPv4 in {0:N1}s (http {1})" -f $dt2, $code) -ForegroundColor Green
  } else {
    Write-Host "  FAIL - a dual-stack host is unreachable; the v6 block is breaking v4 too" -ForegroundColor Red
    $fail += 'happy-eyeballs'
  }

  Write-Host "=== exemption sweep (rank order, research S7.1) ===" -ForegroundColor Cyan
  $loop = Test-Connection -ComputerName 127.0.0.1 -Count 2 -Quiet
  Write-Host "  loopback              : $(if ($loop) {'pass'} else {'FAIL (rank 2)'})"
  if (-not $loop) { $fail += 'loopback' }
  Write-Host "  our own service       : the app must still be able to reconnect -"
  Write-Host "                          disconnect and reconnect once by hand (rank 1)."
  Write-Host "  DHCP                  : run  ipconfig /renew  and confirm the address"
  Write-Host "                          survives (rank 3; the failure is DELAYED, so"
  Write-Host "                          this is the one that is easy to miss)."

  if ($fail.Count -eq 0) {
    Write-Host "PASS - no leak found by any of these checks" -ForegroundColor Green
    Write-Host "       This is NOT the same as 'no leak'. Confirm at the wire with the" -ForegroundColor Yellow
    Write-Host "       pktmon capture described above before believing it." -ForegroundColor Yellow
    return $true
  }
  Write-Host "FAIL - $($fail -join ', ')" -ForegroundColor Red
  Write-Host "       ABORT: Ctrl+C in shell 1, then Assert-P7WfpAbsent and Assert-P7Baseline." -ForegroundColor Yellow
  return $false
}

# ---------------------------------------------------------------------------
# GATE I2 - ALE reauthorization. Adding a filter re-submits already-established
# flows for classification on their NEXT packet, so a browser that had TCP
# connections open before we armed gets them killed rather than continuing to
# leak. It is triggered by TRAFFIC, not by time: an idle flow survives until it
# moves, so the flow has to be poked.
#
# By hand, in this order:
#   1. DISCONNECTED: start a long-lived IPv6 flow, e.g.
#        curl.exe -6 -N -m 600 https://ipv6.google.com/  (leave it running)
#   2. Connect (or arm the kill switch).
#   3. Poke the flow - send/receive on it.
#   pass = the flow DIES. fail = it keeps carrying data, i.e. established
#          IPv6 connections leak straight through the block.
#   Repeat for a long-lived UDP flow: the research could not find a Microsoft
#   guarantee that reauth re-evaluates already-established UDP, and that gap is
#   still open. Record what you observe either way.
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# GATE J - the IPv6-only-network refusal.
#
# Our tunnel is v4-only, so protecting the user means blocking v6. On an
# IPv6-only access network that does not degrade them, it DISCONNECTS them,
# including from our own providers, so the client cannot recover. The service
# detects this before writing anything and refuses to connect.
#
# To exercise it you need a network with NO IPv4 default route. Options, in
# order of how easy they are to undo:
#   a) a Hyper-V internal switch with IPv6 only, machine attached to nothing else
#   b) `netsh interface ipv4 set interface <if> ignoredefaultroutes=enabled` on
#      every v4 adapter (undo with =disabled), then confirm:
#         Get-NetRoute -AddressFamily IPv4 -DestinationPrefix 0.0.0.0/0   # empty
#         Get-NetRoute -AddressFamily IPv6 -DestinationPrefix ::/0        # present
#   pass = the connect attempt FAILS with an error naming an IPv6-only network,
#          state=error, and NOTHING is written: no adapter, no route, no filter.
#          Assert-P7Baseline and Assert-P7WfpAbsent must both pass afterwards.
#   fail = it connects and blocks v6, leaving the machine with no network at
#          all and no way to recover from inside the app. ABORT: undo (b)
#          immediately, then Invoke-P7Abort.
# ---------------------------------------------------------------------------
function Assert-P7IPv6OnlyRefusal {
  $v4 = @(Get-NetRoute -AddressFamily IPv4 -DestinationPrefix '0.0.0.0/0' -ErrorAction SilentlyContinue)
  $v6 = @(Get-NetRoute -AddressFamily IPv6 -DestinationPrefix '::/0' -ErrorAction SilentlyContinue)
  Write-Host "IPv4 default routes : $($v4.Count)"
  Write-Host "IPv6 default routes : $($v6.Count)"
  if ($v4.Count -gt 0) {
    Write-Host "SKIP - this network has IPv4; the refusal path cannot be reached here." -ForegroundColor Yellow
    Write-Host "       See the comment above this function for how to build the case." -ForegroundColor Yellow
    return $null
  }
  Write-Host "This network IS IPv6-only. Now attempt a connect." -ForegroundColor Cyan
  Write-Host "  pass  : it REFUSES, naming the IPv6-only network, and writes nothing"
  Write-Host "  fail  : it connects and blocks v6, cutting the machine off entirely"
  Write-Host "Then:  Assert-P7Baseline  AND  Assert-P7WfpAbsent  must both pass."
  Get-Content $script:P7Log -Tail 20 | Write-Host
  return $true
}

# ---------------------------------------------------------------------------
# GATE K - the SCM start path must not strip a live console tunnel.
#
# This is the recovery-path defect fixed in main.cpp Run(): the sweep used to
# run BEFORE the control-pipe conflict was detected, so `sc start urnetworkd`
# against a live `urnetworkd console` tunnel deleted that tunnel's routes and
# cleared its DNS while the console kept reporting Up and traffic left in the
# clear. The fix hoists ControlPipeInUse() to the top of Run().
#
# It CANNOT be tested without installing the service, so it is not covered by
# `urnetworkd selftest` and it was NOT verified by the agent that wrote it -
# only by reading. This gate is how it gets verified.
#
#   shell 1 (elevated):  urnetworkd console        # let it bring a tunnel up
#   shell 2 (elevated):  urnetworkd install ; sc start urnetworkd
#
#   pass = sc start FAILS, the service log says "REFUSING to start - another
#          urnetworkd is already serving \\.\pipe\urnetwork.control", and
#          Assert-P7TunnelIntact below shows the console's 31 routes and its DNS
#          STILL PRESENT.
#   fail = the console's routes vanish while it still reports Up. That is the
#          original defect; ABORT with Invoke-P7Abort and do not proceed to
#          gate G.
#   cleanup: sc stop urnetworkd ; urnetworkd uninstall
# ---------------------------------------------------------------------------
function Assert-P7TunnelIntact {
  $r = @(Get-NetRoute -InterfaceAlias $script:P7TunAlias -AddressFamily IPv4 -ErrorAction SilentlyContinue |
           Where-Object { $_.Store -eq 'ActiveStore' -and $_.Protocol -eq 'NetMgmt' })
  $dns = Get-DnsClientServerAddress -InterfaceAlias $script:P7TunAlias -AddressFamily IPv4 -ErrorAction SilentlyContinue
  Write-Host "tun routes : $($r.Count)  (expect $($script:P7RouteCnt))"
  Write-Host "tun dns    : $($dns.ServerAddresses -join ', ')"
  if ($r.Count -eq $script:P7RouteCnt -and $dns.ServerAddresses.Count -gt 0) {
    Write-Host "PASS - the live tunnel was not stripped" -ForegroundColor Green
    return $true
  }
  Write-Host "FAIL - the live tunnel lost routes and/or DNS while still reporting Up." -ForegroundColor Red
  Write-Host "       THIS IS THE UNACCEPTABLE CASE: traffic is leaving in the clear" -ForegroundColor Red
  Write-Host "       with the UI still saying Connected. Invoke-P7Abort NOW." -ForegroundColor Red
  return $false
}

# ---------------------------------------------------------------------------
# NOT A GATE - armed-across-reboot (persistent + boot-time) is NOT SHIPPED.
#
# BuildPersistentFilterSet() exists and is unit-tested, but no code path
# installs it, and `urnetworkd selftest` asserts that no live policy state can
# emit a persistent filter. The reason is one unverified claim: persistent
# objects are documented to be enabled at boot only when their provider names
# no service or names an AUTO-START one (FWPM_PROVIDER0.serviceName), but
# BOOT-TIME filters are applied by netio/tcpip from
#   HKLM\SYSTEM\CurrentControlSet\Services\BFE\Parameters\Policy\BootTime
# BEFORE BFE consults the SCM, so they may not be gated at all. A persistent
# block orphaned by a crash or an uninstall is worse than the leak it prevents.
#
# To verify it - IN A VM, WITH A CHECKPOINT, NEVER ON THE DAILY DRIVER:
#   1. Hand-install the persistent + boot-time set with serviceName=urnetworkd.
#   2. sc config urnetworkd start= disabled ; reboot.
#      Expect: the PERSISTENT set is NOT enabled. Check the boot-time set
#      separately - that is the part nobody has confirmed.
#   3. Uninstall the MSI ; reboot. Expect: the machine has network.
#   4. netsh wfp show filters after each reboot; diff against wfp-before.xml.
# Only if 2 and 3 both hold does the persistent path become shippable.
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# Assert-P7NoLeak - R6 / R7 EVIDENCE (not assertions). Kept for the pre-fix
# comparison: it prints what is reachable off-tunnel. Assert-P7NoLeakHard above
# is the one with pass/fail.
# ---------------------------------------------------------------------------
function Assert-P7NoLeak {
  Write-Host "=== R7: is there a route off-box for IPv6 that is NOT the tun? ==="
  Get-NetRoute -AddressFamily IPv6 -DestinationPrefix '::/0' -ErrorAction SilentlyContinue |
    Select-Object ifIndex, InterfaceAlias, NextHop, RouteMetric |
    Format-Table -AutoSize | Out-String | Write-Host
  Write-Host "  Any row whose InterfaceAlias is not '$($script:P7TunAlias)' is a live IPv6 bypass."
  Write-Host "  This machine has a GLOBAL v6 address and a v6 default route, and the tunnel is"
  Write-Host "  v4-only, so this is expected to FAIL until R7 is implemented."

  Write-Host "`n=== R6: which resolvers can still be reached off-tunnel? ==="
  Get-DnsClientServerAddress | Where-Object { $_.ServerAddresses.Count -gt 0 } |
    Select-Object InterfaceIndex, InterfaceAlias, AddressFamily,
                  @{n='Servers';e={$_.ServerAddresses -join ', '}} |
    Format-Table -AutoSize | Out-String -Width 160 | Write-Host
  Write-Host "  Any resolver on a NON-tun adapter whose address falls in 10/8, 172.16/12 or"
  Write-Host "  192.168/16 is reachable AROUND the tunnel: those ranges are deliberately"
  Write-Host "  excluded from the tun capture (NetworkConfig.cpp:66-88)."

  Write-Host "`n=== NRPT rules (must match the baseline; URnetwork adds none today) ==="
  Get-DnsClientNrptRule | Select-Object Name, Namespace, NameServers |
    Format-Table -AutoSize | Out-String -Width 160 | Write-Host
}

# ---------------------------------------------------------------------------
# Invoke-P7Abort - the escape hatch, in the ONLY order that works.
#
# `urnetworkd revert` REFUSES while any urnetworkd serves the control pipe
# (main.cpp:335-347), because it cannot tell a live tunnel from an orphaned one
# and reverting a live one drops traffic to the clear while the UI still says
# Connected. So the process MUST be stopped first. That ordering is the whole
# safety of the command; do not reach for --force to get around it.
# ---------------------------------------------------------------------------
function Invoke-P7Abort {
  if (-not (Test-P7Elevated)) {
    Write-Host "This shell is NOT elevated. From an elevated shell, run:" -ForegroundColor Yellow
    Write-Host "    Get-Process urnetworkd | Stop-Process -Force"
    Write-Host "    & '$($script:P7Exe)' revert"
    return
  }
  Write-Host "1. stopping every urnetworkd (frees the control pipe)" -ForegroundColor Cyan
  Write-Host "   This ALSO lifts the WFP leak-prevention policy: it is registered on a" -ForegroundColor DarkGray
  Write-Host "   session opened with FWPM_SESSION_FLAG_DYNAMIC, so BFE deletes the" -ForegroundColor DarkGray
  Write-Host "   provider, both sublayers and every filter when the process dies -" -ForegroundColor DarkGray
  Write-Host "   however it dies. Killing urnetworkd is the fastest way to get the" -ForegroundColor DarkGray
  Write-Host "   network back, and it fails OPEN by design." -ForegroundColor DarkGray
  Get-Process urnetworkd -ErrorAction SilentlyContinue | Stop-Process -Force
  Start-Sleep -Seconds 2
  Write-Host "2. urnetworkd revert" -ForegroundColor Cyan
  & $script:P7Exe revert
  Start-Sleep -Seconds 1
  Write-Host "3. state after revert" -ForegroundColor Cyan
  Show-P7State
  Write-Host "4. baseline comparison" -ForegroundColor Cyan
  Assert-P7Baseline -Label 'post-abort' | Out-Null
  Write-Host "5. filter engine" -ForegroundColor Cyan
  Assert-P7WfpAbsent -Label 'post-abort' | Out-Null
  Write-Host "If filters SURVIVED all of that they were not from a dynamic session." -ForegroundColor Yellow
  Write-Host "Last resort, and an admin can always do it:  net stop bfe ; net start bfe" -ForegroundColor Yellow
}

Write-Host @"
P7 gate helpers loaded. Elevated: $(Test-P7Elevated)

  Invoke-P7SelfTest           pure-logic unit tests (UNELEVATED, safe, run first)
  Assert-P7Baseline           diff the machine against the pre-flight baseline
  Show-P7State                adapter / routes / DNS / marker / log tail
  Assert-P7GateC -ServicePid  R1 socket self-exclusion (the top risk)
  Assert-P7NoLeak             R6/R7 EVIDENCE (prints what is reachable)
  Assert-P7WfpAbsent          nothing of ours in the filter engine       [gate H1]
  Assert-P7WfpInstalled -Expect armed|connecting|connected
                              the designed policy                        [gate H2]
  Assert-P7ServiceDns -State connecting|armed|connected
                              connecting/connected RESOLVE; armed MUST NOT [gate H3]
  Start-P7ConnectWindow / Assert-P7ConnectWindow -Window $w
                              the DNS window opens for an attempt and
                              closes after it                            [gate H4]
  Assert-P7NoLeakHard -Router <gw>   THE LEAK ASSERTIONS                 [gate I]
  Assert-P7IPv6OnlyRefusal    refuse rather than block into a dead end   [gate J]
  Assert-P7TunnelIntact       the SCM start path did not strip a tunnel  [gate K]
  Invoke-P7Abort              stop urnetworkd, then revert, then verify

WHAT WAS AND WAS NOT VERIFIED BEFORE THIS FILE REACHED YOU
  Verified by running, unelevated: the filter-set construction and the shared
  route/firewall table (urnetworkd selftest, 86 checks, including the RESTATED
  invariant - every state a connection is attempted from has a DNS path the
  service can use, armed has none and blocks port 53 unconditionally, and
  Connecting is exactly Armed plus one named filter), that
  urnetworkd console --rpc-only still starts and still cannot reach step 6/8,
  that the app launches, and that the new protocol fields round-trip over the
  control pipe.
  NOT verified, and unverifiable without elevation: THAT ANY FILTER BLOCKS
  ANYTHING. No filter was ever added to this machine. Gates H, I, J and K below
  are the entire evidence base for the leak-prevention layer, and until they are
  run the layer is code that compiles, not protection that works.

PRECONDITIONS before gate B (all of them):
  [ ] RE-CAPTURE THE BASELINE FIRST. The committed baseline\ is already STALE:
      Tailscale's adapter has since moved ifIndex (33 -> 9) and its 2 NRPT rules
      are gone, so Assert-P7Baseline reports 8 differing captures that have
      nothing to do with URnetwork. Verified 2026-08-08: zero rows in the current
      capture mention URnetwork. Stop Tailscale, then:
          .\capture-netstate.ps1 -OutDir .\baseline
      A baseline that fails for unrelated reasons trains you to ignore it.
  [ ] Tailscale stopped        Stop-Service Tailscale
      It is a second wintun 0.14.1 consumer, owns 2 NRPT rules and a 100.100.100.100
      resolver, and holds a global IPv6 address. Leaving it up makes every R6/R7
      result uninterpretable.
  [ ] Invoke-P7SelfTest passes. It is unelevated and cannot touch the machine;
      a failure there means the policy is wrong before any gate can tell you so.
  [ ] Assert-P7WfpAbsent passes, and keep that dump as wfp-before.xml. Every
      later filter diff is against it.
  [ ] Only ONE urnetworkd build on this box. C:\ProgramData\URnetwork\service is
      machine-wide and NOT redirectable (Paths.cpp:44-50): every worktree shares one
      marker file, one log and one adapter GUID.
  [ ] URnetwork.exe (the app) not running, unless the gate needs it.
  [ ] SHELL 2 already open and elevated, with the revert command typed, not entered.
  [ ] Baseline captured:  .\capture-netstate.ps1 -OutDir .\baseline

GATES - the exact command, the assertion, the abort.

  A  unelevated console, expect step 1/8 to refuse
     run    : urnetworkd console      (then have a client send start_tunnel)
     pass   : "wintun: CreateAdapter failed: 5" + "start FAILED at step 1/8"
     abort  : none needed - nothing is applied
     STATUS : PASSED 2026-08-08 on build CD0814BF, unelevated. See the report.

  B  ELEVATED console, adapter only, abort before step 6
     run    : urnetworkd console      (elevated; do NOT let a client start a tunnel)
     pass   : Show-P7State shows the adapter present with NO address and NO route;
              Assert-P7Baseline PASSES (an adapter alone changes no route)
     abort  : Ctrl+C in shell 1. Then adapter ABSENT and marker false.

  C  R1 socket self-exclusion            <<< THE GATE THAT MATTERS MOST
     run    : with the adapter up and BEFORE routes, Assert-P7GateC -ServicePid <pid>
     pass   : the egress: line names the physical NIC, and no socket is sourced
              from the tun address
     abort  : Ctrl+C immediately. A failure here means the service deadlocks
              against itself the moment step 6 runs.

  D  routes, then IMMEDIATE orderly revert          <<< first destructive gate
     run    : allow step 6/8, then Show-P7State, then Ctrl+C
     pass   : after Ctrl+C, Assert-P7Baseline is byte-identical
     abort  : Ctrl+C; if that does not restore it, Invoke-P7Abort from shell 2.
     NOTE   : do this BEFORE ever running the packet pump.

  E  crash revert                                   <<< the test that matters
     run    : repeat D, then from shell 2: Get-Process urnetworkd | Stop-Process -Force
     pass   : Assert-P7Baseline is byte-identical WITHOUT running anything of ours -
              the wintun adapter dies with the process and the stack drops its routes
     abort  : Invoke-P7Abort from shell 2 (the pipe is already free after the kill)
     NOTE   : Stop-Process is TerminateProcess. It runs NO user code - not the
              exception filter, not the terminate handler, not the console handler.
              CrashRevert does NOT run. That is the point of the gate.

  F  traffic, then the leak EVIDENCE
     run    : allow steps 7-8, browse, then Assert-P7NoLeak
     pass   : nothing here is an assertion - it prints what is reachable
              off-tunnel so H/I have something to be compared against.
     abort  : Ctrl+C, then Assert-P7Baseline.

  H  THE FIREWALL IS THE POLICY THAT WAS DESIGNED     <<< new, needs elevation
     H1 run  : Assert-P7WfpAbsent          (BEFORE arming, and after every teardown)
        pass : 0 urnetwork filters, 0 provider-GUID hits, and above all
               0 PERSISTENT-provider hits - this build never installs one.
        abort: stop every urnetworkd (that alone lifts a dynamic session),
               then urnetworkd revert; last resort net stop bfe.
     H2 run  : with the tunnel up      Assert-P7WfpInstalled -Expect connected
               a connect IN FLIGHT          Assert-P7WfpInstalled -Expect connecting
               kill switch on, tunnel down  Assert-P7WfpInstalled -Expect armed
        pass : the count matches urnetworkd selftest - armed 38, connecting 39,
               connected DERIVED as 42 + one per tunnel resolver (43 with one,
               44 with two; the old hardcoded 43 failed a two-resolver session
               for no reason) - every required filter name is present, ARMED
               contains NO tun permit AND NO DNS PERMIT OF ANY KIND, and
               CONNECTED contains NO urnetwork-permit-dns-host-resolver.
     H3 run  : Assert-P7ServiceDns -State connecting   (a connect in flight)
               Assert-P7ServiceDns -State armed        (kill switch on, down)
               Assert-P7ServiceDns -State connected    (tunnel up)
        why  : the DNS-sublayer permit for our own service is scoped on the app
               id of urnetworkd.exe, and OUR NAME RESOLUTION DOES NOT COME OUT
               OF urnetworkd.exe - Go on Windows resolves via GetAddrInfoW and
               the wire query is issued by svchost.exe. So the permit that keeps
               us resolving has to be ADDRESS-scoped, and an address-scoped
               permit is MACHINE-WIDE: it lets every process on the box resolve
               in plaintext. That is why it is now installed only while a
               connection attempt is in flight.
        pass : for CONNECTING and CONNECTED - the state's required DNS permit is
               installed, the port-53 hard block is installed ALONGSIDE it, a
               name resolves right now, and the lookup lands in the OS-wide DNS
               Client cache (which proves it went through Dnscache - the same
               path the service uses). For 'connecting' the log also shows a
               start reaching step 4/8 AFTER a 'wfp: policy armed -> connecting'
               line.
               for ARMED - THE OPPOSITE. No DNS permit at all, the port-53 block
               present at both layers, and NOTHING RESOLVES. A successful lookup
               while armed is the failure.
        fail : 'permit-missing' or 'no-resolution' in CONNECTING means the
               machine is in the unrecoverable state NOW. Ctrl+C in shell 1
               immediately - the dynamic session dies with the process and the
               network returns.
               'permit-present-while-idle' / 'resolves-while-armed' is the other
               failure: not a stuck machine, a leaking one. The kill switch is on
               and DNS is still leaving.
        note : 'block-stood-down' in CONNECTING/CONNECTED is not a crash, it is
               the coupling working: the code found no usable IPv4 resolver and
               chose an open DNS path over an attempt that could never resolve.
               Plaintext DNS to any server is permitted while it lasts, the
               service log says so, and it closes on the return to armed.
     H4 run  : $w = Start-P7ConnectWindow ; make ONE connect attempt from armed ;
               Assert-P7ConnectWindow -Window $w
        why  : H2/H3 photograph a state. H4 is the EDGE, and the edge is all the
               Armed/Connecting split added. Never opening kills automatic
               reconnect; never closing leaves the kill switch leaking DNS
               machine-wide, which is the defect the split exists to remove.
        pass : the log shows 'wfp: policy armed -> connecting' and then a
               transition OUT of connecting, and the live dump matches the end
               state (permit gone if the attempt failed, permit gone if it
               succeeded - it belongs to neither end).
        note : 'held the DNS window open' in the log means the 60s watchdog fired
               and force-closed the window. That is the backstop working, and it
               means the attempt was wedged rather than slow.
        abort: Ctrl+C in shell 1, then Assert-P7WfpAbsent.

  I  THE LEAK TESTS                                   <<< THE GATE THAT MATTERS
     run    : with pktmon capturing the PHYSICAL adapter,
              Assert-P7NoLeakHard -Router <your gateway ip>
     pass   : a query to 8.8.8.8 fails; a query to the router's resolver fails;
              a query through the tun succeeds; IPv6 to a global address is
              refused in UNDER 3 SECONDS (a slow failure is a timeout, not a
              block, and it defeats Happy Eyeballs); a dual-stack host is still
              reachable over v4; loopback and the LAN still work.
              THEN confirm at the wire: zero port-53 packets to anything but our
              resolver, zero IPv6 to a global address on the physical adapter.
     abort  : Ctrl+C, then Assert-P7WfpAbsent and Assert-P7Baseline.
     I2     : ALE reauthorization - see the block comment above gate J in this
              file. Established flows must DIE, not continue. Poke them; reauth
              is triggered by traffic, not by time. Do the UDP variant too: that
              one has no documented Microsoft guarantee.

  J  IPv6-ONLY NETWORK REFUSAL                        <<< new
     run    : on a network with no IPv4 default route, Assert-P7IPv6OnlyRefusal,
              then attempt a connect.
     pass   : it REFUSES, names the IPv6-only network, and writes NOTHING - no
              adapter, no route, no filter. Assert-P7Baseline and
              Assert-P7WfpAbsent both pass afterwards.
     fail   : it connects and blocks v6, cutting the machine off from everything
              including our own providers, with no recovery from inside the app.
     abort  : undo the ignoredefaultroutes change, then Invoke-P7Abort.

  K  THE SCM START PATH DOES NOT STRIP A LIVE TUNNEL  <<< the defect fix
     run    : shell 1 elevated urnetworkd console with a tunnel up;
              shell 2 elevated urnetworkd install then sc start urnetworkd.
              Then Assert-P7TunnelIntact.
     pass   : sc start FAILS, the log says "REFUSING to start - another
              urnetworkd is already serving the pipe", and the console's 31
              routes and its DNS are STILL THERE.
     fail   : the console's routes vanish while it still reports Up. Original
              defect; Invoke-P7Abort and do not proceed to G.
     note   : this fix was verified BY READING ONLY. Testing it needs a service
              install, which the agent that wrote it was not permitted to do.
     cleanup: sc stop urnetworkd ; urnetworkd uninstall

  G  the service proper - install, auto-start, reboot survival
     UNBLOCKED once gate K passes. The Run() ordering defect that blocked it is
     fixed (ControlPipeInUse() is now the first thing Run() does), but K is what
     turns that from a claim into a result.
"@ -ForegroundColor Cyan
