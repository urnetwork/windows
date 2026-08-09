# P7 RECOVERY CARD - read this when the network is gone

ASCII only, on purpose. Keep this file open in a text editor BEFORE starting, and
keep a SECOND elevated PowerShell window open with the abort command typed but
NOT entered. When this is needed you will have no internet, and no Claude.

Paths on this machine:

    SERVICE   C:\Users\ryanm\Downloads\claude_sandbox_windows\app\build\x64\Release\urnetworkd.exe
    APP       C:\Users\ryanm\Downloads\claude_sandbox_windows\app\build\x64\Release\URnetwork.exe
    BASELINE  C:\Users\ryanm\Downloads\claude_sandbox_windows\docs\superpowers\reports\p7-baseline\baseline\
    GATES     C:\Users\ryanm\Downloads\claude_sandbox_windows\docs\superpowers\reports\p7-baseline\p7-gates.ps1

--------------------------------------------------------------------------------
## THE ONE COMMAND

In an ELEVATED PowerShell:

    Get-Process urnetworkd -ErrorAction Ignore | Stop-Process -Force
    Start-Sleep -Seconds 2
    & "C:\Users\ryanm\Downloads\claude_sandbox_windows\app\build\x64\Release\urnetworkd.exe" revert

**Order matters. `revert` REFUSES while any urnetworkd is running** - it cannot
tell a live tunnel from a dead one, and reverting a live one would drop your
working connection. So: stop first, then revert.

--------------------------------------------------------------------------------
## WHAT YOU SHOULD KNOW BEFORE PANICKING

**Your LAN never goes away.** The tunnel route set deliberately EXCLUDES
10.0.0.0/8, 172.16.0.0/12 and 192.168.0.0/16 (NetworkConfig.cpp:66-88). Your
router (192.168.12.1), your LAN, your local DNS and this shell stay reachable
even in the worst case. The bad outcome is "no internet", not "dead machine".

**Killing the process is itself a recovery action, for two separate reasons:**

1. ROUTES. wintun never calls SwDeviceSetLifetime, so process death triggers a
   PnP surprise removal of the adapter, and Windows drops every route and address
   bound to that adapter's LUID. This is what actually restores routing - NOT our
   crash handler. `CrashRevert` does NOT run on a force-kill: TerminateProcess
   executes no user code (not the exception filter at main.cpp:159, not the
   terminate handler at :172, not the console handler at :178).
2. WFP FILTERS. They are registered on a DYNAMIC session, so the Base Filtering
   Engine deletes all of them when the owning process exits. There is no
   persistent filter to strand you - the persistent path exists in code but is
   never installed, and the selftest asserts that.

**What is NOT guaranteed by a force-kill:** a phantom devnode and some registry
residue survive, because DIF_REMOVE never runs. "The network came back" and
"nothing was left behind" are different claims. The leftovers are cleaned by
`SweepOrphanedTunnel` on the next ELEVATED start - an unelevated `--rpc-only`
run is observe-only and cleans nothing.

**We do not use NRPT.** If `Get-DnsClientNrptRule` shows rules belonging to us,
something is wrong - report it. (Tailscale may legitimately have its own.)

--------------------------------------------------------------------------------
## SYMPTOM -> ACTION

### "No internet, but the LAN works"
Expected during gates D and E. Do THE ONE COMMAND above.
Then confirm recovery:

    Get-NetRoute -DestinationPrefix '0.0.0.0/0' | Select ifIndex,NextHop,RouteMetric
    Get-DnsClientServerAddress -AddressFamily IPv4 | Where {$_.ServerAddresses}

Healthy on this machine looks like: default route on **ifIndex 7** via
**192.168.12.1**, and Ethernet's resolver **192.168.12.1**.

### "revert says it refuses / already running"
That is the guard working. Stop every urnetworkd first:

    Get-Process urnetworkd -ErrorAction Ignore | Select Id,Path
    Get-Process urnetworkd -ErrorAction Ignore | Stop-Process -Force

Note there may be TWO urnetworkd builds on this box - the Release one and a
scratchpad copy. Stop BOTH. Check `Path` before you decide you are done.

### "A URnetwork adapter is still listed"
    Get-NetAdapter | Where-Object { $_.InterfaceDescription -match 'wintun|URnetwork' }

If one lingers with no owning process, the next ELEVATED urnetworkd start sweeps
it. To force it now:

    Get-NetAdapter -Name 'URnetwork*' | Disable-NetAdapter -Confirm:$false

Do NOT run `WintunDeleteDriver` and do NOT uninstall the wintun driver: it
detaches EVERY wintun adapter on the machine including Tailscale's. Our code
resolves that export and deliberately never calls it.

### "DNS is broken but routing looks fine"
Tunnel DNS was set and not cleared. THE ONE COMMAND handles it. Then:

    Clear-DnsClientCache
    Get-DnsClientServerAddress -AddressFamily IPv4 | Where {$_.ServerAddresses}

Cached answers are a real failure mode: entries resolved through the exit are
served to every process for their TTL after the tunnel dies. Always flush.

### "Everything is blocked, even the LAN"
Should be impossible - the WFP policy exempts loopback, LAN, DHCP and NDP. If it
happens, kill the process (dynamic session = filters die with it). Only if that
fails, the nuclear option, in an elevated shell:

    net stop bfe /y

**WARNING: this stops the Base Filtering Engine and drops ALL WFP filters on the
machine, including Windows Firewall's.** It leaves the box unprotected until you
`net start bfe`. Last resort only, and start it again immediately after.

### "The app says Connected but nothing works"
Known gap, not a mystery: nothing watches transport connectivity, so the policy
stays at Connected after a drop. It fails CLOSED - no leak - but the UI has no
way to tell you. Disconnect in the app, or THE ONE COMMAND.

### "I force-killed it and I want to be sure the machine is clean"
    & "...\urnetworkd.exe" revert          # elevated, nothing else running
    Get-NetAdapter | Where { $_.InterfaceDescription -match 'wintun' }
    Get-NetRoute -DestinationPrefix '0.0.0.0/0'
    Get-DnsClientNrptRule
    Clear-DnsClientCache

--------------------------------------------------------------------------------
## THE ABSOLUTE FLOOR

If nothing above works: **reboot**. Our routes and DNS are set through the
runtime APIs and are not registered as persistent, and the WFP filters are on a
dynamic session, so a reboot is expected to clear all of it. The wintun adapter
is recreated per run, not persisted (wintun >= 0.14; we ship 0.14.1).

Marked EXPECTED, not verified - no reboot with routes installed has ever been
performed. If a reboot does NOT clear it, that is a genuine finding: capture
`Get-NetRoute`, `Get-DnsClientServerAddress` and `Get-NetAdapter` and report.

--------------------------------------------------------------------------------
## WHAT TO CAPTURE FOR ME

Whatever happened, paste these back and I can diagnose without guessing:

    Get-NetRoute -AddressFamily IPv4 | Sort-Object DestinationPrefix | Format-Table -AutoSize
    Get-NetAdapter | Format-Table -AutoSize
    Get-DnsClientServerAddress -AddressFamily IPv4 | Format-Table -AutoSize
    Get-Content C:\ProgramData\URnetwork\service\logs\urnetworkd.log -Tail 80

The service log is the single most useful artifact - it names the step number
(`step 1/8` ... `8/8`), whether routes were installed, and every WFP policy
transition (`wfp: policy X -> Y`).
