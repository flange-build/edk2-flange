/** @file
  Global clock controller (GCC) helpers for QCS6490 drivers: power domains
  (GDSC), branch clocks, root clock generators and block resets, used the way
  Linux's drivers/clk/qcom (gdsc.c, clk-branch.c, clk-rcg2.c, reset.c) uses
  them for sc7280.

  Copyright (c) 2026, edk2-flange contributors.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#ifndef QCS6490_GCC_LIB_H_
#define QCS6490_GCC_LIB_H_

#include <Uefi/UefiBaseType.h>

#define GCC_BASE  0x00100000

//
// Branch votes: one bit per clock in these registers turns a voted branch
// on for the apps processor (gcc-sc7280.c enable_reg).
//
#define GCC_APCS_CLOCK_BRANCH_ENA_VOTE    (GCC_BASE + 0x52000)
#define GCC_APCS_CLOCK_BRANCH_ENA_VOTE_1  (GCC_BASE + 0x52008)

//
// Wait times of a GDSC (gdsc.c EN_REST_WAIT, EN_FEW_WAIT, CLK_DIS_WAIT), as
// the GCC driver sets them before turning it on. Linux's defaults are
// GCC_GDSC_WAITS (2, 8, 2).
//
#define GCC_GDSC_WAITS(EnRest, EnFew, ClkDis)  \
  ((((UINT32)(EnRest) & 0xF) << 20) | (((UINT32)(EnFew) & 0xF) << 16) | (((UINT32)(ClkDis) & 0xF) << 12))

//
// How Linux waits for a branch clock to report on (clk-branch.h halt_check).
//
typedef enum {
  GccHalt,        // BRANCH_HALT: the branch's own CBCR shows it on
  GccHaltVoted,   // BRANCH_HALT_VOTED: as GccHalt, after a vote
  GccHaltSkip     // BRANCH_HALT_SKIP: no status to wait for
} GCC_HALT_CHECK;

/**
  Polls a register until the masked bits read a value.

  @param[in]  Address    The register.
  @param[in]  Mask       The bits.
  @param[in]  Value      The value they must read.
  @param[in]  TimeoutUs  How long to wait, in microseconds.

  @retval EFI_SUCCESS  They do.
  @retval EFI_TIMEOUT  They did not in time.
**/
EFI_STATUS
EFIAPI
GccPoll32 (
  IN UINTN   Address,
  IN UINT32  Mask,
  IN UINT32  Value,
  IN UINTN   TimeoutUs
  );

/**
  Turns a GDSC power domain on under software control: sets its wait times
  with hardware control and the software override off (gdsc_init), then
  clears SW_COLLAPSE, waits up to 2 ms for PWR_ON, waits 1 us and sets
  RETAIN_FF_ENABLE (gdsc_enable). A GDSC that is already on is left on with
  RETAIN_FF_ENABLE set.

  @param[in]  Gdscr  The GDSC control register.
  @param[in]  Waits  GCC_GDSC_WAITS() of the domain.
  @param[in]  Name   A name for the log.

  @retval EFI_SUCCESS       On.
  @retval EFI_DEVICE_ERROR  It did not report power.
**/
EFI_STATUS
EFIAPI
GccGdscEnable (
  IN UINTN        Gdscr,
  IN UINT32       Waits,
  IN CONST CHAR8  *Name
  );

/**
  Turns a branch clock on and waits for it the way clk_branch2_enable does:
  sets EnableMask in EnableReg (the branch's CBCR, or a vote register), then
  unless Halt is GccHaltSkip, or the CBCR has hardware gating (HW_CTL, bit 1)
  on, waits up to 200 us for the CBCR to show the clock on: CLK_OFF (bit 31)
  clear, or NOC_FSM_STATUS (bits 30:28) ON.

  @param[in]  EnableReg   The register with the enable bit.
  @param[in]  EnableMask  The enable bit.
  @param[in]  Cbcr        The branch's CBCR.
  @param[in]  Halt        How to wait.
  @param[in]  Name        A name for the log.

  @retval EFI_SUCCESS  On.
  @retval EFI_TIMEOUT  The branch still reads off.
**/
EFI_STATUS
EFIAPI
GccBranchEnable (
  IN UINTN           EnableReg,
  IN UINT32          EnableMask,
  IN UINTN           Cbcr,
  IN GCC_HALT_CHECK  Halt,
  IN CONST CHAR8     *Name
  );

/**
  Configures a root clock generator (clk_rcg2 layout: CMD_RCGR, CFG_RCGR at
  +4) without M/N: replaces SRC_DIV (bits 4:0), SRC_SEL (bits 10:8), MODE
  (bits 13:12) and HW_CLK_CTRL (bit 20) of CFG_RCGR with Cfg, then sets
  UPDATE in CMD_RCGR and waits up to 500 us for it to clear
  (__clk_rcg2_configure, update_config). Nothing is written when the root
  already has that configuration. The current and the new source must run.

  @param[in]  CmdRcgr  The root's CMD_RCGR.
  @param[in]  Cfg      SRC_SEL << 8 | (2 * divider - 1).
  @param[in]  Name     A name for the log.

  @retval EFI_SUCCESS  Configured.
  @retval EFI_TIMEOUT  The root did not take the configuration.
**/
EFI_STATUS
EFIAPI
GccRcgConfigure (
  IN UINTN        CmdRcgr,
  IN UINT32       Cfg,
  IN CONST CHAR8  *Name
  );

/**
  Resets a block through its block control register (BCR): sets bit 0,
  waits AssertUs, clears it and waits DeassertUs, reading the register back
  after each write (qcom_reset_assert/deassert plus the caller's delays).

  @param[in]  Bcr         The BCR.
  @param[in]  AssertUs    How long to hold the reset, in microseconds.
  @param[in]  DeassertUs  How long to wait after releasing it.
**/
VOID
EFIAPI
GccResetPulse (
  IN UINTN  Bcr,
  IN UINTN  AssertUs,
  IN UINTN  DeassertUs
  );

#endif // QCS6490_GCC_LIB_H_
