/** @file
  QCS6490 top-level mode multiplexer (TLMM) GPIO helpers: pin function,
  pull, drive strength, output enable and level.

  Register layout from the Linux sc7280 pin controller (Linux 7.0.2
  drivers/pinctrl/qcom/pinctrl-sc7280.c PINGROUP: ctl at 0x1000 * id, io at
  +4, pull_bit 0, mux_bit 2, drv_bit 6, oe_bit 9, egpio_present 11,
  egpio_enable 12; in_bit 0, out_bit 1). Writes follow pinctrl-msm.c: the
  configuration register is read, modified and written back, so that the
  bits this library does not own (bits 10 and up) keep their value, and an
  output GPIO gets its level before its output enable.

  The TLMM is always clocked, so its registers can be read at any time. Only
  the registers of the pins passed in are touched.

  Copyright (c) 2026, edk2-flange contributors.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Uefi.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/Qcs6490TlmmLib.h>

//
// The CFG fields this library sets.
//
#define TLMM_CFG_FIELDS_MASK  (TLMM_CFG_PULL_MASK | TLMM_CFG_FUNC_MASK | TLMM_CFG_DRV_MASK | TLMM_CFG_OE)

//
// eGPIO pins (GPIO 144 to 174 on sc7280) can be handed to the LPASS island
// TLMM, which then drives them instead: EGPIO_PRESENT reads 1 on such a pin,
// and EGPIO_ENABLE = 1 keeps it with this TLMM. pinctrl-msm.c
// msm_pinmux_set_mux sets EGPIO_ENABLE for every function but egpio
// (sc7280 egpio_func 9), which only clears it. This library never hands a
// pin over.
//
#define TLMM_CFG_EGPIO_PRESENT  BIT11
#define TLMM_CFG_EGPIO_ENABLE   BIT12
#define TLMM_FUNC_EGPIO         9

//
// Limits of the CFG fields.
//
#define TLMM_FUNC_MAX      15
#define TLMM_DRIVE_MA_MIN  2
#define TLMM_DRIVE_MA_MAX  16

STATIC CONST CHAR8  *mPullNames[] = { "no pull", "pull-down", "keeper", "pull-up" };

/**
  Checks a pin and the settings asked for it.

  @param[in]  Caller    The function asking, for the log.
  @param[in]  Gpio      The GPIO.
  @param[in]  Function  Its function.
  @param[in]  Pull      TLMM_PULL_*.
  @param[in]  DriveMa   Drive strength in mA.

  @retval EFI_SUCCESS            All exist.
  @retval EFI_INVALID_PARAMETER  One does not; it has been logged.
**/
STATIC
EFI_STATUS
TlmmCheck (
  IN CONST CHAR8  *Caller,
  IN UINTN        Gpio,
  IN UINT8        Function,
  IN UINT8        Pull,
  IN UINT8        DriveMa
  )
{
  if (Gpio >= TLMM_GPIO_COUNT) {
    DEBUG ((DEBUG_ERROR, "%a: GPIO%lu does not exist\n", Caller, (UINT64)Gpio));
    return EFI_INVALID_PARAMETER;
  }

  if (Function > TLMM_FUNC_MAX) {
    DEBUG ((DEBUG_ERROR, "%a: GPIO%u: no function %u\n", Caller, (UINT32)Gpio, Function));
    return EFI_INVALID_PARAMETER;
  }

  if (Pull > TLMM_PULL_UP) {
    DEBUG ((DEBUG_ERROR, "%a: GPIO%u: no pull setting %u\n", Caller, (UINT32)Gpio, Pull));
    return EFI_INVALID_PARAMETER;
  }

  if ((DriveMa < TLMM_DRIVE_MA_MIN) || (DriveMa > TLMM_DRIVE_MA_MAX) || ((DriveMa & 1) != 0)) {
    DEBUG ((DEBUG_ERROR, "%a: GPIO%u: no drive strength of %u mA\n", Caller, (UINT32)Gpio, DriveMa));
    return EFI_INVALID_PARAMETER;
  }

  if ((Function == TLMM_FUNC_EGPIO) && ((MmioRead32 (TLMM_GPIO_CFG (Gpio)) & TLMM_CFG_EGPIO_PRESENT) != 0)) {
    DEBUG ((DEBUG_ERROR, "%a: GPIO%u: handing a pin to the LPASS TLMM (egpio) is not supported\n", Caller, (UINT32)Gpio));
    return EFI_INVALID_PARAMETER;
  }

  return EFI_SUCCESS;
}

/**
  Writes a pin's function, pull, drive strength and output enable, keeping
  its other CFG bits and claiming an eGPIO pin for this TLMM, as
  pinctrl-msm.c msm_pinmux_set_mux and msm_config_group_set do. The
  arguments must have been checked.

  @param[in]  Gpio          The GPIO.
  @param[in]  Function      Its function.
  @param[in]  Pull          TLMM_PULL_*.
  @param[in]  DriveMa       Drive strength in mA.
  @param[in]  OutputEnable  Whether the pin drives its output level.

  @return  The CFG value before the write.
**/
STATIC
UINT32
TlmmWriteCfg (
  IN UINTN    Gpio,
  IN UINT8    Function,
  IN UINT8    Pull,
  IN UINT8    DriveMa,
  IN BOOLEAN  OutputEnable
  )
{
  UINT32  Old;
  UINT32  Cfg;

  Old  = MmioRead32 (TLMM_GPIO_CFG (Gpio));
  Cfg  = Old & ~(UINT32)TLMM_CFG_FIELDS_MASK;
  Cfg |= (UINT32)Pull & TLMM_CFG_PULL_MASK;
  Cfg |= ((UINT32)Function << TLMM_CFG_FUNC_SHIFT) & TLMM_CFG_FUNC_MASK;

  //
  // The drive field holds mA / 2 - 1 (msm_regval_to_drive).
  //
  Cfg |= (((UINT32)DriveMa / 2 - 1) << TLMM_CFG_DRV_SHIFT) & TLMM_CFG_DRV_MASK;
  if (OutputEnable) {
    Cfg |= TLMM_CFG_OE;
  }

  if ((Cfg & TLMM_CFG_EGPIO_PRESENT) != 0) {
    Cfg |= TLMM_CFG_EGPIO_ENABLE;
  }

  MmioWrite32 (TLMM_GPIO_CFG (Gpio), Cfg);

  return Old;
}

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
  )
{
  UINT32      Old;
  EFI_STATUS  Status;

  Status = TlmmCheck (__func__, Gpio, Function, Pull, DriveMa);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Old = TlmmWriteCfg (Gpio, Function, Pull, DriveMa, OutputEnable);

  DEBUG ((
    DEBUG_INFO,
    "%a: GPIO%u func%u, %a, %u mA, %a: CFG 0x%x -> 0x%x, IO 0x%x\n",
    __func__,
    (UINT32)Gpio,
    Function,
    mPullNames[Pull],
    DriveMa,
    OutputEnable ? "output" : "input",
    Old,
    MmioRead32 (TLMM_GPIO_CFG (Gpio)),
    MmioRead32 (TLMM_GPIO_IO (Gpio))
    ));

  return EFI_SUCCESS;
}

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
  )
{
  UINT32      OldCfg;
  UINT32      OldIo;
  EFI_STATUS  Status;

  Status = TlmmCheck (__func__, Gpio, TLMM_FUNC_GPIO, TLMM_PULL_NONE, DriveMa);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // The level first (msm_gpio_direction_output), so that the pin never
  // drives the other level, whatever it did before. IN is read-only.
  //
  OldIo = MmioRead32 (TLMM_GPIO_IO (Gpio));
  MmioWrite32 (TLMM_GPIO_IO (Gpio), High ? TLMM_IO_OUT : 0);
  OldCfg = TlmmWriteCfg (Gpio, TLMM_FUNC_GPIO, TLMM_PULL_NONE, DriveMa, TRUE);

  DEBUG ((
    DEBUG_INFO,
    "%a: GPIO%u %a, %u mA: CFG 0x%x -> 0x%x, IO 0x%x -> 0x%x\n",
    __func__,
    (UINT32)Gpio,
    High ? "high" : "low",
    DriveMa,
    OldCfg,
    MmioRead32 (TLMM_GPIO_CFG (Gpio)),
    OldIo,
    MmioRead32 (TLMM_GPIO_IO (Gpio))
    ));

  return EFI_SUCCESS;
}

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
  )
{
  UINT32  OldIo;

  if (Gpio >= TLMM_GPIO_COUNT) {
    DEBUG ((DEBUG_ERROR, "%a: GPIO%lu does not exist\n", __func__, (UINT64)Gpio));
    return EFI_INVALID_PARAMETER;
  }

  OldIo = MmioRead32 (TLMM_GPIO_IO (Gpio));
  MmioWrite32 (TLMM_GPIO_IO (Gpio), High ? TLMM_IO_OUT : 0);

  DEBUG ((
    DEBUG_INFO,
    "%a: GPIO%u %a: IO 0x%x -> 0x%x, CFG 0x%x\n",
    __func__,
    (UINT32)Gpio,
    High ? "high" : "low",
    OldIo,
    MmioRead32 (TLMM_GPIO_IO (Gpio)),
    MmioRead32 (TLMM_GPIO_CFG (Gpio))
    ));

  return EFI_SUCCESS;
}

/**
  Reads the level on a pin.

  @param[in]  Gpio  The GPIO.

  @return  TRUE if it is high; FALSE if it is low or there is no such GPIO.
**/
BOOLEAN
EFIAPI
TlmmGetInput (
  IN UINTN  Gpio
  )
{
  if (Gpio >= TLMM_GPIO_COUNT) {
    DEBUG ((DEBUG_ERROR, "%a: GPIO%lu does not exist\n", __func__, (UINT64)Gpio));
    return FALSE;
  }

  return (MmioRead32 (TLMM_GPIO_IO (Gpio)) & TLMM_IO_IN) != 0;
}

/**
  Returns whether a pin is a GPIO that drives its output high.

  @param[in]  Gpio  The GPIO.

  @return  TRUE if it is; FALSE otherwise or if there is no such GPIO.
**/
BOOLEAN
EFIAPI
TlmmIsDrivenHigh (
  IN UINTN  Gpio
  )
{
  UINT32  Cfg;
  UINT32  Io;

  if (Gpio >= TLMM_GPIO_COUNT) {
    DEBUG ((DEBUG_ERROR, "%a: GPIO%lu does not exist\n", __func__, (UINT64)Gpio));
    return FALSE;
  }

  Cfg = MmioRead32 (TLMM_GPIO_CFG (Gpio));
  Io  = MmioRead32 (TLMM_GPIO_IO (Gpio));

  //
  // An eGPIO pin handed to the LPASS TLMM does not follow this TLMM's
  // settings.
  //
  if (((Cfg & TLMM_CFG_EGPIO_PRESENT) != 0) && ((Cfg & TLMM_CFG_EGPIO_ENABLE) == 0)) {
    return FALSE;
  }

  return ((Cfg & TLMM_CFG_FUNC_MASK) == (TLMM_FUNC_GPIO << TLMM_CFG_FUNC_SHIFT)) &&
         ((Cfg & TLMM_CFG_OE) != 0) &&
         ((Io & TLMM_IO_OUT) != 0);
}
