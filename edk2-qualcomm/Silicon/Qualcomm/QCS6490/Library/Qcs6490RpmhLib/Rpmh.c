/** @file
  Polled RPMh requests through the apps RSC: regulator, power rail and
  interconnect votes.

  RPMh resources (PMIC regulators, bus clock managers) are voted through the
  RSC of the apps subsystem. The VM that UEFI and Linux share under Gunyah
  owns DRV2 of that RSC (sc7280.dtsi apps_rsc, qcom,drv-id = 2), and its
  trigger command sets (TCSes) 0 and 1 are the active ones (qcom,tcs-config:
  2 active, 3 sleep, 3 wake). A request goes into an idle active TCS and is
  triggered in active mode (AMC), the way the Linux RSC driver
  (drivers/soc/qcom/rpmh-rsc.c) sends ACTIVE_ONLY requests. The completion
  is polled from the command status words instead of taken from the RSC
  interrupt.

  Linux assumes it starts with idle TCSes: it ORs the CMD_ENABLE bits it
  finds into its next message, so stale bits make it resend old commands.
  Every request therefore leaves its TCS with no command enabled, not
  triggered, AMC mode off and its interrupt cleared.

  Reads are commands without the write bit; the RSC puts the answer in the
  command's RESP_DATA word. Linux does not use them. U-Boot sends them on
  the same RSC to read back regulator and interconnect votes (U-Boot
  drivers/soc/qcom/rpmh-rsc.c, "soc/qcom: rpmh: add RPMh read"). Here they
  serve logs and saving a vote to put back later.

  Copyright (c) 2026, edk2-flange contributors.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/Qcs6490RpmhLib.h>
#include <Library/TimerLib.h>

//
// DRV2 of the apps RSC (sc7280.dtsi apps_rsc "drv-2").
//
#define RSC_DRV2_BASE    0x18220000
#define RSC_DRV_ID_HLOS  2

//
// DRV registers, "ver_2_7" layout (rpmh-rsc.c), used for RSC major version 2.
//
#define RSC_DRV_ID                0x000
#define RSC_DRV_ID_MAJOR(x)       (((x) >> 16) & 0xFF)
#define RSC_DRV_ID_MINOR(x)       (((x) >> 8) & 0xFF)
#define DRV_SOLVER_CONFIG         0x004
#define DRV_HW_SOLVER             BIT24
#define DRV_PRNT_CHLD_CONFIG      0x00C
#define DRV_NUM_TCS(Config, Drv)  (((Config) >> (6 * (Drv))) & 0x3F)
#define DRV_NCPT(Config)          (((Config) >> 27) & 0x1F)

//
// TCS block, qcom,tcs-offset = 0xd00.
//
#define RSC_TCS_BASE        (RSC_DRV2_BASE + 0xD00)
#define RSC_DRV_IRQ_ENABLE  0x00
#define RSC_DRV_IRQ_STATUS  0x04
#define RSC_DRV_IRQ_CLEAR   0x08

//
// Per TCS, at RSC_TCS_BASE + RSC_DRV_TCS_SIZE * n.
//
#define RSC_DRV_TCS_SIZE           0x2A0
#define RSC_DRV_CMD_WAIT_FOR_CMPL  0x10
#define RSC_DRV_CONTROL            0x14
#define RSC_DRV_STATUS             0x18
#define RSC_DRV_CMD_ENABLE         0x1C

//
// Per command, at + RSC_DRV_CMD_SIZE * j.
//
#define RSC_DRV_CMD_SIZE       0x14
#define RSC_DRV_CMD_MSGID      0x30
#define RSC_DRV_CMD_ADDR       0x34
#define RSC_DRV_CMD_DATA       0x38
#define RSC_DRV_CMD_STATUS     0x3C
#define RSC_DRV_CMD_RESP_DATA  0x40

#define TCS_AMC_MODE_ENABLE   BIT16
#define TCS_AMC_MODE_TRIGGER  BIT24

#define CMD_MSGID_LEN       8
#define CMD_MSGID_RESP_REQ  BIT8
#define CMD_MSGID_WRITE     BIT16
#define CMD_STATUS_ISSUED   BIT8
#define CMD_STATUS_COMPL    BIT16

#define RSC_TCS_REG(Tcs, Reg) \
  (RSC_TCS_BASE + (UINTN)(Tcs) * RSC_DRV_TCS_SIZE + (Reg))
#define RSC_TCS_CMD_REG(Tcs, Cmd, Reg) \
  (RSC_TCS_REG (Tcs, Reg) + (UINTN)(Cmd) * RSC_DRV_CMD_SIZE)

//
// TCS groups in the order of qcom,tcs-config; the control TCS takes no
// slot.
//
#define RSC_ACTIVE_TCS_FIRST  0
#define RSC_ACTIVE_TCS_COUNT  2
#define RSC_SLEEP_TCS_FIRST   2
#define RSC_WAKE_TCS_FIRST    5

//
// Room for 16 commands per TCS is reserved in the register map.
//
#define RSC_MAX_CMDS_PER_TCS  16

//
// More TCSes than this are not logged: DT describes 8.
//
#define RSC_MAX_TCS_LOGGED  16

//
// Linux retries a control register write for up to a second until it reads
// back; it takes a few cycles. Linux waits up to 10 s for a request to
// complete; regulator and bus votes take well under a millisecond.
//
#define RPMH_SYNC_TIMEOUT_US  1000
#define RPMH_TIMEOUT_US       100000

STATIC BOOLEAN  mRpmhReady;
STATIC UINT32   mRpmhNcpt;

/**
  Polls a 32-bit register until it reads a value.

  @param[in]  Address    The register.
  @param[in]  Value      The value.
  @param[in]  TimeoutUs  How long to wait, in microseconds.

  @retval EFI_SUCCESS  The register reads the value.
  @retval EFI_TIMEOUT  It does not.
**/
STATIC
EFI_STATUS
RpmhPoll (
  IN UINTN   Address,
  IN UINT32  Value,
  IN UINTN   TimeoutUs
  )
{
  UINTN  Elapsed;

  for (Elapsed = 0; ; Elapsed++) {
    if (MmioRead32 (Address) == Value) {
      return EFI_SUCCESS;
    }

    if (Elapsed >= TimeoutUs) {
      return EFI_TIMEOUT;
    }

    MicroSecondDelay (1);
  }
}

/**
  Writes a TCS register and waits until it reads back the value, as the
  hardware requires for the control register (rpmh-rsc.c
  write_tcs_reg_sync).

  @param[in]  Address  The register.
  @param[in]  Value    The value.

  @retval EFI_SUCCESS  The register holds the value.
  @retval EFI_TIMEOUT  It does not.
**/
STATIC
EFI_STATUS
RpmhWriteSync (
  IN UINTN   Address,
  IN UINT32  Value
  )
{
  EFI_STATUS  Status;

  MmioWrite32 (Address, Value);
  Status = RpmhPoll (Address, Value, RPMH_SYNC_TIMEOUT_US);
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "%a: 0x%lx reads 0x%x after writing 0x%x\n",
      __func__,
      (UINT64)Address,
      MmioRead32 (Address),
      Value
      ));
  }

  return Status;
}

/**
  Takes a TCS out of AMC mode and, if asked, triggers it again: clear the
  trigger, then the AMC mode, each confirmed by a read back, then set the AMC
  mode and trigger (rpmh-rsc.c __tcs_set_trigger).

  @param[in]  Tcs      The TCS.
  @param[in]  Trigger  TRUE to start the commands it holds.

  @retval EFI_SUCCESS  Done.
  @retval EFI_TIMEOUT  The control register did not take a value.
**/
STATIC
EFI_STATUS
RpmhSetTrigger (
  IN UINTN    Tcs,
  IN BOOLEAN  Trigger
  )
{
  UINTN       Control;
  UINT32      Value;
  EFI_STATUS  Status;

  Control = RSC_TCS_REG (Tcs, RSC_DRV_CONTROL);

  Value  = MmioRead32 (Control);
  Value &= ~TCS_AMC_MODE_TRIGGER;
  Status = RpmhWriteSync (Control, Value);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Value &= ~TCS_AMC_MODE_ENABLE;
  Status = RpmhWriteSync (Control, Value);
  if (EFI_ERROR (Status) || !Trigger) {
    return Status;
  }

  Value  = TCS_AMC_MODE_ENABLE;
  Status = RpmhWriteSync (Control, Value);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  MmioWrite32 (Control, Value | TCS_AMC_MODE_TRIGGER);
  return EFI_SUCCESS;
}

/**
  Leaves a TCS the way Linux expects to find it: not triggered, AMC mode
  off, no command enabled and its interrupt cleared (rpmh-rsc.c
  tcs_tx_done).

  @param[in]  Tcs  The TCS.
**/
STATIC
VOID
RpmhReleaseTcs (
  IN UINTN  Tcs
  )
{
  RpmhSetTrigger (Tcs, FALSE);
  RpmhWriteSync (RSC_TCS_REG (Tcs, RSC_DRV_CMD_ENABLE), 0);
  MmioWrite32 (RSC_TCS_BASE + RSC_DRV_IRQ_CLEAR, (UINT32)1 << Tcs);
}

/**
  Picks the first idle active TCS, as Linux does with an empty in-use map.
  A TCS is idle when its STATUS register is non-zero.

  @param[out]  Tcs  The TCS.

  @retval EFI_SUCCESS    Found.
  @retval EFI_NOT_READY  Both active TCSes are busy.
**/
STATIC
EFI_STATUS
RpmhClaimTcs (
  OUT UINTN  *Tcs
  )
{
  UINTN   Index;
  UINT32  Enabled;

  for (Index = RSC_ACTIVE_TCS_FIRST; Index < RSC_ACTIVE_TCS_FIRST + RSC_ACTIVE_TCS_COUNT; Index++) {
    if (MmioRead32 (RSC_TCS_REG (Index, RSC_DRV_STATUS)) == 0) {
      continue;
    }

    Enabled = MmioRead32 (RSC_TCS_REG (Index, RSC_DRV_CMD_ENABLE));
    if (Enabled != 0) {
      //
      // Left by an earlier boot stage. The request below replaces them, and
      // the TCS is left with none.
      //
      DEBUG ((DEBUG_WARN, "%a: TCS%u is idle with stale CMD_ENABLE 0x%x\n", __func__, (UINT32)Index, Enabled));
    }

    *Tcs = Index;
    return EFI_SUCCESS;
  }

  DEBUG ((
    DEBUG_ERROR,
    "%a: no idle active TCS (TCS0 STATUS 0x%x CTRL 0x%x, TCS1 STATUS 0x%x CTRL 0x%x)\n",
    __func__,
    MmioRead32 (RSC_TCS_REG (0, RSC_DRV_STATUS)),
    MmioRead32 (RSC_TCS_REG (0, RSC_DRV_CONTROL)),
    MmioRead32 (RSC_TCS_REG (1, RSC_DRV_STATUS)),
    MmioRead32 (RSC_TCS_REG (1, RSC_DRV_CONTROL))
    ));
  return EFI_NOT_READY;
}

/**
  Waits until every command of a triggered TCS has been sent, and answered
  when it asked for a response.

  @param[in]  Tcs     The TCS.
  @param[in]  Cmds    Its commands.
  @param[in]  Count   How many.
  @param[in]  IsRead  TRUE for a read, which always asks for a response.

  @retval EFI_SUCCESS  Complete.
  @retval EFI_TIMEOUT  Not complete in RPMH_TIMEOUT_US.
**/
STATIC
EFI_STATUS
RpmhWaitDone (
  IN UINTN           Tcs,
  IN CONST RPMH_CMD  *Cmds,
  IN UINTN           Count,
  IN BOOLEAN         IsRead
  )
{
  UINTN   Elapsed;
  UINTN   Index;
  UINT32  Pending;
  UINT32  Need;

  for (Elapsed = 0; ; Elapsed++) {
    Pending = 0;
    for (Index = 0; Index < Count; Index++) {
      Need = (IsRead || Cmds[Index].Wait) ? CMD_STATUS_COMPL : CMD_STATUS_ISSUED;
      if ((MmioRead32 (RSC_TCS_CMD_REG (Tcs, Index, RSC_DRV_CMD_STATUS)) & Need) == 0) {
        Pending |= (UINT32)1 << Index;
      }
    }

    if (Pending == 0) {
      return EFI_SUCCESS;
    }

    if (Elapsed >= RPMH_TIMEOUT_US) {
      break;
    }

    MicroSecondDelay (1);
  }

  DEBUG ((
    DEBUG_ERROR,
    "%a: TCS%u commands 0x%x not done after %u us (STATUS 0x%x CTRL 0x%x IRQ_STATUS 0x%x)\n",
    __func__,
    (UINT32)Tcs,
    Pending,
    RPMH_TIMEOUT_US,
    MmioRead32 (RSC_TCS_REG (Tcs, RSC_DRV_STATUS)),
    MmioRead32 (RSC_TCS_REG (Tcs, RSC_DRV_CONTROL)),
    MmioRead32 (RSC_TCS_BASE + RSC_DRV_IRQ_STATUS)
    ));
  for (Index = 0; Index < Count; Index++) {
    DEBUG ((
      DEBUG_ERROR,
      "%a:   cmd%u addr 0x%x status 0x%x\n",
      __func__,
      (UINT32)Index,
      Cmds[Index].Addr,
      MmioRead32 (RSC_TCS_CMD_REG (Tcs, Index, RSC_DRV_CMD_STATUS))
      ));
  }

  return EFI_TIMEOUT;
}

/**
  Sends one active-only request in one TCS and waits for it.

  @param[in]   Cmds      The commands.
  @param[in]   Count     How many.
  @param[in]   IsRead    TRUE for a one-command read.
  @param[out]  Response  The read's answer; unused for writes.

  @retval EFI_SUCCESS  Done.
  @retval Other        See RpmhWrite().
**/
STATIC
EFI_STATUS
RpmhSend (
  IN  CONST RPMH_CMD  *Cmds,
  IN  UINTN           Count,
  IN  BOOLEAN         IsRead,
  OUT UINT32          *Response OPTIONAL
  )
{
  EFI_STATUS  Status;
  UINTN       Tcs;
  UINTN       Index;
  UINT32      MsgId;
  UINT32      Enable;
  UINTN       Waited;

  Status = RpmhInit ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if ((Count == 0) || (Count > mRpmhNcpt) || (IsRead && (Count != 1))) {
    return EFI_INVALID_PARAMETER;
  }

  Status = RpmhClaimTcs (&Tcs);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // Commands as rpmh-rsc.c __tcs_buffer_write writes them. A read leaves out
  // the write bit and the data (U-Boot).
  //
  Enable = 0;
  for (Index = 0; Index < Count; Index++) {
    MsgId = CMD_MSGID_LEN;
    if (!IsRead) {
      MsgId |= CMD_MSGID_WRITE;
    }

    if (IsRead || Cmds[Index].Wait) {
      MsgId |= CMD_MSGID_RESP_REQ;
    }

    MmioWrite32 (RSC_TCS_CMD_REG (Tcs, Index, RSC_DRV_CMD_MSGID), MsgId);
    MmioWrite32 (RSC_TCS_CMD_REG (Tcs, Index, RSC_DRV_CMD_ADDR), Cmds[Index].Addr);
    if (!IsRead) {
      MmioWrite32 (RSC_TCS_CMD_REG (Tcs, Index, RSC_DRV_CMD_DATA), Cmds[Index].Data);
    }

    Enable |= (UINT32)1 << Index;
  }

  MmioWrite32 (RSC_TCS_REG (Tcs, RSC_DRV_CMD_ENABLE), Enable);

  Status = RpmhSetTrigger (Tcs, TRUE);
  if (!EFI_ERROR (Status)) {
    //
    // The trigger restarts the status words of the commands, which still
    // hold those of the previous request. Read the control register back so
    // that the trigger has reached the RSC before they are polled.
    //
    MmioRead32 (RSC_TCS_REG (Tcs, RSC_DRV_CONTROL));
    Status = RpmhWaitDone (Tcs, Cmds, Count, IsRead);
  }

  if (!EFI_ERROR (Status) && IsRead) {
    *Response = MmioRead32 (RSC_TCS_CMD_REG (Tcs, 0, RSC_DRV_CMD_RESP_DATA));
  }

  RpmhReleaseTcs (Tcs);

  //
  // The TCS should be idle and clean again; say so if it is not, as Linux
  // would find it that way.
  //
  Waited = 0;
  while ((MmioRead32 (RSC_TCS_REG (Tcs, RSC_DRV_STATUS)) == 0) && (Waited < RPMH_SYNC_TIMEOUT_US)) {
    MicroSecondDelay (1);
    Waited++;
  }

  if ((MmioRead32 (RSC_TCS_REG (Tcs, RSC_DRV_STATUS)) == 0) ||
      (MmioRead32 (RSC_TCS_REG (Tcs, RSC_DRV_CMD_ENABLE)) != 0))
  {
    DEBUG ((
      DEBUG_ERROR,
      "%a: TCS%u not idle after the request: STATUS 0x%x CTRL 0x%x CMD_ENABLE 0x%x\n",
      __func__,
      (UINT32)Tcs,
      MmioRead32 (RSC_TCS_REG (Tcs, RSC_DRV_STATUS)),
      MmioRead32 (RSC_TCS_REG (Tcs, RSC_DRV_CONTROL)),
      MmioRead32 (RSC_TCS_REG (Tcs, RSC_DRV_CMD_ENABLE))
      ));
  }

  return Status;
}

/**
  Checks that DRV2 of the apps RSC has the register layout and the active
  TCSes this library expects.

  @retval EFI_SUCCESS      Usable.
  @retval EFI_UNSUPPORTED  Unknown RSC version or TCS configuration.
**/
EFI_STATUS
EFIAPI
RpmhInit (
  VOID
  )
{
  UINT32  Id;
  UINT32  Config;
  UINT32  Solver;
  UINT32  NumTcs;

  if (mRpmhReady) {
    return EFI_SUCCESS;
  }

  Id        = MmioRead32 (RSC_DRV2_BASE + RSC_DRV_ID);
  Solver    = MmioRead32 (RSC_DRV2_BASE + DRV_SOLVER_CONFIG);
  Config    = MmioRead32 (RSC_DRV2_BASE + DRV_PRNT_CHLD_CONFIG);
  NumTcs    = DRV_NUM_TCS (Config, RSC_DRV_ID_HLOS);
  mRpmhNcpt = DRV_NCPT (Config);

  DEBUG ((
    DEBUG_INFO,
    "%a: apps RSC DRV2 v%u.%u, %u TCS of %u commands%a\n",
    __func__,
    RSC_DRV_ID_MAJOR (Id),
    RSC_DRV_ID_MINOR (Id),
    NumTcs,
    mRpmhNcpt,
    ((Solver & DRV_HW_SOLVER) != 0) ? ", HW solver" : ""
    ));

  //
  // Version 3 moved the TCS registers (rpmh_rsc_reg_offset_ver_3_0).
  //
  if (RSC_DRV_ID_MAJOR (Id) != 2) {
    DEBUG ((DEBUG_ERROR, "%a: unknown RSC layout (ID 0x%x)\n", __func__, Id));
    return EFI_UNSUPPORTED;
  }

  if ((NumTcs < RSC_ACTIVE_TCS_FIRST + RSC_ACTIVE_TCS_COUNT) ||
      (mRpmhNcpt == 0) || (mRpmhNcpt > RSC_MAX_CMDS_PER_TCS))
  {
    DEBUG ((DEBUG_ERROR, "%a: unexpected TCS configuration 0x%x\n", __func__, Config));
    return EFI_UNSUPPORTED;
  }

  mRpmhReady = TRUE;
  return EFI_SUCCESS;
}

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
EFIAPI
RpmhWrite (
  IN CONST RPMH_CMD  *Cmds,
  IN UINTN           Count
  )
{
  EFI_STATUS  Status;
  UINTN       Index;

  //
  // Logged before sending, so that a log that stops here shows the request.
  //
  for (Index = 0; Index < Count; Index++) {
    DEBUG ((
      DEBUG_INFO,
      "%a: 0x%05x <- 0x%08x%a\n",
      __func__,
      Cmds[Index].Addr,
      Cmds[Index].Data,
      Cmds[Index].Wait ? " (response required)" : ""
      ));
  }

  Status = RpmhSend (Cmds, Count, FALSE, NULL);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: request of %u commands failed: %r\n", __func__, (UINT32)Count, Status));
  }

  return Status;
}

/**
  Reads the vote of an RPMh resource address. Leaves the TCS it used idle
  and clean.

  @param[in]   Addr  The resource address.
  @param[out]  Data  The response.

  @retval EFI_SUCCESS  Read.
  @retval Other        As for RpmhWrite().
**/
EFI_STATUS
EFIAPI
RpmhRead (
  IN  UINT32  Addr,
  OUT UINT32  *Data
  )
{
  RPMH_CMD  Cmd;

  Cmd.Addr = Addr;
  Cmd.Data = 0;
  Cmd.Wait = TRUE;

  return RpmhSend (&Cmd, 1, TRUE, Data);
}

/**
  Logs DRV2 of the apps RSC: version, TCS configuration, interrupt state and,
  for each TCS, its control words and the commands it holds. Reads only.

  @param[in]  When  A label for the log lines.
**/
VOID
EFIAPI
RpmhLogState (
  IN CONST CHAR8  *When
  )
{
  UINT32       Id;
  UINT32       Config;
  UINT32       NumTcs;
  UINT32       Ncpt;
  UINT32       Tcs;
  UINT32       Cmd;
  UINT32       Enabled;
  UINT32       Addr;
  CONST CHAR8  *Type;

  Id     = MmioRead32 (RSC_DRV2_BASE + RSC_DRV_ID);
  Config = MmioRead32 (RSC_DRV2_BASE + DRV_PRNT_CHLD_CONFIG);

  DEBUG ((
    DEBUG_INFO,
    "%a: [%a] DRV2 ID 0x%x solver 0x%x config 0x%x IRQ_ENABLE 0x%x IRQ_STATUS 0x%x\n",
    __func__,
    When,
    Id,
    MmioRead32 (RSC_DRV2_BASE + DRV_SOLVER_CONFIG),
    Config,
    MmioRead32 (RSC_TCS_BASE + RSC_DRV_IRQ_ENABLE),
    MmioRead32 (RSC_TCS_BASE + RSC_DRV_IRQ_STATUS)
    ));

  if (RSC_DRV_ID_MAJOR (Id) != 2) {
    DEBUG ((DEBUG_WARN, "%a: [%a] unknown RSC layout, TCSes not logged\n", __func__, When));
    return;
  }

  NumTcs = MIN (DRV_NUM_TCS (Config, RSC_DRV_ID_HLOS), RSC_MAX_TCS_LOGGED);
  Ncpt   = MIN (DRV_NCPT (Config), RSC_MAX_CMDS_PER_TCS);

  for (Tcs = 0; Tcs < NumTcs; Tcs++) {
    if (Tcs < RSC_SLEEP_TCS_FIRST) {
      Type = "active";
    } else if (Tcs < RSC_WAKE_TCS_FIRST) {
      Type = "sleep";
    } else {
      Type = "wake";
    }

    Enabled = MmioRead32 (RSC_TCS_REG (Tcs, RSC_DRV_CMD_ENABLE));
    DEBUG ((
      DEBUG_INFO,
      "%a: [%a] TCS%u %a: WAIT 0x%x CTRL 0x%x STATUS 0x%x CMD_ENABLE 0x%x\n",
      __func__,
      When,
      Tcs,
      Type,
      MmioRead32 (RSC_TCS_REG (Tcs, RSC_DRV_CMD_WAIT_FOR_CMPL)),
      MmioRead32 (RSC_TCS_REG (Tcs, RSC_DRV_CONTROL)),
      MmioRead32 (RSC_TCS_REG (Tcs, RSC_DRV_STATUS)),
      Enabled
      ));

    //
    // Enabled commands, and the last request of the active TCSes, which
    // shows who used them before.
    //
    for (Cmd = 0; Cmd < Ncpt; Cmd++) {
      Addr = MmioRead32 (RSC_TCS_CMD_REG (Tcs, Cmd, RSC_DRV_CMD_ADDR));
      if ((((Enabled >> Cmd) & 1) == 0) && (Addr == 0)) {
        continue;
      }

      DEBUG ((
        DEBUG_INFO,
        "%a: [%a]   cmd%u%a msgid 0x%x addr 0x%05x data 0x%08x status 0x%x\n",
        __func__,
        When,
        Cmd,
        (((Enabled >> Cmd) & 1) != 0) ? " (enabled)" : "",
        MmioRead32 (RSC_TCS_CMD_REG (Tcs, Cmd, RSC_DRV_CMD_MSGID)),
        Addr,
        MmioRead32 (RSC_TCS_CMD_REG (Tcs, Cmd, RSC_DRV_CMD_DATA)),
        MmioRead32 (RSC_TCS_CMD_REG (Tcs, Cmd, RSC_DRV_CMD_STATUS))
        ));
    }
  }
}
