/** @file
  DSI0 host controller (DSI 6G v2.5) in video mode, for the LT9611.

  The LT9611 takes 4 lanes of RGB888 video, non-burst with sync pulses, HSE
  packets, EoT packets and a continuous HS clock, and needs no DSI commands.
  The sequence follows the vendor Linux kernel of the board (6.6.90,
  drivers/gpu/drm/msm/dsi/dsi_host.c): msm_dsi_host_power_on (timing,
  software reset, controller configuration and enable) and
  msm_dsi_host_enable (video mode on), and the reverse at disable. The values
  it writes are the ones a register dump of that kernel shows while it scans
  out 1920x1080@60 over this path.

  The byte, pixel and byte interface clocks come from the DSI0 PHY PLL
  (DsiPhyEnable) and the escape and AHB clocks from DisplayPowerOn; the
  controller cannot be reset without them.

  Copyright (c) 2026, edk2-flange contributors.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include "MdssDisplay.h"
#include "Dsi.h"

//
// Registers read without the 6G shift, at DSI0_CTRL_BASE (dsi_get_version).
//
#define DSI_6G_HW_VERSION            0x000
#define DSI_6G_HW_VERSION_V2_5_0     0x20050000   // MSM_DSI_6G_VER_MINOR_V2_5_0, sc7280
#define DSI_VERSION_UNSHIFTED        0x1F0        // a scratch register on DSI 6G: reads 0

//
// Registers in the shifted window, DSI0_REG() (dsi.xml.h).
//
#define DSI_CTRL                     0x000
#define DSI_CTRL_ENABLE              BIT0
#define DSI_CTRL_VID_MODE_EN         BIT1
#define DSI_CTRL_CMD_MODE_EN         BIT2
#define DSI_CTRL_LANE0               BIT4
#define DSI_CTRL_CLK_EN              BIT8
#define DSI_STATUS0                  0x004
#define DSI_FIFO_STATUS              0x008
#define DSI_VID_CFG0                 0x00C
#define DSI_VID_CFG0_DST_FORMAT(x)   ((x) << 4)
#define DSI_VID_CFG0_TRAFFIC_MODE(x) ((x) << 8)
#define DSI_VID_CFG0_BLLP_POWER_STOP      BIT12
#define DSI_VID_CFG0_EOF_BLLP_POWER_STOP  BIT15
#define DSI_VID_CFG0_PULSE_MODE_HSA_HE    BIT28
#define DSI_VID_DST_FORMAT_RGB888    3
#define DSI_NON_BURST_SYNCH_PULSE    0
#define DSI_VID_CFG1                 0x01C
#define DSI_ACTIVE_H                 0x020
#define DSI_ACTIVE_V                 0x024
#define DSI_TOTAL                    0x028
#define DSI_ACTIVE_HSYNC             0x02C
#define DSI_ACTIVE_VSYNC_HPOS        0x030
#define DSI_ACTIVE_VSYNC_VPOS        0x034
#define DSI_CMD_DMA_CTRL             0x038
#define DSI_CMD_DMA_CTRL_LOW_POWER          BIT26
#define DSI_CMD_DMA_CTRL_FROM_FRAME_BUFFER  BIT28
#define DSI_TRIG_CTRL                0x080
#define DSI_TRIG_CTRL_DMA_TRIGGER(x) (x)
#define DSI_TRIG_CTRL_MDP_TRIGGER(x) ((x) << 4)
#define DSI_TRIG_CTRL_STREAM(x)      ((x) << 8)
#define DSI_TRIG_CTRL_BLOCK_DMA_WITHIN_FRAME  BIT12
#define DSI_TRIG_CTRL_TE             BIT31
#define DSI_TRIGGER_NONE             0
#define DSI_TRIGGER_SW               4
#define DSI_LANE_STATUS              0x0A4
#define DSI_LANE_STATUS_CLKLN_STOPSTATE     BIT4
#define DSI_LANE_CTRL                0x0A8
#define DSI_LANE_CTRL_CLKLN_HS_FORCE_REQUEST  BIT28
#define DSI_LANE_SWAP_CTRL           0x0AC
#define DSI_LANE_SWAP_0123           0
#define DSI_CLKOUT_TIMING_CTRL       0x0C0
#define DSI_CLKOUT_TIMING_CTRL_T_CLK_PRE(x)   ((x) & 0x3F)
#define DSI_CLKOUT_TIMING_CTRL_T_CLK_POST(x)  (((x) & 0x3F) << 8)
#define DSI_EOT_PACKET_CTRL          0x0C8
#define DSI_EOT_PACKET_CTRL_TX_EOT_APPEND     BIT0
#define DSI_ERR_INT_MASK0            0x108
#define DSI_ERR_INT_MASK0_VALUE      0x13FF3FE0   // Linux: only ack errors unmasked
#define DSI_RESET                    0x114
#define DSI_CLK_CTRL                 0x118
#define DSI_CLK_CTRL_ENABLE_CLKS     0x23F        // AHBS, AHBM, PCLK, DSICLK, BYTECLK, ESCCLK, FORCE_ON_DYN_AHBM
#define DSI_CLK_STATUS               0x11C
#define DSI_CLK_STATUS_AON_BYTECLK_ACTIVE     BIT6
#define DSI_CLK_STATUS_AON_ESCCLK_ACTIVE      BIT8
#define DSI_CLK_STATUS_AON_PCLK_ACTIVE        BIT9
#define DSI_CLK_STATUS_PLL_UNLOCKED           BIT16
#define DSI_PHY_RESET                0x128
#define DSI_PHY_RESET_RESET          BIT0

//
// Virtual channel 0; data-lanes <0 1 2 3>, no swap.
//
#define DSI_VIRTUAL_CHANNEL          0

//
// Delays and timeouts (dsi_host.c).
//
#define DSI_PHY_RESET_ASSERT_US      1000
#define DSI_PHY_RESET_SETTLE_US      100
#define DSI_RESET_TOGGLE_US          20000        // DSI_RESET_TOGGLE_DELAY_MS
#define DSI_CLKLN_HS_TIMEOUT_US      1000

STATIC BOOLEAN  mDsiHostEnabled;

/**
  Checks that the DSI0 controller answers as a DSI 6G v2.5, the way the Linux
  driver identifies it. Reading it needs the MDSS GDSC and AHB clock.

  @retval EFI_SUCCESS       It does.
  @retval EFI_DEVICE_ERROR  It does not.
**/
STATIC
EFI_STATUS
DsiHostCheckVersion (
  VOID
  )
{
  UINT32  HwVersion;
  UINT32  Unshifted;

  HwVersion = MmioRead32 (DSI0_CTRL_BASE + DSI_6G_HW_VERSION);
  Unshifted = MmioRead32 (DSI0_CTRL_BASE + DSI_VERSION_UNSHIFTED);
  if ((HwVersion != DSI_6G_HW_VERSION_V2_5_0) || (Unshifted != 0)) {
    DEBUG ((
      DEBUG_ERROR,
      "%a: DSI0 6G_HW_VERSION 0x%x (want 0x%x), unshifted VERSION 0x%x (want 0)\n",
      __func__,
      HwVersion,
      DSI_6G_HW_VERSION_V2_5_0,
      Unshifted
      ));
    return EFI_DEVICE_ERROR;
  }

  return EFI_SUCCESS;
}

/**
  Resets the controller's state machines (VK dsi_sw_reset). The controller
  is disabled first and, unlike in Linux, not re-enabled afterwards: both
  callers want it off. The configuration registers keep their values.
**/
STATIC
VOID
DsiHostSoftReset (
  VOID
  )
{
  UINT32  Ctrl;

  Ctrl = MmioRead32 (DSI0_REG (DSI_CTRL));
  if ((Ctrl & DSI_CTRL_ENABLE) != 0) {
    MmioWrite32 (DSI0_REG (DSI_CTRL), Ctrl & ~(UINT32)DSI_CTRL_ENABLE);
  }

  //
  // The reset only takes effect with the controller clocks running.
  //
  MmioWrite32 (DSI0_REG (DSI_CLK_CTRL), DSI_CLK_CTRL_ENABLE_CLKS);
  MmioWrite32 (DSI0_REG (DSI_RESET), 1);
  MicroSecondDelay (DSI_RESET_TOGGLE_US);
  MmioWrite32 (DSI0_REG (DSI_RESET), 0);
}

/**
  Checks that the DSI0 controller answers as the expected DSI 6G v2.5, then
  resets the DSI0 PHY through its DSI_PHY_RESET register, as the Linux DSI
  manager does before it enables the PHY.

  @retval EFI_SUCCESS       The PHY has been reset.
  @retval EFI_DEVICE_ERROR  The controller is not the expected one; nothing
                            was written.
**/
EFI_STATUS
DsiHostResetPhy (
  VOID
  )
{
  EFI_STATUS  Status;
  UINT32      Ctrl;

  Status = DsiHostCheckVersion ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // The controller must not drive the lanes while the PHY is reset. Nothing
  // before this driver runs the display, so this only catches a previous
  // owner that did not clean up.
  //
  Ctrl = MmioRead32 (DSI0_REG (DSI_CTRL));
  if ((Ctrl & DSI_CTRL_ENABLE) != 0) {
    DEBUG ((DEBUG_WARN, "%a: DSI0 enabled at entry (CTRL 0x%x), disabling\n", __func__, Ctrl));
    MmioWrite32 (DSI0_REG (DSI_CTRL), 0);
  }

  MmioWrite32 (DSI0_REG (DSI_PHY_RESET), DSI_PHY_RESET_RESET);
  MicroSecondDelay (DSI_PHY_RESET_ASSERT_US);
  MmioWrite32 (DSI0_REG (DSI_PHY_RESET), 0);
  MicroSecondDelay (DSI_PHY_RESET_SETTLE_US);

  DEBUG ((DEBUG_INFO, "%a: DSI0 6G v2.5, PHY reset\n", __func__));
  return EFI_SUCCESS;
}

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
  )
{
  EFI_STATUS              Status;
  DSI_PHY_SHARED_TIMINGS  Shared;
  UINT32                  HTotal;
  UINT32                  VTotal;
  UINT32                  HSyncStart;
  UINT32                  VSyncStart;
  UINT32                  HActiveStart;
  UINT32                  VActiveStart;
  UINT32                  ClkStatus;
  UINT32                  LaneCtrl;
  UINT32                  Ctrl;

  if (mDsiHostEnabled) {
    DEBUG ((DEBUG_ERROR, "%a: already enabled\n", __func__));
    return EFI_ALREADY_STARTED;
  }

  Status = DsiPhyGetSharedTimings (Timing, &Shared);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: no PHY timing for a %u kHz pixel clock\n", __func__, Timing->PixelClockKhz));
    return Status;
  }

  HTotal = DISPLAY_H_TOTAL (Timing);
  VTotal = DISPLAY_V_TOTAL (Timing);
  if ((Timing->HActive == 0) || (Timing->VActive == 0) || (HTotal > 0xFFFF) || (VTotal > 0xFFFF)) {
    DEBUG ((DEBUG_ERROR, "%a: bad timing %ux%u, total %ux%u\n", __func__, Timing->HActive, Timing->VActive, HTotal, VTotal));
    return EFI_INVALID_PARAMETER;
  }

  Status = DsiHostCheckVersion ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // Timing (dsi_timing_setup): positions counted from the start of the sync
  // pulse, so the active area starts after sync + back porch.
  //
  HSyncStart   = Timing->HActive + Timing->HFrontPorch;
  VSyncStart   = Timing->VActive + Timing->VFrontPorch;
  HActiveStart = HTotal - HSyncStart;
  VActiveStart = VTotal - VSyncStart;

  MmioWrite32 (DSI0_REG (DSI_ACTIVE_H), ((HActiveStart + Timing->HActive) << 16) | HActiveStart);
  MmioWrite32 (DSI0_REG (DSI_ACTIVE_V), ((VActiveStart + Timing->VActive) << 16) | VActiveStart);
  MmioWrite32 (DSI0_REG (DSI_TOTAL), ((VTotal - 1) << 16) | (HTotal - 1));
  MmioWrite32 (DSI0_REG (DSI_ACTIVE_HSYNC), (UINT32)Timing->HSyncWidth << 16);
  MmioWrite32 (DSI0_REG (DSI_ACTIVE_VSYNC_HPOS), 0);
  MmioWrite32 (DSI0_REG (DSI_ACTIVE_VSYNC_VPOS), (UINT32)Timing->VSyncWidth << 16);

  DEBUG ((
    DEBUG_INFO,
    "%a: ACTIVE_H 0x%x ACTIVE_V 0x%x TOTAL 0x%x ACTIVE_HSYNC 0x%x ACTIVE_VSYNC_VPOS 0x%x\n",
    __func__,
    MmioRead32 (DSI0_REG (DSI_ACTIVE_H)),
    MmioRead32 (DSI0_REG (DSI_ACTIVE_V)),
    MmioRead32 (DSI0_REG (DSI_TOTAL)),
    MmioRead32 (DSI0_REG (DSI_ACTIVE_HSYNC)),
    MmioRead32 (DSI0_REG (DSI_ACTIVE_VSYNC_VPOS))
    ));

  DsiHostSoftReset ();

  //
  // With the controller clocks forced on, the controller sees the link and
  // escape clocks, and the PLL lock the PHY reports.
  //
  ClkStatus = MmioRead32 (DSI0_REG (DSI_CLK_STATUS));
  DEBUG ((DEBUG_INFO, "%a: reset done, CLK_STATUS 0x%x\n", __func__, ClkStatus));
  if (((ClkStatus & (DSI_CLK_STATUS_AON_BYTECLK_ACTIVE | DSI_CLK_STATUS_AON_ESCCLK_ACTIVE |
                     DSI_CLK_STATUS_AON_PCLK_ACTIVE)) !=
       (DSI_CLK_STATUS_AON_BYTECLK_ACTIVE | DSI_CLK_STATUS_AON_ESCCLK_ACTIVE | DSI_CLK_STATUS_AON_PCLK_ACTIVE)) ||
      ((ClkStatus & DSI_CLK_STATUS_PLL_UNLOCKED) != 0))
  {
    DEBUG ((DEBUG_WARN, "%a: a link clock looks inactive or the PLL unlocked\n", __func__));
  }

  //
  // Video mode configuration (dsi_ctrl_enable). HSE: sync end packets as
  // well as sync start. The blanking between lines and frames goes to LP.
  //
  MmioWrite32 (
    DSI0_REG (DSI_VID_CFG0),
    DSI_VID_CFG0_PULSE_MODE_HSA_HE |
    DSI_VID_CFG0_EOF_BLLP_POWER_STOP |
    DSI_VID_CFG0_BLLP_POWER_STOP |
    DSI_VID_CFG0_TRAFFIC_MODE (DSI_NON_BURST_SYNCH_PULSE) |
    DSI_VID_CFG0_DST_FORMAT (DSI_VID_DST_FORMAT_RGB888) |
    DSI_VIRTUAL_CHANNEL
    );
  MmioWrite32 (DSI0_REG (DSI_VID_CFG1), 0);

  //
  // Command DMA and trigger setup, unused in video mode but written by Linux
  // for every mode.
  //
  MmioWrite32 (DSI0_REG (DSI_CMD_DMA_CTRL), DSI_CMD_DMA_CTRL_FROM_FRAME_BUFFER | DSI_CMD_DMA_CTRL_LOW_POWER);
  MmioWrite32 (
    DSI0_REG (DSI_TRIG_CTRL),
    DSI_TRIG_CTRL_TE |
    DSI_TRIG_CTRL_BLOCK_DMA_WITHIN_FRAME |
    DSI_TRIG_CTRL_STREAM (DSI_VIRTUAL_CHANNEL) |
    DSI_TRIG_CTRL_MDP_TRIGGER (DSI_TRIGGER_NONE) |
    DSI_TRIG_CTRL_DMA_TRIGGER (DSI_TRIGGER_SW)
    );

  //
  // Clock lane timing around HS data bursts, the PHY's T_CLK_POST/T_CLK_PRE.
  //
  MmioWrite32 (
    DSI0_REG (DSI_CLKOUT_TIMING_CTRL),
    DSI_CLKOUT_TIMING_CTRL_T_CLK_POST (Shared.ClkPost) | DSI_CLKOUT_TIMING_CTRL_T_CLK_PRE (Shared.ClkPre)
    );

  MmioWrite32 (DSI0_REG (DSI_EOT_PACKET_CTRL), DSI_EOT_PACKET_CTRL_TX_EOT_APPEND);

  //
  // Error interrupt mask as Linux sets it. The controller's interrupts stay
  // disabled (INTR_CTRL): UEFI polls, and the OS enables its own.
  //
  MmioWrite32 (DSI0_REG (DSI_ERR_INT_MASK0), DSI_ERR_INT_MASK0_VALUE);

  MmioWrite32 (DSI0_REG (DSI_CLK_CTRL), DSI_CLK_CTRL_ENABLE_CLKS);
  MmioWrite32 (DSI0_REG (DSI_LANE_SWAP_CTRL), DSI_LANE_SWAP_0123);

  //
  // Continuous HS clock: force the clock lane into HS. The sc7280 PHY has no
  // continuous clock control of its own in the vendor kernel, so
  // HS_REQ_SEL_PHY keeps its value.
  //
  LaneCtrl = MmioRead32 (DSI0_REG (DSI_LANE_CTRL));
  MmioWrite32 (DSI0_REG (DSI_LANE_CTRL), LaneCtrl | DSI_LANE_CTRL_CLKLN_HS_FORCE_REQUEST);

  Ctrl = DSI_CTRL_CLK_EN | ((DSI_CTRL_LANE0 << DSI_LANE_COUNT) - DSI_CTRL_LANE0) | DSI_CTRL_ENABLE;
  MmioWrite32 (DSI0_REG (DSI_CTRL), Ctrl);

  //
  // The clock lane leaves its LP-11 stop state once it runs in HS: the
  // LT9611 now has its MIPI byte clock.
  //
  Status = MmioPoll32 (DSI0_REG (DSI_LANE_STATUS), DSI_LANE_STATUS_CLKLN_STOPSTATE, 0, DSI_CLKLN_HS_TIMEOUT_US);
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_WARN,
      "%a: clock lane still in stop state (LANE_STATUS 0x%x)\n",
      __func__,
      MmioRead32 (DSI0_REG (DSI_LANE_STATUS))
      ));
  }

  //
  // Video mode on (msm_dsi_host_enable). The video engine waits for the
  // INTF_1 timing engine.
  //
  MmioWrite32 (DSI0_REG (DSI_CTRL), Ctrl | DSI_CTRL_VID_MODE_EN);
  mDsiHostEnabled = TRUE;

  DEBUG ((
    DEBUG_INFO,
    "%a: CTRL 0x%x VID_CFG0 0x%x LANE_CTRL 0x%x (was 0x%x) CLKOUT_TIMING 0x%x, "
    "STATUS0 0x%x FIFO_STATUS 0x%x LANE_STATUS 0x%x CLK_STATUS 0x%x\n",
    __func__,
    MmioRead32 (DSI0_REG (DSI_CTRL)),
    MmioRead32 (DSI0_REG (DSI_VID_CFG0)),
    MmioRead32 (DSI0_REG (DSI_LANE_CTRL)),
    LaneCtrl,
    MmioRead32 (DSI0_REG (DSI_CLKOUT_TIMING_CTRL)),
    MmioRead32 (DSI0_REG (DSI_STATUS0)),
    MmioRead32 (DSI0_REG (DSI_FIFO_STATUS)),
    MmioRead32 (DSI0_REG (DSI_LANE_STATUS)),
    MmioRead32 (DSI0_REG (DSI_CLK_STATUS))
    ));

  return EFI_SUCCESS;
}

/**
  Undoes DsiHostEnable.
**/
VOID
DsiHostDisable (
  VOID
  )
{
  if (!mDsiHostEnabled) {
    return;
  }

  DEBUG ((
    DEBUG_INFO,
    "%a: STATUS0 0x%x FIFO_STATUS 0x%x LANE_STATUS 0x%x CLK_STATUS 0x%x\n",
    __func__,
    MmioRead32 (DSI0_REG (DSI_STATUS0)),
    MmioRead32 (DSI0_REG (DSI_FIFO_STATUS)),
    MmioRead32 (DSI0_REG (DSI_LANE_STATUS)),
    MmioRead32 (DSI0_REG (DSI_CLK_STATUS))
    ));

  //
  // Video mode off (msm_dsi_host_disable).
  //
  MmioAnd32 (
    DSI0_REG (DSI_CTRL),
    ~(UINT32)(DSI_CTRL_ENABLE | DSI_CTRL_VID_MODE_EN | DSI_CTRL_CMD_MODE_EN)
    );

  //
  // Release the HS clock request so that the clock lane returns to LP-11
  // before the PHY goes down. Linux leaves it set, and sets it again on its
  // own enable.
  //
  MmioAnd32 (DSI0_REG (DSI_LANE_CTRL), ~(UINT32)DSI_LANE_CTRL_CLKLN_HS_FORCE_REQUEST);

  //
  // The video engine does not stop by itself once INTF_1 has; Linux resets
  // the controller here, while the link clocks still run.
  //
  DsiHostSoftReset ();

  //
  // Controller off (dsi_ctrl_disable in msm_dsi_host_power_off).
  //
  MmioWrite32 (DSI0_REG (DSI_CTRL), 0);
  mDsiHostEnabled = FALSE;

  DEBUG ((
    DEBUG_INFO,
    "%a: CTRL 0x%x LANE_CTRL 0x%x LANE_STATUS 0x%x\n",
    __func__,
    MmioRead32 (DSI0_REG (DSI_CTRL)),
    MmioRead32 (DSI0_REG (DSI_LANE_CTRL)),
    MmioRead32 (DSI0_REG (DSI_LANE_STATUS))
    ));
}
