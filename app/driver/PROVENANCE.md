# Split-tunnel driver — provenance & clean-room record

This driver is a **clean-room implementation** so the entire URnetwork Windows
distribution stays free of GPL code and the driver ships under **MPL-2.0**, like
the rest of URnetwork. GPL split-tunnel drivers (e.g. Mullvad's
`win-split-tunnel`) were **not** consulted for their source.

## Permitted sources (only these)

The mechanism — redirecting a process's socket binds via a WFP callout — is a
documented Windows platform capability. Implementation draws ONLY from:

1. **Microsoft Learn / WDK documentation** (public API reference):
   - WFP redirection layers: `FWPM_LAYER_ALE_BIND_REDIRECT_V4/V6`,
     `FWPM_LAYER_ALE_CONNECT_REDIRECT_V4/V6`.
   - Redirect APIs: `FwpsRedirectHandleCreate`, `FwpsAcquireWritableLayerDataPointer`,
     `FwpsApplyModifiedLayerData`, `FwpsAcquireClassifyHandle`,
     `FwpsPendClassify`, `FwpsCompleteClassify`.
   - Callout registration: `FwpsCalloutRegister`, `FwpmCalloutAdd`,
     `FwpmFilterAdd`, `FwpmSubLayerAdd`, `FwpmEngineOpen`.
   - Process tracking: `PsSetCreateProcessNotifyRoutineEx`,
     `ZwQueryInformationProcess` / `SeLocateProcessImageName`.
   - WDM/WDF device + IOCTL: `IoCreateDevice`, `IoCreateSymbolicLink`,
     `IRP_MJ_DEVICE_CONTROL`.
2. **Microsoft Windows-driver-samples** (MIT-licensed, github.com/microsoft/Windows-driver-samples):
   - `network/trans/*` WFP callout samples (inspection/proxy/redirect) for the
     callout registration and classify/redirect mechanics.
3. Public vendor **design write-ups / blog posts** (prose only) may inform the
   behavioral spec in `README.md` — never their source code.

## Discipline

- No developer who writes or reviews driver code reads any GPL driver source.
- Every non-obvious API use cites the Microsoft doc or MIT sample it came from,
  in a comment at the call site.
- This file is updated when a new source is consulted. If in doubt, it is out.

## Attestation signing

The built `SplitTunnel.sys` is a kernel driver: Windows 10 1607+ requires
Microsoft attestation signing (EV cert + Partner Center `.cab` submission). That
pipeline is release infrastructure separate from this source and is set up in
M0. The driver here is unsigned dev-built; test-signing mode is required to load
it during development (`bcdedit /set testsigning on`).
