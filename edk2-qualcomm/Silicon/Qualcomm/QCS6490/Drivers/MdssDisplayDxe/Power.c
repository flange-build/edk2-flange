/** @file
  Power and clocks of the display subsystem: RPMh votes, the DSI REFGEN, the
  GCC display clocks, the MDSS core GDSC and the DISPCC clocks of MDSS, the
  DPU and the DSI0 escape clock.

  The steps and their order are those of the vendor Linux kernel on this
  board, which the register dump of a running 1080p60 pipeline confirms:
  - RPMh: the DSI rails, and MMNOC bandwidth for the MDP before any clock
    (msm_mdss.c: AXI clocks only run with the interconnect voted).
  - REFGEN, which the DSI PHY waits for (qcom-refgen-regulator.c).
  - GCC_DISP_AHB/GCC_DISP_XO and DISP_CC_XO, which the GCC and DISPCC
    drivers force on when they probe.
  - The shared roots (AHB, MDP) parked on XO, as clk_rcg2_shared_init does,
    so that the GDSC cannot wedge on a root stuck on a stopped source.
  - The MDSS core GDSC (gdsc.c).
  - The clocks in the order MDSS, then the DPU, then DSI0 turn them on (the
    clocks properties in sc7280.dtsi), each root before its branch.

  The GDSC stays under software control. Linux hands it to the hardware
  (HW_CTRL) after power-up so that the MDSS can collapse it when idle and
  runtime PM is in charge; here nothing would restore the state lost in a
  collapse, and Linux's gdsc_init clears HW_CTRL again when it probes.

  Each register bit changed is recorded with its value at entry, and
  DisplayPowerOff(), or a failure, undoes the steps in reverse order.

  Copyright (c) 2026, edk2-flange contributors.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include "MdssDisplay.h"
#include "MdssPower.h"

//
// Read the RPMh votes of the display resources back, at entry and after
// voting, for the log. Linux never sends reads; set to FALSE to send writes
// only.
//
#define POWER_LOG_RPMH_VOTES  TRUE

//
// DSI rails (qcs6490-thundercomm-rubikpi3.dtsi vreg_l6b_1p2 1.14-1.26 V,
// vreg_l10c_0p88 0.88-1.05 V), in high power mode: the PHY draws 37.55 mA
// from L10C (dsi_phy_7nm.c), above the 30 mA limit of a PMIC5 NLDO in low
// power mode.
//
#define DSI_VDDA_MV      1200
#define DSI_PHY_VDDS_MV  880

//
// Scan-out bandwidth as the DPU computes it (dpu_plane.c, dpu_core_perf.c,
// catalog dpu_7_2_sc7280.h): an XRGB8888 plane fetched at
// width x vtotal x fps, times the bus inefficiency factor, as the average;
// the catalog's minimum DRAM ib as the peak, above the 400 MB/s msm_mdss.c
// votes on the same path.
//
#define DPU_BYTES_PER_PIXEL      4
#define DPU_BW_INEFFICIENCY_PCT  120
#define DPU_MIN_DRAM_IB_KBPS     1600000
#define MDSS_MIN_IB_KBPS         400000

//
// BCM arithmetic (bcm-voter.c bcm_aggregate): unit and width from the
// command DB aux data of MM0/MM1, bus width and channels of the MDP path
// nodes (interconnect/qcom/sc7280.c).
//
#define BCM_VOTE_SCALE           1000
#define BCM_MM_UNIT              2350000
#define BCM_MM0_WIDTH            64
#define BCM_MM1_WIDTH            32
#define QXM_MDP0_BUSWIDTH        32
#define QXM_MDP0_CHANNELS        1
#define QNS_MEM_NOC_HF_BUSWIDTH  32
#define QNS_MEM_NOC_HF_CHANNELS  2

//
// Root configurations (dispcc-sc7280.c frequency tables, CFG = source << 8 |
// (2 x divider - 1)).
//
#define RCG_CFG_SAFE        0x000   // XO undivided: where clk_rcg2 parks a shared root
#define RCG_CFG_XO_19_2MHZ  0x001   // XO / 1
#define RCG_CFG_MDP_200MHZ  0x405   // GCC_DISP_GPLL0 (600 MHz, source 4) / 3

//
// MDSS core GDSC wait times (dispcc-sc7280.c), and the fields gdsc_init
// sets: no hardware trigger, no software override of the sequencer.
//
#define MDSS_GDSC_WAIT_VALUES  ((0x2 << GDSC_EN_REST_WAIT_SHIFT) |   \
                                (0x2 << GDSC_EN_FEW_WAIT_SHIFT) |    \
                                (0xF << GDSC_CLK_DIS_WAIT_SHIFT))
#define MDSS_GDSC_INIT_MASK    (GDSC_HW_CONTROL | GDSC_SW_OVERRIDE |    \
                                GDSC_EN_REST_WAIT_MASK |                \
                                GDSC_EN_FEW_WAIT_MASK |                 \
                                GDSC_CLK_DIS_WAIT_MASK)

//
// Timeouts and delays of clk-branch.c, clk-rcg2.c, gdsc.c and the REFGEN
// enable time.
//
#define BRANCH_HALT_TIMEOUT_US  200
#define BRANCH_HALT_DELAY_US    10
#define RCG_TIMEOUT_US          500
#define GDSC_TIMEOUT_US         2000
#define REFGEN_ENABLE_TIME_US   5

//
// How Linux waits for a branch clock (clk-branch.h halt_check).
//
typedef enum {
  PowerHalt,          // CLK_OFF polled on enable and on disable
  PowerHaltVoted,     // polled on enable, a delay on disable
  PowerHaltDelay,     // a delay only
  PowerHaltSkip       // nothing
} POWER_HALT;

//
// An enable bit: a clock branch, a clock vote, the REFGEN enable.
//
typedef struct {
  CONST CHAR8    *Name;
  UINTN          Address;
  UINT32         Mask;
  POWER_HALT     Halt;
  UINT32         Entry;         // the Mask bits at entry
  BOOLEAN        Saved;
} POWER_SWITCH;

//
// A root clock generator.
//
typedef struct {
  CONST CHAR8    *Name;
  UINTN          CmdRcgr;
  BOOLEAN        Shared;        // clk_rcg2_shared_ops: parked on XO when off
  UINT32         EntryCfg;
  BOOLEAN        Saved;
} POWER_RCG;

//
// An RPMh address read back for the log.
//
typedef struct {
  CONST CHAR8    *Name;
  UINT32         Addr;
} POWER_RPMH_RESOURCE;

//
// The enable bits DisplayPowerOn() sets, with the halt check Linux uses for
// each. GCC_DISP_AHB and GCC_DISP_XO are forced on by the gcc-sc7280.c probe
// without a check (GCC_DISP_AHB runs under hardware gating and can read
// off), DISP_CC_XO likewise by the dispcc-sc7280.c probe.
//
STATIC POWER_SWITCH  mRefgen = {
  "REFGEN", REFGEN_REG_PWRDWN_CTRL5, REFGEN_PWRDWN_CTRL5_ENABLE, PowerHaltSkip, 0, FALSE
};
STATIC POWER_SWITCH  mGccDispAhb = {
  "GCC_DISP_AHB", GCC_DISP_AHB_CBCR, CBCR_CLK_ENABLE, PowerHaltSkip, 0, FALSE
};
STATIC POWER_SWITCH  mGccDispXo = {
  "GCC_DISP_XO", GCC_DISP_XO_CBCR, CBCR_CLK_ENABLE, PowerHaltSkip, 0, FALSE
};
STATIC POWER_SWITCH  mDispCcXo = {
  "DISP_CC_XO", DISP_CC_XO_CBCR, CBCR_CLK_ENABLE, PowerHaltSkip, 0, FALSE
};
STATIC POWER_SWITCH  mGccDispGpll0 = {
  "GCC_DISP_GPLL0", GCC_APCS_CLOCK_BRANCH_ENA_VOTE, GCC_DISP_GPLL0_CLK_SRC_ENA, PowerHaltDelay, 0, FALSE
};
STATIC POWER_SWITCH  mMdssAhb = {
  "MDSS_AHB", DISP_CC_MDSS_AHB_CBCR, CBCR_CLK_ENABLE, PowerHalt, 0, FALSE
};
STATIC POWER_SWITCH  mMdssMdp = {
  "MDSS_MDP", DISP_CC_MDSS_MDP_CBCR, CBCR_CLK_ENABLE, PowerHalt, 0, FALSE
};
STATIC POWER_SWITCH  mGccDispHfAxi = {
  "GCC_DISP_HF_AXI", GCC_DISP_HF_AXI_CBCR, CBCR_CLK_ENABLE, PowerHaltSkip, 0, FALSE
};
STATIC POWER_SWITCH  mGccDispSfAxi = {
  "GCC_DISP_SF_AXI", GCC_DISP_SF_AXI_CBCR, CBCR_CLK_ENABLE, PowerHaltSkip, 0, FALSE
};
STATIC POWER_SWITCH  mMdssMdpLut = {
  "MDSS_MDP_LUT", DISP_CC_MDSS_MDP_LUT_CBCR, CBCR_CLK_ENABLE, PowerHaltVoted, 0, FALSE
};
STATIC POWER_SWITCH  mMdssVsync = {
  "MDSS_VSYNC", DISP_CC_MDSS_VSYNC_CBCR, CBCR_CLK_ENABLE, PowerHalt, 0, FALSE
};
STATIC POWER_SWITCH  mMdssEsc0 = {
  "MDSS_ESC0", DISP_CC_MDSS_ESC0_CBCR, CBCR_CLK_ENABLE, PowerHalt, 0, FALSE
};

STATIC POWER_RCG  mAhbRcg   = { "MDSS_AHB", DISP_CC_MDSS_AHB_CMD_RCGR, TRUE, 0, FALSE };
STATIC POWER_RCG  mMdpRcg   = { "MDSS_MDP", DISP_CC_MDSS_MDP_CMD_RCGR, TRUE, 0, FALSE };
STATIC POWER_RCG  mVsyncRcg = { "MDSS_VSYNC", DISP_CC_MDSS_VSYNC_CMD_RCGR, FALSE, 0, FALSE };
STATIC POWER_RCG  mEsc0Rcg  = { "MDSS_ESC0", DISP_CC_MDSS_ESC0_CMD_RCGR, FALSE, 0, FALSE };

STATIC CONST POWER_RPMH_RESOURCE  mRpmhLogged[] = {
  { "L6B voltage",   RPMH_ADDR_LDOB6 + RPMH_VRM_VOLTAGE  },
  { "L6B enable",    RPMH_ADDR_LDOB6 + RPMH_VRM_ENABLE   },
  { "L6B mode",      RPMH_ADDR_LDOB6 + RPMH_VRM_MODE     },
  { "L10C voltage",  RPMH_ADDR_LDOC10 + RPMH_VRM_VOLTAGE },
  { "L10C enable",   RPMH_ADDR_LDOC10 + RPMH_VRM_ENABLE  },
  { "L10C mode",     RPMH_ADDR_LDOC10 + RPMH_VRM_MODE    },
  //
  // No BCM reads: nothing known to work reads a BCM back, and a read that
  // never completes would leave a TCS busy for Linux.
  //
};

STATIC UINT32   mGdscEntry;
STATIC BOOLEAN  mGdscSaved;
STATIC BOOLEAN  mGdscPoweredOn;
STATIC BOOLEAN  mBandwidthVoted;
STATIC BOOLEAN  mRpmhReadFailed;
STATIC BOOLEAN  mPowerOn;

/**
  Changes bits of a register, writing only if that changes it, as Linux
  regmap_update_bits does.

  @param[in]  Address  The register.
  @param[in]  Mask     The bits.
  @param[in]  Value    Their new value.
**/
STATIC
VOID
PowerUpdateBits (
  IN UINTN   Address,
  IN UINT32  Mask,
  IN UINT32  Value
  )
{
  UINT32  Old;
  UINT32  New;

  Old = MmioRead32 (Address);
  New = (Old & ~Mask) | (Value & Mask);
  if (New != Old) {
    MmioWrite32 (Address, New);
  }
}

/**
  Sets an enable bit, recording its entry value, and waits for the clock the
  way Linux clk_branch2 does for its halt type.

  @param[in,out]  Switch  The enable bit.

  @retval EFI_SUCCESS  On.
  @retval EFI_TIMEOUT  The branch still reads off.
**/
STATIC
EFI_STATUS
PowerSwitchOn (
  IN OUT POWER_SWITCH  *Switch
  )
{
  UINT32      Old;
  EFI_STATUS  Status;

  Old = MmioRead32 (Switch->Address);
  if (!Switch->Saved) {
    Switch->Entry = Old & Switch->Mask;
    Switch->Saved = TRUE;
  }

  PowerUpdateBits (Switch->Address, Switch->Mask, Switch->Mask);

  Status = EFI_SUCCESS;
  switch (Switch->Halt) {
    case PowerHalt:
    case PowerHaltVoted:
      Status = MmioPoll32 (Switch->Address, CBCR_CLK_OFF, 0, BRANCH_HALT_TIMEOUT_US);
      break;

    case PowerHaltDelay:
      MicroSecondDelay (BRANCH_HALT_DELAY_US);
      break;

    default:
      break;
  }

  DEBUG ((
    EFI_ERROR (Status) ? DEBUG_ERROR : DEBUG_INFO,
    "%a: %a %a: 0x%x -> 0x%x\n",
    __func__,
    Switch->Name,
    EFI_ERROR (Status) ? "did not start" : "on",
    Old,
    MmioRead32 (Switch->Address)
    ));

  return Status;
}

/**
  Puts an enable bit back to its entry value and, when that turns a clock
  off, waits the way Linux clk_branch2 does.

  @param[in,out]  Switch  The enable bit.
**/
STATIC
VOID
PowerSwitchRestore (
  IN OUT POWER_SWITCH  *Switch
  )
{
  UINT32  Old;

  if (!Switch->Saved) {
    return;
  }

  Switch->Saved = FALSE;
  Old           = MmioRead32 (Switch->Address);
  if ((Old & Switch->Mask) == Switch->Entry) {
    return;
  }

  PowerUpdateBits (Switch->Address, Switch->Mask, Switch->Entry);

  switch (Switch->Halt) {
    case PowerHalt:
      if (EFI_ERROR (MmioPoll32 (Switch->Address, CBCR_CLK_OFF, CBCR_CLK_OFF, BRANCH_HALT_TIMEOUT_US))) {
        DEBUG ((DEBUG_WARN, "%a: %a still reads on (0x%x)\n", __func__, Switch->Name, MmioRead32 (Switch->Address)));
      }

      break;

    case PowerHaltVoted:
    case PowerHaltDelay:
      MicroSecondDelay (BRANCH_HALT_DELAY_US);
      break;

    default:
      break;
  }

  DEBUG ((DEBUG_INFO, "%a: %a restored: 0x%x -> 0x%x\n", __func__, Switch->Name, Old, MmioRead32 (Switch->Address)));
}

/**
  Returns whether a root configuration selects XO undivided (19.2 MHz).

  @param[in]  Cfg  The CFG_RCGR value.

  @return  TRUE if it does.
**/
STATIC
BOOLEAN
PowerRcgIsXo (
  IN UINT32  Cfg
  )
{
  //
  // A divider field of 0 (bypass) and of 1 both divide by one.
  //
  return ((Cfg & (RCG_CFG_SRC_SEL_MASK | RCG_CFG_MODE_MASK)) == 0) &&
         ((Cfg & RCG_CFG_SRC_DIV_MASK) <= 1);
}

/**
  Switches a root clock generator to a configuration: CFG_RCGR, then the
  UPDATE handshake (clk-rcg2.c update_config). A shared root is forced on
  around the switch, so that the switch happens on a running root whatever
  its branches do (clk_rcg2_shared_force_enable_clear). The current and the
  new source must both run.

  @param[in,out]  Rcg  The root.
  @param[in]      Cfg  The configuration.

  @retval EFI_SUCCESS  Switched.
  @retval EFI_TIMEOUT  The root did not turn on or did not switch.
**/
STATIC
EFI_STATUS
PowerRcgSet (
  IN OUT POWER_RCG  *Rcg,
  IN     UINT32     Cfg
  )
{
  UINTN       Cmd;
  UINTN       CfgRcgr;
  UINT32      Old;
  EFI_STATUS  Status;

  Cmd     = Rcg->CmdRcgr + RCG_CMD;
  CfgRcgr = Rcg->CmdRcgr + RCG_CFG;
  Old     = MmioRead32 (CfgRcgr);
  if (!Rcg->Saved) {
    Rcg->EntryCfg = Old;
    Rcg->Saved    = TRUE;
  }

  if (Rcg->Shared) {
    PowerUpdateBits (Cmd, RCG_CMD_ROOT_EN, RCG_CMD_ROOT_EN);
    Status = MmioPoll32 (Cmd, RCG_CMD_ROOT_OFF, 0, RCG_TIMEOUT_US);
    if (EFI_ERROR (Status)) {
      DEBUG ((
        DEBUG_ERROR,
        "%a: %a root did not turn on (CMD 0x%x CFG 0x%x)\n",
        __func__,
        Rcg->Name,
        MmioRead32 (Cmd),
        Old
        ));
      PowerUpdateBits (Cmd, RCG_CMD_ROOT_EN, 0);
      return Status;
    }
  }

  MmioWrite32 (CfgRcgr, (Old & ~RCG_CFG_MASK) | Cfg);
  PowerUpdateBits (Cmd, RCG_CMD_UPDATE, RCG_CMD_UPDATE);
  Status = MmioPoll32 (Cmd, RCG_CMD_UPDATE, 0, RCG_TIMEOUT_US);

  if (Rcg->Shared) {
    PowerUpdateBits (Cmd, RCG_CMD_ROOT_EN, 0);
  }

  DEBUG ((
    EFI_ERROR (Status) ? DEBUG_ERROR : DEBUG_INFO,
    "%a: %a root CFG 0x%x -> 0x%x%a, CMD 0x%x\n",
    __func__,
    Rcg->Name,
    Old,
    MmioRead32 (CfgRcgr),
    EFI_ERROR (Status) ? " did not update" : "",
    MmioRead32 (Cmd)
    ));

  return Status;
}

/**
  Puts a root that is not shared on XO at 19.2 MHz unless it is there
  already. Linux leaves these roots alone when they already run at the rate
  asked for (clk_set_rate of the same rate), which is the reset state.

  @param[in,out]  Rcg  The root.

  @retval EFI_SUCCESS  On XO.
  @retval Other        From PowerRcgSet().
**/
STATIC
EFI_STATUS
PowerRcgUseXo (
  IN OUT POWER_RCG  *Rcg
  )
{
  UINT32  Cfg;

  Cfg = MmioRead32 (Rcg->CmdRcgr + RCG_CFG);
  if (PowerRcgIsXo (Cfg)) {
    DEBUG ((DEBUG_INFO, "%a: %a root on XO already (CFG 0x%x)\n", __func__, Rcg->Name, Cfg));
    return EFI_SUCCESS;
  }

  return PowerRcgSet (Rcg, RCG_CFG_XO_19_2MHZ);
}

/**
  Undoes PowerRcgSet(). A shared root is parked on XO as Linux parks it when
  its clock goes off (clk_rcg2_shared_disable), which is also its reset
  configuration. The other roots were only ever moved to XO and stay there:
  back on the source they had, they could be left on one that is stopped.

  @param[in,out]  Rcg  The root.
**/
STATIC
VOID
PowerRcgPark (
  IN OUT POWER_RCG  *Rcg
  )
{
  if (!Rcg->Saved) {
    return;
  }

  if (Rcg->Shared) {
    PowerRcgSet (Rcg, RCG_CFG_SAFE);
  }

  if (!PowerRcgIsXo (Rcg->EntryCfg)) {
    DEBUG ((DEBUG_WARN, "%a: %a root left on XO; its CFG was 0x%x at entry\n", __func__, Rcg->Name, Rcg->EntryCfg));
  }

  Rcg->Saved = FALSE;
}

/**
  Powers the MDSS core GDSC up under software control (gdsc.c gdsc_init and
  gdsc_enable).

  @retval EFI_SUCCESS       On.
  @retval EFI_DEVICE_ERROR  It did not report power.
**/
STATIC
EFI_STATUS
PowerGdscOn (
  VOID
  )
{
  UINT32      Gdscr;
  EFI_STATUS  Status;

  Gdscr      = MmioRead32 (DISP_CC_MDSS_CORE_GDSCR);
  mGdscEntry = Gdscr;
  mGdscSaved = TRUE;

  PowerUpdateBits (DISP_CC_MDSS_CORE_GDSCR, MDSS_GDSC_INIT_MASK, MDSS_GDSC_WAIT_VALUES);

  if ((Gdscr & GDSC_PWR_ON) != 0) {
    //
    // Not expected: nothing before this driver uses the display. Keep it
    // on and make sure it retains its flops, as gdsc_init does.
    //
    DEBUG ((DEBUG_WARN, "%a: MDSS GDSC on at entry (GDSCR 0x%x)\n", __func__, Gdscr));
    PowerUpdateBits (DISP_CC_MDSS_CORE_GDSCR, GDSC_RETAIN_FF_ENABLE, GDSC_RETAIN_FF_ENABLE);
    return EFI_SUCCESS;
  }

  mGdscPoweredOn = TRUE;
  PowerUpdateBits (DISP_CC_MDSS_CORE_GDSCR, GDSC_SW_COLLAPSE, 0);
  Status = MmioPoll32 (DISP_CC_MDSS_CORE_GDSCR, GDSC_PWR_ON, GDSC_PWR_ON, GDSC_TIMEOUT_US);
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "%a: MDSS GDSC did not power up (GDSCR 0x%x CFG_GDSCR 0x%x)\n",
      __func__,
      MmioRead32 (DISP_CC_MDSS_CORE_GDSCR),
      MmioRead32 (DISP_CC_MDSS_CORE_CFG_GDSCR)
      ));
    return EFI_DEVICE_ERROR;
  }

  //
  // Clocks of the domain take a few cycles to come back after power-up, and
  // must not be turned on within 400 ns of the memories getting power.
  //
  MicroSecondDelay (1);
  PowerUpdateBits (DISP_CC_MDSS_CORE_GDSCR, GDSC_RETAIN_FF_ENABLE, GDSC_RETAIN_FF_ENABLE);

  DEBUG ((
    DEBUG_INFO,
    "%a: MDSS GDSC on: GDSCR 0x%x -> 0x%x, CFG_GDSCR 0x%x\n",
    __func__,
    Gdscr,
    MmioRead32 (DISP_CC_MDSS_CORE_GDSCR),
    MmioRead32 (DISP_CC_MDSS_CORE_CFG_GDSCR)
    ));

  return EFI_SUCCESS;
}

/**
  Undoes PowerGdscOn(): collapses the GDSC if it was off at entry (gdsc.c
  gdsc_disable; the hardware trigger was never turned on). The wait times
  and the retention bit stay as Linux leaves them when the display is idle.
**/
STATIC
VOID
PowerGdscRestore (
  VOID
  )
{
  if (!mGdscSaved) {
    return;
  }

  mGdscSaved = FALSE;
  if (!mGdscPoweredOn) {
    PowerUpdateBits (DISP_CC_MDSS_CORE_GDSCR, GDSC_HW_CONTROL, mGdscEntry);
    return;
  }

  mGdscPoweredOn = FALSE;
  PowerUpdateBits (DISP_CC_MDSS_CORE_GDSCR, GDSC_SW_COLLAPSE, GDSC_SW_COLLAPSE);
  if (EFI_ERROR (MmioPoll32 (DISP_CC_MDSS_CORE_GDSCR, GDSC_PWR_ON, 0, GDSC_TIMEOUT_US))) {
    DEBUG ((
      DEBUG_ERROR,
      "%a: MDSS GDSC did not collapse (GDSCR 0x%x CFG_GDSCR 0x%x)\n",
      __func__,
      MmioRead32 (DISP_CC_MDSS_CORE_GDSCR),
      MmioRead32 (DISP_CC_MDSS_CORE_CFG_GDSCR)
      ));
    return;
  }

  DEBUG ((DEBUG_INFO, "%a: MDSS GDSC off: GDSCR 0x%x\n", __func__, MmioRead32 (DISP_CC_MDSS_CORE_GDSCR)));
}

/**
  Division of bcm-voter.c: a non-zero amount below one unit still counts as
  one.

  @param[in]  Num   The dividend.
  @param[in]  Base  The divisor.

  @return  The quotient.
**/
STATIC
UINT64
PowerBcmDiv (
  IN UINT64  Num,
  IN UINT64  Base
  )
{
  if ((Num != 0) && (Num < Base)) {
    return 1;
  }

  return DivU64x64Remainder (Num, Base, NULL);
}

/**
  Turns a bandwidth into a BCM vote (bcm-voter.c bcm_aggregate).

  @param[in]  KBps     The bandwidth, in kB/s.
  @param[in]  Width    The BCM width, from the command DB.
  @param[in]  Divisor  The node's bus width, times its channels for an
                       average vote.

  @return  The vote, clamped to the command field.
**/
STATIC
UINT32
PowerBcmVote (
  IN UINT64  KBps,
  IN UINT32  Width,
  IN UINT32  Divisor
  )
{
  UINT64  Vote;

  Vote = PowerBcmDiv (MultU64x32 (KBps, Width), Divisor);
  Vote = PowerBcmDiv (MultU64x32 (Vote, BCM_VOTE_SCALE), BCM_MM_UNIT);

  return (UINT32)MIN (Vote, RPMH_BCM_VOTE_MASK);
}

/**
  Votes MMNOC bandwidth for the MDP to memory path, or takes the vote back.

  The vote covers scan-out of the largest mode the driver drives, 1080p60.
  MM1 (qxm_mdp0) and MM0 (qns_mem_noc_hf) are the BCMs of the MMNOC part of
  the path; the memory NoC and DDR BCMs (SH0, MC0, ACV) are kept alive by
  every other master and have ample room for 0.6 GB/s, so they are not
  touched.

  @param[in]  On  TRUE to vote, FALSE to leave no vote from this DRV.

  @retval EFI_SUCCESS  Voted.
  @retval Other        From RpmhWrite().
**/
STATIC
EFI_STATUS
PowerVoteBandwidth (
  IN BOOLEAN  On
  )
{
  CONST DISPLAY_TIMING  *Timing;
  UINT32                Total;
  UINT32                Fps;
  UINT64                AvgKBps;
  UINT64                PeakKBps;
  UINT32                Mm1X;
  UINT32                Mm1Y;
  UINT32                Mm0X;
  UINT32                Mm0Y;
  RPMH_CMD              Cmds[2];

  Mm1X = 0;
  Mm1Y = 0;
  Mm0X = 0;
  Mm0Y = 0;

  if (On) {
    Timing = &gDisplayTiming1080p60;
    Total  = DISPLAY_H_TOTAL (Timing) * DISPLAY_V_TOTAL (Timing);
    Fps    = (UINT32)DivU64x32 (MultU64x32 (Timing->PixelClockKhz, 1000) + Total / 2, Total);

    AvgKBps = MultU64x32 ((UINT64)Timing->HActive * DISPLAY_V_TOTAL (Timing), Fps * DPU_BYTES_PER_PIXEL);
    AvgKBps = DivU64x32 (MultU64x32 (AvgKBps, DPU_BW_INEFFICIENCY_PCT), 100);
    AvgKBps = DivU64x32 (AvgKBps, 1000);
    PeakKBps = MAX (DPU_MIN_DRAM_IB_KBPS, MDSS_MIN_IB_KBPS);

    Mm1X = PowerBcmVote (AvgKBps, BCM_MM1_WIDTH, QXM_MDP0_BUSWIDTH * QXM_MDP0_CHANNELS);
    Mm1Y = PowerBcmVote (PeakKBps, BCM_MM1_WIDTH, QXM_MDP0_BUSWIDTH);
    Mm0X = PowerBcmVote (AvgKBps, BCM_MM0_WIDTH, QNS_MEM_NOC_HF_BUSWIDTH * QNS_MEM_NOC_HF_CHANNELS);
    Mm0Y = PowerBcmVote (PeakKBps, BCM_MM0_WIDTH, QNS_MEM_NOC_HF_BUSWIDTH);

    DEBUG ((
      DEBUG_INFO,
      "%a: MDP to memory %lu kB/s average, %lu kB/s peak: MM1 x %u y %u, MM0 x %u y %u\n",
      __func__,
      AvgKBps,
      PeakKBps,
      Mm1X,
      Mm1Y,
      Mm0X,
      Mm0Y
      ));
  }

  //
  // MM1 and MM0 share VCD 4: only the last command of a VCD commits, and
  // only the committing command asks for a response (bcm-voter.c
  // tcs_list_gen, qcom,tcs-wait defaulting to the active bucket). A zero
  // vote is sent as not valid, which is what "no vote" looks like.
  //
  Cmds[0].Addr = RPMH_ADDR_BCM_MM1;
  Cmds[0].Data = RPMH_BCM_CMD (FALSE, (Mm1X | Mm1Y) != 0, Mm1X, Mm1Y);
  Cmds[0].Wait = FALSE;
  Cmds[1].Addr = RPMH_ADDR_BCM_MM0;
  Cmds[1].Data = RPMH_BCM_CMD (TRUE, (Mm0X | Mm0Y) != 0, Mm0X, Mm0Y);
  Cmds[1].Wait = TRUE;

  return RpmhWrite (Cmds, ARRAY_SIZE (Cmds));
}

/**
  Votes a PMIC5 LDO on, at a voltage, in high power mode. Like
  qcom-rpmh-regulator.c, each setting goes as its own request and waits for
  its acknowledgement.

  @param[in]  Addr        The regulator's RPMh address.
  @param[in]  MilliVolts  The voltage.

  @retval EFI_SUCCESS  Voted.
  @retval Other        From RpmhWrite().
**/
STATIC
EFI_STATUS
PowerVoteRail (
  IN UINT32  Addr,
  IN UINT32  MilliVolts
  )
{
  RPMH_CMD    Cmd;
  EFI_STATUS  Status;

  Cmd.Wait = TRUE;

  Cmd.Addr = Addr + RPMH_VRM_VOLTAGE;
  Cmd.Data = MilliVolts;
  Status   = RpmhWrite (&Cmd, 1);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Cmd.Addr = Addr + RPMH_VRM_MODE;
  Cmd.Data = RPMH_PMIC5_LDO_MODE_HPM;
  Status   = RpmhWrite (&Cmd, 1);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Cmd.Addr = Addr + RPMH_VRM_ENABLE;
  Cmd.Data = 1;
  return RpmhWrite (&Cmd, 1);
}

/**
  Logs the RPMh votes of the display resources, read back through the RSC.
  After a failed read no more are sent.

  @param[in]  When  A label for the log lines.
**/
STATIC
VOID
PowerLogRpmhVotes (
  IN CONST CHAR8  *When
  )
{
  UINTN       Index;
  UINT32      Data;
  EFI_STATUS  Status;

  if (!POWER_LOG_RPMH_VOTES || mRpmhReadFailed) {
    return;
  }

  for (Index = 0; Index < ARRAY_SIZE (mRpmhLogged); Index++) {
    Status = RpmhRead (mRpmhLogged[Index].Addr, &Data);
    if (EFI_ERROR (Status)) {
      DEBUG ((
        DEBUG_ERROR,
        "%a: [%a] reading %a failed (%r), no more RPMh reads\n",
        __func__,
        When,
        mRpmhLogged[Index].Name,
        Status
        ));
      mRpmhReadFailed = TRUE;
      return;
    }

    DEBUG ((
      DEBUG_INFO,
      "%a: [%a] %a (0x%05x) reads 0x%x\n",
      __func__,
      When,
      mRpmhLogged[Index].Name,
      mRpmhLogged[Index].Addr,
      Data
      ));
  }
}

/**
  Checks that MDSS and the DPU are the sc7280 ones, and logs the DSI0
  controller version.

  @retval EFI_SUCCESS       They are.
  @retval EFI_DEVICE_ERROR  They are not, or do not answer.
**/
STATIC
EFI_STATUS
PowerCheckVersions (
  VOID
  )
{
  UINT32  Mdss;
  UINT32  Mdp;
  UINT32  Dsi;

  Mdss = MmioRead32 (MDSS_HW_VERSION);
  Mdp  = MmioRead32 (MDP_HW_VERSION);
  Dsi  = MmioRead32 (DSI_6G_HW_VERSION);

  DEBUG ((DEBUG_INFO, "%a: MDSS 0x%x, DPU 0x%x, DSI0 0x%x\n", __func__, Mdss, Mdp, Dsi));

  if ((Mdss != MDSS_HW_VERSION_SC7280) || (Mdp != MDSS_HW_VERSION_SC7280)) {
    DEBUG ((DEBUG_ERROR, "%a: expected MDSS and DPU 0x%x\n", __func__, MDSS_HW_VERSION_SC7280));
    return EFI_DEVICE_ERROR;
  }

  if (Dsi != DSI_6G_HW_VERSION_SC7280) {
    DEBUG ((DEBUG_WARN, "%a: expected DSI0 0x%x\n", __func__, DSI_6G_HW_VERSION_SC7280));
  }

  return EFI_SUCCESS;
}

/**
  Undoes whatever DisplayPowerOn() did, in reverse order. The rail votes
  stay: the UFS, USB and PCIe PHYs share both rails.
**/
STATIC
VOID
PowerUndo (
  VOID
  )
{
  PowerSwitchRestore (&mMdssEsc0);
  PowerRcgPark (&mEsc0Rcg);
  PowerSwitchRestore (&mMdssVsync);
  PowerRcgPark (&mVsyncRcg);
  PowerSwitchRestore (&mMdssMdpLut);
  PowerSwitchRestore (&mGccDispSfAxi);
  PowerSwitchRestore (&mGccDispHfAxi);

  //
  // The MDP root leaves GPLL0 while GPLL0 still runs.
  //
  PowerSwitchRestore (&mMdssMdp);
  PowerRcgPark (&mMdpRcg);
  PowerSwitchRestore (&mGccDispGpll0);
  PowerSwitchRestore (&mMdssAhb);
  PowerRcgPark (&mAhbRcg);

  PowerGdscRestore ();

  PowerSwitchRestore (&mDispCcXo);
  PowerSwitchRestore (&mGccDispXo);
  PowerSwitchRestore (&mGccDispAhb);
  PowerSwitchRestore (&mRefgen);

  //
  // Nothing before this driver votes the MMNOC from this DRV, so the entry
  // state is no vote (also what Linux puts in its sleep set for MM0).
  //
  if (mBandwidthVoted) {
    mBandwidthVoted = FALSE;
    PowerVoteBandwidth (FALSE);
  }
}

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
  )
{
  EFI_STATUS  Status;

  if (mPowerOn) {
    return EFI_SUCCESS;
  }

  DEBUG ((DEBUG_INFO, "%a: powering up MDSS\n", __func__));

  //
  // RPMh. The DSI rails are shared with the UFS, USB and PCIe PHYs and are
  // most likely on already; the votes make sure of the voltage, the enable
  // and high power mode.
  //
  Status = RpmhInit ();
  if (EFI_ERROR (Status)) {
    goto Fail;
  }

  PowerLogRpmhVotes ("entry");

  Status = PowerVoteRail (RPMH_ADDR_LDOB6, DSI_VDDA_MV);
  if (EFI_ERROR (Status)) {
    goto Fail;
  }

  Status = PowerVoteRail (RPMH_ADDR_LDOC10, DSI_PHY_VDDS_MV);
  if (EFI_ERROR (Status)) {
    goto Fail;
  }

  mBandwidthVoted = TRUE;
  Status          = PowerVoteBandwidth (TRUE);
  if (EFI_ERROR (Status)) {
    goto Fail;
  }

  PowerLogRpmhVotes ("voted");

  //
  // The DSI PHY waits for REFGEN_READY before it starts.
  //
  PowerSwitchOn (&mRefgen);
  MicroSecondDelay (REFGEN_ENABLE_TIME_US);

  //
  // GCC_DISP_AHB clocks the DISPCC and MDSS register interfaces.
  //
  PowerSwitchOn (&mGccDispAhb);
  PowerSwitchOn (&mGccDispXo);
  PowerSwitchOn (&mDispCcXo);

  //
  // Shared roots on XO before the GDSC powers up. XO at 19.2 MHz is also
  // the rate the AHB root runs at (assigned-clock-rates).
  //
  Status = PowerRcgSet (&mAhbRcg, RCG_CFG_SAFE);
  if (EFI_ERROR (Status)) {
    goto Fail;
  }

  Status = PowerRcgSet (&mMdpRcg, RCG_CFG_SAFE);
  if (EFI_ERROR (Status)) {
    goto Fail;
  }

  Status = PowerGdscOn ();
  if (EFI_ERROR (Status)) {
    goto Fail;
  }

  //
  // MDSS clocks: iface (GCC_DISP_AHB, on), ahb, core. The core runs at
  // 200 MHz, GPLL0 / 3, the lowest DPU operating point (CX at LOW_SVS, the
  // lowest active CX level); 1080p60 needs 136 MHz.
  //
  PowerSwitchOn (&mGccDispGpll0);

  Status = PowerSwitchOn (&mMdssAhb);
  if (EFI_ERROR (Status)) {
    goto Fail;
  }

  Status = PowerRcgSet (&mMdpRcg, RCG_CFG_MDP_200MHZ);
  if (EFI_ERROR (Status)) {
    goto Fail;
  }

  Status = PowerSwitchOn (&mMdssMdp);
  if (EFI_ERROR (Status)) {
    goto Fail;
  }

  //
  // DPU clocks: bus (HF AXI), nrt_bus (SF AXI), iface, lut, core, vsync.
  // Linux does not wait for the AXI branches; a HF AXI branch that stays
  // off is logged, as it points at a missing MMNOC vote.
  //
  PowerSwitchOn (&mGccDispHfAxi);
  PowerSwitchOn (&mGccDispSfAxi);
  if (EFI_ERROR (MmioPoll32 (GCC_DISP_HF_AXI_CBCR, CBCR_CLK_OFF, 0, BRANCH_HALT_TIMEOUT_US))) {
    DEBUG ((
      DEBUG_WARN,
      "%a: GCC_DISP_HF_AXI reads off (0x%x): is the MMNOC voted?\n",
      __func__,
      MmioRead32 (GCC_DISP_HF_AXI_CBCR)
      ));
  }

  Status = PowerSwitchOn (&mMdssMdpLut);
  if (EFI_ERROR (Status)) {
    goto Fail;
  }

  Status = PowerRcgUseXo (&mVsyncRcg);
  if (EFI_ERROR (Status)) {
    goto Fail;
  }

  Status = PowerSwitchOn (&mMdssVsync);
  if (EFI_ERROR (Status)) {
    goto Fail;
  }

  //
  // DSI0 escape clock, XO. Its byte and pixel clocks come from the DSI PLL
  // (DsiPhy.c).
  //
  Status = PowerRcgUseXo (&mEsc0Rcg);
  if (EFI_ERROR (Status)) {
    goto Fail;
  }

  Status = PowerSwitchOn (&mMdssEsc0);
  if (EFI_ERROR (Status)) {
    goto Fail;
  }

  Status = PowerCheckVersions ();
  if (EFI_ERROR (Status)) {
    goto Fail;
  }

  PowerDiagLogClocks ("powered");

  mPowerOn = TRUE;
  DEBUG ((DEBUG_INFO, "%a: MDSS powered\n", __func__));
  return EFI_SUCCESS;

Fail:
  DEBUG ((DEBUG_ERROR, "%a: failed: %r, undoing\n", __func__, Status));
  PowerDiagLogClocks ("failed");
  PowerUndo ();
  return Status;
}

/**
  Undoes DisplayPowerOn, restoring what it changed to the values found at
  entry, and leaves the RSC TCS it used idle and clean for the OS. Must run
  after DsiPhyDisable.
**/
VOID
DisplayPowerOff (
  VOID
  )
{
  DEBUG ((DEBUG_INFO, "%a: powering down MDSS\n", __func__));

  PowerUndo ();
  mPowerOn = FALSE;

  PowerDiagLogClocks ("off");
  RpmhLogState ("off");
}
