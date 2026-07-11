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

#include <winioctl.h>

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

// Replace the excluded-image-path set. Input: URST_EXCLUDED_PATHS (variable).
#define IOCTL_URST_SET_EXCLUDED_PATHS URST_IOCTL(0x802)

// Clear all excluded paths and disable. No input.
#define IOCTL_URST_CLEAR URST_IOCTL(0x803)

#pragma pack(push, 1)

typedef struct _URST_ENABLED {
  UINT32 Enabled;
} URST_ENABLED;

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
