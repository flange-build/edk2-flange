/** @file
  Leaves Gunyah for EL2 once the OS loader's ExitBootServices succeeded, in
  boots that run the OS at EL2 but UEFI as a Gunyah guest (SEC decides that,
  Library/Qcs6490NvStatusLib.h).

  TrustZone starts the DSPs only for a Gunyah guest: at EL2 it accepts every
  PAS call but the DSPs never run. So DspPreloadDxe starts them while Gunyah
  is still there, and the OS gets EL2 afterwards. That is also when the stock
  Qualcomm firmware leaves Gunyah: its EnvDxe makes the call from an event
  group its DXE core signals right after the ExitBootServices one.

  Here the boot services' ExitBootServices is wrapped. The switch happens
  once the original has returned success, after every ExitBootServices
  notification ran at EL1 as usual (SmmuDxe putting its entries back, the
  timer, the GIC); an OS loader whose first ExitBootServices fails retries
  with a new memory map, and the switch waits for the call that succeeds.
  The OS loader then carries on at EL2 with the MMU on and the same
  translation tables (AArch64/GunyahExit.S), interrupts masked.

  Gunyah wipes the SMMU stream match entries it does not leave in bypass, so
  SmmuDxe then sets up those of the DSPs that run, for Linux to adopt.

  Copyright (c) 2026, edk2-flange contributors.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/PrintLib.h>
#include <Library/Qcs6490NvStatusLib.h>
#include <Library/SerialPortLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/Qcs6490GunyahExit.h>
#include <Protocol/Qcs6490Smmu.h>

#include "GunyahExit.h"

STATIC_ASSERT (sizeof (GUNYAH_EXIT_STATE) == GUNYAH_EXIT_STATE_SIZE, "GUNYAH_EXIT_STATE does not match GunyahExit.S");
STATIC_ASSERT (OFFSET_OF (GUNYAH_EXIT_STATE, El1Sp) == GUNYAH_EXIT_EL1_SP, "GUNYAH_EXIT_STATE does not match GunyahExit.S");
STATIC_ASSERT (OFFSET_OF (GUNYAH_EXIT_STATE, Result) == GUNYAH_EXIT_RESULT, "GUNYAH_EXIT_STATE does not match GunyahExit.S");
STATIC_ASSERT (OFFSET_OF (GUNYAH_EXIT_STATE, LeftSp) == GUNYAH_EXIT_LEFT_SP, "GUNYAH_EXIT_STATE does not match GunyahExit.S");
STATIC_ASSERT (OFFSET_OF (GUNYAH_EXIT_STATE, El2Tcr) == GUNYAH_EXIT_EL2_TCR, "GUNYAH_EXIT_STATE does not match GunyahExit.S");
STATIC_ASSERT (OFFSET_OF (GUNYAH_EXIT_STATE, Isr) == GUNYAH_EXIT_ISR, "GUNYAH_EXIT_STATE does not match GunyahExit.S");

//
// What Gunyah returns when it does not leave.
//
#define GUNYAH_EXIT_TRAMPOLINE      0x00    // at EL1: its EL2 jump could not be mapped, EL2 already given up
#define GUNYAH_EXIT_ALREADY_CALLED  0x04    // a second call in this boot
#define GUNYAH_EXIT_SMMU_FAILED     0x10    // the SMMU clean-up failed: SMMU in global bypass
#define GUNYAH_EXIT_LATE_FAILURE    0x11    // the SMMU clean-up, or mapping UEFI after EL2 was given up
#define GUNYAH_EXIT_TZ_QUERY        0x37    // TrustZone's feature query failed

#define GUNYAH_EXIT_MAX_NOTIFIES  4

typedef struct {
  QCS6490_GUNYAH_EXIT_NOTIFY    Notify;
  VOID                          *Context;
} GUNYAH_EXIT_NOTIFY_ENTRY;

STATIC GUNYAH_EXIT_NOTIFY_ENTRY  mNotifies[GUNYAH_EXIT_MAX_NOTIFIES];
STATIC UINTN                     mNotifyCount;
STATIC EFI_EXIT_BOOT_SERVICES    mOriginalExitBootServices;
STATIC QCS6490_NVSTORE_STATUS    *mNvStatus;
STATIC QCS6490_SMMU_PROTOCOL     *mSmmu;
STATIC BOOLEAN                   mSwitched;

/**
  Prints a line on the serial console, in RELEASE builds too: the boot
  services are gone by the time most of these are printed.

  @param[in]  Format  The format.
  @param[in]  ...     Its arguments.
**/
STATIC
VOID
EFIAPI
GunyahExitPrint (
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
  Implements QCS6490_GUNYAH_EXIT_PROTOCOL.RegisterNotify().

  @param[in]  This     The protocol.
  @param[in]  Notify   The function.
  @param[in]  Context  Passed to it.

  @retval EFI_SUCCESS           Registered.
  @retval EFI_OUT_OF_RESOURCES  Too many functions registered.
**/
STATIC
EFI_STATUS
EFIAPI
GunyahExitRegisterNotify (
  IN QCS6490_GUNYAH_EXIT_PROTOCOL  *This,
  IN QCS6490_GUNYAH_EXIT_NOTIFY    Notify,
  IN VOID                          *Context
  )
{
  if (mNotifyCount == ARRAY_SIZE (mNotifies)) {
    return EFI_OUT_OF_RESOURCES;
  }

  mNotifies[mNotifyCount].Notify  = Notify;
  mNotifies[mNotifyCount].Context = Context;
  mNotifyCount++;
  return EFI_SUCCESS;
}

STATIC QCS6490_GUNYAH_EXIT_PROTOCOL  mGunyahExitProtocol = {
  GunyahExitRegisterNotify
};

/**
  Calls the registered functions.

  @param[in]  Phase  Where the exit is.
**/
STATIC
VOID
GunyahExitNotify (
  IN QCS6490_GUNYAH_EXIT_PHASE  Phase
  )
{
  UINTN  Index;

  for (Index = 0; Index < mNotifyCount; Index++) {
    mNotifies[Index].Notify (mNotifies[Index].Context, Phase);
  }
}

/**
  Names what Gunyah returned when it stays.

  @param[in]  Result  x0.

  @return  The reason.
**/
STATIC
CONST CHAR8 *
GunyahExitRefusal (
  IN UINT64  Result
  )
{
  switch (Result) {
    case GUNYAH_EXIT_TRAMPOLINE:
      return "it could not map its jump to EL2, after giving EL2 up and stopping its GIC and watchdog";
    case GUNYAH_EXIT_ALREADY_CALLED:
      return "called before in this boot, or a bad mode";
    case GUNYAH_EXIT_SMMU_FAILED:
      return "its SMMU clean-up failed, the SMMU is in global bypass";
    case GUNYAH_EXIT_LATE_FAILURE:
      return "its SMMU clean-up failed (SMMU in global bypass), or mapping UEFI failed after it gave EL2 up";
    case GUNYAH_EXIT_TZ_QUERY:
      return "TrustZone's feature query failed";
    default:
      return "unknown, possibly after it gave EL2 up";
  }
}

/**
  Leaves Gunyah, after the original ExitBootServices succeeded.
**/
STATIC
VOID
GunyahExitLeave (
  VOID
  )
{
  GUNYAH_EXIT_STATE  State;
  UINT64             Result;
  EFI_STATUS         Status;

  ZeroMem (&State, sizeof (State));

  GunyahExitNotify (Qcs6490GunyahExitBefore);

  //
  // GunyahExitSwitch () adds a letter per step: s (calling), r (back at
  // EL2), t (tables in place), m (MMU on); 1 if Gunyah stays.
  //
  GunyahExitPrint ("GunyahExit: leaving Gunyah: ");
  Result = GunyahExitSwitch (&State);
  GunyahExitPrint ("\n");

  if (mNvStatus != NULL) {
    mNvStatus->LateExitStatus = (INT32)Result;
  }

  if (State.CurrentEl != 2) {
    GunyahExitPrint (
      "GunyahExit: Gunyah stays (0x%lx: %a), still at EL1: the OS expects EL2 and is unlikely to run.\n"
      "GunyahExit: set Hypervisor to EL1, or DSP preload to Disabled, in the setup menu\n",
      Result,
      GunyahExitRefusal (Result)
      );
    GunyahExitNotify (Qcs6490GunyahExitRefused);
    return;
  }

  GunyahExitPrint (
    "GunyahExit: at EL2 (x0 %ld); Gunyah left HCR_EL2 0x%lx SCTLR_EL2 0x%lx TCR_EL2 0x%lx VBAR_EL2 0x%lx\n",
    (INT64)Result,
    State.LeftHcr,
    State.LeftSctlr,
    State.LeftTcr,
    State.LeftVbar
    );
  GunyahExitPrint (
    "GunyahExit: and CPTR_EL2 0x%lx CNTVOFF_EL2 0x%lx DAIF|SPSel 0x%lx SP 0x%lx (UEFI's SP 0x%lx)\n",
    State.LeftCptr,
    State.LeftCntvoff,
    State.LeftDaif,
    State.LeftSp,
    State.El1Sp
    );
  GunyahExitPrint (
    "GunyahExit: MMU on at EL2 with UEFI's tables: TTBR0 0x%lx, TCR_EL2 0x%lx from TCR_EL1 0x%lx, MAIR 0x%lx, VBAR 0x%lx, ISR 0x%lx\n",
    State.El1Ttbr0,
    State.El2Tcr,
    State.El1Tcr,
    State.El1Mair,
    State.El1Vbar,
    State.Isr
    );

  if (mSmmu != NULL) {
    Status = mSmmu->AfterGunyahExit (mSmmu);
    if (EFI_ERROR (Status)) {
      GunyahExitPrint ("GunyahExit: SMMU not set up for the OS: %r\n", Status);
    }
  } else {
    GunyahExitPrint ("GunyahExit: no SMMU protocol, the DSPs' streams are not set up for the OS\n");
  }

  GunyahExitNotify (Qcs6490GunyahExitDone);
  GunyahExitPrint ("GunyahExit: back to the OS loader at EL2\n");
}

/**
  Wraps the boot services' ExitBootServices: leaves Gunyah once the original
  has succeeded.

  @param[in]  ImageHandle  The OS loader.
  @param[in]  MapKey       Its memory map key.

  @return  What the original returned.
**/
STATIC
EFI_STATUS
EFIAPI
GunyahExitBootServices (
  IN EFI_HANDLE  ImageHandle,
  IN UINTN       MapKey
  )
{
  EFI_STATUS  Status;

  Status = mOriginalExitBootServices (ImageHandle, MapKey);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // Gunyah takes one call per boot; the boot services are gone from here on.
  //
  if (!mSwitched) {
    mSwitched = TRUE;
    GunyahExitLeave ();
  }

  return Status;
}

/**
  Finds SmmuDxe's protocol before the boot services go away.

  @param[in]  Event    The ReadyToBoot event.
  @param[in]  Context  Unused.
**/
STATIC
VOID
EFIAPI
GunyahExitOnReadyToBoot (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  if (mSmmu == NULL) {
    gBS->LocateProtocol (&gQcs6490SmmuProtocolGuid, NULL, (VOID **)&mSmmu);
  }
}

/**
  Entry point.

  @param[in]  ImageHandle  The image handle.
  @param[in]  SystemTable  The system table.

  @retval EFI_SUCCESS      ExitBootServices is wrapped.
  @retval EFI_UNSUPPORTED  Gunyah does not leave at ExitBootServices in this
                           boot.
  @retval Other            The protocol or the event could not be set up.
**/
EFI_STATUS
EFIAPI
GunyahExitDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  Handle;
  EFI_EVENT   Event;
  EFI_TPL     OldTpl;

  if (!Qcs6490GunyahExitDeferred ()) {
    return EFI_UNSUPPORTED;
  }

  mNvStatus = Qcs6490GetNvStatus ();

  Status = EfiCreateEventReadyToBootEx (TPL_CALLBACK, GunyahExitOnReadyToBoot, NULL, &Event);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Handle = NULL;
  Status = gBS->InstallMultipleProtocolInterfaces (
                  &Handle,
                  &gQcs6490GunyahExitProtocolGuid,
                  &mGunyahExitProtocol,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    gBS->CloseEvent (Event);
    return Status;
  }

  OldTpl                    = gBS->RaiseTPL (TPL_HIGH_LEVEL);
  mOriginalExitBootServices = gBS->ExitBootServices;
  gBS->ExitBootServices     = GunyahExitBootServices;
  gBS->Hdr.CRC32            = 0;
  gBS->CalculateCrc32 ((UINT8 *)gBS, gBS->Hdr.HeaderSize, &gBS->Hdr.CRC32);
  gBS->RestoreTPL (OldTpl);

  GunyahExitPrint ("GunyahExit: Gunyah leaves once the OS loader's ExitBootServices succeeded\n");
  return EFI_SUCCESS;
}
