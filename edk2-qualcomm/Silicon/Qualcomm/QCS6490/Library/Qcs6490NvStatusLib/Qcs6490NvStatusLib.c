/** @file
  The status page SEC leaves after the variable store, for DXE drivers.

  Copyright (c) 2026, edk2-flange contributors.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Base.h>
#include <Library/ArmLib.h>
#include <Library/PcdLib.h>
#include <Library/Qcs6490NvStatusLib.h>

QCS6490_NVSTORE_STATUS *
EFIAPI
Qcs6490GetNvStatus (
  VOID
  )
{
  QCS6490_NVSTORE_STATUS  *Status;

  Status = (QCS6490_NVSTORE_STATUS *)(UINTN)QCS6490_NVSTORE_STATUS_BASE;

  //
  // The same checks as the FVB driver's and the setup UI's.
  //
  if ((Status->Signature != QCS6490_NVSTORE_STATUS_SIGNATURE) ||
      (Status->Version != QCS6490_NVSTORE_STATUS_VERSION) ||
      (Status->Size != sizeof (*Status)))
  {
    return NULL;
  }

  return Status;
}

BOOLEAN
EFIAPI
Qcs6490GunyahExitDeferred (
  VOID
  )
{
  QCS6490_NVSTORE_STATUS  *Status;

  Status = Qcs6490GetNvStatus ();
  return (Status != NULL) &&
         (Status->GunyahExit == QCS6490_GUNYAH_EXIT_EXIT_BOOT_SERVICES) &&
         (ArmReadCurrentEL () == AARCH64_EL1);
}
