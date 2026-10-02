/** @file
  Bit-banged I2C master on two TLMM GPIOs, for talking to the LT9611.

  The LT9611 sits on I2C9 (QUP1 SE1). Driving that serial engine needs its
  protocol firmware, which lives on UFS and is only loaded once UFS is up,
  after the display must already work. Bit-banging needs no firmware, no QUP
  clocks and no DMA, and the traffic is small (a couple of hundred register
  writes and the EDID).

  The pins emulate open drain: a line is released by making the pin an input
  with its internal pull-up (the external pull-ups sit on the LT9611 supply,
  so the bridge is powered first), and pulled low by enabling the output
  driver, whose output value stays 0. Standard mode, about 100 kHz, with the
  I2C-bus specification timings rounded up to whole microseconds, and a
  bounded wait whenever a slave stretches the clock.

  Copyright (c) 2026, edk2-flange contributors.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include "Lt9611Internal.h"

//
// Half a clock period. Standard mode needs tLOW >= 4.7 us and tHIGH >= 4.0 us;
// with the register accesses on top, the clock runs a little under 100 kHz.
//
#define I2C_HALF_PERIOD_US  5

//
// Time the data line is held after the clock falls before it changes. The
// specification asks for none from the master; this is margin for the slow
// rise and fall with the weak pull-ups and a possible level shifter.
//
#define I2C_DATA_HOLD_US  1

//
// How long a slave may hold the clock low. The stock firmware allows 500 us
// on this bus.
//
#define I2C_STRETCH_TIMEOUT_US  1000

//
// Clocks for a bus clear: enough for a slave to finish any byte and its
// acknowledge bit.
//
#define I2C_BUS_CLEAR_CLOCKS  9

#define I2C_READ   1
#define I2C_WRITE  0

STATIC BOOLEAN  mI2cReady;
STATIC UINT16   mSda;
STATIC UINT16   mScl;

/**
  Releases the data line: the pull-ups take it high unless a slave holds it.
**/
STATIC
VOID
SdaRelease (
  VOID
  )
{
  Lt9611TlmmSetOutputEnable (mSda, FALSE);
}

/**
  Pulls the data line low.
**/
STATIC
VOID
SdaLow (
  VOID
  )
{
  Lt9611TlmmSetOutputEnable (mSda, TRUE);
}

/**
  Pulls the clock line low.
**/
STATIC
VOID
SclLow (
  VOID
  )
{
  Lt9611TlmmSetOutputEnable (mScl, TRUE);
}

/**
  Releases the clock line and waits for it to go high, which a slave
  stretching the clock delays.

  @retval EFI_SUCCESS  The clock is high.
  @retval EFI_TIMEOUT  It stayed low.
**/
STATIC
EFI_STATUS
SclRelease (
  VOID
  )
{
  EFI_STATUS  Status;

  Lt9611TlmmSetOutputEnable (mScl, FALSE);
  Status = Lt9611TlmmWaitHigh (mScl, I2C_STRETCH_TIMEOUT_US);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: SCL (GPIO%u) held low for over %u us\n", __func__, mScl, I2C_STRETCH_TIMEOUT_US));
  }

  return Status;
}

/**
  Sends a START from an idle bus: data falls while the clock is high.

  @retval EFI_SUCCESS       Started; the clock is low.
  @retval EFI_DEVICE_ERROR  The bus is not idle.
**/
STATIC
EFI_STATUS
I2cStart (
  VOID
  )
{
  if (!Lt9611TlmmGetInput (mSda) || !Lt9611TlmmGetInput (mScl)) {
    DEBUG ((
      DEBUG_ERROR,
      "%a: bus not idle (SDA %u, SCL %u)\n",
      __func__,
      Lt9611TlmmGetInput (mSda),
      Lt9611TlmmGetInput (mScl)
      ));
    return EFI_DEVICE_ERROR;
  }

  SdaLow ();
  MicroSecondDelay (I2C_HALF_PERIOD_US);  // tHD;STA >= 4.0 us
  SclLow ();
  MicroSecondDelay (I2C_DATA_HOLD_US);
  return EFI_SUCCESS;
}

/**
  Sends a repeated START; the clock is low on entry.

  @retval EFI_SUCCESS  Started; the clock is low.
  @retval EFI_TIMEOUT  A slave held the clock low.
**/
STATIC
EFI_STATUS
I2cRepeatedStart (
  VOID
  )
{
  EFI_STATUS  Status;

  SdaRelease ();
  MicroSecondDelay (I2C_HALF_PERIOD_US);
  Status = SclRelease ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  MicroSecondDelay (I2C_HALF_PERIOD_US);  // tSU;STA >= 4.7 us
  SdaLow ();
  MicroSecondDelay (I2C_HALF_PERIOD_US);  // tHD;STA >= 4.0 us
  SclLow ();
  MicroSecondDelay (I2C_DATA_HOLD_US);
  return EFI_SUCCESS;
}

/**
  Sends a STOP: data rises while the clock is high. The clock is low on
  entry. Also used to end a failed transfer, so it goes on when the clock
  does not rise and reports it.

  @retval EFI_SUCCESS       Both lines are high.
  @retval EFI_DEVICE_ERROR  A line is held low.
**/
STATIC
EFI_STATUS
I2cStop (
  VOID
  )
{
  SdaLow ();
  MicroSecondDelay (I2C_HALF_PERIOD_US);
  SclRelease ();
  MicroSecondDelay (I2C_HALF_PERIOD_US);  // tSU;STO >= 4.0 us
  SdaRelease ();
  MicroSecondDelay (I2C_HALF_PERIOD_US);  // tBUF >= 4.7 us before the next START

  if (!Lt9611TlmmGetInput (mSda) || !Lt9611TlmmGetInput (mScl)) {
    return EFI_DEVICE_ERROR;
  }

  return EFI_SUCCESS;
}

/**
  Clocks one bit out; the clock is low on entry and on return.

  @param[in]  Bit  The bit.

  @retval EFI_SUCCESS  Sent.
  @retval EFI_TIMEOUT  A slave held the clock low.
**/
STATIC
EFI_STATUS
I2cWriteBit (
  IN BOOLEAN  Bit
  )
{
  EFI_STATUS  Status;

  if (Bit) {
    SdaRelease ();
  } else {
    SdaLow ();
  }

  MicroSecondDelay (I2C_HALF_PERIOD_US);  // tLOW, and tSU;DAT
  Status = SclRelease ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  MicroSecondDelay (I2C_HALF_PERIOD_US);  // tHIGH
  SclLow ();
  MicroSecondDelay (I2C_DATA_HOLD_US);
  return EFI_SUCCESS;
}

/**
  Clocks one bit in; the clock is low on entry and on return.

  @param[out]  Bit  The bit.

  @retval EFI_SUCCESS  Received.
  @retval EFI_TIMEOUT  A slave held the clock low.
**/
STATIC
EFI_STATUS
I2cReadBit (
  OUT BOOLEAN  *Bit
  )
{
  EFI_STATUS  Status;

  SdaRelease ();
  MicroSecondDelay (I2C_HALF_PERIOD_US);
  Status = SclRelease ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // Sample at the end of the high phase, when the data line has had the
  // longest time to rise through the weak pull-ups.
  //
  MicroSecondDelay (I2C_HALF_PERIOD_US);
  *Bit = Lt9611TlmmGetInput (mSda);
  SclLow ();
  MicroSecondDelay (I2C_DATA_HOLD_US);
  return EFI_SUCCESS;
}

/**
  Sends a byte, MSB first, and reads the acknowledge bit.

  @param[in]   Byte  The byte.
  @param[out]  Ack   TRUE when the slave acknowledged it.

  @retval EFI_SUCCESS  Sent.
  @retval EFI_TIMEOUT  A slave held the clock low.
**/
STATIC
EFI_STATUS
I2cWriteByte (
  IN  UINT8    Byte,
  OUT BOOLEAN  *Ack
  )
{
  EFI_STATUS  Status;
  UINTN       Index;
  BOOLEAN     Nack;

  for (Index = 0; Index < 8; Index++) {
    Status = I2cWriteBit ((Byte & (0x80 >> Index)) != 0);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  Status = I2cReadBit (&Nack);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  *Ack = !Nack;
  return EFI_SUCCESS;
}

/**
  Receives a byte, MSB first, and sends the acknowledge bit.

  @param[out]  Byte  The byte.
  @param[in]   Ack   TRUE to acknowledge (more bytes to come), FALSE after
                     the last byte.

  @retval EFI_SUCCESS  Received.
  @retval EFI_TIMEOUT  A slave held the clock low.
**/
STATIC
EFI_STATUS
I2cReadByte (
  OUT UINT8    *Byte,
  IN  BOOLEAN  Ack
  )
{
  EFI_STATUS  Status;
  UINTN       Index;
  BOOLEAN     Bit;
  UINT8       Value;

  Value = 0;
  for (Index = 0; Index < 8; Index++) {
    Status = I2cReadBit (&Bit);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    Value = (UINT8)((Value << 1) | (Bit ? 1 : 0));
  }

  //
  // Acknowledge = data low.
  //
  Status = I2cWriteBit (!Ack);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  *Byte = Value;
  return EFI_SUCCESS;
}

/**
  Sends the address byte and the bytes to write, after a START or a repeated
  START.

  @param[in]  Address    The 7-bit slave address.
  @param[in]  Direction  I2C_WRITE or I2C_READ.
  @param[in]  Data       The bytes to write, for I2C_WRITE.
  @param[in]  Length     Their number.

  @retval EFI_SUCCESS       All acknowledged.
  @retval EFI_NO_RESPONSE   The address was not acknowledged.
  @retval EFI_DEVICE_ERROR  A data byte was not acknowledged.
  @retval EFI_TIMEOUT       A slave held the clock low.
**/
STATIC
EFI_STATUS
I2cSendAddressAndData (
  IN UINT8        Address,
  IN UINT8        Direction,
  IN CONST UINT8  *Data,
  IN UINTN        Length
  )
{
  EFI_STATUS  Status;
  BOOLEAN     Ack;
  UINTN       Index;

  Status = I2cWriteByte ((UINT8)((Address << 1) | Direction), &Ack);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (!Ack) {
    return EFI_NO_RESPONSE;
  }

  for (Index = 0; Index < Length; Index++) {
    Status = I2cWriteByte (Data[Index], &Ack);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    if (!Ack) {
      DEBUG ((DEBUG_ERROR, "%a: 0x%02x: byte %u (0x%02x) not acknowledged\n", __func__, Address, (UINT32)Index, Data[Index]));
      return EFI_DEVICE_ERROR;
    }
  }

  return EFI_SUCCESS;
}

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
  )
{
  ASSERT (Lt9611TlmmIsValidGpio (SdaGpio) && Lt9611TlmmIsValidGpio (SclGpio));
  ASSERT (SdaGpio != SclGpio);

  mSda = SdaGpio;
  mScl = SclGpio;

  //
  // Output value 0 first, while the pins still have their old function and
  // the value does not matter, so that enabling the output later can only
  // ever pull low. Then GPIO function, input, internal pull-up: both lines
  // released.
  //
  Lt9611TlmmSetOutput (mSda, FALSE);
  Lt9611TlmmSetOutput (mScl, FALSE);
  Lt9611TlmmConfigure (mScl, LT9611_TLMM_FUNC_GPIO, LT9611_TLMM_PULL_UP, 2, FALSE);
  Lt9611TlmmConfigure (mSda, LT9611_TLMM_FUNC_GPIO, LT9611_TLMM_PULL_UP, 2, FALSE);
  mI2cReady = TRUE;

  MicroSecondDelay (I2C_HALF_PERIOD_US * 4);
  DEBUG ((
    DEBUG_INFO,
    "%a: SDA GPIO%u = %u, SCL GPIO%u = %u after release\n",
    __func__,
    mSda,
    Lt9611TlmmGetInput (mSda),
    mScl,
    Lt9611TlmmGetInput (mScl)
    ));

  return Lt9611I2cBusClear ();
}

/**
  Lets go of both lines (inputs with pull-up) and stops using them. The
  caller then restores the pins' configuration.
**/
VOID
Lt9611I2cDeinit (
  VOID
  )
{
  if (!mI2cReady) {
    return;
  }

  SdaRelease ();
  Lt9611TlmmSetOutputEnable (mScl, FALSE);
  mI2cReady = FALSE;
}

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
  )
{
  EFI_STATUS  Status;
  UINTN       Index;
  BOOLEAN     SdaWasLow;

  if (!mI2cReady) {
    return EFI_NOT_READY;
  }

  SdaRelease ();
  Status = SclRelease ();
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: SCL stuck low\n", __func__));
    return EFI_DEVICE_ERROR;
  }

  SdaWasLow = !Lt9611TlmmGetInput (mSda);

  //
  // A slave interrupted in the middle of a read still drives its next data
  // bit. Clocking with the data line released lets it finish the byte, see
  // no acknowledge and let go. Nine clocks are harmless for an idle bus: no
  // slave listens before a START.
  //
  for (Index = 0; Index < I2C_BUS_CLEAR_CLOCKS; Index++) {
    SclLow ();
    MicroSecondDelay (I2C_HALF_PERIOD_US);
    Status = SclRelease ();
    if (EFI_ERROR (Status)) {
      return EFI_DEVICE_ERROR;
    }

    MicroSecondDelay (I2C_HALF_PERIOD_US);
  }

  SclLow ();
  MicroSecondDelay (I2C_HALF_PERIOD_US);
  Status = I2cStop ();
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "%a: bus still held (SDA %u, SCL %u)\n",
      __func__,
      Lt9611TlmmGetInput (mSda),
      Lt9611TlmmGetInput (mScl)
      ));
    return EFI_DEVICE_ERROR;
  }

  if (SdaWasLow) {
    DEBUG ((DEBUG_INFO, "%a: SDA was held low, bus freed\n", __func__));
  }

  return EFI_SUCCESS;
}

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
  )
{
  EFI_STATUS  Status;
  EFI_STATUS  StopStatus;

  if (!mI2cReady) {
    return EFI_NOT_READY;
  }

  Status = I2cStart ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status     = I2cSendAddressAndData (Address, I2C_WRITE, Data, Length);
  StopStatus = I2cStop ();
  if (!EFI_ERROR (Status) && EFI_ERROR (StopStatus)) {
    DEBUG ((DEBUG_ERROR, "%a: 0x%02x: bus held after STOP\n", __func__, Address));
    Status = StopStatus;
  }

  return Status;
}

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
  )
{
  EFI_STATUS  Status;
  EFI_STATUS  StopStatus;
  UINTN       Index;

  if (!mI2cReady) {
    return EFI_NOT_READY;
  }

  if (ReadLength == 0) {
    return EFI_INVALID_PARAMETER;
  }

  Status = I2cStart ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = I2cSendAddressAndData (Address, I2C_WRITE, WriteData, WriteLength);
  if (!EFI_ERROR (Status)) {
    Status = I2cRepeatedStart ();
  }

  if (!EFI_ERROR (Status)) {
    Status = I2cSendAddressAndData (Address, I2C_READ, NULL, 0);
  }

  //
  // Acknowledge every byte but the last, which tells the slave to stop
  // driving the data line so that the STOP can be sent.
  //
  for (Index = 0; !EFI_ERROR (Status) && (Index < ReadLength); Index++) {
    Status = I2cReadByte (&ReadData[Index], Index + 1 < ReadLength);
  }

  StopStatus = I2cStop ();
  if (!EFI_ERROR (Status) && EFI_ERROR (StopStatus)) {
    DEBUG ((DEBUG_ERROR, "%a: 0x%02x: bus held after STOP\n", __func__, Address));
    Status = StopStatus;
  }

  return Status;
}
