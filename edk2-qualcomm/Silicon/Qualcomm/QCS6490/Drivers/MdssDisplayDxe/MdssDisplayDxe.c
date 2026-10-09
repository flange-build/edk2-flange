/** @file
  HDMI output for QCS6490 boards with an LT9611 MIPI DSI to HDMI bridge on
  DSI0, as a Graphics Output Protocol.

  Nothing before UEFI sets the display up, so this driver does it all: the
  LT9611 over bit-banged I2C, then power and clocks, the DSI PHY and its PLL,
  the DSI controller and the DPU, scanning out a framebuffer at 1920x1080@60.

  Boards opting into the simple-framebuffer handoff keep the pipeline and
  its SMMU bypass alive for the OS. Other boards stop it before boot services
  exit, leaving the LT9611 powered and out of reset.

  Copyright (c) 2026, edk2-flange contributors.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include "MdssDisplay.h"
#include "Gop.h"

#include <Library/CacheMaintenanceLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include <Guid/EventGroup.h>
#include <Protocol/Cpu.h>
#include <Protocol/Qcs6490Smmu.h>

#define DISPLAY_BYTES_PER_PIXEL  4

CONST DISPLAY_TIMING  gDisplayTiming1080p60 = {
  148500,                               // PixelClockKhz
  1920,                                 // HActive
  88,                                   // HFrontPorch
  44,                                   // HSyncWidth
  148,                                  // HBackPorch
  1080,                                 // VActive
  4,                                    // VFrontPorch
  5,                                    // VSyncWidth
  36,                                   // VBackPorch
  TRUE,                                 // HSyncPositive
  TRUE,                                 // VSyncPositive
  16                                    // Vic
};

//
// How far bring-up got, so that a failure, or ExitBootServices, undoes
// exactly what was done.
//
typedef enum {
  DisplayStageNone,
  DisplayStageBridgeOn,
  DisplayStagePowerOn,
  DisplayStageDsiPhyOn,
  DisplayStageDsiHostOn,
  DisplayStageDpuSetUp,
  DisplayStageBridgeEnabled,
  DisplayStageRunning
} DISPLAY_STAGE;

STATIC DISPLAY_STAGE  mStage;

STATIC UINT8  mEdid[256];
STATIC UINTN  mEdidSize;

STATIC EFI_EVENT  mBeforeExitBootServicesEvent;

/**
  Polls a 32-bit register until (value & Mask) == Value.

  @param[in]  Address    The register.
  @param[in]  Mask       The bits to look at.
  @param[in]  Value      The value they must have.
  @param[in]  TimeoutUs  How long to wait, in microseconds.

  @retval EFI_SUCCESS  The bits reached the value.
  @retval EFI_TIMEOUT  They did not.
**/
EFI_STATUS
MmioPoll32 (
  IN UINTN   Address,
  IN UINT32  Mask,
  IN UINT32  Value,
  IN UINTN   TimeoutUs
  )
{
  UINTN  Elapsed;

  for (Elapsed = 0; ; Elapsed++) {
    if ((MmioRead32 (Address) & Mask) == Value) {
      return EFI_SUCCESS;
    }

    if (Elapsed >= TimeoutUs) {
      return EFI_TIMEOUT;
    }

    MicroSecondDelay (1);
  }
}

/**
  Undoes bring-up from wherever it got to, in reverse order. Safe to call
  more than once.
**/
STATIC
VOID
DisplayStop (
  VOID
  )
{
  if (mStage >= DisplayStageDpuSetUp) {
    DpuStop ();
  }

  if (mStage >= DisplayStageBridgeEnabled) {
    Lt9611Disable ();
  }

  if (mStage >= DisplayStageDsiHostOn) {
    DsiHostDisable ();
  }

  if (mStage >= DisplayStageDsiPhyOn) {
    DsiPhyDisable ();
  }

  if (mStage >= DisplayStagePowerOn) {
    DisplayPowerOff ();
  }

  if (mStage >= DisplayStageBridgeOn) {
    Lt9611ReleaseBus ();
  }

  mStage = DisplayStageNone;
}

/**
  Keep an opted-in GOP pipeline running; otherwise stop it before the SMMU
  mappings used only by UEFI are restored at ExitBootServices.

  @param[in]  Event    The event.
  @param[in]  Context  Unused.
**/
STATIC
VOID
EFIAPI
OnBeforeExitBootServices (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  if (mStage != DisplayStageNone) {
    if (FixedPcdGetBool (PcdDisplayHandoff) && (mStage == DisplayStageRunning)) {
      DEBUG ((DEBUG_INFO, "%a: keeping HDMI scanout for the OS\n", __func__));
      Lt9611ReleaseBus ();
      return;
    }

    DEBUG ((DEBUG_INFO, "%a: stopping the display\n", __func__));
    DisplayStop ();
  }
}

/**
  Allocates the framebuffer below 4 GiB, where the DPU can reach it, and maps
  it write-combining.

  The OS keeps the framebuffer the GOP reports out of its memory map whatever
  the memory type, so reserving it costs nothing more, and keeps it valid if
  the display is ever left running for the OS.

  @param[in]   Timing  The mode.
  @param[out]  Base    The framebuffer.
  @param[out]  Size    Its size in bytes.

  @retval EFI_SUCCESS  Allocated.
  @retval Other        Failed.
**/
STATIC
EFI_STATUS
AllocateFrameBuffer (
  IN  CONST DISPLAY_TIMING  *Timing,
  OUT EFI_PHYSICAL_ADDRESS  *Base,
  OUT UINTN                 *Size
  )
{
  EFI_STATUS             Status;
  EFI_CPU_ARCH_PROTOCOL  *Cpu;

  *Size = ALIGN_VALUE ((UINTN)Timing->HActive * Timing->VActive * DISPLAY_BYTES_PER_PIXEL, EFI_PAGE_SIZE);
  *Base = SIZE_4GB - 1;

  Status = gBS->AllocatePages (
                  AllocateMaxAddress,
                  EfiReservedMemoryType,
                  EFI_SIZE_TO_PAGES (*Size),
                  Base
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: no memory below 4 GiB: %r\n", __func__, Status));
    return Status;
  }

  //
  // No dirty line of the pages' previous life may land on the picture later.
  //
  WriteBackInvalidateDataCacheRange ((VOID *)(UINTN)*Base, *Size);

  Status = gBS->LocateProtocol (&gEfiCpuArchProtocolGuid, NULL, (VOID **)&Cpu);
  if (!EFI_ERROR (Status)) {
    Status = Cpu->SetMemoryAttributes (Cpu, *Base, *Size, EFI_MEMORY_WC);
  }

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: cannot map the framebuffer write-combining: %r\n", __func__, Status));
    gBS->FreePages (*Base, EFI_SIZE_TO_PAGES (*Size));
    return Status;
  }

  return EFI_SUCCESS;
}

/**
  Entry point.

  @param[in]  ImageHandle  The image handle.
  @param[in]  SystemTable  The system table.

  @retval EFI_SUCCESS  The display runs and has a GOP.
  @retval Other        There is no display; the hardware is as it was.
**/
EFI_STATUS
EFIAPI
MdssDisplayDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS             Status;
  LT9611_BOARD_CONFIG    Board;
  CONST DISPLAY_TIMING   *Timing;
  BOOLEAN                Hdmi;
  EFI_PHYSICAL_ADDRESS   FrameBufferBase;
  UINTN                  FrameBufferSize;
  EFI_HANDLE             GopHandle;
  QCS6490_SMMU_PROTOCOL   *Smmu;

  if (!FixedPcdGetBool (PcdDisplayEnable)) {
    return EFI_UNSUPPORTED;
  }

  // A running scanout must survive Linux enabling the SMMU. At EL1 this
  // records a deferred bypass, reapplied once Gunyah exits to EL2.
  if (FixedPcdGetBool (PcdDisplayHandoff)) {
    Status = gBS->LocateProtocol (&gQcs6490SmmuProtocolGuid, NULL, (VOID **)&Smmu);
    if (!EFI_ERROR (Status)) {
      Status = Smmu->HandOverBypass (Smmu, 0x900, 0x402, "MDSS");
    }

    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "%a: cannot hand over MDSS DMA: %r\n", __func__, Status));
      return Status;
    }
  }

  Board.I2cAddress     = FixedPcdGet8 (PcdLt9611I2cAddress);
  Board.SdaGpio        = FixedPcdGet16 (PcdLt9611SdaGpio);
  Board.SclGpio        = FixedPcdGet16 (PcdLt9611SclGpio);
  Board.I2cPinFunction = FixedPcdGet8 (PcdLt9611I2cPinFunction);
  Board.ResetGpio      = FixedPcdGet16 (PcdLt9611ResetGpio);
  Board.PowerGpio      = FixedPcdGet16 (PcdLt9611PowerGpio);
  Board.PortB          = FixedPcdGetBool (PcdLt9611PortB);

  DisplayLogEntryState ();

  Status = Lt9611PowerOn (&Board);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: no LT9611: %r\n", __func__, Status));
    return Status;
  }

  mStage = DisplayStageBridgeOn;

  //
  // Without a readable EDID, assume an HDMI sink: 1080p60 with an AVI
  // InfoFrame is what nearly every monitor and TV takes.
  //
  Hdmi = TRUE;
  if (Lt9611IsHotPlugged ()) {
    Status = Lt9611ReadEdid (mEdid, sizeof (mEdid), &mEdidSize);
    if (!EFI_ERROR (Status)) {
      Hdmi = EdidParse (mEdid, mEdidSize);
    } else {
      DEBUG ((DEBUG_WARN, "%a: no EDID (%r), assuming an HDMI sink\n", __func__, Status));
      mEdidSize = 0;
    }
  } else {
    DEBUG ((DEBUG_WARN, "%a: nothing plugged in, setting up for an HDMI sink anyway\n", __func__));
  }

  Timing = &gDisplayTiming1080p60;

  Status = AllocateFrameBuffer (Timing, &FrameBufferBase, &FrameBufferSize);
  if (EFI_ERROR (Status)) {
    goto Fail;
  }

  //
  // Black until the console and the boot logo draw on it.
  //
  ZeroMem ((VOID *)(UINTN)FrameBufferBase, FrameBufferSize);

  DEBUG ((
    DEBUG_INFO,
    "%a: %ux%u@%ukHz (%a), framebuffer 0x%lx\n",
    __func__,
    Timing->HActive,
    Timing->VActive,
    Timing->PixelClockKhz,
    Hdmi ? "HDMI" : "DVI",
    FrameBufferBase
    ));

  Status = DisplayPowerOn ();
  if (EFI_ERROR (Status)) {
    goto FreeFrameBuffer;
  }

  mStage = DisplayStagePowerOn;

  Status = DsiPhyEnable (Timing);
  if (EFI_ERROR (Status)) {
    goto FreeFrameBuffer;
  }

  mStage = DisplayStageDsiPhyOn;

  Status = DsiHostEnable (Timing);
  if (EFI_ERROR (Status)) {
    goto FreeFrameBuffer;
  }

  mStage = DisplayStageDsiHostOn;

  Status = DpuSetup (Timing, FrameBufferBase, (UINT32)Timing->HActive * DISPLAY_BYTES_PER_PIXEL);
  if (EFI_ERROR (Status)) {
    goto FreeFrameBuffer;
  }

  mStage = DisplayStageDpuSetUp;

  Status = Lt9611Enable (Timing, Hdmi);
  if (EFI_ERROR (Status)) {
    goto FreeFrameBuffer;
  }

  mStage = DisplayStageBridgeEnabled;

  Status = DpuStart ();
  if (EFI_ERROR (Status)) {
    goto FreeFrameBuffer;
  }

  mStage = DisplayStageRunning;

  Lt9611LogVideoCheck ();

  Status = gBS->CreateEventEx (
                  EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  OnBeforeExitBootServices,
                  NULL,
                  &gEfiEventBeforeExitBootServicesGuid,
                  &mBeforeExitBootServicesEvent
                  );
  if (EFI_ERROR (Status)) {
    goto FreeFrameBuffer;
  }

  GopHandle = NULL;
  Status    = DisplayGopInstall (
                FrameBufferBase,
                FrameBufferSize,
                Timing,
                (mEdidSize != 0) ? mEdid : NULL,
                mEdidSize,
                &GopHandle
                );
  if (EFI_ERROR (Status)) {
    gBS->CloseEvent (mBeforeExitBootServicesEvent);
    goto FreeFrameBuffer;
  }

  DEBUG ((DEBUG_INFO, "%a: HDMI output running\n", __func__));

  return EFI_SUCCESS;

FreeFrameBuffer:
  DisplayStop ();
  gBS->FreePages (FrameBufferBase, EFI_SIZE_TO_PAGES (FrameBufferSize));

Fail:
  DisplayStop ();
  DEBUG ((DEBUG_ERROR, "%a: no HDMI output: %r\n", __func__, Status));

  return Status;
}
