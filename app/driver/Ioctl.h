// Shared IOCTL contract between the URnetwork split-tunnel driver
// (SplitTunnel.sys) and the service (urnetworkd, the only client). Included by
// both the kernel driver and the user-mode SplitTunnelClient.
//
// The driver redirects the outbound socket binds of excluded processes to the
// physical interface, so those apps bypass the tunnel (the Mullvad/NordVPN-class
// mechanism), implemented clean-room from Microsoft WFP documentation — see
// driver/PROVENANCE.md.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

// CTL_CODE / METHOD_BUFFERED / FILE_WRITE_ACCESS come from <winioctl.h> in user
// mode (the SplitTunnelClient side), but in kernel mode they are already provided
// by <wdm.h> (via <ntddk.h>, included before this header in Driver.c). Including
// the user-mode <winioctl.h> in the driver redefines DEVICE_TYPE (C4005 -> C2220
// warnings-as-errors) and drags in user-mode-only types (FILE_ID_128, USN_RECORD_*
// -> C2061), so guard it to user mode only.
#ifndef _KERNEL_MODE
#include <winioctl.h>
#endif

// Device type in the vendor range.
#define URST_DEVICE_TYPE 0x8000

// FILE_WRITE_ACCESS on every control code so only an admin/SYSTEM handle (the
// service) can drive the driver.
#define URST_IOCTL(fn) \
  CTL_CODE(URST_DEVICE_TYPE, (fn), METHOD_BUFFERED, FILE_WRITE_ACCESS)

// Enable/disable redirection. Input: URST_ENABLED (UINT32, 0/1).
#define IOCTL_URST_SET_ENABLED URST_IOCTL(0x800)

// Set the physical interface addresses excluded flows are rebound to.
// Input: URST_PHYSICAL_ADDRS.
#define IOCTL_URST_SET_PHYSICAL_ADDRS URST_IOCTL(0x801)

// Replace the image-path set. Input: URST_EXCLUDED_PATHS (variable). The set's
// MEANING depends on the mode (IOCTL_URST_SET_MODE): in denylist mode it is the
// BYPASS set (redirect these; everyone else tunnels); in allowlist mode it is the
// KEEP-ON-TUNNEL set (only these tunnel; everyone else is redirected).
#define IOCTL_URST_SET_EXCLUDED_PATHS URST_IOCTL(0x802)

// Clear all paths, reset to denylist, and disable. No input.
#define IOCTL_URST_CLEAR URST_IOCTL(0x803)

// Select redirect mode. Input: URST_MODE.
//   Denylist (0, default): the path set is the BYPASS set - those apps egress the
//     physical NIC; every other process stays on the tunnel (permit unchanged).
//   Allowlist (1): the path set is the KEEP-ON-TUNNEL set - only those apps stay
//     on the tunnel; every OTHER process is redirected to the physical NIC.
// Mirrors Android: any app marked include-in-tunnel flips the whole tunnel to
// allowlist ("inclusions take precedence"). The controlling service (urnetworkd,
// the process that opened this device) is ALWAYS exempt from redirection in both
// modes, so the VPN transport is never rebound - see the classify path.
#define IOCTL_URST_SET_MODE URST_IOCTL(0x804)

#pragma pack(push, 1)

typedef struct _URST_ENABLED {
  UINT32 Enabled;
} URST_ENABLED;

typedef struct _URST_MODE {
  UINT32 Allowlist;  // 0 = denylist (redirect the path set), 1 = allowlist (redirect all BUT the path set)
} URST_MODE;

typedef struct _URST_PHYSICAL_ADDRS {
  UINT32 InterfaceIndex4;  // physical IPv4 interface index (0 = none)
  UINT32 InterfaceIndex6;  // physical IPv6 interface index (0 = none)
  UINT8 Address4[4];       // physical IPv4 source address (network order)
  UINT8 Address6[16];      // physical IPv6 source address (network order)
} URST_PHYSICAL_ADDRS;

// Excluded paths are packed as: UINT32 Count, then Count entries each of
// { UINT16 LengthChars; WCHAR Path[LengthChars]; } with no null terminators.
// Paths are full NT device or DOS image paths, compared case-insensitively
// against the process image at bind time.
typedef struct _URST_EXCLUDED_PATHS_HEADER {
  UINT32 Count;
  // followed by Count packed entries
} URST_EXCLUDED_PATHS_HEADER;

#pragma pack(pop)
