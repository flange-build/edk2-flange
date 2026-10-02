/** @file
  LT9611 part of the MDSS display driver: TLMM pin access (Tlmm.c) and the
  bit-banged I2C master (GpioI2c.c) that Lt9611.c talks to the bridge with.

  Copyright (c) 2026, edk2-flange contributors.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#ifndef LT9611_INTERNAL_H_
#define LT9611_INTERNAL_H_

#include "MdssDisplay.h"

//
// TLMM pin bias, as encoded in the GPIO_CFG pull field.
//
#define LT9611_TLMM_PULL_NONE  0
#define LT9611_TLMM_PULL_DOWN  1
#define LT9611_TLMM_PULL_UP    3

//
// TLMM function 0: the pin is a GPIO.
//
#define LT9611_TLMM_FUNC_GPIO  0

//
// The function field of a saved GPIO_CFG value.
//
#define LT9611_TLMM_CFG_FUNCTION(Cfg)  (((Cfg) >> 2) & 0xF)

//
// A pin's configuration as found, to put it back later.
//
typedef struct {
  UINT16    Gpio;
  UINT32    Cfg;
  UINT32    InOut;
} LT9611_TLMM_PIN_STATE;

//
// TLMM (Tlmm.c)
//

/**
  Returns whether a GPIO exists on the QCS6490 TLMM.

  @param[in]  Gpio  The GPIO number.

  @retval TRUE   It exists.
  @retval FALSE  It does not; its registers must not be touched.
**/
BOOLEAN
Lt9611TlmmIsValidGpio (
  IN UINT16  Gpio
  );

/**
  Reads a pin's configuration and output registers.

  @param[in]   Gpio   The GPIO number.
  @param[out]  State  Where to store them.
**/
VOID
Lt9611TlmmSavePin (
  IN  UINT16                 Gpio,
  OUT LT9611_TLMM_PIN_STATE  *State
  );

/**
  Writes back a pin's configuration and output registers saved by
  Lt9611TlmmSavePin.

  @param[in]  State  The saved state.
**/
VOID
Lt9611TlmmRestorePin (
  IN CONST LT9611_TLMM_PIN_STATE  *State
  );

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
  );

/**
  Sets the value a GPIO drives when its output is enabled.

  @param[in]  Gpio  The GPIO number.
  @param[in]  High  TRUE for high.
**/
VOID
Lt9611TlmmSetOutput (
  IN UINT16   Gpio,
  IN BOOLEAN  High
  );

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
  );

/**
  Returns the level on a pin.

  @param[in]  Gpio  The GPIO number.

  @retval TRUE   High.
  @retval FALSE  Low.
**/
BOOLEAN
Lt9611TlmmGetInput (
  IN UINT16  Gpio
  );

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
  );

/**
  Logs a pin's configuration, decoded.

  @param[in]  Name  What the pin is, for the log.
  @param[in]  Gpio  The GPIO number.
**/
VOID
Lt9611TlmmLogPin (
  IN CONST CHAR8  *Name,
  IN UINT16       Gpio
  );

//
// Bit-banged I2C master (GpioI2c.c)
//

/**
  Takes two GPIOs over as an open-drain I2C bus and clears the bus. The
  caller has saved the pins' configuration.

  @param[in]  SdaGpio  The data line.
  @param[in]  SclGpio  The clock line.

  @retval EFI_SUCCESS       The bus is idle, both lines high.
  @retval EFI_DEVICE_ERROR  A line is held low.
**/
EFI_STATUS
Lt9611I2cInit (
  IN UINT16  SdaGpio,
  IN UINT16  SclGpio
  );

/**
  Lets go of both lines (inputs with pull-up) and stops using them. The
  caller then restores the pins' configuration.
**/
VOID
Lt9611I2cDeinit (
  VOID
  );

/**
  Frees a bus a slave holds: nine clocks with the data line released, then
  a STOP.

  @retval EFI_SUCCESS       The bus is idle, both lines high.
  @retval EFI_DEVICE_ERROR  A line is still held low.
  @retval EFI_NOT_READY     The bus has not been set up.
**/
EFI_STATUS
Lt9611I2cBusClear (
  VOID
  );

/**
  Writes bytes to a slave: START, address + W, the bytes, STOP.

  @param[in]  Address  The 7-bit slave address.
  @param[in]  Data     The bytes.
  @param[in]  Length   Their number.

  @retval EFI_SUCCESS       Every byte was acknowledged.
  @retval EFI_NO_RESPONSE   The slave did not acknowledge its address.
  @retval EFI_DEVICE_ERROR  It did not acknowledge a data byte, or the bus
                            was not idle.
  @retval EFI_TIMEOUT       The slave stretched the clock for too long.
  @retval EFI_NOT_READY     The bus has not been set up.
**/
EFI_STATUS
Lt9611I2cWrite (
  IN UINT8        Address,
  IN CONST UINT8  *Data,
  IN UINTN        Length
  );

/**
  Writes bytes to a slave, then reads from it after a repeated START:
  START, address + W, the bytes, Sr, address + R, the reads, STOP.

  @param[in]   Address      The 7-bit slave address.
  @param[in]   WriteData    The bytes to write (typically a register).
  @param[in]   WriteLength  Their number.
  @param[out]  ReadData     Where to store what is read.
  @param[in]   ReadLength   The number of bytes to read, at least 1.

  @retval EFI_SUCCESS  Done.
  @retval Other        As for Lt9611I2cWrite.
**/
EFI_STATUS
Lt9611I2cWriteRead (
  IN  UINT8        Address,
  IN  CONST UINT8  *WriteData,
  IN  UINTN        WriteLength,
  OUT UINT8        *ReadData,
  IN  UINTN        ReadLength
  );

#endif // LT9611_INTERNAL_H_
