/** @file
  Read-only diagnostics of the display's power, clock, pin and RPMh state.

  Nothing here writes a register. A block is read only when it can be: the
  DISPCC only with GCC_DISP_AHB_CLK enabled, as that clock runs its register
  interface (GCC_DISP_AHB is normally under hardware gating, so its CLK_OFF
  bit reads 1 even when accesses work; the enable bit is what counts), and
  MDSS, the DPU and DSI0 only with the MDSS GDSC on and their interface and
  core clocks running (msm_mdss.c: "Register access requires
  MDSS_MDP_CLK"). Reading an unclocked block can hang the NoC and reset the
  board.

  Copyright (c) 2026, edk2-flange contributors.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include "MdssDisplay.h"
#include "MdssPower.h"

//
// TLMM (pinctrl-msm.c, pinctrl-sc7280.c): per GPIO a CFG and an IN_OUT
// register.
//
#define TLMM_BASE            0x0F100000
#define TLMM_GPIO_CFG(n)     (TLMM_BASE + 0x1000 * (UINTN)(n))
#define TLMM_GPIO_IN_OUT(n)  (TLMM_GPIO_CFG (n) + 0x4)
#define TLMM_CFG_PULL(x)     ((x) & 0x3)
#define TLMM_CFG_FUNC(x)     (((x) >> 2) & 0xF)
#define TLMM_CFG_DRV_MA(x)   (((((x) >> 6) & 0x7) + 1) * 2)
#define TLMM_CFG_OE          BIT9
#define TLMM_IO_IN           BIT0
#define TLMM_IO_OUT          BIT1

typedef struct {
  UINT32         Gpio;
  CONST CHAR8    *Name;
} DIAG_GPIO;

//
// The RUBIK Pi 3 pins around the LT9611 (qcs6490-thundercomm-rubikpi3.dtsi;
// GPIO40/41 are driven by the stock firmware only). None is in a reserved
// (secure) range.
//
STATIC CONST DIAG_GPIO  mDiagGpios[] = {
  { 20, "LT9611 IRQ"     },
  { 21, "LT9611 RESET_N" },
  { 36, "I2C9 SDA"       },
  { 37, "I2C9 SCL"       },
  { 40, "stock 5V_EN"    },
  { 41, "stock OCB"      },
  { 83, "LT9611 3V3_EN"  },
};

STATIC CONST CHAR8  *mDiagPull[] = { "no pull", "pull-down", "keeper", "pull-up" };

/**
  Returns whether a branch clock is enabled and reports running.

  @param[in]  Cbcr  Its CBCR.

  @return  TRUE if it runs.
**/
STATIC
BOOLEAN
DiagBranchRuns (
  IN UINTN  Cbcr
  )
{
  UINT32  Value;

  Value = MmioRead32 (Cbcr);
  return ((Value & CBCR_CLK_ENABLE) != 0) && ((Value & CBCR_CLK_OFF) == 0);
}

/**
  Logs the MDSS, DPU and DSI0 hardware versions, if the blocks can be read.

  @param[in]  When   A label for the log lines.
  @param[in]  Gdscr  The MDSS core GDSC register.
**/
STATIC
VOID
DiagLogVersions (
  IN CONST CHAR8  *When,
  IN UINT32       Gdscr
  )
{
  if (((Gdscr & GDSC_PWR_ON) == 0) ||
      !DiagBranchRuns (DISP_CC_MDSS_AHB_CBCR) ||
      !DiagBranchRuns (DISP_CC_MDSS_MDP_CBCR))
  {
    DEBUG ((DEBUG_INFO, "%a: [%a] MDSS GDSC or AHB/MDP clock off: MDSS, DPU and DSI0 not read\n", __func__, When));
    return;
  }

  DEBUG ((
    DEBUG_INFO,
    "%a: [%a] MDSS HW_VERSION 0x%x, DPU HW_VERSION 0x%x\n",
    __func__,
    When,
    MmioRead32 (MDSS_HW_VERSION),
    MmioRead32 (MDP_HW_VERSION)
    ));

  //
  // DSI0 also wants its bus clock, the HF AXI branch (dsi_bus_clk_enable).
  //
  if ((MmioRead32 (GCC_DISP_HF_AXI_CBCR) & CBCR_CLK_ENABLE) == 0) {
    DEBUG ((DEBUG_INFO, "%a: [%a] HF AXI clock off: DSI0 not read\n", __func__, When));
    return;
  }

  DEBUG ((DEBUG_INFO, "%a: [%a] DSI0 HW_VERSION 0x%x\n", __func__, When, MmioRead32 (DSI_6G_HW_VERSION)));
}

/**
  Logs, reading only, the GCC display clocks, REFGEN, the DISPCC clocks and
  GDSC when the DISPCC can be accessed, and the MDSS, DPU and DSI hardware
  versions when their power domain and clocks are on.

  @param[in]  When  A label for the log lines.
**/
VOID
PowerDiagLogClocks (
  IN CONST CHAR8  *When
  )
{
  UINT32  DispAhb;
  UINT32  Vote;
  UINT32  Gdscr;

  DispAhb = MmioRead32 (GCC_DISP_AHB_CBCR);
  Vote    = MmioRead32 (GCC_APCS_CLOCK_BRANCH_ENA_VOTE);

  DEBUG ((
    DEBUG_INFO,
    "%a: [%a] GCC DISP_AHB 0x%x DISP_XO 0x%x HF_AXI 0x%x SF_AXI 0x%x\n",
    __func__,
    When,
    DispAhb,
    MmioRead32 (GCC_DISP_XO_CBCR),
    MmioRead32 (GCC_DISP_HF_AXI_CBCR),
    MmioRead32 (GCC_DISP_SF_AXI_CBCR)
    ));
  DEBUG ((
    DEBUG_INFO,
    "%a: [%a] GCC ENA_VOTE 0x%x (DISP_GPLL0 %u), GPLL0 vote %u; REFGEN PWRDWN_CTRL5 0x%x\n",
    __func__,
    When,
    Vote,
    (Vote & GCC_DISP_GPLL0_CLK_SRC_ENA) != 0,
    (MmioRead32 (GCC_APCS_GPLL0_ENA_VOTE) & GCC_GPLL0_ENA) != 0,
    MmioRead32 (REFGEN_REG_PWRDWN_CTRL5)
    ));

  if ((DispAhb & CBCR_CLK_ENABLE) == 0) {
    DEBUG ((DEBUG_INFO, "%a: [%a] GCC_DISP_AHB disabled: DISPCC and MDSS not read\n", __func__, When));
    return;
  }

  Gdscr = MmioRead32 (DISP_CC_MDSS_CORE_GDSCR);
  DEBUG ((
    DEBUG_INFO,
    "%a: [%a] MDSS_CORE_GDSCR 0x%x (%a%a) CFG_GDSCR 0x%x, DISP_CC_XO 0x%x\n",
    __func__,
    When,
    Gdscr,
    ((Gdscr & GDSC_PWR_ON) != 0) ? "on" : "off",
    ((Gdscr & GDSC_HW_CONTROL) != 0) ? ", HW control" : "",
    MmioRead32 (DISP_CC_MDSS_CORE_CFG_GDSCR),
    MmioRead32 (DISP_CC_XO_CBCR)
    ));
  DEBUG ((
    DEBUG_INFO,
    "%a: [%a] CBCR AHB 0x%x MDP 0x%x LUT 0x%x VSYNC 0x%x ESC0 0x%x\n",
    __func__,
    When,
    MmioRead32 (DISP_CC_MDSS_AHB_CBCR),
    MmioRead32 (DISP_CC_MDSS_MDP_CBCR),
    MmioRead32 (DISP_CC_MDSS_MDP_LUT_CBCR),
    MmioRead32 (DISP_CC_MDSS_VSYNC_CBCR),
    MmioRead32 (DISP_CC_MDSS_ESC0_CBCR)
    ));
  DEBUG ((
    DEBUG_INFO,
    "%a: [%a] CBCR BYTE0 0x%x BYTE0_INTF 0x%x PCLK0 0x%x\n",
    __func__,
    When,
    MmioRead32 (DISP_CC_MDSS_BYTE0_CBCR),
    MmioRead32 (DISP_CC_MDSS_BYTE0_INTF_CBCR),
    MmioRead32 (DISP_CC_MDSS_PCLK0_CBCR)
    ));
  DEBUG ((
    DEBUG_INFO,
    "%a: [%a] RCG CMD/CFG AHB 0x%x/0x%x MDP 0x%x/0x%x VSYNC 0x%x/0x%x ESC0 0x%x/0x%x\n",
    __func__,
    When,
    MmioRead32 (DISP_CC_MDSS_AHB_CMD_RCGR + RCG_CMD),
    MmioRead32 (DISP_CC_MDSS_AHB_CMD_RCGR + RCG_CFG),
    MmioRead32 (DISP_CC_MDSS_MDP_CMD_RCGR + RCG_CMD),
    MmioRead32 (DISP_CC_MDSS_MDP_CMD_RCGR + RCG_CFG),
    MmioRead32 (DISP_CC_MDSS_VSYNC_CMD_RCGR + RCG_CMD),
    MmioRead32 (DISP_CC_MDSS_VSYNC_CMD_RCGR + RCG_CFG),
    MmioRead32 (DISP_CC_MDSS_ESC0_CMD_RCGR + RCG_CMD),
    MmioRead32 (DISP_CC_MDSS_ESC0_CMD_RCGR + RCG_CFG)
    ));
  DEBUG ((
    DEBUG_INFO,
    "%a: [%a] RCG CMD/CFG BYTE0 0x%x/0x%x PCLK0 0x%x/0x%x, BYTE0 DIV 0x%x\n",
    __func__,
    When,
    MmioRead32 (DISP_CC_MDSS_BYTE0_CMD_RCGR + RCG_CMD),
    MmioRead32 (DISP_CC_MDSS_BYTE0_CMD_RCGR + RCG_CFG),
    MmioRead32 (DISP_CC_MDSS_PCLK0_CMD_RCGR + RCG_CMD),
    MmioRead32 (DISP_CC_MDSS_PCLK0_CMD_RCGR + RCG_CFG),
    MmioRead32 (DISP_CC_MDSS_BYTE0_DIV_CDIVR)
    ));

  DiagLogVersions (When, Gdscr);
}

/**
  Logs, without writing anything, the state the display hardware is in when
  the driver starts: GCC/DISPCC clocks and GDSC, REFGEN, the LT9611 GPIOs, the
  RSC, and the MDSS/DSI version registers if their clocks are on.
**/
VOID
DisplayLogEntryState (
  VOID
  )
{
  UINTN   Index;
  UINT32  Cfg;
  UINT32  Io;

  PowerDiagLogClocks ("entry");

  for (Index = 0; Index < ARRAY_SIZE (mDiagGpios); Index++) {
    Cfg = MmioRead32 (TLMM_GPIO_CFG (mDiagGpios[Index].Gpio));
    Io  = MmioRead32 (TLMM_GPIO_IN_OUT (mDiagGpios[Index].Gpio));
    DEBUG ((
      DEBUG_INFO,
      "%a: GPIO%u %a: CFG 0x%x (func %u, %a, %u mA, %a) IO 0x%x (in %u, out %u)\n",
      __func__,
      mDiagGpios[Index].Gpio,
      mDiagGpios[Index].Name,
      Cfg,
      TLMM_CFG_FUNC (Cfg),
      mDiagPull[TLMM_CFG_PULL (Cfg)],
      TLMM_CFG_DRV_MA (Cfg),
      ((Cfg & TLMM_CFG_OE) != 0) ? "output" : "input",
      Io,
      (Io & TLMM_IO_IN) != 0,
      (Io & TLMM_IO_OUT) != 0
      ));
  }

  PowerRpmhLogState ("entry");
}
