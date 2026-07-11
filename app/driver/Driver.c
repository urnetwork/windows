/*
 * URnetwork split-tunnel driver (SplitTunnel.sys).
 *
 * A WFP callout driver that redirects excluded processes' outbound socket
 * binds to the physical interface, so those apps bypass the VPN tunnel.
 * Clean-room from Microsoft docs + MIT Windows-driver-samples (PROVENANCE.md).
 *
 * SPDX-License-Identifier: MPL-2.0
 */

#include <ntddk.h>
#include <wdf.h>
#define INITGUID
#include <guiddef.h>
#include <fwpsk.h>
#include <fwpmk.h>

#include "Ioctl.h"

// ---- identity ------------------------------------------------------------

// WFP sublayer + callout GUIDs (stable; registered in DriverEntry).
//
// The redirect decision is PROCESS-based, never destination-based: we redirect
// the socket binds of excluded processes and consult no remote/destination IP.
// ALE_BIND_REDIRECT is therefore the right layer — it fires at bind() with the
// owning process id in the metadata and no destination in scope, so the
// exclusion is inherently "which socket", not "which destination".
// (Microsoft WFP redirection docs; MIT trans samples.)
// {2B3C4D5E-6F70-4811-9A2B-3C4D5E6F7081}
DEFINE_GUID(URST_SUBLAYER, 0x2b3c4d5e, 0x6f70, 0x4811, 0x9a, 0x2b, 0x3c, 0x4d, 0x5e, 0x6f, 0x70, 0x81);
// {3C4D5E6F-7081-4922-AB3C-4D5E6F708192}
DEFINE_GUID(URST_CALLOUT_BIND_V4, 0x3c4d5e6f, 0x7081, 0x4922, 0xab, 0x3c, 0x4d, 0x5e, 0x6f, 0x70, 0x81, 0x92);
// {4D5E6F70-8192-4A33-BC4D-5E6F70819203}
DEFINE_GUID(URST_CALLOUT_BIND_V6, 0x4d5e6f70, 0x8192, 0x4a33, 0xbc, 0x4d, 0x5e, 0x6f, 0x70, 0x81, 0x92, 0x03);

#define URST_DEVICE_NAME L"\\Device\\URnetworkSplitTunnel"
#define URST_SYMLINK_NAME L"\\DosDevices\\URnetworkSplitTunnel"
#define URST_POOL_TAG 'tSrU'

// ---- state ---------------------------------------------------------------

typedef struct _URST_STATE {
  HANDLE engine;              // WFP engine handle
  UINT32 calloutIdV4;         // registered callout ids (for unregister)
  UINT32 calloutIdV6;
  HANDLE injectHandle;        // redirect handle (FwpsRedirectHandleCreate)
  DEVICE_OBJECT* device;

  EX_SPIN_LOCK lock;          // guards the fields below
  BOOLEAN enabled;
  URST_PHYSICAL_ADDRS physical;

  // excluded image paths (owned copies)
  UNICODE_STRING* paths;
  ULONG pathCount;

  // tracked excluded process ids (excluded image or descendant); simple growable
  // array for the skeleton — a hash set is the hardening follow-up.
  HANDLE* trackedPids;
  ULONG trackedCount;
  ULONG trackedCapacity;
} URST_STATE;

static URST_STATE g_state;

// ---- exclusion set helpers ----------------------------------------------

static BOOLEAN UrstIsPidTracked(HANDLE pid) {
  // caller holds the lock (shared)
  for (ULONG i = 0; i < g_state.trackedCount; ++i) {
    if (g_state.trackedPids[i] == pid) return TRUE;
  }
  return FALSE;
}

static VOID UrstTrackPid(HANDLE pid) {
  // caller holds the lock (exclusive)
  if (UrstIsPidTracked(pid)) return;
  if (g_state.trackedCount == g_state.trackedCapacity) {
    ULONG newCap = g_state.trackedCapacity ? g_state.trackedCapacity * 2 : 32;
    HANDLE* grown = ExAllocatePool2(POOL_FLAG_NON_PAGED, newCap * sizeof(HANDLE), URST_POOL_TAG);
    if (!grown) return;
    if (g_state.trackedPids) {
      RtlCopyMemory(grown, g_state.trackedPids, g_state.trackedCount * sizeof(HANDLE));
      ExFreePoolWithTag(g_state.trackedPids, URST_POOL_TAG);
    }
    g_state.trackedPids = grown;
    g_state.trackedCapacity = newCap;
  }
  g_state.trackedPids[g_state.trackedCount++] = pid;
}

static VOID UrstUntrackPid(HANDLE pid) {
  for (ULONG i = 0; i < g_state.trackedCount; ++i) {
    if (g_state.trackedPids[i] == pid) {
      g_state.trackedPids[i] = g_state.trackedPids[--g_state.trackedCount];
      return;
    }
  }
}

// Case-insensitive suffix/exact match of an image path against the exclusion set.
static BOOLEAN UrstImageExcluded(PCUNICODE_STRING image) {
  for (ULONG i = 0; i < g_state.pathCount; ++i) {
    if (RtlEqualUnicodeString(image, &g_state.paths[i], TRUE)) return TRUE;
  }
  return FALSE;
}

// ---- process create/exit notify ------------------------------------------
// PsSetCreateProcessNotifyRoutineEx (Microsoft docs): track excluded image
// launches and their descendants; drop pids on exit. This is how "child of an
// excluded app is also excluded" is implemented (README spec).

static VOID UrstCreateProcessNotify(PEPROCESS process, HANDLE pid,
                                    PPS_CREATE_NOTIFY_INFO info) {
  UNREFERENCED_PARAMETER(process);
  KIRQL irql = ExAcquireSpinLockExclusive(&g_state.lock);
  if (info) {
    // process created
    BOOLEAN excluded = FALSE;
    if (info->ImageFileName) {
      excluded = UrstImageExcluded(info->ImageFileName);
    }
    if (!excluded) {
      // inherit exclusion from an excluded parent (child-process inheritance)
      excluded = UrstIsPidTracked(info->ParentProcessId) ||
                 UrstIsPidTracked(info->CreatingThreadId.UniqueProcess);
    }
    if (excluded) UrstTrackPid(pid);
  } else {
    // process exited
    UrstUntrackPid(pid);
  }
  ExReleaseSpinLockExclusive(&g_state.lock, irql);
}

// ---- WFP classify (the redirect) -----------------------------------------
// At ALE_BIND_REDIRECT the flow's owning process id is in the metadata; if it
// is tracked (excluded) and redirection is enabled with a physical interface,
// rewrite the bind to egress the physical interface. Fail-open otherwise.

static void NTAPI UrstClassify(const FWPS_INCOMING_VALUES* inFixedValues,
                               const FWPS_INCOMING_METADATA_VALUES* inMetaValues,
                               void* layerData, const void* classifyContext,
                               const FWPS_FILTER* filter, UINT64 flowContext,
                               FWPS_CLASSIFY_OUT* classifyOut) {
  UNREFERENCED_PARAMETER(inFixedValues);
  UNREFERENCED_PARAMETER(flowContext);

  // Default: permit unchanged (fail-open).
  classifyOut->actionType = FWP_ACTION_PERMIT;
  if (!(classifyOut->rights & FWPS_RIGHT_ACTION_WRITE)) return;

  // The ONLY exclusion criterion is the owning process. We look at no remote or
  // destination address here — this is per-socket (per-process) redirection.
  if (!FWPS_IS_METADATA_FIELD_PRESENT(inMetaValues, FWPS_METADATA_FIELD_PROCESS_ID))
    return;
  HANDLE pid = (HANDLE)(ULONG_PTR)inMetaValues->processId;

  // Snapshot the decision + physical source under the lock.
  URST_PHYSICAL_ADDRS physical;
  KIRQL irql = ExAcquireSpinLockShared(&g_state.lock);
  BOOLEAN redirect = g_state.enabled && UrstIsPidTracked(pid);
  physical = g_state.physical;
  ExReleaseSpinLockShared(&g_state.lock, irql);
  if (!redirect) return;

  // Rewrite this excluded socket's bind to the physical interface's source
  // address, so it egresses the physical NIC and bypasses the tun's default
  // route. Purely process-scoped; no destination is consulted.
  // (FwpsAcquireWritableLayerDataPointer + FwpsApplyModifiedLayerData, WFP
  //  bind-redirect sample.) Hardening surface (R10): Driver Verifier + tests.
  HANDLE classifyHandle = NULL;
  NTSTATUS status = FwpsAcquireClassifyHandle((void*)classifyContext, 0, &classifyHandle);
  if (!NT_SUCCESS(status)) return;  // fail-open

  FWPS_BIND_REQUEST* bindRequest = NULL;
  status = FwpsAcquireWritableLayerDataPointer(classifyHandle, filter->filterId, 0,
                                               (PVOID*)&bindRequest, classifyOut);
  if (NT_SUCCESS(status) && bindRequest) {
    SOCKADDR_STORAGE* local = &bindRequest->localAddressAndPort;
    if (local->ss_family == AF_INET && physical.InterfaceIndex4 != 0) {
      SOCKADDR_IN* sin = (SOCKADDR_IN*)local;
      RtlCopyMemory(&sin->sin_addr, physical.Address4, sizeof(physical.Address4));
    } else if (local->ss_family == AF_INET6 && physical.InterfaceIndex6 != 0) {
      SOCKADDR_IN6* sin6 = (SOCKADDR_IN6*)local;
      RtlCopyMemory(&sin6->sin6_addr, physical.Address6, sizeof(physical.Address6));
    }
    FwpsApplyModifiedLayerData(classifyHandle, bindRequest, 0);
  }
  FwpsReleaseClassifyHandle(classifyHandle);

  // Known refinement (not destination-based exclusion): an excluded process that
  // talks to 127.0.0.1 after we rebind it to the physical source would break its
  // loopback. Mullvad-style designs add a COMPLEMENTARY connect-redirect that,
  // for loopback/local *destinations only*, reverts the source to loopback. That
  // is a safety fixup keyed on the destination being local — it never changes
  // WHICH sockets are excluded (still purely the process set). Deferred to the
  // hardening pass (R10) so we do not ship an unverified second callout.
}

static NTSTATUS NTAPI UrstNotify(FWPS_CALLOUT_NOTIFY_TYPE type,
                                 const GUID* filterKey, const FWPS_FILTER* filter) {
  UNREFERENCED_PARAMETER(type);
  UNREFERENCED_PARAMETER(filterKey);
  UNREFERENCED_PARAMETER(filter);
  return STATUS_SUCCESS;
}

// ---- WFP setup -----------------------------------------------------------

static NTSTATUS UrstRegisterCallout(const GUID* calloutKey, UINT32* calloutId) {
  FWPS_CALLOUT callout = {0};
  callout.calloutKey = *calloutKey;
  callout.classifyFn = UrstClassify;
  callout.notifyFn = UrstNotify;
  return FwpsCalloutRegister(g_state.device, &callout, calloutId);
}

static NTSTATUS UrstAddFilter(const GUID* layerKey, const GUID* calloutKey,
                              const wchar_t* name) {
  FWPM_CALLOUT mCallout = {0};
  mCallout.calloutKey = *calloutKey;
  mCallout.displayData.name = (wchar_t*)name;
  mCallout.applicableLayer = *layerKey;
  FwpmCalloutAdd(g_state.engine, &mCallout, NULL, NULL);

  FWPM_FILTER filter = {0};
  filter.layerKey = *layerKey;
  filter.subLayerKey = URST_SUBLAYER;
  filter.displayData.name = (wchar_t*)name;
  filter.action.type = FWP_ACTION_CALLOUT_UNKNOWN;
  filter.action.calloutKey = *calloutKey;
  filter.weight.type = FWP_EMPTY;  // auto-weight
  filter.numFilterConditions = 0;  // all flows; the classify decides
  return FwpmFilterAdd(g_state.engine, &filter, NULL, NULL);
}

static NTSTATUS UrstSetupWfp(void) {
  NTSTATUS status = FwpmEngineOpen(NULL, RPC_C_AUTHN_WINNT, NULL, NULL, &g_state.engine);
  if (!NT_SUCCESS(status)) return status;

  FWPM_SUBLAYER sublayer = {0};
  sublayer.subLayerKey = URST_SUBLAYER;
  sublayer.displayData.name = L"URnetwork split tunnel";
  sublayer.weight = 0x8000;
  FwpmSubLayerAdd(g_state.engine, &sublayer, NULL);

  status = UrstRegisterCallout(&URST_CALLOUT_BIND_V4, &g_state.calloutIdV4);
  if (NT_SUCCESS(status))
    status = UrstRegisterCallout(&URST_CALLOUT_BIND_V6, &g_state.calloutIdV6);
  if (!NT_SUCCESS(status)) return status;

  UrstAddFilter(&FWPM_LAYER_ALE_BIND_REDIRECT_V4, &URST_CALLOUT_BIND_V4, L"URnetwork bind v4");
  UrstAddFilter(&FWPM_LAYER_ALE_BIND_REDIRECT_V6, &URST_CALLOUT_BIND_V6, L"URnetwork bind v6");

  FwpsRedirectHandleCreate(&URST_SUBLAYER, 0, &g_state.injectHandle);
  return STATUS_SUCCESS;
}

static VOID UrstTeardownWfp(void) {
  if (g_state.injectHandle) FwpsRedirectHandleDestroy(g_state.injectHandle);
  if (g_state.calloutIdV4) FwpsCalloutUnregisterById(g_state.calloutIdV4);
  if (g_state.calloutIdV6) FwpsCalloutUnregisterById(g_state.calloutIdV6);
  if (g_state.engine) {
    FwpmSubLayerDeleteByKey(g_state.engine, &URST_SUBLAYER);
    FwpmEngineClose(g_state.engine);
    g_state.engine = NULL;
  }
}

// ---- IOCTL ---------------------------------------------------------------

static VOID UrstClearExcluded(void) {
  KIRQL irql = ExAcquireSpinLockExclusive(&g_state.lock);
  if (g_state.paths) {
    for (ULONG i = 0; i < g_state.pathCount; ++i) {
      if (g_state.paths[i].Buffer) ExFreePoolWithTag(g_state.paths[i].Buffer, URST_POOL_TAG);
    }
    ExFreePoolWithTag(g_state.paths, URST_POOL_TAG);
    g_state.paths = NULL;
  }
  g_state.pathCount = 0;
  g_state.trackedCount = 0;  // stop tracking; new launches re-evaluate
  g_state.enabled = FALSE;
  ExReleaseSpinLockExclusive(&g_state.lock, irql);
}

// Parse the packed URST_EXCLUDED_PATHS payload into owned UNICODE_STRINGs.
static NTSTATUS UrstSetExcludedPaths(const UINT8* buf, ULONG len) {
  if (len < sizeof(URST_EXCLUDED_PATHS_HEADER)) return STATUS_BUFFER_TOO_SMALL;
  const URST_EXCLUDED_PATHS_HEADER* hdr = (const URST_EXCLUDED_PATHS_HEADER*)buf;
  ULONG count = hdr->Count;
  const UINT8* p = buf + sizeof(URST_EXCLUDED_PATHS_HEADER);
  const UINT8* end = buf + len;

  UNICODE_STRING* paths = NULL;
  if (count) {
    paths = ExAllocatePool2(POOL_FLAG_NON_PAGED, count * sizeof(UNICODE_STRING), URST_POOL_TAG);
    if (!paths) return STATUS_INSUFFICIENT_RESOURCES;
  }
  ULONG parsed = 0;
  for (; parsed < count; ++parsed) {
    if (p + sizeof(UINT16) > end) break;
    UINT16 chars = *(const UINT16*)p;
    p += sizeof(UINT16);
    ULONG bytes = (ULONG)chars * sizeof(WCHAR);
    if (p + bytes > end) break;
    WCHAR* copy = ExAllocatePool2(POOL_FLAG_NON_PAGED, bytes, URST_POOL_TAG);
    if (!copy) break;
    RtlCopyMemory(copy, p, bytes);
    paths[parsed].Buffer = copy;
    paths[parsed].Length = (USHORT)bytes;
    paths[parsed].MaximumLength = (USHORT)bytes;
    p += bytes;
  }

  UrstClearExcluded();
  KIRQL irql = ExAcquireSpinLockExclusive(&g_state.lock);
  g_state.paths = paths;
  g_state.pathCount = parsed;
  ExReleaseSpinLockExclusive(&g_state.lock, irql);
  return STATUS_SUCCESS;
}

static NTSTATUS UrstDeviceControl(DEVICE_OBJECT* device, IRP* irp) {
  UNREFERENCED_PARAMETER(device);
  IO_STACK_LOCATION* stack = IoGetCurrentIrpStackLocation(irp);
  ULONG code = stack->Parameters.DeviceIoControl.IoControlCode;
  ULONG inLen = stack->Parameters.DeviceIoControl.InputBufferLength;
  void* buf = irp->AssociatedIrp.SystemBuffer;
  NTSTATUS status = STATUS_SUCCESS;

  switch (code) {
    case IOCTL_URST_SET_ENABLED:
      if (inLen >= sizeof(URST_ENABLED)) {
        KIRQL irql = ExAcquireSpinLockExclusive(&g_state.lock);
        g_state.enabled = ((URST_ENABLED*)buf)->Enabled != 0;
        ExReleaseSpinLockExclusive(&g_state.lock, irql);
      } else status = STATUS_BUFFER_TOO_SMALL;
      break;
    case IOCTL_URST_SET_PHYSICAL_ADDRS:
      if (inLen >= sizeof(URST_PHYSICAL_ADDRS)) {
        KIRQL irql = ExAcquireSpinLockExclusive(&g_state.lock);
        g_state.physical = *(URST_PHYSICAL_ADDRS*)buf;
        ExReleaseSpinLockExclusive(&g_state.lock, irql);
      } else status = STATUS_BUFFER_TOO_SMALL;
      break;
    case IOCTL_URST_SET_EXCLUDED_PATHS:
      status = UrstSetExcludedPaths((const UINT8*)buf, inLen);
      break;
    case IOCTL_URST_CLEAR:
      UrstClearExcluded();
      break;
    default:
      status = STATUS_INVALID_DEVICE_REQUEST;
      break;
  }

  irp->IoStatus.Status = status;
  irp->IoStatus.Information = 0;
  IoCompleteRequest(irp, IO_NO_INCREMENT);
  return status;
}

static NTSTATUS UrstCreateClose(DEVICE_OBJECT* device, IRP* irp) {
  UNREFERENCED_PARAMETER(device);
  irp->IoStatus.Status = STATUS_SUCCESS;
  irp->IoStatus.Information = 0;
  IoCompleteRequest(irp, IO_NO_INCREMENT);
  return STATUS_SUCCESS;
}

// ---- lifecycle -----------------------------------------------------------

static VOID UrstUnload(DRIVER_OBJECT* driver) {
  UNICODE_STRING symlink;
  RtlInitUnicodeString(&symlink, URST_SYMLINK_NAME);
  PsSetCreateProcessNotifyRoutineEx(UrstCreateProcessNotify, TRUE /*remove*/);
  UrstTeardownWfp();
  UrstClearExcluded();
  if (g_state.trackedPids) ExFreePoolWithTag(g_state.trackedPids, URST_POOL_TAG);
  IoDeleteSymbolicLink(&symlink);
  if (driver->DeviceObject) IoDeleteDevice(driver->DeviceObject);
}

NTSTATUS DriverEntry(DRIVER_OBJECT* driver, UNICODE_STRING* registryPath) {
  UNREFERENCED_PARAMETER(registryPath);
  RtlZeroMemory(&g_state, sizeof(g_state));

  UNICODE_STRING deviceName, symlink;
  RtlInitUnicodeString(&deviceName, URST_DEVICE_NAME);
  RtlInitUnicodeString(&symlink, URST_SYMLINK_NAME);

  NTSTATUS status = IoCreateDevice(driver, 0, &deviceName, FILE_DEVICE_NETWORK,
                                   FILE_DEVICE_SECURE_OPEN, FALSE, &g_state.device);
  if (!NT_SUCCESS(status)) return status;
  status = IoCreateSymbolicLink(&symlink, &deviceName);
  if (!NT_SUCCESS(status)) {
    IoDeleteDevice(g_state.device);
    return status;
  }

  driver->MajorFunction[IRP_MJ_CREATE] = UrstCreateClose;
  driver->MajorFunction[IRP_MJ_CLOSE] = UrstCreateClose;
  driver->MajorFunction[IRP_MJ_DEVICE_CONTROL] = UrstDeviceControl;
  driver->DriverUnload = UrstUnload;

  status = PsSetCreateProcessNotifyRoutineEx(UrstCreateProcessNotify, FALSE);
  if (!NT_SUCCESS(status)) {
    IoDeleteSymbolicLink(&symlink);
    IoDeleteDevice(g_state.device);
    return status;
  }

  status = UrstSetupWfp();
  if (!NT_SUCCESS(status)) {
    PsSetCreateProcessNotifyRoutineEx(UrstCreateProcessNotify, TRUE);
    IoDeleteSymbolicLink(&symlink);
    IoDeleteDevice(g_state.device);
    return status;
  }

  return STATUS_SUCCESS;
}
