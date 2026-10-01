/** @file
  QCS6490 top-level mode multiplexer (TLMM) GPIO helpers
  (pinctrl-sc7280.c, pinctrl-msm.c): one CFG and one IN_OUT register per
  GPIO.

  Copyright (c) 2026, edk2-flange contributors.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#ifndef QCS6490_TLMM_LIB_H_
#define QCS6490_TLMM_LIB_H_

#include <Uefi/UefiBaseType.h>

#define TLMM_BASE        0x0F100000
#define TLMM_GPIO_COUNT  175

#define TLMM_GPIO_CFG(n)  (TLMM_BASE + (UINTN)(n) * 0x1000)
#define TLMM_GPIO_IO(n)   (TLMM_BASE + (UINTN)(n) * 0x1000 + 0x4)

//
// CFG: pull in bits 1:0, function in bits 5:2, drive strength in bits 8:6
// (mA / 2 - 1), output enable in bit 9. IN_OUT: input level in bit 0,
// output level in bit 1.
//
#define TLMM_CFG_PULL_MASK     0x3
#define TLMM_CFG_FUNC_SHIFT    2
#define TLMM_CFG_FUNC_MASK     (0xF << TLMM_CFG_FUNC_SHIFT)
#define TLMM_CFG_DRV_SHIFT     6
#define TLMM_CFG_DRV_MASK      (0x7 << TLMM_CFG_DRV_SHIFT)
#define TLMM_CFG_OE            BIT9
#define TLMM_IO_IN             BIT0
#define TLMM_IO_OUT            BIT1

#define TLMM_PULL_NONE    0
#define TLMM_PULL_DOWN    1
#define TLMM_PULL_KEEPER  2
#define TLMM_PULL_UP      3

#define TLMM_FUNC_GPIO  0

/**
  Configures a pin: its function, pull, drive strength and output enable.
  The output level is not changed.

  @param[in]  Gpio          The GPIO.
  @param[in]  Function      Its function, TLMM_FUNC_GPIO for a GPIO.
  @param[in]  Pull          TLMM_PULL_*.
  @param[in]  DriveMa       Drive strength in mA: 2 to 16, even.
  @param[in]  OutputEnable  Whether the pin drives its output level.

  @retval EFI_SUCCESS            Configured.
  @retval EFI_INVALID_PARAMETER  No such GPIO, function, pull or strength.
**/
EFI_STATUS
EFIAPI
TlmmConfigure (
  IN UINTN    Gpio,
  IN UINT8    Function,
  IN UINT8    Pull,
  IN UINT8    DriveMa,
  IN BOOLEAN  OutputEnable
  );

/**
  Drives a pin as a GPIO output: the level first, then GPIO function, no
  pull, the drive strength and output enable, as pinctrl-msm does for an
  output GPIO.

  @param[in]  Gpio     The GPIO.
  @param[in]  High     The level.
  @param[in]  DriveMa  Drive strength in mA: 2 to 16, even.

  @retval EFI_SUCCESS            Driven.
  @retval EFI_INVALID_PARAMETER  No such GPIO or strength.
**/
EFI_STATUS
EFIAPI
TlmmDriveOutput (
  IN UINTN    Gpio,
  IN BOOLEAN  High,
  IN UINT8    DriveMa
  );

/**
  Sets the output level of a pin, leaving its configuration alone.

  @param[in]  Gpio  The GPIO.
  @param[in]  High  The level.

  @retval EFI_SUCCESS            Set.
  @retval EFI_INVALID_PARAMETER  No such GPIO.
**/
EFI_STATUS
EFIAPI
TlmmSetOutput (
  IN UINTN    Gpio,
  IN BOOLEAN  High
  );

/**
  Reads the level on a pin.

  @param[in]  Gpio  The GPIO.

  @return  TRUE if it is high; FALSE if it is low or there is no such GPIO.
**/
BOOLEAN
EFIAPI
TlmmGetInput (
  IN UINTN  Gpio
  );

/**
  Returns whether a pin is a GPIO that drives its output high.

  @param[in]  Gpio  The GPIO.

  @return  TRUE if it is; FALSE otherwise or if there is no such GPIO.
**/
BOOLEAN
EFIAPI
TlmmIsDrivenHigh (
  IN UINTN  Gpio
  );

#endif // QCS6490_TLMM_LIB_H_
