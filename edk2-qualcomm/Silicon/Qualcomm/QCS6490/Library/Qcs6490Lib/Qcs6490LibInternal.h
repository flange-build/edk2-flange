/** @file
  Copyright (c) 2026, edk2-flange contributors.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#pragma once

#include <Base.h>

//
// gQcs6490ExitGunyahStatus before ArmPlatformPeiBootAction made the call.
//
#define QCS6490_SMC_NOT_ISSUED  MAX_INT32

typedef struct {
  UINT64         Base;
  UINT64         Size;
  CONST CHAR8    *Name;
} QCS6490_MEMORY_RANGE;

//
// Carve-outs owned by the boot firmware, hypervisor, TrustZone and the remote
// processors, sorted by address and non-overlapping. Qcs6490Mem.c.
//
extern CONST QCS6490_MEMORY_RANGE  gQcs6490Carveouts[];
extern CONST UINTN                 gQcs6490CarveoutCount;

//
// Whether ArmPlatformPeiBootAction asked TrustZone to remove Gunyah, as
// Qcs6490EarlyInit() decided. PcdExitGunyah until then.
//
extern BOOLEAN  gQcs6490ExitGunyah;

//
// What TrustZone returned to ArmPlatformPeiBootAction for the call to
// remove Gunyah.
//
extern INT32  gQcs6490ExitGunyahStatus;

//
// Set by Qcs6490EarlyInit(): the status page describes this boot.
//
extern BOOLEAN  gQcs6490EarlyInitDone;

/**
  Prints a message on the serial console, in RELEASE builds too.

  For the few things worth knowing about every boot; everything else goes
  through DEBUG ().

  @param[in]  Format  An ASCII format string.
  @param[in]  ...     The arguments of Format.
**/
VOID
EFIAPI
Qcs6490Print (
  IN CONST CHAR8  *Format,
  ...
  );
