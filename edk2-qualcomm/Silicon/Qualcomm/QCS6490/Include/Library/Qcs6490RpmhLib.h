/** @file
  Polled RPMh requests through DRV2 of the QCS6490 apps RSC, the DRV that
  UEFI and the OS share: regulator, power rail (ARC) and bus clock manager
  (BCM) votes.

  Copyright (c) 2026, edk2-flange contributors.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#ifndef QCS6490_RPMH_LIB_H_
#define QCS6490_RPMH_LIB_H_

#include <Uefi/UefiBaseType.h>

//
// Bus clock manager vote (include/soc/qcom/tcs.h BCM_TCS_CMD): commit in
// bit 30, valid in bit 29, average (x) in bits 27:14, peak (y) in bits 13:0.
//
#define RPMH_BCM_COMMIT        BIT30
#define RPMH_BCM_VALID         BIT29
#define RPMH_BCM_VOTE_MASK     0x3FFF
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
EFIAPI
RpmhWrite (
  IN CONST RPMH_CMD  *Cmds,
  IN UINTN           Count
  );

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
  );

/**
  Logs DRV2 of the apps RSC: version, TCS configuration, interrupt state and,
  for each TCS, its control words and the commands it holds. Reads only.

  @param[in]  When  A label for the log lines.
**/
VOID
EFIAPI
RpmhLogState (
  IN CONST CHAR8  *When
  );

#endif // QCS6490_RPMH_LIB_H_
