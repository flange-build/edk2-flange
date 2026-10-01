/** @file
  QCS6490 platform settings kept in UEFI variables: their vendor GUID, which
  is also the GUID of the Platform Configuration formset, and their layout.

  The early SEC code reads HypervisorMode straight out of the variable store
  before it decides whether to leave Gunyah, so its name, GUID and meaning are
  part of the boot flow, not only of the setup UI.

  Copyright (c) 2026, edk2-flange contributors.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#ifndef QCS6490_PLATFORM_CONFIG_H_
#define QCS6490_PLATFORM_CONFIG_H_

#define QCS6490_PLATFORM_CONFIG_GUID \
  { 0xe2442d08, 0x80fa, 0x478e, { 0x8c, 0x24, 0x88, 0x38, 0xb4, 0x5f, 0x8d, 0xa9 } }

//
// Which exception level UEFI and the OS run at. Stored as a one-byte
// QCS6490_HYPERVISOR_CONFIG in the variable below, non-volatile and boot
// services only.
//
#define QCS6490_HYPERVISOR_MODE_VARIABLE  L"HypervisorMode"

//
// Follow the boot firmware: xbl_config's /sw/uefi/uefiplat
// OsConfigTableSelection, the way the stock Qualcomm UEFI does (1 keeps
// Gunyah, 2 leaves it for KVM), or PcdExitGunyah when xbl_config says
// nothing.
//
#define QCS6490_HYPERVISOR_MODE_AUTO  0

//
// Stay a Gunyah guest at EL1.
//
#define QCS6490_HYPERVISOR_MODE_EL1  1

//
// Ask TrustZone to remove Gunyah, and run UEFI and the OS at EL2 (KVM).
//
#define QCS6490_HYPERVISOR_MODE_EL2  2

typedef struct {
  UINT8    Mode;
} QCS6490_HYPERVISOR_CONFIG;

//
// Whether UEFI starts the DSPs (ADSP and CDSP) before the OS, for a kernel
// that attaches to them: Linux at EL2 cannot start them itself on these
// boards. Stored as a QCS6490_DSP_PRELOAD_CONFIG in the variable below,
// non-volatile and boot services only. Read by DspPreloadDxe at
// ReadyToBoot.
//
#define QCS6490_DSP_PRELOAD_VARIABLE  L"DspPreload"

//
// Preload when UEFI runs at EL2. At EL1 the OS starts the DSPs itself.
//
#define QCS6490_DSP_PRELOAD_AUTO  0

//
// Never preload.
//
#define QCS6490_DSP_PRELOAD_DISABLED  1

//
// Preload at EL1 too, for a kernel that attaches there.
//
#define QCS6490_DSP_PRELOAD_ALWAYS  2

typedef struct {
  UINT8    Mode;
  UINT8    Adsp;          // preload the ADSP
  UINT8    Cdsp;          // preload the CDSP
} QCS6490_DSP_PRELOAD_CONFIG;

#ifndef VFRCOMPILE
extern EFI_GUID  gQcs6490PlatformConfigGuid;
#endif

#endif // QCS6490_PLATFORM_CONFIG_H_
