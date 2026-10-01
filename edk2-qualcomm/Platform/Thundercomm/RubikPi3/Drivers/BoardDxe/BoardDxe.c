/** @file
  RUBIK Pi 3 board setup.

  The LT9611 HDMI bridge has two control lines besides reset and its 3.3 V
  supply: "enable" on GPIO40 and "ocb" on GPIO41 (see rubikpi3-display.dtsi in
  the vendor kernel). The device tree the vendor kernel boots with leaves them
  out, so Linux relies on the boot firmware having driven them high, as the
  stock Qualcomm UEFI does. Do the same.

  Copyright (c) 2026, edk2-flange contributors.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Uefi.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>

#define TLMM_BASE          0x0F100000
#define TLMM_GPIO_CFG(n)   (TLMM_BASE + (n) * 0x1000)
#define TLMM_GPIO_IO(n)    (TLMM_BASE + (n) * 0x1000 + 0x4)

#define TLMM_CFG_OE        BIT9
#define TLMM_IO_OUT        BIT1

#define LT9611_ENABLE_GPIO  40
#define LT9611_OCB_GPIO     41

/**
  Drives a GPIO as a plain output.

  @param[in]  Gpio   The GPIO.
  @param[in]  High   The level.
**/
STATIC
VOID
GpioSetOutput (
  IN UINTN    Gpio,
  IN BOOLEAN  High
  )
{
  //
  // Level first, then GPIO function, no pull, 2 mA, output enabled.
  //
  MmioWrite32 (TLMM_GPIO_IO (Gpio), High ? TLMM_IO_OUT : 0);
  MmioWrite32 (TLMM_GPIO_CFG (Gpio), TLMM_CFG_OE);
}

/**
  Entry point.

  @param[in]  ImageHandle  The image handle.
  @param[in]  SystemTable  The system table.

  @retval EFI_SUCCESS  Always.
**/
EFI_STATUS
EFIAPI
BoardDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  GpioSetOutput (LT9611_ENABLE_GPIO, TRUE);
  GpioSetOutput (LT9611_OCB_GPIO, TRUE);

  DEBUG ((DEBUG_INFO, "RubikPi3: LT9611 enable/ocb (GPIO%u/%u) high\n", LT9611_ENABLE_GPIO, LT9611_OCB_GPIO));

  return EFI_SUCCESS;
}
