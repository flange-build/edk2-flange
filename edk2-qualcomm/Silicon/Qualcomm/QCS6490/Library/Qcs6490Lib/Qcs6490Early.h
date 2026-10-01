/** @file
  Early SEC code of the QCS6490 platform library: what Qcs6490EarlyInit()
  and its helpers share.

  All of it runs from ArmPlatformPeiBootAction(), at EL1 as a Gunyah guest,
  on a temporary stack, with the MMU and caches off and before any library
  constructor has run. See EarlyInit.c.

  Copyright (c) 2026, edk2-flange contributors.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#pragma once

#include <Base.h>
#include <Library/PcdLib.h>

#include <Qcs6490NvStore.h>

//
// SEC scratch memory, the page after the status page (Qcs6490NvStore.h):
// the transfer request list SEC starts when XBL left none running (1 KiB
// aligned), the UTP command descriptor (128-byte aligned) and a one-block
// buffer for the GPT header.
//
#define QCS6490_EARLY_UTRL_BASE          (QCS6490_NVSTORE_SCRATCH_BASE)
#define QCS6490_EARLY_UCD_BASE           (QCS6490_NVSTORE_SCRATCH_BASE + SIZE_4KB)
#define QCS6490_EARLY_BLOCK_BUFFER_BASE  (QCS6490_NVSTORE_SCRATCH_BASE + SIZE_8KB)

//
// Everything SEC may have UFS write into: the store, the status page and
// the scratch memory.
//
#define QCS6490_EARLY_NV_AREA_BASE  QCS6490_NVSTORE_BASE
#define QCS6490_EARLY_NV_AREA_SIZE  \
  (QCS6490_NVSTORE_SCRATCH_BASE + QCS6490_NVSTORE_SCRATCH_SIZE - QCS6490_NVSTORE_BASE)

//
// xbl_config's OsConfigTableSelection, as QCS6490_NVSTORE_STATUS.XblOsConfig
// records it (QCS6490_XBL_OS_CONFIG_UNKNOWN when there is none).
//
#define QCS6490_XBL_OS_CONFIG_GUNYAH  1
#define QCS6490_XBL_OS_CONFIG_KVM     2

//
// QCS6490_NVSTORE_STATUS.UfsDiagnostic: what SEC's UFS or GPT code failed
// on, 0 if nothing did. The step is in bits 31:24, the overall command
// status (OCS) of the transfer request in 23:16, the SCSI status in 15:8 and
// a detail that depends on the step in 7:0.
//
#define QCS6490_UFS_DIAG(Step, Ocs, ScsiStatus, Detail)  \
  (((UINT32)(Step) << 24) | (((UINT32)(Ocs) & 0xFF) << 16) | \
   (((UINT32)(ScsiStatus) & 0xFF) << 8) | ((UINT32)(Detail) & 0xFF))

#define QCS6490_UFS_DIAG_STEP(Diagnostic)  ((UINT32)(Diagnostic) >> 24)

//
// UFS host controller steps (LoadResult UFS_ERROR).
//
// No base address, or the controller is not running.
// Detail: HCE | HCS.DP << 1 | HCS.UTRLRDY << 2 | HCS.UCRDY << 3.
#define QCS6490_UFS_STEP_NOT_RUNNING  0x01
// A fatal error is flagged in IS.
// Detail: IS.ULLS | DFES << 1 | UTPES << 2 | HCFES << 3 | SBFES << 4.
#define QCS6490_UFS_STEP_FATAL        0x02
// Transfer requests still outstanding. Detail: UTRLDBR bits 7:0.
#define QCS6490_UFS_STEP_BUSY         0x03
// The transfer request list XBL left running is not in DRAM SEC may write.
#define QCS6490_UFS_STEP_LIST         0x04
// The list SEC set up did not start.
#define QCS6490_UFS_STEP_LIST_START   0x05
// A request did not complete in time. Detail: 1 if it could not be cleared
// either (the SMMU entry is then left in place).
#define QCS6490_UFS_STEP_TIMEOUT      0x06
// The request completed with an error OCS (in bits 23:16).
#define QCS6490_UFS_STEP_OCS          0x07
// The response UPIU is not one, or not for this request. Detail: its
// response field.
#define QCS6490_UFS_STEP_RESPONSE     0x08
// SCSI status other than GOOD (in bits 15:8). Detail: the sense key.
#define QCS6490_UFS_STEP_SCSI         0x09
// Fewer bytes transferred than asked for.
#define QCS6490_UFS_STEP_RESIDUAL     0x0A
// A read SEC refused to issue: the buffer is outside the store or not
// aligned, or the LUN does not exist.
#define QCS6490_UFS_STEP_REQUEST      0x0B

//
// GPT steps (LoadResult NO_PARTITION).
//
#define QCS6490_UFS_STEP_FIRST_GPT      0x10
// The primary GPT header is not valid. Detail: which check failed.
#define QCS6490_UFS_STEP_GPT_HEADER     0x10
// The partition entries do not match the header's CRC.
#define QCS6490_UFS_STEP_GPT_ENTRIES    0x11
// No partition has the name.
#define QCS6490_UFS_STEP_GPT_NOT_FOUND  0x12
// Several partitions have the name.
#define QCS6490_UFS_STEP_GPT_DUPLICATE  0x13
// The partition lies outside the usable blocks.
#define QCS6490_UFS_STEP_GPT_RANGE      0x14

//
// A bounded wait: the generic timer, and a poll count in case the counter
// does not run.
//
typedef struct {
  UINT64    Start;
  UINT64    Ticks;
  UINT64    PollsLeft;
} QCS6490_EARLY_TIMEOUT;

//
// What SEC changed in the apps SMMU for UFS, to put back.
//
typedef struct {
  BOOLEAN    Changed;
  BOOLEAN    ExtendedIds;    // the entry's valid bit is S2CR.EXIDVALID, not SMR.VALID
  UINT32     Index;
  UINT32     Smr;
  UINT32     S2cr;
} QCS6490_EARLY_SMMU;

//
// The UFS host controller as SEC uses it.
//
typedef struct {
  UINTN      Base;
  UINT32     SlotCount;
  UINT64     ListBase;       // the transfer request list in use
  BOOLEAN    XblList;        // it is the list XBL left running
  UINT32     Slot;           // the slot SEC uses in it
  BOOLEAN    TrdSaved;       // SavedTrd holds XBL's descriptor in Slot
  UINT32     SavedTrd[8];
  BOOLEAN    Ready;          // Qcs6490EarlyUfsInit() succeeded
  BOOLEAN    Pending;        // a request could be neither completed nor cleared
  UINT32     Diagnostic;     // QCS6490_UFS_DIAG () of the last failure
} QCS6490_EARLY_UFS;

//
// EarlyInit.c
//

/**
  Starts a bounded wait.

  @param[out] Timeout       The wait.
  @param[in]  Microseconds  How long it may take.
**/
VOID
Qcs6490EarlyTimeoutStart (
  OUT QCS6490_EARLY_TIMEOUT  *Timeout,
  IN  UINT32                 Microseconds
  );

/**
  Checks a bounded wait, once per poll.

  @param[in, out] Timeout  The wait.

  @retval TRUE   The time is up.
  @retval FALSE  Keep polling.
**/
BOOLEAN
Qcs6490EarlyTimeoutExpired (
  IN OUT QCS6490_EARLY_TIMEOUT  *Timeout
  );

/**
  Checks that a range is DRAM that belongs to the OS and that UEFI does not
  use yet: inside the DRAM every QCS6490 board has, and clear of the
  firmware carve-outs, the firmware volume and the SEC stack.

  @param[in]  Base  The start of the range.
  @param[in]  Size  Its size in bytes.

  @retval TRUE   SEC may write the range and have devices write it.
  @retval FALSE  It may not.
**/
BOOLEAN
Qcs6490EarlyIsFreeDram (
  IN UINT64  Base,
  IN UINT64  Size
  );

/**
  Checks that a range lies inside the store, status page and SEC scratch
  memory.

  @param[in]  Base  The start of the range.
  @param[in]  Size  Its size in bytes.

  @retval TRUE   It does.
  @retval FALSE  It does not, or is empty.
**/
BOOLEAN
Qcs6490EarlyIsInNvArea (
  IN UINT64  Base,
  IN UINT64  Size
  );

/**
  Checks the variable store layout the PCDs describe: contiguous regions of
  whole blocks, in DRAM nothing else uses.

  @retval TRUE   The store, status page and scratch memory may be used.
  @retval FALSE  They may not; nothing may be written there.
**/
BOOLEAN
Qcs6490NvStoreLayoutValid (
  VOID
  );

/**
  Fills in the status page as SEC finds things before it reads the store.

  The layout must be valid (Qcs6490NvStoreLayoutValid ()).

  @param[in]  Decision  The exception level UEFI runs at so far,
                        QCS6490_HYPERVISOR_MODE_EL1 or _EL2.

  @return  The status page.
**/
QCS6490_NVSTORE_STATUS *
Qcs6490NvStoreStatusInit (
  IN UINT8  Decision
  );

/**
  Returns the status page SEC filled in on this boot.

  @return  The status page, or NULL if the layout is not valid or the page
           was not filled in.
**/
QCS6490_NVSTORE_STATUS *
Qcs6490NvStoreStatusGet (
  VOID
  );

/**
  Early SEC initialization: loads the variable store from UFS and decides
  whether to leave Gunyah. Called by ArmPlatformPeiBootAction() at EL1.

  @retval TRUE   Ask TrustZone to remove Gunyah and continue at EL2.
  @retval FALSE  Stay a Gunyah guest at EL1.
**/
BOOLEAN
EFIAPI
Qcs6490EarlyInit (
  VOID
  );

/**
  Reports an exception taken while the early vectors are installed. The
  caller parks the CPU afterwards.

  @param[in]  Vector  The offset of the vector taken.
  @param[in]  Esr     ESR_EL1.
  @param[in]  Elr     ELR_EL1.
  @param[in]  Far     FAR_EL1.
  @param[in]  Spsr    SPSR_EL1.
**/
VOID
EFIAPI
Qcs6490EarlyException (
  IN UINTN  Vector,
  IN UINTN  Esr,
  IN UINTN  Elr,
  IN UINTN  Far,
  IN UINTN  Spsr
  );

//
// EarlySmmu.c
//

/**
  Lets the UFS stream through the apps SMMU without stage 1 translation.

  @param[out] Smmu  What was changed, for Qcs6490EarlySmmuRestore ().

  @retval RETURN_SUCCESS  UFS may do DMA.
  @retval Other           It may not; nothing was changed.
**/
RETURN_STATUS
Qcs6490EarlySmmuEnableUfs (
  OUT QCS6490_EARLY_SMMU  *Smmu
  );

/**
  Puts back the stream match entry Qcs6490EarlySmmuEnableUfs () used. UFS
  must not do DMA any more.

  @param[in, out] Smmu  What was changed.
**/
VOID
Qcs6490EarlySmmuRestore (
  IN OUT QCS6490_EARLY_SMMU  *Smmu
  );

//
// EarlyUfs.c
//

/**
  Checks the UFS host controller XBL left running and picks the transfer
  request list and slot to use. Does not start any transfer.

  @param[out] Ufs  The controller.

  @retval RETURN_SUCCESS  Reads may be issued once the SMMU lets UFS through.
  @retval Other           The controller cannot be used; Ufs->Diagnostic
                          says why.
**/
RETURN_STATUS
Qcs6490EarlyUfsInit (
  OUT QCS6490_EARLY_UFS  *Ufs
  );

/**
  Reads blocks of 4 KiB from a LUN into the store memory.

  @param[in, out] Ufs     The controller.
  @param[in]      Lun     The LUN.
  @param[in]      Lba     The first block.
  @param[in]      Blocks  The number of blocks.
  @param[in]      Buffer  Where to put them, inside the store memory.

  @retval RETURN_SUCCESS  The blocks were read.
  @retval Other           They were not; Ufs->Diagnostic says why.
**/
RETURN_STATUS
Qcs6490EarlyUfsRead (
  IN OUT QCS6490_EARLY_UFS  *Ufs,
  IN     UINT8              Lun,
  IN     UINT64             Lba,
  IN     UINT32             Blocks,
  IN     UINT64             Buffer
  );

/**
  Leaves the controller as XBL left it, as far as SEC can.

  @param[in, out] Ufs  The controller.

  @retval TRUE   No transfer is in progress: the SMMU entry may go.
  @retval FALSE  A transfer may still be in progress.
**/
BOOLEAN
Qcs6490EarlyUfsFinish (
  IN OUT QCS6490_EARLY_UFS  *Ufs
  );

//
// EarlyNvStore.c
//

/**
  Finds a partition by name in the GPT of a LUN. Uses the store memory as a
  buffer for the partition entries.

  @param[in, out] Ufs         The controller.
  @param[in]      Lun         The LUN.
  @param[in]      Name        The partition name.
  @param[out]     StartLba    The first block of the partition.
  @param[out]     BlockCount  Its size in blocks.

  @retval RETURN_SUCCESS  The partition was found.
  @retval Other           It was not; Ufs->Diagnostic says why.
**/
RETURN_STATUS
Qcs6490EarlyFindPartition (
  IN OUT QCS6490_EARLY_UFS  *Ufs,
  IN     UINT8              Lun,
  IN     CONST CHAR16       *Name,
  OUT    UINT64             *StartLba,
  OUT    UINT64             *BlockCount
  );

/**
  Checks that the store memory holds a variable firmware volume the
  variable driver can use: its header and the variable store header.

  @retval TRUE   It does.
  @retval FALSE  It does not.
**/
BOOLEAN
Qcs6490EarlyCheckStore (
  VOID
  );

/**
  Looks up the HypervisorMode variable in the store memory, which must
  have passed Qcs6490EarlyCheckStore ().

  @return  Its value, QCS6490_HYPERVISOR_MODE_AUTO, _EL1 or _EL2, or
           QCS6490_HYPERVISOR_SETTING_NONE if it is missing or invalid.
**/
UINT8
Qcs6490EarlyFindHypervisorMode (
  VOID
  );

//
// XblConfig.c
//

/**
  Reads OsConfigTableSelection from xbl_config's device tree.

  @return  QCS6490_XBL_OS_CONFIG_GUNYAH or _KVM, or
           QCS6490_XBL_OS_CONFIG_UNKNOWN.
**/
UINT8
Qcs6490EarlyReadXblConfig (
  VOID
  );
