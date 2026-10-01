/** @file

  QCS6490 platform support functions.

  Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
  Copyright (c) 2026, edk2-flange contributors.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Library/ArmLib.h>
#include <Library/ArmPlatformLib.h>
#include <Library/ArmSmcLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/PcdLib.h>
#include <Library/PrintLib.h>
#include <Library/SerialPortLib.h>

#include <Ppi/ArmMpCoreInfo.h>

#include <Uefi/UefiBaseType.h>
#include <Guid/Qcs6490PlatformConfig.h>
#include <Qcs6490NvStore.h>

#include "Qcs6490Early.h"
#include "Qcs6490Helper.h"
#include "Qcs6490LibInternal.h"

//
// Written by Qcs6490EarlyInit() and ArmPlatformPeiBootAction, in the FD XBL
// loaded into DRAM.
//
BOOLEAN  gQcs6490ExitGunyah       = FixedPcdGetBool (PcdExitGunyah);
INT32    gQcs6490ExitGunyahStatus = QCS6490_SMC_NOT_ISSUED;

ARM_CORE_INFO  mPlatformCoreInfoTable[] = {
  {
    // Cluster 0, Core 0
    0x000,
  },
};

/**
  Prints a message on the serial console, in RELEASE builds too.

  @param[in]  Format  An ASCII format string.
  @param[in]  ...     The arguments of Format.
**/
VOID
EFIAPI
Qcs6490Print (
  IN CONST CHAR8  *Format,
  ...
  )
{
  CHAR8    Buffer[256];
  UINTN    Length;
  VA_LIST  Marker;

  VA_START (Marker, Format);
  Length = AsciiVSPrint (Buffer, sizeof (Buffer), Format, Marker);
  VA_END (Marker);

  SerialPortWrite ((UINT8 *)Buffer, Length);
}

/**
  Tells TrustZone whether Gunyah stays, the call ArmPlatformPeiBootAction
  makes to remove it.

  @param[in]  Mode  TZ_EL2_SWITCH_PARAM2_KEEP_GUNYAH or
                    TZ_EL2_SWITCH_PARAM2_EXIT_GUNYAH.

  @return  What TrustZone returned: 0 on success, negative on failure.
**/
STATIC
INT32
TzEl2Switch (
  IN UINTN  Mode
  )
{
  ARM_SMC_ARGS  Args;
  UINTN         Function;
  UINTN         Session;

  Function = TZ_EL2_SWITCH_SMC_ID;
  Session  = 0;

  do {
    ZeroMem (&Args, sizeof (Args));
    Args.Arg0 = Function;
    Args.Arg1 = TZ_EL2_SWITCH_PARAM_ID;
    Args.Arg4 = Mode;
    Args.Arg6 = Session;
    ArmCallSmc (&Args);

    //
    // A positive result means TZ was preempted: resume the call with the
    // result as the function ID and the session TZ returned.
    //
    Function = Args.Arg0;
    Session  = Args.Arg6;
  } while ((INT32)Args.Arg0 > 0);

  return (INT32)Args.Arg0;
}

/**
  Timer constructor.

  This function should be better located into TimerLib implementation.

  @retval EFI_SUCCESS   The timer was initialized successfully.
**/
EFI_STATUS
EFIAPI
TimerConstructor (
  VOID
  )
{
  return EFI_SUCCESS;
}

/**
  Return the current Boot Mode

  This function returns the boot mode on the platform.

  @retval BOOT_WITH_FULL_CONFIGURATION  Perform a full configuration boot.
**/
EFI_BOOT_MODE
ArmPlatformGetBootMode (
  VOID
  )
{
  return BOOT_WITH_FULL_CONFIGURATION;
}

/**
  Initialize controllers that must setup in the normal world.

  This function is called by the ArmPlatformPkg/PrePi or ArmPlatformPkg/PlatformPei
  in the PEI phase.

  @param[in] MpId  The Multiprocessor Affinity Register (MPIDR) value of the core.

  @retval EFI_SUCCESS  Initialization completed successfully.
**/
EFI_STATUS
ArmPlatformInitialize (
  IN  UINTN  MpId
  )
{
  INT32                   Status;
  QCS6490_NVSTORE_STATUS  *NvStatus;

  //
  // Complete the status page with what TrustZone said. Entered at EL2, the
  // early code did not run: the page must not describe an earlier boot.
  //
  if (gQcs6490EarlyInitDone) {
    NvStatus = Qcs6490NvStoreStatusGet ();
    if (NvStatus != NULL) {
      NvStatus->ExitGunyahStatus = gQcs6490ExitGunyahStatus;
    }
  } else if (Qcs6490NvStoreLayoutValid ()) {
    Qcs6490NvStoreStatusInit (
      (ArmReadCurrentEL () == AARCH64_EL2) ? QCS6490_HYPERVISOR_MODE_EL2
                                           : QCS6490_HYPERVISOR_MODE_EL1
      );
  }

  //
  // SEC calls this first thing after its banner, so this is where to report
  // what ArmPlatformPeiBootAction did.
  //
  if (ArmReadCurrentEL () == AARCH64_EL2) {
    if (gQcs6490ExitGunyahStatus == QCS6490_SMC_NOT_ISSUED) {
      Qcs6490Print ("QCS6490: Entered at EL2\n");
    } else {
      Qcs6490Print ("QCS6490: TrustZone removed Gunyah, running at EL2\n");
    }

    return EFI_SUCCESS;
  }

  //
  // EL2 was asked for, and TrustZone refused.
  //
  if (gQcs6490ExitGunyah) {
    Qcs6490Print (
      "QCS6490: TrustZone did not remove Gunyah (%d), running at EL1\n",
      gQcs6490ExitGunyahStatus
      );
    return EFI_SUCCESS;
  }

  //
  // The stock Qualcomm UEFI confirms a boot that keeps Gunyah with the call
  // that would otherwise remove it, and stops if that fails. Warn instead:
  // the upstream RB3 Gen 2 port never makes the call.
  //
  Qcs6490Print ("QCS6490: Keeping Gunyah, running at EL1: ");
  Status = TzEl2Switch (TZ_EL2_SWITCH_PARAM2_KEEP_GUNYAH);
  if (Status == 0) {
    Qcs6490Print ("confirmed by TrustZone\n");
  } else {
    Qcs6490Print ("TrustZone returned %d\n", Status);
  }

  return EFI_SUCCESS;
}

/**
  Retrieves the multi-processor core information table for the platform.

  @param[out] CoreCount     Pointer to receive the number of entries in the core info table.
  @param[out] ArmCoreTable  Pointer to receive the base address of the ARM core info table.

  @retval EFI_SUCCESS       The core information was successfully retrieved.
  @retval EFI_UNSUPPORTED   The platform does not support MP Core information.
**/
EFI_STATUS
PrePeiCoreGetMpCoreInfo (
  OUT UINTN          *CoreCount,
  OUT ARM_CORE_INFO  **ArmCoreTable
  )
{
  if (ArmIsMpCore ()) {
    *CoreCount    = ARRAY_SIZE (mPlatformCoreInfoTable);
    *ArmCoreTable = mPlatformCoreInfoTable;
    return EFI_SUCCESS;
  } else {
    return EFI_UNSUPPORTED;
  }
}

ARM_MP_CORE_INFO_PPI  mMpCoreInfoPpi = { PrePeiCoreGetMpCoreInfo };

EFI_PEI_PPI_DESCRIPTOR  gPlatformPpiTable[] = {
  {
    EFI_PEI_PPI_DESCRIPTOR_PPI,
    &gArmMpCoreInfoPpiGuid,
    &mMpCoreInfoPpi
  }
};

/**
  Retrieves the platform-specific PPI list.

  @param[out] PpiListSize  Size in bytes of the PPI list.
  @param[out] PpiList      Pointer to the platform PPI descriptor table.
**/
VOID
ArmPlatformGetPlatformPpiList (
  OUT UINTN                   *PpiListSize,
  OUT EFI_PEI_PPI_DESCRIPTOR  **PpiList
  )
{
  if (ArmIsMpCore ()) {
    *PpiListSize = sizeof (gPlatformPpiTable);
    *PpiList     = gPlatformPpiTable;
  } else {
    *PpiListSize = 0;
    *PpiList     = NULL;
  }
}
