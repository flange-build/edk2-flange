/** @file
  QCS6490 MDSS display driver: registers and helpers shared by the power,
  clock, RPMh and diagnostics part (Power.c, Rpmh.c, Diag.c).

  Register names follow the Linux clock and regulator drivers of the vendor
  kernel (drivers/clk/qcom/gcc-sc7280.c, dispcc-sc7280.c, clk-rcg2.c,
  clk-branch.c, gdsc.c, drivers/regulator/qcom-refgen-regulator.c).

  Copyright (c) 2026, edk2-flange contributors.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#ifndef MDSS_POWER_H_
#define MDSS_POWER_H_

#include "MdssDisplay.h"

//
// Global clock controller (gcc-sc7280.c).
//
#define GCC_BASE                        0x00100000
#define GCC_DISP_AHB_CBCR               (GCC_BASE + 0x27004)
#define GCC_DISP_HF_AXI_CBCR            (GCC_BASE + 0x2700C)
#define GCC_DISP_SF_AXI_CBCR            (GCC_BASE + 0x27014)
#define GCC_DISP_XO_CBCR                (GCC_BASE + 0x2701C)
#define GCC_APCS_CLOCK_BRANCH_ENA_VOTE  (GCC_BASE + 0x52000)
#define GCC_DISP_GPLL0_CLK_SRC_ENA      BIT7
#define GCC_APCS_GPLL0_ENA_VOTE         (GCC_BASE + 0x52010)
#define GCC_GPLL0_ENA                   BIT0

//
// Branch clock control register (clk-branch.h).
//
#define CBCR_CLK_ENABLE  BIT0
#define CBCR_HW_CTL      BIT1
#define CBCR_CLK_OFF     BIT31

//
// Root clock generator, clk_rcg2 layout (clk-rcg2.c): CMD_RCGR, then CFG_RCGR.
//
#define RCG_CMD                0x0
#define RCG_CMD_UPDATE         BIT0
#define RCG_CMD_ROOT_EN        BIT1
#define RCG_CMD_ROOT_OFF       BIT31
#define RCG_CFG                0x4
#define RCG_CFG_SRC_DIV_MASK   0x1F
#define RCG_CFG_SRC_SEL_SHIFT  8
#define RCG_CFG_SRC_SEL_MASK   (0x7 << RCG_CFG_SRC_SEL_SHIFT)
#define RCG_CFG_MODE_MASK      (0x3 << 12)
#define RCG_CFG_HW_CLK_CTRL    BIT20

//
// The bits clk_rcg2 writes when it configures a root.
//
#define RCG_CFG_MASK  (RCG_CFG_SRC_DIV_MASK | RCG_CFG_SRC_SEL_MASK |  \
                       RCG_CFG_MODE_MASK | RCG_CFG_HW_CLK_CTRL)

//
// Display clock controller (dispcc-sc7280.c).
//
#define DISP_CC_BASE                  0x0AF00000
#define DISP_CC_MDSS_CORE_GDSCR       (DISP_CC_BASE + 0x1004)
#define DISP_CC_MDSS_CORE_CFG_GDSCR   (DISP_CC_BASE + 0x1008)
#define DISP_CC_MDSS_PCLK0_CBCR       (DISP_CC_BASE + 0x1010)
#define DISP_CC_MDSS_MDP_CBCR         (DISP_CC_BASE + 0x1014)
#define DISP_CC_MDSS_MDP_LUT_CBCR     (DISP_CC_BASE + 0x1024)
#define DISP_CC_MDSS_VSYNC_CBCR       (DISP_CC_BASE + 0x102C)
#define DISP_CC_MDSS_BYTE0_CBCR       (DISP_CC_BASE + 0x1030)
#define DISP_CC_MDSS_BYTE0_INTF_CBCR  (DISP_CC_BASE + 0x1034)
#define DISP_CC_MDSS_ESC0_CBCR        (DISP_CC_BASE + 0x1038)
#define DISP_CC_MDSS_AHB_CBCR         (DISP_CC_BASE + 0x1050)
#define DISP_CC_MDSS_PCLK0_CMD_RCGR   (DISP_CC_BASE + 0x1078)
#define DISP_CC_MDSS_MDP_CMD_RCGR     (DISP_CC_BASE + 0x1090)
#define DISP_CC_MDSS_VSYNC_CMD_RCGR   (DISP_CC_BASE + 0x10C0)
#define DISP_CC_MDSS_BYTE0_CMD_RCGR   (DISP_CC_BASE + 0x10D8)
#define DISP_CC_MDSS_BYTE0_DIV_CDIVR  (DISP_CC_BASE + 0x10F0)
#define DISP_CC_MDSS_ESC0_CMD_RCGR    (DISP_CC_BASE + 0x10F4)
#define DISP_CC_MDSS_AHB_CMD_RCGR     (DISP_CC_BASE + 0x1170)
#define DISP_CC_XO_CBCR               (DISP_CC_BASE + 0x5008)

//
// GDSC control register (gdsc.c).
//
#define GDSC_PWR_ON              BIT31
#define GDSC_EN_REST_WAIT_SHIFT  20
#define GDSC_EN_REST_WAIT_MASK   (0xF << GDSC_EN_REST_WAIT_SHIFT)
#define GDSC_EN_FEW_WAIT_SHIFT   16
#define GDSC_EN_FEW_WAIT_MASK    (0xF << GDSC_EN_FEW_WAIT_SHIFT)
#define GDSC_CLK_DIS_WAIT_SHIFT  12
#define GDSC_CLK_DIS_WAIT_MASK   (0xF << GDSC_CLK_DIS_WAIT_SHIFT)
#define GDSC_RETAIN_FF_ENABLE    BIT11
#define GDSC_SW_OVERRIDE         BIT2
#define GDSC_HW_CONTROL          BIT1
#define GDSC_SW_COLLAPSE         BIT0

//
// DSI reference generator, sm8250 variant (qcom-refgen-regulator.c).
//
#define REFGEN_BASE                 0x088E7000
#define REFGEN_REG_PWRDWN_CTRL5     (REFGEN_BASE + 0x80)
#define REFGEN_PWRDWN_CTRL5_ENABLE  BIT0

//
// Hardware version registers, and what they read on sc7280 (the DPU
// catalog's 7.2 and DSI 6G v2.5.0, dsi_cfg.h).
//
#define MDSS_HW_VERSION           0x0AE00000
#define MDP_HW_VERSION            0x0AE01000
#define DSI_6G_HW_VERSION         0x0AE94000
#define MDSS_HW_VERSION_SC7280    0x70020000
#define DSI_6G_HW_VERSION_SC7280  0x20050000

//
// RPMh resources the display votes for. Addresses from the command DB of
// this board's firmware (read on the running board). The command DB
// itself sits in a carve-out that is not mapped in DXE.
//
#define RPMH_ADDR_LDOB6    0x41900  // PM7325 LDO6 (L6B): DSI vdda, 1.2 V
#define RPMH_ADDR_LDOC10   0x40B00  // PM8350C LDO10 (L10C): DSI PHY vdds, 0.88 V
#define RPMH_ADDR_BCM_MM0  0x50054  // MMNOC HF to memory NoC, VCD 4
#define RPMH_ADDR_BCM_MM1  0x50058  // MDP/camera HF masters, VCD 4

//
// VRM registers of a regulator resource (qcom-rpmh-regulator.c).
//
#define RPMH_VRM_VOLTAGE           0x0    // mV
#define RPMH_VRM_ENABLE            0x4
#define RPMH_VRM_MODE              0x8
#define RPMH_VRM_VOLTAGE_MASK      0x1FFF
#define RPMH_VRM_ENABLE_MASK       0x1
#define RPMH_VRM_MODE_MASK         0x7
#define RPMH_PMIC5_LDO_MODE_LPM    4
#define RPMH_PMIC5_LDO_MODE_HPM    7

//
// Bus clock manager vote (include/soc/qcom/tcs.h BCM_TCS_CMD): commit in
// bit 30, valid in bit 29, average (x) in bits 27:14, peak (y) in bits 13:0.
//
#define RPMH_BCM_COMMIT      BIT30
#define RPMH_BCM_VALID       BIT29
#define RPMH_BCM_VOTE_MASK   0x3FFF
#define RPMH_BCM_VOTE_X_SHIFT  14
#define RPMH_BCM_CMD(Commit, Valid, X, Y)                                  \
  (((Commit) ? RPMH_BCM_COMMIT : 0) | ((Valid) ? RPMH_BCM_VALID : 0) |     \
   (((UINT32)(X) & RPMH_BCM_VOTE_MASK) << RPMH_BCM_VOTE_X_SHIFT) |         \
   ((UINT32)(Y) & RPMH_BCM_VOTE_MASK))

//
// One command of an RPMh request.
//
typedef struct {
  UINT32     Addr;
  UINT32     Data;
  BOOLEAN    Wait;      // ask for a response and wait for it
} RPMH_CMD;

//
// RPMh through the apps RSC (Rpmh.c).
//

/**
  Checks that DRV2 of the apps RSC has the register layout and the active
  TCSes this driver expects.

  @retval EFI_SUCCESS      Usable.
  @retval EFI_UNSUPPORTED  Unknown RSC version or TCS configuration.
**/
EFI_STATUS
PowerRpmhInit (
  VOID
  );

/**
  Sends an active-only (AMC) write request and waits until it has been
  sent, and answered for the commands that ask for a response. Leaves the
  TCS it used idle and clean.

  @param[in]  Cmds   The commands, all sent in one TCS, in order.
  @param[in]  Count  How many.

  @retval EFI_SUCCESS            Done.
  @retval EFI_INVALID_PARAMETER  Too many commands for one TCS.
  @retval EFI_NOT_READY          No active TCS is idle.
  @retval EFI_TIMEOUT            The request did not complete.
  @retval EFI_UNSUPPORTED        The RSC is not usable.
**/
EFI_STATUS
PowerRpmhWrite (
  IN CONST RPMH_CMD  *Cmds,
  IN UINTN           Count
  );

/**
  Reads the vote of an RPMh resource address. Leaves the TCS it used idle
  and clean.

  @param[in]   Addr  The resource address.
  @param[out]  Data  The response.

  @retval EFI_SUCCESS  Read.
  @retval Other        As for PowerRpmhWrite().
**/
EFI_STATUS
PowerRpmhRead (
  IN  UINT32  Addr,
  OUT UINT32  *Data
  );

/**
  Logs DRV2 of the apps RSC: version, TCS configuration, interrupt state and,
  for each TCS, its control words and the commands it holds. Reads only.

  @param[in]  When  A label for the log lines.
**/
VOID
PowerRpmhLogState (
  IN CONST CHAR8  *When
  );

//
// Diagnostics (Diag.c).
//

/**
  Logs, reading only, the GCC display clocks, REFGEN, the DISPCC clocks and
  GDSC when the DISPCC can be accessed, and the MDSS, DPU and DSI hardware
  versions when their power domain and clocks are on.

  @param[in]  When  A label for the log lines.
**/
VOID
PowerDiagLogClocks (
  IN CONST CHAR8  *When
  );

#endif // MDSS_POWER_H_
