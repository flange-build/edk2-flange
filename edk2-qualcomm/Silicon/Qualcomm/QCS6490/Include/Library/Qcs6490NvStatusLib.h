/** @file
  The status page SEC leaves after the variable store (Qcs6490NvStore.h),
  as DXE drivers read it: what SEC found and decided on this boot.

  Copyright (c) 2026, edk2-flange contributors.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#ifndef QCS6490_NV_STATUS_LIB_H_
#define QCS6490_NV_STATUS_LIB_H_

#include <Qcs6490NvStore.h>

/**
  Returns the status page SEC filled in on this boot.

  @return  The status page, or NULL if it is missing or does not add up.
**/
QCS6490_NVSTORE_STATUS *
EFIAPI
Qcs6490GetNvStatus (
  VOID
  );

/**
  Returns whether UEFI runs as a Gunyah guest until the OS loader's
  ExitBootServices, and leaves Gunyah for EL2 then (GunyahExitDxe): the OS
  runs at EL2 although UEFI runs at EL1.

  @retval TRUE   Gunyah leaves at ExitBootServices.
  @retval FALSE  UEFI runs at the exception level the OS gets.
**/
BOOLEAN
EFIAPI
Qcs6490GunyahExitDeferred (
  VOID
  );

#endif // QCS6490_NV_STATUS_LIB_H_
