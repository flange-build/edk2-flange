/** @file
  PCIe0 bring-up for PciHostBridgeDxe: from cold to a trained link and a root
  port that PciBusDxe can enumerate.

  The sequence is the one of Linux 7.0.2, which runs it on this board with a
  Gen2 x1 link to the Renesas uPD720201 behind PCIe0:
  - qcom_pcie_host_init (pcie-qcom.c): PERST# asserted;
    qcom_pcie_init_2_7_0: the 13 clocks of the pcie0 node, the controller
    reset, the PARF set-up; the PHY power-on; qcom_pcie_post_init_2_7_0
    (hot-plug capability cleared); PERST# released no less than 100 ms
    after it was asserted; qcom_pcie_config_sid_1_9_0 (requester ID to SMMU
    stream table).
  - phy-qcom-qmp-pcie.c qmp_pcie_init + qmp_pcie_power_on: PHY reset, the
    sm8250 gen3x1 tables (Pcie0Phy.c), start, wait for PHYSTATUS.
  - pcie-designware-host.c dw_pcie_setup_rc: the root port's header, link
    width, the iATU (one MEM window; region 0 is left to the PCI Segment
    Library for configuration requests).
  - qcom_pcie_start_link: equalization settings, LTSSM enabled through
    PARF_LTSSM; dw_pcie_wait_for_link.
  Ahead of all that comes what Linux gets from elsewhere: the board supplies
  of the devices on the link (fixed regulators in the device tree), the
  CLKREQ# pin (pinctrl), the PHY rails and the PCIe bandwidth (RPMh), and
  the power domain (runtime PM).

  What differs from Linux: the root port's subordinate bus is 1, the bus
  range PciHostBridgeDxe gets; it has no IO window, and IO decode is not
  enabled; ASPM stays off (Linux turns it on after enumeration).

  At ExitBootServices, after XhciDxe has halted the controller behind the
  link, link training is turned off and PERST# asserted (Qcs6490Pcie0Quiesce);
  the supplies stay on. Linux resets the controller, the PHY and the endpoint
  (both block resets, PERST#) before it uses them, but it turns the clocks
  it does not use yet off first (clk_disable_unused, seconds before
  pcie-qcom probes), the link's reference clock among them. A uPD720201
  whose firmware runs, left on a live link through that, came up at
  2.5 GT/s only after Linux's PERST#; held in reset meanwhile, it trains at
  5 GT/s and keeps its firmware.

  Copyright (c) 2026, edk2-flange contributors.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <IndustryStandard/Pci.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/PciSegmentLib.h>
#include <Library/Qcs6490RpmhLib.h>
#include <Library/Qcs6490TlmmLib.h>
#include <Library/TimerLib.h>

#include "Pcie0Init.h"

//
// PCIe0's CLKREQ#, GPIO88 in its pcie0_clkreqn function (kodiak.dtsi
// pcie0_clkreq_n, pinctrl-sc7280.c function 1), pulled up at 8 mA
// (qcs6490-thundercomm-rubikpi3.dts). It is the SoC's pin for PCIe0, not a
// board choice.
//
#define PCIE0_CLKREQ_GPIO      88
#define PCIE0_CLKREQ_FUNCTION  1

//
// Drive strength of PERST# (rubikpi3.dts pcie0_reset_n) and of the board
// power GPIOs (what Linux leaves them at).
//
#define PCIE0_GPIO_DRIVE_MA  8

//
// RPMh resources (this board's command DB). The PHY's vdda-phy is L10C and
// its vdda-pll L6B (rubikpi3.dts pcie0_phy), voted at the rail voltages in
// high power mode, as MdssDisplayDxe votes them for the DSI PHY: both are
// shared with the UFS, USB and DSI PHYs. The PCIe-to-memory path has two
// bus clock managers of its own, SN5 (xm_pcie3_0) and SN14
// (qns_pcie_mem_noc), both in VCD 3.
//
#define RPMH_ADDR_LDOC10          0x40B00
#define RPMH_ADDR_LDOB6           0x41900
#define RPMH_ADDR_BCM_SN5         0x50030
#define RPMH_ADDR_BCM_SN14        0x50050
#define RPMH_VRM_VOLTAGE          0x0   // mV
#define RPMH_VRM_ENABLE           0x4
#define RPMH_VRM_MODE             0x8
#define RPMH_PMIC5_LDO_MODE_HPM   7

#define PCIE0_VDDA_PHY_MV  880
#define PCIE0_VDDA_PLL_MV  1200

//
// Peak bandwidth vote of the 5 GT/s operating point of pcie0 (kodiak.dtsi
// pcie0_opp_table: 500000 kB/s on pcie-mem, no average vote), turned into
// BCM votes as bcm-voter.c does: kB/s x BCM width / node bus width x 1000 /
// BCM unit. SN5 (width 8, xm_pcie3_0 bus width 8) and SN14 (width 16,
// qns_pcie_mem_noc bus width 16) both have unit 530000 (command DB), so both
// get 500000 x 1000 / 530000 = 943. The uPD720201 is a 5 GT/s device.
//
#define PCIE0_BCM_PEAK_VOTE  943

//
// Delays and timeouts, from the Linux sources named.
//
#define PCIE0_BCR_ASSERT_US        1000    // qcom_pcie_init_2_7_0 usleep_range (1000, 1500)
#define PCIE0_BCR_DEASSERT_US      1000
#define PCIE0_PHY_BCR_ASSERT_US    250     // qmp_pcie_init usleep_range (200, 300)
#define PCIE0_PHY_BCR_DEASSERT_US  10      // Linux enables the PHY clocks next
#define PCIE0_PHY_START_US         1000    // qmp_pcie_power_on usleep_range (1000, 1200)
#define PCIE0_PHY_POLL_US          200
#define PCIE0_PHY_TIMEOUT_US       10000   // PHY_INIT_COMPLETE_TIMEOUT
#define PCIE0_PERST_MIN_US         100000  // PCIE_T_PVPERL_MS
#define PCIE0_PERST_DELAY_US       1000    // PERST_DELAY_US
#define PCIE0_LINK_RETRIES         10      // PCIE_LINK_WAIT_MAX_RETRIES
#define PCIE0_LINK_WAIT_US         90000   // PCIE_LINK_WAIT_SLEEP_MS
#define PCIE0_CONFIG_WAIT_US       100000  // PCIE_RESET_CONFIG_WAIT_MS
#define PCIE0_ENDPOINT_POLL_US     10000   // config requests may be retried for 1 s
#define PCIE0_ENDPOINT_TIMEOUT_US  1000000 // after reset (PCIe r6.0 sec 6.6.1)

//
// LTSSM states dw_pcie_wait_for_link tells apart (enum dw_pcie_ltssm).
//
#define DW_LTSSM_DETECT_ACT       0x01
#define DW_LTSSM_POLL_COMPLIANCE  0x03

//
// A branch clock and how Linux waits for it to run.
//
typedef struct {
  CONST CHAR8       *Name;
  UINTN             EnableReg;
  UINT32            EnableMask;
  UINTN             Cbcr;
  GCC_HALT_CHECK    Halt;
} PCIE0_CLOCK;

//
// The controller's clocks in the order of the clocks property of pcie0
// (kodiak.dtsi), which clk_bulk_prepare_enable follows. pipe_mux, phy_pipe
// and ref (the RPMh XO) have no branch here: the mux is set separately and
// the XO runs. Then the PHY's own, ref and refgen (its aux and cfg_ahb are
// the controller's). Enable bits and halt checks are gcc-sc7280.c's.
//
STATIC CONST PCIE0_CLOCK  mPcie0Clocks[] = {
  { "pcie_0_pipe_clk",                  GCC_APCS_CLOCK_BRANCH_ENA_VOTE_1, BIT4,  GCC_PCIE_0_PIPE_CBCR,                  GccHaltSkip  },
  { "pcie_0_aux_clk",                   GCC_APCS_CLOCK_BRANCH_ENA_VOTE_1, BIT3,  GCC_PCIE_0_AUX_CBCR,                   GccHaltVoted },
  { "pcie_0_cfg_ahb_clk",               GCC_APCS_CLOCK_BRANCH_ENA_VOTE_1, BIT2,  GCC_PCIE_0_CFG_AHB_CBCR,               GccHaltVoted },
  { "pcie_0_mstr_axi_clk",              GCC_APCS_CLOCK_BRANCH_ENA_VOTE_1, BIT1,  GCC_PCIE_0_MSTR_AXI_CBCR,              GccHaltSkip  },
  { "pcie_0_slv_axi_clk",               GCC_APCS_CLOCK_BRANCH_ENA_VOTE_1, BIT0,  GCC_PCIE_0_SLV_AXI_CBCR,               GccHaltVoted },
  { "pcie_0_slv_q2a_axi_clk",           GCC_APCS_CLOCK_BRANCH_ENA_VOTE_1, BIT5,  GCC_PCIE_0_SLV_Q2A_AXI_CBCR,           GccHaltVoted },
  { "aggre_noc_pcie_tbu_clk",           GCC_APCS_CLOCK_BRANCH_ENA_VOTE,   BIT18, GCC_AGGRE_NOC_PCIE_TBU_CBCR,           GccHaltVoted },
  { "ddrss_pcie_sf_clk",                GCC_APCS_CLOCK_BRANCH_ENA_VOTE,   BIT19, GCC_DDRSS_PCIE_SF_CBCR,                GccHaltSkip  },
  { "aggre_noc_pcie_center_sf_axi_clk", GCC_APCS_CLOCK_BRANCH_ENA_VOTE_1, BIT28, GCC_AGGRE_NOC_PCIE_CENTER_SF_AXI_CBCR, GccHaltVoted },
  { "aggre_noc_pcie_0_axi_clk",         GCC_APCS_CLOCK_BRANCH_ENA_VOTE,   BIT12, GCC_AGGRE_NOC_PCIE_0_AXI_CBCR,         GccHaltSkip  },
  { "pcie_clkref_en",                   GCC_PCIE_CLKREF_EN,               BIT0,  GCC_PCIE_CLKREF_EN,                    GccHalt      },
  { "pcie0_phy_rchng_clk",              GCC_APCS_CLOCK_BRANCH_ENA_VOTE,   BIT22, GCC_PCIE0_PHY_RCHNG_CBCR,              GccHaltVoted },
};

//
// iommu-map of pcie0 (kodiak.dtsi): requester ID to SMMU stream. The first
// entry's stream is the base that the table holds offsets from.
//
typedef struct {
  UINT16    Bdf;
  UINT16    Sid;
} PCIE0_SID_MAP;

STATIC CONST PCIE0_SID_MAP  mPcie0SidMap[] = {
  { 0x0000, 0x1C00 },   // 00:00.0, the root port
  { 0x0100, 0x1C01 },   // 01:00.0
};

/**
  Returns the time since a performance counter reading.

  @param[in]  StartTicks  The reading.

  @return  Microseconds.
**/
STATIC
UINT64
Pcie0ElapsedUs (
  IN UINT64  StartTicks
  )
{
  return DivU64x32 (GetTimeInNanoSecond (GetPerformanceCounter () - StartTicks), 1000);
}

/**
  Asserts PERST#: drives the pin low as a GPIO output (rubikpi3.dts
  pcie0_reset_n: GPIO function, no pull, 8 mA). Linux requests it as
  GPIOD_OUT_HIGH of an active-low line, which is the same level.

  @param[in]  Board  How the board wires PCIe0.

  @retval EFI_SUCCESS  Asserted.
  @retval Other        From TlmmDriveOutput().
**/
STATIC
EFI_STATUS
Pcie0AssertPerst (
  IN CONST PCIE0_BOARD  *Board
  )
{
  EFI_STATUS  Status;

  Status = TlmmDriveOutput (Board->PerstGpio, FALSE, PCIE0_GPIO_DRIVE_MA);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: PERST# GPIO%u: %r\n", __func__, Board->PerstGpio, Status));
    return Status;
  }

  DEBUG ((
    DEBUG_INFO,
    "%a: PERST# (GPIO%u) asserted: CFG 0x%x IO 0x%x\n",
    __func__,
    Board->PerstGpio,
    MmioRead32 (TLMM_GPIO_CFG (Board->PerstGpio)),
    MmioRead32 (TLMM_GPIO_IO (Board->PerstGpio))
    ));
  return EFI_SUCCESS;
}

/**
  Turns on the board supplies of the devices on the link: drives each power
  GPIO high in the board's order and waits its ramp time after it, the way
  the regulator core enables a chain of fixed regulators (rubikpi3.dts
  vreg_usbhub_pwr_1v8, vreg_eth_1v8, vreg_usbhub_rest_1v8, each with a
  50 ms regulator-enable-ramp-delay). A GPIO already driven high is left as
  it is, and its ramp time is not waited for again.

  @param[in]   Board     How the board wires PCIe0.
  @param[out]  Switched  Whether any supply was turned on.

  @retval EFI_SUCCESS  The supplies are on.
  @retval Other        From TlmmDriveOutput().
**/
STATIC
EFI_STATUS
Pcie0PowerOn (
  IN  CONST PCIE0_BOARD  *Board,
  OUT BOOLEAN            *Switched
  )
{
  UINTN       Index;
  UINT16      Gpio;
  EFI_STATUS  Status;

  *Switched = FALSE;

  for (Index = 0; Index < Board->PowerGpioCount; Index++) {
    Gpio = Board->PowerGpios[Index];

    if (TlmmIsDrivenHigh (Gpio)) {
      DEBUG ((DEBUG_INFO, "%a: power GPIO%u already driven high\n", __func__, Gpio));
      continue;
    }

    Status = TlmmDriveOutput (Gpio, TRUE, PCIE0_GPIO_DRIVE_MA);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "%a: power GPIO%u: %r\n", __func__, Gpio, Status));
      return Status;
    }

    *Switched = TRUE;
    MicroSecondDelay (Board->PowerRampUs);

    DEBUG ((
      DEBUG_INFO,
      "%a: power GPIO%u driven high, waited %u us: CFG 0x%x IO 0x%x\n",
      __func__,
      Gpio,
      Board->PowerRampUs,
      MmioRead32 (TLMM_GPIO_CFG (Gpio)),
      MmioRead32 (TLMM_GPIO_IO (Gpio))
      ));
  }

  return EFI_SUCCESS;
}

/**
  Hands the CLKREQ# pin to PCIe0 (pinctrl state pcie0_clkreq_n).

  @retval EFI_SUCCESS  Done.
  @retval Other        From TlmmConfigure().
**/
STATIC
EFI_STATUS
Pcie0ClkreqPin (
  VOID
  )
{
  EFI_STATUS  Status;

  Status = TlmmConfigure (
             PCIE0_CLKREQ_GPIO,
             PCIE0_CLKREQ_FUNCTION,
             TLMM_PULL_UP,
             PCIE0_GPIO_DRIVE_MA,
             FALSE
             );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: CLKREQ# GPIO%u: %r\n", __func__, PCIE0_CLKREQ_GPIO, Status));
    return Status;
  }

  DEBUG ((
    DEBUG_INFO,
    "%a: CLKREQ# (GPIO%u) to pcie0_clkreqn: CFG 0x%x IO 0x%x\n",
    __func__,
    PCIE0_CLKREQ_GPIO,
    MmioRead32 (TLMM_GPIO_CFG (PCIE0_CLKREQ_GPIO)),
    MmioRead32 (TLMM_GPIO_IO (PCIE0_CLKREQ_GPIO))
    ));
  return EFI_SUCCESS;
}

/**
  Votes a PMIC5 LDO on, at a voltage, in high power mode. Like
  qcom-rpmh-regulator.c (and MdssDisplayDxe), each setting goes as its own
  request and waits for its acknowledgement.

  @param[in]  Addr        The regulator's RPMh address.
  @param[in]  MilliVolts  The voltage.

  @retval EFI_SUCCESS  Voted.
  @retval Other        From RpmhWrite().
**/
STATIC
EFI_STATUS
Pcie0VoteRail (
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
  Votes the PHY rails on and the PCIe-to-memory bandwidth. The rails are
  required; a failed bandwidth vote is only logged: whether UEFI needs it
  at all is not known, the rest of the path is kept up by other masters.

  @retval EFI_SUCCESS  The rails are voted.
  @retval Other        From RpmhInit() or RpmhWrite().
**/
STATIC
EFI_STATUS
Pcie0VoteRpmh (
  VOID
  )
{
  RPMH_CMD    Cmds[2];
  EFI_STATUS  Status;

  Status = RpmhInit ();
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: RPMh: %r\n", __func__, Status));
    return Status;
  }

  Status = Pcie0VoteRail (RPMH_ADDR_LDOC10, PCIE0_VDDA_PHY_MV);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: L10C (vdda-phy) vote: %r\n", __func__, Status));
    return Status;
  }

  Status = Pcie0VoteRail (RPMH_ADDR_LDOB6, PCIE0_VDDA_PLL_MV);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: L6B (vdda-pll) vote: %r\n", __func__, Status));
    return Status;
  }

  //
  // SN5 and SN14 share VCD 3: only the last command of a VCD commits, and
  // only the committing command asks for a response (bcm-voter.c
  // tcs_list_gen), as MdssDisplayDxe sends MM1 and MM0.
  //
  Cmds[0].Addr = RPMH_ADDR_BCM_SN5;
  Cmds[0].Data = RPMH_BCM_CMD (FALSE, TRUE, 0, PCIE0_BCM_PEAK_VOTE);
  Cmds[0].Wait = FALSE;
  Cmds[1].Addr = RPMH_ADDR_BCM_SN14;
  Cmds[1].Data = RPMH_BCM_CMD (TRUE, TRUE, 0, PCIE0_BCM_PEAK_VOTE);
  Cmds[1].Wait = TRUE;

  Status = RpmhWrite (Cmds, ARRAY_SIZE (Cmds));
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "%a: SN5/SN14 bandwidth vote: %r, going on without it\n", __func__, Status));
  }

  DEBUG ((
    DEBUG_INFO,
    "%a: L10C %u mV and L6B %u mV on in HPM; SN5/SN14 peak 0x%x, 0x%x: %r\n",
    __func__,
    PCIE0_VDDA_PHY_MV,
    PCIE0_VDDA_PLL_MV,
    Cmds[0].Data,
    Cmds[1].Data,
    Status
    ));
  return EFI_SUCCESS;
}

/**
  Powers the controller and its PHY clocks up and resets the controller:
  the GCC_PCIE_0_GDSC power domain; the aux and PHY refgen roots; the pipe
  mux to the PHY's pipe clock (enabling the pipe branch enables its parent
  mux in Linux, before the PHY runs, which is why the pipe branch has no
  halt check); the branches; then the controller block reset
  (qcom_pcie_init_2_7_0).

  @retval EFI_SUCCESS  The controller's registers can be accessed.
  @retval Other        A domain, root or branch did not come up.
**/
STATIC
EFI_STATUS
Pcie0ClocksOn (
  VOID
  )
{
  UINTN       Index;
  EFI_STATUS  Status;

  //
  // Both block resets are released at power-on, but phy_exit leaves the PHY
  // one asserted, and the aux clock does not come up while the controller
  // is held in reset (the vendor kernel's qcom_pcie_init_2_7_0). Release
  // whichever something before UEFI left asserted; both are pulsed below.
  //
  if ((MmioRead32 (PCIE0_GCC_BCR) & PCIE0_GCC_BCR_ASSERT) != 0) {
    DEBUG ((DEBUG_WARN, "%a: controller found held in reset, releasing it\n", __func__));
    MmioAnd32 (PCIE0_GCC_BCR, ~(UINT32)PCIE0_GCC_BCR_ASSERT);
    MicroSecondDelay (PCIE0_BCR_DEASSERT_US);
  }

  if ((MmioRead32 (GCC_PCIE_0_PHY_BCR) & PCIE0_GCC_BCR_ASSERT) != 0) {
    DEBUG ((DEBUG_WARN, "%a: PHY found held in reset, releasing it\n", __func__));
    MmioAnd32 (GCC_PCIE_0_PHY_BCR, ~(UINT32)PCIE0_GCC_BCR_ASSERT);
    MicroSecondDelay (PCIE0_PHY_BCR_ASSERT_US);
  }

  //
  // Qcs6490GccLib logs each step, with the registers when one fails.
  //
  Status = GccGdscEnable (PCIE0_GCC_GDSCR, PCIE0_GDSC_WAITS, "pcie_0_gdsc");
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = GccRcgConfigure (GCC_PCIE_0_AUX_CMD_RCGR, PCIE0_AUX_RCG_CFG, "pcie_0_aux_clk_src");
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // The refgen root runs from GPLL0_OUT_EVEN, which nothing turns on: Linux
  // has no enable for it either (clk_alpha_pll_postdiv_lucid_ops), the
  // boot loader leaves the output on.
  //
  Status = GccRcgConfigure (GCC_PCIE_0_PHY_RCHNG_CMD_RCGR, PCIE0_RCHNG_RCG_CFG, "pcie_0_phy_rchng_clk_src");
  if (EFI_ERROR (Status)) {
    return Status;
  }

  MmioAndThenOr32 (GCC_PCIE_0_PIPE_MUXR, ~(UINT32)GCC_PIPE_MUX_MASK, GCC_PIPE_MUX_PHY_SRC);

  for (Index = 0; Index < ARRAY_SIZE (mPcie0Clocks); Index++) {
    Status = GccBranchEnable (
               mPcie0Clocks[Index].EnableReg,
               mPcie0Clocks[Index].EnableMask,
               mPcie0Clocks[Index].Cbcr,
               mPcie0Clocks[Index].Halt,
               mPcie0Clocks[Index].Name
               );
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  GccResetPulse (PCIE0_GCC_BCR, PCIE0_BCR_ASSERT_US, PCIE0_BCR_DEASSERT_US);

  DEBUG ((
    DEBUG_INFO,
    "%a: GDSCR 0x%x, aux CFG 0x%x, rchng CFG 0x%x, pipe mux 0x%x, votes 0x%x 0x%x, clkref 0x%x, BCR 0x%x\n",
    __func__,
    MmioRead32 (PCIE0_GCC_GDSCR),
    MmioRead32 (GCC_PCIE_0_AUX_CMD_RCGR + PCIE0_CFG_RCGR),
    MmioRead32 (GCC_PCIE_0_PHY_RCHNG_CMD_RCGR + PCIE0_CFG_RCGR),
    MmioRead32 (GCC_PCIE_0_PIPE_MUXR),
    MmioRead32 (GCC_APCS_CLOCK_BRANCH_ENA_VOTE),
    MmioRead32 (GCC_APCS_CLOCK_BRANCH_ENA_VOTE_1),
    MmioRead32 (GCC_PCIE_CLKREF_EN),
    MmioRead32 (PCIE0_GCC_BCR)
    ));
  return EFI_SUCCESS;
}

/**
  Sets the PARF up for a root complex (qcom_pcie_init_2_7_0 after the
  reset, with qcom_pcie_configure_dbi_atu_base): root complex mode, PHY test
  power-down off, where the DBI and iATU are and the slave address space
  size, the MAC PHY power-down mux off, MHI clock reset bypassed, L1 entry
  allowed, AXI master write address halt on.
**/
STATIC
VOID
Pcie0ParfInit (
  VOID
  )
{
  //
  // The block reset leaves link training off. Make sure of it: whatever ran
  // before UEFI must not have the link train before the PHY and the root
  // port are set up.
  //
  if ((MmioRead32 (PARF_LTSSM) & PARF_LTSSM_EN) != 0) {
    DEBUG ((DEBUG_WARN, "%a: LTSSM enabled after reset (0x%x), disabling it\n", __func__, MmioRead32 (PARF_LTSSM)));
    MmioAnd32 (PARF_LTSSM, ~(UINT32)PARF_LTSSM_EN);
  }

  MmioWrite32 (PARF_DEVICE_TYPE, PARF_DEVICE_TYPE_RC);
  MmioAnd32 (PARF_PHY_CTRL, ~(UINT32)PARF_PHY_CTRL_PHY_TEST_PWR_DOWN);

  MmioWrite32 (PARF_DBI_BASE_ADDR_V2, PCIE0_DBI_BASE);
  MmioWrite32 (PARF_DBI_BASE_ADDR_V2_HI, 0);
  MmioWrite32 (PARF_ATU_BASE_ADDR, PCIE0_ATU_BASE);
  MmioWrite32 (PARF_ATU_BASE_ADDR_HI, 0);
  MmioWrite32 (PARF_SLV_ADDR_SPACE_SIZE_V2, 0);
  MmioWrite32 (PARF_SLV_ADDR_SPACE_SIZE_V2_HI, PARF_SLV_ADDR_SPACE_SZ);

  MmioAnd32 (PARF_SYS_CTRL, ~(UINT32)PARF_SYS_CTRL_MAC_PHY_PWRDN_MUX_EN);
  MmioOr32 (PARF_MHI_CLOCK_RESET_CTRL, PARF_MHI_CLOCK_RESET_CTRL_BYPASS);
  MmioAnd32 (PARF_PM_CTRL, ~(UINT32)PARF_PM_CTRL_REQ_NOT_ENTR_L1);
  MmioOr32 (PARF_AXI_MSTR_WR_ADDR_HALT_V2, PARF_AXI_MSTR_WR_ADDR_HALT_EN);

  DEBUG ((
    DEBUG_INFO,
    "%a: DEVICE_TYPE 0x%x PHY_CTRL 0x%x SYS_CTRL 0x%x MHI_CLOCK_RESET_CTRL 0x%x PM_CTRL 0x%x AXI_MSTR_WR_ADDR_HALT 0x%x LTSSM 0x%x\n",
    __func__,
    MmioRead32 (PARF_DEVICE_TYPE),
    MmioRead32 (PARF_PHY_CTRL),
    MmioRead32 (PARF_SYS_CTRL),
    MmioRead32 (PARF_MHI_CLOCK_RESET_CTRL),
    MmioRead32 (PARF_PM_CTRL),
    MmioRead32 (PARF_AXI_MSTR_WR_ADDR_HALT_V2),
    MmioRead32 (PARF_LTSSM)
    ));
}

/**
  Logs the PHY status registers.

  @param[in]  ErrorLevel  The debug level.
  @param[in]  When        A label.
**/
STATIC
VOID
Pcie0LogPhy (
  IN UINTN        ErrorLevel,
  IN CONST CHAR8  *When
  )
{
  DEBUG ((
    ErrorLevel,
    "%a: [%a] PCS_STATUS1 0x%x, C_READY 0x%x, CMN_STATUS 0x%x, RESET_SM 0x%x, SW_RESET 0x%x, POWER_DOWN 0x%x, START 0x%x\n",
    __func__,
    When,
    MmioRead32 (QPHY_V4_PCS_PCS_STATUS1),
    MmioRead32 (QSERDES_V4_COM_C_READY_STATUS),
    MmioRead32 (QSERDES_V4_COM_CMN_STATUS),
    MmioRead32 (QSERDES_V4_COM_RESET_SM_STATUS),
    MmioRead32 (QPHY_V4_PCS_SW_RESET),
    MmioRead32 (QPHY_V4_PCS_POWER_DOWN_CONTROL),
    MmioRead32 (QPHY_V4_PCS_START_CONTROL)
    ));
}

/**
  Powers the QMP PHY on (qmp_pcie_init + qmp_pcie_power_on): PHY block
  reset, power-down control on, the init tables, out of reset, SerDes and
  PCS started, then waits for PHYSTATUS to clear, which means the PHY runs
  and its pipe clock ticks. Its clocks were turned on with the controller's
  and its rails voted.

  @retval EFI_SUCCESS  The PHY runs.
  @retval EFI_TIMEOUT  PHYSTATUS did not clear.
**/
STATIC
EFI_STATUS
Pcie0PhyPowerOn (
  VOID
  )
{
  UINTN   Index;
  UINTN   Elapsed;
  UINTN   Mismatches;
  UINT32  Value;

  GccResetPulse (GCC_PCIE_0_PHY_BCR, PCIE0_PHY_BCR_ASSERT_US, PCIE0_PHY_BCR_DEASSERT_US);

  MmioOr32 (QPHY_V4_PCS_POWER_DOWN_CONTROL, QPHY_SW_PWRDN | QPHY_REFCLK_DRV_DSBL);

  for (Index = 0; Index < gQcs6490Pcie0PhyInitCount; Index++) {
    MmioWrite32 (PCIE0_PHY_BASE + gQcs6490Pcie0PhyInit[Index].Offset, gQcs6490Pcie0PhyInit[Index].Value);
  }

  MmioAnd32 (QPHY_V4_PCS_SW_RESET, ~(UINT32)QPHY_SW_RESET);
  MmioOr32 (QPHY_V4_PCS_START_CONTROL, QPHY_SERDES_START | QPHY_PCS_START);

  MicroSecondDelay (PCIE0_PHY_START_US);

  for (Elapsed = 0; ; Elapsed += PCIE0_PHY_POLL_US) {
    if ((MmioRead32 (QPHY_V4_PCS_PCS_STATUS1) & QPHY_PHYSTATUS) == 0) {
      break;
    }

    if (Elapsed >= PCIE0_PHY_TIMEOUT_US) {
      break;
    }

    MicroSecondDelay (PCIE0_PHY_POLL_US);
  }

  //
  // A running PHY reads every table value back (checked on a board running
  // Linux); a register that does not points at a PHY that is not clocked or
  // not powered.
  //
  Mismatches = 0;
  for (Index = 0; Index < gQcs6490Pcie0PhyInitCount; Index++) {
    Value = MmioRead32 (PCIE0_PHY_BASE + gQcs6490Pcie0PhyInit[Index].Offset);
    if (Value != gQcs6490Pcie0PhyInit[Index].Value) {
      if (Mismatches < 4) {
        DEBUG ((
          DEBUG_WARN,
          "%a: PHY +0x%03x reads 0x%x, written 0x%x\n",
          __func__,
          gQcs6490Pcie0PhyInit[Index].Offset,
          Value,
          gQcs6490Pcie0PhyInit[Index].Value
          ));
      }

      Mismatches++;
    }
  }

  if ((MmioRead32 (QPHY_V4_PCS_PCS_STATUS1) & QPHY_PHYSTATUS) != 0) {
    DEBUG ((
      DEBUG_ERROR,
      "%a: PHY did not start in %u us (%u of %u table writes read back different)\n",
      __func__,
      PCIE0_PHY_START_US + PCIE0_PHY_TIMEOUT_US,
      (UINT32)Mismatches,
      (UINT32)gQcs6490Pcie0PhyInitCount
      ));
    Pcie0LogPhy (DEBUG_ERROR, "timeout");
    return EFI_TIMEOUT;
  }

  DEBUG ((
    DEBUG_INFO,
    "%a: PHY up after %u us, %u table writes, %u read back different\n",
    __func__,
    (UINT32)(PCIE0_PHY_START_US + Elapsed),
    (UINT32)gQcs6490Pcie0PhyInitCount,
    (UINT32)Mismatches
    ));
  Pcie0LogPhy (DEBUG_INFO, "up");
  return EFI_SUCCESS;
}

/**
  Checks that the DBI answers as this controller: Qualcomm's vendor ID, and
  the iATU in the unrolled layout the iATU programming here and in the PCI
  Segment Library assume (dw_pcie_iatu_detect: the viewport register reads
  all ones). Logs the core version (dw_pcie_version_detect).

  @retval EFI_SUCCESS       It does.
  @retval EFI_DEVICE_ERROR  It does not.
**/
STATIC
EFI_STATUS
Pcie0CheckDbi (
  VOID
  )
{
  UINT32  Id;
  UINT32  Viewport;

  Id       = MmioRead32 (DBI_VENDOR_ID);
  Viewport = MmioRead32 (DBI_ATU_VIEWPORT);

  DEBUG ((
    DEBUG_INFO,
    "%a: root port %04x:%04x, DesignWare version 0x%08x type 0x%08x, iATU viewport 0x%x\n",
    __func__,
    Id & 0xFFFF,
    Id >> 16,
    MmioRead32 (DBI_VERSION_NUMBER),
    MmioRead32 (DBI_VERSION_TYPE),
    Viewport
    ));

  if ((Id & 0xFFFF) != 0x17CB) {
    DEBUG ((DEBUG_ERROR, "%a: the DBI reads 0x%x at 0, not a Qualcomm root port\n", __func__, Id));
    return EFI_DEVICE_ERROR;
  }

  if (Viewport != MAX_UINT32) {
    DEBUG ((DEBUG_ERROR, "%a: the iATU is not in the unrolled layout\n", __func__));
    return EFI_DEVICE_ERROR;
  }

  return EFI_SUCCESS;
}

/**
  Clears the hot-plug capable bit of the root port's slot
  (qcom_pcie_post_init_2_7_0, qcom_pcie_clear_hpc): the slot is not
  hot-pluggable, and PciBusDxe would otherwise treat it as such.
**/
STATIC
VOID
Pcie0ClearHotPlug (
  VOID
  )
{
  MmioOr32 (DBI_MISC_CONTROL_1, DBI_RO_WR_EN);
  MmioAnd32 (DBI_SLTCAP, ~(UINT32)DBI_SLTCAP_HPC);
  MmioAnd32 (DBI_MISC_CONTROL_1, ~(UINT32)DBI_RO_WR_EN);

  DEBUG ((DEBUG_INFO, "%a: SLTCAP 0x%x\n", __func__, MmioRead32 (DBI_SLTCAP)));
}

/**
  Releases PERST# (qcom_pcie_perst_deassert): no less than 100 ms after it
  was asserted and the supplies became valid (PCIE_T_PVPERL_MS), then waits
  PERST_DELAY_US.

  @param[in]  Board       How the board wires PCIe0.
  @param[in]  PerstTicks  Performance counter when PERST# was asserted with
                          the supplies on.
**/
STATIC
VOID
Pcie0ReleasePerst (
  IN CONST PCIE0_BOARD  *Board,
  IN UINT64             PerstTicks
  )
{
  UINT64  Elapsed;

  Elapsed = Pcie0ElapsedUs (PerstTicks);
  if (Elapsed < PCIE0_PERST_MIN_US) {
    MicroSecondDelay ((UINTN)(PCIE0_PERST_MIN_US - Elapsed));
  }

  TlmmSetOutput (Board->PerstGpio, TRUE);
  MicroSecondDelay (PCIE0_PERST_DELAY_US);

  DEBUG ((
    DEBUG_INFO,
    "%a: PERST# released after %lu ms: IO 0x%x\n",
    __func__,
    DivU64x32 (Pcie0ElapsedUs (PerstTicks), 1000),
    MmioRead32 (TLMM_GPIO_IO (Board->PerstGpio))
    ));
}

/**
  Computes the CRC-8 the BDF to SID table is hashed with (crc8_populate_msb
  with QCOM_PCIE_CRC8_POLYNOMIAL 0x07, MSB first, initial value 0).

  @param[in]  Data    The bytes.
  @param[in]  Length  How many.

  @return  The CRC.
**/
STATIC
UINT8
Pcie0Crc8 (
  IN CONST UINT8  *Data,
  IN UINTN        Length
  )
{
  UINT8  Crc;
  UINTN  Bit;

  Crc = 0;
  while (Length-- > 0) {
    Crc ^= *Data++;
    for (Bit = 0; Bit < 8; Bit++) {
      Crc = ((Crc & BIT7) != 0) ? (UINT8)((Crc << 1) ^ 0x07) : (UINT8)(Crc << 1);
    }
  }

  return Crc;
}

/**
  Programs the requester ID to SMMU stream table from pcie0's iommu-map
  (qcom_pcie_config_sid_1_9_0): translation on, the table cleared, then
  each requester ID placed at the CRC-8 of its big-endian value, chained to
  the next free entry on a collision. 01:00.0 then uses stream 0x1C01 and
  the root port 0x1C00, the streams SmmuDxe lets through.
**/
STATIC
VOID
Pcie0ConfigSid (
  VOID
  )
{
  UINTN   Index;
  UINT8   Bytes[2];
  UINT8   Hash;
  UINT8   Current;
  UINT32  Value;

  MmioAnd32 (PARF_BDF_TO_SID_CFG, ~(UINT32)PARF_BDF_TO_SID_BYPASS);

  for (Index = 0; Index < PARF_BDF_TO_SID_ENTRIES; Index++) {
    MmioWrite32 (PARF_BDF_TO_SID_TABLE_N + Index * sizeof (UINT32), 0);
  }

  for (Index = 0; Index < ARRAY_SIZE (mPcie0SidMap); Index++) {
    Bytes[0] = (UINT8)(mPcie0SidMap[Index].Bdf >> 8);
    Bytes[1] = (UINT8)mPcie0SidMap[Index].Bdf;
    Hash     = Pcie0Crc8 (Bytes, sizeof (Bytes));

    Value = MmioRead32 (PARF_BDF_TO_SID_TABLE_N + Hash * sizeof (UINT32));
    while (Value != 0) {
      Current = Hash++;
      if ((Value & 0xFF) == 0) {
        Value |= Hash;
        MmioWrite32 (PARF_BDF_TO_SID_TABLE_N + Current * sizeof (UINT32), Value);
      }

      Value = MmioRead32 (PARF_BDF_TO_SID_TABLE_N + Hash * sizeof (UINT32));
    }

    //
    // BDF in bits 31:16, stream offset in bits 15:8, next entry in 7:0.
    //
    Value = ((UINT32)mPcie0SidMap[Index].Bdf << 16) |
            ((UINT32)(mPcie0SidMap[Index].Sid - mPcie0SidMap[0].Sid) << 8);
    MmioWrite32 (PARF_BDF_TO_SID_TABLE_N + Hash * sizeof (UINT32), Value);

    DEBUG ((
      DEBUG_INFO,
      "%a: requester 0x%04x -> stream 0x%x: entry 0x%02x reads 0x%08x\n",
      __func__,
      mPcie0SidMap[Index].Bdf,
      mPcie0SidMap[Index].Sid,
      Hash,
      MmioRead32 (PARF_BDF_TO_SID_TABLE_N + Hash * sizeof (UINT32))
      ));
  }

  DEBUG ((DEBUG_INFO, "%a: BDF_TO_SID_CFG 0x%x\n", __func__, MmioRead32 (PARF_BDF_TO_SID_CFG)));
}

/**
  Programs an outbound iATU region and waits for its enable to read back
  (dw_pcie_prog_outbound_atu: the bases, limits and targets, then the type,
  then the enable).

  @param[in]  Region  The region.
  @param[in]  Type    DW_ATU_TYPE_*.
  @param[in]  Base    The first CPU address.
  @param[in]  Limit   The last CPU address.
  @param[in]  Target  The PCI address of Base.

  @retval EFI_SUCCESS  Enabled.
  @retval EFI_TIMEOUT  The enable did not read back.
**/
STATIC
EFI_STATUS
Pcie0ProgramOutboundAtu (
  IN UINTN   Region,
  IN UINT32  Type,
  IN UINT64  Base,
  IN UINT64  Limit,
  IN UINT64  Target
  )
{
  UINTN  Atu;
  UINTN  Retry;

  Atu = DW_ATU_OB (Region);

  MmioWrite32 (Atu + DW_ATU_LWR_BASE, (UINT32)Base);
  MmioWrite32 (Atu + DW_ATU_UPR_BASE, (UINT32)RShiftU64 (Base, 32));
  MmioWrite32 (Atu + DW_ATU_LIMIT, (UINT32)Limit);
  MmioWrite32 (Atu + DW_ATU_UPR_LIMIT, (UINT32)RShiftU64 (Limit, 32));
  MmioWrite32 (Atu + DW_ATU_LWR_TARGET, (UINT32)Target);
  MmioWrite32 (Atu + DW_ATU_UPR_TARGET, (UINT32)RShiftU64 (Target, 32));
  MmioWrite32 (Atu + DW_ATU_CTRL1, Type);
  MmioWrite32 (Atu + DW_ATU_CTRL2, DW_ATU_ENABLE);

  for (Retry = 0; Retry < DW_ATU_ENABLE_RETRIES; Retry++) {
    if ((MmioRead32 (Atu + DW_ATU_CTRL2) & DW_ATU_ENABLE) != 0) {
      DEBUG ((
        DEBUG_INFO,
        "%a: outbound %u type %u: 0x%lx-0x%lx -> 0x%lx\n",
        __func__,
        (UINT32)Region,
        Type,
        Base,
        Limit,
        Target
        ));
      return EFI_SUCCESS;
    }

    MicroSecondDelay (DW_ATU_ENABLE_RETRY_US);
  }

  DEBUG ((
    DEBUG_ERROR,
    "%a: outbound region %u did not enable: CTRL1 0x%x CTRL2 0x%x\n",
    __func__,
    (UINT32)Region,
    MmioRead32 (Atu + DW_ATU_CTRL1),
    MmioRead32 (Atu + DW_ATU_CTRL2)
    ));
  return EFI_TIMEOUT;
}

/**
  Sets the root port up (dw_pcie_setup_rc with dw_pcie_setup): link control
  and one lane, its BARs, interrupt pin, bus numbers and command register,
  the iATU (every region off, then the MEM window in region 1; no IO
  window, and region 0 is the PCI Segment Library's), the PCI-to-PCI bridge
  class, how retried configuration requests read, and a directed speed
  change. Read-only DBI registers are made writable meanwhile.

  The maximum link speed is the hardware's (no max-link-speed in the device
  tree), so dw_pcie_link_set_max_speed writes nothing.

  @param[out]  MaxSpeed  The maximum link speed (LNKCAP encoding).

  @retval EFI_SUCCESS  Set up.
  @retval Other        The MEM window did not enable.
**/
STATIC
EFI_STATUS
Pcie0SetupRootPort (
  OUT UINT32  *MaxSpeed
  )
{
  UINT32      Plc;
  UINT32      Lwsc;
  UINT32      LnkCap;
  UINTN       Index;
  EFI_STATUS  Status;

  MmioOr32 (DBI_MISC_CONTROL_1, DBI_RO_WR_EN);

  *MaxSpeed = PCIE0_LNKCAP_SLS (MmioRead32 (DBI_LNKCAP));

  MmioAndThenOr32 (DBI_PORT_LINK_CONTROL, ~(UINT32)PORT_LINK_FAST_LINK_MODE, PORT_LINK_DLL_LINK_EN);

  //
  // dw_pcie_link_set_max_link_width (num-lanes = 1).
  //
  Plc  = MmioRead32 (DBI_PORT_LINK_CONTROL);
  Plc &= ~(UINT32)(PORT_LINK_FAST_LINK_MODE | PORT_LINK_MODE_MASK);
  Plc |= PORT_LINK_MODE_1_LANES;

  Lwsc  = MmioRead32 (DBI_LINK_WIDTH_SPEED_CONTROL);
  Lwsc &= ~(UINT32)PORT_LOGIC_LINK_WIDTH_MASK;
  Lwsc |= PORT_LOGIC_LINK_WIDTH_1_LANES;

  MmioWrite32 (DBI_PORT_LINK_CONTROL, Plc);
  MmioWrite32 (DBI_LINK_WIDTH_SPEED_CONTROL, Lwsc);

  LnkCap  = MmioRead32 (DBI_LNKCAP);
  LnkCap &= ~(UINT32)PCIE0_LNKCAP_MLW_MASK;
  LnkCap |= PCIE0_LNKCAP_MLW (1);
  MmioWrite32 (DBI_LNKCAP, LnkCap);

  MmioWrite32 (DBI_BAR0, 0x00000004);
  MmioWrite32 (DBI_BAR1, 0x00000000);

  //
  // Interrupt pin INTA.
  //
  MmioAndThenOr32 (DBI_INTERRUPT_LINE, 0xFFFF00FF, 0x00000100);

  //
  // Primary bus 0, secondary 1, subordinate 1 (Linux: 0xFF): the bus range
  // PciHostBridgeDxe gets. PciBusDxe programs them again anyway.
  //
  MmioAndThenOr32 (
    DBI_PRIMARY_BUS,
    0xFF000000,
    ((UINT32)PCIE0_SECONDARY_BUS << 16) | ((UINT32)PCIE0_SECONDARY_BUS << 8) | PCIE0_ROOT_BUS
    );

  //
  // Memory decode, bus master and SERR# (Linux also sets IO decode; there
  // is no IO window here).
  //
  MmioAndThenOr32 (
    DBI_COMMAND,
    0xFFFF0000,
    EFI_PCI_COMMAND_MEMORY_SPACE | EFI_PCI_COMMAND_BUS_MASTER | EFI_PCI_COMMAND_SERR
    );

  //
  // dw_pcie_iatu_setup: every region off first. There are no inbound
  // windows (no dma-ranges): inbound requests go to the AXI master with the
  // PCI address as the address.
  //
  for (Index = 0; Index < DW_ATU_REGIONS; Index++) {
    MmioWrite32 (DW_ATU_OB (Index) + DW_ATU_CTRL2, 0);
  }

  for (Index = 0; Index < DW_ATU_REGIONS; Index++) {
    MmioWrite32 (DW_ATU_IB (Index) + DW_ATU_CTRL2, 0);
  }

  Status = Pcie0ProgramOutboundAtu (
             DW_ATU_REGION_MEM,
             DW_ATU_TYPE_MEM,
             PCIE0_MEM_BASE,
             PCIE0_MEM_LIMIT,
             PCIE0_MEM_BASE
             );

  MmioWrite32 (DBI_BAR0, 0);
  MmioWrite16 (DBI_CLASS_DEVICE, (UINT16)((PCI_CLASS_BRIDGE << 8) | PCI_CLASS_BRIDGE_P2P));

  //
  // A configuration request the device answers with Request Retry Status
  // (it may for up to 1 s after reset) completes on AXI as a read of
  // 0xFFFF0001 for the vendor ID and of all ones otherwise, which tells
  // retry and absent apart from a device (pcie-tegra194.c sets the same).
  // The reset value makes it a read of undefined data.
  //
  MmioAndThenOr32 (
    DBI_AMBA_ERROR_RESPONSE_DEFAULT,
    ~(UINT32)AMBA_ERROR_RESPONSE_RRS_MASK,
    AMBA_ERROR_RESPONSE_RRS_FFFF0001
    );
  MmioOr32 (DBI_LINK_WIDTH_SPEED_CONTROL, PORT_LOGIC_SPEED_CHANGE);

  MmioAnd32 (DBI_MISC_CONTROL_1, ~(UINT32)DBI_RO_WR_EN);

  DEBUG ((
    DEBUG_INFO,
    "%a: COMMAND 0x%x CLASS 0x%x BUS 0x%x LNKCAP 0x%x PORT_LINK_CONTROL 0x%x LINK_WIDTH_SPEED 0x%x\n",
    __func__,
    MmioRead32 (DBI_COMMAND),
    MmioRead32 (DBI_CLASS_DEVICE - 2),
    MmioRead32 (DBI_PRIMARY_BUS),
    MmioRead32 (DBI_LNKCAP),
    MmioRead32 (DBI_PORT_LINK_CONTROL),
    MmioRead32 (DBI_LINK_WIDTH_SPEED_CONTROL)
    ));

  return Status;
}

/**
  Starts link training (qcom_pcie_start_link): the equalization settings
  for each rate from 8 GT/s up to the maximum (qcom_pcie_common_set_equalization;
  the core is an 8 GT/s one, so the 16 GT/s lane margining does not apply),
  then LTSSM enabled through PARF_LTSSM (qcom_pcie_2_3_2_ltssm_enable).

  @param[in]  MaxSpeed  The maximum link speed (LNKCAP encoding).
**/
STATIC
VOID
Pcie0StartLink (
  IN UINT32  MaxSpeed
  )
{
  UINT32  Speed;

  for (Speed = LNK_SPEED_8_0GT; Speed <= MIN (MaxSpeed, LNK_SPEED_32_0GT); Speed++) {
    MmioAndThenOr32 (
      DBI_GEN3_RELATED_OFF,
      ~(UINT32)(GEN3_RELATED_OFF_GEN3_ZRXDC_NONCOMPL | GEN3_RELATED_OFF_RATE_SHADOW_SEL_MASK),
      GEN3_RELATED_OFF_RATE_SHADOW_SEL (Speed - LNK_SPEED_8_0GT)
      );

    MmioAndThenOr32 (
      DBI_GEN3_EQ_FB_MODE_DIR_CHANGE_OFF,
      ~(UINT32)GEN3_EQ_FMDC_MASK,
      GEN3_EQ_FMDC_T_MIN_PHASE23 (0x1) | GEN3_EQ_FMDC_N_EVALS (0xD) |
      GEN3_EQ_FMDC_MAX_PRE_CURSOR_DELTA (0x5) | GEN3_EQ_FMDC_MAX_POST_CURSOR_DELTA (0x5)
      );

    MmioAnd32 (
      DBI_GEN3_EQ_CONTROL_OFF,
      ~(UINT32)(GEN3_EQ_CONTROL_OFF_FB_MODE | GEN3_EQ_CONTROL_OFF_PHASE23_EXIT_MODE |
                GEN3_EQ_CONTROL_OFF_FOM_INC_INITIAL_EVAL | GEN3_EQ_CONTROL_OFF_PSET_REQ_VEC)
      );
  }

  MmioOr32 (PARF_LTSSM, PARF_LTSSM_EN);

  DEBUG ((
    DEBUG_INFO,
    "%a: max speed %u, GEN3_RELATED 0x%x EQ_CONTROL 0x%x EQ_FB_MODE_DIR_CHANGE 0x%x, PARF_LTSSM 0x%x\n",
    __func__,
    MaxSpeed,
    MmioRead32 (DBI_GEN3_RELATED_OFF),
    MmioRead32 (DBI_GEN3_EQ_CONTROL_OFF),
    MmioRead32 (DBI_GEN3_EQ_FB_MODE_DIR_CHANGE_OFF),
    MmioRead32 (PARF_LTSSM)
    ));
}

/**
  Waits for the link (dw_pcie_wait_for_link): data link layer link active,
  checked every 90 ms, ten times. Once it is up, waits 100 ms before the
  first configuration request when the port can go faster than 5 GT/s
  (PCIe r6.0 sec 6.6.1), and logs the negotiated speed and width.

  @param[in]  MaxSpeed  The maximum link speed (LNKCAP encoding).

  @retval EFI_SUCCESS    The link is up.
  @retval EFI_NOT_FOUND  No device answered on the link.
  @retval EFI_TIMEOUT    A device is there but the link did not come up.
**/
STATIC
EFI_STATUS
Pcie0WaitForLink (
  IN UINT32  MaxSpeed
  )
{
  UINTN   Retry;
  UINT32  Debug0;
  UINT32  Ltssm;
  UINT16  LnkSta;

  for (Retry = 0; Retry < PCIE0_LINK_RETRIES; Retry++) {
    if (Qcs6490Pcie0LinkUp ()) {
      break;
    }

    MicroSecondDelay (PCIE0_LINK_WAIT_US);
  }

  if (Retry >= PCIE0_LINK_RETRIES) {
    Debug0 = MmioRead32 (DBI_PORT_DEBUG0);
    Ltssm  = PORT_LOGIC_LTSSM_STATE (Debug0);

    DEBUG ((
      DEBUG_ERROR,
      "%a: no link after %u ms, %a: LTSSM 0x%x, PARF_LTSSM 0x%x, PORT_DEBUG0 0x%x, PORT_DEBUG1 0x%x, LNKSTA 0x%x\n",
      __func__,
      (PCIE0_LINK_RETRIES * PCIE0_LINK_WAIT_US) / 1000,
      (Ltssm <= DW_LTSSM_DETECT_ACT) ? "no device detected" :
      (Ltssm <= DW_LTSSM_POLL_COMPLIANCE) ? "device found but not active" : "training failed",
      Ltssm,
      MmioRead32 (PARF_LTSSM),
      Debug0,
      MmioRead32 (DBI_PORT_DEBUG1),
      MmioRead16 (DBI_LNKSTA)
      ));
    Pcie0LogPhy (DEBUG_ERROR, "no link");
    return (Ltssm <= DW_LTSSM_DETECT_ACT) ? EFI_NOT_FOUND : EFI_TIMEOUT;
  }

  if (MaxSpeed > LNK_SPEED_5_0GT) {
    MicroSecondDelay (PCIE0_CONFIG_WAIT_US);
  }

  LnkSta = MmioRead16 (DBI_LNKSTA);
  DEBUG ((
    DEBUG_INFO,
    "%a: Gen%u x%u link up after %u ms: LNKSTA 0x%x, PARF_LTSSM 0x%x, PORT_DEBUG0 0x%x\n",
    __func__,
    PCIE0_LNKSTA_CLS (LnkSta),
    PCIE0_LNKSTA_NLW (LnkSta),
    (UINT32)((Retry * PCIE0_LINK_WAIT_US) / 1000),
    LnkSta,
    MmioRead32 (PARF_LTSSM),
    MmioRead32 (DBI_PORT_DEBUG0)
    ));
  return EFI_SUCCESS;
}

/**
  Waits for 01:00.0 to answer configuration requests, through the PCI
  Segment Library the way PciBusDxe will read it, and logs its IDs. A device
  may answer with Configuration Request Retry Status for up to 1 s after
  reset (PCIe r6.0 sec 6.6.1), which reads as 0xFFFF0001 here (the AMBA
  error response set up with the root port), and an absent one as all ones.
  A device that never answers is only logged: the root bridge stays, and
  PciBusDxe finds nothing below it.
**/
STATIC
VOID
Pcie0WaitForEndpoint (
  VOID
  )
{
  UINT32  Id;
  UINT32  Class;
  UINTN   Waited;

  for (Waited = 0; ; Waited += PCIE0_ENDPOINT_POLL_US) {
    Id = PciSegmentRead32 (PCI_SEGMENT_LIB_ADDRESS (PCIE0_SEGMENT, PCIE0_SECONDARY_BUS, 0, 0, PCI_VENDOR_ID_OFFSET));
    if ((Id != MAX_UINT32) && ((Id & 0xFFFF) != 0x0001) && ((Id & 0xFFFF) != 0x0000)) {
      break;
    }

    if (Waited >= PCIE0_ENDPOINT_TIMEOUT_US) {
      DEBUG ((DEBUG_WARN, "%a: 01:00.0 does not answer (0x%x)\n", __func__, Id));
      return;
    }

    MicroSecondDelay (PCIE0_ENDPOINT_POLL_US);
  }

  Class = PciSegmentRead32 (PCI_SEGMENT_LIB_ADDRESS (PCIE0_SEGMENT, PCIE0_SECONDARY_BUS, 0, 0, PCI_REVISION_ID_OFFSET));

  DEBUG ((
    DEBUG_INFO,
    "%a: 01:00.0 is %04x:%04x, class 0x%06x revision 0x%02x, answered after %u ms\n",
    __func__,
    Id & 0xFFFF,
    Id >> 16,
    Class >> 8,
    Class & 0xFF,
    (UINT32)(Waited / 1000)
    ));
}

/**
  Leaves PCIe0 after a failed bring-up: link training off, if the controller
  got that far, and PERST# asserted.

  @param[in]  Board           How the board wires PCIe0.
  @param[in]  ParfAccessible  Whether the controller's clocks are on.
**/
STATIC
VOID
Pcie0Abort (
  IN CONST PCIE0_BOARD  *Board,
  IN BOOLEAN            ParfAccessible
  )
{
  if (ParfAccessible) {
    MmioAnd32 (PARF_LTSSM, ~(UINT32)PARF_LTSSM_EN);
  }

  TlmmSetOutput (Board->PerstGpio, FALSE);
}

/**
  Brings PCIe0 up to a trained link, the way Linux 7.0.2 does
  (pcie-qcom.c, pcie-designware-host.c, phy-qcom-qmp-pcie.c). On failure
  the link training is left disabled and PERST# asserted.

  @param[in]  Board  How the board wires PCIe0.

  @retval EFI_SUCCESS  The link is up and the root port set up.
  @retval Other        It is not; the cause has been logged.
**/
EFI_STATUS
Qcs6490Pcie0Init (
  IN CONST PCIE0_BOARD  *Board
  )
{
  UINT64      StartTicks;
  UINT64      PerstTicks;
  BOOLEAN     Switched;
  BOOLEAN     ParfAccessible;
  UINT32      MaxSpeed;
  EFI_STATUS  Status;

  StartTicks     = GetPerformanceCounter ();
  ParfAccessible = FALSE;

  DEBUG ((
    DEBUG_INFO,
    "%a: PERST# GPIO%u, %u power GPIO(s), %u us ramp each\n",
    __func__,
    Board->PerstGpio,
    (UINT32)Board->PowerGpioCount,
    Board->PowerRampUs
    ));

  Status = Pcie0AssertPerst (Board);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  PerstTicks = GetPerformanceCounter ();

  Status = Pcie0PowerOn (Board, &Switched);
  if (EFI_ERROR (Status)) {
    goto Fail;
  }

  //
  // PERST# is held for 100 ms after the supplies are valid, too.
  //
  if (Switched) {
    PerstTicks = GetPerformanceCounter ();
  }

  Status = Pcie0ClkreqPin ();
  if (EFI_ERROR (Status)) {
    goto Fail;
  }

  Status = Pcie0VoteRpmh ();
  if (EFI_ERROR (Status)) {
    goto Fail;
  }

  Status = Pcie0ClocksOn ();
  if (EFI_ERROR (Status)) {
    goto Fail;
  }

  ParfAccessible = TRUE;
  Pcie0ParfInit ();

  Status = Pcie0PhyPowerOn ();
  if (EFI_ERROR (Status)) {
    goto Fail;
  }

  Status = Pcie0CheckDbi ();
  if (EFI_ERROR (Status)) {
    goto Fail;
  }

  Pcie0ClearHotPlug ();
  Pcie0ReleasePerst (Board, PerstTicks);
  Pcie0ConfigSid ();

  Status = Pcie0SetupRootPort (&MaxSpeed);
  if (EFI_ERROR (Status)) {
    goto Fail;
  }

  Pcie0StartLink (MaxSpeed);

  Status = Pcie0WaitForLink (MaxSpeed);
  if (EFI_ERROR (Status)) {
    goto Fail;
  }

  Pcie0WaitForEndpoint ();

  DEBUG ((DEBUG_INFO, "%a: PCIe0 up in %lu ms\n", __func__, DivU64x32 (Pcie0ElapsedUs (StartTicks), 1000)));
  return EFI_SUCCESS;

Fail:
  Pcie0Abort (Board, ParfAccessible);
  DEBUG ((DEBUG_ERROR, "%a: PCIe0 bring-up failed: %r; PERST# left asserted\n", __func__, Status));
  return Status;
}

/**
  Quiesces PCIe0 for the OS at ExitBootServices: link training off and
  PERST# asserted, as after a failed bring-up. The supplies stay on, so the
  devices on the link keep what they hold (the uPD720201 its firmware).

  @param[in]  Board  How the board wires PCIe0.
**/
VOID
Qcs6490Pcie0Quiesce (
  IN CONST PCIE0_BOARD  *Board
  )
{
  Pcie0Abort (Board, Qcs6490Pcie0Accessible ());
}
