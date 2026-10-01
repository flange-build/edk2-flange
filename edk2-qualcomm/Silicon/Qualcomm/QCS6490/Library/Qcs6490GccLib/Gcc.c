/** @file
  Global clock controller (GCC) helpers for QCS6490 drivers: power domains
  (GDSC), branch clocks, root clock generators and block resets.

  Each helper does what the Linux qcom clock driver (drivers/clk/qcom,
  Linux 7.0.2) does for sc7280, in the same order and with the same
  timeouts:
  - GDSC: gdsc.c gdsc_init and gdsc_enable.
  - Branch: clk-branch.c clk_branch2_enable (clk_enable_regmap and
    clk_branch_wait).
  - Root: clk-rcg2.c __clk_rcg2_configure without M/N, then update_config.
  - Reset: reset.c qcom_reset_assert/deassert.
  The register writes change only the bits Linux changes (regmap_update_bits
  semantics: no write when nothing changes), and every helper logs what it
  found and what it left, so that a boot log shows the clock state without a
  debugger.

  Copyright (c) 2026, edk2-flange contributors.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/Qcs6490GccLib.h>
#include <Library/TimerLib.h>

//
// Branch clock control register (clk-branch.h): hardware clock gating in
// bit 1 (the hwcg_bit of the GCC branches), NOC_FSM_STATUS in bits 30:28
// (2 = on) and CLK_OFF in bit 31. The enable bit is the caller's.
//
#define GCC_CBCR_HW_CTL                BIT1
#define GCC_CBCR_NOC_FSM_STATUS_SHIFT  28
#define GCC_CBCR_NOC_FSM_STATUS_MASK   (0x7U << GCC_CBCR_NOC_FSM_STATUS_SHIFT)
#define GCC_CBCR_NOC_FSM_STATUS_ON     0x2
#define GCC_CBCR_CLK_OFF               BIT31

//
// Root clock generator, clk_rcg2 layout (clk-rcg2.c): CMD_RCGR, CFG_RCGR at
// +4. The dirty bits say that CFG/M/N/D were written but not yet taken by
// an UPDATE.
//
#define GCC_RCG_CMD_UPDATE        BIT0
#define GCC_RCG_CMD_DIRTY_MASK    (BIT4 | BIT5 | BIT6 | BIT7)
#define GCC_RCG_CFG               0x4
#define GCC_RCG_CFG_SRC_DIV_MASK  0x1FU
#define GCC_RCG_CFG_SRC_SEL_MASK  (0x7U << 8)
#define GCC_RCG_CFG_MODE_MASK     (0x3U << 12)
#define GCC_RCG_CFG_HW_CLK_CTRL   BIT20

//
// The CFG_RCGR bits __clk_rcg2_configure replaces: the divider (hid_width 5
// on every sc7280 GCC root), the source, the M/N mode and hardware clock
// control.
//
#define GCC_RCG_CFG_MASK  (GCC_RCG_CFG_SRC_DIV_MASK | GCC_RCG_CFG_SRC_SEL_MASK |  \
                           GCC_RCG_CFG_MODE_MASK | GCC_RCG_CFG_HW_CLK_CTRL)

//
// GDSC control register (gdsc.c), and its CFG_GDSCR at +4.
//
#define GCC_GDSC_SW_COLLAPSE       BIT0
#define GCC_GDSC_HW_CONTROL        BIT1
#define GCC_GDSC_SW_OVERRIDE       BIT2
#define GCC_GDSC_RETAIN_FF_ENABLE  BIT11
#define GCC_GDSC_WAITS_MASK        GCC_GDSC_WAITS (0xF, 0xF, 0xF)
#define GCC_GDSC_PWR_ON            BIT31
#define GCC_GDSC_CFG_GDSCR         0x4

//
// The GDSCR bits gdsc_init sets: no hardware trigger, no software override
// of the power sequencer, and the wait times.
//
#define GCC_GDSC_INIT_MASK  (GCC_GDSC_HW_CONTROL | GCC_GDSC_SW_OVERRIDE | GCC_GDSC_WAITS_MASK)

//
// Block control register (reset.c): the reset bit of every GCC BCR.
//
#define GCC_BCR_BLK_ARES  BIT0

//
// Timeouts of clk-branch.c (get_branch_timeout: at least 200 us),
// clk-rcg2.c (get_update_timeout: at least 500 us) and gdsc.c
// (STATUS_POLL_TIMEOUT_US).
//
#define GCC_BRANCH_TIMEOUT_US  200
#define GCC_RCG_TIMEOUT_US     500
#define GCC_GDSC_TIMEOUT_US    2000

/**
  Changes bits of a register, writing only if that changes it, as Linux
  regmap_update_bits does.

  @param[in]  Address  The register.
  @param[in]  Mask     The bits.
  @param[in]  Value    Their new value.
**/
STATIC
VOID
GccUpdateBits (
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
  Returns whether a branch's CBCR shows the clock running, the way
  clk_branch2_check_halt does when a clock is enabled: CLK_OFF clear, or the
  NoC handshake state machine on.

  @param[in]  Cbcr  The CBCR value.

  @return  TRUE if it does.
**/
STATIC
BOOLEAN
GccCbcrIsOn (
  IN UINT32  Cbcr
  )
{
  return ((Cbcr & GCC_CBCR_CLK_OFF) == 0) ||
         (((Cbcr & GCC_CBCR_NOC_FSM_STATUS_MASK) >> GCC_CBCR_NOC_FSM_STATUS_SHIFT) == GCC_CBCR_NOC_FSM_STATUS_ON);
}

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
  )
{
  UINTN  Elapsed;

  for (Elapsed = 0; ; Elapsed++) {
    if ((MmioRead32 (Address) & Mask) == Value) {
      return EFI_SUCCESS;
    }

    if (Elapsed >= TimeoutUs) {
      return EFI_TIMEOUT;
    }

    MicroSecondDelay (1);
  }
}

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
  )
{
  UINT32      Entry;
  UINT32      Value;
  EFI_STATUS  Status;

  ASSERT ((Waits & ~(UINT32)GCC_GDSC_WAITS_MASK) == 0);

  //
  // gdsc_init: collapse and restore follow the register writes (no
  // hardware trigger), the hardware sequencer is used (no software
  // override), with the domain's wait times.
  //
  Entry = MmioRead32 (Gdscr);
  GccUpdateBits (Gdscr, GCC_GDSC_INIT_MASK, Waits & GCC_GDSC_WAITS_MASK);

  Value = MmioRead32 (Gdscr);
  if (((Value & GCC_GDSC_PWR_ON) != 0) && ((Value & GCC_GDSC_SW_COLLAPSE) == 0)) {
    //
    // On already, and with our vote. Keep it on and make sure it retains
    // its flops if it ever collapses, as gdsc_init does for a GDSC it
    // finds on.
    //
    GccUpdateBits (Gdscr, GCC_GDSC_RETAIN_FF_ENABLE, GCC_GDSC_RETAIN_FF_ENABLE);
    DEBUG ((
      DEBUG_INFO,
      "%a: %a GDSC on already: GDSCR 0x%x -> 0x%x, CFG_GDSCR 0x%x\n",
      __func__,
      Name,
      Entry,
      MmioRead32 (Gdscr),
      MmioRead32 (Gdscr + GCC_GDSC_CFG_GDSCR)
      ));
    return EFI_SUCCESS;
  }

  if ((Value & GCC_GDSC_PWR_ON) != 0) {
    //
    // On, but only through another master's vote (the PCIe and USB GDSCs
    // are VOTABLE), or through the hardware trigger turned off above:
    // gdsc_init clears SW_COLLAPSE for a votable GDSC it finds on, so that
    // the domain stays on when the other vote goes. The status is polled
    // below all the same, as for a domain that was off.
    //
    DEBUG ((
      DEBUG_INFO,
      "%a: %a GDSC on without our vote (GDSCR 0x%x), voting\n",
      __func__,
      Name,
      Value
      ));
  }

  //
  // gdsc_enable -> gdsc_toggle_logic: clear SW_COLLAPSE, poll PWR_ON.
  //
  GccUpdateBits (Gdscr, GCC_GDSC_SW_COLLAPSE, 0);
  Status = GccPoll32 (Gdscr, GCC_GDSC_PWR_ON, GCC_GDSC_PWR_ON, GCC_GDSC_TIMEOUT_US);
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "%a: %a GDSC did not power up in %u us: GDSCR 0x%x -> 0x%x, CFG_GDSCR 0x%x\n",
      __func__,
      Name,
      GCC_GDSC_TIMEOUT_US,
      Entry,
      MmioRead32 (Gdscr),
      MmioRead32 (Gdscr + GCC_GDSC_CFG_GDSCR)
      ));
    return EFI_DEVICE_ERROR;
  }

  //
  // Clocks of the domain take a few cycles to come back after power-up, and
  // must not be turned on within 400 ns of the memories getting power
  // (gdsc_enable).
  //
  MicroSecondDelay (1);
  GccUpdateBits (Gdscr, GCC_GDSC_RETAIN_FF_ENABLE, GCC_GDSC_RETAIN_FF_ENABLE);

  DEBUG ((
    DEBUG_INFO,
    "%a: %a GDSC on: GDSCR 0x%x -> 0x%x, CFG_GDSCR 0x%x\n",
    __func__,
    Name,
    Entry,
    MmioRead32 (Gdscr),
    MmioRead32 (Gdscr + GCC_GDSC_CFG_GDSCR)
    ));

  return EFI_SUCCESS;
}

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
  )
{
  UINT32       Old;
  UINT32       Value;
  UINTN        Elapsed;
  CONST CHAR8  *How;
  EFI_STATUS   Status;

  ASSERT (EnableMask != 0);
  ASSERT (Cbcr != 0);

  //
  // clk_enable_regmap.
  //
  Old = MmioRead32 (EnableReg);
  GccUpdateBits (EnableReg, EnableMask, EnableMask);

  //
  // clk_branch_wait: nothing to wait for when the halt bit is ignored, or
  // when the branch is in hardware gated mode (it may read off while
  // gated). On enable, BRANCH_HALT and BRANCH_HALT_VOTED poll the CBCR
  // alike.
  //
  Status = EFI_SUCCESS;
  Value  = MmioRead32 (Cbcr);
  if (Halt == GccHaltSkip) {
    How = ", not waited for";
  } else if ((Value & GCC_CBCR_HW_CTL) != 0) {
    How = ", hardware gated, not waited for";
  } else {
    How = "";
    for (Elapsed = 0; ; Elapsed++) {
      Value = MmioRead32 (Cbcr);
      if (GccCbcrIsOn (Value)) {
        break;
      }

      if (Elapsed >= GCC_BRANCH_TIMEOUT_US) {
        Status = EFI_TIMEOUT;
        break;
      }

      MicroSecondDelay (1);
    }
  }

  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "%a: %a did not start in %u us: enable 0x%lx 0x%x -> 0x%x (mask 0x%x), CBCR 0x%lx 0x%x\n",
      __func__,
      Name,
      GCC_BRANCH_TIMEOUT_US,
      (UINT64)EnableReg,
      Old,
      MmioRead32 (EnableReg),
      EnableMask,
      (UINT64)Cbcr,
      MmioRead32 (Cbcr)
      ));
    return Status;
  }

  if (EnableReg == Cbcr) {
    DEBUG ((
      DEBUG_INFO,
      "%a: %a on: CBCR 0x%x -> 0x%x%a\n",
      __func__,
      Name,
      Old,
      Value,
      How
      ));
  } else {
    DEBUG ((
      DEBUG_INFO,
      "%a: %a on: vote 0x%lx 0x%x -> 0x%x, CBCR 0x%x%a\n",
      __func__,
      Name,
      (UINT64)EnableReg,
      Old,
      MmioRead32 (EnableReg),
      Value,
      How
      ));
  }

  return EFI_SUCCESS;
}

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
  )
{
  UINTN       CfgRcgr;
  UINT32      Cmd;
  UINT32      Old;
  UINT32      New;
  EFI_STATUS  Status;

  ASSERT ((Cfg & ~(UINT32)GCC_RCG_CFG_MASK) == 0);

  CfgRcgr = CmdRcgr + GCC_RCG_CFG;
  Cmd     = MmioRead32 (CmdRcgr);
  Old     = MmioRead32 (CfgRcgr);
  New     = (Old & ~(UINT32)GCC_RCG_CFG_MASK) | (Cfg & GCC_RCG_CFG_MASK);

  //
  // A configuration that was written but not taken (a dirty bit set) is not
  // the one the root runs: update it then too.
  //
  if ((New == Old) && ((Cmd & (GCC_RCG_CMD_UPDATE | GCC_RCG_CMD_DIRTY_MASK)) == 0)) {
    DEBUG ((
      DEBUG_INFO,
      "%a: %a root has CFG 0x%x already (CMD 0x%x)\n",
      __func__,
      Name,
      Old,
      Cmd
      ));
    return EFI_SUCCESS;
  }

  if (New != Old) {
    MmioWrite32 (CfgRcgr, New);
  }

  //
  // update_config: the root switches on UPDATE and clears it once both
  // sources have handed over.
  //
  GccUpdateBits (CmdRcgr, GCC_RCG_CMD_UPDATE, GCC_RCG_CMD_UPDATE);
  Status = GccPoll32 (CmdRcgr, GCC_RCG_CMD_UPDATE, 0, GCC_RCG_TIMEOUT_US);
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "%a: %a root did not update in %u us: CFG 0x%x -> 0x%x, CMD 0x%x -> 0x%x\n",
      __func__,
      Name,
      GCC_RCG_TIMEOUT_US,
      Old,
      MmioRead32 (CfgRcgr),
      Cmd,
      MmioRead32 (CmdRcgr)
      ));
    return Status;
  }

  DEBUG ((
    DEBUG_INFO,
    "%a: %a root CFG 0x%x -> 0x%x, CMD 0x%x -> 0x%x\n",
    __func__,
    Name,
    Old,
    MmioRead32 (CfgRcgr),
    Cmd,
    MmioRead32 (CmdRcgr)
    ));

  return EFI_SUCCESS;
}

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
  )
{
  UINT32  Entry;
  UINT32  Asserted;
  UINT32  Released;

  Entry = MmioRead32 (Bcr);

  //
  // qcom_reset_set_assert reads the register back to make sure the write
  // has landed before the delay starts.
  //
  GccUpdateBits (Bcr, GCC_BCR_BLK_ARES, GCC_BCR_BLK_ARES);
  Asserted = MmioRead32 (Bcr);
  MicroSecondDelay (AssertUs);

  GccUpdateBits (Bcr, GCC_BCR_BLK_ARES, 0);
  Released = MmioRead32 (Bcr);
  MicroSecondDelay (DeassertUs);

  DEBUG ((
    DEBUG_INFO,
    "%a: BCR 0x%lx: 0x%x, asserted 0x%x for %lu us, released 0x%x, waited %lu us\n",
    __func__,
    (UINT64)Bcr,
    Entry,
    Asserted,
    (UINT64)AssertUs,
    Released,
    (UINT64)DeassertUs
    ));
}
