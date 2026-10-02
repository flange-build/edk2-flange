/** @file
  QCS6490 TLMM pin access for the LT9611 part of the display driver: the
  bridge's power and reset GPIOs, and its I2C pins while they are bit-banged.

  The TLMM is always clocked, so its registers can be read at any time. Only
  the registers of the pins passed in are touched.

  Register layout from the Linux sc7280 pin controller
  (drivers/pinctrl/qcom/pinctrl-sc7280.c, pinctrl-msm.c).

  Copyright (c) 2026, edk2-flange contributors.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include "Lt9611Internal.h"

//
// tlmm in sc7280.dtsi. GPIO 0..174; 175 is UFS_RESET, which has a different
// layout and is not a GPIO.
//
#define TLMM_BASE       0x0F100000
#define TLMM_NUM_GPIOS  175

#define TLMM_GPIO_CFG(n)     (TLMM_BASE + 0x1000 * (UINTN)(n))
#define TLMM_GPIO_IN_OUT(n)  (TLMM_BASE + 0x1000 * (UINTN)(n) + 0x4)

//
// GPIO_CFG fields (pinctrl-sc7280.c PINGROUP: pull_bit 0, mux_bit 2,
// drv_bit 6, oe_bit 9). The bits above 9 (egpio) are left as they are.
//
#define TLMM_GPIO_CFG_PULL_MASK    0x3
#define TLMM_GPIO_CFG_FUNC_SHIFT   2
#define TLMM_GPIO_CFG_FUNC_MASK    (0xF << TLMM_GPIO_CFG_FUNC_SHIFT)
#define TLMM_GPIO_CFG_DRV_SHIFT    6
#define TLMM_GPIO_CFG_DRV_MASK     (0x7 << TLMM_GPIO_CFG_DRV_SHIFT)
#define TLMM_GPIO_CFG_OE           BIT9
#define TLMM_GPIO_CFG_FIELDS_MASK  (TLMM_GPIO_CFG_PULL_MASK | TLMM_GPIO_CFG_FUNC_MASK | \
                                    TLMM_GPIO_CFG_DRV_MASK | TLMM_GPIO_CFG_OE)

//
// GPIO_IN_OUT: in_bit 0 (pad level), out_bit 1 (value driven when OE).
//
#define TLMM_GPIO_IN_OUT_IN   BIT0
#define TLMM_GPIO_IN_OUT_OUT  BIT1

//
// How long Lt9611TlmmWaitHigh checks a pin itself before it hands over to
// MmioPoll32.
//
#define TLMM_WAIT_HIGH_SPIN_US  10

STATIC CONST CHAR8  *mPullNames[] = { "no pull", "pull-down", "keeper", "pull-up" };

/**
  Returns whether a GPIO exists on the QCS6490 TLMM.

  @param[in]  Gpio  The GPIO number.

  @retval TRUE   It exists.
  @retval FALSE  It does not; its registers must not be touched.
**/
BOOLEAN
Lt9611TlmmIsValidGpio (
  IN UINT16  Gpio
  )
{
  return Gpio < TLMM_NUM_GPIOS;
}

/**
  Reads a pin's configuration and output registers.

  @param[in]   Gpio   The GPIO number.
  @param[out]  State  Where to store them.
**/
VOID
Lt9611TlmmSavePin (
  IN  UINT16                 Gpio,
  OUT LT9611_TLMM_PIN_STATE  *State
  )
{
  ASSERT (Lt9611TlmmIsValidGpio (Gpio));

  State->Gpio  = Gpio;
  State->Cfg   = MmioRead32 (TLMM_GPIO_CFG (Gpio));
  State->InOut = MmioRead32 (TLMM_GPIO_IN_OUT (Gpio));
}

/**
  Writes back a pin's configuration and output registers saved by
  Lt9611TlmmSavePin.

  @param[in]  State  The saved state.
**/
VOID
Lt9611TlmmRestorePin (
  IN CONST LT9611_TLMM_PIN_STATE  *State
  )
{
  ASSERT (Lt9611TlmmIsValidGpio (State->Gpio));

  //
  // The output value first, so that a pin that goes back to being a driven
  // GPIO does not glitch. The input bit is read-only.
  //
  MmioWrite32 (TLMM_GPIO_IN_OUT (State->Gpio), State->InOut & TLMM_GPIO_IN_OUT_OUT);
  MmioWrite32 (TLMM_GPIO_CFG (State->Gpio), State->Cfg);
}

/**
  Sets a pin's function, bias, drive strength and output enable.

  @param[in]  Gpio          The GPIO number.
  @param[in]  Function      The TLMM function, 0 for GPIO.
  @param[in]  Pull          LT9611_TLMM_PULL_*.
  @param[in]  DriveMa       Drive strength in mA: 2, 4, ... 16.
  @param[in]  OutputEnable  TRUE to drive the output value.
**/
VOID
Lt9611TlmmConfigure (
  IN UINT16   Gpio,
  IN UINT32   Function,
  IN UINT32   Pull,
  IN UINT32   DriveMa,
  IN BOOLEAN  OutputEnable
  )
{
  UINT32  Cfg;
  UINT32  Drive;

  ASSERT (Lt9611TlmmIsValidGpio (Gpio));
  ASSERT ((DriveMa >= 2) && (DriveMa <= 16) && ((DriveMa & 1) == 0));

  //
  // The drive field holds mA / 2 - 1.
  //
  Drive = (DriveMa / 2) - 1;

  Cfg  = MmioRead32 (TLMM_GPIO_CFG (Gpio)) & ~(UINT32)TLMM_GPIO_CFG_FIELDS_MASK;
  Cfg |= Pull & TLMM_GPIO_CFG_PULL_MASK;
  Cfg |= (Function << TLMM_GPIO_CFG_FUNC_SHIFT) & TLMM_GPIO_CFG_FUNC_MASK;
  Cfg |= (Drive << TLMM_GPIO_CFG_DRV_SHIFT) & TLMM_GPIO_CFG_DRV_MASK;
  if (OutputEnable) {
    Cfg |= TLMM_GPIO_CFG_OE;
  }

  MmioWrite32 (TLMM_GPIO_CFG (Gpio), Cfg);
}

/**
  Sets the value a GPIO drives when its output is enabled.

  @param[in]  Gpio  The GPIO number.
  @param[in]  High  TRUE for high.
**/
VOID
Lt9611TlmmSetOutput (
  IN UINT16   Gpio,
  IN BOOLEAN  High
  )
{
  ASSERT (Lt9611TlmmIsValidGpio (Gpio));

  MmioWrite32 (TLMM_GPIO_IN_OUT (Gpio), High ? TLMM_GPIO_IN_OUT_OUT : 0);
}

/**
  Turns a GPIO's output driver on or off, leaving the rest of its
  configuration alone.

  @param[in]  Gpio    The GPIO number.
  @param[in]  Enable  TRUE to drive the output value.
**/
VOID
Lt9611TlmmSetOutputEnable (
  IN UINT16   Gpio,
  IN BOOLEAN  Enable
  )
{
  ASSERT (Lt9611TlmmIsValidGpio (Gpio));

  if (Enable) {
    MmioOr32 (TLMM_GPIO_CFG (Gpio), TLMM_GPIO_CFG_OE);
  } else {
    MmioAnd32 (TLMM_GPIO_CFG (Gpio), ~(UINT32)TLMM_GPIO_CFG_OE);
  }
}

/**
  Returns the level on a pin.

  @param[in]  Gpio  The GPIO number.

  @retval TRUE   High.
  @retval FALSE  Low.
**/
BOOLEAN
Lt9611TlmmGetInput (
  IN UINT16  Gpio
  )
{
  ASSERT (Lt9611TlmmIsValidGpio (Gpio));

  return (MmioRead32 (TLMM_GPIO_IN_OUT (Gpio)) & TLMM_GPIO_IN_OUT_IN) != 0;
}

/**
  Waits for a pin to go high.

  @param[in]  Gpio       The GPIO number.
  @param[in]  TimeoutUs  How long to wait, in microseconds.

  @retval EFI_SUCCESS  The pin is high.
  @retval EFI_TIMEOUT  It stayed low.
**/
EFI_STATUS
Lt9611TlmmWaitHigh (
  IN UINT16  Gpio,
  IN UINTN   TimeoutUs
  )
{
  UINTN  Waited;

  ASSERT (Lt9611TlmmIsValidGpio (Gpio));

  //
  // Nearly always the pin is high at once, or after a rise time of a
  // microsecond or two through the pull-ups. Check that in 1 us steps, so
  // that the I2C clock does not slow down to the poll helper's pace; leave
  // longer waits (a slave stretching the clock) to the helper.
  //
  for (Waited = 0; Waited < MIN (TimeoutUs, TLMM_WAIT_HIGH_SPIN_US); Waited++) {
    if (Lt9611TlmmGetInput (Gpio)) {
      return EFI_SUCCESS;
    }

    MicroSecondDelay (1);
  }

  return MmioPoll32 (TLMM_GPIO_IN_OUT (Gpio), TLMM_GPIO_IN_OUT_IN, TLMM_GPIO_IN_OUT_IN, TimeoutUs - Waited);
}

/**
  Logs a pin's configuration, decoded.

  @param[in]  Name  What the pin is, for the log.
  @param[in]  Gpio  The GPIO number.
**/
VOID
Lt9611TlmmLogPin (
  IN CONST CHAR8  *Name,
  IN UINT16       Gpio
  )
{
  UINT32  Cfg;
  UINT32  InOut;

  if (!Lt9611TlmmIsValidGpio (Gpio)) {
    DEBUG ((DEBUG_ERROR, "%a: %a: GPIO%u does not exist\n", __func__, Name, Gpio));
    return;
  }

  Cfg   = MmioRead32 (TLMM_GPIO_CFG (Gpio));
  InOut = MmioRead32 (TLMM_GPIO_IN_OUT (Gpio));

  DEBUG ((
    DEBUG_INFO,
    "%a: %a GPIO%u: CFG 0x%x IO 0x%x (func%u, %a, %umA, %a, in %u, out %u)\n",
    __func__,
    Name,
    Gpio,
    Cfg,
    InOut,
    (Cfg & TLMM_GPIO_CFG_FUNC_MASK) >> TLMM_GPIO_CFG_FUNC_SHIFT,
    mPullNames[Cfg & TLMM_GPIO_CFG_PULL_MASK],
    (((Cfg & TLMM_GPIO_CFG_DRV_MASK) >> TLMM_GPIO_CFG_DRV_SHIFT) + 1) * 2,
    ((Cfg & TLMM_GPIO_CFG_OE) != 0) ? "output" : "input",
    InOut & TLMM_GPIO_IN_OUT_IN,
    (InOut & TLMM_GPIO_IN_OUT_OUT) >> 1
    ));
}
