/** @file
  Lontium LT9611 MIPI DSI to HDMI 1.4 bridge of the RUBIK Pi 3.

  The bridge takes DSI0 on its MIPI port B and drives the HDMI connector. It
  is controlled over I2C (7-bit address 0x39 on I2C9); here the I2C pins are
  bit-banged (GpioI2c.c) while UEFI owns them, and given back to the QUP at
  the end.

  Registers are addressed as 0xPPRR: page PP, selected by writing it to
  register 0xFF, then register RR of that page, 8-bit values. The sequences
  and values follow the vendor Linux driver that drives this board
  (drivers/gpu/drm/bridge/lontium-lt9611.c of the Qualcomm 6.6 kernel),
  with the board's device tree overrides (mipi-digi-regs: MIPI port B), and
  were checked against the bridge's registers while that kernel showed
  1920x1080@60:
    - power-on: power enable, reset pulse, register access on, chip ID and
      revision, HPD interrupt setup, system init;
    - enable: MIPI input, TX PLL, video timing, PCR, MIPI analog, InfoFrames,
      HDMI/DVI mode, TX PHY, 500 ms, video check, TMDS on;
    - disable: TMDS off. The chip stays powered and out of reset, and the OS
      resets it when it probes.

  Copyright (c) 2026, edk2-flange contributors.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include "Lt9611Internal.h"

#define LT9611_PAGE_SELECT  0xFF        // register 0xFF of every page
#define LT9611_PAGE_NONE    0xFFFF      // page register state unknown

//
// Page 0x80: system.
//
#define LT9611_CHIP_ID0      0x8000
#define LT9611_CHIP_ID1      0x8001
#define LT9611_CHIP_REV      0x8002
#define LT9611_PCR_RESET     0x8011     // 0x5A resets the PCR, 0xFA runs it
#define LT9611_TXPLL_RESET   0x8016     // 0xF1 then 0xF3 after the TX PLL is set up
#define LT9611_I2C_ACCESS    0x80EE     // 0x01 enables register access

#define LT9611_PCR_RESET_ASSERT    0x5A
#define LT9611_PCR_RESET_RELEASE   0xFA
#define LT9611_TXPLL_RESET_ASSERT  0xF1
#define LT9611_TXPLL_RESET_RELEASE 0xF3

//
// Page 0x81: analog (MIPI RX, TX PLL, HDMI TX PHY).
//
#define LT9611_MIPI_RX_POWER  0x8102    // 0x12 on, 0x48 in the sleep sequence
#define LT9611_TXPLL_DIV      0x812D    // post divider for the pixel clock band
#define LT9611_HDMI_TX_PHY    0x8130    // TMDS output control

#define LT9611_MIPI_RX_POWER_ON  0x12
#define LT9611_HDMI_TX_PHY_ON    0xEA
#define LT9611_HDMI_TX_PHY_OFF   0x6A

//
// Page 0x82: interrupts, HPD, MIPI byte clock, frequency meters, PLL target,
// HDMI TX digital.
//
#define LT9611_IRQ_MASK3        0x8203  // bits 7:6: HPD unplug/plug, 1 = masked
#define LT9611_IRQ_CLEAR3       0x8207  // bits 7:6: clear HPD unplug/plug
#define LT9611_MIPI_BYTE_CLK    0x8250  // byte clock source: MIPI port A or B
#define LT9611_HPD_STATUS       0x825E
#define LT9611_VIDCHK_VTOTAL    0x826C  // 16-bit, big endian
#define LT9611_VIDCHK_VACTIVE   0x8282
#define LT9611_VIDCHK_HTOTAL    0x8286  // in system clock cycles
#define LT9611_HDMI_MODE        0x82D6
#define LT9611_HDMI_TX_CTRL     0x82D7
#define LT9611_TXPLL_CAL        0x82DE  // 0x20 then 0xE0 after the PLL target is set
#define LT9611_TXPLL_PCLK_HI    0x82E3  // pixel clock / 2 in kHz, bits 19:16
#define LT9611_TXPLL_PCLK_MID   0x82E4  // bits 15:8
#define LT9611_TXPLL_PCLK_LO    0x82E5  // bits 7:0

#define LT9611_MIPI_BYTE_CLK_PORT_A 0x10
#define LT9611_MIPI_BYTE_CLK_PORT_B 0x14
#define LT9611_IRQ3_HPD_MASK        (BIT7 | BIT6)
#define LT9611_IRQ_CLEAR3_ALL       0xFF
#define LT9611_IRQ_CLEAR3_NONE      0x3F
#define LT9611_HPD_STATUS_DETECTED  (BIT2 | BIT0)
#define LT9611_HDMI_MODE_HDMI       0x8C
#define LT9611_HDMI_MODE_DVI        0x0C
#define LT9611_HDMI_TX_CTRL_VALUE   0x04
#define LT9611_TXPLL_CAL_START      0x20
#define LT9611_TXPLL_CAL_RUN        0xE0

//
// Page 0x83: MIPI RX digital, video timing, PCR.
//
#define LT9611_VIDEO_VTOTAL_HI     0x830D
#define LT9611_VIDEO_VTOTAL_LO     0x830E
#define LT9611_VIDEO_VACTIVE_HI    0x830F
#define LT9611_VIDEO_VACTIVE_LO    0x8310
#define LT9611_VIDEO_HTOTAL_HI     0x8311
#define LT9611_VIDEO_HTOTAL_LO     0x8312
#define LT9611_VIDEO_HACTIVE_HI    0x8313
#define LT9611_VIDEO_HACTIVE_LO    0x8314
#define LT9611_VIDEO_VSYNC_LEN     0x8315
#define LT9611_VIDEO_HSYNC_LEN     0x8316
#define LT9611_VIDEO_VFP           0x8317
#define LT9611_VIDEO_VSYNC_PORCH   0x8318  // vsync + back porch
#define LT9611_VIDEO_HFP_LO        0x8319
#define LT9611_VIDEO_PORCH_HI      0x831A  // [7:4] hfp[11:8], [3:0] hsync porch[11:8]
#define LT9611_VIDEO_HSYNC_PORCH   0x831B  // hsync + back porch, bits 7:0
#define LT9611_SYNC_POLARITY       0x831D
#define LT9611_PCR_M               0x8326
#define LT9611_VIDCHK_HACTIVE_A    0x8382  // port A, 16-bit, bytes (3 per pixel)
#define LT9611_VIDCHK_HACTIVE_B    0x8386  // port B

#define LT9611_SYNC_POLARITY_BASE   0x10
#define LT9611_SYNC_POLARITY_NHSYNC BIT1
#define LT9611_SYNC_POLARITY_NVSYNC BIT0

//
// Page 0x84: InfoFrames.
//
#define LT9611_INFOFRAME_EN    0x843D
#define LT9611_AVI_INFOFRAME   0x8440
#define LT9611_HDMI_INFOFRAME  0x8474

#define LT9611_INFOFRAME_EN_AUDIO  BIT1
#define LT9611_INFOFRAME_EN_AVI    BIT3
#define LT9611_INFOFRAME_EN_HDMI   BIT5

//
// Page 0x85: DDC master, reads the sink's EDID in 32-byte pieces.
//
#define LT9611_DDC_CFG      0x8503
#define LT9611_DDC_SLAVE    0x8504
#define LT9611_DDC_OFFSET   0x8505
#define LT9611_DDC_LENGTH   0x8506
#define LT9611_DDC_CMD      0x8507
#define LT9611_DDC_CFG2     0x8514
#define LT9611_DDC_STATUS   0x8540
#define LT9611_DDC_FIFO     0x8583

#define LT9611_DDC_CFG_VALUE      0xC9
#define LT9611_DDC_SLAVE_EDID     0xA0     // 8-bit address of the EDID EEPROM
#define LT9611_DDC_CFG2_VALUE     0x7F
#define LT9611_DDC_CMD_RESET      0x36
#define LT9611_DDC_CMD_READ       0x31
#define LT9611_DDC_CMD_START      0x37
#define LT9611_DDC_CMD_IDLE       0x1F
#define LT9611_DDC_STATUS_DONE    BIT1
#define LT9611_DDC_STATUS_ERROR   (BIT6 | BIT4)   // no acknowledge, arbitration lost
#define LT9611_DDC_CHUNK          32

//
// What this board's bridge reads back (the vendor kernel logs revision 0xe2).
//
#define LT9611_EXPECTED_CHIP_ID   0x1702
#define LT9611_EXPECTED_REVISION  0xE2

//
// Waits. Power and reset follow the stock firmware of the board (a superset
// of the Linux driver's 20 ms steps); the others are the Linux driver's.
//
#define LT9611_POWER_SETTLE_US    (20 * 1000)
#define LT9611_RESET_HIGH_US      (20 * 1000)
#define LT9611_RESET_LOW_US       (100 * 1000)
#define LT9611_RESET_RECOVERY_US  (100 * 1000)
#define LT9611_TX_SETTLE_US       (500 * 1000)
#define LT9611_DDC_WAIT_US        (5 * 1000)
#define LT9611_DDC_POLL_US        1000
#define LT9611_DDC_POLLS          20
#define LT9611_PROBE_RETRY_US     (10 * 1000)
#define LT9611_PROBE_TRIES        3
#define LT9611_I2C_TRIES          2
#define LT9611_EDID_TRIES         4

//
// Mode limits: the vendor driver's (3840x2160 with UHD support), and what
// the timing registers hold.
//
#define LT9611_MAX_HACTIVE          3840
#define LT9611_MAX_VACTIVE          2160
#define LT9611_MAX_PIXEL_CLOCK_KHZ  340000

//
// CTA-861 InfoFrames.
//
#define HDMI_INFOFRAME_HEADER_SIZE     4
#define HDMI_INFOFRAME_TYPE_VENDOR     0x81
#define HDMI_INFOFRAME_TYPE_AVI        0x82
#define HDMI_AVI_INFOFRAME_VERSION     2
#define HDMI_AVI_INFOFRAME_LENGTH      13
#define HDMI_VENDOR_INFOFRAME_VERSION  1
#define HDMI_VENDOR_INFOFRAME_LENGTH   4     // OUI + HDMI_Video_Format, no HDMI VIC or 3D
#define HDMI_IEEE_OUI                  0x000C03

#define HDMI_AVI_SCAN_UNDERSCAN        2
#define HDMI_AVI_ACTIVE_INFO_VALID     BIT4
#define HDMI_AVI_ACTIVE_ASPECT_PICTURE 8     // active format = the picture
#define HDMI_PICTURE_ASPECT_NONE       0
#define HDMI_PICTURE_ASPECT_4_3        1
#define HDMI_PICTURE_ASPECT_16_9       2

#define EDID_BLOCK_SIZE       128
#define EDID_EXTENSION_COUNT  126

typedef struct {
  UINT16    Reg;
  UINT8     Value;
} LT9611_REG_VALUE;

//
// System init: crystal clock, frequency meter timers, interrupt setup (HPD
// and video check), and the blocks that must be powered to work.
//
STATIC CONST LT9611_REG_VALUE  mSystemInit[] = {
  { 0x8101, 0x18 },   // system clock from the crystal
  { 0x821B, 0x69 },   // frequency meter timer 2
  { 0x821C, 0x78 },
  { 0x82CB, 0x69 },   // frequency meter timer 1
  { 0x82CC, 0x78 },
  { 0x8251, 0x01 },   // interrupts
  { 0x8258, 0x0A },   // HPD interrupt
  { 0x8259, 0x80 },   // HPD debounce
  { 0x829E, 0xF7 },   // video check interrupt
  { 0x8004, 0xF0 },   // power for the working blocks
  { 0x8006, 0xF0 },
  { 0x800A, 0x80 },
  { 0x800B, 0x40 },
  { 0x800D, 0xEF },
  { 0x8011, 0xFA },
};

//
// MIPI input, digital part, port A: the Linux driver's default. 4 lanes,
// one port, byte clock from port A.
//
STATIC CONST LT9611_REG_VALUE  mMipiDigitalPortA[] = {
  { 0x8300, 0x00 },   // 4 lanes
  { 0x830A, 0x00 },   // single port
  { 0x824F, 0x80 },
  { LT9611_MIPI_BYTE_CLK, LT9611_MIPI_BYTE_CLK_PORT_A },
  { 0x8302, 0x0A },   // settle time
  { 0x8306, 0x0A },
};

//
// Port B: the RUBIK Pi 3 device tree replaces the table above
// (mipi-digi-regs): byte clock from port B, and port B made the primary
// port.
//
STATIC CONST LT9611_REG_VALUE  mMipiDigitalPortB[] = {
  { 0x8300, 0x00 },   // 4 lanes
  { 0x830A, 0x00 },   // single port
  { 0x824F, 0x80 },
  { LT9611_MIPI_BYTE_CLK, LT9611_MIPI_BYTE_CLK_PORT_B },
  { 0x8303, 0x40 },   // port B is the primary port
  { 0x8302, 0x0A },   // settle time
  { 0x8306, 0x0A },
};

//
// TX PLL, before the pixel clock specific settings.
//
STATIC CONST LT9611_REG_VALUE  mTxPllInit[] = {
  { 0x8123, 0x40 },
  { 0x8124, 0x64 },
  { 0x8125, 0x80 },
  { 0x8126, 0x55 },
  { 0x812C, 0x37 },
  { 0x812F, 0x01 },
  { 0x8126, 0x55 },
  { 0x8127, 0x66 },
  { 0x8128, 0x88 },
  { 0x812A, 0x20 },
};

//
// Pixel clock recovery: locks the HDMI pixel clock to the MIPI input.
//
STATIC CONST LT9611_REG_VALUE  mPcrSetup[] = {
  { 0x830B, 0x01 },
  { 0x830C, 0x10 },
  { 0x8348, 0x00 },
  { 0x8349, 0x81 },
  { 0x8321, 0x4A },   // stage 1
  { 0x8324, 0x71 },
  { 0x8325, 0x30 },
  { 0x832A, 0x01 },
  { 0x834A, 0x40 },   // stage 2
  { 0x832D, 0x38 },   // M limits
  { 0x8331, 0x08 },
};

//
// MIPI input, analog part: receiver current, LDO and low-power receivers of
// both ports, clock lane modes.
//
STATIC CONST LT9611_REG_VALUE  mMipiAnalog[] = {
  { 0x8106, 0x40 },   // port A receiver current
  { 0x810A, 0xFE },   // port A LDO voltage
  { 0x810B, 0xBF },   // port A low-power receiver on
  { 0x8111, 0x40 },   // port B receiver current
  { 0x8115, 0xFE },   // port B LDO voltage
  { 0x8116, 0xBF },   // port B low-power receiver on
  { 0x811C, 0x03 },   // port A clock lane
  { 0x8120, 0x03 },   // port B clock lane
};

//
// HDMI TX PHY, DC coupled, TMDS output still off.
//
STATIC CONST LT9611_REG_VALUE  mHdmiTxPhy[] = {
  { 0x8130, LT9611_HDMI_TX_PHY_OFF },
  { 0x8131, 0x44 },
  { 0x8132, 0x4A },   // DC coupled (0x73 for AC coupling)
  { 0x8133, 0x0B },
  { 0x8134, 0x00 },
  { 0x8135, 0x00 },
  { 0x8136, 0x00 },
  { 0x8137, 0x44 },
  { 0x813F, 0x0F },
  { 0x8140, 0xA0 },   // TMDS swing, per lane
  { 0x8141, 0xA0 },
  { 0x8142, 0xA0 },
  { 0x8143, 0xA0 },
  { 0x8144, 0x0A },
};

//
// CTA-861 VICs with a 4:3 picture. The other VICs up to 64 are 16:9.
//
STATIC CONST UINT8  mVics4By3[] = {
  1, 2, 6, 8, 10, 12, 14, 17, 21, 23, 25, 27, 29, 35, 37, 42, 44, 48, 50, 52, 54, 56, 58
};

//
// VICs above 64 with a 16:9 picture. The others there are 64:27 or 256:135,
// which the AVI InfoFrame cannot express.
//
STATIC CONST UINT8  mVicsHigh16By9[] = {
  93, 94, 95, 96, 97, 108, 111, 114, 117, 118, 194, 195, 196, 197, 198, 199, 200, 201
};

STATIC CONST UINT8  mEdidHeader[] = { 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00 };

STATIC LT9611_BOARD_CONFIG    mBoard;
STATIC BOOLEAN                mBusOwned;
STATIC BOOLEAN                mSystemInitDone;
STATIC UINT16                 mPage = LT9611_PAGE_NONE;
STATIC LT9611_TLMM_PIN_STATE  mSdaEntry;
STATIC LT9611_TLMM_PIN_STATE  mSclEntry;
STATIC DISPLAY_TIMING         mTiming;
STATIC BOOLEAN                mTimingSet;

/**
  Selects the register page, unless it is already selected.

  @param[in]  Page  The page.

  @retval EFI_SUCCESS  Selected.
  @retval Other        The I2C write failed.
**/
STATIC
EFI_STATUS
Lt9611SelectPage (
  IN UINT8  Page
  )
{
  EFI_STATUS  Status;
  UINT8       Buffer[2];

  if (mPage == Page) {
    return EFI_SUCCESS;
  }

  Buffer[0] = LT9611_PAGE_SELECT;
  Buffer[1] = Page;
  Status    = Lt9611I2cWrite (mBoard.I2cAddress, Buffer, sizeof (Buffer));
  mPage     = EFI_ERROR (Status) ? LT9611_PAGE_NONE : Page;
  return Status;
}

/**
  Recovers the bus after a failed transfer, so that the next one can work.
**/
STATIC
VOID
Lt9611RecoverBus (
  VOID
  )
{
  //
  // The page register may or may not have been written.
  //
  mPage = LT9611_PAGE_NONE;
  Lt9611I2cBusClear ();
}

/**
  Writes a register.

  @param[in]  Reg    The register, 0xPPRR.
  @param[in]  Value  The value.

  @retval EFI_SUCCESS  Written.
  @retval Other        The I2C transfer failed.
**/
STATIC
EFI_STATUS
Lt9611RegWrite (
  IN UINT16  Reg,
  IN UINT8   Value
  )
{
  EFI_STATUS  Status;
  UINT8       Buffer[2];
  UINTN       Try;

  Status = EFI_NOT_READY;
  for (Try = 0; Try < LT9611_I2C_TRIES; Try++) {
    Status = Lt9611SelectPage ((UINT8)(Reg >> 8));
    if (!EFI_ERROR (Status)) {
      Buffer[0] = (UINT8)Reg;
      Buffer[1] = Value;
      Status    = Lt9611I2cWrite (mBoard.I2cAddress, Buffer, sizeof (Buffer));
    }

    if (!EFI_ERROR (Status)) {
      return EFI_SUCCESS;
    }

    Lt9611RecoverBus ();
  }

  DEBUG ((DEBUG_ERROR, "%a: 0x%04x = 0x%02x: %r\n", __func__, Reg, Value, Status));
  return Status;
}

/**
  Reads a register.

  @param[in]   Reg    The register, 0xPPRR.
  @param[out]  Value  The value.

  @retval EFI_SUCCESS  Read.
  @retval Other        The I2C transfer failed.
**/
STATIC
EFI_STATUS
Lt9611RegRead (
  IN  UINT16  Reg,
  OUT UINT8   *Value
  )
{
  EFI_STATUS  Status;
  UINT8       Offset;
  UINTN       Try;

  Status = EFI_NOT_READY;
  Offset = (UINT8)Reg;
  for (Try = 0; Try < LT9611_I2C_TRIES; Try++) {
    Status = Lt9611SelectPage ((UINT8)(Reg >> 8));
    if (!EFI_ERROR (Status)) {
      Status = Lt9611I2cWriteRead (mBoard.I2cAddress, &Offset, 1, Value, 1);
    }

    if (!EFI_ERROR (Status)) {
      return EFI_SUCCESS;
    }

    Lt9611RecoverBus ();
  }

  DEBUG ((DEBUG_ERROR, "%a: 0x%04x: %r\n", __func__, Reg, Status));
  *Value = 0;
  return Status;
}

/**
  Reads a 16-bit big-endian register pair, as the video check registers are.

  @param[in]   Reg    The first (high) register.
  @param[out]  Value  The value.

  @retval EFI_SUCCESS  Read.
  @retval Other        The I2C transfer failed.
**/
STATIC
EFI_STATUS
Lt9611RegRead16 (
  IN  UINT16  Reg,
  OUT UINT32  *Value
  )
{
  EFI_STATUS  Status;
  UINT8       High;
  UINT8       Low;

  High   = 0;
  Low    = 0;
  *Value = 0;
  Status = Lt9611RegRead (Reg, &High);
  if (!EFI_ERROR (Status)) {
    Status = Lt9611RegRead (Reg + 1, &Low);
  }

  if (!EFI_ERROR (Status)) {
    *Value = ((UINT32)High << 8) | Low;
  }

  return Status;
}

/**
  Writes a register sequence, in order.

  @param[in]  Sequence  The registers and values.
  @param[in]  Count     Their number.

  @retval EFI_SUCCESS  All written.
  @retval Other        A write failed; the rest were not written.
**/
STATIC
EFI_STATUS
Lt9611WriteSequence (
  IN CONST LT9611_REG_VALUE  *Sequence,
  IN UINTN                   Count
  )
{
  EFI_STATUS  Status;
  UINTN       Index;

  for (Index = 0; Index < Count; Index++) {
    Status = Lt9611RegWrite (Sequence[Index].Reg, Sequence[Index].Value);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  return EFI_SUCCESS;
}

/**
  Writes consecutive registers from a buffer.

  @param[in]  Reg     The first register.
  @param[in]  Data    The values.
  @param[in]  Length  Their number.

  @retval EFI_SUCCESS  All written.
  @retval Other        A write failed.
**/
STATIC
EFI_STATUS
Lt9611WriteBuffer (
  IN UINT16       Reg,
  IN CONST UINT8  *Data,
  IN UINTN        Length
  )
{
  EFI_STATUS  Status;
  UINTN       Index;

  for (Index = 0; Index < Length; Index++) {
    Status = Lt9611RegWrite ((UINT16)(Reg + Index), Data[Index]);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  return EFI_SUCCESS;
}

/**
  Reads a register back and logs it next to the value it should have, for
  the serial log of a bring-up.

  @param[in]  Reg       The register.
  @param[in]  Expected  The value it should read.
**/
STATIC
VOID
Lt9611LogReadBack (
  IN UINT16  Reg,
  IN UINT8   Expected
  )
{
  UINT8  Value;

  if (EFI_ERROR (Lt9611RegRead (Reg, &Value))) {
    return;
  }

  DEBUG ((
    (Value == Expected) ? DEBUG_INFO : DEBUG_WARN,
    "%a: 0x%04x = 0x%02x%a\n",
    __func__,
    Reg,
    Value,
    (Value == Expected) ? "" : " (differs from what was written)"
    ));
}

/**
  Powers the bridge up and runs the reset pulse. The I2C pins are not
  touched: the bus pull-ups hang off the bridge's supply.
**/
STATIC
VOID
Lt9611PowerAndReset (
  VOID
  )
{
  //
  // Keep the bridge in reset while its supply comes up. Both pins end up as
  // Linux leaves them: GPIO, output, 2 mA, with the reset default pull-down
  // still set, which holds the bridge off should the output ever be
  // disabled.
  //
  Lt9611TlmmSetOutput (mBoard.ResetGpio, FALSE);
  Lt9611TlmmConfigure (mBoard.ResetGpio, LT9611_TLMM_FUNC_GPIO, LT9611_TLMM_PULL_DOWN, 2, TRUE);

  Lt9611TlmmSetOutput (mBoard.PowerGpio, TRUE);
  Lt9611TlmmConfigure (mBoard.PowerGpio, LT9611_TLMM_FUNC_GPIO, LT9611_TLMM_PULL_DOWN, 2, TRUE);
  MicroSecondDelay (LT9611_POWER_SETTLE_US);

  //
  // Reset pulse (reset is active low).
  //
  Lt9611TlmmSetOutput (mBoard.ResetGpio, TRUE);
  MicroSecondDelay (LT9611_RESET_HIGH_US);
  Lt9611TlmmSetOutput (mBoard.ResetGpio, FALSE);
  MicroSecondDelay (LT9611_RESET_LOW_US);
  Lt9611TlmmSetOutput (mBoard.ResetGpio, TRUE);
  MicroSecondDelay (LT9611_RESET_RECOVERY_US);

  Lt9611TlmmLogPin ("LT9611 power", mBoard.PowerGpio);
  Lt9611TlmmLogPin ("LT9611 reset", mBoard.ResetGpio);
}

/**
  Turns on register access and checks the chip ID and revision.

  @retval EFI_SUCCESS    An LT9611 answers.
  @retval EFI_NOT_FOUND  Nothing answers, or not an LT9611.
**/
STATIC
EFI_STATUS
Lt9611Probe (
  VOID
  )
{
  EFI_STATUS  Status;
  UINTN       Try;
  UINT8       Id0;
  UINT8       Id1;
  UINT8       Revision;

  Id0      = 0;
  Id1      = 0;
  Revision = 0;
  Status   = EFI_NOT_FOUND;
  for (Try = 0; Try < LT9611_PROBE_TRIES; Try++) {
    Status = Lt9611RegWrite (LT9611_I2C_ACCESS, 0x01);
    if (!EFI_ERROR (Status)) {
      break;
    }

    MicroSecondDelay (LT9611_PROBE_RETRY_US);
  }

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: no answer at I2C address 0x%02x: %r\n", __func__, mBoard.I2cAddress, Status));
    return EFI_NOT_FOUND;
  }

  Status = Lt9611RegRead (LT9611_CHIP_ID0, &Id0);
  if (!EFI_ERROR (Status)) {
    Status = Lt9611RegRead (LT9611_CHIP_ID1, &Id1);
  }

  if (!EFI_ERROR (Status)) {
    Status = Lt9611RegRead (LT9611_CHIP_REV, &Revision);
  }

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: cannot read the chip ID: %r\n", __func__, Status));
    return EFI_NOT_FOUND;
  }

  DEBUG ((DEBUG_INFO, "%a: chip ID 0x%02x%02x, revision 0x%02x\n", __func__, Id0, Id1, Revision));

  if ((((UINT32)Id0 << 8) | Id1) != LT9611_EXPECTED_CHIP_ID) {
    DEBUG ((DEBUG_ERROR, "%a: not an LT9611 (chip ID 0x%04x expected)\n", __func__, LT9611_EXPECTED_CHIP_ID));
    return EFI_NOT_FOUND;
  }

  if (Revision != LT9611_EXPECTED_REVISION) {
    DEBUG ((DEBUG_WARN, "%a: revision 0x%02x, this board has 0x%02x\n", __func__, Revision, LT9611_EXPECTED_REVISION));
  }

  return EFI_SUCCESS;
}

/**
  Unmasks the HPD interrupts and clears pending ones, as the Linux driver
  does when it probes. Nothing here uses the interrupt line (the HPD status
  is polled); the flags are only latched.

  @retval EFI_SUCCESS  Done.
  @retval Other        An I2C transfer failed.
**/
STATIC
EFI_STATUS
Lt9611SetUpHpdInterrupts (
  VOID
  )
{
  EFI_STATUS  Status;
  UINT8       Mask;

  Status = Lt9611RegRead (LT9611_IRQ_MASK3, &Mask);
  if (!EFI_ERROR (Status)) {
    Status = Lt9611RegWrite (LT9611_IRQ_MASK3, Mask & (UINT8) ~LT9611_IRQ3_HPD_MASK);
  }

  if (!EFI_ERROR (Status)) {
    Status = Lt9611RegWrite (LT9611_IRQ_CLEAR3, LT9611_IRQ_CLEAR3_ALL);
  }

  if (!EFI_ERROR (Status)) {
    Status = Lt9611RegWrite (LT9611_IRQ_CLEAR3, LT9611_IRQ_CLEAR3_NONE);
  }

  return Status;
}

/**
  Writes the system init sequence, which the HPD detection, the DDC master
  and the video path all need. Done once after reset, and again should the
  bridge have been turned off.

  @retval EFI_SUCCESS  Done.
  @retval Other        An I2C transfer failed.
**/
STATIC
EFI_STATUS
Lt9611SystemInit (
  VOID
  )
{
  EFI_STATUS  Status;

  Status = Lt9611WriteSequence (mSystemInit, ARRAY_SIZE (mSystemInit));
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: %r\n", __func__, Status));
    return Status;
  }

  mSystemInitDone = TRUE;
  return EFI_SUCCESS;
}

/**
  Powers the LT9611 up and out of reset, takes over its I2C pins as GPIOs and
  checks that it answers with the expected chip ID and revision.

  @param[in]  Board  How the LT9611 is wired.

  @retval EFI_SUCCESS    The LT9611 is there.
  @retval EFI_NOT_FOUND  It does not answer; the pins have been released.
**/
EFI_STATUS
Lt9611PowerOn (
  IN CONST LT9611_BOARD_CONFIG  *Board
  )
{
  EFI_STATUS  Status;

  if (Board == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (mBusOwned) {
    DEBUG ((DEBUG_WARN, "%a: already on, starting over\n", __func__));
    Lt9611ReleaseBus ();
  }

  if (!Lt9611TlmmIsValidGpio (Board->SdaGpio) || !Lt9611TlmmIsValidGpio (Board->SclGpio) ||
      !Lt9611TlmmIsValidGpio (Board->ResetGpio) || !Lt9611TlmmIsValidGpio (Board->PowerGpio) ||
      (Board->SdaGpio == Board->SclGpio) ||
      (Board->ResetGpio == Board->PowerGpio) ||
      (Board->ResetGpio == Board->SdaGpio) || (Board->ResetGpio == Board->SclGpio) ||
      (Board->PowerGpio == Board->SdaGpio) || (Board->PowerGpio == Board->SclGpio) ||
      (Board->I2cAddress < 0x08) || (Board->I2cAddress > 0x77))
  {
    DEBUG ((
      DEBUG_ERROR,
      "%a: bad board config: I2C 0x%02x SDA %u SCL %u reset %u power %u\n",
      __func__,
      Board->I2cAddress,
      Board->SdaGpio,
      Board->SclGpio,
      Board->ResetGpio,
      Board->PowerGpio
      ));
    return EFI_INVALID_PARAMETER;
  }

  CopyMem (&mBoard, Board, sizeof (mBoard));
  mSystemInitDone = FALSE;
  mTimingSet      = FALSE;
  mPage           = LT9611_PAGE_NONE;

  DEBUG ((
    DEBUG_INFO,
    "%a: I2C 0x%02x on SDA GPIO%u / SCL GPIO%u, reset GPIO%u, power GPIO%u, MIPI port %a\n",
    __func__,
    mBoard.I2cAddress,
    mBoard.SdaGpio,
    mBoard.SclGpio,
    mBoard.ResetGpio,
    mBoard.PowerGpio,
    mBoard.PortB ? "B" : "A"
    ));

  Lt9611TlmmLogPin ("LT9611 power (entry)", mBoard.PowerGpio);
  Lt9611TlmmLogPin ("LT9611 reset (entry)", mBoard.ResetGpio);
  Lt9611TlmmLogPin ("I2C SDA (entry)", mBoard.SdaGpio);
  Lt9611TlmmLogPin ("I2C SCL (entry)", mBoard.SclGpio);

  //
  // Saved before anything changes, so that Lt9611ReleaseBus hands the pins
  // back to the QUP exactly as they were.
  //
  Lt9611TlmmSavePin (mBoard.SdaGpio, &mSdaEntry);
  Lt9611TlmmSavePin (mBoard.SclGpio, &mSclEntry);

  Lt9611PowerAndReset ();

  mBusOwned = TRUE;
  Status    = Lt9611I2cInit (mBoard.SdaGpio, mBoard.SclGpio);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: I2C bus unusable: %r\n", __func__, Status));
    goto Fail;
  }

  Status = Lt9611Probe ();
  if (EFI_ERROR (Status)) {
    goto Fail;
  }

  Status = Lt9611SetUpHpdInterrupts ();
  if (!EFI_ERROR (Status)) {
    Status = Lt9611SystemInit ();
  }

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: init failed: %r\n", __func__, Status));
    goto Fail;
  }

  DEBUG ((DEBUG_INFO, "%a: LT9611 up\n", __func__));
  return EFI_SUCCESS;

Fail:
  //
  // The bridge stays powered and out of reset: the OS resets it itself.
  //
  Lt9611ReleaseBus ();
  return EFI_NOT_FOUND;
}

/**
  Returns whether a sink is connected (HDMI hot plug detect).
**/
BOOLEAN
Lt9611IsHotPlugged (
  VOID
  )
{
  UINT8    Value;
  BOOLEAN  Plugged;

  if (!mBusOwned) {
    DEBUG ((DEBUG_ERROR, "%a: LT9611 not powered on\n", __func__));
    return FALSE;
  }

  if (EFI_ERROR (Lt9611RegRead (LT9611_HPD_STATUS, &Value))) {
    return FALSE;
  }

  Plugged = (Value & LT9611_HPD_STATUS_DETECTED) != 0;
  DEBUG ((DEBUG_INFO, "%a: HPD status 0x%02x: %a\n", __func__, Value, Plugged ? "sink connected" : "no sink"));
  return Plugged;
}

/**
  Reads one 128-byte EDID block through the bridge's DDC master.

  @param[in]   Block   The block, 0 or 1 (the DDC offset is 8 bits and
                       there is no segment pointer).
  @param[out]  Buffer  128 bytes.

  @retval EFI_SUCCESS       Read.
  @retval EFI_NO_RESPONSE   The sink did not acknowledge.
  @retval EFI_TIMEOUT       The DDC transfer did not finish.
  @retval Other             An I2C transfer to the bridge failed.
**/
STATIC
EFI_STATUS
Lt9611DdcReadBlock (
  IN  UINTN  Block,
  OUT UINT8  *Buffer
  )
{
  EFI_STATUS  Status;
  EFI_STATUS  IdleStatus;
  UINTN       Chunk;
  UINTN       Poll;
  UINTN       Index;
  UINT8       DdcStatus;

  ASSERT (Block <= 1);

  DdcStatus = 0;

  Status = Lt9611RegWrite (LT9611_DDC_CFG, LT9611_DDC_CFG_VALUE);
  if (!EFI_ERROR (Status)) {
    Status = Lt9611RegWrite (LT9611_DDC_SLAVE, LT9611_DDC_SLAVE_EDID);
  }

  if (!EFI_ERROR (Status)) {
    Status = Lt9611RegWrite (LT9611_DDC_OFFSET, 0);
  }

  if (!EFI_ERROR (Status)) {
    Status = Lt9611RegWrite (LT9611_DDC_LENGTH, LT9611_DDC_CHUNK);
  }

  if (!EFI_ERROR (Status)) {
    Status = Lt9611RegWrite (LT9611_DDC_CFG2, LT9611_DDC_CFG2_VALUE);
  }

  for (Chunk = 0; !EFI_ERROR (Status) && (Chunk < EDID_BLOCK_SIZE / LT9611_DDC_CHUNK); Chunk++) {
    Status = Lt9611RegWrite (LT9611_DDC_OFFSET, (UINT8)(Block * EDID_BLOCK_SIZE + Chunk * LT9611_DDC_CHUNK));
    if (!EFI_ERROR (Status)) {
      Status = Lt9611RegWrite (LT9611_DDC_CMD, LT9611_DDC_CMD_RESET);
    }

    if (!EFI_ERROR (Status)) {
      Status = Lt9611RegWrite (LT9611_DDC_CMD, LT9611_DDC_CMD_READ);
    }

    if (!EFI_ERROR (Status)) {
      Status = Lt9611RegWrite (LT9611_DDC_CMD, LT9611_DDC_CMD_START);
    }

    if (EFI_ERROR (Status)) {
      break;
    }

    //
    // 32 bytes at the DDC's 100 kHz take about 3.5 ms. The Linux driver
    // waits 5 ms and looks once; look a little longer before giving up.
    //
    MicroSecondDelay (LT9611_DDC_WAIT_US);
    Status = EFI_TIMEOUT;
    for (Poll = 0; Poll < LT9611_DDC_POLLS; Poll++) {
      if (EFI_ERROR (Lt9611RegRead (LT9611_DDC_STATUS, &DdcStatus))) {
        Status = EFI_DEVICE_ERROR;
        break;
      }

      if ((DdcStatus & LT9611_DDC_STATUS_DONE) != 0) {
        Status = EFI_SUCCESS;
        break;
      }

      if ((DdcStatus & LT9611_DDC_STATUS_ERROR) != 0) {
        DEBUG ((DEBUG_ERROR, "%a: block %u offset %u: no acknowledge (status 0x%02x)\n", __func__, (UINT32)Block, (UINT32)(Chunk * LT9611_DDC_CHUNK), DdcStatus));
        Status = EFI_NO_RESPONSE;
        break;
      }

      MicroSecondDelay (LT9611_DDC_POLL_US);
    }

    if (Status == EFI_TIMEOUT) {
      DEBUG ((DEBUG_ERROR, "%a: block %u offset %u: not done (status 0x%02x)\n", __func__, (UINT32)Block, (UINT32)(Chunk * LT9611_DDC_CHUNK), DdcStatus));
    }

    //
    // The data comes out of a FIFO, one register read per byte.
    //
    for (Index = 0; !EFI_ERROR (Status) && (Index < LT9611_DDC_CHUNK); Index++) {
      Status = Lt9611RegRead (LT9611_DDC_FIFO, &Buffer[Chunk * LT9611_DDC_CHUNK + Index]);
    }
  }

  IdleStatus = Lt9611RegWrite (LT9611_DDC_CMD, LT9611_DDC_CMD_IDLE);
  if (!EFI_ERROR (Status)) {
    Status = IdleStatus;
  }

  return Status;
}

/**
  Returns whether the bytes of an EDID block add up to 0.

  @param[in]  Block  128 bytes.

  @retval TRUE   The checksum is right.
  @retval FALSE  It is not.
**/
STATIC
BOOLEAN
Lt9611EdidChecksumOk (
  IN CONST UINT8  *Block
  )
{
  UINT8  Sum;
  UINTN  Index;

  Sum = 0;
  for (Index = 0; Index < EDID_BLOCK_SIZE; Index++) {
    Sum = (UINT8)(Sum + Block[Index]);
  }

  return Sum == 0;
}

/**
  Reads an EDID block and checks it, trying again when the transfer fails or
  the data is bad, as the Linux EDID code does.

  @param[in]   Block   0 or 1.
  @param[out]  Buffer  128 bytes.

  @retval EFI_SUCCESS       Read and valid.
  @retval EFI_NO_RESPONSE   The sink did not acknowledge.
  @retval EFI_DEVICE_ERROR  The data stayed invalid, or the transfer failed.
**/
STATIC
EFI_STATUS
Lt9611ReadEdidBlockChecked (
  IN  UINTN  Block,
  OUT UINT8  *Buffer
  )
{
  EFI_STATUS  Status;
  UINTN       Try;

  Status = EFI_DEVICE_ERROR;
  for (Try = 0; Try < LT9611_EDID_TRIES; Try++) {
    ZeroMem (Buffer, EDID_BLOCK_SIZE);
    Status = Lt9611DdcReadBlock (Block, Buffer);
    if (EFI_ERROR (Status)) {
      continue;
    }

    if ((Block == 0) && (CompareMem (Buffer, mEdidHeader, sizeof (mEdidHeader)) != 0)) {
      DEBUG ((DEBUG_ERROR, "%a: block 0: bad header %02x %02x %02x %02x\n", __func__, Buffer[0], Buffer[1], Buffer[2], Buffer[3]));
      Status = EFI_DEVICE_ERROR;
      continue;
    }

    if (!Lt9611EdidChecksumOk (Buffer)) {
      DEBUG ((DEBUG_ERROR, "%a: block %u: bad checksum\n", __func__, (UINT32)Block));
      Status = EFI_DEVICE_ERROR;
      continue;
    }

    return EFI_SUCCESS;
  }

  return Status;
}

/**
  Reads the sink's EDID through the LT9611 DDC master.

  @param[out]  Edid        The buffer.
  @param[in]   BufferSize  Its size; 256 reads the base block and the first
                           extension block when there is one.
  @param[out]  EdidSize    The number of bytes read (128 or 256).

  @retval EFI_SUCCESS       Read, header and checksums valid.
  @retval EFI_NOT_READY     No sink.
  @retval EFI_DEVICE_ERROR  The DDC transfer failed or the data is invalid.
**/
EFI_STATUS
Lt9611ReadEdid (
  OUT UINT8  *Edid,
  IN  UINTN  BufferSize,
  OUT UINTN  *EdidSize
  )
{
  EFI_STATUS  Status;
  BOOLEAN     Plugged;
  UINT8       Hpd;
  UINT8       Extensions;

  if ((Edid == NULL) || (EdidSize == NULL) || (BufferSize < EDID_BLOCK_SIZE)) {
    return EFI_INVALID_PARAMETER;
  }

  *EdidSize = 0;
  if (!mBusOwned) {
    DEBUG ((DEBUG_ERROR, "%a: LT9611 not powered on\n", __func__));
    return EFI_DEVICE_ERROR;
  }

  //
  // The DDC master needs the system init, as in the Linux get_edid path.
  //
  if (!mSystemInitDone && EFI_ERROR (Lt9611SystemInit ())) {
    return EFI_DEVICE_ERROR;
  }

  //
  // Try the read even without HPD: a sink with a weak HPD can still answer
  // on DDC, and its EDID is worth having. The HPD status is read here, not
  // through Lt9611IsHotPlugged, so that a bridge that stopped answering is
  // not reported as a missing sink.
  //
  Status = Lt9611RegRead (LT9611_HPD_STATUS, &Hpd);
  if (EFI_ERROR (Status)) {
    return EFI_DEVICE_ERROR;
  }

  Plugged = (Hpd & LT9611_HPD_STATUS_DETECTED) != 0;
  Status  = Lt9611ReadEdidBlockChecked (0, Edid);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: no valid EDID (HPD status 0x%02x): %r\n", __func__, Hpd, Status));
    return Plugged ? EFI_DEVICE_ERROR : EFI_NOT_READY;
  }

  if (!Plugged) {
    DEBUG ((DEBUG_WARN, "%a: EDID read although HPD is low (status 0x%02x)\n", __func__, Hpd));
  }

  *EdidSize  = EDID_BLOCK_SIZE;
  Extensions = Edid[EDID_EXTENSION_COUNT];
  DEBUG ((DEBUG_INFO, "%a: base block valid, %u extension block(s)\n", __func__, Extensions));

  if (Extensions == 0) {
    return EFI_SUCCESS;
  }

  if (BufferSize < 2 * EDID_BLOCK_SIZE) {
    DEBUG ((DEBUG_INFO, "%a: no room for the extension block\n", __func__));
    return EFI_SUCCESS;
  }

  //
  // A bad extension block does not spoil the base block: return that alone.
  //
  Status = Lt9611ReadEdidBlockChecked (1, Edid + EDID_BLOCK_SIZE);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "%a: extension block unusable (%r), base block only\n", __func__, Status));
    ZeroMem (Edid + EDID_BLOCK_SIZE, EDID_BLOCK_SIZE);
    return EFI_SUCCESS;
  }

  *EdidSize = 2 * EDID_BLOCK_SIZE;
  if (Extensions > 1) {
    DEBUG ((DEBUG_INFO, "%a: only the first of %u extension blocks is readable\n", __func__, Extensions));
  }

  DEBUG ((DEBUG_INFO, "%a: extension block valid, tag 0x%02x\n", __func__, Edid[EDID_BLOCK_SIZE]));
  return EFI_SUCCESS;
}

/**
  Checks that a mode fits the bridge and its timing registers.

  @param[in]  Timing  The mode.

  @retval EFI_SUCCESS      It fits.
  @retval EFI_UNSUPPORTED  It does not.
**/
STATIC
EFI_STATUS
Lt9611CheckTiming (
  IN CONST DISPLAY_TIMING  *Timing
  )
{
  UINT32  HTotal;
  UINT32  VTotal;

  HTotal = DISPLAY_H_TOTAL (Timing);
  VTotal = DISPLAY_V_TOTAL (Timing);

  if ((Timing->PixelClockKhz == 0) || (Timing->PixelClockKhz > LT9611_MAX_PIXEL_CLOCK_KHZ) ||
      (Timing->HActive == 0) || (Timing->HActive > LT9611_MAX_HACTIVE) ||
      (Timing->VActive == 0) || (Timing->VActive > LT9611_MAX_VACTIVE) ||
      (HTotal > MAX_UINT16) || (VTotal > MAX_UINT16) ||
      (Timing->HSyncWidth > MAX_UINT8) || (Timing->VSyncWidth > MAX_UINT8) ||
      (Timing->VFrontPorch > MAX_UINT8) ||
      ((UINT32)Timing->VSyncWidth + Timing->VBackPorch > MAX_UINT8) ||
      (Timing->HFrontPorch > 0xFFF) ||
      ((UINT32)Timing->HSyncWidth + Timing->HBackPorch > 0xFFF))
  {
    DEBUG ((
      DEBUG_ERROR,
      "%a: %ux%u, %u kHz, htotal %u, vtotal %u: not supported\n",
      __func__,
      Timing->HActive,
      Timing->VActive,
      Timing->PixelClockKhz,
      HTotal,
      VTotal
      ));
    return EFI_UNSUPPORTED;
  }

  return EFI_SUCCESS;
}

/**
  Programs the TX PLL for the pixel clock.

  @param[in]   Timing   The mode.
  @param[out]  PostDiv  The post divider the pixel clock band uses.

  @retval EFI_SUCCESS  Done.
  @retval Other        An I2C transfer failed.
**/
STATIC
EFI_STATUS
Lt9611TxPllSetup (
  IN  CONST DISPLAY_TIMING  *Timing,
  OUT UINT32                *PostDiv
  )
{
  EFI_STATUS  Status;
  UINT32      PixelClock;
  UINT8       Divider;

  PixelClock = Timing->PixelClockKhz;

  if (PixelClock > 150000) {
    Divider  = 0x88;
    *PostDiv = 1;
  } else if (PixelClock > 70000) {
    Divider  = 0x99;
    *PostDiv = 2;
  } else {
    Divider  = 0xAA;
    *PostDiv = 4;
  }

  Status = Lt9611WriteSequence (mTxPllInit, ARRAY_SIZE (mTxPllInit));
  if (!EFI_ERROR (Status)) {
    Status = Lt9611RegWrite (LT9611_TXPLL_DIV, Divider);
  }

  //
  // The PLL target is half the pixel clock in kHz, split over three
  // registers. Each register keeps the low 8 bits of its shift, as the
  // Linux driver writes them.
  //
  if (!EFI_ERROR (Status)) {
    Status = Lt9611RegWrite (LT9611_TXPLL_PCLK_HI, (UINT8)(PixelClock >> 17));
  }

  if (!EFI_ERROR (Status)) {
    Status = Lt9611RegWrite (LT9611_TXPLL_PCLK_MID, (UINT8)(PixelClock >> 9));
  }

  if (!EFI_ERROR (Status)) {
    Status = Lt9611RegWrite (LT9611_TXPLL_PCLK_LO, (UINT8)(PixelClock >> 1));
  }

  if (!EFI_ERROR (Status)) {
    Status = Lt9611RegWrite (LT9611_TXPLL_CAL, LT9611_TXPLL_CAL_START);
  }

  if (!EFI_ERROR (Status)) {
    Status = Lt9611RegWrite (LT9611_TXPLL_CAL, LT9611_TXPLL_CAL_RUN);
  }

  if (!EFI_ERROR (Status)) {
    Status = Lt9611RegWrite (LT9611_TXPLL_RESET, LT9611_TXPLL_RESET_ASSERT);
  }

  if (!EFI_ERROR (Status)) {
    Status = Lt9611RegWrite (LT9611_TXPLL_RESET, LT9611_TXPLL_RESET_RELEASE);
  }

  DEBUG ((DEBUG_INFO, "%a: %u kHz: divider 0x%02x, post divider %u: %r\n", __func__, PixelClock, Divider, *PostDiv, Status));
  return Status;
}

/**
  Programs the video timing the bridge expects on its MIPI input.

  @param[in]  Timing  The mode.

  @retval EFI_SUCCESS  Done.
  @retval Other        An I2C transfer failed.
**/
STATIC
EFI_STATUS
Lt9611VideoTimingSetup (
  IN CONST DISPLAY_TIMING  *Timing
  )
{
  UINT32            HTotal;
  UINT32            VTotal;
  UINT32            HSyncPorch;
  UINT32            VSyncPorch;
  LT9611_REG_VALUE  Regs[15];

  HTotal     = DISPLAY_H_TOTAL (Timing);
  VTotal     = DISPLAY_V_TOTAL (Timing);
  HSyncPorch = (UINT32)Timing->HSyncWidth + Timing->HBackPorch;
  VSyncPorch = (UINT32)Timing->VSyncWidth + Timing->VBackPorch;

  Regs[0].Reg    = LT9611_VIDEO_VTOTAL_HI;
  Regs[0].Value  = (UINT8)(VTotal >> 8);
  Regs[1].Reg    = LT9611_VIDEO_VTOTAL_LO;
  Regs[1].Value  = (UINT8)VTotal;
  Regs[2].Reg    = LT9611_VIDEO_VACTIVE_HI;
  Regs[2].Value  = (UINT8)(Timing->VActive >> 8);
  Regs[3].Reg    = LT9611_VIDEO_VACTIVE_LO;
  Regs[3].Value  = (UINT8)Timing->VActive;
  Regs[4].Reg    = LT9611_VIDEO_HTOTAL_HI;
  Regs[4].Value  = (UINT8)(HTotal >> 8);
  Regs[5].Reg    = LT9611_VIDEO_HTOTAL_LO;
  Regs[5].Value  = (UINT8)HTotal;
  Regs[6].Reg    = LT9611_VIDEO_HACTIVE_HI;
  Regs[6].Value  = (UINT8)(Timing->HActive >> 8);
  Regs[7].Reg    = LT9611_VIDEO_HACTIVE_LO;
  Regs[7].Value  = (UINT8)Timing->HActive;
  Regs[8].Reg    = LT9611_VIDEO_VSYNC_LEN;
  Regs[8].Value  = (UINT8)Timing->VSyncWidth;
  Regs[9].Reg    = LT9611_VIDEO_HSYNC_LEN;
  Regs[9].Value  = (UINT8)Timing->HSyncWidth;
  Regs[10].Reg   = LT9611_VIDEO_VFP;
  Regs[10].Value = (UINT8)Timing->VFrontPorch;
  Regs[11].Reg   = LT9611_VIDEO_VSYNC_PORCH;
  Regs[11].Value = (UINT8)VSyncPorch;
  Regs[12].Reg   = LT9611_VIDEO_HFP_LO;
  Regs[12].Value = (UINT8)Timing->HFrontPorch;
  Regs[13].Reg   = LT9611_VIDEO_PORCH_HI;
  Regs[13].Value = (UINT8)(((Timing->HFrontPorch >> 8) << 4) | ((HSyncPorch >> 8) & 0xF));
  Regs[14].Reg   = LT9611_VIDEO_HSYNC_PORCH;
  Regs[14].Value = (UINT8)HSyncPorch;

  return Lt9611WriteSequence (Regs, ARRAY_SIZE (Regs));
}

/**
  Programs the pixel clock recovery and restarts it.

  @param[in]  Timing   The mode.
  @param[in]  PostDiv  The TX PLL post divider.

  @retval EFI_SUCCESS  Done.
  @retval Other        An I2C transfer failed.
**/
STATIC
EFI_STATUS
Lt9611PcrSetup (
  IN CONST DISPLAY_TIMING  *Timing,
  IN UINT32                PostDiv
  )
{
  EFI_STATUS  Status;
  UINT8       Polarity;
  UINT8       PcrM;

  Polarity = LT9611_SYNC_POLARITY_BASE;
  if (!Timing->HSyncPositive) {
    Polarity |= LT9611_SYNC_POLARITY_NHSYNC;
  }

  if (!Timing->VSyncPositive) {
    Polarity |= LT9611_SYNC_POLARITY_NVSYNC;
  }

  //
  // M of the PCR: pixel clock * 5 * post divider over the 27 MHz reference.
  //
  PcrM = (UINT8)(Timing->PixelClockKhz * 5 * PostDiv / 27000);

  Status = Lt9611RegWrite (LT9611_SYNC_POLARITY, Polarity);
  if (!EFI_ERROR (Status)) {
    Status = Lt9611WriteSequence (mPcrSetup, ARRAY_SIZE (mPcrSetup));
  }

  if (!EFI_ERROR (Status)) {
    Status = Lt9611RegWrite (LT9611_PCR_M, PcrM);
  }

  if (!EFI_ERROR (Status)) {
    Status = Lt9611RegWrite (LT9611_PCR_RESET, LT9611_PCR_RESET_ASSERT);
  }

  if (!EFI_ERROR (Status)) {
    Status = Lt9611RegWrite (LT9611_PCR_RESET, LT9611_PCR_RESET_RELEASE);
  }

  DEBUG ((DEBUG_INFO, "%a: polarity 0x%02x, M 0x%02x: %r\n", __func__, Polarity, PcrM, Status));
  return Status;
}

/**
  Returns the picture aspect ratio of a CTA-861 VIC, as the AVI InfoFrame
  codes it.

  @param[in]  Vic  The VIC.

  @return  HDMI_PICTURE_ASPECT_*.
**/
STATIC
UINT8
Lt9611VicPictureAspect (
  IN UINT8  Vic
  )
{
  UINTN  Index;

  if (Vic == 0) {
    return HDMI_PICTURE_ASPECT_NONE;
  }

  for (Index = 0; Index < ARRAY_SIZE (mVics4By3); Index++) {
    if (mVics4By3[Index] == Vic) {
      return HDMI_PICTURE_ASPECT_4_3;
    }
  }

  if (Vic <= 64) {
    return HDMI_PICTURE_ASPECT_16_9;
  }

  for (Index = 0; Index < ARRAY_SIZE (mVicsHigh16By9); Index++) {
    if (mVicsHigh16By9[Index] == Vic) {
      return HDMI_PICTURE_ASPECT_16_9;
    }
  }

  return HDMI_PICTURE_ASPECT_NONE;
}

/**
  Sets an InfoFrame's checksum: all bytes, header included, add up to 0.

  @param[in,out]  Frame   The InfoFrame; byte 3 is the checksum.
  @param[in]      Length  Its size, header included.
**/
STATIC
VOID
Lt9611InfoFrameChecksum (
  IN OUT UINT8  *Frame,
  IN     UINTN  Length
  )
{
  UINT8  Sum;
  UINTN  Index;

  Frame[3] = 0;
  Sum      = 0;
  for (Index = 0; Index < Length; Index++) {
    Sum = (UINT8)(Sum + Frame[Index]);
  }

  Frame[3] = (UINT8)(0x100 - Sum);
}

/**
  Writes the AVI InfoFrame and, for an HDMI sink, the HDMI vendor specific
  InfoFrame, and enables sending them.

  The content is what the Linux DRM helpers build for a mode without
  properties: RGB, underscan, the VIC's picture aspect ratio, active format
  = picture, graphics content, default quantization, no bars.

  @param[in]  Timing  The mode.
  @param[in]  Hdmi    TRUE for an HDMI sink.

  @retval EFI_SUCCESS  Done.
  @retval Other        An I2C transfer failed.
**/
STATIC
EFI_STATUS
Lt9611SetInfoFrames (
  IN CONST DISPLAY_TIMING  *Timing,
  IN BOOLEAN               Hdmi
  )
{
  EFI_STATUS  Status;
  UINT8       Avi[HDMI_INFOFRAME_HEADER_SIZE + HDMI_AVI_INFOFRAME_LENGTH];
  UINT8       Vendor[HDMI_INFOFRAME_HEADER_SIZE + HDMI_VENDOR_INFOFRAME_LENGTH];
  UINT8       Enable;

  ZeroMem (Avi, sizeof (Avi));
  Avi[0] = HDMI_INFOFRAME_TYPE_AVI;
  Avi[1] = HDMI_AVI_INFOFRAME_VERSION;
  Avi[2] = HDMI_AVI_INFOFRAME_LENGTH;
  //
  // PB1: RGB, active format present, underscan. PB2: picture and active
  // format aspect ratios. PB3 and PB5 stay 0. PB4: the VIC, of which a
  // version 2 AVI InfoFrame has room for 7 bits.
  //
  Avi[4] = HDMI_AVI_ACTIVE_INFO_VALID | HDMI_AVI_SCAN_UNDERSCAN;
  Avi[5] = (UINT8)((Lt9611VicPictureAspect (Timing->Vic) << 4) | HDMI_AVI_ACTIVE_ASPECT_PICTURE);
  Avi[7] = (Timing->Vic <= 127) ? Timing->Vic : 0;
  Lt9611InfoFrameChecksum (Avi, sizeof (Avi));

  Status = Lt9611WriteBuffer (LT9611_AVI_INFOFRAME, Avi, sizeof (Avi));
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Enable = LT9611_INFOFRAME_EN_AVI | LT9611_INFOFRAME_EN_AUDIO;

  //
  // Linux sends the HDMI vendor InfoFrame, empty (no HDMI VIC, no 3D) for
  // the CTA modes so that a sink leaves any 3D mode, to the HDMI sinks that
  // can take it: those whose HDMI VSDB has HDMI_Video_present (byte 8 bit 5),
  // as HDMI 1.4 sinks such as the board's reference monitor do. Only HDMI
  // or DVI is known here, so every HDMI sink gets it, as an HDMI 1.3 sink
  // is expected to skip an InfoFrame it does not support.
  //
  if (Hdmi) {
    ZeroMem (Vendor, sizeof (Vendor));
    Vendor[0] = HDMI_INFOFRAME_TYPE_VENDOR;
    Vendor[1] = HDMI_VENDOR_INFOFRAME_VERSION;
    Vendor[2] = HDMI_VENDOR_INFOFRAME_LENGTH;
    Vendor[4] = (UINT8)HDMI_IEEE_OUI;
    Vendor[5] = (UINT8)(HDMI_IEEE_OUI >> 8);
    Vendor[6] = (UINT8)(HDMI_IEEE_OUI >> 16);
    Vendor[7] = 0;                                  // HDMI_Video_Format: none
    Lt9611InfoFrameChecksum (Vendor, sizeof (Vendor));

    Status = Lt9611WriteBuffer (LT9611_HDMI_INFOFRAME, Vendor, sizeof (Vendor));
    if (EFI_ERROR (Status)) {
      return Status;
    }

    Enable |= LT9611_INFOFRAME_EN_HDMI;
  }

  Status = Lt9611RegWrite (LT9611_INFOFRAME_EN, Enable);
  DEBUG ((
    DEBUG_INFO,
    "%a: AVI %02x %02x %02x %02x %02x %02x %02x %02x, enable 0x%02x: %r\n",
    __func__,
    Avi[0],
    Avi[1],
    Avi[2],
    Avi[3],
    Avi[4],
    Avi[5],
    Avi[6],
    Avi[7],
    Enable,
    Status
    ));
  return Status;
}

/**
  Programs the LT9611 for a mode (MIPI input, PLL, timing, PCR, InfoFrame,
  TX PHY) and turns the HDMI TMDS output on.

  @param[in]  Timing  The mode.
  @param[in]  Hdmi    TRUE for an HDMI sink (AVI InfoFrame sent), FALSE for
                      DVI.

  @retval EFI_SUCCESS  Enabled.
  @retval Other        Failed.
**/
EFI_STATUS
Lt9611Enable (
  IN CONST DISPLAY_TIMING  *Timing,
  IN BOOLEAN               Hdmi
  )
{
  EFI_STATUS  Status;
  UINT32      PostDiv;
  UINT8       Value;

  PostDiv = 1;
  Value   = 0;
  if (Timing == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (!mBusOwned) {
    DEBUG ((DEBUG_ERROR, "%a: LT9611 not powered on\n", __func__));
    return EFI_NOT_READY;
  }

  Status = Lt9611CheckTiming (Timing);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  DEBUG ((
    DEBUG_INFO,
    "%a: %ux%u, %u kHz, htotal %u, vtotal %u, VIC %u, %a, MIPI port %a\n",
    __func__,
    Timing->HActive,
    Timing->VActive,
    Timing->PixelClockKhz,
    DISPLAY_H_TOTAL (Timing),
    DISPLAY_V_TOTAL (Timing),
    Timing->Vic,
    Hdmi ? "HDMI" : "DVI",
    mBoard.PortB ? "B" : "A"
    ));

  CopyMem (&mTiming, Timing, sizeof (mTiming));
  mTimingSet = TRUE;

  //
  // The Linux driver powers the MIPI receiver up before enabling when it
  // comes back from its sleep sequence. After a reset it is expected to be
  // on already; make sure of it.
  //
  Status = Lt9611RegRead (LT9611_MIPI_RX_POWER, &Value);
  if (!EFI_ERROR (Status)) {
    DEBUG ((DEBUG_INFO, "%a: MIPI RX power 0x%02x\n", __func__, Value));
    if (Value != LT9611_MIPI_RX_POWER_ON) {
      Status = Lt9611RegWrite (LT9611_MIPI_RX_POWER, LT9611_MIPI_RX_POWER_ON);
    }
  }

  if (EFI_ERROR (Status)) {
    goto Fail;
  }

  if (mBoard.PortB) {
    Status = Lt9611WriteSequence (mMipiDigitalPortB, ARRAY_SIZE (mMipiDigitalPortB));
  } else {
    Status = Lt9611WriteSequence (mMipiDigitalPortA, ARRAY_SIZE (mMipiDigitalPortA));
  }

  if (EFI_ERROR (Status)) {
    goto Fail;
  }

  Status = Lt9611TxPllSetup (Timing, &PostDiv);
  if (EFI_ERROR (Status)) {
    goto Fail;
  }

  Status = Lt9611VideoTimingSetup (Timing);
  if (EFI_ERROR (Status)) {
    goto Fail;
  }

  Status = Lt9611PcrSetup (Timing, PostDiv);
  if (EFI_ERROR (Status)) {
    goto Fail;
  }

  if (!mSystemInitDone) {
    Status = Lt9611SystemInit ();
    if (EFI_ERROR (Status)) {
      goto Fail;
    }
  }

  Status = Lt9611WriteSequence (mMipiAnalog, ARRAY_SIZE (mMipiAnalog));
  if (EFI_ERROR (Status)) {
    goto Fail;
  }

  Status = Lt9611SetInfoFrames (Timing, Hdmi);
  if (EFI_ERROR (Status)) {
    goto Fail;
  }

  Status = Lt9611RegWrite (LT9611_HDMI_MODE, Hdmi ? LT9611_HDMI_MODE_HDMI : LT9611_HDMI_MODE_DVI);
  if (!EFI_ERROR (Status)) {
    Status = Lt9611RegWrite (LT9611_HDMI_TX_CTRL, LT9611_HDMI_TX_CTRL_VALUE);
  }

  if (EFI_ERROR (Status)) {
    goto Fail;
  }

  Status = Lt9611WriteSequence (mHdmiTxPhy, ARRAY_SIZE (mHdmiTxPhy));
  if (EFI_ERROR (Status)) {
    goto Fail;
  }

  //
  // Let the PLL and the PCR settle before the output goes on, as Linux does.
  //
  MicroSecondDelay (LT9611_TX_SETTLE_US);
  Lt9611LogVideoCheck ();

  Status = Lt9611RegWrite (LT9611_HDMI_TX_PHY, LT9611_HDMI_TX_PHY_ON);
  if (EFI_ERROR (Status)) {
    goto Fail;
  }

  //
  // A few read-backs that show in the log whether the key settings stuck.
  //
  Lt9611LogReadBack (LT9611_MIPI_BYTE_CLK, mBoard.PortB ? LT9611_MIPI_BYTE_CLK_PORT_B : LT9611_MIPI_BYTE_CLK_PORT_A);
  Lt9611LogReadBack (LT9611_PCR_M, (UINT8)(Timing->PixelClockKhz * 5 * PostDiv / 27000));
  Lt9611LogReadBack (LT9611_HDMI_MODE, Hdmi ? LT9611_HDMI_MODE_HDMI : LT9611_HDMI_MODE_DVI);
  Lt9611LogReadBack (LT9611_HDMI_TX_PHY, LT9611_HDMI_TX_PHY_ON);

  DEBUG ((DEBUG_INFO, "%a: TMDS output on\n", __func__));
  return EFI_SUCCESS;

Fail:
  DEBUG ((DEBUG_ERROR, "%a: %r\n", __func__, Status));
  //
  // Best effort: leave the TMDS output off.
  //
  Lt9611RegWrite (LT9611_HDMI_TX_PHY, LT9611_HDMI_TX_PHY_OFF);
  return Status;
}

/**
  Logs the video timing the LT9611 detects on its MIPI input.
**/
VOID
Lt9611LogVideoCheck (
  VOID
  )
{
  EFI_STATUS  Status;
  UINT32      VActive;
  UINT32      VTotal;
  UINT32      HTotalSysClk;
  UINT32      HActiveA;
  UINT32      HActiveB;

  if (!mBusOwned) {
    DEBUG ((DEBUG_ERROR, "%a: LT9611 not powered on\n", __func__));
    return;
  }

  Status = Lt9611RegRead16 (LT9611_VIDCHK_VACTIVE, &VActive);
  if (!EFI_ERROR (Status)) {
    Status = Lt9611RegRead16 (LT9611_VIDCHK_VTOTAL, &VTotal);
  }

  if (!EFI_ERROR (Status)) {
    Status = Lt9611RegRead16 (LT9611_VIDCHK_HTOTAL, &HTotalSysClk);
  }

  if (!EFI_ERROR (Status)) {
    Status = Lt9611RegRead16 (LT9611_VIDCHK_HACTIVE_A, &HActiveA);
  }

  if (!EFI_ERROR (Status)) {
    Status = Lt9611RegRead16 (LT9611_VIDCHK_HACTIVE_B, &HActiveB);
  }

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: read failed: %r\n", __func__, Status));
    return;
  }

  //
  // The active width is counted in bytes, 3 per RGB888 pixel.
  //
  DEBUG ((
    DEBUG_INFO,
    "%a: hactive A %u, hactive B %u, vactive %u, vtotal %u, htotal %u system clocks\n",
    __func__,
    HActiveA / 3,
    HActiveB / 3,
    VActive,
    VTotal,
    HTotalSysClk
    ));

  if (mTimingSet) {
    DEBUG ((
      DEBUG_INFO,
      "%a: expected hactive %u on port %a, vactive %u, vtotal %u (all 0 until DSI video runs)\n",
      __func__,
      mTiming.HActive,
      mBoard.PortB ? "B" : "A",
      mTiming.VActive,
      DISPLAY_V_TOTAL (&mTiming)
      ));
  }
}

/**
  Turns the HDMI TMDS output off. Leaves the chip powered and out of reset,
  as the OS expects.
**/
VOID
Lt9611Disable (
  VOID
  )
{
  EFI_STATUS  Status;

  if (!mBusOwned) {
    DEBUG ((DEBUG_ERROR, "%a: LT9611 not powered on\n", __func__));
    return;
  }

  Status = Lt9611RegWrite (LT9611_HDMI_TX_PHY, LT9611_HDMI_TX_PHY_OFF);

  //
  // As in Linux, the system init counts as undone after this, so that an
  // enable writes it again.
  //
  mSystemInitDone = FALSE;
  mTimingSet      = FALSE;
  DEBUG ((DEBUG_INFO, "%a: TMDS output off: %r\n", __func__, Status));
}

/**
  Gives the I2C pins back to the QUP serial engine (restores their TLMM
  configuration). No LT9611 access is possible afterwards.
**/
VOID
Lt9611ReleaseBus (
  VOID
  )
{
  if (!mBusOwned) {
    return;
  }

  Lt9611I2cDeinit ();

  //
  // Both lines are released and high: switching the function back cannot
  // make a START or a STOP.
  //
  Lt9611TlmmRestorePin (&mSclEntry);
  Lt9611TlmmRestorePin (&mSdaEntry);
  mBusOwned       = FALSE;
  mSystemInitDone = FALSE;
  mTimingSet      = FALSE;
  mPage           = LT9611_PAGE_NONE;

  if ((LT9611_TLMM_CFG_FUNCTION (mSdaEntry.Cfg) != mBoard.I2cPinFunction) ||
      (LT9611_TLMM_CFG_FUNCTION (mSclEntry.Cfg) != mBoard.I2cPinFunction))
  {
    DEBUG ((
      DEBUG_WARN,
      "%a: the I2C pins had function %u/%u at entry, not %u; restored as found\n",
      __func__,
      LT9611_TLMM_CFG_FUNCTION (mSdaEntry.Cfg),
      LT9611_TLMM_CFG_FUNCTION (mSclEntry.Cfg),
      mBoard.I2cPinFunction
      ));
  }

  Lt9611TlmmLogPin ("I2C SDA (released)", mBoard.SdaGpio);
  Lt9611TlmmLogPin ("I2C SCL (released)", mBoard.SclGpio);
  Lt9611TlmmLogPin ("LT9611 power (left on)", mBoard.PowerGpio);
  Lt9611TlmmLogPin ("LT9611 reset (left high)", mBoard.ResetGpio);
}
