/** @file
  DSI0 PHY (7nm, the sc7280 "V4.1" revision), its PLL, and the DISPCC link
  clocks (BYTE0, BYTE0_INTF, PCLK0) that run from the PLL.

  The sequence follows the vendor Linux kernel of the board (6.6.90,
  drivers/gpu/drm/msm/dsi/phy/dsi_phy_7nm.c and dsi_phy.c) in the order its
  DSI manager runs it: PHY reset by the DSI host, PHY enable (common block,
  D-PHY timing, lanes), PLL configuration (the "restore state" and set_rate
  paths), then, when the DSI host turns its link clocks on, PLL start and
  lock followed by the DISPCC byte and pixel clocks. The values it writes are
  the ones a register dump of that kernel shows while it scans out
  1920x1080@60 over this path.

  Where mainline Linux differs (PLL_CLOCK_INVERTERS_1 instead of
  PLL_CLOCK_INVERTERS, RBUF_CTRL = 1 after lock, PLL bias around set_rate,
  continuous clock in the PHY), the vendor kernel is followed, because the
  dump shows its values.

  Copyright (c) 2026, edk2-flange contributors.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include "MdssDisplay.h"
#include "Dsi.h"

//
// DSI0 PHY register blocks (mdss_dsi0_phy in sc7280.dtsi).
//
#define DSI0_PHY_CMN_BASE   0x0AE94400
#define DSI0_PHY_LANE_BASE  0x0AE94600
#define DSI0_PHY_PLL_BASE   0x0AE94900

//
// PHY common block (dsi_phy_7nm.xml.h).
//
#define PHY_CMN_REVISION_ID0                   0x000
#define PHY_CMN_CLK_CFG0                       0x010
#define PHY_CMN_CLK_CFG0_BIT_DIV_MASK          0x0F
#define PHY_CMN_CLK_CFG0_PIX_DIV_SHIFT         4
#define PHY_CMN_CLK_CFG1                       0x014
#define PHY_CMN_CLK_CFG1_DSICLK_SEL_MASK       0x03
#define PHY_CMN_CLK_CFG1_CLK_EN_SEL            BIT4
#define PHY_CMN_CLK_CFG1_CLK_EN                BIT5
#define PHY_CMN_RBUF_CTRL                      0x01C
#define PHY_CMN_VREG_CTRL_0                    0x020
#define PHY_CMN_CTRL_0                         0x024
#define PHY_CMN_CTRL_0_LANES_MASK              0x1F
#define PHY_CMN_CTRL_0_PLL_BIAS                BIT5
#define PHY_CMN_CTRL_0_DIGTOP_PLL_PWRDN_B      (BIT6 | BIT5)
#define PHY_CMN_CTRL_0_ALL_ON                  0x7F
#define PHY_CMN_CTRL_2                         0x02C
#define PHY_CMN_CTRL_2_FULL_RATE               0x40
#define PHY_CMN_CTRL_3                         0x030
#define PHY_CMN_CTRL_3_GLOBAL_CLK_EN           0x04
#define PHY_CMN_LANE_CFG0                      0x034
#define PHY_CMN_LANE_CFG1                      0x038
#define PHY_CMN_PLL_CNTRL                      0x03C
#define PHY_CMN_PLL_CNTRL_START                BIT0
#define PHY_CMN_LANE_CTRL0                     0x0A0
#define PHY_CMN_LANE_CTRL0_ALL_LANES           0x1F
#define PHY_CMN_TIMING_CTRL(n)                 (0x0B4 + 4 * (n))
#define PHY_CMN_GLBL_HSTX_STR_CTRL_0           0x0EC
#define PHY_CMN_GLBL_RESCODE_OFFSET_TOP_CTRL   0x0F4
#define PHY_CMN_GLBL_RESCODE_OFFSET_BOT_CTRL   0x0F8
#define PHY_CMN_GLBL_LPTX_STR_CTRL             0x100
#define PHY_CMN_GLBL_PEMPH_CTRL_0              0x104
#define PHY_CMN_GLBL_STR_SWI_CAL_SEL_CTRL      0x10C
#define PHY_CMN_VREG_CTRL_1                    0x110
#define PHY_CMN_CTRL_4                         0x114
#define PHY_CMN_GLBL_DIGTOP_SPARE4             0x128
#define PHY_CMN_PHY_STATUS                     0x140
#define PHY_CMN_PHY_STATUS_REFGEN_READY        BIT0

//
// PHY lanes: lanes 0-3 carry data, lane 4 is the clock lane.
//
#define PHY_LANE_COUNT       5
#define PHY_CLOCK_LANE       4
#define PHY_LN_CFG0(n)       (0x00 + 0x80 * (n))
#define PHY_LN_CFG1(n)       (0x04 + 0x80 * (n))
#define PHY_LN_CFG2(n)       (0x08 + 0x80 * (n))
#define PHY_LN_PIN_SWAP(n)   (0x10 + 0x80 * (n))
#define PHY_LN_LPRX_CTRL(n)  (0x14 + 0x80 * (n))
#define PHY_LN_TX_DCTRL(n)   (0x18 + 0x80 * (n))

//
// TX_DCTRL per lane for V4.1 and later (tx_dctrl_1 in VK).
//
STATIC CONST UINT8  mDsiPhyLaneTxDctrl[PHY_LANE_COUNT] = { 0x40, 0x40, 0x40, 0x46, 0x41 };

//
// PLL.
//
#define PLL_ANALOG_CONTROLS_TWO          0x004
#define PLL_ANALOG_CONTROLS_THREE        0x010
#define PLL_ANALOG_CONTROLS_FIVE         0x018
#define PLL_DSM_DIVIDER                  0x020
#define PLL_FEEDBACK_DIVIDER             0x024
#define PLL_SYSTEM_MUXES                 0x028
#define PLL_SYSTEM_MUXES_BIAS_ON         0xC0
#define PLL_CALIBRATION_SETTINGS         0x044
#define PLL_BAND_SEL_CAL_SETTINGS_THREE  0x068
#define PLL_FREQ_DETECT_SETTINGS_ONE     0x078
#define PLL_PFILT                        0x090
#define PLL_IFILT                        0x094
#define PLL_OUTDIV                       0x0A8
#define PLL_CORE_OVERRIDE                0x0B8
#define PLL_CORE_INPUT_OVERRIDE          0x0BC
#define PLL_PLL_DIGITAL_TIMERS_TWO       0x0C8
#define PLL_DECIMAL_DIV_START_1          0x0E0
#define PLL_FRAC_DIV_START_LOW_1         0x0E4
#define PLL_FRAC_DIV_START_MID_1         0x0E8
#define PLL_FRAC_DIV_START_HIGH_1        0x0EC
#define PLL_PLL_OUTDIV_RATE              0x154
#define PLL_PLL_OUTDIV_RATE_MASK         0x03
#define PLL_PLL_LOCKDET_RATE_1           0x158
#define PLL_PLL_PROP_GAIN_RATE_1         0x160
#define PLL_PLL_BAND_SEL_RATE_1          0x168
#define PLL_PLL_INT_GAIN_IFILT_BAND_1    0x170
#define PLL_PLL_FL_INT_GAIN_PFILT_BAND_1 0x178
#define PLL_PLL_LOCK_OVERRIDE            0x190
#define PLL_PLL_LOCK_DELAY               0x194
#define PLL_CLOCK_INVERTERS              0x19C
#define PLL_COMMON_STATUS_ONE            0x1B0
#define PLL_COMMON_STATUS_ONE_LOCK       BIT0
#define PLL_COMMON_STATUS_TWO            0x1B4
#define PLL_VCO_CONFIG_1                 0x240
#define PLL_CMODE_1                      0x250
#define PLL_CMODE_1_DPHY                 0x10
#define PLL_ANALOG_CONTROLS_FIVE_1       0x258
#define PLL_PERF_OPTIMIZE                0x260

#define PHY_CMN(Offset)   (DSI0_PHY_CMN_BASE + (Offset))
#define PHY_LANE(Offset)  (DSI0_PHY_LANE_BASE + (Offset))
#define PHY_PLL(Offset)   (DSI0_PHY_PLL_BASE + (Offset))

//
// DISPCC (dispcc-sc7280.c). The byte clock RCG and the pixel clock RCG both
// take the DSI0 PHY PLL outputs as source 1 and XO as source 0.
//
#define DISPCC_BASE                     0x0AF00000
#define DISP_CC_MDSS_PCLK0_CBCR         (DISPCC_BASE + 0x1010)
#define DISP_CC_MDSS_BYTE0_CBCR         (DISPCC_BASE + 0x1030)
#define DISP_CC_MDSS_BYTE0_INTF_CBCR    (DISPCC_BASE + 0x1034)
#define DISP_CC_MDSS_PCLK0_CMD_RCGR     (DISPCC_BASE + 0x1078)
#define DISP_CC_MDSS_BYTE0_CMD_RCGR     (DISPCC_BASE + 0x10D8)
#define DISP_CC_MDSS_BYTE0_DIV_CDIVR    (DISPCC_BASE + 0x10F0)
#define DISP_CC_MDSS_BYTE0_DIV_MASK     0x0F

//
// RCG2 (clk-rcg2.c): CMD_RCGR, then CFG_RCGR, M, N, D.
//
#define RCG_CMD                  0x00
#define RCG_CMD_UPDATE           BIT0
#define RCG_CMD_ROOT_OFF         BIT31
#define RCG_CFG                  0x04
#define RCG_CFG_SRC_DIV_MASK     0x1F
#define RCG_CFG_SRC_SEL_SHIFT    8
#define RCG_CFG_SRC_SEL_MASK     (0x7 << RCG_CFG_SRC_SEL_SHIFT)
#define RCG_CFG_MODE_MASK        (0x3 << 12)
#define RCG_CFG_HW_CLK_CTRL      BIT20
#define RCG_CFG_MASK             (RCG_CFG_SRC_DIV_MASK | RCG_CFG_SRC_SEL_MASK | RCG_CFG_MODE_MASK | RCG_CFG_HW_CLK_CTRL)
#define RCG_M                    0x08
#define RCG_N                    0x0C
#define RCG_D                    0x10
#define RCG_MND_MASK             0xFF     // mnd_width 8 of the pixel clock RCG

#define RCG_SRC_XO               0
#define RCG_SRC_DSI0_PHY_PLL     1

//
// Branch CBCR (clk-branch.c).
//
#define CBCR_CLK_ENABLE  BIT0
#define CBCR_CLK_OFF     BIT31

//
// Timeouts. Linux: REFGEN 1 ms, PLL lock 5 ms, RCG update 500 us, branch
// 200 us. The branch timeout is longer here; it costs nothing when the clock
// runs.
//
#define DSI_PHY_REFGEN_TIMEOUT_US  1000
#define DSI_PLL_LOCK_TIMEOUT_US    5000
#define RCG_UPDATE_TIMEOUT_US      500
#define CBCR_TIMEOUT_US            1000

//
// PLL reference: the PLL runs at 2 * 19.2 MHz * (DECIMAL + FRAC / 2^18).
//
#define DSI_PLL_REF_HZ     19200000
#define DSI_PLL_FRAC_BITS  18
#define DSI_PLL_VCO_MIN_HZ 600000000ULL      // min_pll_rate of dsi_phy_7nm_7280_cfgs
#define DSI_PLL_VCO_MAX_HZ 5000000000ULL

//
// What the PHY, the PLL and the link clocks need for one pixel clock.
//
typedef struct {
  UINT32    PixelClockKhz;
  UINT64    VcoHz;
  UINT8     PllOutDivLog2;      // PLL_OUTDIV_RATE[1:0]: VCO / 2^n
  UINT8     BitClkDiv;          // CMN_CLK_CFG0[3:0]: bit clock = out_div clock / n
  UINT8     PclkMux;            // CMN_CLK_CFG1[1:0]: 0 = bit clock, 1 = bit clock / 2
  UINT8     PixClkDiv;          // CMN_CLK_CFG0[7:4]: pixel clock = mux / n
  UINT8     DecimalDivStart;    // DECIMAL_DIV_START_1
  UINT32    FracDivStart;       // FRAC_DIV_START_{LOW,MID,HIGH}_1, 18 bits
  //
  // D-PHY timing, in byte clocks (8 UI) unless noted: TIMING_CTRL_1..8, 12, 13.
  //
  UINT8     ClkZero;
  UINT8     ClkPrepare;
  UINT8     ClkTrail;
  UINT8     HsExit;
  UINT8     HsZero;
  UINT8     HsPrepare;
  UINT8     HsTrail;
  UINT8     HsRqst;
  UINT8     ClkPre;             // 16 UI units, shared with the host
  UINT8     ClkPost;            // 16 UI units, shared with the host
} DSI_PHY_MODE;

//
// The supported pixel clocks. To add a mode, pick a VCO in 600 MHz..5 GHz
// and dividers that give the byte and pixel clocks exactly (from a clean
// state the Linux clock framework settles on the smallest dividers), compute
// DECIMAL/FRAC from the VCO, and evaluate msm_dsi_dphy_timing_calc_v4() (VK
// dsi_phy.c) for the lane rate. DsiPhyCheckMode() checks the clock
// arithmetic of the entry DsiPhyEnable uses. The rate-dependent analog
// settings follow from the VCO and lane rate (DsiPll*() below).
//
// 1920x1080@60, CEA-861 VIC 16, pixel clock 148.5 MHz:
//   byte clock = 148.5 MHz * 24 bpp / (8 * 4 lanes) = 111.375 MHz.
//   lane rate  = 8 * byte clock = 891 Mbit/s, UI = 1122 ps.
//   VCO        = 891 MHz, out_div 1, bit_div 1: bit clock 891 MHz,
//                byte clock = 891 / 8 = 111.375 MHz; pclk mux on the bit
//                clock, pix_div 6: pixel clock = 891 / 6 = 148.5 MHz, so the
//                DISPCC pixel RCG passes it through (M/N 1/1).
//   DECIMAL    = 891 MHz / 38.4 MHz = 23.203125 -> 23 (0x17);
//   FRAC       = 0.203125 * 2^18 = 53248 (0x0D000), exact.
//   D-PHY      = each value is the MIPI D-PHY minimum for the parameter,
//                in 8 UI steps, moved a fixed share towards its maximum
//                (msm_dsi_dphy_timing_calc_v4):
//                clk_prepare [38, 95] ns at 50%         = 0x08
//                clk_zero    300 ns - clk_prepare, 2%   = 0x1E
//                clk_trail   [60 ns + 3 UI, ...] 30%    = 0x09
//                hs_prepare  [40 ns + 4 UI, 85 ns + 6 UI] 50% = 0x08
//                hs_zero     145 ns + 10 UI - hs_prepare, 10% = 0x22
//                hs_trail    [60 ns + 4 UI, ...] 30%    = 0x08
//                hs_rqst     50 ns - 8 UI               = 0x05
//                hs_exit     100 ns, 10%                = 0x24
//                clk_post    60 ns + 52 UI + hs_trail, in 16 UI, 5%  = 0x18
//                clk_pre     52 ns + clk_prepare + clk_zero + 54 ns,
//                            in 16 UI, 1.25%            = 0x1C
//   All of these match the Linux register dump of this mode.
//
STATIC CONST DSI_PHY_MODE  mDsiPhyModes[] = {
  {
    148500,               // PixelClockKhz
    891000000ULL,         // VcoHz
    0,                    // PllOutDivLog2: / 1
    1,                    // BitClkDiv
    0,                    // PclkMux: bit clock
    6,                    // PixClkDiv
    0x17,                 // DecimalDivStart
    0x0D000,              // FracDivStart
    0x1E,                 // ClkZero
    0x08,                 // ClkPrepare
    0x09,                 // ClkTrail
    0x24,                 // HsExit
    0x22,                 // HsZero
    0x08,                 // HsPrepare
    0x08,                 // HsTrail
    0x05,                 // HsRqst
    0x1C,                 // ClkPre
    0x18,                 // ClkPost
  },
};

//
// Rate-dependent analog settings of a V4.1 PHY (VK dsi_phy_7nm.c
// dsi_pll_calc_dec_frac, dsi_pll_config_hzindep_reg, dsi_7nm_phy_enable).
//
#define DSI_PHY_LOW_RATE_MAX_BPS  1500000000ULL   // "less_than_1500_mhz"

/**
  Returns PLL_CLOCK_INVERTERS for a VCO rate (V4.1).

  @param[in]  VcoHz  The VCO rate.

  @return  The register value.
**/
STATIC
UINT8
DsiPllClockInverters (
  IN UINT64  VcoHz
  )
{
  if (VcoHz <= 1000000000ULL) {
    return 0xA0;
  } else if (VcoHz <= 2500000000ULL) {
    return 0x20;
  } else if (VcoHz <= 3020000000ULL) {
    return 0x00;
  }

  return 0x40;
}

/**
  Returns PLL_VCO_CONFIG_1 for a VCO rate (V4.1).

  @param[in]  VcoHz  The VCO rate.

  @return  The register value.
**/
STATIC
UINT8
DsiPllVcoConfig1 (
  IN UINT64  VcoHz
  )
{
  if (VcoHz < 1520000000ULL) {
    return 0x08;
  } else if (VcoHz < 2990000000ULL) {
    return 0x01;
  }

  return 0x00;
}

/**
  Returns PLL_ANALOG_CONTROLS_FIVE_1 for a VCO rate (V4.1 and later).

  @param[in]  VcoHz  The VCO rate.

  @return  The register value.
**/
STATIC
UINT8
DsiPllAnalogControlsFive1 (
  IN UINT64  VcoHz
  )
{
  return (VcoHz >= 3100000000ULL) ? 0x03 : 0x01;
}

STATIC BOOLEAN  mDsiPhyEnabled;
STATIC BOOLEAN  mDsiPhyByte0DivSaved;
STATIC UINT32   mDsiPhyByte0DivEntry;

/**
  Returns the lane rate of a mode.

  @param[in]  Mode  The mode.

  @return  The lane rate in bit/s.
**/
STATIC
UINT64
DsiPhyBitRate (
  IN CONST DSI_PHY_MODE  *Mode
  )
{
  return DivU64x32 (RShiftU64 (Mode->VcoHz, Mode->PllOutDivLog2), Mode->BitClkDiv);
}

/**
  Checks the clock arithmetic of a mode: the VCO the PLL dividers give, the
  byte clock and the pixel clock the post dividers give.

  @param[in]  Mode  The mode.

  @retval TRUE   The entry is consistent.
  @retval FALSE  It is not; it is logged.
**/
STATIC
BOOLEAN
DsiPhyCheckMode (
  IN CONST DSI_PHY_MODE  *Mode
  )
{
  UINT64  PllHz;
  UINT64  BitHz;
  UINT64  PixelHz;
  UINT64  WantBitHz;

  //
  // Field ranges first: the dividers below must not be 0.
  //
  if ((Mode->VcoHz < DSI_PLL_VCO_MIN_HZ) || (Mode->VcoHz > DSI_PLL_VCO_MAX_HZ) ||
      (Mode->FracDivStart >= (1U << DSI_PLL_FRAC_BITS)) ||
      (Mode->BitClkDiv == 0) || (Mode->BitClkDiv > 0xF) ||
      (Mode->PixClkDiv == 0) || (Mode->PixClkDiv > 0xF) ||
      (Mode->PclkMux > 1) || (Mode->PllOutDivLog2 > 3))
  {
    DEBUG ((DEBUG_ERROR, "%a: PLL plan for %u kHz out of range\n", __func__, Mode->PixelClockKhz));
    return FALSE;
  }

  PllHz = MultU64x32 (2 * DSI_PLL_REF_HZ, Mode->DecimalDivStart) +
          RShiftU64 (MultU64x32 (2 * DSI_PLL_REF_HZ, Mode->FracDivStart), DSI_PLL_FRAC_BITS);
  BitHz = DsiPhyBitRate (Mode);
  //
  // Lane rate = pixel clock * bpp / lanes.
  //
  WantBitHz = DivU64x32 (MultU64x32 ((UINT64)Mode->PixelClockKhz * 1000, DSI_BITS_PER_PIXEL), DSI_LANE_COUNT);
  PixelHz   = DivU64x32 (RShiftU64 (BitHz, Mode->PclkMux), Mode->PixClkDiv);

  if ((PllHz != Mode->VcoHz) || (BitHz != WantBitHz) ||
      (PixelHz != (UINT64)Mode->PixelClockKhz * 1000))
  {
    DEBUG ((
      DEBUG_ERROR,
      "%a: bad PLL plan for %u kHz: VCO %lu Hz (PLL gives %lu), lane %lu bit/s (want %lu), pixel %lu Hz\n",
      __func__,
      Mode->PixelClockKhz,
      Mode->VcoHz,
      PllHz,
      BitHz,
      WantBitHz,
      PixelHz
      ));
    return FALSE;
  }

  return TRUE;
}

/**
  Finds the PHY settings for a pixel clock.

  @param[in]  PixelClockKhz  The pixel clock.

  @return  The settings, or NULL if the pixel clock is not supported.
**/
STATIC
CONST DSI_PHY_MODE *
DsiPhyFindMode (
  IN UINT32  PixelClockKhz
  )
{
  UINTN  Index;

  for (Index = 0; Index < ARRAY_SIZE (mDsiPhyModes); Index++) {
    if (mDsiPhyModes[Index].PixelClockKhz == PixelClockKhz) {
      return &mDsiPhyModes[Index];
    }
  }

  return NULL;
}

/**
  Returns the clock lane timing the PHY uses for a mode, for the host's
  CLKOUT_TIMING_CTRL.

  @param[in]   Timing  The mode.
  @param[out]  Shared  The timing.

  @retval EFI_SUCCESS      Returned.
  @retval EFI_UNSUPPORTED  The PHY has no settings for the pixel clock.
**/
EFI_STATUS
DsiPhyGetSharedTimings (
  IN  CONST DISPLAY_TIMING    *Timing,
  OUT DSI_PHY_SHARED_TIMINGS  *Shared
  )
{
  CONST DSI_PHY_MODE  *Mode;

  Mode = DsiPhyFindMode (Timing->PixelClockKhz);
  if (Mode == NULL) {
    return EFI_UNSUPPORTED;
  }

  Shared->ClkPre  = Mode->ClkPre;
  Shared->ClkPost = Mode->ClkPost;
  return EFI_SUCCESS;
}

/**
  Switches an RCG to a new configuration and waits for it to take it.

  @param[in]  CmdRcgr  The RCG's CMD_RCGR.
  @param[in]  Cfg      The new CFG_RCGR fields (source, divider, mode).
  @param[in]  Name     The RCG, for the log.

  @retval EFI_SUCCESS  The RCG took the configuration.
  @retval EFI_TIMEOUT  It did not.
**/
STATIC
EFI_STATUS
DsiPhyRcgUpdate (
  IN UINTN        CmdRcgr,
  IN UINT32       Cfg,
  IN CONST CHAR8  *Name
  )
{
  EFI_STATUS  Status;

  MmioAndThenOr32 (CmdRcgr + RCG_CFG, ~(UINT32)RCG_CFG_MASK, Cfg);
  MmioOr32 (CmdRcgr + RCG_CMD, RCG_CMD_UPDATE);
  Status = MmioPoll32 (CmdRcgr + RCG_CMD, RCG_CMD_UPDATE, 0, RCG_UPDATE_TIMEOUT_US);
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "%a: %a RCG did not take CFG 0x%x (CMD 0x%x CFG 0x%x)\n",
      __func__,
      Name,
      Cfg,
      MmioRead32 (CmdRcgr + RCG_CMD),
      MmioRead32 (CmdRcgr + RCG_CFG)
      ));
  }

  return Status;
}

/**
  Turns a DISPCC branch clock on or off and waits for its CLK_OFF status.

  @param[in]  Cbcr    The branch's CBCR.
  @param[in]  Enable  TRUE to turn it on.
  @param[in]  Name    The branch, for the log.

  @retval EFI_SUCCESS  The branch reached the state.
  @retval EFI_TIMEOUT  It did not.
**/
STATIC
EFI_STATUS
DsiPhyBranchSet (
  IN UINTN        Cbcr,
  IN BOOLEAN      Enable,
  IN CONST CHAR8  *Name
  )
{
  EFI_STATUS  Status;

  if (Enable) {
    MmioOr32 (Cbcr, CBCR_CLK_ENABLE);
  } else {
    MmioAnd32 (Cbcr, ~(UINT32)CBCR_CLK_ENABLE);
  }

  Status = MmioPoll32 (Cbcr, CBCR_CLK_OFF, Enable ? 0 : CBCR_CLK_OFF, CBCR_TIMEOUT_US);
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "%a: %a branch stuck %a (CBCR 0x%x)\n",
      __func__,
      Name,
      Enable ? "off" : "on",
      MmioRead32 (Cbcr)
      ));
  }

  return Status;
}

/**
  Returns whether any of the link branch clocks is on.

  @retval TRUE   BYTE0, BYTE0_INTF or PCLK0 is enabled.
  @retval FALSE  None is.
**/
STATIC
BOOLEAN
DsiPhyLinkClocksEnabled (
  VOID
  )
{
  return ((MmioRead32 (DISP_CC_MDSS_BYTE0_CBCR) |
           MmioRead32 (DISP_CC_MDSS_BYTE0_INTF_CBCR) |
           MmioRead32 (DISP_CC_MDSS_PCLK0_CBCR)) & CBCR_CLK_ENABLE) != 0;
}

/**
  Returns whether the PLL runs and is locked.

  @retval TRUE   It is started and locked.
  @retval FALSE  It is not.
**/
STATIC
BOOLEAN
DsiPllIsLocked (
  VOID
  )
{
  return ((MmioRead32 (PHY_CMN (PHY_CMN_PLL_CNTRL)) & PHY_CMN_PLL_CNTRL_START) != 0) &&
         ((MmioRead32 (PHY_PLL (PLL_COMMON_STATUS_ONE)) & PLL_COMMON_STATUS_ONE_LOCK) != 0);
}

/**
  Parks the byte and pixel clock RCGs on XO, at their reset source.

  An RCG whose root is on only switches between two running sources, so an
  RCG left on the PLL output after the PLL stops can hang the next owner's
  update if anything turns its root on first. Parked on XO, the next owner
  finds the RCGs on a source that always runs.
**/
STATIC
VOID
DsiPhyParkRcgs (
  VOID
  )
{
  DsiPhyRcgUpdate (DISP_CC_MDSS_BYTE0_CMD_RCGR, RCG_SRC_XO << RCG_CFG_SRC_SEL_SHIFT, "BYTE0");
  DsiPhyRcgUpdate (DISP_CC_MDSS_PCLK0_CMD_RCGR, RCG_SRC_XO << RCG_CFG_SRC_SEL_SHIFT, "PCLK0");
}

/**
  Turns the link clocks off and parks their RCGs on XO. Safe to call in any
  state.

  With the PLL running, the RCGs are parked first, while their branches keep
  them on, so that the switch from the PLL to XO happens with both sources
  running. With the PLL not running, the branches go off first: an RCG whose
  root is off takes a new configuration without switching. (Linux relies on
  that when the DSI driver sets the stopped PLL as their parent at probe; an
  idle Linux leaves them on the PLL with their root off.)
**/
STATIC
VOID
DsiPhyLinkClocksOff (
  VOID
  )
{
  BOOLEAN  PllLocked;

  PllLocked = DsiPllIsLocked ();
  if (PllLocked) {
    DsiPhyParkRcgs ();
  }

  //
  // Reverse order of enabling.
  //
  DsiPhyBranchSet (DISP_CC_MDSS_BYTE0_INTF_CBCR, FALSE, "BYTE0_INTF");
  DsiPhyBranchSet (DISP_CC_MDSS_PCLK0_CBCR, FALSE, "PCLK0");
  DsiPhyBranchSet (DISP_CC_MDSS_BYTE0_CBCR, FALSE, "BYTE0");

  if (!PllLocked) {
    DsiPhyParkRcgs ();
  }

  if (mDsiPhyByte0DivSaved) {
    MmioAndThenOr32 (
      DISP_CC_MDSS_BYTE0_DIV_CDIVR,
      ~(UINT32)DISP_CC_MDSS_BYTE0_DIV_MASK,
      mDsiPhyByte0DivEntry & DISP_CC_MDSS_BYTE0_DIV_MASK
      );
    mDsiPhyByte0DivSaved = FALSE;
  }

  DEBUG ((
    DEBUG_INFO,
    "%a: BYTE0 CMD 0x%x CFG 0x%x, PCLK0 CMD 0x%x CFG 0x%x, CBCR BYTE0 0x%x BYTE0_INTF 0x%x PCLK0 0x%x\n",
    __func__,
    MmioRead32 (DISP_CC_MDSS_BYTE0_CMD_RCGR + RCG_CMD),
    MmioRead32 (DISP_CC_MDSS_BYTE0_CMD_RCGR + RCG_CFG),
    MmioRead32 (DISP_CC_MDSS_PCLK0_CMD_RCGR + RCG_CMD),
    MmioRead32 (DISP_CC_MDSS_PCLK0_CMD_RCGR + RCG_CFG),
    MmioRead32 (DISP_CC_MDSS_BYTE0_CBCR),
    MmioRead32 (DISP_CC_MDSS_BYTE0_INTF_CBCR),
    MmioRead32 (DISP_CC_MDSS_PCLK0_CBCR)
    ));
}

/**
  Switches the byte and pixel clocks onto the running PLL and turns them on,
  in the order the Linux DSI host does (rates, then byte, pixel, byte_intf).

  The RCGs are off when they take the PLL as source, so their UPDATE
  completes without proving much; the branches' CLK_OFF clearing proves the
  PLL outputs reach them.

  @retval EFI_SUCCESS  The link clocks run.
  @retval Other        A clock did not start; the caller turns them off.
**/
STATIC
EFI_STATUS
DsiPhyLinkClocksOn (
  VOID
  )
{
  EFI_STATUS  Status;

  //
  // BYTE0: DSI0 PHY PLL byte clock, SRC_DIV 1 (divide by 1), as clk_byte2
  // computes for a parent at the requested rate.
  //
  Status = DsiPhyRcgUpdate (
             DISP_CC_MDSS_BYTE0_CMD_RCGR,
             (RCG_SRC_DSI0_PHY_PLL << RCG_CFG_SRC_SEL_SHIFT) | 1,
             "BYTE0"
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // PCLK0: DSI0 PHY PLL pixel clock at the exact rate, so clk_pixel uses the
  // 1/1 fraction: M = 1, N = ~(N - M) = 0xFF, D = ~(clamped 2D = 0) = 0xFF,
  // mode bypass, SRC_DIV left at 0 (bypass).
  //
  MmioAndThenOr32 (DISP_CC_MDSS_PCLK0_CMD_RCGR + RCG_M, ~(UINT32)RCG_MND_MASK, 0x01);
  MmioAndThenOr32 (DISP_CC_MDSS_PCLK0_CMD_RCGR + RCG_N, ~(UINT32)RCG_MND_MASK, 0xFF);
  MmioAndThenOr32 (DISP_CC_MDSS_PCLK0_CMD_RCGR + RCG_D, ~(UINT32)RCG_MND_MASK, 0xFF);
  Status = DsiPhyRcgUpdate (
             DISP_CC_MDSS_PCLK0_CMD_RCGR,
             RCG_SRC_DSI0_PHY_PLL << RCG_CFG_SRC_SEL_SHIFT,
             "PCLK0"
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // BYTE0_INTF runs at half the byte clock: the v4 D-PHY timing sets
  // byte_intf_clk_div_2. The divider field holds divisor - 1.
  //
  mDsiPhyByte0DivEntry = MmioRead32 (DISP_CC_MDSS_BYTE0_DIV_CDIVR);
  mDsiPhyByte0DivSaved = TRUE;
  MmioAndThenOr32 (DISP_CC_MDSS_BYTE0_DIV_CDIVR, ~(UINT32)DISP_CC_MDSS_BYTE0_DIV_MASK, 2 - 1);

  Status = DsiPhyBranchSet (DISP_CC_MDSS_BYTE0_CBCR, TRUE, "BYTE0");
  if (!EFI_ERROR (Status)) {
    Status = DsiPhyBranchSet (DISP_CC_MDSS_PCLK0_CBCR, TRUE, "PCLK0");
  }

  if (!EFI_ERROR (Status)) {
    Status = DsiPhyBranchSet (DISP_CC_MDSS_BYTE0_INTF_CBCR, TRUE, "BYTE0_INTF");
  }

  DEBUG ((
    EFI_ERROR (Status) ? DEBUG_ERROR : DEBUG_INFO,
    "%a: BYTE0 CMD 0x%x CFG 0x%x DIV 0x%x, PCLK0 CMD 0x%x CFG 0x%x M 0x%x N 0x%x D 0x%x, "
    "CBCR BYTE0 0x%x PCLK0 0x%x BYTE0_INTF 0x%x\n",
    __func__,
    MmioRead32 (DISP_CC_MDSS_BYTE0_CMD_RCGR + RCG_CMD),
    MmioRead32 (DISP_CC_MDSS_BYTE0_CMD_RCGR + RCG_CFG),
    MmioRead32 (DISP_CC_MDSS_BYTE0_DIV_CDIVR),
    MmioRead32 (DISP_CC_MDSS_PCLK0_CMD_RCGR + RCG_CMD),
    MmioRead32 (DISP_CC_MDSS_PCLK0_CMD_RCGR + RCG_CFG),
    MmioRead32 (DISP_CC_MDSS_PCLK0_CMD_RCGR + RCG_M),
    MmioRead32 (DISP_CC_MDSS_PCLK0_CMD_RCGR + RCG_N),
    MmioRead32 (DISP_CC_MDSS_PCLK0_CMD_RCGR + RCG_D),
    MmioRead32 (DISP_CC_MDSS_BYTE0_CBCR),
    MmioRead32 (DISP_CC_MDSS_PCLK0_CBCR),
    MmioRead32 (DISP_CC_MDSS_BYTE0_INTF_CBCR)
    ));

  return Status;
}

/**
  Waits for REFGEN, then powers up and programs the PHY common block, the
  D-PHY timing and the lanes (VK dsi_7nm_phy_enable, D-PHY path).

  @param[in]  Mode  The mode.

  @retval EFI_SUCCESS    Programmed.
  @retval EFI_NOT_READY  REFGEN is not ready; nothing was written.
**/
STATIC
EFI_STATUS
DsiPhyPowerUp (
  IN CONST DSI_PHY_MODE  *Mode
  )
{
  EFI_STATUS  Status;
  BOOLEAN     LowRate;
  UINT32      Revision;
  UINTN       Lane;

  if ((MmioRead32 (PHY_CMN (PHY_CMN_PLL_CNTRL)) & PHY_CMN_PLL_CNTRL_START) != 0) {
    DEBUG ((DEBUG_WARN, "%a: PLL on before the PHY is configured\n", __func__));
  }

  //
  // The PHY bias comes from the REFGEN block, turned on by DisplayPowerOn.
  // V4.1 has no REFGEN request bit (GLBL_DIGTOP_SPARE10 is V4.3/V5.2 only).
  //
  Status = MmioPoll32 (
             PHY_CMN (PHY_CMN_PHY_STATUS),
             PHY_CMN_PHY_STATUS_REFGEN_READY,
             PHY_CMN_PHY_STATUS_REFGEN_READY,
             DSI_PHY_REFGEN_TIMEOUT_US
             );
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "%a: REFGEN not ready (PHY_STATUS 0x%x); is REFGEN (0x088E7080) on?\n",
      __func__,
      MmioRead32 (PHY_CMN (PHY_CMN_PHY_STATUS))
      ));
    return EFI_NOT_READY;
  }

  LowRate  = DsiPhyBitRate (Mode) <= DSI_PHY_LOW_RATE_MAX_BPS;
  Revision = MmioRead32 (PHY_CMN (PHY_CMN_REVISION_ID0));
  DEBUG ((
    DEBUG_INFO,
    "%a: REFGEN ready (PHY_STATUS 0x%x), REVISION_ID0 0x%x\n",
    __func__,
    MmioRead32 (PHY_CMN (PHY_CMN_PHY_STATUS)),
    Revision
    ));

  //
  // Digital and PLL power-down off, PLL core in reset, resync FIFO off.
  //
  MmioWrite32 (PHY_CMN (PHY_CMN_CTRL_0), PHY_CMN_CTRL_0_DIGTOP_PLL_PWRDN_B);
  MmioWrite32 (PHY_CMN (PHY_CMN_PLL_CNTRL), 0);
  MmioWrite32 (PHY_CMN (PHY_CMN_RBUF_CTRL), 0);

  //
  // Minor revision 2 dies need CTRL_4 (this board's reads 0x14: not needed).
  //
  if ((Revision & 0xF0) == 0x20) {
    MmioWrite32 (PHY_CMN (PHY_CMN_CTRL_4), 0x04);
  }

  //
  // Lane mapping: logical lanes 0-3 on physical lanes 0-3, clock on lane 4.
  //
  MmioWrite32 (PHY_CMN (PHY_CMN_LANE_CFG0), 0x21);
  MmioWrite32 (PHY_CMN (PHY_CMN_LANE_CFG1), 0x84);

  //
  // LDO and drive strength, V4.1 D-PHY; the lower rate settings apply up to
  // 1.5 Gbit/s per lane.
  //
  MmioWrite32 (PHY_CMN (PHY_CMN_VREG_CTRL_0), LowRate ? 0x53 : 0x52);
  MmioWrite32 (PHY_CMN (PHY_CMN_VREG_CTRL_1), 0x5C);
  MmioWrite32 (PHY_CMN (PHY_CMN_CTRL_3), 0x00);
  MmioWrite32 (PHY_CMN (PHY_CMN_GLBL_STR_SWI_CAL_SEL_CTRL), 0x00);
  MmioWrite32 (PHY_CMN (PHY_CMN_GLBL_HSTX_STR_CTRL_0), 0x88);
  MmioWrite32 (PHY_CMN (PHY_CMN_GLBL_PEMPH_CTRL_0), 0x00);
  MmioWrite32 (PHY_CMN (PHY_CMN_GLBL_RESCODE_OFFSET_TOP_CTRL), LowRate ? 0x3D : 0x00);
  MmioWrite32 (PHY_CMN (PHY_CMN_GLBL_RESCODE_OFFSET_BOT_CTRL), LowRate ? 0x39 : 0x3C);
  MmioWrite32 (PHY_CMN (PHY_CMN_GLBL_LPTX_STR_CTRL), 0x55);

  //
  // All blocks on, all five lanes on, full-rate mode.
  //
  MmioWrite32 (PHY_CMN (PHY_CMN_CTRL_0), PHY_CMN_CTRL_0_ALL_ON);
  MmioWrite32 (PHY_CMN (PHY_CMN_LANE_CTRL0), PHY_CMN_LANE_CTRL0_ALL_LANES);
  MmioWrite32 (PHY_CMN (PHY_CMN_CTRL_2), PHY_CMN_CTRL_2_FULL_RATE);

  //
  // Standalone PHY: bit clock from its own PLL, pixel mux on the bit clock
  // until the PLL configuration sets it.
  //
  MmioWrite32 (PHY_CMN (PHY_CMN_CLK_CFG1), 0);

  //
  // D-PHY timing.
  //
  MmioWrite32 (PHY_CMN (PHY_CMN_TIMING_CTRL (0)), 0x00);
  MmioWrite32 (PHY_CMN (PHY_CMN_TIMING_CTRL (1)), Mode->ClkZero);
  MmioWrite32 (PHY_CMN (PHY_CMN_TIMING_CTRL (2)), Mode->ClkPrepare);
  MmioWrite32 (PHY_CMN (PHY_CMN_TIMING_CTRL (3)), Mode->ClkTrail);
  MmioWrite32 (PHY_CMN (PHY_CMN_TIMING_CTRL (4)), Mode->HsExit);
  MmioWrite32 (PHY_CMN (PHY_CMN_TIMING_CTRL (5)), Mode->HsZero);
  MmioWrite32 (PHY_CMN (PHY_CMN_TIMING_CTRL (6)), Mode->HsPrepare);
  MmioWrite32 (PHY_CMN (PHY_CMN_TIMING_CTRL (7)), Mode->HsTrail);
  MmioWrite32 (PHY_CMN (PHY_CMN_TIMING_CTRL (8)), Mode->HsRqst);
  MmioWrite32 (PHY_CMN (PHY_CMN_TIMING_CTRL (9)), 0x02);
  MmioWrite32 (PHY_CMN (PHY_CMN_TIMING_CTRL (10)), 0x04);
  MmioWrite32 (PHY_CMN (PHY_CMN_TIMING_CTRL (11)), 0x00);
  MmioWrite32 (PHY_CMN (PHY_CMN_TIMING_CTRL (12)), Mode->ClkPre);
  MmioWrite32 (PHY_CMN (PHY_CMN_TIMING_CTRL (13)), Mode->ClkPost);

  //
  // Lanes: LP receive (for bus turn-around) only on data lane 0, no pin swap,
  // V4.1 drive control.
  //
  for (Lane = 0; Lane < PHY_LANE_COUNT; Lane++) {
    MmioWrite32 (PHY_LANE (PHY_LN_LPRX_CTRL (Lane)), 0);
    MmioWrite32 (PHY_LANE (PHY_LN_PIN_SWAP (Lane)), 0);
  }

  MmioWrite32 (PHY_LANE (PHY_LN_LPRX_CTRL (0)), 0x03);

  for (Lane = 0; Lane < PHY_LANE_COUNT; Lane++) {
    MmioWrite32 (PHY_LANE (PHY_LN_CFG0 (Lane)), 0x00);
    MmioWrite32 (PHY_LANE (PHY_LN_CFG1 (Lane)), 0x00);
    MmioWrite32 (PHY_LANE (PHY_LN_CFG2 (Lane)), (Lane == PHY_CLOCK_LANE) ? 0x8A : 0x0A);
    MmioWrite32 (PHY_LANE (PHY_LN_TX_DCTRL (Lane)), mDsiPhyLaneTxDctrl[Lane]);
  }

  return EFI_SUCCESS;
}

/**
  Powers the PHY down (VK dsi_7nm_phy_disable). The PLL must be stopped.
**/
STATIC
VOID
DsiPhyPowerDown (
  VOID
  )
{
  if ((MmioRead32 (PHY_CMN (PHY_CMN_PLL_CNTRL)) & PHY_CMN_PLL_CNTRL_START) != 0) {
    DEBUG ((DEBUG_WARN, "%a: PHY going off with the PLL on\n", __func__));
  }

  MmioWrite32 (PHY_LANE (PHY_LN_LPRX_CTRL (0)), 0);
  MmioAnd32 (PHY_CMN (PHY_CMN_CTRL_0), ~(UINT32)PHY_CMN_CTRL_0_LANES_MASK);
  MmioWrite32 (PHY_CMN (PHY_CMN_LANE_CTRL0), 0);
  MmioWrite32 (PHY_CMN (PHY_CMN_CTRL_0), 0);
}

/**
  Programs the PLL for a mode without starting it: the post dividers (VK
  dsi_7nm_pll_restore_state), then the dividers and the rate-independent
  loop settings (VK dsi_pll_7nm_vco_set_rate). Spread spectrum stays off.

  @param[in]  Mode  The mode.
**/
STATIC
VOID
DsiPllConfigure (
  IN CONST DSI_PHY_MODE  *Mode
  )
{
  MmioAndThenOr32 (PHY_PLL (PLL_PLL_OUTDIV_RATE), ~(UINT32)PLL_PLL_OUTDIV_RATE_MASK, Mode->PllOutDivLog2);
  MmioWrite32 (
    PHY_CMN (PHY_CMN_CLK_CFG0),
    Mode->BitClkDiv | ((UINT32)Mode->PixClkDiv << PHY_CMN_CLK_CFG0_PIX_DIV_SHIFT)
    );
  MmioAndThenOr32 (PHY_CMN (PHY_CMN_CLK_CFG1), ~(UINT32)PHY_CMN_CLK_CFG1_DSICLK_SEL_MASK, Mode->PclkMux);

  //
  // Dividers (dsi_pll_commit). The vendor kernel writes the clock inverters
  // to PLL_CLOCK_INVERTERS (0x19C); its dump shows 0xA0 there and
  // PLL_CLOCK_INVERTERS_1 (0x248, what mainline writes) at its reset value.
  //
  MmioWrite32 (PHY_PLL (PLL_CORE_INPUT_OVERRIDE), 0x12);
  MmioWrite32 (PHY_PLL (PLL_DECIMAL_DIV_START_1), Mode->DecimalDivStart);
  MmioWrite32 (PHY_PLL (PLL_FRAC_DIV_START_LOW_1), Mode->FracDivStart & 0xFF);
  MmioWrite32 (PHY_PLL (PLL_FRAC_DIV_START_MID_1), (Mode->FracDivStart >> 8) & 0xFF);
  MmioWrite32 (PHY_PLL (PLL_FRAC_DIV_START_HIGH_1), (Mode->FracDivStart >> 16) & 0x3);
  MmioWrite32 (PHY_PLL (PLL_PLL_LOCKDET_RATE_1), 0x40);
  MmioWrite32 (PHY_PLL (PLL_PLL_LOCK_DELAY), 0x06);
  MmioWrite32 (PHY_PLL (PLL_CMODE_1), PLL_CMODE_1_DPHY);
  MmioWrite32 (PHY_PLL (PLL_CLOCK_INVERTERS), DsiPllClockInverters (Mode->VcoHz));

  //
  // Loop settings (dsi_pll_config_hzindep_reg). The filter and gain
  // registers written twice are written twice by Linux as well.
  //
  MmioWrite32 (PHY_PLL (PLL_ANALOG_CONTROLS_FIVE_1), DsiPllAnalogControlsFive1 (Mode->VcoHz));
  MmioWrite32 (PHY_PLL (PLL_VCO_CONFIG_1), DsiPllVcoConfig1 (Mode->VcoHz));
  MmioWrite32 (PHY_PLL (PLL_ANALOG_CONTROLS_FIVE), 0x01);
  MmioWrite32 (PHY_PLL (PLL_ANALOG_CONTROLS_TWO), 0x03);
  MmioWrite32 (PHY_PLL (PLL_ANALOG_CONTROLS_THREE), 0x00);
  MmioWrite32 (PHY_PLL (PLL_DSM_DIVIDER), 0x00);
  MmioWrite32 (PHY_PLL (PLL_FEEDBACK_DIVIDER), 0x4E);
  MmioWrite32 (PHY_PLL (PLL_CALIBRATION_SETTINGS), 0x40);
  MmioWrite32 (PHY_PLL (PLL_BAND_SEL_CAL_SETTINGS_THREE), 0xBA);
  MmioWrite32 (PHY_PLL (PLL_FREQ_DETECT_SETTINGS_ONE), 0x0C);
  MmioWrite32 (PHY_PLL (PLL_OUTDIV), 0x00);
  MmioWrite32 (PHY_PLL (PLL_CORE_OVERRIDE), 0x00);
  MmioWrite32 (PHY_PLL (PLL_PLL_DIGITAL_TIMERS_TWO), 0x08);
  MmioWrite32 (PHY_PLL (PLL_PLL_PROP_GAIN_RATE_1), 0x0A);
  MmioWrite32 (PHY_PLL (PLL_PLL_BAND_SEL_RATE_1), 0xC0);
  MmioWrite32 (PHY_PLL (PLL_PLL_INT_GAIN_IFILT_BAND_1), 0x84);
  MmioWrite32 (PHY_PLL (PLL_PLL_INT_GAIN_IFILT_BAND_1), 0x82);
  MmioWrite32 (PHY_PLL (PLL_PLL_FL_INT_GAIN_PFILT_BAND_1), 0x4C);
  MmioWrite32 (PHY_PLL (PLL_PLL_LOCK_OVERRIDE), 0x80);
  MmioWrite32 (PHY_PLL (PLL_PFILT), 0x29);
  MmioWrite32 (PHY_PLL (PLL_PFILT), 0x2F);
  MmioWrite32 (PHY_PLL (PLL_IFILT), 0x2A);
  MmioWrite32 (PHY_PLL (PLL_IFILT), 0x3F);
  MmioWrite32 (PHY_PLL (PLL_PERF_OPTIMIZE), 0x22);

  DEBUG ((
    DEBUG_INFO,
    "%a: VCO %lu Hz: DECIMAL 0x%x FRAC 0x%x, OUTDIV_RATE 0x%x, CLK_CFG0 0x%x, CLK_CFG1 0x%x, CLOCK_INVERTERS 0x%x\n",
    __func__,
    Mode->VcoHz,
    MmioRead32 (PHY_PLL (PLL_DECIMAL_DIV_START_1)),
    Mode->FracDivStart,
    MmioRead32 (PHY_PLL (PLL_PLL_OUTDIV_RATE)),
    MmioRead32 (PHY_CMN (PHY_CMN_CLK_CFG0)),
    MmioRead32 (PHY_CMN (PHY_CMN_CLK_CFG1)),
    MmioRead32 (PHY_PLL (PLL_CLOCK_INVERTERS))
    ));
}

/**
  Starts the PLL, waits for lock, and turns its outputs on (VK
  dsi_pll_7nm_vco_prepare).

  @retval EFI_SUCCESS  Locked; the byte and pixel clocks run.
  @retval EFI_TIMEOUT  No lock; the caller stops the PLL.
**/
STATIC
EFI_STATUS
DsiPllStart (
  VOID
  )
{
  EFI_STATUS  Status;

  //
  // PLL bias on, then at least 250 ns before starting.
  //
  MmioOr32 (PHY_CMN (PHY_CMN_CTRL_0), PHY_CMN_CTRL_0_PLL_BIAS);
  MmioWrite32 (PHY_PLL (PLL_SYSTEM_MUXES), PLL_SYSTEM_MUXES_BIAS_ON);
  MicroSecondDelay (1);

  MmioWrite32 (PHY_CMN (PHY_CMN_PLL_CNTRL), PHY_CMN_PLL_CNTRL_START);
  Status = MmioPoll32 (
             PHY_PLL (PLL_COMMON_STATUS_ONE),
             PLL_COMMON_STATUS_ONE_LOCK,
             PLL_COMMON_STATUS_ONE_LOCK,
             DSI_PLL_LOCK_TIMEOUT_US
             );
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "%a: PLL did not lock (COMMON_STATUS_ONE 0x%x TWO 0x%x, PHY_STATUS 0x%x, CTRL_0 0x%x)\n",
      __func__,
      MmioRead32 (PHY_PLL (PLL_COMMON_STATUS_ONE)),
      MmioRead32 (PHY_PLL (PLL_COMMON_STATUS_TWO)),
      MmioRead32 (PHY_CMN (PHY_CMN_PHY_STATUS)),
      MmioRead32 (PHY_CMN (PHY_CMN_CTRL_0))
      ));
    return Status;
  }

  DEBUG ((
    DEBUG_INFO,
    "%a: PLL locked (COMMON_STATUS_ONE 0x%x)\n",
    __func__,
    MmioRead32 (PHY_PLL (PLL_COMMON_STATUS_ONE))
    ));

  //
  // Reset the PHY digital block (for a PLL started after a CX or analog
  // power collapse), before the outputs go on.
  //
  MmioWrite32 (PHY_CMN (PHY_CMN_GLBL_DIGTOP_SPARE4), BIT0);
  MmioWrite32 (PHY_CMN (PHY_CMN_GLBL_DIGTOP_SPARE4), 0);

  //
  // Global clock enable: the byte and pixel clocks leave the PHY.
  //
  MmioWrite32 (PHY_CMN (PHY_CMN_CTRL_3), PHY_CMN_CTRL_3_GLOBAL_CLK_EN);
  MmioOr32 (PHY_CMN (PHY_CMN_CLK_CFG1), PHY_CMN_CLK_CFG1_CLK_EN | PHY_CMN_CLK_CFG1_CLK_EN_SEL);

  return EFI_SUCCESS;
}

/**
  Stops the PLL (VK dsi_pll_7nm_vco_unprepare). Its outputs are gated first
  so that no glitch reaches the clocks it feeds.
**/
STATIC
VOID
DsiPllStop (
  VOID
  )
{
  MmioAnd32 (PHY_CMN (PHY_CMN_CLK_CFG1), ~(UINT32)PHY_CMN_CLK_CFG1_CLK_EN);
  MmioWrite32 (PHY_CMN (PHY_CMN_PLL_CNTRL), 0);
  MmioWrite32 (PHY_CMN (PHY_CMN_RBUF_CTRL), 0);
  MmioWrite32 (PHY_PLL (PLL_SYSTEM_MUXES), 0);
  MmioAnd32 (PHY_CMN (PHY_CMN_CTRL_0), ~(UINT32)PHY_CMN_CTRL_0_PLL_BIAS);
  MicroSecondDelay (1);
}

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
  )
{
  CONST DSI_PHY_MODE  *Mode;
  EFI_STATUS          Status;
  UINT64              BitRate;

  if (mDsiPhyEnabled) {
    DEBUG ((DEBUG_ERROR, "%a: already enabled\n", __func__));
    return EFI_ALREADY_STARTED;
  }

  Mode = DsiPhyFindMode (Timing->PixelClockKhz);
  if (Mode == NULL) {
    DEBUG ((DEBUG_ERROR, "%a: no PLL settings for a %u kHz pixel clock\n", __func__, Timing->PixelClockKhz));
    return EFI_UNSUPPORTED;
  }

  if (!DsiPhyCheckMode (Mode)) {
    return EFI_UNSUPPORTED;
  }

  BitRate = DsiPhyBitRate (Mode);
  DEBUG ((
    DEBUG_INFO,
    "%a: pixel %u kHz, %u lanes at %lu bit/s, byte clock %lu Hz, VCO %lu Hz\n",
    __func__,
    Mode->PixelClockKhz,
    DSI_LANE_COUNT,
    BitRate,
    DivU64x32 (BitRate, 8),
    Mode->VcoHz
    ));

  //
  // Nothing should run from the PLL while the PHY is reset. Only a previous
  // owner that did not clean up can have left the link clocks on.
  //
  if (DsiPhyLinkClocksEnabled ()) {
    DEBUG ((DEBUG_WARN, "%a: link clocks on at entry, turning them off\n", __func__));
    DsiPhyLinkClocksOff ();
  }

  Status = DsiHostResetPhy ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = DsiPhyPowerUp (Mode);
  if (EFI_ERROR (Status)) {
    DsiPhyPowerDown ();
    return Status;
  }

  DsiPllConfigure (Mode);

  Status = DsiPllStart ();
  if (EFI_ERROR (Status)) {
    DsiPllStop ();
    DsiPhyPowerDown ();
    return Status;
  }

  Status = DsiPhyLinkClocksOn ();
  if (EFI_ERROR (Status)) {
    DsiPhyLinkClocksOff ();
    DsiPllStop ();
    DsiPhyPowerDown ();
    return Status;
  }

  mDsiPhyEnabled = TRUE;
  DEBUG ((
    DEBUG_INFO,
    "%a: PHY on: CTRL_0 0x%x, CLK_CFG0 0x%x, CLK_CFG1 0x%x, PLL COMMON_STATUS_ONE 0x%x\n",
    __func__,
    MmioRead32 (PHY_CMN (PHY_CMN_CTRL_0)),
    MmioRead32 (PHY_CMN (PHY_CMN_CLK_CFG0)),
    MmioRead32 (PHY_CMN (PHY_CMN_CLK_CFG1)),
    MmioRead32 (PHY_PLL (PLL_COMMON_STATUS_ONE))
    ));

  return EFI_SUCCESS;
}

/**
  Undoes DsiPhyEnable: parks the link clocks on XO before stopping the PLL,
  then powers the PHY down. Must run after DsiHostDisable.
**/
VOID
DsiPhyDisable (
  VOID
  )
{
  if (!mDsiPhyEnabled) {
    return;
  }

  DsiPhyLinkClocksOff ();
  DsiPllStop ();
  DsiPhyPowerDown ();
  mDsiPhyEnabled = FALSE;

  DEBUG ((
    DEBUG_INFO,
    "%a: PHY off: CTRL_0 0x%x, PLL_CNTRL 0x%x, CLK_CFG1 0x%x\n",
    __func__,
    MmioRead32 (PHY_CMN (PHY_CMN_CTRL_0)),
    MmioRead32 (PHY_CMN (PHY_CMN_PLL_CNTRL)),
    MmioRead32 (PHY_CMN (PHY_CMN_CLK_CFG1))
    ));
}
