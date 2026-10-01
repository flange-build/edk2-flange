/** @file
  QCS6490 MDSS display driver: DPU -> DSI0 -> DSI PHY 7nm -> LT9611 -> HDMI.

  Interfaces between the parts of the driver. Each part owns its hardware
  block and undoes what it did in its Disable/Off function, in the reverse
  order of the enable sequence in MdssDisplayDxe.c.

  Copyright (c) 2026, edk2-flange contributors.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#ifndef MDSS_DISPLAY_H_
#define MDSS_DISPLAY_H_

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>

//
// Video timing. Porches and sync widths in pixels/lines, as in CEA-861.
//
typedef struct {
  UINT32     PixelClockKhz;
  UINT16     HActive;
  UINT16     HFrontPorch;
  UINT16     HSyncWidth;
  UINT16     HBackPorch;
  UINT16     VActive;
  UINT16     VFrontPorch;
  UINT16     VSyncWidth;
  UINT16     VBackPorch;
  BOOLEAN    HSyncPositive;
  BOOLEAN    VSyncPositive;
  UINT8      Vic;               // CEA-861 VIC for the AVI InfoFrame, 0 if none
} DISPLAY_TIMING;

#define DISPLAY_H_TOTAL(t)  ((UINT32)(t)->HActive + (t)->HFrontPorch + (t)->HSyncWidth + (t)->HBackPorch)
#define DISPLAY_V_TOTAL(t)  ((UINT32)(t)->VActive + (t)->VFrontPorch + (t)->VSyncWidth + (t)->VBackPorch)

//
// 1920x1080@60, CEA-861 VIC 16, 148.5 MHz. The only mode for now.
//
extern CONST DISPLAY_TIMING  gDisplayTiming1080p60;

//
// How the LT9611 is wired on the board (from the board's PCDs).
//
typedef struct {
  UINT8      I2cAddress;        // 7-bit
  UINT16     SdaGpio;
  UINT16     SclGpio;
  UINT8      I2cPinFunction;    // TLMM function the I2C pins have outside this driver
  UINT16     ResetGpio;         // active low reset, driven high to run
  UINT16     PowerGpio;         // 3.3 V enable, active high
  BOOLEAN    PortB;             // MIPI input on port B
} LT9611_BOARD_CONFIG;

//
// Helpers (MdssDisplayDxe.c)
//

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
  );

//
// Power, clocks and diagnostics (Power.c, Rpmh.c, Diag.c)
//

/**
  Logs, without writing anything, the state the display hardware is in when
  the driver starts: GCC/DISPCC clocks and GDSC, REFGEN, the LT9611 GPIOs, the
  RSC, and the MDSS/DSI version registers if their clocks are on.
**/
VOID
DisplayLogEntryState (
  VOID
  );

/**
  Powers the display subsystem up to the point where MDSS, DPU and DSI0
  registers can be accessed and the MDP core runs: RPMh votes (DSI rails in
  high power mode, MMNOC bandwidth), REFGEN, GCC display clocks, the MDSS
  GDSC and the DISPCC AHB/MDP/LUT/VSYNC/ESC0 clocks. Checks the DPU hardware
  version.

  @retval EFI_SUCCESS  Powered up.
  @retval Other        Failed; whatever was done has been undone.
**/
EFI_STATUS
DisplayPowerOn (
  VOID
  );

/**
  Undoes DisplayPowerOn, restoring what it changed to the values found at
  entry, and leaves the RSC TCS it used idle and clean for the OS. Must run
  after DsiPhyDisable.
**/
VOID
DisplayPowerOff (
  VOID
  );

//
// DSI PHY 7nm, its PLL and the DSI link clocks (DsiPhy.c)
//

/**
  Resets and programs the DSI0 PHY, locks its PLL for the timing's pixel
  clock (4 lanes, RGB888) and switches the DISPCC BYTE0/BYTE0_INTF/PCLK0
  clocks onto it.

  @param[in]  Timing  The mode.

  @retval EFI_SUCCESS      The PLL locked and the link clocks run.
  @retval EFI_UNSUPPORTED  The pixel clock is not supported.
  @retval Other            Failed; whatever was done has been undone.
**/
EFI_STATUS
DsiPhyEnable (
  IN CONST DISPLAY_TIMING  *Timing
  );

/**
  Undoes DsiPhyEnable: parks the link clocks on XO before stopping the PLL,
  then powers the PHY down. Must run after DsiHostDisable.
**/
VOID
DsiPhyDisable (
  VOID
  );

//
// DSI0 host controller (Dsi.c)
//

/**
  Programs the DSI0 controller for video mode (non-burst, sync pulses,
  RGB888, 4 lanes, continuous HS clock) and enables it.

  @param[in]  Timing  The mode.

  @retval EFI_SUCCESS  Enabled.
  @retval Other        Failed; whatever was done has been undone.
**/
EFI_STATUS
DsiHostEnable (
  IN CONST DISPLAY_TIMING  *Timing
  );

/**
  Undoes DsiHostEnable.
**/
VOID
DsiHostDisable (
  VOID
  );

//
// DPU: VBIF, SSPP, LM, CTL and INTF_1 (Dpu.c)
//

/**
  Sets up the DPU to scan out a linear XRGB8888 framebuffer to INTF_1 (DSI0),
  without starting the timing engine.

  @param[in]  Timing           The mode.
  @param[in]  FrameBufferBase  The framebuffer, below 4 GiB.
  @param[in]  StrideBytes      Bytes per line.

  @retval EFI_SUCCESS  Set up.
  @retval Other        Failed.
**/
EFI_STATUS
DpuSetup (
  IN CONST DISPLAY_TIMING    *Timing,
  IN EFI_PHYSICAL_ADDRESS    FrameBufferBase,
  IN UINT32                  StrideBytes
  );

/**
  Flushes the configuration and starts the INTF_1 timing engine, then checks
  that frames are being produced.

  @retval EFI_SUCCESS  The frame counter advances.
  @retval Other        It does not; scan-out has been stopped.
**/
EFI_STATUS
DpuStart (
  VOID
  );

/**
  Stops scan-out: timing engine off, then, after the current frame, no pipe
  fetching and no layer staged. Leaves the DPU idle for the OS.
**/
VOID
DpuStop (
  VOID
  );

//
// LT9611 MIPI DSI to HDMI bridge, over bit-banged I2C (Tlmm.c, GpioI2c.c,
// Lt9611.c)
//

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
  );

/**
  Returns whether a sink is connected (HDMI hot plug detect).
**/
BOOLEAN
Lt9611IsHotPlugged (
  VOID
  );

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
  );

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
  );

/**
  Logs the video timing the LT9611 detects on its MIPI input.
**/
VOID
Lt9611LogVideoCheck (
  VOID
  );

/**
  Turns the HDMI TMDS output off. Leaves the chip powered and out of reset,
  as the OS expects.
**/
VOID
Lt9611Disable (
  VOID
  );

/**
  Gives the I2C pins back to the QUP serial engine (restores their TLMM
  configuration). No LT9611 access is possible afterwards.
**/
VOID
Lt9611ReleaseBus (
  VOID
  );

//
// EDID (Edid.c)
//

/**
  Logs an EDID and returns whether the sink is HDMI (has an HDMI vendor
  specific data block) rather than DVI.

  @param[in]  Edid      The EDID.
  @param[in]  EdidSize  Its size.

  @return  TRUE for an HDMI sink.
**/
BOOLEAN
EdidParse (
  IN CONST UINT8  *Edid,
  IN UINTN        EdidSize
  );

#endif // MDSS_DISPLAY_H_
