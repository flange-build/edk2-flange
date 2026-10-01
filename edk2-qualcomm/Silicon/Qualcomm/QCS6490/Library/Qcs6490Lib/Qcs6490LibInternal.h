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

//
// PcdExitGunyah, for ArmPlatformPeiBootAction, which runs before there is a
// stack.
//
extern CONST BOOLEAN  gQcs6490ExitGunyah;

//
// What TrustZone returned to ArmPlatformPeiBootAction for the call to
// remove Gunyah.
//
extern INT32  gQcs6490ExitGunyahStatus;

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
