/** @file
  Secure monitor calls to TrustZone for the DSP preload: PAS (peripheral
  authentication service) and the SHM bridge.

  The calls follow Linux's qcom_scm "smc arm 64" convention, which this board
  uses (drivers/firmware/qcom/qcom_scm-smc.c): a standard SMC64 SiP call
  whose function ID carries the service and command, the argument types in
  x1, up to four arguments in x2-x5, the status in x0 and results in x1-x3.
  A call TrustZone was interrupted in is resumed with x0 = 1 and the x6 it
  returned; a busy TrustZone is asked again 30 ms later, up to 20 times.

  Copyright (c) 2026, edk2-flange contributors.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Library/ArmLib.h>
#include <Library/ArmSmcLib.h>

#include "DspPreload.h"

//
// Services and commands (qcom_scm.h).
//
#define SCM_SVC_PIL                 0x02
#define SCM_PIL_PAS_INIT_IMAGE      0x01
#define SCM_PIL_PAS_MEM_SETUP       0x02
#define SCM_PIL_PAS_AUTH_AND_RESET  0x05
#define SCM_PIL_PAS_SHUTDOWN        0x06
#define SCM_PIL_PAS_IS_SUPPORTED    0x07

#define SCM_SVC_INFO            0x06
#define SCM_INFO_IS_CALL_AVAIL  0x01

#define SCM_SVC_MP                0x0C
#define SCM_MP_SHM_BRIDGE_ENABLE  0x1C
#define SCM_MP_SHM_BRIDGE_DELETE  0x1D
#define SCM_MP_SHM_BRIDGE_CREATE  0x1E

//
// Function IDs (ARM SMCCC): SMC64 in bit 30, owner SiP (2) in bits 29:24,
// the service and command in the low 16 bits. Standard calls leave bit 31
// clear.
//
#define SCM_OWNER_SIP           (0x2U << 24)
#define SCM_FNID(Svc, Cmd)      (((Svc) << 8) | (Cmd))
#define SCM_SMC64               BIT30
#define SCM_STD_CALL(Svc, Cmd)  (SCM_SMC64 | SCM_OWNER_SIP | SCM_FNID (Svc, Cmd))

//
// Argument info (QCOM_SCM_ARGS): the number of arguments in bits 3:0, then
// two bits of type per argument.
//
#define SCM_ARG_VAL            0
#define SCM_ARG_RW             2
#define SCM_ARGS(n)            (n)
#define SCM_ARG_TYPE(Index, Type)  ((UINT32)(Type) << (4 + 2 * (Index)))

//
// Status codes in x0 (qcom_scm.h), and Linux's retry policy for a busy
// TrustZone (qcom_scm-smc.c).
//
#define SCM_INTERRUPTED      1
#define SCM_WAITQ_SLEEP      2
#define SCM_ERROR            (-1)
#define SCM_EINVAL_ADDR      (-2)
#define SCM_EINVAL_ARG       (-3)
#define SCM_EOPNOTSUPP       (-4)
#define SCM_ENOMEM           (-5)
#define SCM_V2_EBUSY         (-12)
#define SCM_BUSY_RETRIES     20
#define SCM_BUSY_DELAY_US    30000

//
// SHM bridge (qcom_tzmem.c): read and write permissions, and the flags that
// make the bridge owned by the non-secure world itself, as Linux does at EL2
// with qcom,shm-bridge-vmid = <QCOM_SCM_VMID_SELF_OWNER>.
//
#define SHM_BRIDGE_PERM_RW                  6
#define SHM_BRIDGE_SELF_OWNER               BIT1
#define SHM_BRIDGE_SELF_OWNER_PERM_SHIFT    2
#define SHM_BRIDGE_RESULT_NOTSUPP           4

/**
  Makes a call and returns its registers.

  @param[in]   Svc      The service.
  @param[in]   Cmd      The command.
  @param[in]   ArgInfo  The argument types.
  @param[in]   Arg0     First argument.
  @param[in]   Arg1     Second argument.
  @param[in]   Arg2     Third argument.
  @param[in]   Arg3     Fourth argument.
  @param[out]  Result   x0 to x3 after the call.

  @retval EFI_SUCCESS            TrustZone ran the call (x0 is 0).
  @retval EFI_UNSUPPORTED        It does not know the call.
  @retval EFI_INVALID_PARAMETER  It refused an argument.
  @retval EFI_OUT_OF_RESOURCES   It ran out of memory.
  @retval EFI_NOT_READY          It wants to sleep, which this caller cannot
                                 wait for.
  @retval EFI_DEVICE_ERROR       Any other error.
**/
STATIC
EFI_STATUS
ScmCall (
  IN  UINT32        Svc,
  IN  UINT32        Cmd,
  IN  UINT32        ArgInfo,
  IN  UINT64        Arg0,
  IN  UINT64        Arg1,
  IN  UINT64        Arg2,
  IN  UINT64        Arg3,
  OUT ARM_SMC_ARGS  *Result
  )
{
  ARM_SMC_ARGS  Args;
  UINTN         Function;
  UINTN         Session;
  UINTN         Retry;
  INT32         Status;

  for (Retry = 0; ; Retry++) {
    Function = SCM_STD_CALL (Svc, Cmd);
    Session  = 0;

    //
    // Buffers handed over by address must be in memory before TrustZone
    // reads them.
    //
    ArmDataSynchronizationBarrier ();

    do {
      ZeroMem (&Args, sizeof (Args));
      Args.Arg0 = Function;
      Args.Arg1 = ArgInfo;
      Args.Arg2 = (UINTN)Arg0;
      Args.Arg3 = (UINTN)Arg1;
      Args.Arg4 = (UINTN)Arg2;
      Args.Arg5 = (UINTN)Arg3;
      Args.Arg6 = Session;
      ArmCallSmc (&Args);

      Function = SCM_INTERRUPTED;
      Session  = Args.Arg6;
    } while (Args.Arg0 == SCM_INTERRUPTED);

    if (((INT32)Args.Arg0 != SCM_V2_EBUSY) || (Retry >= SCM_BUSY_RETRIES)) {
      break;
    }

    MicroSecondDelay (SCM_BUSY_DELAY_US);
  }

  CopyMem (Result, &Args, sizeof (Args));

  //
  // Linux takes the status as an int.
  //
  Status = (INT32)Args.Arg0;
  switch (Status) {
    case 0:
      return EFI_SUCCESS;
    case SCM_EOPNOTSUPP:
      return EFI_UNSUPPORTED;
    case SCM_EINVAL_ADDR:
    case SCM_EINVAL_ARG:
      return EFI_INVALID_PARAMETER;
    case SCM_ENOMEM:
      return EFI_OUT_OF_RESOURCES;
    case SCM_WAITQ_SLEEP:
      return EFI_NOT_READY;
    default:
      return EFI_DEVICE_ERROR;
  }
}

/**
  Makes a PAS call, which succeeds when TrustZone ran it (x0 is 0) and the
  service says so (x1 is 0), as qcom_scm returns "ret ?: res.result[0]".

  @param[in]   Cmd       The PAS command.
  @param[in]   ArgInfo   The argument types.
  @param[in]   Arg0      First argument.
  @param[in]   Arg1      Second argument.
  @param[in]   Arg2      Third argument.
  @param[out]  TzStatus  x0 if not 0, else x1.

  @retval EFI_SUCCESS  Done.
  @retval Other        Refused.
**/
STATIC
EFI_STATUS
ScmPasCall (
  IN  UINT32  Cmd,
  IN  UINT32  ArgInfo,
  IN  UINT64  Arg0,
  IN  UINT64  Arg1,
  IN  UINT64  Arg2,
  OUT INT64   *TzStatus
  )
{
  ARM_SMC_ARGS  Result;
  EFI_STATUS    Status;

  Status = ScmCall (SCM_SVC_PIL, Cmd, ArgInfo, Arg0, Arg1, Arg2, 0, &Result);
  if (EFI_ERROR (Status)) {
    *TzStatus = (INT32)Result.Arg0;
    return Status;
  }

  *TzStatus = (INT64)Result.Arg1;
  return (Result.Arg1 == 0) ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}

/**
  Asks TrustZone whether it implements a call (qcom_scm
  __qcom_scm_is_call_available).

  @param[in]  Svc  The service.
  @param[in]  Cmd  The command.

  @return  TRUE if it does.
**/
STATIC
BOOLEAN
ScmIsCallAvailable (
  IN UINT32  Svc,
  IN UINT32  Cmd
  )
{
  ARM_SMC_ARGS  Result;
  EFI_STATUS    Status;

  Status = ScmCall (
             SCM_SVC_INFO,
             SCM_INFO_IS_CALL_AVAIL,
             SCM_ARGS (1),
             SCM_OWNER_SIP | SCM_FNID (Svc, Cmd),
             0,
             0,
             0,
             &Result
             );

  return !EFI_ERROR (Status) && (Result.Arg1 != 0);
}

BOOLEAN
ScmPasIsSupported (
  IN UINT32  PasId
  )
{
  ARM_SMC_ARGS  Result;
  EFI_STATUS    Status;

  if (!ScmIsCallAvailable (SCM_SVC_PIL, SCM_PIL_PAS_IS_SUPPORTED)) {
    return FALSE;
  }

  Status = ScmCall (SCM_SVC_PIL, SCM_PIL_PAS_IS_SUPPORTED, SCM_ARGS (1), PasId, 0, 0, 0, &Result);
  return !EFI_ERROR (Status) && (Result.Arg1 != 0);
}

EFI_STATUS
ScmPasInitImage (
  IN  UINT32                PasId,
  IN  EFI_PHYSICAL_ADDRESS  Metadata,
  OUT INT64                 *TzStatus
  )
{
  return ScmPasCall (
           SCM_PIL_PAS_INIT_IMAGE,
           SCM_ARGS (2) | SCM_ARG_TYPE (0, SCM_ARG_VAL) | SCM_ARG_TYPE (1, SCM_ARG_RW),
           PasId,
           Metadata,
           0,
           TzStatus
           );
}

EFI_STATUS
ScmPasMemSetup (
  IN  UINT32                PasId,
  IN  EFI_PHYSICAL_ADDRESS  Base,
  IN  UINT64                Size,
  OUT INT64                 *TzStatus
  )
{
  return ScmPasCall (SCM_PIL_PAS_MEM_SETUP, SCM_ARGS (3), PasId, Base, Size, TzStatus);
}

EFI_STATUS
ScmPasAuthAndReset (
  IN  UINT32  PasId,
  OUT INT64   *TzStatus
  )
{
  return ScmPasCall (SCM_PIL_PAS_AUTH_AND_RESET, SCM_ARGS (1), PasId, 0, 0, TzStatus);
}

EFI_STATUS
ScmPasShutdown (
  IN UINT32  PasId
  )
{
  INT64  TzStatus;

  return ScmPasCall (SCM_PIL_PAS_SHUTDOWN, SCM_ARGS (1), PasId, 0, 0, &TzStatus);
}

EFI_STATUS
ScmShmBridgeEnable (
  VOID
  )
{
  ARM_SMC_ARGS  Result;
  EFI_STATUS    Status;

  if (!ScmIsCallAvailable (SCM_SVC_MP, SCM_MP_SHM_BRIDGE_ENABLE)) {
    return EFI_UNSUPPORTED;
  }

  Status = ScmCall (SCM_SVC_MP, SCM_MP_SHM_BRIDGE_ENABLE, SCM_ARGS (0), 0, 0, 0, 0, &Result);
  DEBUG ((DEBUG_INFO, "%a: x0 0x%lx x1 0x%lx\n", __func__, (UINT64)Result.Arg0, (UINT64)Result.Arg1));
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (Result.Arg1 == SHM_BRIDGE_RESULT_NOTSUPP) {
    return EFI_UNSUPPORTED;
  }

  return (Result.Arg1 == 0) ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}

EFI_STATUS
ScmShmBridgeCreate (
  IN  EFI_PHYSICAL_ADDRESS  Base,
  IN  UINT64                Size,
  OUT UINT64                *Handle
  )
{
  ARM_SMC_ARGS  Result;
  EFI_STATUS    Status;

  Status = ScmCall (
             SCM_SVC_MP,
             SCM_MP_SHM_BRIDGE_CREATE,
             SCM_ARGS (4),
             Base | SHM_BRIDGE_PERM_RW,
             Base | SHM_BRIDGE_PERM_RW,
             Size | SHM_BRIDGE_SELF_OWNER | (SHM_BRIDGE_PERM_RW << SHM_BRIDGE_SELF_OWNER_PERM_SHIFT),
             0,
             &Result
             );
  if (EFI_ERROR (Status) || (Result.Arg1 != 0)) {
    DEBUG ((
      DEBUG_ERROR,
      "%a: 0x%lx size 0x%lx: x0 0x%lx x1 0x%lx\n",
      __func__,
      Base,
      Size,
      (UINT64)Result.Arg0,
      (UINT64)Result.Arg1
      ));
    return EFI_ERROR (Status) ? Status : EFI_DEVICE_ERROR;
  }

  *Handle = Result.Arg2;
  return EFI_SUCCESS;
}

VOID
ScmShmBridgeDelete (
  IN UINT64  Handle
  )
{
  ARM_SMC_ARGS  Result;
  EFI_STATUS    Status;

  Status = ScmCall (SCM_SVC_MP, SCM_MP_SHM_BRIDGE_DELETE, SCM_ARGS (1), Handle, 0, 0, 0, &Result);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: 0x%lx: %r (x0 0x%lx)\n", __func__, Handle, Status, (UINT64)Result.Arg0));
  }
}
